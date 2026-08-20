#include "ui/layout_picker.hpp"

#include "ui/prompt.hpp"

#include <algorithm>
#include <string>
#include <cmath>

namespace ds {
namespace {



}  // namespace

void LayoutPicker::open(const LayoutPickerState& state) {
    open_ = true;
    state_ = state;
    selected_layout_id_ = state.layout_id;

    rebuild_footer();
    const int index = registry_->index_of(state.layout_id);
    focus_ = index >= 0 ? index : 0;
}

void LayoutPicker::rebuild_footer() {
    footer_.clear();
    footer_.push_back(Footer::Filtering);
    // Next to smoothing, because the two are the same kind of question -- how
    // the picture is made -- and both are judged against the frame behind the
    // menu. It is absent only where there is nothing to choose: a system whose
    // core draws at one fixed resolution, or no game at all. A companion gets
    // it like every other entry here -- one menu, reachable from either device
    // -- and asks the host to make the change.
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
    footer_.push_back(Footer::Cursor);
    footer_.push_back(Footer::Resume);
    if (state_.session_active) {
        footer_.push_back(Footer::Disconnect);
    }
    if (!state_.companion) {
        footer_.push_back(Footer::Quit);
    }
    // Focus can be sitting on a footer button that just went away.
    if (focus_ >= focus_count()) {
        focus_ = std::max(0, focus_count() - 1);
    }
}

std::string LayoutPicker::footer_label(Footer kind) const {
    switch (kind) {
        case Footer::Filtering:
            return state_.bilinear_filtering ? "Smoothing: On" : "Smoothing: Off";
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
            return state_.handedness == Handedness::Left ? "Hands: Left" : "Hands: Right";
        case Footer::Resume: return "Resume";
        case Footer::Disconnect: return "Disconnect";
        case Footer::Quit: break;
    }
    return "Quit";
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
    const float footer_height = button_height * static_cast<float>(footer_rows) +
                                button_gap * static_cast<float>(footer_rows - 1) + line * 0.9f;
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
    const float footer_top = geometry.panel.y + geometry.panel.h -
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
            selected_layout_id_ = id;
            result.selection_changed = true;
        }
        return true;
    }

    const int footer_index = focus_ - tiles;
    if (footer_index < 0 || footer_index >= footer_count()) {
        return false;
    }
    switch (footer_[static_cast<std::size_t>(footer_index)]) {
        case Footer::Filtering:
            // Stays open, so the change can be judged against the frame behind
            // it. The caller decides what the new value actually is -- on a
            // companion the host does -- and tells us with set_filtering().
            result.filtering_toggled = true;
            return true;
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
        case Footer::Resume:
            result.close_requested = true;
            return true;
        case Footer::Disconnect:
            result.disconnect_requested = true;
            return true;
        case Footer::Quit:
            result.quit_requested = true;
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

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: move_focus(0, -1, geometry); break;
            case UiAction::Down: move_focus(0, 1, geometry); break;
            case UiAction::Left: move_focus(-1, 0, geometry); break;
            case UiAction::Right: move_focus(1, 0, geometry); break;
            case UiAction::Confirm: activate_focus(result); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            case UiAction::Quit: result.quit_requested = true; break;
            // R3 is handled by the app, so that it means the same thing here
            // as it does in a game; the app pushes the result back in with
            // set_filtering(). Toggling it again here would undo that.
            case UiAction::ToggleFilter: break;
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

    if (result.close_requested || result.quit_requested || result.disconnect_requested) {
        open_ = false;
    }
    return result;
}

void LayoutPicker::draw_preview(QuadRenderer& quads,
                                const Layout& layout,
                                const PixelRect& box,
                                const SystemScreens& tile_geometry,
                                const ScreenTextures* textures,
                                const Texture* bezel_art,
                                bool smoothing) {
    const UiTheme& theme = ui_theme();
    // A tile is not a display -- see as_preview().
    const SystemScreens geometry = as_preview(tile_geometry);
    const bool draws_console = layout.uses_console_frame && geometry.console.exists();
    const bool has_art = draws_console && geometry.console.has_art;
    // The tile shows what the layout shows, background included.
    quads.draw_solid(box, has_art ? Color{37.0f / 255.0f, 37.0f / 255.0f, 37.0f / 255.0f, 1.0f}
                                  : Color{0.02f, 0.03f, 0.05f, 1.0f});

    if (draws_console) {
        draw_ds_shell(quads, layout_frame(layout, box, geometry), has_art ? bezel_art : nullptr);
    }

    for (const PlacedScreen& placed : place_screens(layout, box, geometry)) {
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
                              smoothing && !placed.native);
        } else {
            const Color color =
                placed.screen == ScreenId::Top ? theme.top_screen : theme.bottom_screen;
            quads.draw_solid(rect, color);
        }
    }
}

void LayoutPicker::render(QuadRenderer& quads,
                          TextRenderer& text,
                          float output_width,
                          float output_height,
                          const std::string& game_title,
                          const MenuButtonLabels& labels,
                          const ScreenTextures* textures) {
    if (!open_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    // Dim the game behind the menu rather than hiding it, so the layout change
    // can be judged against what is actually on screen.
    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.scrim);

    text.draw(quads, "Screen layout", geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);
    if (!game_title.empty()) {
        text.draw(quads, game_title, geometry.panel.x + geometry.panel.w, geometry.panel.y,
                  scale, theme.text_dim, TextAlign::Right);
    }
    // The colour key only earns its space when the previews are colours. With
    // the game's own screens in the tiles there is nothing to decode.
    const bool previews_are_coloured =
        textures == nullptr || textures->top == nullptr || !textures->top->valid();
    if (previews_are_coloured) {
        text.draw(quads, "Top screen", geometry.panel.x, geometry.panel.y + line * 2.0f, scale,
                  theme.top_screen);
        text.draw(quads, "Bottom screen",
                  geometry.panel.x + TextRenderer::measure_width("Top screen  ", scale),
                  geometry.panel.y + line * 2.0f, scale, theme.bottom_screen);
    }

    for (std::size_t index = 0; index < geometry.tiles.size(); ++index) {
        const PixelRect& tile = geometry.tiles[index];
        const Layout& layout = registry_->at(index);
        const bool focused = (static_cast<int>(index) == focus_);
        const bool selected = (layout.id == selected_layout_id_);

        quads.draw_solid(tile, focused ? theme.panel_focused : theme.panel);

        const float padding = std::round(tile.h * 0.10f);
        const float label_height = line * 1.4f;
        const PixelRect preview{
            tile.x + padding,
            tile.y + padding,
            tile.w - padding * 2.0f,
            tile.h - padding * 2.0f - label_height,
        };
        draw_preview(quads, resolve_for_handedness(layout, state_.handedness), preview, screens_,
                     textures, bezel_art_, smoothing_);

        text.draw(quads, layout.display_name, tile.x + tile.w * 0.5f,
                  tile.y + tile.h - padding - line, scale,
                  selected ? theme.accent : theme.text, TextAlign::Center);

        if (selected) {
            quads.draw_outline(tile, theme.accent, std::max(2.0f, scale));
        }
        if (focused) {
            quads.draw_outline(tile, theme.text, std::max(1.0f, scale * 0.5f));
        }
    }

    for (std::size_t index = 0; index < geometry.footer.size(); ++index) {
        const PixelRect& button = geometry.footer[index];
        const bool focused = (tile_count() + static_cast<int>(index) == focus_);
        quads.draw_solid(button, focused ? theme.panel_focused : theme.panel);
        if (focused) {
            quads.draw_outline(button, theme.text, std::max(1.0f, scale * 0.5f));
        }
        const Footer kind = footer_[index];
        const std::string label = footer_label(kind);
        Color color = theme.text;
        if (kind == Footer::Disconnect) {
            color = theme.warning;
        } else if (kind == Footer::Quit) {
            color = theme.danger;
        }
        // "Smoothing: Off" is much longer than the labels beside it, and a
        // session adds a fifth button, so a label is shrunk rather than allowed
        // to run past the edge of its button.
        float label_scale = scale;
        const float room = button.w - scale * 6.0f;
        const float natural = TextRenderer::measure_width(label, scale);
        if (natural > room && natural > 0.0f) {
            label_scale = scale * (room / natural);
        }
        const float label_line = TextRenderer::line_height(label_scale);
        text.draw(quads, label, button.x + button.w * 0.5f,
                  button.y + (button.h - label_line) * 0.5f, label_scale, color,
                  TextAlign::Center);
    }

    const std::string prompts = format_prompts({{labels.confirm, "Select"},
                                                {labels.cancel, "Resume"},
                                                {"R3", "Smoothing"},
                                                {"F1", "Toggle menu"}});
    text.draw(quads, prompts, geometry.panel.x + geometry.panel.w * 0.5f,
              geometry.panel.y + geometry.panel.h + line * 0.2f, scale * 0.9f, theme.text_dim,
              TextAlign::Center);
}

}  // namespace ds
