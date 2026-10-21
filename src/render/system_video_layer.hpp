#pragma once

#include <SDL.h>

#include <cstdint>
#include <memory>

#include "core/screen_source.hpp"
#include "layout/layout.hpp"

namespace ds {

// Streamed video shown by the operating system rather than drawn with GL.
//
// On iOS and tvOS a decoded picture is a CVPixelBuffer that the display
// hardware can already read (see DecodedFrame::native). Drawing it with GL
// means mapping it into a texture, converting it to colour in a shader and
// compositing the result through Apple's GL, which is itself a layer over
// Metal and the part of that platform most prone to stalling. Handing the
// same buffer to an AVSampleBufferDisplayLayer instead skips all of it: the
// system shows the picture as it is, on the next refresh, the way Moonlight
// and every video player on the platform do.
//
// The layer sits over the window exactly where the compositor would have drawn
// the screen, and only while nothing is drawn over the screen -- with the
// quick menu open the picture goes back through GL, which has to see it to blur
// it behind the menu. The same buffer serves either way, so switching costs
// nothing: no keyframe, no second decoder.
//
// Everywhere but iOS and tvOS, available() is false and the rest does nothing.
// DUALSCREEN_VIDEO_LAYER=0 turns it off there too, and so does the layer
// failing: after a few failures it is left alone for the rest of the run and
// everything is drawn with GL, as before.
class SystemVideoLayers {
public:
    SystemVideoLayers();
    ~SystemVideoLayers();
    SystemVideoLayers(const SystemVideoLayers&) = delete;
    SystemVideoLayers& operator=(const SystemVideoLayers&) = delete;

    // Whether this build can have them at all.
    static bool available();

    // Finds the window's view. Returns false where there is nothing to attach
    // to, after which usable() stays false.
    bool attach(SDL_Window* window);
    // Whether a screen may be routed to a layer this frame.
    bool usable() const;

    // Before anything is placed this frame.
    void begin_frame();
    // `picture` is the CVPixelBuffer the decoder handed back, and `serial`
    // the frame it belongs to -- only a new one is given to the system.
    void show(ScreenId screen, const std::shared_ptr<void>& picture, uint64_t serial,
              const PlacedScreen& placed, bool smooth, float output_width, float output_height);
    // After the frame is drawn: a layer not shown this frame is hidden.
    void end_frame();
    // A session has ended: hidden, and the last picture let go of rather than
    // kept to show again. A hidden layer still holds what it last showed, and
    // shown again it puts that up until the next session's first picture
    // replaces it.
    void forget();

private:
    struct Layer {
        void* handle = nullptr;
        uint64_t serial = 0;
        bool shown_this_frame = false;
        bool visible = false;
    };
    Layer& layer_for(ScreenId screen) { return screen == ScreenId::Top ? top_ : bottom_; }
    void hide(Layer& layer);

    void* view_ = nullptr;
    Layer top_;
    Layer bottom_;
    // Only ever touched where there are layers to fail -- see available().
    [[maybe_unused]] unsigned failures_ = 0;
    bool given_up_ = false;
    [[maybe_unused]] bool said_working_ = false;
};

}  // namespace ds
