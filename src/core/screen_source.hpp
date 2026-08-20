#pragma once

#include <string>

#include "render/texture.hpp"

namespace ds {

struct ScreenSize {
    int width = 0;
    int height = 0;
};

// The two logical screens for the current frame, as GPU textures.
struct ScreenTextures {
    const Texture* top = nullptr;
    const Texture* bottom = nullptr;

    bool valid() const {
        return top != nullptr && bottom != nullptr && top->valid() && bottom->valid();
    }
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

    // Native size of one screen, in the source's own pixels (256x192 for DS).
    virtual ScreenSize native_screen_size() const = 0;

    // True once the source has decided it is finished (e.g. the core asked the
    // frontend to shut down).
    virtual bool wants_shutdown() const { return false; }

};

}  // namespace ds
