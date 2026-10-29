#pragma once

#include <cstddef>
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
    // Shown by the operating system in a layer of its own laid over the
    // window, exactly where the compositor would have drawn it -- see
    // SystemVideoLayers. The compositor leaves the place black underneath.
    SystemLayer,
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
    // Called for each screen routed to OutputRoute::SystemLayer, with where it
    // landed and whether smoothing was asked for -- the texture is not used,
    // and need not be current.
    virtual void place_in_layer(const PlacedScreen& placed, bool smoothing_requested,
                                float output_width, float output_height);
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

// Whether a screen should be resampled on its way to `rect`, given what the
// player asked for. Smoothing is a request rather than an instruction, and this
// is the one place that decides what to do with it -- the compositor and the
// menu's previews both draw screens, and a rule that lived in only one of them
// would be a rule the other quietly disagreed with.
// `console_size` is the console's own size for that screen: a magnified picture
// that is not a whole multiple of it is always smoothed, whatever was asked for.
bool screen_needs_smoothing(const PlacedScreen& placed,
                            int texture_width,
                            int texture_height,
                            const ScreenSize& console_size,
                            bool requested);
bool screen_needs_smoothing(const PlacedScreen& placed,
                            const Texture& texture,
                            const ScreenSize& console_size,
                            bool requested);

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

    // An LCD's pixel grid over each screen drawn here, of the console's own
    // pixels -- a DS's 256 by 192 -- or none for 0. Drawn only where the
    // screen is drawn by this compositor.
    void set_lcd_grid(int cells_across, int cells_down) {
        lcd_cells_across_ = cells_across;
        lcd_cells_down_ = cells_down;
    }

    // The display the picture actually lands on, when that is not the size
    // being drawn at. A console hands its window the size of its docked
    // output whichever way it is being held, and scales a 1080p picture down
    // onto a 720p panel undocked -- so the window says one thing and the
    // screen in front of the player is another. Layout decisions that are
    // about how much room there really is, chiefly whether to spend any of it
    // on margin, want the panel. Zero means they are the same thing, which is
    // true of every ordinary display.
    void set_display_size(float width, float height) {
        display_width_ = width;
        display_height_ = height;
    }

    void render(QuadRenderer& quads,
                const Layout& layout,
                const ScreenTextures& textures,
                const SystemScreens& screens,
                float output_width,
                float output_height,
                OutputRouter& router);

    // What the last render put on the screen, as one number: where every screen
    // landed, whether each had a picture, and whether a console was drawn
    // around them. Anything that moves a screen changes it, without this
    // having to be told what the settings are.
    // The menu's blurred backdrop uses it to notice that the picture it copied
    // is no longer the picture underneath it.
    std::size_t composition_key() const { return composition_key_; }

private:
    CompositorStyle style_;
    const Texture* bezel_art_ = nullptr;
    bool smoothing_ = false;
    int lcd_cells_across_ = 0;
    int lcd_cells_down_ = 0;
    float display_width_ = 0.0f;
    float display_height_ = 0.0f;
    std::size_t composition_key_ = 0;
};

}  // namespace ds
