// The SDL this file uses, stubbed. See UIKit/UIKit.h for why.
#pragma once
typedef struct SDL_Window SDL_Window;
typedef enum { SDL_FALSE = 0, SDL_TRUE = 1 } SDL_bool;
void SDL_Log(const char *fmt, ...);

// The version the iOS build fetches by default (apple/CMakeLists.txt), so the
// "this SDL is too old" guard in ios_scene_lifecycle.m sees what a real build
// would. Spelled as SDL_version.h spells them.
#define SDL_MAJOR_VERSION 2
#define SDL_MINOR_VERSION 32
#define SDL_PATCHLEVEL 10
#define SDL_VERSIONNUM(X, Y, Z) ((X) * 1000 + (Y) * 100 + (Z))
#define SDL_COMPILEDVERSION SDL_VERSIONNUM(SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL)
#define SDL_VERSION_ATLEAST(X, Y, Z) \
    ((SDL_MAJOR_VERSION >= X) && \
     (SDL_MAJOR_VERSION > X || SDL_MINOR_VERSION >= Y) && \
     (SDL_MAJOR_VERSION > X || SDL_MINOR_VERSION > Y || SDL_PATCHLEVEL >= Z))
