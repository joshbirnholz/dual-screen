#include "ui/choice_popup.hpp"

#include <algorithm>
#include <cmath>

#include "ui/list_touch.hpp"
#include "ui/popup.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

namespace {

// As many as are shown before the list scrolls.
constexpr int kMostShown = 6;

}  // namespace

void ChoicePopup::open(std::vector<std::string> choices, int current, const PixelRect& row) {
    choices_ = std::move(choices);
    current_ = current >= 0 && current < static_cast<int>(choices_.size()) ? current : -1;
    focus_ = std::max(0, current_);
    first_ = 0;
    row_ = row;
    open_ = !choices_.empty();
    keep_focus_in_view(kMostShown);
}

void ChoicePopup::set_focus(int focus) {
    if (focus < 0 || focus >= static_cast<int>(choices_.size())) return;
    focus_ = focus;
    keep_focus_in_view(kMostShown);
}

void ChoicePopup::keep_focus_in_view(int visible) {
    const int count = static_cast<int>(choices_.size());
    visible = std::max(1, std::min(visible, count));
    if (focus_ < first_) first_ = focus_;
    if (focus_ >= first_ + visible) first_ = focus_ - visible + 1;
    first_ = std::clamp(first_, 0, std::max(0, count - visible));
}

ChoicePopup::Geometry ChoicePopup::geometry(float output_width, float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float margin = std::round(line);
    const float pad_x = std::round(line * 0.9f);
    const float pad_y = std::round(line * 0.6f);
    const float circle = std::round(line * 1.0f);
    const float gap = std::round(line * 0.7f);
    geometry.row_height = std::round(line * 2.4f);

    const int count = static_cast<int>(choices_.size());
    const int fitting =
        std::max(1, static_cast<int>((output_height - margin * 2.0f - pad_y * 2.0f) / geometry.row_height));
    geometry.visible = std::max(1, std::min({count, kMostShown, fitting}));
    const bool scrolls = count > geometry.visible;

    float widest = 0.0f;
    for (const std::string& choice : choices_) {
        widest = std::max(widest, TextRenderer::measure_width(choice, scale));
    }
    const float bar = scrolls ? scroll_bar_width(scale) + line * 0.5f : 0.0f;
    const float width = std::clamp(pad_x * 2.0f + circle + gap + widest + bar + line * 0.6f,
                                   line * 10.0f, std::max(line * 10.0f, output_width - margin * 2.0f));
    const float height = geometry.row_height * static_cast<float>(geometry.visible) + pad_y * 2.0f;

    // Beside its row, at the row's right-hand end where its value is, and
    // with the choice in effect level with the row -- as far as the screen
    // allows.
    float x = row_.x + row_.w - width;
    const int shown_at = std::max(0, current_ - first_);
    float y = row_.y + row_.h * 0.5f - pad_y - geometry.row_height * (static_cast<float>(shown_at) + 0.5f);
    x = std::clamp(x, margin, std::max(margin, output_width - margin - width));
    y = std::clamp(y, margin, std::max(margin, output_height - margin - height));
    geometry.box = PixelRect{std::round(x), std::round(y), std::round(width), std::round(height)};
    geometry.list = PixelRect{geometry.box.x, geometry.box.y + pad_y, geometry.box.w,
                              geometry.row_height * static_cast<float>(geometry.visible)};
    const float inset = std::round(line * 0.35f);
    for (int index = 0; index < count; ++index) {
        geometry.rows.push_back(PixelRect{
            geometry.box.x + inset,
            geometry.list.y + geometry.row_height * static_cast<float>(index - first_),
            geometry.box.w - inset * 2.0f - bar, geometry.row_height});
    }
    return geometry;
}

int ChoicePopup::choice_at(float x, float y, float output_width, float output_height) const {
    if (!open_) return -1;
    const Geometry shape = geometry(output_width, output_height);
    if (!shape.list.contains(x, y)) return -1;
    for (std::size_t index = 0; index < shape.rows.size(); ++index) {
        if (shape.rows[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
}

ChoicePopupResult ChoicePopup::update(const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers, float output_width,
                                      float output_height) {
    ChoicePopupResult result;
    if (!open_) return result;
    const int count = static_cast<int>(choices_.size());
    const int visible = geometry(output_width, output_height).visible;
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(count - 1, focus_ + 1); break;
            case UiAction::PageUp: focus_ = std::max(0, focus_ - visible); break;
            case UiAction::PageDown: focus_ = std::min(count - 1, focus_ + visible); break;
            case UiAction::Confirm: result.chosen = focus_; break;
            case UiAction::Cancel: result.closed = true; break;
            case UiAction::ToggleMenu:
                result.closed = true;
                result.close_everything = true;
                break;
            default: break;
        }
        if (result.chosen >= 0 || result.closed) break;
        keep_focus_in_view(visible);
    }
    if (result.chosen < 0 && !result.closed) {
        const Geometry shape = geometry(output_width, output_height);
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released) continue;
            if (!shape.box.contains(pointer.press_x, pointer.press_y) &&
                !shape.box.contains(pointer.x, pointer.y)) {
                // Away from it altogether: put away, as a Switch's are.
                result.closed = true;
                break;
            }
            for (std::size_t index = 0; index < shape.rows.size(); ++index) {
                if (!tapped(pointer, shape.rows[index]) ||
                    !shape.list.contains(pointer.x, pointer.y)) {
                    continue;
                }
                if (tap_chooses(focus_, static_cast<int>(index))) result.chosen = focus_;
                break;
            }
            if (result.chosen >= 0) break;
        }
        keep_focus_in_view(visible);
    }
    if (result.chosen >= 0 || result.closed) open_ = false;
    return result;
}

void ChoicePopup::render(QuadRenderer& quads, TextRenderer& text, float output_width,
                         float output_height) {
    if (!open_) return;
    const UiTheme& theme = ui_theme();
    const Geometry shape = geometry(output_width, output_height);
    const float scale = shape.scale;
    const float line = TextRenderer::line_height(scale);

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                     Color{0.0f, 0.0f, 0.0f, 0.35f});
    const float radius = popup_radius(scale);
    quads.draw_rounded_ring(shape.box, radius, shape.box.w + shape.box.h, popup_fill(), popup_fill());
    quads.draw_rounded_ring(shape.box, radius, 1.0f, popup_rule(), popup_rule());

    quads.set_clip(shape.list);
    const float circle = std::round(line * 1.0f);
    const float pad_x = std::round(line * 0.9f) - std::round(line * 0.35f);
    const float gap = std::round(line * 0.7f);
    const float stroke = std::max(2.0f, std::round(scale * 0.8f));
    for (std::size_t index = 0; index < shape.rows.size(); ++index) {
        const PixelRect& row = shape.rows[index];
        if (row.y + row.h < shape.list.y || row.y > shape.list.y + shape.list.h) continue;
        const bool focused = static_cast<int>(index) == focus_;
        const bool chosen = static_cast<int>(index) == current_;
        if (focused) draw_menu_cursor(quads, row, std::round(std::min(row.h * 0.3f, radius)), scale);
        const PixelRect dot{row.x + pad_x, std::round(row.y + (row.h - circle) * 0.5f), circle, circle};
        quads.draw_rounded_ring(dot, circle * 0.5f, stroke, chosen ? theme.accent : theme.text_dim,
                                chosen ? theme.accent : theme.text_dim);
        if (chosen) {
            const float inner = std::round(circle * 0.5f);
            quads.draw_capsule(PixelRect{dot.x + (circle - inner) * 0.5f, dot.y + (circle - inner) * 0.5f,
                                         inner, inner},
                               theme.accent);
        }
        const float text_x = dot.x + circle + gap;
        text.draw(quads,
                  TextRenderer::elide_right(choices_[index], row.x + row.w - text_x - line * 0.4f, scale),
                  text_x, std::round(row.y + (row.h - line) * 0.5f), scale, theme.text);
    }
    quads.clear_clip();

    const int count = static_cast<int>(choices_.size());
    if (count > shape.visible) {
        const float span = static_cast<float>(count);
        const PixelRect bar_area{shape.box.x, shape.list.y, shape.box.w - std::round(line * 0.5f),
                                 shape.list.h};
        draw_scroll_bar(quads, bar_area, static_cast<float>(first_) / span,
                        static_cast<float>(shape.visible) / span, line, scale);
    }
}

}  // namespace ds
