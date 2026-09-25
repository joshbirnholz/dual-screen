// See CoreFoundation/CoreFoundation.h.
#pragma once

#include <CoreMedia/CoreMedia.h>
#include <QuartzCore/QuartzCore.h>

typedef NSString *AVLayerVideoGravity;
extern AVLayerVideoGravity const AVLayerVideoGravityResize;

typedef long AVQueuedSampleBufferRenderingStatus;
enum {
    AVQueuedSampleBufferRenderingStatusUnknown = 0,
    AVQueuedSampleBufferRenderingStatusRendering = 1,
    AVQueuedSampleBufferRenderingStatusFailed = 2,
};

@interface AVSampleBufferDisplayLayer : CALayer
@property (copy) AVLayerVideoGravity videoGravity;
@property (readonly) AVQueuedSampleBufferRenderingStatus status;
@property (readonly) NSError *error;
@property (readonly) BOOL requiresFlushToResumeDecoding;
- (void)enqueueSampleBuffer:(CMSampleBufferRef)sampleBuffer;
- (void)flush;
- (void)flushAndRemoveImage;
@end
