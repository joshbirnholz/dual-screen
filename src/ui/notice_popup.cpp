#include "ui/notice_popup.hpp"

#include <cmath>

#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

NoticePopupGeometry notice_popup_geometry(float output_width, float output_height) {
    // The quick menu's notice, measured the same way -- see
    // LayoutPicker::compute_confirm_geometry().
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const float dialog_width = std::round(output_width * 0.58f);
    const float dialog_height = std::round(line * 7.0f);
    NoticePopupGeometry geometry;
    geometry.dialog = PixelRect{std::round((output_width - dialog_width) * 0.5f),
                                std::round((output_height - dialog_height) * 0.5f), dialog_width,
                                dialog_height};
    const float row_height = line * 2.4f;
    const float row_y = geometry.dialog.y + geometry.dialog.h - row_height - line * 0.4f;
    geometry.accept = PixelRect{geometry.dialog.x, row_y, geometry.dialog.w, row_height};
    return geometry;
}

bool notice_popup_answered(const std::vector<UiAction>& actions,
                           const std::vector<PointerEvent>& pointers,
                           float output_width,
                           float output_height) {
    for (const UiAction action : actions) {
        if (action == UiAction::Confirm || action == UiAction::Cancel) return true;
    }
    const NoticePopupGeometry geometry = notice_popup_geometry(output_width, output_height);
    for (const PointerEvent& pointer : pointers) {
        if (tapped(pointer, geometry.accept)) return true;
    }
    return false;
}

void render_notice_popup(QuadRenderer& quads,
                         TextRenderer& text,
                         const ButtonGlyphSheet& glyphs,
                         float output_width,
                         float output_height,
                         const std::string& message) {
    const UiTheme& theme = ui_theme();
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const NoticePopupGeometry geometry = notice_popup_geometry(output_width, output_height);
    // The game goes back a step, so there is one thing on screen to answer.
    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                     Color{0.0f, 0.0f, 0.0f, 0.5f});
    quads.draw_solid(geometry.dialog, theme.text);
    text.draw_wrapped(quads, message, geometry.dialog.x + geometry.dialog.w * 0.5f,
                      geometry.dialog.y + line * 1.2f, geometry.dialog.w - line * 2.0f,
                      scale * 1.15f, theme.background, TextAlign::Center);
    // Its one answer, which keeps the default role: it is the only thing on
    // screen to press, whatever is in the player's hands.
    draw_control_guide(quads, text, glyphs, {{ButtonGlyph::A, "OK"}},
                       geometry.dialog.x + geometry.dialog.w * 0.5f, geometry.accept.y,
                       scale * 1.05f, theme.background);
}

}  // namespace ds
