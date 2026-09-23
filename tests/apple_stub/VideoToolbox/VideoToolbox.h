// See tests/apple_stub/README.md. Declared as VideoToolbox declares them.
#pragma once

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>

extern "C" {

typedef struct OpaqueVTDecompressionSession* VTDecompressionSessionRef;
typedef CFTypeRef VTSessionRef;
typedef uint32_t VTDecodeFrameFlags;
typedef uint32_t VTDecodeInfoFlags;

typedef void (*VTDecompressionOutputCallback)(void* decompressionOutputRefCon,
                                              void* sourceFrameRefCon, OSStatus status,
                                              VTDecodeInfoFlags infoFlags,
                                              CVImageBufferRef imageBuffer,
                                              CMTime presentationTimeStamp,
                                              CMTime presentationDuration);
struct VTDecompressionOutputCallbackRecord {
    VTDecompressionOutputCallback decompressionOutputCallback;
    void* decompressionOutputRefCon;
};
typedef struct VTDecompressionOutputCallbackRecord VTDecompressionOutputCallbackRecord;

extern const CFStringRef kVTDecompressionPropertyKey_RealTime;
extern const CFStringRef kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder;
extern const CFStringRef kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder;

OSStatus VTDecompressionSessionCreate(CFAllocatorRef allocator,
                                      CMVideoFormatDescriptionRef videoFormatDescription,
                                      CFDictionaryRef videoDecoderSpecification,
                                      CFDictionaryRef destinationImageBufferAttributes,
                                      const VTDecompressionOutputCallbackRecord* outputCallback,
                                      VTDecompressionSessionRef* decompressionSessionOut);
OSStatus VTDecompressionSessionDecodeFrame(VTDecompressionSessionRef session,
                                           CMSampleBufferRef sampleBuffer,
                                           VTDecodeFrameFlags decodeFlags,
                                           void* sourceFrameRefCon,
                                           VTDecodeInfoFlags* infoFlagsOut);
OSStatus VTDecompressionSessionWaitForAsynchronousFrames(VTDecompressionSessionRef session);
void VTDecompressionSessionInvalidate(VTDecompressionSessionRef session);
OSStatus VTSessionSetProperty(VTSessionRef session, CFStringRef propertyKey,
                              CFTypeRef propertyValue);
OSStatus VTSessionCopyProperty(VTSessionRef session, CFStringRef propertyKey,
                               CFAllocatorRef allocator, void* propertyValueOut);

}  // extern "C"
