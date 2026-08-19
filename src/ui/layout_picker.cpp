#include "ui/layout_picker.hpp"

#include <algorithm>
#include <cmath>

namespace ds {
namespace {

const char* footer_label(int index) {
    switch (index) {
        case 0: return "Settings";
        case 1: return "Resume";
        case 2: return "Quit";
        default: return "";
    }
}

}  // namespace

void LayoutPicker::open(const std::string& layout_id) {
    open_ = true;
    selected_layout_id_ = layout_id;
    const int index = registry_->index_of(layout_id);
    focus_ = index >= 0 ? index : 0;
}

LayoutPicker::Geometry LayoutPicker::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);

    const float margin = std::round(output_width * 0.06f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.06f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.12f)};

    const float header_height = TextRenderer::line_height(geometry.scale) * 3.5f;
    const float footer_height = TextRenderer::line_height(geometry.scale) * 3.0f;
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

    const int footer_count = static_cast<int>(Footer::Count);
    const float footer_width = (geometry.panel.w - gutter * (footer_count - 1)) / footer_count;
    const float footer_y = geometry.panel.y + geometry.panel.h - footer_height * 0.8f;
    for (int index = 0; index < footer_count; ++index) {
        geometry.footer.push_back(PixelRect{
            geometry.panel.x + index * (footer_width + gutter),
            footer_y,
            footer_width,
            footer_height * 0.7f,
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

    // Focus is on the footer row.
    const int footer_index = focus_ - tiles;
    if (delta_x != 0) {
        const int next = footer_index + delta_x;
        if (next >= 0 && next < static_cast<int>(Footer::Count)) {
            focus_ = tiles + next;
        }
        return;
    }
    if (delta_y < 0 && tiles > 0) {
        // Back up into the last grid row, keeping roughly the same column.
        const int last_row = (tiles - 1) / columns;
        const int column = std::min(footer_index, columns - 1);
        focus_ = std::min(tiles - 1, last_row * columns + column);
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

    switch (static_cast<Footer>(focus_ - tiles)) {
        case Footer::Settings:
            result.open_settings = true;
            return true;
        case Footer::Resume:
            result.close_requested = true;
            return true;
        case Footer::Quit:
            result.quit_requested = true;
            return true;
        case Footer::Count:
            break;
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

    if (result.close_requested || result.quit_requested || result.open_settings) {
        open_ = false;
    }
    return result;
}

void LayoutPicker::draw_preview(QuadRenderer& quads, const Layout& layout, const PixelRect& box) {
    const UiTheme& theme = ui_theme();
    quads.draw_solid(box, Color{0.02f, 0.03f, 0.05f, 1.0f});

    for (const ScreenPlacement& placement : layout.placements) {
        const PixelRect rect = placement_pixel_rect(placement, box.w, box.h);
        const PixelRect shifted{box.x + rect.x, box.y + rect.y, rect.w, rect.h};
        const Color color =
            placement.screen == ScreenId::Top ? theme.top_screen : theme.bottom_screen;
        quads.draw_solid(shifted, color);
    }
}

void LayoutPicker::render(QuadRenderer& quads,
                          TextRenderer& text,
                          float output_width,
                          float output_height,
                          const std::string& game_title) {
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
    text.draw(quads, "Top screen", geometry.panel.x, geometry.panel.y + line * 2.0f, scale,
              theme.top_screen);
    text.draw(quads, "Bottom screen", geometry.panel.x + TextRenderer::measure_width("Top screen  ", scale),
              geometry.panel.y + line * 2.0f, scale, theme.bottom_screen);

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
        draw_preview(quads, layout, preview);

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
        const Color color = (static_cast<Footer>(index) == Footer::Quit) ? theme.danger : theme.text;
        text.draw(quads, footer_label(static_cast<int>(index)), button.x + button.w * 0.5f,
                  button.y + (button.h - line) * 0.5f, scale, color, TextAlign::Center);
    }

    text.draw(quads, "R3 closes this menu   -   two fingers also opens it",
              geometry.panel.x + geometry.panel.w * 0.5f,
              geometry.panel.y + geometry.panel.h + line * 0.2f, scale * 0.9f, theme.text_dim,
              TextAlign::Center);
}

}  // namespace ds
