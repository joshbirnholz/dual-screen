// See tests/apple_stub/README.md. iOS unless the compile line says otherwise.
#pragma once
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#endif
#ifndef TARGET_OS_TV
#define TARGET_OS_TV 0
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX (!TARGET_OS_IPHONE)
#endif
