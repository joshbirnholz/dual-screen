#include "ui/controllers_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

namespace {

// The row after the line: done.
constexpr int kExtraRows = 1;

// Where Not used begins, which is where the gap goes; -1 when nothing is
// unused.
int first_unused_row(const std::vector<LineupRow>& rows) {
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (rows[index].slot.kind == PadSlot::Kind::NotUsed) return static_cast<int>(index);
    }
    return -1;
}

}  // namespace

void ControllersScreen::open() {
    open_ = true;
    holding_ = false;
    focus_ = 0;
    scroll_ = 0.0f;
}

ControllersScreen::Geometry ControllersScreen::compute_geometry(float output_width,
                                                                float output_height,
                                                                int row_count,
                                                                int gap_before) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(geometry.scale);
    const float margin = std::round(output_width * 0.07f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.08f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.16f)};
    geometry.view_top = geometry.panel.y + line * 2.6f;
    geometry.view_bottom = geometry.panel.y + geometry.panel.h - line * 2.2f;
    const float row_height = menu_row_height(line);
    const float step = row_height + menu_row_gap(line);
    const float gap = line * 1.2f;
    geometry.content_height = row_count * step + (gap_before >= 0 ? gap : 0.0f);
    // Short of the scroll bar when there is one, which runs beside the rows.
    const bool scrolls = geometry.content_height > geometry.view_bottom - geometry.view_top + 0.5f;
    const float row_width =
        geometry.panel.w - (scrolls ? scroll_bar_width(geometry.scale) + line * 0.6f : 0.0f);
    for (int index = 0; index < row_count; ++index) {
        const float before = (gap_before >= 0 && index >= gap_before) ? gap : 0.0f;
        geometry.rows.push_back(
            PixelRect{geometry.panel.x, geometry.view_top - scroll_ + index * step + before,
                      row_width, row_height});
    }
    return geometry;
}

void ControllersScreen::scroll_focus_into_view(float output_width, float output_height,
                                               int row_count, int gap_before) {
    const Geometry geometry = compute_geometry(output_width, output_height, row_count, gap_before);
    const float view = geometry.view_bottom - geometry.view_top;
    const float max_scroll = std::max(0.0f, geometry.content_height - view);
    const PixelRect& focused = geometry.rows[static_cast<std::size_t>(focus_)];
    if (focused.y < geometry.view_top) {
        scroll_ -= geometry.view_top - focused.y;
    } else if (focused.y + focused.h > geometry.view_bottom) {
        scroll_ += focused.y + focused.h - geometry.view_bottom;
    }
    scroll_ = std::max(0.0f, std::min(scroll_, max_scroll));
}

ControllersScreenResult ControllersScreen::update(const std::vector<UiAction>& actions,
                                                  const std::vector<PointerEvent>& pointers,
                                                  float output_width,
                                                  float output_height,
                                                  const std::vector<KnownPad>& pads,
                                                  bool gamepad_here,
                                                  PadOrder& order) {
    ControllersScreenResult result;
    if (!open_) {
        return result;
    }

    PadLineup lineup = line_up_pads(pads, order, gamepad_here);
    std::vector<LineupRow> rows = lineup_rows(lineup);
    const auto row_count = [&] { return static_cast<int>(rows.size()) + kExtraRows; };
    const auto pad_at = [&](int row) {
        return row < static_cast<int>(rows.size()) ? rows[static_cast<std::size_t>(row)].pad : -1;
    };
    const auto row_of_held = [&] {
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const int pad = rows[row].pad;
            if (pad >= 0 && pad_identity(pads[static_cast<std::size_t>(pad)]) == held_) {
                return static_cast<int>(row);
            }
        }
        return -1;
    };

    // A controller unplugged while in hand is put down, and one unplugged
    // under the cursor leaves it past the end.
    if (holding_) {
        const int row = row_of_held();
        if (row < 0) {
            holding_ = false;
        } else {
            focus_ = row;
        }
    }
    focus_ = std::min(focus_, row_count() - 1);

    const auto move_held = [&](int direction) {
        const int row = row_of_held();
        if (row < 0 || !move_in_lineup(lineup, pad_at(row), direction)) return;
        order = order_of(pads, lineup, gamepad_here);
        rows = lineup_rows(lineup);
        focus_ = row_of_held();
        result.order_changed = true;
    };
    const auto activate = [&] {
        const int line_rows = static_cast<int>(rows.size());
        if (holding_) {
            holding_ = false;
        } else if (focus_ < line_rows) {
            // An empty GamePad has nothing to pick up; a controller is moved
            // into it instead.
            const int pad = pad_at(focus_);
            if (pad >= 0) {
                holding_ = true;
                held_ = pad_identity(pads[static_cast<std::size_t>(pad)]);
            }
        } else {
            result.close_requested = true;
        }
    };

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (holding_) {
                    move_held(-1);
                } else {
                    focus_ = std::max(0, focus_ - 1);
                }
                break;
            case UiAction::Down:
                if (holding_) {
                    move_held(1);
                } else {
                    focus_ = std::min(row_count() - 1, focus_ + 1);
                }
                break;
            case UiAction::Confirm: activate(); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu:
                // Back puts a controller down before it leaves, the way it
                // backs out of anything else one step at a time.
                if (holding_) {
                    holding_ = false;
                } else {
                    result.close_requested = true;
                }
                break;
            // Either upper face button: which controller is this? Both,
            // because X and Y swap places between a Nintendo pad and an Xbox
            // one, and which of them a pad reports as which depends on the
            // pad and on Steam -- the button the prompt names is the one
            // somebody presses, wherever it sits.
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
                if (pad_at(focus_) >= 0) result.identify = pad_at(focus_);
                break;
            // Escape also raises Cancel, which is what it means here.
            case UiAction::OpenMenu:
            case UiAction::Left:
            case UiAction::Right:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::SwapLayout:
            case UiAction::PageUp:
            case UiAction::PageDown: break;
        }
    }

    // The view follows the cursor when it moves, and a finger otherwise.
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(output_width, output_height, row_count(), first_unused_row(rows));
    }
    {
        const Geometry before =
            compute_geometry(output_width, output_height, row_count(), first_unused_row(rows));
        const float view = before.view_bottom - before.view_top;
        drag_.update(pointers,
                     PixelRect{before.panel.x, before.view_top, before.panel.w, view}, scroll_,
                     std::max(0.0f, before.content_height - view));
    }
    const Geometry geometry =
        compute_geometry(output_width, output_height, row_count(), first_unused_row(rows));
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released || drag_.release_was_drag()) continue;
        for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
            if (!tapped(pointer, geometry.rows[index])) continue;
            if (holding_) {
                // Carried to where the finger landed, a step at a time.
                const int target = static_cast<int>(index);
                for (int guard = 0; guard < row_count() && focus_ != target; ++guard) {
                    const int before = focus_;
                    move_held(target < focus_ ? -1 : 1);
                    if (focus_ == before || focus_ < 0) break;
                }
                holding_ = false;
            } else {
                // Highlighted by a first tap, and taken up or put down by a
                // second on the same row.
                if (tap_chooses(focus_, static_cast<int>(index))) activate();
            }
            break;
        }
    }
    focus_ = std::max(0, std::min(focus_, row_count() - 1));
    drag_.settle();

    if (result.close_requested) {
        close();
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
                               const PadOrder& order,
                               const std::string& this_device,
                               const std::string& other_device) {
    if (!open_) {
        return;
    }
    const UiTheme& theme = ui_theme();
    const PadLineup lineup = line_up_pads(pads, order, gamepad_here);
    const std::vector<LineupRow> rows = lineup_rows(lineup);
    const int row_count = static_cast<int>(rows.size()) + kExtraRows;
    const Geometry geometry =
        compute_geometry(output_width, output_height, row_count, first_unused_row(rows));
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.scrim);
    text.draw(quads, "Controller Order", geometry.panel.x, geometry.panel.y, scale * 1.6f,
              theme.text);

    quads.set_clip(PixelRect{geometry.panel.x, geometry.view_top, geometry.panel.w,
                             geometry.view_bottom - geometry.view_top});
    const float pad = menu_row_pad(line);
    const float slot_column = std::max(TextRenderer::measure_width("Player 8", scale),
                                       TextRenderer::measure_width("Not used", scale)) +
                              pad * 3.0f;
    for (int index = 0; index < row_count; ++index) {
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = index == focus_;
        const float baseline = row.y + (row.h - line) * 0.5f;
        const int line_rows = static_cast<int>(rows.size());
        if (index >= line_rows) {
            draw_menu_row(quads, row, focused, scale);
            text.draw(quads, "Done", row.x + pad, baseline, scale, menu_row_text(focused));
            continue;
        }

        // A controller in hand is drawn lifted: the accent colour rather than
        // the cursor's, so moving it is plainly a different thing from
        // moving the cursor.
        const bool lifted = focused && holding_;
        if (lifted) {
            quads.draw_solid(row, theme.accent);
        } else {
            draw_menu_row(quads, row, focused, scale);
        }
        const Color& ink = (focused || lifted) ? theme.background : theme.text;
        const Color dim = (focused || lifted) ? theme.panel : theme.text_dim;

        const LineupRow& entry = rows[static_cast<std::size_t>(index)];
        text.draw(quads, pad_slot_name(entry.slot), row.x + pad, baseline, scale,
                  entry.slot.kind == PadSlot::Kind::NotUsed ? dim : ink);
        const float name_x = row.x + slot_column;
        if (entry.pad < 0) {
            text.draw(quads, "No controller - its screen's touch and motion only", name_x,
                      baseline, scale * 0.9f, dim);
            continue;
        }
        const KnownPad& known = pads[static_cast<std::size_t>(entry.pad)];
        // Steam's name for it where there is one, which is the one the player
        // gave it -- and beside it, always, which device it is plugged into
        // and which player slot that device has it in: "Steam Deck P2", the
        // number its own lights show where it has any.
        const std::string title = known.name.empty() ? pad_label(known) : known.name;
        text.draw(quads, title, name_x, baseline, scale, ink);
        std::string where = known.on_this_device ? this_device : other_device;
        if (known.slot >= 0) {
            where += "  P" + std::to_string(known.slot + 1);
        }
        text.draw(quads, where, row.x + row.w - pad, baseline, scale * 0.8f, dim,
                  TextAlign::Right);
    }
    quads.clear_clip();
    // Only when the list runs past its window, and measured from what it holds.
    {
        const float view = geometry.view_bottom - geometry.view_top;
        if (geometry.content_height > view + 0.5f) {
            draw_scroll_bar(quads, PixelRect{geometry.panel.x, geometry.view_top, geometry.panel.w, view},
                            scroll_ / geometry.content_height, view / geometry.content_height, line,
                            scale);
        }
    }
    if (pads.empty()) {
        text.draw(quads, "No controllers found", geometry.panel.x,
                  geometry.view_bottom + line * 0.4f, scale * 0.9f, theme.text_dim);
    }

    std::vector<GuidePrompt> guide;
    if (holding_) {
        guide.push_back({ButtonGlyph::A, "Put Down", PromptRole::ActsOnSelection});
    } else {
        guide.push_back({ButtonGlyph::X, "Identify"});
        guide.push_back({ButtonGlyph::A, "Reorder", PromptRole::ActsOnSelection});
        guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    }
    const float guide_height = control_guide_height(scale * 0.9f);
    draw_control_guide(quads, text, glyphs, guide, geometry.panel.x + geometry.panel.w,
                       output_height - line * 0.6f - guide_height, scale * 0.9f, theme.text,
                       TextAlign::Right);
}

}  // namespace ds
