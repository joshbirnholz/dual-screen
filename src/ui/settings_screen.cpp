#include "ui/settings_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/prompt.hpp"

namespace ds {

std::vector<BiosFileStatus> check_bios_files(const std::string& system_dir) {
    // melonDS DS can boot DS games with its own open-source BIOS replacement,
    // so none of these are strictly required — but native files are needed for
    // firmware boot and for anything DSi.
    const struct {
        const char* filename;
        bool required;
        const char* note;
    } kFiles[] = {
        {"bios7.bin", false, "DS ARM7 BIOS - needed for native boot"},
        {"bios9.bin", false, "DS ARM9 BIOS - needed for native boot"},
        {"firmware.bin", false, "DS firmware - needed to boot to the DS menu"},
        {"dsi_bios7.bin", false, "DSi only"},
        {"dsi_bios9.bin", false, "DSi only"},
        {"dsi_firmware.bin", false, "DSi only"},
        {"dsi_nand.bin", false, "DSi only"},
    };

    std::vector<BiosFileStatus> statuses;
    for (const auto& file : kFiles) {
        BiosFileStatus status;
        status.filename = file.filename;
        status.required = file.required;
        status.note = file.note;
        status.present = file_exists(path_join(system_dir, file.filename));
        statuses.push_back(status);
    }
    return statuses;
}

void SettingsScreen::open(const Settings& settings, const std::string& resolved_core_path) {
    open_ = true;
    focus_ = 0;
    settings_ = settings;
    resolved_core_path_ = resolved_core_path;
    system_dir_candidates_ = candidate_system_dirs(paths_);
    if (std::find(system_dir_candidates_.begin(), system_dir_candidates_.end(),
                  settings_.system_dir) == system_dir_candidates_.end()) {
        system_dir_candidates_.insert(system_dir_candidates_.begin(), settings_.system_dir);
    }
    refresh_bios();
}

void SettingsScreen::refresh_bios() { bios_ = check_bios_files(settings_.system_dir); }

SettingsScreen::Geometry SettingsScreen::compute_geometry(float output_width,
                                                          float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(geometry.scale);

    const float margin = std::round(output_width * 0.07f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.08f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.16f)};

    const float rows_top = geometry.panel.y + line * 3.5f;
    const float row_height = line * 1.9f;
    for (int index = 0; index < kRowCount; ++index) {
        geometry.rows.push_back(PixelRect{
            geometry.panel.x,
            rows_top + index * (row_height + line * 0.3f),
            geometry.panel.w,
            row_height,
        });
    }

    const float bios_top = rows_top + kRowCount * (row_height + line * 0.3f) + line;
    geometry.bios_panel = PixelRect{geometry.panel.x, bios_top, geometry.panel.w,
                                    std::max(line * 3.0f,
                                             geometry.panel.y + geometry.panel.h - bios_top)};
    return geometry;
}

void SettingsScreen::adjust(int direction, SettingsScreenResult& result) {
    switch (static_cast<Row>(focus_)) {
        case Row::SystemDir: {
            if (system_dir_candidates_.empty()) break;
            const auto it = std::find(system_dir_candidates_.begin(), system_dir_candidates_.end(),
                                      settings_.system_dir);
            int index = (it == system_dir_candidates_.end())
                            ? 0
                            : static_cast<int>(std::distance(system_dir_candidates_.begin(), it));
            const int count = static_cast<int>(system_dir_candidates_.size());
            index = ((index + direction) % count + count) % count;
            settings_.system_dir = system_dir_candidates_[static_cast<std::size_t>(index)];
            refresh_bios();
            result.settings_changed = true;
            break;
        }
        case Row::CorePath:
            break;  // read-only here; edited in settings.json
        case Row::DefaultLayout: {
            const int count = static_cast<int>(registry_->size());
            if (count == 0) break;
            int index = registry_->index_of(settings_.default_layout_id);
            if (index < 0) index = 0;
            index = ((index + direction) % count + count) % count;
            settings_.default_layout_id = registry_->at(static_cast<std::size_t>(index)).id;
            result.settings_changed = true;
            break;
        }
        case Row::Cursor:
            settings_.cursor_mode = next_cursor_mode(settings_.cursor_mode);
            result.settings_changed = true;
            break;
        case Row::Vsync:
            settings_.vsync = !settings_.vsync;
            result.settings_changed = true;
            break;
        case Row::Back:
            result.close_requested = true;
            break;
        case Row::Count:
            break;
    }
}

SettingsScreenResult SettingsScreen::update(const std::vector<UiAction>& actions,
                                            const std::vector<PointerEvent>& pointers,
                                            float output_width,
                                            float output_height) {
    SettingsScreenResult result;
    if (!open_) {
        return result;
    }

    const Geometry geometry = compute_geometry(output_width, output_height);

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: focus_ = (focus_ + kRowCount - 1) % kRowCount; break;
            case UiAction::Down: focus_ = (focus_ + 1) % kRowCount; break;
            case UiAction::Left: adjust(-1, result); break;
            case UiAction::Right: adjust(1, result); break;
            case UiAction::Confirm: adjust(1, result); break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            case UiAction::Quit: break;
            case UiAction::ToggleFilter:  // handled by the app, wherever you are
            case UiAction::PageUp:
            case UiAction::PageDown: break;  // nothing here scrolls
        }
    }

    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
            if (geometry.rows[index].contains(pointer.x, pointer.y)) {
                focus_ = static_cast<int>(index);
                adjust(1, result);
            }
        }
    }

    if (result.close_requested) {
        open_ = false;
    }
    return result;
}

void SettingsScreen::render(QuadRenderer& quads,
                            TextRenderer& text,
                            float output_width,
                            float output_height) {
    if (!open_) {
        return;
    }

    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height}, theme.scrim);
    text.draw(quads, "Settings", geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);
    text.draw(quads,
              format_prompts({{"Left/Right", "Change value"}, {"B", "Back"}}) +
                  "     Paths are edited in settings.json",
              geometry.panel.x, geometry.panel.y + line * 1.8f, scale * 0.9f, theme.text_dim);

    const bool core_found = !resolved_core_path_.empty() && file_exists(resolved_core_path_);
    const std::string rows[kRowCount][2] = {
        {"BIOS / system folder", settings_.system_dir},
        {"melonDS DS core", core_found ? resolved_core_path_ : std::string("not found")},
        {"Default layout for new games", registry_->resolve(settings_.default_layout_id).display_name},
        {"Mouse pointer", to_string(settings_.cursor_mode)},
        {"Wait for vsync", settings_.vsync ? "On" : "Off"},
        {"Back", ""},
    };

    for (int index = 0; index < kRowCount; ++index) {
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = (index == focus_);
        quads.draw_solid(row, focused ? theme.panel_focused : theme.panel);
        if (focused) {
            quads.draw_outline(row, theme.accent, std::max(1.0f, scale * 0.5f));
        }

        // Label on the left, value on the right, both on one baseline. The
        // value is elided rather than allowed to run past the row, which is
        // what a long BIOS path would otherwise do.
        const float pad = line * 0.5f;
        const float baseline = row.y + (row.h - line) * 0.5f;
        text.draw(quads, rows[index][0], row.x + pad, baseline, scale, theme.text);
        if (!rows[index][1].empty()) {
            Color value_color = theme.text_dim;
            if (index == static_cast<int>(Row::CorePath)) {
                value_color = core_found ? theme.success : theme.danger;
            }
            const float label_width = TextRenderer::measure_width(rows[index][0], scale);
            const float available = row.w - label_width - pad * 4.0f;
            const std::string value =
                TextRenderer::elide_left(rows[index][1], available, scale * 0.9f);
            text.draw(quads, value, row.x + row.w - pad, baseline, scale * 0.9f, value_color,
                      TextAlign::Right);
        }
    }

    // BIOS file inventory.
    float pen_y = geometry.bios_panel.y;
    text.draw(quads, "BIOS and firmware files found in that folder", geometry.bios_panel.x, pen_y,
              scale, theme.text);
    pen_y += line * 1.6f;

    const float column_width = geometry.bios_panel.w * 0.5f;
    int column = 0;
    float column_top = pen_y;
    for (const BiosFileStatus& status : bios_) {
        const float x = geometry.bios_panel.x + column * column_width;
        const std::string mark = status.present ? "[found]   " : "[missing] ";
        const Color color = status.present ? theme.success
                                           : (status.required ? theme.danger : theme.text_dim);
        text.draw(quads, mark + status.filename, x, pen_y, scale * 0.9f, color);
        pen_y += line * 1.1f;
        if (pen_y > geometry.bios_panel.y + geometry.bios_panel.h - line * 2.0f && column == 0) {
            column = 1;
            pen_y = column_top;
        }
    }

    const std::string footnote =
        "None of these ship with this app - they have to be your own dumps. DS games still boot "
        "without them, using the core's built-in BIOS replacement.";
    const float footnote_scale = scale * 0.85f;
    const float footnote_width = geometry.panel.w;
    const std::size_t footnote_lines =
        TextRenderer::wrap(footnote, footnote_width, footnote_scale).size();
    text.draw_wrapped(quads, footnote, output_width * 0.5f,
                      output_height - TextRenderer::line_height(footnote_scale) *
                                          (footnote_lines + 1.0f),
                      footnote_width, footnote_scale, theme.text_dim, TextAlign::Center);
}

}  // namespace ds
