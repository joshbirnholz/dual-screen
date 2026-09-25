// The slice of CoreFoundation that apple/system_video_layer.m names, as C --
// tests/apple_stub has its own, but as C++. See UIKit/UIKit.h for what a stub
// here proves and what it does not.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef int32_t OSStatus;
typedef unsigned char Boolean;
typedef long CFIndex;
enum { noErr = 0 };

typedef const void *CFTypeRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFArray *CFArrayRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFBoolean *CFBooleanRef;

extern const CFAllocatorRef kCFAllocatorDefault;
extern const CFBooleanRef kCFBooleanTrue;

void CFRelease(CFTypeRef cf);
CFIndex CFArrayGetCount(CFArrayRef theArray);
const void *CFArrayGetValueAtIndex(CFArrayRef theArray, CFIndex idx);
void CFDictionarySetValue(CFMutableDictionaryRef theDict, const void *key, const void *value);
