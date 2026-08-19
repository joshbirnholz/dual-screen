#include "render/compositor.hpp"

namespace ds {

void OutputRouter::hand_off(ScreenId, const Texture&, const PixelRect&) {
    // Nothing to do until a transport exists.
}

OutputRoute LocalOutputRouter::route_for(ScreenId) const { return OutputRoute::Local; }

void Compositor::render(QuadRenderer& quads,
                        const Layout& resolved_layout,
                        const ScreenTextures& screens,
                        float output_width,
                        float output_height,
                        OutputRouter& router) {
    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(style_.background);

    for (const ScreenPlacement& placement : resolved_layout.placements) {
        const PixelRect rect = placement_pixel_rect(placement, output_width, output_height);
        const Texture* texture =
            (placement.screen == ScreenId::Top) ? screens.top : screens.bottom;

        // The one place that decides local vs. elsewhere.
        const OutputRoute route = router.route_for(placement.screen);
        if (route != OutputRoute::Local) {
            quads.draw_solid(rect, style_.remote_placeholder);
            if (texture != nullptr && texture->valid()) {
                router.hand_off(placement.screen, *texture, rect);
            }
            continue;
        }

        if (texture != nullptr && texture->valid()) {
            quads.draw_screen(rect, *texture, style_.screen_tint);
        } else {
            // No frame yet (core still booting): keep the layout's shape visible
            // rather than flashing the background colour.
            quads.draw_solid(rect, style_.placeholder);
        }
    }
}

}  // namespace ds
