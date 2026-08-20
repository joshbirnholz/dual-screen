#pragma once

#include <string>
#include <vector>

#include "core/screen_source.hpp"
#include "layout/layout.hpp"
#include "render/quad_renderer.hpp"

namespace ds {

// Where a screen's pixels go this frame.
enum class OutputRoute {
    Local,   // drawn into this device's compositor window
    Remote,  // handed to something else (a companion device, later)
};

// The single decision point for "which screens render locally vs. get handed
// off elsewhere". V1 always answers Local. When the networked phase lands, one
// screen's route becomes Remote and its texture goes to an encoder instead of
// the window — a swap-in here, not a refactor spread through the compositor.
class OutputRouter {
public:
    virtual ~OutputRouter() = default;
    virtual OutputRoute route_for(ScreenId screen) const = 0;
    // Called for each screen routed away from the local window. The rect is
    // where that screen *would* have gone locally, which a future encoder can
    // ignore and a future local-preview mode can use.
    virtual void hand_off(ScreenId screen, const Texture& texture, const PixelRect& rect);
};

// V1's router: everything renders locally.
class LocalOutputRouter final : public OutputRouter {
public:
    OutputRoute route_for(ScreenId screen) const override;
};

struct CompositorStyle {
    // Pure black: anything lighter reads as a visible frame around the screens
    // on an OLED panel or in a dark room.
    Color background{0.0f, 0.0f, 0.0f, 1.0f};
    Color screen_tint{1.0f, 1.0f, 1.0f, 1.0f};
    Color placeholder{0.12f, 0.14f, 0.18f, 1.0f};
    // Drawn where a screen was routed away from this device, so the player can
    // see the layout is intact even though the picture is elsewhere.
    Color remote_placeholder{0.10f, 0.18f, 0.26f, 1.0f};
    // What surrounds the console artwork. It is the artwork's own background
    // colour, so the picture has no edge: the frame simply ends.
    Color bezel_background{37.0f / 255.0f, 37.0f / 255.0f, 37.0f / 255.0f, 1.0f};
};

// Draws the console artwork into `frame`, behind the screens the Standard layout
// places inside it. Shared with the layout picker so a preview tile looks like
// what picking it will give you. With no artwork loaded it draws nothing, and
// the screens still land where they belong.
void draw_ds_shell(QuadRenderer& quads, const PixelRect& frame, const Texture* art);

// Draws a resolved layout's screens into the output window.
class Compositor {
public:
    void set_style(const CompositorStyle& style) { style_ = style; }
    const CompositorStyle& style() const { return style_; }

    // The console artwork, for the layout that draws one. Set once, after the
    // GL context exists; null simply means nothing is drawn behind the screens.
    void set_bezel_art(const Texture* art) { bezel_art_ = art; }

    // Whether the player asked for smooth scaling. It is a request rather than
    // an instruction: a screen drawn at its own size is left alone regardless,
    // since smoothing it could only make it worse.
    void set_smoothing(bool smooth) { smoothing_ = smooth; }

    void render(QuadRenderer& quads,
                const Layout& layout,
                const ScreenTextures& textures,
                const SystemScreens& screens,
                float output_width,
                float output_height,
                OutputRouter& router);

private:
    CompositorStyle style_;
    const Texture* bezel_art_ = nullptr;
    bool smoothing_ = false;
};

}  // namespace ds
