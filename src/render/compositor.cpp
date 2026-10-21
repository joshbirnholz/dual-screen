#include "render/compositor.hpp"

#include <cmath>
#include <functional>

#include "util/log.hpp"

namespace ds {

namespace {
// How dark the grid's lines get at their darkest: enough to see the pixels,
// as a handheld's own screen shows them, without dimming the picture much.
constexpr Color kLcdGridColor{0.0f, 0.0f, 0.0f, 0.32f};
}  // namespace

void OutputRouter::hand_off(ScreenId, const Texture&, const PixelRect&) {
    // Nothing to do until a transport exists.
}

void OutputRouter::place_in_layer(const PlacedScreen&, bool, float, float) {}

OutputRoute LocalOutputRouter::route_for(ScreenId) const { return OutputRoute::Local; }

namespace {

void mix(std::size_t& key, std::size_t value) {
    key ^= value + 0x9e3779b97f4a7c15ULL + (key << 6) + (key >> 2);
}

void mix(std::size_t& key, float value) {
    mix(key, std::hash<float>{}(value));
}

std::size_t key_for_composition(const std::vector<PlacedScreen>& placed, bool draws_console,
                                const ScreenTextures& textures) {
    std::size_t key = draws_console ? 1u : 2u;
    for (const PlacedScreen& screen : placed) {
        mix(key, static_cast<std::size_t>(screen.screen));
        // Whether there was a picture to draw there at all. A companion that
        // joins while the menu is up takes the backdrop before the host's
        // first picture lands; without this, the empty screen it copied would
        // stay behind the menu for as long as the menu did.
        const Texture* texture = screen.screen == ScreenId::Top ? textures.top : textures.bottom;
        mix(key, texture != nullptr && texture->valid() ? std::size_t{1} : std::size_t{0});
        mix(key, static_cast<std::size_t>(screen.rotation));
        mix(key, screen.rect.x);
        mix(key, screen.rect.y);
        mix(key, screen.rect.w);
        mix(key, screen.rect.h);
    }
    return key;
}

}  // namespace

bool screen_needs_smoothing(const PlacedScreen& placed,
                            int texture_width,
                            int texture_height,
                            bool requested) {
    if (placed.rect.w <= 0.0f || placed.rect.h <= 0.0f) {
        return requested;
    }
    // A quarter turn swaps which way round the texture meets the rect.
    const bool turned = rotation_is_quarter_turn(placed.rotation);
    const float source_width = static_cast<float>(turned ? texture_height : texture_width);
    const float source_height = static_cast<float>(turned ? texture_width : texture_height);
    if (source_width <= 0.0f || source_height <= 0.0f) {
        return requested;
    }
    // One texel per pixel: there is nothing to resample, and sampling it anyway
    // could only soften an image that was already exact.
    if (std::fabs(source_width - placed.rect.w) < 0.5f &&
        std::fabs(source_height - placed.rect.h) < 0.5f) {
        return false;
    }
    // More texture than there is room for, which is what a core rendering above
    // the size it is drawn at produces. Point-sampling a reduction drops whole
    // rows and columns, and the entire reason to render at 2x is to have that
    // detail arrive -- so "keep it crisp" is read as a statement about being
    // magnified, which is the case it was asked for.
    if (source_width > placed.rect.w + 0.5f || source_height > placed.rect.h + 0.5f) {
        return true;
    }
    return requested;
}

bool screen_needs_smoothing(const PlacedScreen& placed, const Texture& texture, bool requested) {
    if (!texture.valid()) {
        return requested;
    }
    return screen_needs_smoothing(placed, texture.width(), texture.height(), requested);
}

void draw_ds_shell(QuadRenderer& quads, const PixelRect& frame, const Texture* art) {
    if (frame.w <= 0.0f || frame.h <= 0.0f || art == nullptr || !art->valid()) {
        return;
    }
    // Always smoothed: it is a photograph of plastic drawn at whatever size the
    // window is, not pixel art.
    quads.draw_screen(frame, *art, Color{}, ScreenRotation::None, /*smooth=*/true);
}

void Compositor::render(QuadRenderer& quads,
                        const Layout& layout,
                        const ScreenTextures& textures,
                        const SystemScreens& screens,
                        float output_width,
                        float output_height,
                        OutputRouter& router) {
    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));

    // A console drawn from artwork sits on the artwork's own background, so the
    // picture has no edge. A console with no artwork yet is simply blank around
    // its screens, and blank is the black everything else uses.
    const bool draws_console = layout.uses_console_frame && screens.console.exists();
    quads.clear(draws_console && screens.console.has_art ? style_.bezel_background
                                                         : style_.background);

    if (draws_console) {
        draw_ds_shell(quads,
                      layout_frame(layout, PixelRect{0.0f, 0.0f, output_width, output_height},
                                   screens, display_width_, display_height_),
                      screens.console.has_art ? bezel_art_ : nullptr);
    }

    const std::vector<PlacedScreen> placements =
        place_screens(layout, PixelRect{0.0f, 0.0f, output_width, output_height}, screens,
                      display_width_, display_height_);
    composition_key_ = key_for_composition(placements, draws_console, textures);

    for (const PlacedScreen& placed : placements) {
        const PixelRect& rect = placed.rect;
        const Texture* texture = (placed.screen == ScreenId::Top) ? textures.top : textures.bottom;

        // The one place that decides local vs. elsewhere.
        const OutputRoute route = router.route_for(placed.screen);
        if (route == OutputRoute::SystemLayer) {
            // Covered by the layer; black beneath it, so a layer a moment
            // late in appearing shows nothing rather than the backdrop.
            quads.draw_solid(rect, style_.background);
            router.place_in_layer(placed, smoothing_, output_width, output_height);
            continue;
        }
        if (route != OutputRoute::Local) {
            quads.draw_solid(rect, style_.remote_placeholder);
            if (texture != nullptr && texture->valid()) {
                router.hand_off(placed.screen, *texture, rect);
            }
            continue;
        }

        if (texture != nullptr && texture->valid()) {
            // Once. "The window is black" has several possible meanings and this
            // separates them: whether a screen was placed at all, where, from
            // which texture, and with what tint.
            static bool reported = false;
            if (!reported) {
                reported = true;
                log_info("compositing a screen: rect " + std::to_string((int)rect.x) + "," +
                         std::to_string((int)rect.y) + " " + std::to_string((int)rect.w) + "x" +
                         std::to_string((int)rect.h) + " from texture " +
                         std::to_string(texture->id()) + " " + std::to_string(texture->width()) +
                         "x" + std::to_string(texture->height()) + ", tint rgba(" +
                         std::to_string(style_.screen_tint.r) + "," +
                         std::to_string(style_.screen_tint.g) + "," +
                         std::to_string(style_.screen_tint.b) + "," +
                         std::to_string(style_.screen_tint.a) + ")");
            }
            quads.draw_screen(rect, *texture, style_.screen_tint, placed.rotation,
                              screen_needs_smoothing(placed, *texture, smoothing_));
            if (lcd_cells_across_ > 0 && lcd_cells_down_ > 0) {
                const bool sideways = rotation_is_quarter_turn(placed.rotation);
                quads.draw_lcd_grid(rect, sideways ? lcd_cells_down_ : lcd_cells_across_,
                                    sideways ? lcd_cells_across_ : lcd_cells_down_, kLcdGridColor);
            }
        } else {
            // No frame yet (core still booting): keep the layout's shape visible
            // rather than flashing the background colour.
            quads.draw_solid(rect, style_.placeholder);
        }
    }
}

}  // namespace ds
