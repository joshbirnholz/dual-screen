#include "ui/settings_screen.hpp"

#include <algorithm>
#include <cmath>

#include "core/libretro_core.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"

namespace ds {
namespace {

// Said under everything, below the list rather than over it.
constexpr const char* kFootnote =
    "None of these ship with this app - they have to be your own dumps. DS games still boot "
    "without them, using the core's built-in BIOS replacement.";

// Where the footnote starts: the list ends above it.
float footnote_top(float output_height, float width, float scale) {
    const float footnote_scale = scale * 0.85f;
    const std::size_t lines = TextRenderer::wrap(kFootnote, width, footnote_scale).size();
    return output_height - control_guide_height(scale * 0.9f) -
           TextRenderer::wrapped_line_step(footnote_scale) * (static_cast<float>(lines) + 1.0f);
}

}  // namespace

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
    scroll_ = 0.0f;
    refresh_bios();
}

void SettingsScreen::refresh_bios() { bios_ = check_bios_files(settings_.system_dir); }

SettingsScreen::Geometry SettingsScreen::compute_geometry(float output_width,
                                                          float output_height) const {
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(geometry.scale);

    const float margin = std::round(output_width * 0.07f);
    const float panel_top = std::round(output_height * 0.08f);
    const float panel_bottom =
        footnote_top(output_height, output_width - margin * 2.0f, geometry.scale) - line * 0.6f;
    geometry.panel = PixelRect{margin, panel_top, output_width - margin * 2.0f,
                               panel_bottom - panel_top};

    // The heading stays put; everything under it moves with the scroll.
    const float content_top = geometry.panel.y + line * 4.2f;
    const float rows_top = content_top - scroll_;
    const float row_height = menu_row_height(line);
    const float gap = menu_row_gap(line);
    // Short of the panel's right edge by the scroll bar, which runs beside
    // the rows rather than over them.
    const float row_width = geometry.panel.w - scroll_bar_width(geometry.scale) - line * 0.6f;
    for (int index = 0; index < kRowCount; ++index) {
        geometry.rows.push_back(PixelRect{
            geometry.panel.x,
            rows_top + index * (row_height + gap),
            row_width,
            row_height,
        });
    }
    geometry.view_height = geometry.panel.y + geometry.panel.h - content_top;

    // The cores get their own block: a heading plus one line per console, so
    // the space it takes follows the list rather than a remembered count.
    const float core_top = rows_top + kRowCount * (row_height + gap) + line;
    const float core_height = line * (1.6f + 1.1f * static_cast<float>(cores_.size()));
    geometry.core_panel = PixelRect{geometry.panel.x, core_top, geometry.panel.w, core_height};

    const float bios_top = core_top + core_height + line * 0.6f;
    // Exactly what is drawn there -- a heading and one line per file -- so
    // the page ends where its last line does and the scroll bar with it.
    const float bios_height = line * (1.6f + 1.1f * static_cast<float>(bios_.size()));
    geometry.bios_panel = PixelRect{geometry.panel.x, bios_top, geometry.panel.w, bios_height};
    // Measured from the top of the content rather than from the panel, so it
    // is the height of the thing being scrolled and not of the window onto it.
    geometry.content_height = bios_top + bios_height + line * 0.6f - (content_top - scroll_);
    return geometry;
}

void SettingsScreen::adjust(int direction, SettingsScreenResult& result) {
    switch (static_cast<Row>(focus_)) {
#ifndef __SWITCH__
        case Row::PadGlyphs: {
            // Three values, so this one steps rather than flips -- and it
            // steps the way the cursor was pushed.
            const int count = static_cast<int>(PadGlyphStyle::Count);
            const int here = static_cast<int>(settings_.pad_glyphs);
            settings_.pad_glyphs =
                static_cast<PadGlyphStyle>(((here + direction) % count + count) % count);
            result.settings_changed = true;
            break;
        }
        case Row::ConfirmButtonRow:
            // Two values, so either direction is the other one.
            settings_.confirm_button = settings_.confirm_button == ConfirmButton::East
                                           ? ConfirmButton::South
                                           : ConfirmButton::East;
            result.settings_changed = true;
            break;
#endif
        case Row::VideoHold:
            settings_.video_hold = video::next_video_hold(settings_.video_hold);
            result.settings_changed = true;
            break;
        case Row::RunAheadDs:
            // Three values, stepped the way the cursor was pushed.
            settings_.run_ahead_ds = (settings_.run_ahead_ds + (direction < 0 ? 2 : 1)) % 3;
            result.settings_changed = true;
            break;
        case Row::Vsync:
            settings_.vsync = !settings_.vsync;
            result.settings_changed = true;
            break;
        case Row::WiiUScreenAlone:
            settings_.wii_u_screen_alone = next_wii_u_screen_alone(settings_.wii_u_screen_alone);
            result.settings_changed = true;
            break;
        case Row::Count:
            break;
    }
}

void SettingsScreen::scroll_focus_into_view(const Geometry& geometry) {
    const std::size_t index = static_cast<std::size_t>(
        std::max(0, std::min(focus_, kRowCount - 1)));
    if (index >= geometry.rows.size()) {
        return;
    }
    // The first row shows the top of the page, and the last the bottom of it
    // -- the emulators and BIOS files under the rows -- so the cursor alone
    // reaches everything, and the scroll bar ends where the page does.
    if (focus_ == 0) {
        scroll_ = 0.0f;
        return;
    }
    if (focus_ == kRowCount - 1) {
        scroll_ = geometry.max_scroll();
        return;
    }
    const PixelRect& row = geometry.rows[index];
    const float top = geometry.panel.y + TextRenderer::line_height(geometry.scale) * 4.2f;
    const float bottom = geometry.panel.y + geometry.panel.h;
    if (row.y < top) {
        scroll_ -= top - row.y;
    } else if (row.y + row.h > bottom) {
        scroll_ += row.y + row.h - bottom;
    }
    scroll_ = std::max(0.0f, std::min(scroll_, geometry.max_scroll()));
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
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(kRowCount - 1, focus_ + 1); break;
            case UiAction::Left: adjust(-1, result); break;
            case UiAction::Right: adjust(1, result); break;
            case UiAction::Confirm:
                adjust(1, result);
                break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu: result.close_requested = true; break;
            // A menu's face buttons. Nothing here answers them, and a bare
            // fallthrough into Quit would make pressing X leave the screen.
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            // Already here, which is the whole of what it asks for.
            case UiAction::MenuSettings: break;
            // Escape also raises Cancel, which is what it means here.
            case UiAction::OpenMenu: break;
            case UiAction::Quit: break;
            case UiAction::ToggleFilter:  // handled by the app, wherever you are
            case UiAction::SwapLayout:    // nothing here is a layout
            // There is more here than fits, so these move the view rather
            // than doing nothing.
            case UiAction::PageUp: scroll_ -= geometry.view_height * 0.8f; break;
            case UiAction::PageDown: scroll_ += geometry.view_height * 0.8f; break;
        }
    }

    // The cursor and the view are settled before anything is hit-tested: an
    // action may have moved either, and a row cannot be tapped where it was.
    scroll_ = std::max(0.0f, std::min(scroll_, geometry.max_scroll()));
    // The cursor brings the view with it when it is moved -- and only then,
    // so a page dragged by a finger stays where the finger left it.
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(compute_geometry(output_width, output_height));
    }
    {
        const float view_top = geometry.panel.y + TextRenderer::line_height(geometry.scale) * 4.2f;
        drag_.update(pointers,
                     PixelRect{geometry.panel.x, view_top, geometry.panel.w,
                               geometry.panel.y + geometry.panel.h - view_top},
                     scroll_, geometry.max_scroll());
    }
    const Geometry settled = compute_geometry(output_width, output_height);

    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released || drag_.release_was_drag()) continue;
        for (std::size_t index = 0; index < settled.rows.size(); ++index) {
            // A first tap highlights a row; a second changes its value.
            if (tapped(pointer, settled.rows[index]) &&
                tap_chooses(focus_, static_cast<int>(index))) {
                adjust(1, result);
            }
        }
    }
    drag_.settle();

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

    // Over the paused game, dimmed, when opened from its menu; on the app's
    // own background otherwise, since there is nothing behind it to dim.
    quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                     over_game_ ? theme.scrim : theme.background);
    text.draw(quads, "Settings", geometry.panel.x, geometry.panel.y, scale * 1.6f, theme.text);
    text.draw(quads, "Left/Right changes a value.   Paths are edited in settings.json",
              geometry.panel.x, geometry.panel.y + line * 2.3f, scale * 0.9f, theme.text_dim);

    // Everything below the heading scrolls, so everything below the heading is
    // clipped to the room there is for it. Without this a row pushed past the
    // bottom carries on being drawn over whatever is under the panel.
    const float view_top = geometry.panel.y + line * 4.2f;
    quads.set_clip(PixelRect{geometry.panel.x, view_top, geometry.panel.w,
                             geometry.panel.y + geometry.panel.h - view_top});

    const std::string rows[kRowCount][2] = {
#ifndef __SWITCH__
        {"Controller icons", to_string(settings_.pad_glyphs)},
        // Drawn as the two buttons themselves, below.
        {"Confirm/Cancel", ""},
#endif
        {"Streaming quality", video::to_string(settings_.video_hold)},
        {"Run ahead to hide a DS game's lag",
         settings_.run_ahead_ds == 0
             ? std::string("Off")
             : std::to_string(settings_.run_ahead_ds) +
                   (settings_.run_ahead_ds == 1 ? " frame" : " frames")},
        {"Vsync", settings_.vsync ? "On" : "Off"},
        {"This device alone is a Wii U's",
         to_string(settings_.wii_u_screen_alone)},
    };

    for (int index = 0; index < kRowCount; ++index) {
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = (index == focus_);
        draw_menu_row(quads, row, focused, scale);

        // Label on the left, value on the right, both on one baseline. The
        // value is elided rather than allowed to run past the row, which is
        // what a long BIOS path would otherwise do.
        const float pad = menu_row_pad(line);
        const float baseline = row.y + (row.h - line) * 0.5f;
        text.draw(quads, rows[index][0], row.x + pad, baseline, scale, menu_row_text(focused));
#ifndef __SWITCH__
        if (index == static_cast<int>(Row::ConfirmButtonRow)) {
            // The yes button, then the no one, as the prompts everywhere else
            // draw them -- so the row reads (A)/(B) or (B)/(A) in whichever
            // pad's symbols are chosen above, rather than a name for a
            // position that nobody reads a button by.
            const Color value_color = focused ? theme.panel : theme.text_dim;
            const float height = line * 1.5f;
            const float glyph_width = glyphs.width_for(height);
            const float slash_width = TextRenderer::measure_width("/", scale * 0.9f);
            const float gap = line * 0.15f;
            // The button itself centred on the row, not the box it is drawn
            // in, which has headroom above it.
            const float glyph_y = row.y + row.h * 0.5f - height * glyphs.button_centre();
            float pen_x = row.x + row.w - pad - (glyph_width * 2.0f + slash_width + gap * 2.0f);
            glyphs.draw(quads, confirm_glyph_for(settings_.confirm_button), pen_x, glyph_y, height,
                        value_color);
            pen_x += glyph_width + gap;
            text.draw(quads, "/", pen_x, baseline, scale * 0.9f, value_color);
            pen_x += slash_width + gap;
            glyphs.draw(quads, cancel_glyph_for(settings_.confirm_button), pen_x, glyph_y, height,
                        value_color);
            continue;
        }
#endif
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
        const bool found = core.resolved_path == kBuiltInCorePath ||
                           (!core.resolved_path.empty() && file_exists(core.resolved_path));
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
    text.draw(quads, "DS BIOS and firmware files", geometry.bios_panel.x, pen_y,
              scale, theme.text);
    pen_y += line * 1.6f;

    for (const BiosFileStatus& status : bios_) {
        const float x = geometry.bios_panel.x;
        const std::string mark = status.present ? "[found]   " : "[missing] ";
        const Color color = status.present ? theme.success
                                           : (status.required ? theme.danger : theme.text_dim);
        text.draw(quads, mark + status.filename, x, pen_y, scale * 0.9f, color);
        pen_y += line * 1.1f;
    }

    // Everything above scrolled and was clipped to the panel. The footnote and
    // the prompts below it are fixed to the window, so the clip comes off
    // here -- inside it they would be clipped away with whatever else had
    // fallen off the bottom.
    quads.clear_clip();

    const float footnote_scale = scale * 0.85f;
    const float guide_height = control_guide_height(scale * 0.9f);
    text.draw_wrapped(quads, kFootnote, output_width * 0.5f,
                      footnote_top(output_height, geometry.panel.w, scale), geometry.panel.w,
                      footnote_scale, theme.text_dim, TextAlign::Center);

    draw_control_guide(quads, text, glyphs, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                       geometry.panel.x + geometry.panel.w,
                       output_height - line * 0.6f - guide_height, scale * 0.9f, theme.text,
                       TextAlign::Right);

    // Something has to say there is more, or a list that stops at the bottom
    // edge looks like a list that ends there.
    if (geometry.max_scroll() > 0.5f) {
        const float view = geometry.panel.y + geometry.panel.h - view_top;
        const float span = std::max(view, geometry.content_height);
        draw_scroll_bar(quads, PixelRect{geometry.panel.x, view_top, geometry.panel.w, view},
                        scroll_ / span, view / span, line, scale);
    }
}

}  // namespace ds
