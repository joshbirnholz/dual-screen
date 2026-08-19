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
    Color background{0.04f, 0.05f, 0.07f, 1.0f};
    Color screen_tint{1.0f, 1.0f, 1.0f, 1.0f};
    Color placeholder{0.12f, 0.14f, 0.18f, 1.0f};
    // Drawn where a screen was routed away from this device, so the player can
    // see the layout is intact even though the picture is elsewhere.
    Color remote_placeholder{0.10f, 0.18f, 0.26f, 1.0f};
};

// Draws a resolved layout's screens into the output window.
class Compositor {
public:
    void set_style(const CompositorStyle& style) { style_ = style; }
    const CompositorStyle& style() const { return style_; }

    // `resolved_layout` must already have the per-game screen-swap flag applied.
    void render(QuadRenderer& quads,
                const Layout& resolved_layout,
                const ScreenTextures& screens,
                float output_width,
                float output_height,
                OutputRouter& router);

private:
    CompositorStyle style_;
};

}  // namespace ds
