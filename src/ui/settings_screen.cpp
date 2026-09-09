#include "ui/settings_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/control_guide.hpp"

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

void SettingsScreen::open(const Settings& settings, const std::vector<CoreStatus>& cores) {
    open_ = true;
    focus_ = 0;
    settings_ = settings;
    cores_ = cores;
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

    // The cores get their own block: a heading plus one line per console, so
    // the space it takes follows the list rather than a remembered count.
    const float core_top = rows_top + kRowCount * (row_height + line * 0.3f) + line;
    const float core_height = line * (1.6f + 1.1f * static_cast<float>(cores_.size()));
    geometry.core_panel = PixelRect{geometry.panel.x, core_top, geometry.panel.w, core_height};

    const float bios_top = core_top + core_height + line * 0.6f;
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
        case Row::ScreenCodecDs:
            settings_.screen_codec_ds = video::next_screen_codec(settings_.screen_codec_ds);
            result.settings_changed = true;
            break;
        case Row::ScreenCodecThreeDs:
            settings_.screen_codec_3ds = video::next_screen_codec(settings_.screen_codec_3ds);
            result.settings_changed = true;
            break;
        case Row::ScreenCodecWiiU:
            settings_.screen_codec_wii_u = video::next_screen_codec(settings_.screen_codec_wii_u);
            result.settings_changed = true;
            break;
        case Row::Vsync:
            settings_.vsync = !settings_.vsync;
            result.settings_changed = true;
            break;
        case Row::WiiUScreenAlone:
            // Two values, so either direction is the other one.
            settings_.wii_u_screen_alone = (settings_.wii_u_screen_alone == ScreenId::Top)
                                               ? ScreenId::Bottom
                                               : ScreenId::Top;
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
            // A menu's face buttons. Nothing here answers them, and a bare
            // fallthrough into Quit would make pressing X leave the screen.
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            // Already here, which is the whole of what it asks for.
            case UiAction::MenuSettings: break;
            case UiAction::Quit: break;
            case UiAction::ToggleFilter:  // handled by the app, wherever you are
            case UiAction::SwapLayout:    // nothing here is a layout
            case UiAction::PageUp:
            case UiAction::PageDown: break;  // nothing here scrolls
        }
    }

    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
            if (tapped(pointer, geometry.rows[index])) {
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
                            const ButtonGlyphSheet& glyphs,
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
    text.draw(quads, "Left/Right changes a value.   Paths are edited in settings.json",
              geometry.panel.x, geometry.panel.y + line * 1.8f, scale * 0.9f, theme.text_dim);

    const std::string rows[kRowCount][2] = {
        {"BIOS / system folder", settings_.system_dir},
        {"Default layout for new games", registry_->resolve(settings_.default_layout_id).display_name},
        {"Streaming format (DS)", video::to_string(settings_.screen_codec_ds)},
        {"Streaming format (3DS)", video::to_string(settings_.screen_codec_3ds)},
        {"Streaming format (Wii U)", video::to_string(settings_.screen_codec_wii_u)},
        {"Wait for vsync", settings_.vsync ? "On" : "Off"},
        {"This device alone is a Wii U's",
         settings_.wii_u_screen_alone == ScreenId::Bottom ? "GamePad" : "Television"},
        {"Back", ""},
    };

    for (int index = 0; index < kRowCount; ++index) {
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = (index == focus_);
        draw_menu_row(quads, row, focused, scale);

        // Label on the left, value on the right, both on one baseline. The
        // value is elided rather than allowed to run past the row, which is
        // what a long BIOS path would otherwise do.
        const float pad = line * 0.5f;
        const float baseline = row.y + (row.h - line) * 0.5f;
        text.draw(quads, rows[index][0], row.x + pad, baseline, scale, menu_row_text(focused));
        if (!rows[index][1].empty()) {
            const Color value_color = focused ? theme.panel : theme.text_dim;
            const float label_width = TextRenderer::measure_width(rows[index][0], scale);
            const float available = row.w - label_width - pad * 4.0f;
            const std::string value =
                TextRenderer::elide_left(rows[index][1], available, scale * 0.9f);
            text.draw(quads, value, row.x + row.w - pad, baseline, scale * 0.9f, value_color,
                      TextAlign::Right);
        }
    }

    // One line per console, saying where its emulator was found -- which for
    // most of them is a core this app loads and for a Wii U is a program it
    // starts. A machine set up for one and not another says so before a game is
    // picked rather than after.
    float core_pen_y = geometry.core_panel.y;
    text.draw(quads, "Emulators", geometry.core_panel.x, core_pen_y, scale, theme.text);
    core_pen_y += line * 1.6f;
    for (const CoreStatus& core : cores_) {
        const bool found = !core.resolved_path.empty() && file_exists(core.resolved_path);
        const std::string label = core.system_name + "  ";
        text.draw(quads, label, geometry.core_panel.x, core_pen_y, scale * 0.9f, theme.text_dim);
        const float label_width = TextRenderer::measure_width(label, scale * 0.9f);
        const float available = geometry.core_panel.w - label_width;
        const std::string value = TextRenderer::elide_left(
            found ? core.resolved_path : std::string("not found"), available, scale * 0.9f);
        text.draw(quads, value, geometry.core_panel.x + label_width, core_pen_y, scale * 0.9f,
                  found ? theme.success : theme.danger);
        core_pen_y += line * 1.1f;
    }

    // BIOS file inventory.
    float pen_y = geometry.bios_panel.y;
    text.draw(quads, "DS BIOS and firmware files found in that folder", geometry.bios_panel.x, pen_y,
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
    const float guide_height = control_guide_height(scale * 0.9f);
    text.draw_wrapped(quads, footnote, output_width * 0.5f,
                      output_height - guide_height -
                          TextRenderer::wrapped_line_step(footnote_scale) *
                              (footnote_lines + 1.0f),
                      footnote_width, footnote_scale, theme.text_dim, TextAlign::Center);

    draw_control_guide(quads, text, glyphs, {{ButtonGlyph::B, "Back"}},
                       geometry.panel.x + geometry.panel.w,
                       output_height - line * 0.6f - guide_height, scale * 0.9f, theme.text,
                       TextAlign::Right);
}

}  // namespace ds
