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

#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>

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

static int enqueue(DsVideoLayer *self, CVPixelBufferRef picture, char *reason, int reason_size)
{
    describe_colour(picture);
    if (self->format == NULL ||
        !CMVideoFormatDescriptionMatchesImageBuffer(self->format, picture)) {
        if (self->format != NULL) {
            CFRelease(self->format);
            self->format = NULL;
        }
        OSStatus status =
            CMVideoFormatDescriptionCreateForImageBuffer(kCFAllocatorDefault, picture, &self->format);
        if (status != noErr || self->format == NULL) {
            self->format = NULL;
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
                                                               self->format, &timing, &sample);
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
    [self->layer enqueueSampleBuffer:sample];
    CFRelease(sample);
    return 0;
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
