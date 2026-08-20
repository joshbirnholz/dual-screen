#include "render/compositor.hpp"

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
                        const ScreenTextures& screens,
                        float output_width,
                        float output_height,
                        OutputRouter& router) {
    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(layout.bezel ? style_.bezel_background : style_.background);

    if (layout.bezel) {
        draw_ds_shell(quads, layout_frame(layout, output_width, output_height), bezel_art_);
    }

    for (const PlacedScreen& placed : place_screens(layout, output_width, output_height)) {
        const PixelRect& rect = placed.rect;
        const Texture* texture = (placed.screen == ScreenId::Top) ? screens.top : screens.bottom;

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
