#include "ui/layout_picker.hpp"

#include "ui/control_guide.hpp"

#include <algorithm>
#include <string>
#include <cmath>

namespace ds {
namespace {



}  // namespace

void LayoutPicker::open(const LayoutPickerState& state) {
    open_ = true;
    confirm_ = Confirm::None;
    state_ = state;
    selected_layout_id_ = state.layout_id;

    rebuild_footer();
    const int index = registry_->index_of(state.layout_id);
    focus_ = index >= 0 ? index : 0;
}

void LayoutPicker::set_selected_layout(const std::string& layout_id) {
    if (layout_id == selected_layout_id_) {
        return;
    }
    selected_layout_id_ = layout_id;
    // The new layout may offer a gap where the old one did not, or the reverse.
    rebuild_footer();
}

void LayoutPicker::rebuild_footer() {
    footer_.clear();
    // Smoothing, Reset and Quit are not here any more: each is a face button
    // named in the guide along the bottom. What is left is the row of settings
    // that have a value to cycle, which is what a button with a label on it is
    // actually good for.
    //
    // Absent only where there is nothing to choose: a system whose core draws
    // at one fixed resolution, or no game at all. A companion gets it like
    // every other entry here -- one menu, reachable from either device -- and
    // asks the host to make the change.
    if (state_.max_render_scale > 1) {
        footer_.push_back(Footer::RenderScale);
    }
    // Which hand holds the stylus decides which side the small screen sits on.
    // One button rather than a left-handed twin of every layout.
    footer_.push_back(Footer::Hands);
    // Which device makes the sound is only a question when there are two of
    // them, so the entry appears with the session and not before.
    if (state_.session_active) {
        footer_.push_back(Footer::Sound);
    }
    // The pointer is this device's own, and it is the one thing worth reaching
    // quickly while playing: a trackpad you cannot see is a trackpad you cannot
    // aim. Everything else in Settings is a file path or a default for starting
    // a game, which is not a mid-game question, so Settings itself lives on the
    // launcher and not here.
    if (state_.cursor_choosable) {
        footer_.push_back(Footer::Cursor);
    }
    // What the right stick does, where the console has one of its own to have
    // an opinion about -- a 3DS. Which answer a game wants is a fact about
    // that game, so it belongs next to the game rather than in Settings.
    if (state_.right_stick_choosable) {
        footer_.push_back(Footer::RightStick);
    }
    // Only some layouts' gap is a player's choice -- see
    // LayoutComposition::gap_toggleable -- so the button appears only while
    // one of those is the layout actually selected. Read off the layout itself
    // rather than passed in, because the selection changes while the menu is
    // open and a value handed over at open() would already be stale by the
    // time anyone went looking for the button.
    if (registry_->resolve(selected_layout_id_).composition.gap_toggleable) {
        footer_.push_back(Footer::ScreenGap);
    }
    // Focus can be sitting on a footer button that just went away.
    if (focus_ >= focus_count()) {
        focus_ = std::max(0, focus_count() - 1);
    }
}

std::string LayoutPicker::footer_label(Footer kind) const {
    switch (kind) {
        case Footer::RenderScale:
            return "Render: " + std::to_string(state_.render_scale) + "x";
        case Footer::Sound:
            switch (state_.audio_target) {
                case AudioTarget::TopScreen: return "Sound: Top";
                case AudioTarget::BottomScreen: return "Sound: Bottom";
                case AudioTarget::Both: break;
            }
            return "Sound: Both";
        case Footer::Cursor:
            return std::string("Pointer: ") + to_string(state_.cursor_mode);
        case Footer::Hands:
            return state_.handedness == Handedness::Left ? "Left-Handed" : "Right-Handed";
        case Footer::RightStick:
            return std::string("Right Stick: ") + to_string(state_.right_stick_mode);
        case Footer::ScreenGap: break;
    }
    return state_.screen_gap ? "Screen Gap: On" : "Screen Gap: Off";
}

LayoutPicker::Geometry LayoutPicker::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);

    const float margin = std::round(output_width * 0.06f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.06f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.12f)};

    const float line = TextRenderer::line_height(geometry.scale);
    const int buttons = std::max(1, footer_count());
    // Four across is as narrow as a label stays readable at arm's length, so a
    // fifth button starts a second row rather than shrinking them all.
    const int footer_rows = (buttons > 4) ? 2 : 1;
    geometry.footer_columns = (buttons + footer_rows - 1) / footer_rows;

    const float header_height = line * 3.5f;
    const float button_height = line * 2.1f;
    const float button_gap = std::round(line * 0.35f);
    // The guide sits below the buttons, along the bottom of the panel, and is
    // a row of pictures rather than a line of text -- so the space it needs is
    // asked for rather than assumed.
    const float guide_height = control_guide_height(geometry.scale * 0.9f);
    const float footer_height = button_height * static_cast<float>(footer_rows) +
                                button_gap * static_cast<float>(footer_rows - 1) + guide_height +
                                line * 1.4f;
    const float grid_top = geometry.panel.y + header_height;
    const float grid_height = geometry.panel.h - header_height - footer_height;

    const int count = tile_count();
    // Prefer three across; drop to two on a narrow window so previews stay big
    // enough to read at arm's length.
    geometry.columns = (output_width < output_height * 1.2f) ? 2 : 3;
    geometry.columns = std::min(geometry.columns, std::max(1, count));
    const int rows = (count + geometry.columns - 1) / geometry.columns;

    const float gutter = std::round(output_width * 0.015f);
    const float tile_width = (geometry.panel.w - gutter * (geometry.columns - 1)) / geometry.columns;
    const float tile_height = rows > 0 ? (grid_height - gutter * (rows - 1)) / rows : grid_height;

    geometry.tiles.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        const int column = index % geometry.columns;
        const int row = index / geometry.columns;
        geometry.tiles.push_back(PixelRect{
            geometry.panel.x + column * (tile_width + gutter),
            grid_top + row * (tile_height + gutter),
            tile_width,
            tile_height,
        });
    }

    const int footer_columns = std::max(1, geometry.footer_columns);
    const float footer_width =
        (geometry.panel.w - gutter * (footer_columns - 1)) / footer_columns;
    const float footer_top = geometry.panel.y + geometry.panel.h - guide_height - line * 0.5f -
                             (button_height * static_cast<float>(footer_rows) +
                              button_gap * static_cast<float>(footer_rows - 1));
    for (int index = 0; index < buttons; ++index) {
        const int column = index % footer_columns;
        const int row = index / footer_columns;
        geometry.footer.push_back(PixelRect{
            geometry.panel.x + column * (footer_width + gutter),
            footer_top + row * (button_height + button_gap),
            footer_width,
            button_height,
        });
    }
    return geometry;
}

LayoutPicker::ConfirmGeometry LayoutPicker::compute_confirm_geometry(float output_width,
                                                                    float output_height) const {
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const float dialog_width = std::round(output_width * 0.58f);
    const float dialog_height = std::round(line * 8.0f);

    ConfirmGeometry geometry;
    geometry.dialog = PixelRect{std::round((output_width - dialog_width) * 0.5f),
                                std::round((output_height - dialog_height) * 0.5f), dialog_width,
                                dialog_height};
    // Two halves of the prompt row, so a finger has somewhere to land on a
    // device with no buttons to press instead.
    const float row_height = line * 2.4f;
    const float row_y = geometry.dialog.y + geometry.dialog.h - row_height - line * 0.4f;
    geometry.accept = PixelRect{geometry.dialog.x, row_y, geometry.dialog.w * 0.5f, row_height};
    geometry.dismiss = PixelRect{geometry.dialog.x + geometry.dialog.w * 0.5f, row_y,
                                 geometry.dialog.w * 0.5f, row_height};
    return geometry;
}

void LayoutPicker::move_focus(int delta_x, int delta_y, const Geometry& geometry) {
    const int tiles = tile_count();
    const int columns = std::max(1, geometry.columns);

    if (focus_ < tiles) {
        const int row = focus_ / columns;
        const int column = focus_ % columns;
        if (delta_x != 0) {
            const int next = focus_ + delta_x;
            if (next >= row * columns && next < std::min(tiles, (row + 1) * columns)) {
                focus_ = next;
            }
            return;
        }
        if (delta_y != 0) {
            const int next = focus_ + delta_y * columns;
            if (next >= 0 && next < tiles) {
                focus_ = next;
            } else if (delta_y > 0) {
                focus_ = tiles;  // fall through into the footer row
            }
            (void)column;
            return;
        }
        return;
    }

    // Focus is in the footer, which may itself be two rows deep.
    const int footer_index = focus_ - tiles;
    const int footer_columns = std::max(1, geometry.footer_columns);
    if (delta_x != 0) {
        const int next = footer_index + delta_x;
        if (next >= 0 && next < footer_count()) {
            focus_ = tiles + next;
        }
        return;
    }
    if (delta_y > 0) {
        const int next = footer_index + footer_columns;
        if (next < footer_count()) {
            focus_ = tiles + next;
        }
        return;
    }
    if (delta_y < 0) {
        if (footer_index >= footer_columns) {
            focus_ = tiles + footer_index - footer_columns;
            return;
        }
        if (tiles > 0) {
            // Back up into the last grid row, keeping roughly the same column.
            const int last_row = (tiles - 1) / columns;
            const int column = std::min(footer_index, columns - 1);
            focus_ = std::min(tiles - 1, last_row * columns + column);
        }
    }
}

bool LayoutPicker::activate_focus(LayoutPickerResult& result) {
    const int tiles = tile_count();
    if (focus_ < tiles) {
        const std::string id = registry_->at(static_cast<std::size_t>(focus_)).id;
        if (id != selected_layout_id_) {
            set_selected_layout(id);
            result.selection_changed = true;
        }
        return true;
    }

    const int footer_index = focus_ - tiles;
    if (footer_index < 0 || footer_index >= footer_count()) {
        return false;
    }
    switch (footer_[static_cast<std::size_t>(footer_index)]) {
        case Footer::RenderScale:
            // Same again: the caller cycles it and tells us what it landed on,
            // so the menu never shows a scale the core was not actually given.
            result.render_scale_changed = true;
            return true;
        case Footer::Sound:
            result.audio_target_changed = true;
            return true;
        case Footer::Cursor:
            result.cursor_mode_changed = true;
            return true;
        case Footer::Hands:
            result.handedness_changed = true;
            return true;
        case Footer::RightStick:
            // Cycled by the caller and handed back with set_right_stick_mode(),
            // the same as the others here: on a companion the host decides.
            result.right_stick_mode_changed = true;
            return true;
        case Footer::ScreenGap:
            result.screen_gap_toggled = true;
            return true;
    }
    return false;
}

LayoutPickerResult LayoutPicker::update(const std::vector<UiAction>& actions,
                                        const std::vector<PointerEvent>& pointers,
                                        float output_width,
                                        float output_height) {
    LayoutPickerResult result;
    if (!open_) {
        return result;
    }

    const Geometry geometry = compute_geometry(output_width, output_height);

    // A confirmation takes the whole menu: while one is up it is the only
    // question on screen, so nothing behind it moves, cycles or opens.
    if (confirming()) {
        const ConfirmGeometry confirm = compute_confirm_geometry(output_width, output_height);
        bool accepted = false;
        bool dismissed = false;
        for (const UiAction action : actions) {
            if (action == UiAction::Confirm) accepted = true;
            if (action == UiAction::Cancel) dismissed = true;
        }
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released) continue;
            if (confirm.accept.contains(pointer.x, pointer.y)) accepted = true;
            else if (confirm.dismiss.contains(pointer.x, pointer.y)) dismissed = true;
        }
        if (accepted) {
            if (confirm_ == Confirm::Reset) {
                result.reset_requested = true;
            } else {
                result.quit_requested = true;
            }
            confirm_ = Confirm::None;
            open_ = false;
        } else if (dismissed) {
            confirm_ = Confirm::None;
        }
        return result;
    }

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: move_focus(0, -1, geometry); break;
            case UiAction::Down: move_focus(0, 1, geometry); break;
            case UiAction::Left: move_focus(-1, 0, geometry); break;
            case UiAction::Right: move_focus(1, 0, geometry); break;
            case UiAction::Confirm: activate_focus(result); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            // The app's own quit key, which is not a face button and is not
            // asked about twice.
            case UiAction::Quit: result.quit_requested = true; break;
            case UiAction::MenuQuit: confirm_ = Confirm::Quit; break;
            case UiAction::MenuReset: confirm_ = Confirm::Reset; break;
            // Not asked about: unlike resetting or quitting, reconnecting is a
            // menu away and costs nothing but the walk back.
            case UiAction::MenuDisconnect:
                if (state_.session_active) result.disconnect_requested = true;
                break;
            // R3 is handled by the app, so that it means the same thing here
            // as it does in a game; the app pushes the result back in with
            // set_filtering(). Toggling it again here would undo that.
            case UiAction::ToggleFilter: break;
            // L3 only swaps layouts with a game on screen: here every layout
            // is a tile away already, and jumping the selection out from under
            // the focus would be the opposite of a menu.
            case UiAction::SwapLayout: break;
            case UiAction::PageUp:
            case UiAction::PageDown: break;  // nothing here scrolls
        }
    }

    // Touch: a tap picks the thing under the finger directly, no focus dance.
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) {
            continue;
        }
        for (std::size_t index = 0; index < geometry.tiles.size(); ++index) {
            if (geometry.tiles[index].contains(pointer.x, pointer.y)) {
                focus_ = static_cast<int>(index);
                activate_focus(result);
            }
        }
        for (std::size_t index = 0; index < geometry.footer.size(); ++index) {
            if (geometry.footer[index].contains(pointer.x, pointer.y)) {
                focus_ = tile_count() + static_cast<int>(index);
                activate_focus(result);
            }
        }
    }

    if (confirming()) {
        // Raised by a press this frame. Anything else that press implied is
        // dropped: the menu is now waiting on an answer.
        return LayoutPickerResult();
    }

    if (result.close_requested || result.quit_requested || result.disconnect_requested ||
        result.reset_requested) {
        open_ = false;
    }
    return result;
}

Layout LayoutPicker::resolved_layout_for_preview(const Layout& layout) const {
    Layout resolved =
        resolve_for_handedness(resolve_gap_choice(layout, state_.screen_gap), state_.handedness);
    resolved.fill_display = fill_display_;
    return resolved;
}

void LayoutPicker::draw_preview(QuadRenderer& quads,
                                const Layout& layout,
                                const PixelRect& box,
                                float display_width,
                                float display_height,
                                const SystemScreens& tile_geometry,
                                const ScreenTextures* textures,
                                const Texture* bezel_art,
                                bool smoothing) {
    const UiTheme& theme = ui_theme();
    const SystemScreens& geometry = tile_geometry;
    const bool draws_console = layout.uses_console_frame && geometry.console.exists();
    const bool has_art = draws_console && geometry.console.has_art;
    // A layout can run off the edges of what it is drawn into -- a console
    // picture filling a small display does exactly that -- and inside a tile
    // that would land on the neighbours.
    quads.set_clip(box);
    // The tile shows what the layout shows, background included.
    quads.draw_solid(box, has_art ? Color{37.0f / 255.0f, 37.0f / 255.0f, 37.0f / 255.0f, 1.0f}
                                  : Color{0.02f, 0.03f, 0.05f, 1.0f});

    if (draws_console) {
        draw_ds_shell(quads, layout_frame(layout, box, geometry, display_width, display_height),
                     has_art ? bezel_art : nullptr);
    }

    for (const PlacedScreen& placed :
        place_screens(layout, box, geometry, display_width, display_height)) {
        const PixelRect& rect = placed.rect;
        const Texture* texture = nullptr;
        if (textures != nullptr) {
            texture = (placed.screen == ScreenId::Top) ? textures->top : textures->bottom;
        }
        // The paused game itself, which says what picking this will actually
        // look like far better than two coloured blocks and a key can. The
        // blocks are still there for when there is no frame to show -- a
        // companion that has not received one yet, or a core still booting.
        if (texture != nullptr && texture->valid()) {
            quads.draw_screen(rect, *texture, Color{}, placed.rotation,
                              screen_needs_smoothing(placed, *texture, smoothing));
        } else {
            const Color color =
                placed.screen == ScreenId::Top ? theme.top_screen : theme.bottom_screen;
            quads.draw_solid(rect, color);
        }
    }
    quads.clear_clip();
}

void LayoutPicker::render(QuadRenderer& quads,
                          TextRenderer& text,
                          const ButtonGlyphSheet& glyphs,
                          float output_width,
                          float output_height,
                          const std::string& game_title,
                          const ScreenTextures* textures) {
    if (!open_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float border = std::max(2.0f, std::round(scale * 0.6f));

    // The game the menu was opened over, where the heading used to say what
    // the screen already looks like.
    if (!game_title.empty()) {
        text.draw(quads, game_title, geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);
    }

    for (std::size_t index = 0; index < geometry.tiles.size(); ++index) {
        const PixelRect& tile = geometry.tiles[index];
        const Layout& layout = registry_->at(index);
        const bool focused = (static_cast<int>(index) == focus_);
        const bool selected = (layout.id == selected_layout_id_);

        // Focused tiles are solid and everything else is a hairline, so the
        // blurred game stays visible through the menu that sits over it.
        if (focused) {
            quads.draw_solid(tile, theme.text);
        }

        const float padding = std::round(tile.h * 0.10f);
        const float label_height = line * 1.4f;
        const PixelRect preview{
            tile.x + padding,
            tile.y + padding,
            tile.w - padding * 2.0f,
            tile.h - padding * 2.0f - label_height,
        };
        draw_preview(quads, resolved_layout_for_preview(layout), preview,
                     display_width_ > 0.0f ? display_width_ : output_width,
                     display_height_ > 0.0f ? display_height_ : output_height, screens_, textures,
                     bezel_art_, smoothing_);

        text.draw(quads, layout.display_name, tile.x + tile.w * 0.5f,
                  tile.y + tile.h - padding - line, scale,
                  focused ? theme.background : theme.text, TextAlign::Center);

        // The layout actually in use keeps its outline even when the cursor has
        // moved off it, so the menu still says which one the game is drawn in.
        if (!focused) {
            quads.draw_outline(tile, selected ? theme.accent : theme.text, border);
        }
    }

    for (std::size_t index = 0; index < geometry.footer.size(); ++index) {
        const PixelRect& button = geometry.footer[index];
        const bool focused = (tile_count() + static_cast<int>(index) == focus_);
        if (focused) {
            quads.draw_solid(button, theme.text);
        } else {
            quads.draw_outline(button, theme.text, border);
        }

        const std::string label = footer_label(footer_[index]);
        // A label is shrunk rather than allowed to run past the edge of its
        // button: the longest of them is much longer than the rest.
        float label_scale = scale;
        const float room = button.w - scale * 6.0f;
        const float natural = TextRenderer::measure_width(label, scale);
        if (natural > room && natural > 0.0f) {
            label_scale = scale * (room / natural);
        }
        const float label_line = TextRenderer::line_height(label_scale);
        text.draw(quads, label, button.x + button.w * 0.5f,
                  button.y + (button.h - label_line) * 0.5f, label_scale,
                  focused ? theme.background : theme.text, TextAlign::Center);
    }

    std::vector<GuidePrompt> guide{{ButtonGlyph::RightStick, "Screen Smoothing"},
                                   {ButtonGlyph::A, "OK"},
                                   {ButtonGlyph::B, "Resume"},
                                   {ButtonGlyph::X, "Quit"},
                                   {ButtonGlyph::Y, "Reset"}};
    if (state_.session_active) {
        guide.push_back({ButtonGlyph::Plus, "Disconnect"});
    }
    draw_control_guide(quads, text, glyphs, guide, geometry.panel.x + geometry.panel.w,
                       geometry.panel.y + geometry.panel.h - control_guide_height(scale * 0.9f),
                       scale * 0.9f, theme.text, TextAlign::Right);

    if (confirming()) {
        const ConfirmGeometry confirm = compute_confirm_geometry(output_width, output_height);
        // Everything the confirmation is asking about goes back another step,
        // so there is one thing on screen to answer.
        quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                         Color{0.0f, 0.0f, 0.0f, 0.5f});
        quads.draw_solid(confirm.dialog, theme.text);

        const char* question = confirm_ == Confirm::Reset
                                   ? "Are you sure you want to reset?\nUnsaved progress will be lost."
                                   : "Are you sure you want to quit?\nUnsaved progress will be lost.";
        text.draw_wrapped(quads, question, confirm.dialog.x + confirm.dialog.w * 0.5f,
                          confirm.dialog.y + line * 1.2f, confirm.dialog.w - line * 2.0f,
                          scale * 1.15f, theme.background, TextAlign::Center);

        draw_control_guide(quads, text, glyphs,
                           {{ButtonGlyph::A, "OK"}, {ButtonGlyph::B, "Back"}},
                           confirm.dialog.x + confirm.dialog.w * 0.5f, confirm.accept.y,
                           scale * 1.05f, theme.background);
    }
}

}  // namespace ds
