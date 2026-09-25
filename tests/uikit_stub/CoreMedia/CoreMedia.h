// See CoreFoundation/CoreFoundation.h.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

typedef int64_t CMTimeValue;
typedef int32_t CMTimeScale;
typedef uint32_t CMTimeFlags;
typedef int64_t CMTimeEpoch;
typedef struct {
    CMTimeValue value;
    CMTimeScale timescale;
    CMTimeFlags flags;
    CMTimeEpoch epoch;
} CMTime;
extern const CMTime kCMTimeInvalid;

typedef struct {
    CMTime duration;
    CMTime presentationTimeStamp;
    CMTime decodeTimeStamp;
} CMSampleTimingInfo;

typedef struct OpaqueCMClock *CMClockRef;
CMClockRef CMClockGetHostTimeClock(void);
CMTime CMClockGetTime(CMClockRef clock);

typedef const struct opaqueCMFormatDescription *CMFormatDescriptionRef;
typedef CMFormatDescriptionRef CMVideoFormatDescriptionRef;
typedef struct opaqueCMSampleBuffer *CMSampleBufferRef;

OSStatus CMVideoFormatDescriptionCreateForImageBuffer(
    CFAllocatorRef allocator, CVImageBufferRef imageBuffer,
    CMVideoFormatDescriptionRef *formatDescriptionOut);
Boolean CMVideoFormatDescriptionMatchesImageBuffer(CMVideoFormatDescriptionRef desc,
                                                   CVImageBufferRef imageBuffer);
OSStatus CMSampleBufferCreateReadyWithImageBuffer(CFAllocatorRef allocator,
                                                  CVImageBufferRef imageBuffer,
                                                  CMVideoFormatDescriptionRef formatDescription,
                                                  const CMSampleTimingInfo *sampleTiming,
                                                  CMSampleBufferRef *sampleBufferOut);
CFArrayRef CMSampleBufferGetSampleAttachmentsArray(CMSampleBufferRef sbuf,
                                                   Boolean createIfNecessary);
extern const CFStringRef kCMSampleAttachmentKey_DisplayImmediately;
