#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "render/quad_renderer.hpp"
#include "render/texture.hpp"

namespace ds {

// The frame a menu opened over, blurred and darkened so the menu on top of it
// stays readable while the game stays recognisable underneath.
//
// Captured when the picture behind it changes rather than rebuilt every frame:
// the game is paused, so most frames have nothing new to blur, and a blur wide
// enough to read as one is not worth paying for sixty times a second. What does
// change while the menu is up is the arrangement of the screens, since picking a
// layout applies it underneath -- so the copy is keyed to what was drawn.
// How dark a game that goes on running is under a menu -- a Wii U game, which
// Cemu cannot pause -- drawn live rather than blurred: as dark as a paused one
// is under a page opened from the menu, its blurred picture with 60% more
// black over it.
inline constexpr float kLiveGameDim = 0.8f;

class MenuBackdrop {
public:
    // Reads whatever is in the framebuffer right now. Call after the game has
    // been composited and before the menu is drawn over it. `key` says what was
    // composited -- see Compositor::composition_key().
    void capture(int output_width, int output_height, std::size_t key);
    // The same from a picture rather than the framebuffer: a game drawn by
    // another program, whose frame this app has as pixels and never drew --
    // a Wii U's television, as Cemu sent it when the game paused. `rgba` says
    // the bytes are a GPU's R, G, B, A rather than XRGB8888. Stretched to
    // `output_width` x `output_height`, the window it goes behind.
    void capture_image(const uint32_t* pixels, int width, int height, int stride_pixels, bool rgba,
                       int output_width, int output_height, std::size_t key);
    // Dropped when the menu closes, so the next one opens onto the frame it
    // actually opened over.
    void forget();
    // Whether what was copied is still what is underneath.
    bool matches(std::size_t key) const { return texture_.valid() && key_ == key; }

    void draw(QuadRenderer& quads, float output_width, float output_height) const;

private:
    Texture texture_;
    std::size_t key_ = 0;
    std::vector<uint32_t> pixels_;
    std::vector<uint32_t> scratch_;
    int width_ = 0;
    int height_ = 0;
};

// Blurs and adjusts an XRGB8888 image in place, exposed for its own sake so the
// arithmetic can be checked without a GPU. `blur_radius` is the standard
// deviation of the gaussian being approximated, in pixels of this image.
void blur_and_darken(std::vector<uint32_t>& pixels,
                     std::vector<uint32_t>& scratch,
                     int width,
                     int height,
                     float blur_radius);

}  // namespace ds
