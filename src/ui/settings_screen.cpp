#include "ui/settings_screen.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "audio/ui_sounds.hpp"
#include "config/ds_profile.hpp"
#include "core/libretro_core.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/page_frame.hpp"

namespace ds {
namespace {

// Said under Vsync, which a device that only shows another device's game
// never uses: the picture it shows is paced by the stream, not by this.
constexpr const char* kVsyncDescription = "Applies when a game is running on this device.";

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
}

void SettingsScreen::set_melonds_page(bool deko, std::string path) {
    melonds_available_ = true;
    melonds_deko_ = deko;
    melonds_path_ = std::move(path);
}

int SettingsScreen::row_count() const {
    if (page_ == Page::Emulators) {
        int count = 0;
        for (const Entry& entry : entries_) count += entry.focusable() ? 1 : 0;
        return count;
    }
    return first_setting_row() + kRowCount + (has_emulators_page() ? 1 : 0) + (updates_row() >= 0 ? 1 : 0);
}

std::vector<SettingsScreen::Entry> SettingsScreen::emulator_entries() const {
    std::vector<Entry> entries;
    const auto add_melon_rows = [&] {
        for (const MelonRow row : melon_rows()) {
            Entry entry;
            entry.kind = Entry::Kind::Melon;
            entry.text = melon_label(row);
            entry.value = melon_value(row);
            entry.melon = row;
            entries.push_back(entry);
        }
    };
    const bool ds_listed = std::any_of(cores_.begin(), cores_.end(),
                                       [](const CoreStatus& core) { return core.system == SystemId::Ds; });
    if (melonds_available_ && !ds_listed) {
        entries.push_back(Entry{Entry::Kind::Heading, "Nintendo DS", "melonDS"});
        add_melon_rows();
    }
    for (const CoreStatus& core : cores_) {
        entries.push_back(Entry{Entry::Kind::Heading, core.system_name, core.emulator});
        const bool built_in = core.resolved_path == kBuiltInCorePath;
        const bool found = built_in || (!core.resolved_path.empty() && file_exists(core.resolved_path));
        Entry where{Entry::Kind::Location, "", "", found};
        where.text = built_in               ? std::string("Built into Dualscreen")
                     : found                ? core.resolved_path
                     : core.resolved_path.empty() ? std::string("Not found")
                                            : "Not found at " + core.resolved_path;
        entries.push_back(where);
        if (core.system == SystemId::Ds && melonds_available_) add_melon_rows();
        const auto download = download_rows_.find(core.system);
        if (download != download_rows_.end()) {
            Entry row{Entry::Kind::Download, download->second[0], download->second[1], true};
            row.system = core.system;
            entries.push_back(row);
        }
    }
    return entries;
}

const SettingsScreen::Entry* SettingsScreen::focused_entry() const {
    int index = 0;
    for (const Entry& entry : entries_) {
        if (!entry.focusable()) continue;
        if (index++ == focus_) return &entry;
    }
    return nullptr;
}

std::vector<SettingsScreen::MelonRow> SettingsScreen::melon_rows() const {
    // libretro's melonDS has no frames to hand over late: only the clock.
    if (!melonds_deko_) return {MelonRow::CpuClock};
    return {MelonRow::CpuClock, MelonRow::Pipeline};
}

void SettingsScreen::open_emulators_page() {
    // Read afresh each time, so a file edited by hand since is what shows.
    if (melonds_available_) tuning_ = read_melonds_tuning(melonds_path_);
    melonds_error_.clear();
    page_ = Page::Emulators;
    refresh_entries();
    focus_ = 0;
    scroll_ = 0.0f;
}

std::string SettingsScreen::melon_label(MelonRow row) {
    switch (row) {
        case MelonRow::CpuClock: return "CPU Clock While Playing";
        case MelonRow::Pipeline: return "Each Frame Shown";
    }
    return std::string();
}

std::string SettingsScreen::melon_value(MelonRow row) const {
    switch (row) {
        case MelonRow::CpuClock: return std::to_string(tuning_.cpu_mhz) + " MHz";
        case MelonRow::Pipeline: return tuning_.pipeline ? "A frame later" : "As soon as drawn";
    }
    return std::string();
}

void SettingsScreen::adjust_melonds(MelonRow row, int direction) {
    switch (row) {
        case MelonRow::CpuClock: tuning_.cpu_mhz = next_melonds_cpu_mhz(tuning_.cpu_mhz, direction); break;
        case MelonRow::Pipeline: tuning_.pipeline = !tuning_.pipeline; break;
    }
    save_melonds();
}

std::vector<std::string> SettingsScreen::focused_choices(int& current) const {
    current = -1;
    std::vector<std::string> choices;
    if (page_ == Page::Emulators) {
        const Entry* entry = focused_entry();
        if (entry == nullptr || entry->kind != Entry::Kind::Melon) return choices;
        if (entry->melon == MelonRow::CpuClock) {
            for (const int mhz : kMelonDsCpuMhz) {
                if (mhz == tuning_.cpu_mhz) current = static_cast<int>(choices.size());
                choices.push_back(std::to_string(mhz) + " MHz");
            }
        } else {
            choices = {"As soon as drawn", "A frame later"};
            current = tuning_.pipeline ? 1 : 0;
        }
        return choices;
    }
    const int index = focus_ - first_setting_row();
    if (index < 0 || index >= kRowCount) return choices;
    switch (static_cast<Row>(index)) {
#ifndef __SWITCH__
        case Row::PadGlyphs:
            for (int style = 0; style < static_cast<int>(PadGlyphStyle::Count); ++style) {
                choices.push_back(to_string(static_cast<PadGlyphStyle>(style)));
            }
            current = static_cast<int>(settings_.pad_glyphs);
            break;
        case Row::ConfirmButtonRow: {
            // Named as the chosen controller icons name the two buttons: the
            // right-hand one first, then the bottom one.
            const char* east = "A";
            const char* south = "B";
            switch (settings_.pad_glyphs) {
                case PadGlyphStyle::Xbox:
                    east = "B";
                    south = "A";
                    break;
                case PadGlyphStyle::PlayStation:
                    east = "Circle";
                    south = "Cross";
                    break;
                case PadGlyphStyle::Positional:
                    east = "Right button";
                    south = "Bottom button";
                    break;
                default: break;
            }
            // Drawn as the two buttons -- see open_choice() -- and named
            // for anything that reads them instead.
            choices = {std::string(east) + " / " + south, std::string(south) + " / " + east};
            current = settings_.confirm_button == ConfirmButton::South ? 1 : 0;
            break;
        }
#endif
        case Row::VideoHold:
            for (const video::VideoHold hold :
                 {video::VideoHold::Responsive, video::VideoHold::Balanced, video::VideoHold::Smooth}) {
                if (hold == settings_.video_hold) current = static_cast<int>(choices.size());
                choices.push_back(video::to_string(hold));
            }
            break;
        case Row::RunAheadDs:
            choices = {"Off", "1 frame", "2 frames"};
            current = static_cast<int>(settings_.run_ahead_ds);
            break;
        case Row::DsFavoriteColor:
            for (const DsFavoriteColor& color : kDsFavoriteColors) choices.push_back(color.name);
            current = settings_.ds_favorite_color;
            break;
        case Row::DsBirthday:
            // The month first; the day's choices follow -- see choose().
            choices.assign(kDsMonths.begin(), kDsMonths.end());
            current = settings_.ds_birth_month - 1;
            break;
#if DS_PLAYS_WII_U
        case Row::WiiUScreenAlone:
            for (const WiiUScreenAlone screen :
                 {WiiUScreenAlone::Auto, WiiUScreenAlone::Television, WiiUScreenAlone::GamePad}) {
                if (screen == settings_.wii_u_screen_alone) current = static_cast<int>(choices.size());
                choices.push_back(to_string(screen));
            }
            break;
        case Row::WiiUTvAudio:
            choices.assign(std::begin(kWiiUTvAudioChoices), std::end(kWiiUTvAudioChoices));
            current = std::clamp(settings_.wii_u_tv_audio, 0, 2);
            break;
#endif
        default: break;
    }
    return choices;
}

void SettingsScreen::choose(int index, SettingsScreenResult& result) {
    if (page_ == Page::Emulators) {
        const Entry* entry = focused_entry();
        if (entry == nullptr || entry->kind != Entry::Kind::Melon) return;
        if (entry->melon == MelonRow::CpuClock) {
            if (index < 0 || index >= static_cast<int>(std::size(kMelonDsCpuMhz))) return;
            tuning_.cpu_mhz = kMelonDsCpuMhz[index];
        } else {
            tuning_.pipeline = index == 1;
        }
        save_melonds();
        refresh_entries();
        return;
    }
    switch (static_cast<Row>(focus_ - first_setting_row())) {
#ifndef __SWITCH__
        case Row::PadGlyphs:
            if (index < 0 || index >= static_cast<int>(PadGlyphStyle::Count)) return;
            settings_.pad_glyphs = static_cast<PadGlyphStyle>(index);
            break;
        case Row::ConfirmButtonRow:
            if (index < 0 || index > 1) return;
            settings_.confirm_button = index == 1 ? ConfirmButton::South : ConfirmButton::East;
            break;
#endif
        case Row::VideoHold: {
            const video::VideoHold holds[] = {video::VideoHold::Responsive, video::VideoHold::Balanced,
                                              video::VideoHold::Smooth};
            if (index < 0 || index >= 3) return;
            settings_.video_hold = holds[index];
            break;
        }
        case Row::RunAheadDs:
            if (index < 0 || index >= 3) return;
            settings_.run_ahead_ds = static_cast<unsigned>(index);
            break;
        case Row::DsFavoriteColor:
            if (index < 0 || index >= static_cast<int>(kDsFavoriteColors.size())) return;
            settings_.ds_favorite_color = index;
            break;
        case Row::DsBirthday:
            if (birth_month_choosing_ == 0) {
                // The month: then which day of it, the one kept still chosen
                // where the month has it.
                if (index < 0 || index >= 12) return;
                const int month = index + 1;
                std::vector<std::string> days;
                for (int day = 1; day <= ds_days_in_month(month); ++day) days.push_back(std::to_string(day));
                const int current_day =
                    settings_.ds_birth_day >= 1 && settings_.ds_birth_day <= ds_days_in_month(month)
                        ? settings_.ds_birth_day - 1
                        : 0;
                birth_month_choosing_ = month;
                choice_.open(std::move(days), current_day, choice_row_);
                return;
            }
            if (index < 0 || index >= ds_days_in_month(birth_month_choosing_)) return;
            settings_.ds_birth_month = birth_month_choosing_;
            settings_.ds_birth_day = index + 1;
            birth_month_choosing_ = 0;
            break;
#if DS_PLAYS_WII_U
        case Row::WiiUScreenAlone: {
            const WiiUScreenAlone screens[] = {WiiUScreenAlone::Auto, WiiUScreenAlone::Television,
                                               WiiUScreenAlone::GamePad};
            if (index < 0 || index >= 3) return;
            settings_.wii_u_screen_alone = screens[index];
            break;
        }
        case Row::WiiUTvAudio:
            if (index < 0 || index >= 3) return;
            settings_.wii_u_tv_audio = index;
            break;
#endif
        default: return;
    }
    result.settings_changed = true;
}

void SettingsScreen::press_row(SettingsScreenResult& result, const Geometry& geometry) {
    if (page_ == Page::Main && focus_ == first_setting_row() + static_cast<int>(Row::DsNickname)) {
        const char* header = "Nickname";
        std::string typed;
        if (text_editor_) {
            // The platform's own keyboard, there and then.
            if (text_editor_(header, settings_.ds_nickname, kDsNicknameMax, typed)) {
                settings_.ds_nickname = typed;
                result.settings_changed = true;
            }
        } else {
            text_entry_.open(header, settings_.ds_nickname, kDsNicknameMax);
        }
        return;
    }
    int current = -1;
    std::vector<std::string> choices = focused_choices(current);
    if (choices.empty()) {
        adjust(1, result);
        return;
    }
    if (focus_ < 0 || focus_ >= static_cast<int>(geometry.rows.size())) return;
    choice_row_ = geometry.rows[static_cast<std::size_t>(focus_)];
    birth_month_choosing_ = 0;
    const int row = focus_ - first_setting_row();
    if (page_ == Page::Main && row == static_cast<int>(Row::DsFavoriteColor)) {
        // The DS's own sixteen, in its own order, four to a line as its
        // settings lay them out.
        std::vector<Color> colors;
        for (const DsFavoriteColor& color : kDsFavoriteColors) colors.push_back(Color{color.r, color.g, color.b, 1.0f});
        choice_.open_colors(std::move(colors), std::move(choices), 4, current, choice_row_);
        return;
    }
    choice_.open(std::move(choices), current, choice_row_);
#ifndef __SWITCH__
    if (page_ == Page::Main && row == static_cast<int>(Row::ConfirmButtonRow)) {
        // The two buttons, as the row shows them: yes, then no.
        choice_.set_glyph_pairs(
            {{confirm_glyph_for(ConfirmButton::East), cancel_glyph_for(ConfirmButton::East)},
             {confirm_glyph_for(ConfirmButton::South), cancel_glyph_for(ConfirmButton::South)}},
            glyph_aspect_);
    }
#endif
}

void SettingsScreen::save_melonds() {
    std::string error;
    if (write_melonds_tuning(tuning_, melonds_path_, &error)) {
        melonds_error_.clear();
    } else {
        melonds_error_ = "Could not save " + melonds_path_ + (error.empty() ? "" : ": " + error);
    }
}

void SettingsScreen::return_from_data_management() {
    page_ = Page::Main;
    focus_ = std::max(0, data_management_row());
    scroll_ = 0.0f;
}

std::string SettingsScreen::footnote() const {
    if (page_ != Page::Emulators || !melonds_available_) return std::string();
    std::string note = "melonDS's settings take effect the next time a game starts. They are kept in " +
                       melonds_path_ + ", which can be edited by hand as well.";
    if (melonds_deko_) {
        note += " Each frame shown a frame later lets the emulator run on while the GPU draws, which keeps "
                "busy 3D games at full speed; as soon as drawn shows each picture a frame sooner.";
    }
    return note;
}

const char* SettingsScreen::row_description(int index) const {
    if (page_ == Page::Main && index == first_setting_row() + static_cast<int>(Row::Vsync)) {
        return kVsyncDescription;
    }
    return nullptr;
}

float SettingsScreen::description_height(float scale) const {
    // Its own line, and room under it so it reads as the row above's.
    return TextRenderer::line_height(scale * 0.8f) + TextRenderer::line_height(scale) * 0.7f;
}

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
    geometry.view_height = geometry.panel.y + geometry.panel.h - content_top;
    const float top = content_top - scroll_;
    const float row_height = menu_row_height(line);
    const float gap = menu_row_gap(line);
    // Short of the panel's right edge by the scroll bar, which runs beside
    // the rows rather than over them.
    const float row_width = geometry.panel.w - scroll_bar_width(geometry.scale) - line * 0.6f;

    if (page_ == Page::Emulators) {
        // Each console's heading, where its emulator is, then its rows.
        float y = top + line * 0.5f;
        for (std::size_t index = 0; index < entries_.size(); ++index) {
            switch (entries_[index].kind) {
                case Entry::Kind::Heading:
                    if (index > 0) y += line * 0.9f;
                    geometry.texts.push_back(Geometry::Text{index, y});
                    y += line * 1.45f;
                    break;
                case Entry::Kind::Location:
                    geometry.texts.push_back(Geometry::Text{index, y});
                    y += line * 1.35f;
                    break;
                case Entry::Kind::Melon:
                case Entry::Kind::Download:
                    geometry.rows.push_back(PixelRect{geometry.panel.x, y, row_width, row_height});
                    y += row_height + gap;
                    break;
            }
        }
        const float footnote_scale = geometry.scale * 0.85f;
        const std::string note = footnote();
        const float footnote_height =
            note.empty() ? 0.0f
                         : TextRenderer::wrapped_line_step(footnote_scale) *
                               static_cast<float>(TextRenderer::wrap(note, geometry.panel.w, footnote_scale).size());
        geometry.footnote_top = y + line * 0.4f;
        const float error_height = melonds_error_.empty() ? 0.0f : line * 1.4f;
        geometry.content_height = geometry.footnote_top + footnote_height + error_height + line * 0.4f - top;
        return geometry;
    }

    float row_y = top;
    const int rows = row_count();
    for (int index = 0; index < rows; ++index) {
        geometry.rows.push_back(PixelRect{geometry.panel.x, row_y, row_width, row_height});
        row_y += row_height + gap;
        // A line of explanation under the row, in the gap before the next.
        if (row_description(index) != nullptr) row_y += description_height(geometry.scale);
    }
    // The page ends where the last row does, and the scroll bar with it.
    // Measured from the top of the content rather than from the panel, so it
    // is the height of the thing being scrolled and not of the window onto it.
    geometry.content_height = row_y + line * 0.4f - top;
    return geometry;
}

void SettingsScreen::adjust(int direction, SettingsScreenResult& result) {
    if (page_ == Page::Emulators) {
        const Entry* entry = focused_entry();
        if (entry == nullptr) return;
        if (entry->kind == Entry::Kind::Melon) {
            adjust_melonds(entry->melon, direction);
            refresh_entries();
        } else if (entry->kind == Entry::Kind::Download && direction > 0) {
            // Downloading or updating is the app's -- and asked first.
            result.open_download = entry->system;
        }
        return;
    }
    if (focus_ == emulators_row()) {
        // A way in, not a value: forwards opens it, back does nothing.
        if (direction > 0) open_emulators_page();
        return;
    }
    if (focus_ == data_management_row()) {
        if (direction > 0) result.open_data_management = true;
        return;
    }
    if (focus_ == updates_row()) {
        if (direction > 0) result.open_updates = true;
        return;
    }
    switch (static_cast<Row>(focus_ - first_setting_row())) {
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
        // A name is typed -- see press_row() -- and a color chosen.
        case Row::DsNickname:
        case Row::DsFavoriteColor:
        case Row::DsBirthday: break;
        case Row::Vsync:
            settings_.vsync = !settings_.vsync;
            result.settings_changed = true;
            break;
#if DS_PLAYS_WII_U
        case Row::WiiUScreenAlone:
            settings_.wii_u_screen_alone = next_wii_u_screen_alone(settings_.wii_u_screen_alone);
            result.settings_changed = true;
            break;
        case Row::PauseWiiUInMenu:
            settings_.pause_wii_u_in_menu = !settings_.pause_wii_u_in_menu;
            result.settings_changed = true;
            break;
        case Row::WiiUTvAudio:
            settings_.wii_u_tv_audio = ((settings_.wii_u_tv_audio + direction) % 3 + 3) % 3;
            result.settings_changed = true;
            break;
#endif
        case Row::UiSounds:
            settings_.ui_sounds = !settings_.ui_sounds;
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

SettingsScreenResult SettingsScreen::update(const std::vector<UiAction>& pressed,
                                            const std::vector<PointerEvent>& touched,
                                            float output_width,
                                            float output_height) {
    SettingsScreenResult result;
    if (!open_) {
        return result;
    }
    // What the Emulators page shows can change under it -- a download's
    // progress, say -- so it is read again each time.
    if (page_ == Page::Emulators) refresh_entries();
    // A tap on Back at the foot of the page is Back pressed.
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    take_guide_taps(guide_hits_, actions, pointers);

    const Geometry geometry = compute_geometry(output_width, output_height);

    // A name being typed takes every press, and what was typed.
    if (text_entry_.is_open()) {
        const TextEntryResult typed =
            text_entry_.update(actions, pointers, typed_, backspaces_, output_width, output_height);
        typed_.clear();
        backspaces_ = 0;
        if (typed.done) {
            // Spaces at either end are not part of a name.
            std::string name = text_entry_.text();
            const std::size_t first = name.find_first_not_of(' ');
            name = first == std::string::npos ? std::string()
                                              : name.substr(first, name.find_last_not_of(' ') - first + 1);
            if (name != settings_.ds_nickname) {
                settings_.ds_nickname = name;
                result.settings_changed = true;
            }
        }
        return result;
    }
    // A row's choices, up beside it, take every press until put away.
    if (choice_.is_open()) {
        const ChoicePopupResult chosen = choice_.update(actions, pointers, output_width, output_height);
        if (chosen.closed) birth_month_choosing_ = 0;
        if (chosen.chosen >= 0) choose(chosen.chosen, result);
        if (chosen.close_everything) {
            result.close_requested = true;
            open_ = false;
        }
        return result;
    }

    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(row_count() - 1, focus_ + 1); break;
            // Values are not stepped from the side any more: A opens a row's
            // choices, or flips its switch, or opens its page.
            case UiAction::Left: break;
            // A row that opens a page opens with A, never with a direction.
            case UiAction::Right: break;
            case UiAction::Confirm:
                press_row(result, geometry);
                break;
            case UiAction::Cancel:
            case UiAction::ToggleMenu:
                if (page_ == Page::Emulators) {
                    // Back to Settings, on the row it was opened from.
                    page_ = Page::Main;
                    focus_ = emulators_row();
                    scroll_ = 0.0f;
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
            // L and R jump nowhere: a shoulder is not a way through a list.
            case UiAction::PageUp:
            case UiAction::PageDown: break;
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
                press_row(result, settled);
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
    if (page_ == Page::Emulators) refresh_entries();
    draw_page_header(quads, text, frame, page_ == Page::Emulators ? "Settings \xC2\xB7 Emulators" : "Settings");

    // Everything below the heading scrolls, so everything below the heading is
    // clipped to the room there is for it. Without this a row pushed past the
    // bottom carries on being drawn over whatever is under the panel.
    const float view_top = geometry.view_top;
    quads.set_clip(PixelRect{geometry.panel.x, view_top, geometry.panel.w,
                             geometry.panel.y + geometry.panel.h - view_top});

    std::vector<std::array<std::string, 2>> rows;
    if (page_ == Page::Emulators) {
        for (const Entry& entry : entries_) {
            if (entry.focusable()) rows.push_back({entry.text, entry.value});
        }
    }
    const std::string main_rows[kRowCount][2] = {
#ifndef __SWITCH__
        {"Controller Icons", to_string(settings_.pad_glyphs)},
        // Drawn as the two buttons themselves, below.
        {"Confirm/Cancel", ""},
#endif
        {"Streaming Quality", video::to_string(settings_.video_hold)},
        {"Run Ahead to Hide a DS Game's Lag",
         settings_.run_ahead_ds == 0
             ? std::string("Off")
             : std::to_string(settings_.run_ahead_ds) +
                   (settings_.run_ahead_ds == 1 ? " frame" : " frames")},
        {"Nickname", settings_.ds_nickname.empty() ? std::string("Not set") : settings_.ds_nickname},
        {"Favorite Color",
         kDsFavoriteColors[static_cast<std::size_t>(
                               std::clamp(settings_.ds_favorite_color, 0,
                                          static_cast<int>(kDsFavoriteColors.size()) - 1))]
             .name},
        {"Birthday", settings_.ds_birth_month >= 1 && settings_.ds_birth_month <= 12
                         ? std::string(kDsMonths[static_cast<std::size_t>(settings_.ds_birth_month - 1)]) +
                               " " + std::to_string(settings_.ds_birth_day)
                         : std::string("Not set")},
        {"Vsync", settings_.vsync ? "On" : "Off"},
#if DS_PLAYS_WII_U
        {"Default Wii U Screen", to_string(settings_.wii_u_screen_alone)},
        {"Pause Wii U Games in the Quick Menu", settings_.pause_wii_u_in_menu ? "On" : "Off"},
        {"Wii U TV Audio", kWiiUTvAudioChoices[std::clamp(settings_.wii_u_tv_audio, 0, 2)]},
#endif
        {"Sound Effects", settings_.ui_sounds ? "On" : "Off"},
    };
    if (page_ == Page::Main) {
        if (offers_data_management_) rows.push_back({"Data Management", ""});
        for (const auto& row : main_rows) rows.push_back({row[0], row[1]});
        if (has_emulators_page()) rows.push_back({"Emulators", ""});
        if (updates_row() >= 0) rows.push_back({"Updates", updates_value_});
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
        if (const char* description = row_description(index)) {
            text.draw(quads, description, row.x + pad, row.y + row.h + menu_row_gap(line) * 0.3f,
                      scale * 0.8f, theme.text_dim);
        }
#ifndef __SWITCH__
        if (page_ == Page::Main && index == first_setting_row() + static_cast<int>(Row::ConfirmButtonRow)) {
            // The yes button, then the no one, as the prompts everywhere else
            // draw them -- so the row reads (A)/(B) or (B)/(A) in whichever
            // pad's symbols are chosen above, rather than a name for a
            // position that nobody reads a button by.
            const Color value_color = theme.text_dim;
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
        if (page_ == Page::Main && index == first_setting_row() + static_cast<int>(Row::DsFavoriteColor)) {
            // The color itself, unnamed, where a value goes.
            const DsFavoriteColor& color = kDsFavoriteColors[static_cast<std::size_t>(
                std::clamp(settings_.ds_favorite_color, 0, static_cast<int>(kDsFavoriteColors.size()) - 1))];
            const float dot = std::round(line * 1.3f);
            quads.draw_capsule(PixelRect{row.x + row.w - pad - dot, std::round(row.y + (row.h - dot) * 0.5f), dot, dot},
                               Color{color.r, color.g, color.b, 1.0f});
            continue;
        }
        // "Off" is also one of Run Ahead's three: a switch only where there
        // is nothing else to choose.
        const bool has_choices = page_ == Page::Main &&
                                 index == first_setting_row() + static_cast<int>(Row::RunAheadDs);
        if (is_toggle_value(rows[index][1]) && !has_choices) {
            draw_toggle(quads, row.x + row.w - pad, row.y + row.h * 0.5f, rows[index][1] == "On", line);
        } else if (!rows[index][1].empty()) {
            const Color value_color = theme.text_dim;
            const float label_width = TextRenderer::measure_width(rows[index][0], scale);
            const float available = row.w - label_width - pad * 4.0f;
            const std::string value =
                TextRenderer::elide_left(rows[index][1], available, scale * 0.9f);
            text.draw(quads, value, row.x + row.w - pad, page_row_text_y(row, scale * 0.9f),
                      scale * 0.9f, value_color, TextAlign::Right);
        }
    }

    if (page_ == Page::Emulators) {
        // Each console's heading -- its name, then its emulator's, dimmer --
        // and on the line under it where that emulator was found.
        for (const Geometry::Text& placed : geometry.texts) {
            const Entry& entry = entries_[placed.entry];
            if (entry.kind == Entry::Kind::Heading) {
                text.draw(quads, entry.text, geometry.panel.x, placed.y, scale, theme.text);
                if (!entry.value.empty()) {
                    const float name_width = TextRenderer::measure_width(entry.text + "  ", scale);
                    text.draw(quads, entry.value, geometry.panel.x + name_width, placed.y, scale,
                              theme.text_dim);
                }
            } else {
                text.draw(quads, TextRenderer::elide_left(entry.text, geometry.panel.w, scale * 0.85f),
                          geometry.panel.x, placed.y, scale * 0.85f, theme.text_dim);
            }
        }
        const std::string note = footnote();
        if (!note.empty()) {
            text.draw_wrapped(quads, note, geometry.panel.x, geometry.footnote_top, geometry.panel.w,
                              scale * 0.85f, theme.text_dim);
        }
        if (!melonds_error_.empty()) {
            const float note_height =
                note.empty() ? 0.0f
                             : TextRenderer::wrapped_line_step(scale * 0.85f) *
                                   static_cast<float>(TextRenderer::wrap(note, geometry.panel.w, scale * 0.85f).size());
            text.draw(quads, TextRenderer::elide_right(melonds_error_, geometry.panel.w, scale * 0.85f),
                      geometry.panel.x, geometry.footnote_top + note_height + line * 0.4f, scale * 0.85f,
                      theme.danger);
        }
    }

    // Everything above scrolled and was clipped to the panel. The prompts
    // below it are fixed to the window, so the clip comes off here.
    quads.clear_clip();

    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                    &guide_hits_);

    // Something has to say there is more, or a list that stops at the bottom
    // edge looks like a list that ends there.
    if (geometry.max_scroll() > 0.5f) {
        const float view = geometry.panel.y + geometry.panel.h - view_top;
        const float span = std::max(view, geometry.content_height);
        draw_scroll_bar(quads, PixelRect{geometry.panel.x, view_top, geometry.panel.w, view},
                        scroll_ / span, view / span, line, scale);
    }
    glyph_aspect_ = glyphs.width_for(1.0f);
    choice_.render(quads, text, output_width, output_height, &glyphs);
    text_entry_.render(quads, text, output_width, output_height);
}

void SettingsScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(page_));
#ifndef __SWITCH__
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.pad_glyphs));
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.confirm_button));
#endif
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.video_hold));
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.run_ahead_ds));
    mix_sound_state(view.rest, settings_.vsync ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.wii_u_screen_alone));
    mix_sound_state(view.rest, settings_.pause_wii_u_in_menu ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.wii_u_tv_audio));
    mix_sound_state(view.rest, settings_.ui_sounds ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(tuning_.cpu_mhz));
    mix_sound_state(view.rest, choice_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, text_entry_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, settings_.ds_nickname);
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.ds_favorite_color + 1));
    if (choice_.is_open()) mix_sound_state(view.focus, static_cast<uint64_t>(choice_.focus() + 1000));
    mix_sound_state(view.rest, tuning_.pipeline ? 1u : 0u);
}

}  // namespace ds
