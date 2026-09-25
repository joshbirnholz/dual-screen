// See CoreFoundation/CoreFoundation.h.
#pragma once

#include <CoreFoundation/CoreFoundation.h>

typedef struct __CVBuffer *CVBufferRef;
typedef CVBufferRef CVImageBufferRef;
typedef CVImageBufferRef CVPixelBufferRef;

typedef uint32_t CVAttachmentMode;
enum {
    kCVAttachmentMode_ShouldNotPropagate = 0,
    kCVAttachmentMode_ShouldPropagate = 1,
};

void CVBufferSetAttachment(CVBufferRef buffer, CFStringRef key, CFTypeRef value,
                           CVAttachmentMode attachmentMode);

extern const CFStringRef kCVImageBufferYCbCrMatrixKey;
extern const CFStringRef kCVImageBufferYCbCrMatrix_ITU_R_601_4;
extern const CFStringRef kCVImageBufferColorPrimariesKey;
extern const CFStringRef kCVImageBufferColorPrimaries_ITU_R_709_2;
extern const CFStringRef kCVImageBufferTransferFunctionKey;
extern const CFStringRef kCVImageBufferTransferFunction_sRGB;
