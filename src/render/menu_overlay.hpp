#pragma once

#include <cstdint>
#include <vector>

#include "render/texture.hpp"

namespace ds {

// The quick menu drawn on its own, with nothing behind it, for another program
// to put over its window -- this app's menu over a Wii U game in Cemu's. See
// ExternalFrameLink::publish_overlay().
//
// Drawn into a texture of its own rather than the window: cleared to nothing,
// so only what the menu covers has any alpha, and blended so that alpha
// accumulates the way a window's own compositing would (the colour comes out
// premultiplied, and is put back to straight on the way out). Read back each
// frame it is drawn; changed() says whether that differs from the last.
class MenuOverlay {
public:
    ~MenuOverlay();

    // Whether this context can draw one at all: it needs framebuffers and a
    // separate blend for alpha.
    static bool supported();

    // The size to draw at for a window `width` x `height`: the same shape,
    // no bigger than `max_width` x `max_height`.
    static void fit(int width, int height, int max_width, int max_height, int& out_width,
                    int& out_height);

    // Points drawing at the overlay, `width` x `height`, cleared to nothing
    // and blending as above. False when it could not be made; the window's
    // framebuffer is bound again in that case.
    bool begin(int width, int height);
    // Reads what was drawn since begin() into pixels(), top row first, RGBA8
    // with straight alpha, and binds the window's framebuffer again. True
    // when that differs from what the last call read.
    bool finish();

    const std::vector<uint8_t>& pixels() const { return pixels_; }
    int width() const { return width_; }
    int height() const { return height_; }
    // Forgets what was last read, so the next finish() reports a change
    // whatever it reads -- for an overlay the far end has stopped showing.
    void forget() { last_.clear(); }

    // The arithmetic of finish(), apart from the GPU: flips `rows` rows of
    // `width` RGBA8 pixels read bottom row first into top row first, and
    // divides each colour by its alpha.
    static void straighten(const std::vector<uint8_t>& read, int width, int rows,
                           std::vector<uint8_t>& out);

private:
    void release();

    Texture texture_;
    unsigned framebuffer_ = 0;
    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> read_;
    std::vector<uint8_t> pixels_;
    std::vector<uint8_t> last_;
};

}  // namespace ds
