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
    // Usually XRGB8888, the byte order the wire encoder reads by default --
    // except a screen read back off the GPU, whose channel_order says so, so
    // the CPU pass that would otherwise put it back in that order can be
    // skipped rather than paid for on every frame.
    const uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride_pixels = 0;
    ChannelOrder channel_order = ChannelOrder::XrgbBytes;
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
    //
    // `target_width`/`target_height` say how big a picture the recipient
    // actually needs -- typically the pixel rect that screen occupies in
    // their own window. Zero means the caller doesn't know or doesn't care,
    // which gets the console's own resolution, the same as always. A source
    // that can offer more than that (a core rendering above native) is free
    // to use the hint to send exactly as much as is useful and no more; one
    // that cannot (nothing rendered above native to send) ignores it. Either
    // way the result is never smaller than the console's own resolution and
    // never bigger than what was actually rendered -- there is nothing
    // sharper than native to send, and sending more than was asked for is
    // never a saving.
    virtual bool read_screen(ScreenId screen,
                             ScreenReadback& out,
                             int target_width = 0,
                             int target_height = 0) {
        (void)screen;
        (void)out;
        (void)target_width;
        (void)target_height;
        return false;
    }

    // Which frame is currently on offer. It only moves when the picture
    // actually changed, so whatever is sending screens to another device can
    // tell an unchanged frame from a new one without comparing pixels.
    virtual uint64_t frame_serial() const { return 0; }

    // True once the source has decided it is finished (e.g. the core asked the
    // frontend to shut down).
    virtual bool wants_shutdown() const { return false; }

    // Forwards one physical key event to whatever the source is running, for
    // a core's own on-screen keyboard (a 3DS's swkbd, chiefly) to read.
    // `retro_keycode`/`modifiers` are RETROK_*/RETROKMOD_* from libretro.h.
    // A no-op on anything not hosting a live core -- a companion has nothing
    // local to forward to, and the source that does own the core it should
    // reach is a decision this seam exists so callers never have to make.
    virtual void send_key_event(bool down, unsigned retro_keycode, uint32_t character,
                                uint16_t modifiers) {
        (void)down;
        (void)retro_keycode;
        (void)character;
        (void)modifiers;
    }

};

}  // namespace ds
