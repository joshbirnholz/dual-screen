#include "ui/controllers_screen.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

namespace {

// Past a third of the way out: far enough that a stick at rest, or one that
// has drifted, does not keep its row lit.
constexpr int kStickInUse = 32767 / 3;

// The two rows after the controllers.
constexpr int kExtraRows = 2;

}  // namespace

bool pad_in_use(const DsInputState& held) {
    const auto pushed = [](int16_t value) { return std::abs(static_cast<int>(value)) > kStickInUse; };
    return held.buttons != 0 || pushed(held.circle_x) || pushed(held.circle_y) ||
           pushed(held.c_stick_x) || pushed(held.c_stick_y);
}

void ControllersScreen::open() {
    open_ = true;
    focus_ = 0;
    scroll_ = 0.0f;
}

ControllersScreen::Geometry ControllersScreen::compute_geometry(float output_width,
                                                                float output_height,
                                                                int row_count) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(geometry.scale);
    const float margin = std::round(output_width * 0.07f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.08f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.16f)};
    geometry.view_top = geometry.panel.y + line * 4.6f;
    geometry.view_bottom = geometry.panel.y + geometry.panel.h - line * 2.2f;
    const float row_height = line * 1.9f;
    const float step = row_height + line * 0.3f;
    for (int index = 0; index < row_count; ++index) {
        geometry.rows.push_back(
            PixelRect{geometry.panel.x, geometry.view_top - scroll_ + index * step,
                      geometry.panel.w, row_height});
    }
    geometry.content_height = row_count * step;
    return geometry;
}

bool ControllersScreen::adjust(int direction, const std::vector<KnownPad>& pads,
                               bool gamepad_here, std::map<std::string, std::string>& choices,
                               ControllersScreenResult& result) {
    const int pad_count = static_cast<int>(pads.size());
    if (focus_ < pad_count) {
        const std::vector<PadSlot> slots = assign_pads(pads, choices, gamepad_here);
        const PadSlot now = slots[static_cast<std::size_t>(focus_)];
        const PadSlot next = direction < 0 ? previous_pad_slot(now) : next_pad_slot(now);
        choices[pad_key(pads[static_cast<std::size_t>(focus_)])] = pad_slot_key(next);
        result.choices_changed = true;
        return true;
    }
    if (focus_ == pad_count) {
        // Back to what the order says.
        if (!choices.empty()) {
            choices.clear();
            result.choices_changed = true;
        }
        return true;
    }
    result.close_requested = true;
    return true;
}

ControllersScreenResult ControllersScreen::update(const std::vector<UiAction>& actions,
                                                  const std::vector<PointerEvent>& pointers,
                                                  float output_width,
                                                  float output_height,
                                                  const std::vector<KnownPad>& pads,
                                                  bool gamepad_here,
                                                  std::map<std::string, std::string>& choices) {
    ControllersScreenResult result;
    if (!open_) {
        return result;
    }
    const int row_count = static_cast<int>(pads.size()) + kExtraRows;
    // A pad unplugged under the cursor leaves it past the end.
    focus_ = std::min(focus_, row_count - 1);

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: focus_ = (focus_ + row_count - 1) % row_count; break;
            case UiAction::Down: focus_ = (focus_ + 1) % row_count; break;
            case UiAction::Left: adjust(-1, pads, gamepad_here, choices, result); break;
            case UiAction::Right:
            case UiAction::Confirm: adjust(1, pads, gamepad_here, choices, result); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            // Escape also raises Cancel, which is what it means here.
            case UiAction::OpenMenu:
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::SwapLayout:
            case UiAction::PageUp:
            case UiAction::PageDown: break;
        }
    }

    // The cursor kept in view, then anything tapped.
    Geometry geometry = compute_geometry(output_width, output_height, row_count);
    const float view = geometry.view_bottom - geometry.view_top;
    const float max_scroll = std::max(0.0f, geometry.content_height - view);
    const PixelRect& focused = geometry.rows[static_cast<std::size_t>(focus_)];
    if (focused.y < geometry.view_top) {
        scroll_ -= geometry.view_top - focused.y;
    } else if (focused.y + focused.h > geometry.view_bottom) {
        scroll_ += focused.y + focused.h - geometry.view_bottom;
    }
    scroll_ = std::max(0.0f, std::min(scroll_, max_scroll));
    geometry = compute_geometry(output_width, output_height, row_count);
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
            if (tapped(pointer, geometry.rows[index])) {
                focus_ = static_cast<int>(index);
                adjust(1, pads, gamepad_here, choices, result);
            }
        }
    }

    if (result.close_requested) {
        open_ = false;
    }
    return result;
}

void ControllersScreen::render(QuadRenderer& quads,
                               TextRenderer& text,
                               const ButtonGlyphSheet& glyphs,
                               float output_width,
                               float output_height,
                               const std::vector<KnownPad>& pads,
                               bool gamepad_here,
                               const std::map<std::string, std::string>& choices,
                               bool wii_u) {
    if (!open_) {
        return;
    }
    const UiTheme& theme = ui_theme();
    const int row_count = static_cast<int>(pads.size()) + kExtraRows;
    const Geometry geometry = compute_geometry(output_width, output_height, row_count);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const std::vector<PadSlot> slots = assign_pads(pads, choices, gamepad_here);

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.scrim);
    text.draw(quads, "Controllers", geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);
    text.draw(quads,
              wii_u ? "Left/Right picks what each one is. Press a button to find its row."
                    : "These only apply to Wii U games. DS and 3DS games take every controller.",
              geometry.panel.x, geometry.panel.y + line * 1.8f, scale * 0.9f,
              wii_u ? theme.text_dim : theme.warning);
    text.draw(quads,
              "Numbered in each device's own order (Steam Input's, under Steam). "
              "Kept until this app closes.",
              geometry.panel.x, geometry.panel.y + line * 2.9f, scale * 0.8f, theme.text_dim);

    quads.set_clip(PixelRect{geometry.panel.x, geometry.view_top, geometry.panel.w,
                             geometry.view_bottom - geometry.view_top});
    const float pad = line * 0.5f;
    for (int index = 0; index < row_count; ++index) {
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = index == focus_;
        draw_menu_row(quads, row, focused, scale);
        const float baseline = row.y + (row.h - line) * 0.5f;
        const int pad_count = static_cast<int>(pads.size());
        if (index >= pad_count) {
            const std::string label = index == pad_count ? "Back to the defaults" : "Back";
            text.draw(quads, label, row.x + pad, baseline, scale, menu_row_text(focused));
            continue;
        }
        const KnownPad& known = pads[static_cast<std::size_t>(index)];
        // A light at the front of the row while the pad is being pressed.
        const float light = line * 0.45f;
        const PixelRect dot{row.x + pad, row.y + (row.h - light) * 0.5f, light, light};
        if (pad_in_use(known.held)) {
            quads.draw_capsule(dot, theme.success);
        } else {
            Color faint = focused ? theme.background : theme.text_dim;
            faint.a *= 0.35f;
            quads.draw_capsule(dot, faint);
        }
        const float label_x = dot.x + light + pad;
        const std::string label = pad_label(known);
        text.draw(quads, label, label_x, baseline, scale, menu_row_text(focused));
        const std::string where = known.on_this_device ? "  this device" : "  other device";
        text.draw(quads, where, label_x + TextRenderer::measure_width(label, scale), baseline,
                  scale * 0.8f, focused ? theme.panel : theme.text_dim);
        const PadSlot slot = slots[static_cast<std::size_t>(index)];
        const bool chosen = choices.count(pad_key(known)) != 0;
        const std::string value = pad_slot_name(slot) + (chosen ? "" : "  (default)");
        Color value_color = focused ? theme.panel : theme.text;
        if (!focused && slot.kind == PadSlot::Kind::NotUsed) value_color = theme.text_dim;
        text.draw(quads, value, row.x + row.w - pad, baseline, scale * 0.9f, value_color,
                  TextAlign::Right);
    }
    quads.clear_clip();
    if (pads.empty()) {
        text.draw(quads, "No controllers found", geometry.panel.x, geometry.view_bottom + line * 0.4f,
                  scale * 0.9f, theme.text_dim);
    }

    const float guide_height = control_guide_height(scale * 0.9f);
    draw_control_guide(quads, text, glyphs, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                       geometry.panel.x + geometry.panel.w,
                       output_height - line * 0.6f - guide_height, scale * 0.9f, theme.text,
                       TextAlign::Right);
}

}  // namespace ds
