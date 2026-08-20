#pragma once

#include <string>

#include "layout/layout.hpp"
#include "render/texture.hpp"

namespace ds {

// The two logical screens for the current frame, as GPU textures.
struct ScreenTextures {
    const Texture* top = nullptr;
    const Texture* bottom = nullptr;

    bool valid() const {
        return top != nullptr && bottom != nullptr && top->valid() && bottom->valid();
    }
};

// One screen's pixels on the CPU, at the console's own resolution, for sending
// to another device. Kept separate from ScreenTextures because a texture is
// what this machine draws and this is what leaves it: a source that renders on
// the GPU has no CPU copy of its frame at all and has to be asked for one.
struct ScreenReadback {
    // XRGB8888, the byte order the wire encoder reads.
    const uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride_pixels = 0;
    bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
};

// Seam: something that produces two textures (top/bottom) per frame plus their
// native resolution.
//
// V1's only implementation is LibretroCoreScreenSource, which wraps the melonDS
// DS core. A future 3DS ScreenSource, and a future Wii U source that captures
// an emulator window via the desktop portal / PipeWire, plug in here. The
// compositor and the layout system never learn which one is in use.
class ScreenSource {
public:
    virtual ~ScreenSource() = default;

    // Produces the next frame. Returns false on a fatal error, with a
    // human-readable reason in `error`.
    virtual bool advance_frame(std::string* error) = 0;

    // The current frame's screens as textures. Valid until the next
    // advance_frame(). May report invalid textures before the first frame.
    virtual const ScreenTextures& screens() = 0;

    // The screens this source produces, at their own sizes -- which are not
    // necessarily the same as each other.
    virtual SystemScreens screens_geometry() const = 0;

    // One screen as CPU pixels, valid until the next call or the next frame.
    // Only what is actually being sent should be asked for: on a source that
    // renders on the GPU this reads back off the card, which is not free.
    // False when there is nothing to read yet.
    virtual bool read_screen(ScreenId screen, ScreenReadback& out) {
        (void)screen;
        (void)out;
        return false;
    }

    // True once the source has decided it is finished (e.g. the core asked the
    // frontend to shut down).
    virtual bool wants_shutdown() const { return false; }

};

}  // namespace ds
