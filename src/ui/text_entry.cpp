#include "ui/text_entry.hpp"

#include <cmath>

#include "ui/ui_theme.hpp"

namespace ds {

namespace {

const std::vector<PopupButton>& buttons() {
    static const std::vector<PopupButton> both = {{"Cancel", ""}, {"OK", ""}};
    return both;
}

bool continuation(unsigned char byte) { return (byte & 0xC0u) == 0x80u; }

}  // namespace

std::size_t utf8_length(const std::string& text) {
    std::size_t count = 0;
    for (const char c : text) {
        if (!continuation(static_cast<unsigned char>(c))) ++count;
    }
    return count;
}

std::string utf8_without_last(const std::string& text) {
    if (text.empty()) return text;
    std::size_t end = text.size() - 1;
    while (end > 0 && continuation(static_cast<unsigned char>(text[end]))) --end;
    return text.substr(0, end);
}

void TextEntryPopup::open(std::string title, std::string initial, std::size_t max_characters) {
    title_ = std::move(title);
    text_ = std::move(initial);
    max_characters_ = max_characters;
    while (max_characters_ > 0 && utf8_length(text_) > max_characters_) text_ = utf8_without_last(text_);
    // On OK, so Enter or A takes what is typed.
    cursor_.reset(static_cast<int>(buttons().size()), 1);
    open_ = true;
}

void TextEntryPopup::set_text(std::string text) {
    text_ = std::move(text);
    while (max_characters_ > 0 && utf8_length(text_) > max_characters_) text_ = utf8_without_last(text_);
}

PopupText TextEntryPopup::words() const {
    PopupText words;
    words.message = title_;
    return words;
}

float TextEntryPopup::field_height(float output_height) const {
    return std::round(TextRenderer::line_height(popup_scale(output_height)) * 2.4f);
}

PopupGeometry TextEntryPopup::geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    const float line = TextRenderer::line_height(popup_scale(output_height));
    const float body = popup_text_height(words(), width, output_height) + line * 0.8f +
                       field_height(output_height);
    return popup_geometry(output_width, output_height, body, buttons());
}

TextEntryResult TextEntryPopup::update(const std::vector<UiAction>& actions,
                                       const std::vector<PointerEvent>& pointers,
                                       const std::string& typed, int backspaces,
                                       float output_width, float output_height) {
    TextEntryResult result;
    if (!open_) return result;
    for (int erased = 0; erased < backspaces; ++erased) text_ = utf8_without_last(text_);
    // A character at a time, so the limit cuts whole ones; and nothing that
    // is not a printable character -- a newline pasted in, say.
    for (std::size_t at = 0; at < typed.size();) {
        std::size_t next = at + 1;
        while (next < typed.size() && continuation(static_cast<unsigned char>(typed[next]))) ++next;
        const std::string character = typed.substr(at, next - at);
        at = next;
        if (character.size() == 1 && static_cast<unsigned char>(character[0]) < 0x20) continue;
        if (max_characters_ > 0 && utf8_length(text_) >= max_characters_) break;
        text_ += character;
    }
    const int answer = cursor_.update(actions, pointers, geometry(output_width, output_height));
    if (answer == 1) {
        result.done = true;
    } else if (answer == 0 || answer == PopupCursor::kCancelled) {
        result.cancelled = true;
    }
    for (const UiAction action : actions) {
        if (action == UiAction::ToggleMenu) result.cancelled = true;
    }
    if (result.done || result.cancelled) open_ = false;
    return result;
}

void TextEntryPopup::render(QuadRenderer& quads, TextRenderer& text, float output_width,
                            float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const PopupGeometry shape = geometry(output_width, output_height);
    draw_popup(quads, text, output_width, output_height, shape, buttons(), cursor_.focus(),
               cursor_.shown());
    const float bottom = draw_popup_text(quads, text, words(), shape.body.x, shape.body.y,
                                         shape.body.w, output_height);
    const float scale = popup_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const PixelRect field{shape.body.x, std::round(bottom + line * 0.8f), shape.body.w,
                          field_height(output_height)};
    quads.draw_rounded_ring(field, std::round(line * 0.4f), field.w + field.h,
                            Color{0.0f, 0.0f, 0.0f, 0.35f}, Color{0.0f, 0.0f, 0.0f, 0.35f});
    quads.draw_rounded_ring(field, std::round(line * 0.4f), std::max(2.0f, std::round(scale)),
                            theme.accent, theme.accent);
    const float pad = std::round(line * 0.7f);
    const float text_y = std::round(field.y + (field.h - line) * 0.5f);
    const std::string shown = TextRenderer::elide_left(text_, field.w - pad * 3.0f, scale);
    text.draw(quads, shown, field.x + pad, text_y, scale, theme.text);
    // Where the next character goes.
    const float caret_x = field.x + pad + TextRenderer::measure_width(shown, scale) + 2.0f;
    quads.draw_solid(PixelRect{caret_x, text_y, std::max(2.0f, std::round(scale * 0.6f)), line},
                     theme.accent);
    if (max_characters_ > 0) {
        const std::string count =
            std::to_string(utf8_length(text_)) + "/" + std::to_string(max_characters_);
        text.draw(quads, count, field.x + field.w - pad, text_y, scale * 0.85f, theme.text_dim,
                  TextAlign::Right);
    }
}

}  // namespace ds
