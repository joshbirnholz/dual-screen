#include "render/compositor.hpp"

#include "util/log.hpp"

namespace ds {

void OutputRouter::hand_off(ScreenId, const Texture&, const PixelRect&) {
    // Nothing to do until a transport exists.
}

OutputRoute LocalOutputRouter::route_for(ScreenId) const { return OutputRoute::Local; }

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
        draw_ds_shell(quads, layout_frame(layout, output_width, output_height, screens),
                      screens.console.has_art ? bezel_art_ : nullptr);
    }

    for (const PlacedScreen& placed : place_screens(layout, output_width, output_height, screens)) {
        const PixelRect& rect = placed.rect;
        const Texture* texture = (placed.screen == ScreenId::Top) ? textures.top : textures.bottom;

        // The one place that decides local vs. elsewhere.
        const OutputRoute route = router.route_for(placed.screen);
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
                              smoothing_ && !placed.native);
        } else {
            // No frame yet (core still booting): keep the layout's shape visible
            // rather than flashing the background colour.
            quads.draw_solid(rect, style_.placeholder);
        }
    }
}

}  // namespace ds
