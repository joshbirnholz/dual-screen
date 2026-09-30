#include "ui/notice_popup.hpp"

#include "ui/popup.hpp"

namespace ds {
namespace {

const std::vector<PopupButton>& ok_button() {
    static const std::vector<PopupButton> buttons{{"OK", ""}};
    return buttons;
}

PopupGeometry geometry_for(float output_width, float output_height, const std::string& message) {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height,
                          popup_text_height(PopupText{message, {}, 0}, width, output_height),
                          ok_button());
}

}  // namespace

NoticePopupGeometry notice_popup_geometry(float output_width, float output_height,
                                          const std::string& message) {
    const PopupGeometry popup = geometry_for(output_width, output_height, message);
    return NoticePopupGeometry{popup.box, popup.buttons.front()};
}

bool notice_popup_answered(const std::vector<UiAction>& actions,
                           const std::vector<PointerEvent>& pointers,
                           float output_width,
                           float output_height,
                           const std::string& message) {
    for (const UiAction action : actions) {
        if (action == UiAction::Confirm || action == UiAction::Cancel) return true;
    }
    const NoticePopupGeometry geometry = notice_popup_geometry(output_width, output_height, message);
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
    (void)glyphs;
    const PopupGeometry geometry = geometry_for(output_width, output_height, message);
    // Its one button always has the cursor: there is nothing else to press.
    draw_popup(quads, text, output_width, output_height, geometry, ok_button(), 0,
               prompt_style() != PromptStyle::Touch);
    draw_popup_text(quads, text, PopupText{message, {}, 0}, geometry.body.x, geometry.body.y,
                    geometry.body.w, output_height);
}

}  // namespace ds
