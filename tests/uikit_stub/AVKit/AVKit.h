// See CoreFoundation/CoreFoundation.h. The slice of AVKit that
// apple/system_video_layer.m's picture in picture names: an iPhone's and an
// iPad's, and marked unavailable on tvOS's SDK for the sample-buffer half, so
// left out there the same way.
#pragma once

#include <TargetConditionals.h>

#include <AVFoundation/AVFoundation.h>

#if !TARGET_OS_TV

@class AVPictureInPictureController;

@protocol AVPictureInPictureControllerDelegate
@optional
- (void)pictureInPictureControllerWillStartPictureInPicture:
    (AVPictureInPictureController *)pictureInPictureController;
- (void)pictureInPictureControllerDidStopPictureInPicture:
    (AVPictureInPictureController *)pictureInPictureController;
- (void)pictureInPictureController:(AVPictureInPictureController *)pictureInPictureController
    failedToStartPictureInPictureWithError:(NSError *)error;
- (void)pictureInPictureController:(AVPictureInPictureController *)pictureInPictureController
    restoreUserInterfaceForPictureInPictureStopWithCompletionHandler:
        (void (^)(BOOL restored))completionHandler;
@end

@protocol AVPictureInPictureSampleBufferPlaybackDelegate
@required
- (void)pictureInPictureController:(AVPictureInPictureController *)pictureInPictureController
                        setPlaying:(BOOL)playing;
- (CMTimeRange)pictureInPictureControllerTimeRangeForPlayback:
    (AVPictureInPictureController *)pictureInPictureController;
- (BOOL)pictureInPictureControllerIsPlaybackPaused:
    (AVPictureInPictureController *)pictureInPictureController;
- (void)pictureInPictureController:(AVPictureInPictureController *)pictureInPictureController
         didTransitionToRenderSize:(CMVideoDimensions)newRenderSize;
- (void)pictureInPictureController:(AVPictureInPictureController *)pictureInPictureController
                    skipByInterval:(CMTime)skipInterval
                 completionHandler:(void (^)(void))completionHandler;
@end

@interface AVPictureInPictureControllerContentSource : NSObject
- (instancetype)initWithSampleBufferDisplayLayer:(AVSampleBufferDisplayLayer *)sampleBufferDisplayLayer
                                playbackDelegate:
                                    (id<AVPictureInPictureSampleBufferPlaybackDelegate>)playbackDelegate;
@end

@interface AVPictureInPictureController : NSObject
+ (BOOL)isPictureInPictureSupported;
- (instancetype)initWithContentSource:(AVPictureInPictureControllerContentSource *)contentSource;
@property (nonatomic, assign) id<AVPictureInPictureControllerDelegate> delegate;
@property (nonatomic) BOOL requiresLinearPlayback;
@property (nonatomic) BOOL canStartPictureInPictureAutomaticallyFromInline;
- (void)stopPictureInPicture;
@end

#endif
