#include "ui/wii_u_options_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/page_frame.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

void WiiUOptionsScreen::open() {
    open_ = true;
    waiting_ = true;
    restart_pending_ = false;
    options_.clear();
    focus_ = 0;
}

void WiiUOptionsScreen::set_options(std::vector<ExternalOption> options) {
    const std::string id =
        focus_ >= 0 && focus_ < static_cast<int>(options_.size()) ? options_[static_cast<std::size_t>(focus_)].id
                                                                    : std::string();
    waiting_ = false;
    options_ = std::move(options);
    for (int index = 0; index < static_cast<int>(options_.size()); ++index) {
        if (options_[static_cast<std::size_t>(index)].id == id) {
            focus_ = index;
            return;
        }
    }
    focus_ = std::max(0, std::min(focus_, static_cast<int>(options_.size()) - 1));
}

std::vector<PixelRect> WiiUOptionsScreen::rows(float output_width, float output_height) const {
    // The same frame and rows as Settings, which never runs past the bottom
    // with the few Cemu offers.
    const PageFrame frame = page_frame(output_width, output_height);
    const float row_height = menu_row_height(frame.line);
    const float gap = menu_row_gap(frame.line);
    std::vector<PixelRect> out;
    float y = frame.content_top;
    for (std::size_t index = 0; index < options_.size(); ++index) {
        out.push_back(PixelRect{frame.panel.x, y, frame.panel.w, row_height});
        y += row_height + gap;
    }
    return out;
}

void WiiUOptionsScreen::adjust(int direction, WiiUOptionsScreenResult& result) {
    if (focus_ < 0 || focus_ >= static_cast<int>(options_.size())) return;
    ExternalOption& option = options_[static_cast<std::size_t>(focus_)];
    const int count = static_cast<int>(option.choices.size());
    if (count < 2) return;
    const int at = option.active >= 0 ? option.active : 0;
    option.active = ((at + direction) % count + count) % count;
    if (option.restart_needed) restart_pending_ = true;
    result.change = std::make_pair(option.id, option.active);
}

WiiUOptionsScreenResult WiiUOptionsScreen::update(const std::vector<UiAction>& pressed,
                                                  const std::vector<PointerEvent>& touched,
                                                  float output_width, float output_height) {
    WiiUOptionsScreenResult result;
    if (!open_) return result;
    // A tap on Back at the foot of the page is Back pressed.
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    take_guide_taps(guide_hits_, actions, pointers);

    const int count = static_cast<int>(options_.size());
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::max(0, std::min(count - 1, focus_ + 1)); break;
            case UiAction::Left: adjust(-1, result); break;
            case UiAction::Right:
            case UiAction::Confirm: adjust(1, result); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            // A menu's face buttons and the app's own keys: nothing here
            // answers them, and there is nothing to page through.
            case UiAction::PageUp:
            case UiAction::PageDown:
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::OpenMenu:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::SwapLayout: break;
        }
    }
    const std::vector<PixelRect> placed = rows(output_width, output_height);
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        for (std::size_t index = 0; index < placed.size(); ++index) {
            // A first tap highlights a row; a second changes its value.
            if (tapped(pointer, placed[index]) && tap_chooses(focus_, static_cast<int>(index))) {
                adjust(1, result);
            }
        }
    }
    if (result.close_requested) open_ = false;
    return result;
}

void WiiUOptionsScreen::render(QuadRenderer& quads, TextRenderer& text,
                               const ButtonGlyphSheet& glyphs, float output_width,
                               float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const PageFrame frame = page_frame(output_width, output_height);
    const float scale = frame.scale;
    const float line = frame.line;
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame, "Options");

    if (waiting_ || options_.empty()) {
        const std::string message = waiting_ ? "Asking Cemu for its options..."
                                             : "This Cemu has no options to change from here.";
        const PixelRect content = frame.content();
        text.draw(quads, TextRenderer::elide_right(message, content.w, scale),
                  content.x + content.w * 0.5f,
                  std::round(content.y + (content.h - line) * 0.5f), scale, theme.text_dim,
                  TextAlign::Center);
    }
    const std::vector<PixelRect> placed = rows(output_width, output_height);
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const PixelRect& row = placed[index];
        const ExternalOption& option = options_[index];
        const bool focused = static_cast<int>(index) == focus_;
        draw_menu_row(quads, row, focused, scale);
        const float pad = menu_row_pad(line);
        const float value_scale = scale * 0.9f;
        const int at = option.active >= 0 ? option.active : 0;
        const std::string value = option.choices.empty()
                                      ? std::string()
                                      : option.choices[static_cast<std::size_t>(at)];
        const float value_width =
            std::min(TextRenderer::measure_width(value, value_scale), row.w * 0.5f);
        text.draw(quads,
                  TextRenderer::elide_right(option.label, row.w - pad * 3.0f - value_width, scale),
                  row.x + pad, page_row_text_y(row, scale), scale, menu_row_text(focused));
        text.draw(quads, TextRenderer::elide_right(value, row.w * 0.5f, value_scale),
                  row.x + row.w - pad, page_row_text_y(row, value_scale), value_scale,
                  focused ? theme.panel : theme.text_dim, TextAlign::Right);
    }

    if (restart_pending_) {
        draw_page_status(quads, text, glyphs, frame,
                         "Some changes will take effect when the game is reset.", theme.text_dim);
    }
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                    &guide_hits_);
}

void WiiUOptionsScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    for (const ExternalOption& option : options_) {
        mix_sound_state(view.rest, static_cast<uint64_t>(option.active + 1));
    }
}

}  // namespace ds
