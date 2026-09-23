// See tests/apple_stub/README.md. Declared as CoreVideo declares them.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#if TARGET_OS_IPHONE
#include <OpenGLES/gltypes.h>
#endif

extern "C" {

typedef int32_t CVReturn;
enum { kCVReturnSuccess = 0 };
typedef uint64_t CVOptionFlags;

typedef struct __CVBuffer* CVBufferRef;
typedef CVBufferRef CVImageBufferRef;
typedef CVImageBufferRef CVPixelBufferRef;

typedef CVOptionFlags CVPixelBufferLockFlags;
enum { kCVPixelBufferLock_ReadOnly = 0x00000001 };
enum { kCVPixelFormatType_420YpCbCr8BiPlanarFullRange = '420f' };

extern const CFStringRef kCVPixelBufferPixelFormatTypeKey;
extern const CFStringRef kCVPixelBufferIOSurfacePropertiesKey;
#if TARGET_OS_IPHONE
extern const CFStringRef kCVPixelBufferOpenGLESCompatibilityKey;
#else
extern const CFStringRef kCVPixelBufferOpenGLCompatibilityKey;
#endif

CVPixelBufferRef CVPixelBufferRetain(CVPixelBufferRef texture);
void CVPixelBufferRelease(CVPixelBufferRef texture);
CVReturn CVPixelBufferLockBaseAddress(CVPixelBufferRef pixelBuffer, CVPixelBufferLockFlags lockFlags);
CVReturn CVPixelBufferUnlockBaseAddress(CVPixelBufferRef pixelBuffer,
                                        CVPixelBufferLockFlags unlockFlags);
size_t CVPixelBufferGetWidth(CVPixelBufferRef pixelBuffer);
size_t CVPixelBufferGetHeight(CVPixelBufferRef pixelBuffer);
OSType CVPixelBufferGetPixelFormatType(CVPixelBufferRef pixelBuffer);
void* CVPixelBufferGetBaseAddressOfPlane(CVPixelBufferRef pixelBuffer, size_t planeIndex);
size_t CVPixelBufferGetBytesPerRowOfPlane(CVPixelBufferRef pixelBuffer, size_t planeIndex);
size_t CVPixelBufferGetWidthOfPlane(CVPixelBufferRef pixelBuffer, size_t planeIndex);
size_t CVPixelBufferGetHeightOfPlane(CVPixelBufferRef pixelBuffer, size_t planeIndex);

#if TARGET_OS_IPHONE
// CVOpenGLESTexture.h and CVOpenGLESTextureCache.h. Outside Objective-C the
// context is a plain pointer.
typedef CVImageBufferRef CVOpenGLESTextureRef;
typedef struct __CVOpenGLESTextureCache* CVOpenGLESTextureCacheRef;
typedef void* CVEAGLContext;

GLenum CVOpenGLESTextureGetTarget(CVOpenGLESTextureRef image);
GLuint CVOpenGLESTextureGetName(CVOpenGLESTextureRef image);
CVReturn CVOpenGLESTextureCacheCreate(CFAllocatorRef allocator, CFDictionaryRef cacheAttributes,
                                      CVEAGLContext eaglContext,
                                      CFDictionaryRef textureAttributes,
                                      CVOpenGLESTextureCacheRef* cacheOut);
CVReturn CVOpenGLESTextureCacheCreateTextureFromImage(
    CFAllocatorRef allocator, CVOpenGLESTextureCacheRef textureCache, CVImageBufferRef sourceImage,
    CFDictionaryRef textureAttributes, GLenum target, GLint internalFormat, GLsizei width,
    GLsizei height, GLenum format, GLenum type, size_t planeIndex,
    CVOpenGLESTextureRef* textureOut);
void CVOpenGLESTextureCacheFlush(CVOpenGLESTextureCacheRef textureCache, CVOptionFlags options);
#endif

}  // extern "C"
