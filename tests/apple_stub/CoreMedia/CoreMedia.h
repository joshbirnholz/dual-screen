// See tests/apple_stub/README.md. Declared as CoreMedia declares them.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

extern "C" {

typedef long CMItemCount;
typedef struct {
    int64_t value;
    int32_t timescale;
    uint32_t flags;
    int64_t epoch;
} CMTime;
typedef struct {
    CMTime duration;
    CMTime presentationTimeStamp;
    CMTime decodeTimeStamp;
} CMSampleTimingInfo;

typedef const struct opaqueCMFormatDescription* CMFormatDescriptionRef;
typedef CMFormatDescriptionRef CMVideoFormatDescriptionRef;
typedef struct OpaqueCMBlockBuffer* CMBlockBufferRef;
typedef struct opaqueCMSampleBuffer* CMSampleBufferRef;
typedef uint32_t CMBlockBufferFlags;
typedef struct {
    uint32_t version;
    void* AllocateBlock;
    void* FreeBlock;
    void* refCon;
} CMBlockBufferCustomBlockSource;

OSStatus CMVideoFormatDescriptionCreateFromH264ParameterSets(
    CFAllocatorRef allocator, size_t parameterSetCount, const uint8_t* const* parameterSetPointers,
    const size_t* parameterSetSizes, int NALUnitHeaderLength,
    CMFormatDescriptionRef* formatDescriptionOut);
OSStatus CMBlockBufferCreateWithMemoryBlock(CFAllocatorRef structureAllocator, void* memoryBlock,
                                            size_t blockLength, CFAllocatorRef blockAllocator,
                                            const CMBlockBufferCustomBlockSource* customBlockSource,
                                            size_t offsetToData, size_t dataLength,
                                            CMBlockBufferFlags flags,
                                            CMBlockBufferRef* blockBufferOut);
OSStatus CMSampleBufferCreateReady(CFAllocatorRef allocator, CMBlockBufferRef dataBuffer,
                                   CMFormatDescriptionRef formatDescription,
                                   CMItemCount numSamples, CMItemCount numSampleTimingEntries,
                                   const CMSampleTimingInfo* sampleTimingArray,
                                   CMItemCount numSampleSizeEntries, const size_t* sampleSizeArray,
                                   CMSampleBufferRef* sampleBufferOut);

}  // extern "C"
