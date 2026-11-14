// The iOS and tvOS half of SystemVideoLayers (src/render/system_video_layer.hpp):
// an AVSampleBufferDisplayLayer per streamed screen, laid over SDL's GL view
// exactly where the compositor would have drawn the screen.
//
// What goes in is the decoder's own CVPixelBuffer, already decoded -- not the
// compressed stream. The layer could decode H.264 itself, but then the picture
// could only ever be shown by the layer: with the quick menu open the app has
// to draw the game with GL, to blur it behind the menu, and a stream the layer
// was decoding is a stream the app's decoder never saw. Handing it decoded
// pictures keeps one decoder, and lets any frame go either way.
//
// Compiled without ARC, like ios_scene_lifecycle.m beside it: SDL_SysWMinfo
// holds its UIWindow in a union, which ARC refuses to compile. Everything this
// file keeps, it retains and releases itself.
//
// Main thread only, which is where SDL runs the app on these platforms.

#include <TargetConditionals.h>

#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

// Picture in picture is an iPhone's and an iPad's; tvOS has its own idea of it
// and nothing here asks for that.
#if TARGET_OS_IPHONE && !TARGET_OS_TV
#define DS_HAVE_PICTURE_IN_PICTURE 1
#import <AVKit/AVKit.h>
#else
#define DS_HAVE_PICTURE_IN_PICTURE 0
#endif

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_syswm.h>

typedef struct DsVideoLayer {
    AVSampleBufferDisplayLayer *layer;
    // The view it is laid over, which outlives it: SDL's, for the whole run.
    UIView *view;
    // Made from the first picture and kept while pictures keep matching it:
    // one per size, not one per frame.
    CMVideoFormatDescriptionRef format;
} DsVideoLayer;

static void write_reason(char *reason, int reason_size, NSString *text)
{
    if (reason == NULL || reason_size <= 0) {
        return;
    }
    const char *utf8 = text != nil ? [text UTF8String] : "no reason given";
    if (utf8 == NULL) {
        utf8 = "no reason given";
    }
    strncpy(reason, utf8, (size_t)reason_size - 1);
    reason[reason_size - 1] = '\0';
}

void *ds_video_view_for_window(SDL_Window *window)
{
    if (window == NULL) {
        return NULL;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_UIKIT) {
        return NULL;
    }
    UIWindow *uiwindow = info.info.uikit.window;
    // SDL puts its GL view in as the root view controller's view: the layer
    // goes over that, in the same coordinates the view draws in.
    UIView *view = uiwindow.rootViewController.view;
    if (view == nil) {
        view = uiwindow;
    }
    return (void *)view;
}

void *ds_video_layer_create(void *view_handle)
{
    UIView *view = (UIView *)view_handle;
    if (view == nil) {
        return NULL;
    }
    DsVideoLayer *self = (DsVideoLayer *)calloc(1, sizeof(DsVideoLayer));
    if (self == NULL) {
        return NULL;
    }
    self->layer = [[AVSampleBufferDisplayLayer alloc] init];
    if (self->layer == nil) {
        free(self);
        return NULL;
    }
    // The compositor has already worked out the rectangle, aspect and all:
    // the picture fills it exactly, as the GL quad it replaces would.
    self->layer.videoGravity = AVLayerVideoGravityResize;
    self->layer.opaque = YES;
    self->layer.hidden = YES;
    self->layer.backgroundColor = [UIColor blackColor].CGColor;
    self->view = view;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [view.layer addSublayer:self->layer];
    [CATransaction commit];
    return self;
}

void ds_video_layer_destroy(void *handle)
{
    DsVideoLayer *self = (DsVideoLayer *)handle;
    if (self == NULL) {
        return;
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [self->layer flushAndRemoveImage];
    [self->layer removeFromSuperlayer];
    [CATransaction commit];
    [self->layer release];
    if (self->format != NULL) {
        CFRelease(self->format);
    }
    free(self);
}

void ds_video_layer_hide(void *handle)
{
    DsVideoLayer *self = (DsVideoLayer *)handle;
    if (self == NULL) {
        return;
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->layer.hidden = YES;
    [CATransaction commit];
}

void ds_video_layer_clear(void *handle)
{
    DsVideoLayer *self = (DsVideoLayer *)handle;
    if (self == NULL) {
        return;
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->layer.hidden = YES;
    [self->layer flushAndRemoveImage];
    [CATransaction commit];
}

// Says what the picture is, which the GL path says in its shader instead: the
// app sends BT.601 full range (see color_convert.cpp and ds_yuv_to_rgb in
// quad_renderer.cpp), and with nothing attached the system assumes BT.709 and
// shows every colour a little off. Full range itself is in the pixel format,
// which is '420f'. The primaries and curve are sRGB's, which is what the GL
// path's output is taken to be -- so the two paths show the same colours.
static void describe_colour(CVPixelBufferRef picture)
{
    CVBufferSetAttachment(picture, kCVImageBufferYCbCrMatrixKey,
                          kCVImageBufferYCbCrMatrix_ITU_R_601_4, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(picture, kCVImageBufferColorPrimariesKey,
                          kCVImageBufferColorPrimaries_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(picture, kCVImageBufferTransferFunctionKey,
                          kCVImageBufferTransferFunction_sRGB, kCVAttachmentMode_ShouldPropagate);
}

static int enqueue_into(AVSampleBufferDisplayLayer *layer, CMVideoFormatDescriptionRef *format,
                        CVPixelBufferRef picture, char *reason, int reason_size)
{
    describe_colour(picture);
    if (*format == NULL || !CMVideoFormatDescriptionMatchesImageBuffer(*format, picture)) {
        if (*format != NULL) {
            CFRelease(*format);
            *format = NULL;
        }
        OSStatus status =
            CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, picture, format);
        if (status != noErr || *format == NULL) {
            *format = NULL;
            write_reason(reason, reason_size,
                         [NSString stringWithFormat:@"no format for the picture: %d", (int)status]);
            return -1;
        }
    }
    // Stamped now and shown at once: the app has already decided this is the
    // moment for this picture (see CompanionSession::poll), and the layer is
    // not to hold it back for a timeline of its own.
    CMSampleTimingInfo timing;
    timing.duration = kCMTimeInvalid;
    timing.presentationTimeStamp = CMClockGetTime(CMClockGetHostTimeClock());
    timing.decodeTimeStamp = kCMTimeInvalid;
    CMSampleBufferRef sample = NULL;
    OSStatus status = CMSampleBufferCreateReadyWithImageBuffer(kCFAllocatorDefault, picture,
                                                               *format, &timing, &sample);
    if (status != noErr || sample == NULL) {
        write_reason(reason, reason_size,
                     [NSString stringWithFormat:@"no sample for the picture: %d", (int)status]);
        return -1;
    }
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, true);
    if (attachments != NULL && CFArrayGetCount(attachments) > 0) {
        CFMutableDictionaryRef first =
            (CFMutableDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFDictionarySetValue(first, kCMSampleAttachmentKey_DisplayImmediately, kCFBooleanTrue);
    }
    [layer enqueueSampleBuffer:sample];
    CFRelease(sample);
    return 0;
}

static int enqueue(DsVideoLayer *self, CVPixelBufferRef picture, char *reason, int reason_size)
{
    return enqueue_into(self->layer, &self->format, picture, reason, reason_size);
}

int ds_video_layer_show(void *handle, void *picture_handle, int is_new, float x, float y,
                        float width, float height, int quarter_turns, int smooth,
                        float output_width, float output_height, char *reason, int reason_size)
{
    DsVideoLayer *self = (DsVideoLayer *)handle;
    CVPixelBufferRef picture = (CVPixelBufferRef)picture_handle;
    if (self == NULL || picture == NULL || output_width <= 0.0f || output_height <= 0.0f) {
        return 1;
    }
    // A layer the system stopped -- the app went to the background, or the
    // media services were reset -- shows nothing until flushed. Said once as a
    // failure, so the caller draws with GL for that frame and counts it.
    if (self->layer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        NSError *error = self->layer.error;
        write_reason(reason, reason_size,
                     error != nil ? [error localizedDescription] : @"the layer failed");
        [self->layer flush];
        return -1;
    }
    if (self->layer.requiresFlushToResumeDecoding) {
        [self->layer flush];
        is_new = 1;
    }

    // The compositor's rectangle is in the GL drawable's pixels, with the
    // origin top left; the layer's are in the view's points, the same way up.
    const CGRect bounds = self->view.bounds;
    const CGFloat scale_x = bounds.size.width / output_width;
    const CGFloat scale_y = bounds.size.height / output_height;
    const CGFloat centre_x = (x + width / 2.0f) * scale_x;
    const CGFloat centre_y = (y + height / 2.0f) * scale_y;
    // A screen turned a quarter is placed in a rectangle the other way round
    // from its picture: the layer is the picture's shape, turned into place.
    CGFloat layer_width = width * scale_x;
    CGFloat layer_height = height * scale_y;
    CGFloat angle = 0.0;
    if (quarter_turns != 0) {
        layer_width = height * scale_y;
        layer_height = width * scale_x;
        angle = quarter_turns > 0 ? M_PI_2 : -M_PI_2;
    }

    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->layer.affineTransform = CGAffineTransformIdentity;
    self->layer.bounds = CGRectMake(0.0, 0.0, layer_width, layer_height);
    self->layer.position = CGPointMake(centre_x, centre_y);
    self->layer.affineTransform = CGAffineTransformMakeRotation(angle);
    NSString *filter = smooth ? kCAFilterLinear : kCAFilterNearest;
    self->layer.magnificationFilter = filter;
    self->layer.minificationFilter = kCAFilterLinear;
    int result = 0;
    if (is_new) {
        result = enqueue(self, picture, reason, reason_size);
    }
    if (result == 0) {
        self->layer.hidden = NO;
    }
    [CATransaction commit];
    return result;
}

// --- picture in picture -----------------------------------------------------
//
// The streamed game going on in the system's floating window when the app is
// left (Settings > General > Picture in Picture). The system starts it by
// itself as the app goes to the background, provided it was told it may
// (canStartPictureInPictureAutomaticallyFromInline) and the layer it was given
// is showing video at that moment.
//
// That layer is one of its own, not one of the DsVideoLayers above. Those are
// hidden whenever anything is drawn over the game -- the quick menu, the touch
// controls -- and a hidden layer is one the system will not take into a
// window. This one is fed the same CVPixelBuffer every frame (a reference, not
// a copy) and sits in the window *behind* SDL's opaque GL view, where it is
// on screen as far as the system is concerned and out of sight as far as the
// player is.
//
// While the app is in the background nothing is drawn with GL -- iOS ends an
// app that touches GL there -- but the session goes on being polled and each
// new picture still comes here, which is what keeps the floating window live.
// That needs "audio" in UIBackgroundModes (Info-iOS.plist.in), the mode that
// picture in picture belongs to.

#if DS_HAVE_PICTURE_IN_PICTURE

@interface DsPictureInPicture
    : NSObject <AVPictureInPictureControllerDelegate, AVPictureInPictureSampleBufferPlaybackDelegate> {
@public
    AVSampleBufferDisplayLayer *layer;
    AVPictureInPictureController *controller;
    UIView *view;
    CMVideoFormatDescriptionRef format;
    // From the system's first word that it is starting to its word that it
    // has stopped.
    BOOL active;
    // Why it would not start, once, for the log.
    NSString *failure;
}
@end

@implementation DsPictureInPicture

// --- AVPictureInPictureControllerDelegate

- (void)pictureInPictureControllerWillStartPictureInPicture:(AVPictureInPictureController *)c
{
    (void)c;
    active = YES;
}

- (void)pictureInPictureControllerDidStopPictureInPicture:(AVPictureInPictureController *)c
{
    (void)c;
    active = NO;
}

- (void)pictureInPictureController:(AVPictureInPictureController *)c
    failedToStartPictureInPictureWithError:(NSError *)error
{
    (void)c;
    active = NO;
    [failure release];
    failure = [(error != nil ? [error localizedDescription] : @"no reason given") retain];
}

// The floating window's button back into the app. There is nothing to put
// back: the app's own screen never stopped being what it was.
- (void)pictureInPictureController:(AVPictureInPictureController *)c
    restoreUserInterfaceForPictureInPictureStopWithCompletionHandler:
        (void (^)(BOOL restored))completionHandler
{
    (void)c;
    completionHandler(YES);
}

// --- AVPictureInPictureSampleBufferPlaybackDelegate
//
// A live picture from another machine: nothing to pause, nothing to skip
// through. An endless time range is how the system is told it is live, and
// it shows no scrubber and no skip buttons for it.

- (void)pictureInPictureController:(AVPictureInPictureController *)c setPlaying:(BOOL)playing
{
    (void)c;
    (void)playing;
}

- (CMTimeRange)pictureInPictureControllerTimeRangeForPlayback:(AVPictureInPictureController *)c
{
    (void)c;
    return CMTimeRangeMake(kCMTimeNegativeInfinity, kCMTimePositiveInfinity);
}

- (BOOL)pictureInPictureControllerIsPlaybackPaused:(AVPictureInPictureController *)c
{
    (void)c;
    return NO;
}

- (void)pictureInPictureController:(AVPictureInPictureController *)c
         didTransitionToRenderSize:(CMVideoDimensions)newRenderSize
{
    (void)c;
    (void)newRenderSize;
}

- (void)pictureInPictureController:(AVPictureInPictureController *)c
                    skipByInterval:(CMTime)skipInterval
                 completionHandler:(void (^)(void))completionHandler
{
    (void)c;
    (void)skipInterval;
    completionHandler();
}

@end

#endif  // DS_HAVE_PICTURE_IN_PICTURE

void *ds_pip_create(void *view_handle)
{
#if DS_HAVE_PICTURE_IN_PICTURE
    UIView *view = (UIView *)view_handle;
    if (view == nil || view.superview == nil ||
        ![AVPictureInPictureController isPictureInPictureSupported]) {
        return NULL;
    }
    DsPictureInPicture *self = [[DsPictureInPicture alloc] init];
    self->view = view;
    self->layer = [[AVSampleBufferDisplayLayer alloc] init];
    // The floating window is the picture's own shape, whatever the layer's is.
    self->layer.videoGravity = AVLayerVideoGravityResizeAspect;
    self->layer.backgroundColor = [UIColor blackColor].CGColor;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    self->layer.frame = view.frame;
    [view.superview.layer insertSublayer:self->layer below:view.layer];
    [CATransaction commit];

    AVPictureInPictureControllerContentSource *source =
        [[AVPictureInPictureControllerContentSource alloc]
            initWithSampleBufferDisplayLayer:self->layer
                            playbackDelegate:self];
    self->controller = [[AVPictureInPictureController alloc] initWithContentSource:source];
    [source release];
    if (self->controller == nil) {
        [self->layer removeFromSuperlayer];
        [self->layer release];
        [self release];
        return NULL;
    }
    self->controller.delegate = self;
    self->controller.requiresLinearPlayback = YES;
    self->controller.canStartPictureInPictureAutomaticallyFromInline = NO;
    return (void *)self;
#else
    (void)view_handle;
    return NULL;
#endif
}

void ds_pip_destroy(void *handle)
{
#if DS_HAVE_PICTURE_IN_PICTURE
    DsPictureInPicture *self = (DsPictureInPicture *)handle;
    if (self == nil) {
        return;
    }
    self->controller.delegate = nil;
    [self->controller stopPictureInPicture];
    [self->controller release];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [self->layer flushAndRemoveImage];
    [self->layer removeFromSuperlayer];
    [CATransaction commit];
    [self->layer release];
    if (self->format != NULL) {
        CFRelease(self->format);
    }
    [self->failure release];
    [self release];
#else
    (void)handle;
#endif
}

// Whether the system may take the picture into a window when the app is left:
// only while there is a picture to take, and Picture in Picture is on.
void ds_pip_set_allowed(void *handle, int allowed)
{
#if DS_HAVE_PICTURE_IN_PICTURE
    DsPictureInPicture *self = (DsPictureInPicture *)handle;
    if (self == nil) {
        return;
    }
    const BOOL value = allowed ? YES : NO;
    if (self->controller.canStartPictureInPictureAutomaticallyFromInline != value) {
        self->controller.canStartPictureInPictureAutomaticallyFromInline = value;
    }
#else
    (void)handle;
    (void)allowed;
#endif
}

// 0 given; -1 not, with the reason written to `reason`.
int ds_pip_feed(void *handle, void *picture_handle, char *reason, int reason_size)
{
#if DS_HAVE_PICTURE_IN_PICTURE
    DsPictureInPicture *self = (DsPictureInPicture *)handle;
    CVPixelBufferRef picture = (CVPixelBufferRef)picture_handle;
    if (self == nil || picture == NULL) {
        return 0;
    }
    if (self->layer.status == AVQueuedSampleBufferRenderingStatusFailed) {
        NSError *error = self->layer.error;
        write_reason(reason, reason_size,
                     error != nil ? [error localizedDescription] : @"the layer failed");
        [self->layer flush];
        return -1;
    }
    if (self->layer.requiresFlushToResumeDecoding) {
        [self->layer flush];
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    // Kept the size of the view it hides behind, which a rotation or a fold
    // changes.
    if (!CGRectEqualToRect(self->layer.frame, self->view.frame)) {
        self->layer.frame = self->view.frame;
    }
    const int result = enqueue_into(self->layer, &self->format, picture, reason, reason_size);
    [CATransaction commit];
    return result;
#else
    (void)handle;
    (void)picture_handle;
    (void)reason;
    (void)reason_size;
    return 0;
#endif
}

// The session ended: the last picture let go of, and the window closed if it
// is up -- it would only ever show that picture again.
void ds_pip_clear(void *handle)
{
#if DS_HAVE_PICTURE_IN_PICTURE
    DsPictureInPicture *self = (DsPictureInPicture *)handle;
    if (self == nil) {
        return;
    }
    self->controller.canStartPictureInPictureAutomaticallyFromInline = NO;
    if (self->active) {
        [self->controller stopPictureInPicture];
    }
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [self->layer flushAndRemoveImage];
    [CATransaction commit];
#else
    (void)handle;
#endif
}

// 1 while the floating window is up or on its way; 0 otherwise. A reason it
// would not start is written to `failure` once, and empty otherwise.
int ds_pip_poll(void *handle, char *failure, int failure_size)
{
    if (failure != NULL && failure_size > 0) {
        failure[0] = '\0';
    }
#if DS_HAVE_PICTURE_IN_PICTURE
    DsPictureInPicture *self = (DsPictureInPicture *)handle;
    if (self == nil) {
        return 0;
    }
    if (self->failure != nil) {
        write_reason(failure, failure_size, self->failure);
        [self->failure release];
        self->failure = nil;
    }
    return self->active ? 1 : 0;
#else
    (void)handle;
    return 0;
#endif
}
