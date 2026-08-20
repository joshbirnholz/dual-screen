#pragma once

#include <string>

#include "render/gl_loader.hpp"

namespace ds {

// The framebuffer a GPU-rendering libretro core draws into.
//
// A core that renders on the CPU hands the frontend a buffer of pixels. A core
// that renders on the GPU does the opposite: it asks the frontend for a
// framebuffer object, draws into it, and says "the picture is in there". This
// owns that object -- a colour texture, plus the depth and stencil the core
// asked for -- and hands out its name through get_current_framebuffer.
//
// The texture is kept at the largest size the core declared, so a core that
// changes its output size (Citra does, with its resolution multiplier) does not
// force a reallocation mid-game.
class HwRenderTarget {
public:
    ~HwRenderTarget();

    HwRenderTarget() = default;
    HwRenderTarget(const HwRenderTarget&) = delete;
    HwRenderTarget& operator=(const HwRenderTarget&) = delete;

    // Allocates for a core that wants `width` x `height` at most. Safe to call
    // again with a larger size; a smaller one is ignored, since the existing
    // texture already covers it.
    // `exact` replaces the current storage even when it is larger, for the
    // moment the real size becomes known and the placeholder guessed before it
    // should stop counting as a floor.
    bool create(unsigned width, unsigned height, bool want_depth, bool want_stencil, bool exact,
                std::string* error);
    void destroy();

    bool valid() const { return framebuffer_ != 0; }
    GLuint framebuffer() const { return framebuffer_; }
    GLuint texture() const { return texture_; }
    unsigned width() const { return width_; }
    unsigned height() const { return height_; }

private:
    GLuint framebuffer_ = 0;
    GLuint texture_ = 0;
    GLuint depth_stencil_ = 0;
    unsigned width_ = 0;
    unsigned height_ = 0;
    bool has_depth_ = false;
    bool has_stencil_ = false;
};

}  // namespace ds
