#include "ui/settings_screen.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "core/libretro_core.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/page_frame.hpp"

namespace ds {
namespace {

// Said under the BIOS files, at the end of the page, where it scrolls with
// them rather than taking room from every page's content.
constexpr const char* kFootnote =
    "None of these ship with this app - they have to be your own dumps. DS games still boot "
    "without them, using the core's built-in BIOS replacement.";

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
    page_ = Page::Main;
    refresh_bios();
}

void SettingsScreen::set_melonds_page(bool deko, std::string path) {
    melonds_available_ = true;
    melonds_deko_ = deko;
    melonds_path_ = std::move(path);
}

int SettingsScreen::row_count() const {
    if (page_ == Page::MelonDs) return static_cast<int>(melon_rows().size());
    return kRowCount + (melonds_available_ ? 1 : 0);
}

std::vector<SettingsScreen::MelonRow> SettingsScreen::melon_rows() const {
    if (!melonds_deko_) return {MelonRow::CpuClock, MelonRow::GpuAlone};
    return {MelonRow::CpuClock, MelonRow::Jit,          MelonRow::FastMemory, MelonRow::Pipeline,
            MelonRow::GpuAlone, MelonRow::TextureCache, MelonRow::Shaders,    MelonRow::VramCheck};
}

void SettingsScreen::open_melonds_page() {
    // Read afresh each time, so a file edited by hand since is what shows.
    tuning_ = read_melonds_tuning(melonds_path_);
    melonds_error_.clear();
    page_ = Page::MelonDs;
    focus_ = 0;
    scroll_ = 0.0f;
}

std::string SettingsScreen::melon_label(MelonRow row) {
    switch (row) {
        case MelonRow::CpuClock: return "CPU clock while playing";
        case MelonRow::Jit: return "JIT recompiler";
        case MelonRow::FastMemory: return "JIT fast memory";
        case MelonRow::Pipeline: return "Each frame shown";
        case MelonRow::GpuAlone: return "App drawing finished first";
        case MelonRow::TextureCache: return "3D texture cache";
        case MelonRow::Shaders: return "GPU shaders";
        case MelonRow::VramCheck: return "Video memory check";
    }
    return std::string();
}

std::string SettingsScreen::melon_value(MelonRow row) const {
    const auto on_off = [](bool on) { return std::string(on ? "On" : "Off"); };
    switch (row) {
        case MelonRow::CpuClock: return std::to_string(tuning_.cpu_mhz) + " MHz";
        case MelonRow::Jit: return on_off(tuning_.jit);
        case MelonRow::FastMemory: return on_off(tuning_.fastmem);
        case MelonRow::Pipeline: return tuning_.pipeline ? "A frame later" : "As soon as drawn";
        case MelonRow::GpuAlone: return on_off(tuning_.gpu_alone);
        case MelonRow::TextureCache: return tuning_.texcache ? "On" : "Emptied every frame";
        case MelonRow::Shaders:
            if (tuning_.shaders.empty()) return "This build's";
            if (tuning_.shaders == "standalone") return "melonDS app's";
            return "From " + path_basename(tuning_.shaders);
        case MelonRow::VramCheck: return on_off(tuning_.vramcheck);
    }
    return std::string();
}

void SettingsScreen::adjust_melonds(MelonRow row, int direction) {
    switch (row) {
        case MelonRow::CpuClock: tuning_.cpu_mhz = next_melonds_cpu_mhz(tuning_.cpu_mhz, direction); break;
        case MelonRow::Jit: tuning_.jit = !tuning_.jit; break;
        case MelonRow::FastMemory: tuning_.fastmem = !tuning_.fastmem; break;
        case MelonRow::Pipeline: tuning_.pipeline = !tuning_.pipeline; break;
        case MelonRow::GpuAlone: tuning_.gpu_alone = !tuning_.gpu_alone; break;
        case MelonRow::TextureCache: tuning_.texcache = !tuning_.texcache; break;
        case MelonRow::Shaders:
            // This build's and the app's; one named by hand goes back to
            // this build's, and is written in by hand again if wanted.
            tuning_.shaders = tuning_.shaders.empty() ? "standalone" : std::string();
            break;
        case MelonRow::VramCheck: tuning_.vramcheck = !tuning_.vramcheck; break;
    }
    std::string error;
    if (write_melonds_tuning(tuning_, melonds_path_, &error)) {
        melonds_error_.clear();
    } else {
        melonds_error_ = "Could not save " + melonds_path_ + (error.empty() ? "" : ": " + error);
    }
}

std::string SettingsScreen::footnote() const {
    if (page_ != Page::MelonDs) return kFootnote;
    std::string note = "These take effect the next time a game starts. They are kept in " + melonds_path_ +
                       ", which can be edited by hand as well.";
    if (melonds_deko_) {
        note += " Apart from the CPU clock, they are for comparing a game here with melonDS's own app: "
                "the defaults are the fastest.";
    }
    return note;
}

void SettingsScreen::refresh_bios() { bios_ = check_bios_files(settings_.system_dir); }

SettingsScreen::Geometry SettingsScreen::compute_geometry(float output_width,
                                                          float output_height) const {
    Geometry geometry;
    // The same frame as every other page: see ui/page_frame.hpp. The panel
    // runs from the top margin to the bottom of the content area.
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    const float line = frame.line;
    geometry.panel = PixelRect{frame.panel.x, frame.panel.y, frame.panel.w,
                               frame.content_bottom - frame.panel.y};

    // The heading stays put; everything under it moves with the scroll.
    const float content_top = frame.content_top;
    geometry.view_top = content_top;
    const float rows_top = content_top - scroll_;
    const float row_height = menu_row_height(line);
    const float gap = menu_row_gap(line);
    // Short of the panel's right edge by the scroll bar, which runs beside
    // the rows rather than over them.
    const float row_width = geometry.panel.w - scroll_bar_width(geometry.scale) - line * 0.6f;
    const int rows = row_count();
    for (int index = 0; index < rows; ++index) {
        geometry.rows.push_back(PixelRect{
            geometry.panel.x,
            rows_top + index * (row_height + gap),
            row_width,
            row_height,
        });
    }
    geometry.view_height = geometry.panel.y + geometry.panel.h - content_top;
    const float rows_bottom = rows_top + rows * (row_height + gap);
    const float footnote_scale = geometry.scale * 0.85f;
    const std::string note = footnote();
    const float footnote_height =
        TextRenderer::wrapped_line_step(footnote_scale) *
        static_cast<float>(TextRenderer::wrap(note, geometry.panel.w, footnote_scale).size());

    if (page_ == Page::MelonDs) {
        // Its rows and what they are, and nothing else.
        geometry.footnote_top = rows_bottom + line * 0.4f;
        const float error_height = melonds_error_.empty() ? 0.0f : line * 1.4f;
        geometry.content_height =
            geometry.footnote_top + footnote_height + error_height + line * 0.4f - (content_top - scroll_);
        return geometry;
    }

    // The cores get their own block: a heading plus one line per console, so
    // the space it takes follows the list rather than a remembered count.
    const float core_top = rows_bottom + line;
    const float core_height = line * (1.6f + 1.1f * static_cast<float>(cores_.size()));
    geometry.core_panel = PixelRect{geometry.panel.x, core_top, geometry.panel.w, core_height};

    const float bios_top = core_top + core_height + line * 0.6f;
    // Exactly what is drawn there -- a heading and one line per file -- so
    // the page ends where its last line does and the scroll bar with it.
    const float bios_height = line * (1.6f + 1.1f * static_cast<float>(bios_.size()));
    geometry.bios_panel = PixelRect{geometry.panel.x, bios_top, geometry.panel.w, bios_height};
    geometry.footnote_top = bios_top + bios_height + line * 0.4f;
    // Measured from the top of the content rather than from the panel, so it
    // is the height of the thing being scrolled and not of the window onto it.
    geometry.content_height =
        geometry.footnote_top + footnote_height + line * 0.4f - (content_top - scroll_);
    return geometry;
}

void SettingsScreen::adjust(int direction, SettingsScreenResult& result) {
    if (page_ == Page::MelonDs) {
        const std::vector<MelonRow> rows = melon_rows();
        if (focus_ >= 0 && focus_ < static_cast<int>(rows.size())) {
            adjust_melonds(rows[static_cast<std::size_t>(focus_)], direction);
        }
        return;
    }
    if (melonds_available_ && focus_ == melonds_row()) {
        // A way in, not a value: forwards opens it, back does nothing.
        if (direction > 0) open_melonds_page();
        return;
    }
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
    const int rows = row_count();
    const std::size_t index = static_cast<std::size_t>(std::max(0, std::min(focus_, rows - 1)));
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
    if (focus_ == rows - 1) {
        scroll_ = geometry.max_scroll();
        return;
    }
    const PixelRect& row = geometry.rows[index];
    const float top = geometry.view_top;
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
            case UiAction::Down: focus_ = std::min(row_count() - 1, focus_ + 1); break;
            case UiAction::Left: adjust(-1, result); break;
            case UiAction::Right: adjust(1, result); break;
            case UiAction::Confirm:
                adjust(1, result);
                break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu:
                if (page_ == Page::MelonDs) {
                    // Back to Settings, on the row it was opened from.
                    page_ = Page::Main;
                    focus_ = melonds_row();
                } else {
                    result.close_requested = true;
                }
                break;
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
        const float view_top = geometry.view_top;
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

    const PageFrame frame = page_frame(output_width, output_height);
    // Over the paused game, dimmed, when opened from its menu; on the app's
    // own background otherwise, since there is nothing behind it to dim.
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame, page_ == Page::MelonDs ? "Settings \xC2\xB7 melonDS" : "Settings");

    // Everything below the heading scrolls, so everything below the heading is
    // clipped to the room there is for it. Without this a row pushed past the
    // bottom carries on being drawn over whatever is under the panel.
    const float view_top = geometry.view_top;
    quads.set_clip(PixelRect{geometry.panel.x, view_top, geometry.panel.w,
                             geometry.panel.y + geometry.panel.h - view_top});

    std::vector<std::array<std::string, 2>> rows;
    if (page_ == Page::MelonDs) {
        for (const MelonRow row : melon_rows()) rows.push_back({melon_label(row), melon_value(row)});
    }
    const std::string main_rows[kRowCount][2] = {
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
    if (page_ == Page::Main) {
        for (const auto& row : main_rows) rows.push_back({row[0], row[1]});
        if (melonds_available_) rows.push_back({"melonDS", ""});
    }

    for (int index = 0; index < static_cast<int>(rows.size()); ++index) {
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = (index == focus_);
        draw_menu_row(quads, row, focused, scale);

        // Label on the left, value on the right, both on one baseline. The
        // value is elided rather than allowed to run past the row, which is
        // what a long BIOS path would otherwise do.
        const float pad = menu_row_pad(line);
        const float baseline = page_row_text_y(row, scale);
        text.draw(quads, rows[index][0], row.x + pad, baseline, scale, menu_row_text(focused));
#ifndef __SWITCH__
        if (page_ == Page::Main && index == static_cast<int>(Row::ConfirmButtonRow)) {
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
            text.draw(quads, "/", pen_x, page_row_text_y(row, scale * 0.9f), scale * 0.9f,
                      value_color);
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
            text.draw(quads, value, row.x + row.w - pad, page_row_text_y(row, scale * 0.9f),
                      scale * 0.9f, value_color, TextAlign::Right);
        }
    }

    if (page_ == Page::MelonDs) {
        text.draw_wrapped(quads, footnote(), geometry.panel.x, geometry.footnote_top, geometry.panel.w,
                          scale * 0.85f, theme.text_dim);
        if (!melonds_error_.empty()) {
            const float note_height =
                TextRenderer::wrapped_line_step(scale * 0.85f) *
                static_cast<float>(TextRenderer::wrap(footnote(), geometry.panel.w, scale * 0.85f).size());
            text.draw(quads, TextRenderer::elide_right(melonds_error_, geometry.panel.w, scale * 0.85f),
                      geometry.panel.x, geometry.footnote_top + note_height + line * 0.4f, scale * 0.85f,
                      theme.danger);
        }
    }

    if (page_ == Page::Main) {
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

    text.draw_wrapped(quads, kFootnote, geometry.panel.x, geometry.footnote_top, geometry.panel.w,
                      scale * 0.85f, theme.text_dim);
    }

    // Everything above scrolled and was clipped to the panel. The prompts
    // below it are fixed to the window, so the clip comes off here.
    quads.clear_clip();

    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}});

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
