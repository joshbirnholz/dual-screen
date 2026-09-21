// See UIKit/UIKit.h. The union is the point: the real SDL_SysWMinfo holds its
// UIWindow in one, which is why ios_scene_lifecycle.m cannot be built with ARC.
#pragma once
#include <SDL.h>

typedef struct { int major, minor, patch; } SDL_version;
#define SDL_VERSION(v) do { (v)->major = 2; (v)->minor = 32; (v)->patch = 8; } while (0)

typedef enum { SDL_SYSWM_UNKNOWN = 0, SDL_SYSWM_UIKIT } SDL_SYSWM_TYPE;

typedef struct SDL_SysWMinfo {
    SDL_version version;
    SDL_SYSWM_TYPE subsystem;
    union {
        struct {
            UIWindow *window;
            unsigned int framebuffer;
            unsigned int colorbuffer;
            unsigned int resolveFramebuffer;
        } uikit;
        int dummy;
    } info;
} SDL_SysWMinfo;

SDL_bool SDL_GetWindowWMInfo(SDL_Window *window, SDL_SysWMinfo *info);
