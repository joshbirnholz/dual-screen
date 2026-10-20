#include "ui/choice_popup.hpp"

#include <algorithm>
#include <cmath>

#include "ui/fitted_text.hpp"
#include "ui/popup.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

void ChoicePopup::open(std::vector<std::string> choices, int current, const PixelRect& row) {
    choices_ = std::move(choices);
    glyph_pairs_.clear();
    headings_.clear();
    hints_.clear();
    colors_.clear();
    columns_ = 0;
    current_ = current >= 0 && current < static_cast<int>(choices_.size()) ? current : -1;
    focus_ = std::max(0, current_);
    scroll_rows_ = 0.0f;
    row_ = row;
    open_ = !choices_.empty();
    placed_ = false;
    fresh_ = true;
    drag_.stop();
    // Every row it has, as far as is known yet: how many fit is known when
    // it is first laid out -- see settle_rows().
    shown_rows_ = static_cast<int>(choices_.size());
    anchor_rows_ = static_cast<float>(std::max(0, current_));
}

void ChoicePopup::settle_rows(int visible) {
    shown_rows_ = visible;
    if (!fresh_) return;
    // Its first pass: the list brought round to the current choice at
    // however many rows fit, and the box placed by the rows above it then.
    keep_focus_in_view(visible);
    if (!grid()) anchor_rows_ = static_cast<float>(std::max(0, current_)) - scroll_rows_;
    fresh_ = false;
}

void ChoicePopup::open_colors(std::vector<Color> colors, std::vector<std::string> names, int columns,
                              int current, const PixelRect& row) {
    names.resize(colors.size());
    open(std::move(names), current, row);
    colors_ = std::move(colors);
    columns_ = std::max(1, columns);
    scroll_rows_ = 0.0f;
    anchor_rows_ = 0.0f;
}

void ChoicePopup::set_glyph_pairs(std::vector<std::pair<ButtonGlyph, ButtonGlyph>> pairs,
                                  float aspect) {
    glyph_pairs_ = std::move(pairs);
    glyph_aspect_ = aspect > 0.0f ? aspect : 1.0f;
}

void ChoicePopup::set_headings(std::vector<bool> headings) {
    headings_ = std::move(headings);
    if (!heading(focus_)) return;
    // The cursor on the first choice under it, or failing that over it.
    int focus = next_choice(focus_, 1);
    if (focus < 0) focus = next_choice(focus_, -1);
    if (focus < 0) {
        open_ = false;
        return;
    }
    focus_ = focus;
    keep_focus_in_view(shown_rows_);
}

int ChoicePopup::next_choice(int from, int step) const {
    const int count = static_cast<int>(choices_.size());
    for (int index = from + step; index >= 0 && index < count; index += step) {
        if (!heading(index)) return index;
    }
    return -1;
}

void ChoicePopup::set_focus(int focus) {
    if (focus < 0 || focus >= static_cast<int>(choices_.size()) || heading(focus)) return;
    // Told again each pass from the other device: the list follows the
    // cursor when it moves, and is otherwise where a finger or the wheel
    // left it -- or it could never be scrolled away from the cursor.
    if (focus == focus_) return;
    focus_ = focus;
    keep_focus_in_view(shown_rows_);
}

float ChoicePopup::max_scroll_rows(int visible) const {
    return std::max(0.0f, static_cast<float>(static_cast<int>(choices_.size()) - visible));
}

void ChoicePopup::keep_focus_in_view(int visible) {
    if (grid()) return;
    const int count = static_cast<int>(choices_.size());
    visible = std::max(1, std::min(visible, count));
    const float at = static_cast<float>(focus_);
    // A heading just over the cursor comes into view with it, so what the
    // choice is under is not left out of sight.
    const float top = visible > 1 && heading(focus_ - 1) ? at - 1.0f : at;
    if (top < scroll_rows_) scroll_rows_ = top;
    if (at + 1.0f > scroll_rows_ + static_cast<float>(visible)) scroll_rows_ = at + 1.0f - static_cast<float>(visible);
    scroll_rows_ = std::clamp(scroll_rows_, 0.0f, max_scroll_rows(visible));
}

ChoicePopup::Geometry ChoicePopup::geometry(float output_width, float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float margin = std::round(line);
    const float pad_x = std::round(line * 0.9f);
    const float pad_y = std::round(line * 0.6f);
    const int count = static_cast<int>(choices_.size());

    if (grid()) {
        // Circles in a grid, as a DS's own settings lay its colors out, and
        // the one under the cursor named beneath them.
        const float cell = std::round(line * 3.0f);
        const int lines = (count + columns_ - 1) / columns_;
        geometry.row_height = cell;
        geometry.visible = lines;
        // The colors are the whole of it: nothing is written under them.
        const float name_height = 0.0f;
        const float width = std::max(pad_x * 2.0f + cell * static_cast<float>(columns_), line * 10.0f);
        const float height = pad_y * 2.0f + cell * static_cast<float>(lines) + name_height;
        // Beside its row, at the row's right-hand end where its value is, and
        // level with it -- as far as the screen allows.
        float x = row_.x + row_.w - width;
        float y = row_.y + row_.h * 0.5f - height * 0.5f;
        x = std::clamp(x, margin, std::max(margin, output_width - margin - width));
        y = std::clamp(y, margin, std::max(margin, output_height - margin - height));
        geometry.box = PixelRect{std::round(x), std::round(y), std::round(width), std::round(height)};
        const float grid_x = std::round(geometry.box.x + (geometry.box.w - cell * static_cast<float>(columns_)) * 0.5f);
        geometry.list = PixelRect{grid_x, geometry.box.y + pad_y, cell * static_cast<float>(columns_),
                                  cell * static_cast<float>(lines)};
        for (int index = 0; index < count; ++index) {
            geometry.rows.push_back(PixelRect{grid_x + cell * static_cast<float>(index % columns_),
                                              geometry.list.y + cell * static_cast<float>(index / columns_),
                                              cell, cell});
        }
        geometry.name_y = std::round(geometry.list.y + geometry.list.h + (name_height - line) * 0.5f);
        return geometry;
    }

    const float circle = std::round(line * 1.0f);
    const float gap = std::round(line * 0.7f);
    geometry.row_height = std::round(line * 2.8f);
    const int fitting =
        std::max(1, static_cast<int>((output_height - margin * 2.0f - pad_y * 2.0f) / geometry.row_height));
    // A long list takes a good part of the screen, but not the whole of it:
    // no taller than kMaxListShare of it, and never fewer than
    // kMinRowsShown rows where they fit -- a short box with half the screen
    // empty around it scrolled too much, and one from edge to edge looked
    // like a page of its own.
    const int share =
        static_cast<int>(output_height * kMaxListShare / geometry.row_height);
    const int tallest = std::min(fitting, std::max(kMinRowsShown, share));
    geometry.visible = std::max(1, std::min(count, tallest));
    const bool scrolls = count > geometry.visible;

    float widest = 0.0f;
    if (!glyph_pairs_.empty()) {
        const float glyph = line * 1.5f * glyph_aspect_;
        widest = glyph * 2.0f + TextRenderer::measure_width("/", scale * 0.9f) + line * 0.3f;
    } else {
        for (const std::string& choice : choices_) {
            widest = std::max(widest, TextRenderer::measure_width(choice, scale));
        }
    }
    const float bar = scrolls ? scroll_bar_width(scale) + line * 0.5f : 0.0f;
    const float width = std::clamp(pad_x * 2.0f + circle + gap + widest + bar + line * 0.6f,
                                   line * 10.0f, std::max(line * 10.0f, output_width - margin * 2.0f));
    const float height = geometry.row_height * static_cast<float>(geometry.visible) + pad_y * 2.0f;

    // Beside its row, at the row's right-hand end where its value is, and
    // with the choice in effect level with the row as it opened -- as far as
    // the screen allows. Placed by the rows in view then, not now, so that
    // moving through the list scrolls it inside a box that stays put.
    float x = row_.x + row_.w - width;
    const float shown_at = std::clamp(anchor_rows_, 0.0f, static_cast<float>(geometry.visible - 1));
    float y = row_.y + row_.h * 0.5f - pad_y - geometry.row_height * (shown_at + 0.5f);
    x = std::clamp(x, margin, std::max(margin, output_width - margin - width));
    y = std::clamp(y, margin, std::max(margin, output_height - margin - height));
    geometry.box = PixelRect{std::round(x), std::round(y), std::round(width), std::round(height)};
    geometry.list = PixelRect{geometry.box.x, geometry.box.y + pad_y, geometry.box.w,
                              geometry.row_height * static_cast<float>(geometry.visible)};
    const float inset = std::round(line * 0.35f);
    const float scroll = std::clamp(scroll_rows_, 0.0f, max_scroll_rows(geometry.visible));
    for (int index = 0; index < count; ++index) {
        geometry.rows.push_back(PixelRect{
            geometry.box.x + inset,
            std::round(geometry.list.y + geometry.row_height * (static_cast<float>(index) - scroll)),
            geometry.box.w - inset * 2.0f - bar, geometry.row_height});
    }
    return geometry;
}

int ChoicePopup::choice_at(float x, float y, float output_width, float output_height) const {
    if (!open_) return -1;
    const Geometry shape = geometry(output_width, output_height);
    if (!shape.list.contains(x, y)) return -1;
    for (std::size_t index = 0; index < shape.rows.size(); ++index) {
        if (shape.rows[index].contains(x, y)) return heading(static_cast<int>(index)) ? -1 : static_cast<int>(index);
    }
    return -1;
}

bool ChoicePopup::contains(float x, float y, float output_width, float output_height) const {
    return open_ && geometry(output_width, output_height).box.contains(x, y);
}

bool ChoicePopup::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                         float output_height) {
    if (!open_ || grid()) return false;
    const Geometry shape = geometry(output_width, output_height);
    float pixels = scroll_rows_ * shape.row_height;
    drag_.set_wheel_row(shape.row_height);
    drag_.update(pointers, shape.list, pixels, max_scroll_rows(shape.visible) * shape.row_height);
    scroll_rows_ = shape.row_height > 0.0f ? pixels / shape.row_height : 0.0f;
    return drag_.release_was_drag();
}

ChoicePopupResult ChoicePopup::update(const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers, float output_width,
                                      float output_height) {
    ChoicePopupResult result;
    if (!open_) return result;
    const int count = static_cast<int>(choices_.size());
    const int visible = geometry(output_width, output_height).visible;
    settle_rows(visible);
    // A grid moves a line at a time up and down, and along one with left
    // and right, stopping at the edges.
    const int step = grid() ? columns_ : 1;
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (grid()) {
                    if (focus_ - step >= 0) focus_ -= step;
                } else if (const int next = next_choice(focus_, -1); next >= 0) {
                    focus_ = next;
                }
                break;
            case UiAction::Down:
                if (grid()) {
                    if (focus_ + step < count) focus_ += step;
                } else if (const int next = next_choice(focus_, 1); next >= 0) {
                    focus_ = next;
                }
                break;
            case UiAction::Left:
                if (grid() && focus_ % columns_ > 0) --focus_;
                break;
            case UiAction::Right:
                if (grid() && focus_ % columns_ < columns_ - 1 && focus_ + 1 < count) ++focus_;
                break;
            // L and R jump nowhere: a shoulder is not a way through a list.
            case UiAction::PageUp:
            case UiAction::PageDown: break;
            case UiAction::Confirm: result.chosen = focus_; break;
            case UiAction::Cancel: result.closed = true; break;
            case UiAction::ToggleMenu:
                result.closed = true;
                result.close_everything = true;
                break;
            default: break;
        }
        if (result.chosen >= 0 || result.closed) break;
        // The cursor brings the list with it when it moves -- and only then,
        // so a list moved by a finger or the wheel stays where it was left.
        drag_.stop();
        keep_focus_in_view(visible);
    }
    if (result.chosen < 0 && !result.closed) {
        scroll(pointers, output_width, output_height);
        const Geometry shape = geometry(output_width, output_height);
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released) continue;
            if (!shape.box.contains(pointer.press_x, pointer.press_y) &&
                !shape.box.contains(pointer.x, pointer.y)) {
                // Away from it altogether: put away, as a Switch's are.
                result.closed = true;
                break;
            }
            if (drag_.release_was_drag()) continue;
            for (std::size_t index = 0; index < shape.rows.size(); ++index) {
                if (!tapped(pointer, shape.rows[index]) ||
                    !shape.list.contains(pointer.x, pointer.y) || heading(static_cast<int>(index))) {
                    continue;
                }
                // Chosen at once: the list is already the question, and a
                // tap on one of its answers is the answer.
                focus_ = static_cast<int>(index);
                result.chosen = focus_;
                break;
            }
            if (result.chosen >= 0) break;
        }
        drag_.settle();
    }
    if (result.chosen >= 0 || result.closed) open_ = false;
    return result;
}

void ChoicePopup::render(QuadRenderer& quads, TextRenderer& text, float output_width,
                         float output_height, const ButtonGlyphSheet* glyphs) {
    if (!open_) return;
    settle_rows(geometry(output_width, output_height).visible);
    placed_ = true;
    const UiTheme& theme = ui_theme();
    const Geometry shape = geometry(output_width, output_height);
    const float scale = shape.scale;
    const float line = TextRenderer::line_height(scale);

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                     Color{0.0f, 0.0f, 0.0f, 0.35f});
    const float radius = popup_radius(scale);
    quads.draw_rounded_ring(shape.box, radius, shape.box.w + shape.box.h, popup_fill(), popup_fill());
    quads.draw_rounded_ring(shape.box, radius, 1.0f, popup_rule(), popup_rule());

    if (grid()) {
        const float circle = std::round(shape.row_height * 0.68f);
        for (std::size_t index = 0; index < shape.rows.size(); ++index) {
            const PixelRect& cell = shape.rows[index];
            const PixelRect dot{std::round(cell.x + (cell.w - circle) * 0.5f),
                                std::round(cell.y + (cell.h - circle) * 0.5f), circle, circle};
            if (static_cast<int>(index) == focus_) {
                const float out = std::round((cell.w - circle) * 0.3f);
                const PixelRect ring{dot.x - out, dot.y - out, dot.w + out * 2.0f, dot.h + out * 2.0f};
                draw_menu_cursor(quads, ring, ring.w * 0.5f, scale);
            }
            const Color& color = colors_[index];
            quads.draw_capsule(dot, Color{color.r, color.g, color.b, 1.0f});
            if (static_cast<int>(index) == current_) {
                // The one in effect: a white dot in it, edged so it shows on
                // the palest of them.
                const float inner = std::round(circle * 0.34f);
                const PixelRect mark{dot.x + std::round((circle - inner) * 0.5f),
                                     dot.y + std::round((circle - inner) * 0.5f), inner, inner};
                quads.draw_capsule(mark, Color{1.0f, 1.0f, 1.0f, 1.0f});
                quads.draw_rounded_ring(mark, inner * 0.5f, std::max(1.0f, std::round(scale * 0.5f)),
                                        Color{0.0f, 0.0f, 0.0f, 0.4f}, Color{0.0f, 0.0f, 0.0f, 0.4f});
            }
        }
        (void)theme;
        return;
    }

    quads.set_clip(shape.list);
    const auto now = std::chrono::steady_clock::now();
    if (focus_ != drawn_focus_) {
        drawn_focus_ = focus_;
        focus_since_ = now;
    }
    const float focus_seconds = std::chrono::duration<float>(now - focus_since_).count();
    const float circle = std::round(line * 1.0f);
    const float pad_x = std::round(line * 0.9f) - std::round(line * 0.35f);
    const float gap = std::round(line * 0.7f);
    const float stroke = std::max(2.0f, std::round(scale * 0.8f));
    for (std::size_t index = 0; index < shape.rows.size(); ++index) {
        const PixelRect& row = shape.rows[index];
        if (row.y + row.h < shape.list.y || row.y > shape.list.y + shape.list.h) continue;
        if (heading(static_cast<int>(index))) {
            // A heading: dim, from where the circles start, and nothing else.
            draw_fitted_text(quads, text, choices_[index], row.x + pad_x,
                             std::round(row.y + (row.h - line) * 0.5f), row.w - pad_x - line * 0.4f, scale,
                             theme.text_dim, TextAlign::Left, false, 0.0f);
            continue;
        }
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
        if (glyphs != nullptr && index < glyph_pairs_.size()) {
            // Two buttons, as the row they belong to draws them: "(A) / (B)".
            const float height = line * 1.5f;
            const float glyph_width = glyphs->width_for(height);
            const float slash_width = TextRenderer::measure_width("/", scale * 0.9f);
            const float slash_gap = line * 0.15f;
            const float glyph_y = row.y + row.h * 0.5f - height * glyphs->button_centre();
            float pen_x = text_x;
            glyphs->draw(quads, glyph_pairs_[index].first, pen_x, glyph_y, height, theme.text);
            pen_x += glyph_width + slash_gap;
            text.draw(quads, "/", pen_x, std::round(row.y + (row.h - TextRenderer::line_height(scale * 0.9f)) * 0.5f),
                      scale * 0.9f, theme.text);
            pen_x += slash_width + slash_gap;
            glyphs->draw(quads, glyph_pairs_[index].second, pen_x, glyph_y, height, theme.text);
            continue;
        }
        float text_right = row.x + row.w - line * 0.4f;
        if (index < hints_.size() && !hints_[index].empty()) {
            const float hint_scale = scale * 0.8f;
            const float hint_width = TextRenderer::measure_width(hints_[index], hint_scale);
            const float hint_x = std::round(text_right - line * 0.4f - hint_width);
            text.draw(quads, hints_[index], hint_x,
                      std::round(row.y + (row.h - TextRenderer::line_height(hint_scale)) * 0.5f), hint_scale,
                      theme.text_dim);
            text_right = hint_x - gap;
        }
        draw_fitted_text(quads, text, choices_[index], text_x, std::round(row.y + (row.h - line) * 0.5f),
                         text_right - text_x, scale, theme.text, TextAlign::Left, focused, focus_seconds);
    }
    quads.clear_clip();

    const int count = static_cast<int>(choices_.size());
    if (count > shape.visible) {
        const float span = static_cast<float>(count);
        const PixelRect bar_area{shape.box.x, shape.list.y, shape.box.w - std::round(line * 0.5f),
                                 shape.list.h};
        draw_scroll_bar(quads, bar_area,
                        std::clamp(scroll_rows_, 0.0f, max_scroll_rows(shape.visible)) / span,
                        static_cast<float>(shape.visible) / span, line, scale);
    }
}

}  // namespace ds
