// See tests/apple_stub/README.md. Declared as CoreFoundation declares them.
#pragma once

#include <TargetConditionals.h>

#include <cstddef>
#include <cstdint>

extern "C" {

typedef unsigned char Boolean;
typedef long CFIndex;
typedef int32_t OSStatus;
typedef uint32_t OSType;
enum { noErr = 0 };

typedef const void* CFTypeRef;
typedef const struct __CFAllocator* CFAllocatorRef;
typedef const struct __CFString* CFStringRef;
typedef const struct __CFDictionary* CFDictionaryRef;
typedef struct __CFDictionary* CFMutableDictionaryRef;
typedef const struct __CFNumber* CFNumberRef;
typedef const struct __CFBoolean* CFBooleanRef;

extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFAllocatorRef kCFAllocatorNull;

void CFRelease(CFTypeRef cf);
CFTypeRef CFRetain(CFTypeRef cf);

typedef struct {
    CFIndex version;
    const void* retain;
    const void* release;
    const void* copyDescription;
    const void* equal;
    const void* hash;
} CFDictionaryKeyCallBacks;
typedef struct {
    CFIndex version;
    const void* retain;
    const void* release;
    const void* copyDescription;
    const void* equal;
} CFDictionaryValueCallBacks;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef allocator, CFIndex capacity,
                                                 const CFDictionaryKeyCallBacks* keyCallBacks,
                                                 const CFDictionaryValueCallBacks* valueCallBacks);
CFDictionaryRef CFDictionaryCreate(CFAllocatorRef allocator, const void** keys,
                                   const void** values, CFIndex numValues,
                                   const CFDictionaryKeyCallBacks* keyCallBacks,
                                   const CFDictionaryValueCallBacks* valueCallBacks);
void CFDictionarySetValue(CFMutableDictionaryRef theDict, const void* key, const void* value);

typedef CFIndex CFNumberType;
enum { kCFNumberSInt32Type = 3 };
CFNumberRef CFNumberCreate(CFAllocatorRef allocator, CFNumberType theType, const void* valuePtr);

extern const CFBooleanRef kCFBooleanTrue;
extern const CFBooleanRef kCFBooleanFalse;
Boolean CFBooleanGetValue(CFBooleanRef boolean);

}  // extern "C"
