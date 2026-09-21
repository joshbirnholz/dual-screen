// Apple's, reduced to the three this project asks about.
//
// Each is overridable from the command line so the one file can be parsed as
// each platform sees it: the orientation code is compiled out on tvOS, and
// code that only ever builds on one of the two is code nobody has checked.
#pragma once
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
#endif
#ifndef TARGET_OS_TV
#define TARGET_OS_TV 0
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#endif
