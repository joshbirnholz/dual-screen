#include "render/compositor.hpp"

namespace ds {

void OutputRouter::hand_off(ScreenId, const Texture&, const PixelRect&) {
    // Nothing to do until a transport exists.
}

OutputRoute LocalOutputRouter::route_for(ScreenId) const { return OutputRoute::Local; }

namespace {

// The plastic, the darker trim, and the well a screen sits in.
constexpr Color kShellBody{0.78f, 0.79f, 0.81f, 1.0f};
constexpr Color kShellShade{0.62f, 0.63f, 0.66f, 1.0f};
constexpr Color kShellWell{0.07f, 0.07f, 0.08f, 1.0f};
constexpr Color kShellDetail{0.52f, 0.53f, 0.56f, 1.0f};

PixelRect inset(const PixelRect& frame, float x, float y, float w, float h) {
    return PixelRect{frame.x + x * frame.w, frame.y + y * frame.h, w * frame.w, h * frame.h};
}

}  // namespace

void draw_ds_shell(QuadRenderer& quads, const PixelRect& frame) {
    if (frame.w <= 0.0f || frame.h <= 0.0f) {
        return;
    }

    // The two halves, with the hinge line between them.
    quads.draw_solid(inset(frame, 0.0f, 0.0f, 1.0f, 0.5f), kShellBody);
    quads.draw_solid(inset(frame, 0.0f, 0.5f, 1.0f, 0.5f), kShellBody);
    quads.draw_solid(inset(frame, 0.0f, 0.49f, 1.0f, 0.02f), kShellShade);
    // The hinge barrel, which is what stops the two halves reading as one slab.
    quads.draw_solid(inset(frame, 0.30f, 0.475f, 0.40f, 0.05f), kShellDetail);

    // Wells around each screen. The screens themselves are drawn on top of
    // these by the compositor, from the layout's own placements.
    quads.draw_solid(inset(frame, 0.27f, 0.095f, 0.46f, 0.31f), kShellWell);
    quads.draw_solid(inset(frame, 0.27f, 0.595f, 0.46f, 0.31f), kShellWell);

    // Speaker grilles either side of the upper screen.
    for (float x : {0.16f, 0.78f}) {
        for (int row = 0; row < 3; ++row) {
            quads.draw_solid(inset(frame, x, 0.18f + static_cast<float>(row) * 0.045f, 0.06f, 0.02f),
                             kShellShade);
        }
    }

    // The d-pad, as a plus.
    quads.draw_solid(inset(frame, 0.085f, 0.705f, 0.09f, 0.035f), kShellDetail);
    quads.draw_solid(inset(frame, 0.105f, 0.665f, 0.05f, 0.115f), kShellDetail);

    // A, B, X and Y, in their diamond.
    quads.draw_solid(inset(frame, 0.885f, 0.705f, 0.035f, 0.03f), kShellDetail);
    quads.draw_solid(inset(frame, 0.825f, 0.705f, 0.035f, 0.03f), kShellDetail);
    quads.draw_solid(inset(frame, 0.855f, 0.665f, 0.035f, 0.03f), kShellDetail);
    quads.draw_solid(inset(frame, 0.855f, 0.745f, 0.035f, 0.03f), kShellDetail);

    // Start and Select, and the microphone below the lower screen.
    quads.draw_solid(inset(frame, 0.60f, 0.925f, 0.06f, 0.018f), kShellShade);
    quads.draw_solid(inset(frame, 0.69f, 0.925f, 0.06f, 0.018f), kShellShade);
    quads.draw_solid(inset(frame, 0.487f, 0.925f, 0.026f, 0.018f), kShellShade);
}

void Compositor::render(QuadRenderer& quads,
                        const Layout& layout,
                        const ScreenTextures& screens,
                        float output_width,
                        float output_height,
                        OutputRouter& router) {
    quads.begin_frame(static_cast<int>(output_width), static_cast<int>(output_height));
    quads.clear(style_.background);

    const PixelRect frame = layout_frame(layout, output_width, output_height);
    if (layout.bezel) {
        draw_ds_shell(quads, frame);
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
            quads.draw_screen(rect, *texture, style_.screen_tint, placed.rotation);
        } else {
            // No frame yet (core still booting): keep the layout's shape visible
            // rather than flashing the background colour.
            quads.draw_solid(rect, style_.placeholder);
        }
    }
}

}  // namespace ds
