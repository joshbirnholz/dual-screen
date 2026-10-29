#include "ui/settings_screen.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>

#include "audio/ui_sounds.hpp"
#include "config/ds_profile.hpp"
#include "core/libretro_core.hpp"
#if DS_REMOTE_START_HOST
#include "launch/launch_host.hpp"
#include "net/launch_service.hpp"
#endif
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/page_frame.hpp"

namespace ds {
namespace {

// Settings::wii_remote_rumble, as the row says it.
std::string rumble_label(int percent) {
    if (percent <= 0) return "Off";
    if (percent <= 25) return "Weak";
    if (percent <= 50) return "Normal";
    if (percent <= 75) return "Strong";
    return "Strongest";
}

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

const char* settings_group_name(SettingsGroup group) {
    switch (group) {
        case SettingsGroup::General: return "General";
        case SettingsGroup::Consoles: return "DS & 3DS";
        case SettingsGroup::WiiU: return "Wii U";
        case SettingsGroup::System: return "System";
        case SettingsGroup::Count: break;
    }
    return "";
}

void SettingsScreen::open(const Settings& settings, const std::vector<CoreStatus>& cores) {
    open_ = true;
    group_ = 0;
    in_group_ = false;
    focus_ = 0;
    settings_ = settings;
    cores_ = cores;
    scroll_ = 0.0f;
    asking_remote_start_ = false;
    notice_.clear();
    // Read afresh each time, so a file edited by hand since is what shows.
    if (melonds_available_) tuning_ = read_melonds_tuning(melonds_path_);
    melonds_error_.clear();
#if DS_REMOTE_START_HOST
    remote_start_on_ = service_starts_at_login();
#endif
    refresh_items();
}

void SettingsScreen::toggle_remote_start() {
#if DS_REMOTE_START_HOST
    std::string error;
    const bool on = !remote_start_on_;
    if (!set_service_starts_at_login(on, &error)) {
        notice_ = std::string("Remote Start could not be ") + (on ? "enabled" : "disabled") + ": " + error + ".";
        alert_cursor_.reset(1, 0);
        return;
    }
    remote_start_on_ = on;
    // Off forgets every device let in, so enabling it again lets in only
    // the ones that join a game here again.
    if (!on) net::RemoteStartRecord(remote_start_record()).forget_devices();
#endif
}

std::vector<PopupButton> SettingsScreen::remote_start_buttons() const {
    return {{"Cancel", ""}, {remote_start_on_ ? "Disable" : "Enable", ""}};
}

PopupText SettingsScreen::remote_start_text() const {
    PopupText words;
    if (remote_start_on_) {
        words.message = "Remote Start will be disabled, and its background service will be removed from this "
                        "computer.";
        words.lines.push_back("Other devices will no longer be able to start games here. To let one again, enable "
                              "Remote Start and join a game here from it once.");
        return words;
    }
    words.message = "Remote Start will install a small background service on this computer.";
    words.lines.push_back("The service will start each time you log in, in Game Mode too, and keep running with "
                          "Dualscreen closed.");
    words.lines.push_back("Any device that joins a game here will then show the games you play here on its home "
                          "screen, and can start one here from there, waking this computer first if it is asleep "
                          "and Wake-on-LAN is on.");
    words.lines.push_back("Disable Remote Start will remove the service.");
    return words;
}

PopupGeometry SettingsScreen::remote_start_geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height, popup_text_height(remote_start_text(), width, output_height),
                          remote_start_buttons());
}

PopupGeometry SettingsScreen::notice_geometry(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height,
                          popup_text_height(PopupText{notice_, {}, 0}, width, output_height), {{"OK", ""}});
}

void SettingsScreen::set_melonds_page(bool deko, std::string path) {
    melonds_available_ = true;
    melonds_deko_ = deko;
    melonds_path_ = std::move(path);
}

std::string SettingsScreen::row_label(Row row) const {
    switch (row) {
#ifndef __SWITCH__
        case Row::PadGlyphs: return "Controller Icons";
        case Row::ConfirmButtonRow: return "Confirm/Cancel";
#endif
        case Row::VideoHold: return "Streaming Quality";
        case Row::RunAheadDs: return "Run Ahead";
        case Row::DsNickname: return "Nickname";
        case Row::DsFavoriteColor: return "Favorite Color";
        case Row::DsBirthMonth: return "Birth Month";
        case Row::DsBirthDay: return "Birth Day";
        case Row::Vsync: return "Vsync";
#if DS_PLAYS_WII_U
        case Row::WiiUScreenAlone: return "Default Wii U Screen";
        case Row::PauseWiiUInMenu: return "Pause Wii U Games in the Quick Menu";
        case Row::WiiUTvAudio: return "Wii U TV Audio";
        case Row::WiiRemoteRumble: return "Wii Remote Rumble Strength";
        case Row::DiscordStatus: return "Discord Rich Presence";
#endif
#if DS_REMOTE_START_HOST
        case Row::RemoteStart: return remote_start_on_ ? "Disable Remote Start" : "Enable Remote Start";
#endif
        case Row::UiSounds: return "Sound Effects";
        case Row::Count: break;
    }
    return std::string();
}

std::string SettingsScreen::row_value(Row row) const {
    switch (row) {
#ifndef __SWITCH__
        case Row::PadGlyphs: return to_string(settings_.pad_glyphs);
        // Drawn as the two buttons themselves -- see render().
        case Row::ConfirmButtonRow: return std::string();
#endif
        case Row::VideoHold: return video::to_string(settings_.video_hold);
        case Row::RunAheadDs:
            return settings_.run_ahead_ds == 0
                       ? std::string("Off")
                       : std::to_string(settings_.run_ahead_ds) + (settings_.run_ahead_ds == 1 ? " frame" : " frames");
        case Row::DsNickname: return settings_.ds_nickname.empty() ? std::string("Not set") : settings_.ds_nickname;
        case Row::DsFavoriteColor:
            return kDsFavoriteColors[static_cast<std::size_t>(std::clamp(
                                         settings_.ds_favorite_color, 0, static_cast<int>(kDsFavoriteColors.size()) - 1))]
                .name;
        case Row::DsBirthMonth:
            return settings_.ds_birth_month >= 1 && settings_.ds_birth_month <= 12
                       ? std::string(kDsMonths[static_cast<std::size_t>(settings_.ds_birth_month - 1)])
                       : std::string("Not set");
        case Row::DsBirthDay:
            return settings_.ds_birth_day >= 1 ? std::to_string(settings_.ds_birth_day) : std::string("Not set");
        case Row::Vsync: return settings_.vsync ? "On" : "Off";
#if DS_PLAYS_WII_U
        case Row::WiiUScreenAlone: return to_string(settings_.wii_u_screen_alone);
        case Row::PauseWiiUInMenu: return settings_.pause_wii_u_in_menu ? "On" : "Off";
        case Row::WiiUTvAudio: return kWiiUTvAudioChoices[std::clamp(settings_.wii_u_tv_audio, 0, 2)];
        case Row::WiiRemoteRumble: return rumble_label(settings_.wii_remote_rumble);
        case Row::DiscordStatus: return settings_.discord_status ? "On" : "Off";
#endif
#if DS_REMOTE_START_HOST
        case Row::RemoteStart: return std::string();
#endif
        case Row::UiSounds: return settings_.ui_sounds ? "On" : "Off";
        case Row::Count: break;
    }
    return std::string();
}

std::vector<SettingsScreen::Item> SettingsScreen::group_items(SettingsGroup group) const {
    std::vector<Item> items;
    const auto add = [&](Row row) {
        Item item;
        item.kind = Item::Kind::Setting;
        item.row = row;
        item.text = row_label(row);
        item.value = row_value(row);
        items.push_back(item);
    };
    const auto add_melon_rows = [&] {
        for (const MelonRow row : melon_rows()) {
            Item item;
            item.kind = Item::Kind::Melon;
            item.text = melon_label(row);
            item.value = melon_value(row);
            item.melon = row;
            items.push_back(item);
        }
    };
    // A console's emulator: its heading, where it was found, its own
    // settings, and the row that downloads it.
    const auto add_core = [&](const CoreStatus& core) {
        items.push_back(Item{Item::Kind::Heading, Row::Count, core.system_name, core.emulator});
        const bool built_in = core.resolved_path == kBuiltInCorePath;
        const bool found = built_in || (!core.resolved_path.empty() && file_exists(core.resolved_path));
        Item where{Item::Kind::Location};
        where.found = found;
        where.text = built_in                     ? std::string("Built into Dualscreen")
                     : found                      ? core.resolved_path
                     : core.resolved_path.empty() ? std::string("Not found")
                                                  : "Not found at " + core.resolved_path;
        items.push_back(where);
        if (core.system == SystemId::Ds && melonds_available_) add_melon_rows();
        const auto download = download_rows_.find(core.system);
        if (download != download_rows_.end()) {
            Item row{Item::Kind::Download, Row::Count, download->second[0], download->second[1], true};
            row.system = core.system;
            items.push_back(row);
        }
    };
    switch (group) {
        case SettingsGroup::General:
#ifndef __SWITCH__
            add(Row::PadGlyphs);
            add(Row::ConfirmButtonRow);
#endif
            add(Row::VideoHold);
            add(Row::Vsync);
            add(Row::UiSounds);
            break;
        case SettingsGroup::Consoles: {
            add(Row::DsNickname);
            add(Row::DsFavoriteColor);
            add(Row::DsBirthMonth);
            add(Row::DsBirthDay);
            add(Row::RunAheadDs);
            const bool ds_listed = std::any_of(cores_.begin(), cores_.end(),
                                               [](const CoreStatus& core) { return core.system == SystemId::Ds; });
            if (melonds_available_ && !ds_listed) {
                items.push_back(Item{Item::Kind::Heading, Row::Count, "Nintendo DS", "melonDS"});
                add_melon_rows();
            }
            for (const CoreStatus& core : cores_) {
                if (core.system != SystemId::WiiU) add_core(core);
            }
            break;
        }
        case SettingsGroup::WiiU:
#if DS_PLAYS_WII_U
            add(Row::WiiUScreenAlone);
            add(Row::PauseWiiUInMenu);
            add(Row::WiiUTvAudio);
            add(Row::WiiRemoteRumble);
#endif
            for (const CoreStatus& core : cores_) {
                if (core.system == SystemId::WiiU) add_core(core);
            }
            break;
        case SettingsGroup::System:
#if DS_PLAYS_WII_U
            add(Row::DiscordStatus);
#endif
#if DS_REMOTE_START_HOST
            add(Row::RemoteStart);
#endif
            if (offers_data_management_) items.push_back(Item{Item::Kind::DataManagement, Row::Count, "Data Management"});
            if (offers_updates_) {
                items.push_back(Item{Item::Kind::Updates, Row::Count, "Updates", updates_value_});
            }
            break;
        case SettingsGroup::Count: break;
    }
    return items;
}

std::vector<SettingsGroup> SettingsScreen::groups() const {
    std::vector<SettingsGroup> shown;
    for (int index = 0; index < static_cast<int>(SettingsGroup::Count); ++index) {
        const SettingsGroup group = static_cast<SettingsGroup>(index);
        const std::vector<Item> items = group_items(group);
        if (std::any_of(items.begin(), items.end(), [](const Item& item) { return item.focusable(); })) {
            shown.push_back(group);
        }
    }
    return shown;
}

SettingsGroup SettingsScreen::group() const {
    const std::vector<SettingsGroup> shown = groups();
    if (shown.empty()) return SettingsGroup::General;
    return shown[static_cast<std::size_t>(std::clamp(group_, 0, static_cast<int>(shown.size()) - 1))];
}

int SettingsScreen::row_count() const {
    int count = 0;
    for (const Item& item : items_) count += item.focusable() ? 1 : 0;
    return count;
}

const SettingsScreen::Item* SettingsScreen::focused_item() const {
    if (!in_group_) return nullptr;
    int index = 0;
    for (const Item& item : items_) {
        if (!item.focusable()) continue;
        if (index++ == focus_) return &item;
    }
    return nullptr;
}

SettingsScreen::Row SettingsScreen::focused_row() const {
    const Item* item = focused_item();
    return item != nullptr && item->kind == Item::Kind::Setting ? item->row : Row::Count;
}

std::vector<SettingsScreen::MelonRow> SettingsScreen::melon_rows() const {
    // libretro's melonDS has no frames to hand over late: only the clock.
    if (!melonds_deko_) return {MelonRow::CpuClock};
    return {MelonRow::CpuClock, MelonRow::Pipeline};
}

void SettingsScreen::show_group(int index) {
    if (index != group_) scroll_ = 0.0f;
    group_ = index;
    in_group_ = false;
    focus_ = 0;
    refresh_items();
}

void SettingsScreen::open_group(SettingsGroup group) {
    const std::vector<SettingsGroup> shown = groups();
    const auto found = std::find(shown.begin(), shown.end(), group);
    if (found == shown.end()) return;
    show_group(static_cast<int>(found - shown.begin()));
    in_group_ = row_count() > 0;
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

void SettingsScreen::adjust_melonds(MelonRow row) {
    switch (row) {
        case MelonRow::CpuClock: tuning_.cpu_mhz = next_melonds_cpu_mhz(tuning_.cpu_mhz, 1); break;
        case MelonRow::Pipeline: tuning_.pipeline = !tuning_.pipeline; break;
    }
    save_melonds();
}

std::vector<std::string> SettingsScreen::focused_choices(int& current) const {
    current = -1;
    std::vector<std::string> choices;
    const Item* item = focused_item();
    if (item == nullptr) return choices;
    if (item->kind == Item::Kind::Melon) {
        if (item->melon == MelonRow::CpuClock) {
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
    if (item->kind != Item::Kind::Setting) return choices;
    switch (item->row) {
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
                case PadGlyphStyle::Generic:
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
        case Row::DsBirthMonth:
            choices.assign(kDsMonths.begin(), kDsMonths.end());
            current = settings_.ds_birth_month - 1;
            break;
        case Row::DsBirthDay:
            // The month's own days; every day there is, before a month is.
            for (int day = 1; day <= ds_days_in_month(std::max(1, settings_.ds_birth_month)); ++day) {
                choices.push_back(std::to_string(day));
            }
            current = settings_.ds_birth_day - 1;
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
        case Row::WiiRemoteRumble:
            for (const int percent : kWiiRemoteRumbleChoices) {
                if (percent == settings_.wii_remote_rumble) current = static_cast<int>(choices.size());
                choices.push_back(rumble_label(percent));
            }
            break;
#endif
        default: break;
    }
    return choices;
}

void SettingsScreen::choose(int index, SettingsScreenResult& result) {
    const Item* item = focused_item();
    if (item == nullptr) return;
    if (item->kind == Item::Kind::Melon) {
        if (item->melon == MelonRow::CpuClock) {
            if (index < 0 || index >= static_cast<int>(std::size(kMelonDsCpuMhz))) return;
            tuning_.cpu_mhz = kMelonDsCpuMhz[index];
        } else {
            tuning_.pipeline = index == 1;
        }
        save_melonds();
        refresh_items();
        return;
    }
    if (item->kind != Item::Kind::Setting) return;
    switch (item->row) {
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
        case Row::DsBirthMonth:
            // The day kept, as far as the month has it: the 31st becomes
            // the 30th in April. None yet is the 1st.
            if (index < 0 || index >= 12) return;
            settings_.ds_birth_month = index + 1;
            settings_.ds_birth_day = std::clamp(settings_.ds_birth_day, 1, ds_days_in_month(index + 1));
            break;
        case Row::DsBirthDay:
            // A day before a month: January's, until the month is chosen.
            if (index < 0 || index >= ds_days_in_month(std::max(1, settings_.ds_birth_month))) return;
            settings_.ds_birth_month = std::max(1, settings_.ds_birth_month);
            settings_.ds_birth_day = index + 1;
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
        case Row::WiiRemoteRumble:
            if (index < 0 || index >= static_cast<int>(std::size(kWiiRemoteRumbleChoices))) return;
            settings_.wii_remote_rumble = kWiiRemoteRumbleChoices[index];
            break;
#endif
        default: return;
    }
    result.settings_changed = true;
}


void SettingsScreen::press_row(SettingsScreenResult& result, const Geometry& geometry) {
    const Row row = focused_row();
    if (row == Row::DsNickname) {
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
        adjust(result);
        return;
    }
    if (focus_ < 0 || focus_ >= static_cast<int>(geometry.rows.size())) return;
    choice_row_ = geometry.rows[static_cast<std::size_t>(focus_)];
    if (row == Row::DsFavoriteColor) {
        // The DS's own sixteen, in its own order, four to a line as its
        // settings lay them out.
        std::vector<Color> colors;
        for (const DsFavoriteColor& color : kDsFavoriteColors) colors.push_back(Color{color.r, color.g, color.b, 1.0f});
        choice_.open_colors(std::move(colors), std::move(choices), 4, current, choice_row_);
        return;
    }
    choice_.open(std::move(choices), current, choice_row_);
#ifndef __SWITCH__
    if (row == Row::ConfirmButtonRow) {
        // The two buttons, as the row shows them: yes, then no.
        choice_.set_glyph_pairs(
            {{confirm_glyph_for(ConfirmButton::East), cancel_glyph_for(ConfirmButton::East)},
             {confirm_glyph_for(ConfirmButton::South), cancel_glyph_for(ConfirmButton::South)}},
            glyph_aspect_);
    }
#endif
}

bool SettingsScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                            float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    drag_.update(pointers, geometry.detail, scroll_, geometry.max_scroll());
    return drag_.release_was_drag();
}

std::vector<float> SettingsScreen::scroll_state(float output_width, float output_height) const {
    return {scroll_in_lines(scroll_, compute_geometry(output_width, output_height).scale)};
}

void SettingsScreen::set_scroll_state(const std::vector<float>& state, float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 0), geometry.scale), 0.0f, geometry.max_scroll());
    drag_.stop();
}

int SettingsScreen::entry_at(float x, float y, float output_width, float output_height) const {
    const Geometry geometry = compute_geometry(output_width, output_height);
    for (std::size_t index = 0; index < geometry.groups.size(); ++index) {
        // The whole width up to the line, as the side tab is held.
        const PixelRect& tab = geometry.groups[index];
        if (PixelRect{tab.x, tab.y, geometry.divider_x - tab.x, tab.h}.contains(x, y)) {
            return static_cast<int>(index) * 2;
        }
    }
    if (!geometry.detail.contains(x, y)) return -1;
    for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
        if (geometry.rows[index].contains(x, y)) return static_cast<int>(index) * 2 + 1;
    }
    return -1;
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
    open_group(SettingsGroup::System);
    int index = 0;
    for (const Item& item : items_) {
        if (!item.focusable()) continue;
        if (item.kind == Item::Kind::DataManagement) focus_ = index;
        ++index;
    }
}

std::string SettingsScreen::footnote() const {
    if (group() != SettingsGroup::Consoles || !melonds_available_) return std::string();
    std::string note = "melonDS's settings take effect the next time a game starts. They are kept in " +
                       melonds_path_ + ", which can be edited by hand as well.";
    if (melonds_deko_) {
        note += " Each frame shown a frame later lets the emulator run on while the GPU draws, which keeps "
                "busy 3D games at full speed; as soon as drawn shows each picture a frame sooner.";
    }
    return note;
}

const char* SettingsScreen::row_description(int index) const {
    int at = 0;
    for (const Item& item : items_) {
        if (!item.focusable()) continue;
        if (at++ != index) continue;
        if (item.kind != Item::Kind::Setting) return nullptr;
        if (item.row == Row::Vsync) return kVsyncDescription;
#if DS_REMOTE_START_HOST
        if (item.row == Row::RemoteStart) {
            return remote_start_on_ ? "Other devices can start games here. Disabling removes the service."
                                    : "Installs a background service so other devices can start games here.";
        }
#endif
        return nullptr;
    }
    return nullptr;
}

float SettingsScreen::description_height(float scale) const {
    // Under the row's own rule, as a Switch sets one: a little below the
    // rule, then most of a row's room before the next one's.
    return TextRenderer::line_height(scale) * 4.5f;
}

SettingsScreen::Geometry SettingsScreen::compute_geometry(float output_width,
                                                          float output_height) const {
    Geometry geometry;
    // The same frame as every other page, split as Graphics Packs and the
    // quick menu are: the groups, a line, and the rows of the one
    // highlighted.
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    const float line = frame.line;
    geometry.line = line;
    geometry.panel = frame.panel;
    geometry.view_top = frame.content_top;
    geometry.view_height = frame.content_bottom - frame.content_top;

    const float row_height = menu_row_height(line);
    const float gap = menu_row_gap(line);
    // The list as wide as its longest group needs, with room for the bar
    // that marks the one showing, and no wider, so the rows get the rest.
    const float row_scale = geometry.scale * 1.1f;
    float label_width = 0.0f;
    const std::vector<SettingsGroup> shown = groups();
    for (const SettingsGroup group : shown) {
        label_width = std::max(label_width, TextRenderer::measure_width(settings_group_name(group), row_scale));
    }
    const float list_width =
        std::round(std::clamp(label_width + line * 3.0f, geometry.panel.w * 0.2f, geometry.panel.w * 0.34f));
    float y = geometry.view_top;
    for (std::size_t index = 0; index < shown.size(); ++index) {
        geometry.groups.push_back(PixelRect{geometry.panel.x, y, list_width, row_height});
        y += row_height + gap;
    }
    geometry.divider_x = std::round(geometry.panel.x + list_width + line);
    const float detail_x = geometry.divider_x + line;
    geometry.detail = PixelRect{detail_x, geometry.view_top, geometry.panel.x + geometry.panel.w - detail_x,
                                geometry.view_height};

    // The group's rows, short of its right-hand edge by the scroll bar, which
    // runs beside them rather than over them; a console's heading and where
    // its emulator is between them.
    const float top = geometry.view_top - scroll_;
    const float row_width = geometry.detail.w - scroll_bar_width(geometry.scale) - line * 0.5f;
    float row_y = top;
    int row = 0;
    for (std::size_t index = 0; index < items_.size(); ++index) {
        switch (items_[index].kind) {
            case Item::Kind::Heading:
                row_y += index > 0 ? line * 1.3f : line * 0.3f;
                geometry.texts.push_back(Geometry::Text{index, row_y});
                row_y += line * 1.45f;
                break;
            case Item::Kind::Location:
                geometry.texts.push_back(Geometry::Text{index, row_y});
                row_y += line * 1.8f;
                break;
            default:
                geometry.rows.push_back(PixelRect{geometry.detail.x, row_y, row_width, row_height});
                row_y += row_height + gap;
                // A line of explanation under the row, in the gap before the next.
                if (row_description(row) != nullptr) row_y += description_height(geometry.scale);
                ++row;
                break;
        }
    }
    float end = row_y;
    const std::string note = footnote();
    geometry.footnote_top = row_y + line * 0.4f;
    if (!note.empty()) {
        const float footnote_scale = geometry.scale * 0.85f;
        const float note_width = row_width - menu_row_pad(line) * 2.0f;
        end = geometry.footnote_top + TextRenderer::wrapped_line_step(footnote_scale) *
                                          static_cast<float>(TextRenderer::wrap(note, note_width, footnote_scale).size());
    }
    if (!melonds_error_.empty() && group() == SettingsGroup::Consoles) end += line * 1.4f;
    // Measured from the top of the content rather than from the panel, so it
    // is the height of the thing being scrolled and not of the window onto it.
    geometry.content_height = end + line * 0.4f - top;
    return geometry;
}

void SettingsScreen::adjust(SettingsScreenResult& result) {
    const Item* item = focused_item();
    if (item == nullptr) return;
    switch (item->kind) {
        case Item::Kind::Melon:
            adjust_melonds(item->melon);
            refresh_items();
            return;
        // Downloading or updating is the app's -- and asked first.
        case Item::Kind::Download: result.open_download = item->system; return;
        case Item::Kind::DataManagement: result.open_data_management = true; return;
        case Item::Kind::Updates: result.open_updates = true; return;
        case Item::Kind::Heading:
        case Item::Kind::Location: return;
        case Item::Kind::Setting: break;
    }
    // Only the switches and Remote Start get here: every other row has its
    // choices, which press_row() puts up.
    switch (item->row) {
        case Row::Vsync:
            settings_.vsync = !settings_.vsync;
            result.settings_changed = true;
            break;
#if DS_PLAYS_WII_U
        case Row::PauseWiiUInMenu:
            settings_.pause_wii_u_in_menu = !settings_.pause_wii_u_in_menu;
            result.settings_changed = true;
            break;
        case Row::DiscordStatus:
            settings_.discord_status = !settings_.discord_status;
            result.settings_changed = true;
            break;
#endif
#if DS_REMOTE_START_HOST
        case Row::RemoteStart:
            // Asked first: it installs or removes a service.
            asking_remote_start_ = true;
            alert_cursor_.reset(static_cast<int>(remote_start_buttons().size()), 1);
            break;
#endif
        case Row::UiSounds:
            settings_.ui_sounds = !settings_.ui_sounds;
            result.settings_changed = true;
            break;
        default: break;
    }
    refresh_items();
}

void SettingsScreen::scroll_focus_into_view(const Geometry& geometry) {
    if (!in_group_) {
        scroll_ = std::min(scroll_, geometry.max_scroll());
        return;
    }
    const int rows = row_count();
    const std::size_t index = static_cast<std::size_t>(std::max(0, std::min(focus_, rows - 1)));
    if (index >= geometry.rows.size()) {
        return;
    }
    // The first row shows the top of the group, and the last the bottom of
    // it -- melonDS's footnote under its rows -- so the cursor alone reaches
    // everything, and the scroll bar ends where the group does.
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
    const float bottom = geometry.view_top + geometry.view_height;
    // A row's heading comes into view with it.
    float row_top = row.y;
    for (const Geometry::Text& placed : geometry.texts) {
        if (placed.y < row.y && placed.y > row.y - row.h - TextRenderer::line_height(geometry.scale) * 3.0f) {
            row_top = std::min(row_top, placed.y);
        }
    }
    if (row_top < top) {
        scroll_ -= top - row_top;
    } else {
        // With its line of explanation, which is the cursor's too.
        const float end = row.y + row.h +
                          (row_description(focus_) != nullptr ? description_height(geometry.scale) : 0.0f);
        if (end > bottom) scroll_ += std::min(end - bottom, row.y - top);
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
    // What a group shows can change under it -- a download's progress, say
    // -- so it is read again each time.
    refresh_items();
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
    // An alert takes every press until put away.
    if (!notice_.empty()) {
        if (alert_cursor_.update(actions, pointers, notice_geometry(output_width, output_height)) !=
            PopupCursor::kNone) {
            notice_.clear();
        }
        return result;
    }
    if (asking_remote_start_) {
        const int answer =
            alert_cursor_.update(actions, pointers, remote_start_geometry(output_width, output_height));
        if (answer == PopupCursor::kNone) return result;
        asking_remote_start_ = false;
        if (answer == 1) toggle_remote_start();
        refresh_items();
        return result;
    }
    // A row's choices, up beside it, take every press until put away.
    if (choice_.is_open()) {
        const ChoicePopupResult chosen = choice_.update(actions, pointers, output_width, output_height);
        if (chosen.chosen >= 0) choose(chosen.chosen, result);
        if (chosen.close_everything) {
            result.close_requested = true;
            open_ = false;
        }
        return result;
    }

    const int group_count = static_cast<int>(groups().size());
    for (const UiAction action : actions) {
        switch (action) {
            case UiAction::Up:
                if (in_group_) {
                    focus_ = std::max(0, focus_ - 1);
                } else if (group_ > 0) {
                    show_group(group_ - 1);
                }
                break;
            case UiAction::Down:
                if (in_group_) {
                    focus_ = std::max(0, std::min(row_count() - 1, focus_ + 1));
                } else if (group_ + 1 < group_count) {
                    show_group(group_ + 1);
                }
                break;
            // Into the group from the list, on its first row. In it, A opens
            // a row's choices, or flips its switch, or opens its page; Right
            // does nothing there, values not being stepped from the side.
            case UiAction::Right:
            case UiAction::Confirm:
                if (in_group_) {
                    if (action == UiAction::Confirm) press_row(result, compute_geometry(output_width, output_height));
                } else if (row_count() > 0) {
                    in_group_ = true;
                    focus_ = 0;
                }
                break;
            // Back to the list from the group's left-hand edge, as in the
            // quick menu.
            case UiAction::Left: in_group_ = false; break;
            // B a step at a time; the menu's own button out of everything.
            case UiAction::Cancel:
                if (in_group_) {
                    in_group_ = false;
                } else {
                    result.close_requested = true;
                }
                break;
            case UiAction::ToggleMenu: result.close_requested = true; break;
            // A menu's face buttons. Nothing here answers them, and a bare
            // fallthrough into Quit would make pressing X leave the screen.
            case UiAction::MenuQuit:
            case UiAction::MenuReset:
            case UiAction::MenuDisconnect:
            case UiAction::MenuMinus:
            // Already here, which is the whole of what it asks for.
            case UiAction::MenuSettings: break;
            // Escape also raises Cancel, which is what it means here.
            case UiAction::OpenMenu: break;
            case UiAction::Quit: break;
            case UiAction::ToggleFilter:  // handled by the app, wherever you are
            case UiAction::NextLayout:
            case UiAction::SwapLayout:    // nothing here is a layout
            // L and R jump nowhere: a shoulder is not a way through a list.
            case UiAction::PageUp:
            case UiAction::PageDown: break;
        }
        // A press that opened something takes the rest of this pass's.
        if (choice_.is_open() || text_entry_.is_open() || asking_remote_start_) break;
    }

    // The cursor and the view are settled before anything is hit-tested: an
    // action may have moved either, and a row cannot be tapped where it was.
    const Geometry moved = compute_geometry(output_width, output_height);
    scroll_ = std::max(0.0f, std::min(scroll_, moved.max_scroll()));
    // The cursor brings the view with it when it is moved -- and only then,
    // so a page dragged by a finger stays where the finger left it.
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(compute_geometry(output_width, output_height));
    }
    // A tap on a group shows it, the cursor staying on the list, as a tap on
    // a Switch's settings' left-hand list does; one on a row presses it.
    // Heard the one way every tap is.
    for (const int entry : take_taps(pointers, output_width, output_height)) {
        if (entry < 0) continue;
        const int index = entry / 2;
        if (entry % 2 == 0) {
            if (index < group_count) show_group(index);
            continue;
        }
        if (index >= row_count()) continue;
        in_group_ = true;
        if (tap_chooses(focus_, index)) press_row(result, compute_geometry(output_width, output_height));
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

    refresh_items();
    const ShownScroll shown(scroll_, scroll_ease_, drag_.dragging(), group_);
    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float row_scale = scale * 1.1f;

    const PageFrame frame = page_frame(output_width, output_height);
    // Over the paused game, dimmed, when opened from its menu; on the app's
    // own background otherwise, since there is nothing behind it to dim.
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame, "Settings");

    // The groups, drawn as Graphics Packs draws its packs: the one whose rows
    // are showing in the accent colour with a bar beside it, and ringed while
    // the cursor is on it.
    const std::vector<SettingsGroup> listed = groups();
    for (std::size_t index = 0; index < listed.size() && index < geometry.groups.size(); ++index) {
        const PixelRect& row = geometry.groups[index];
        const bool current = static_cast<int>(index) == group_;
        draw_side_tab(quads, row, current, current && !in_group_, scale);
        text.draw(quads, settings_group_name(listed[index]), row.x + line * 0.8f,
                  row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f, row_scale,
                  current ? theme.accent : theme.text);
    }
    quads.draw_solid(PixelRect{geometry.divider_x, geometry.view_top, std::max(1.0f, std::round(scale * 0.5f)),
                               geometry.view_height},
                     theme.text_dim);

    // Everything on the right scrolls, so it is clipped to the room there is
    // for it. Without this a row pushed past the bottom carries on being
    // drawn over whatever is under the panel.
    quads.set_clip(menu_list_clip(geometry.detail, scale));

    int index = -1;
    const int rows = static_cast<int>(geometry.rows.size());
    for (const Item& item : items_) {
        if (!item.focusable()) continue;
        ++index;
        if (index >= rows) break;
        const PixelRect& row = geometry.rows[static_cast<std::size_t>(index)];
        const bool focused = in_group_ && index == focus_;
        // A row with a line of explanation is ruled off above it, and the
        // rule over the row after stays when the cursor is on this one. A
        // row with a heading after it is the last of its run.
        const bool described = row_description(index) != nullptr;
        const std::size_t at = static_cast<std::size_t>(&item - items_.data());
        const bool last = index == rows - 1 || (at + 1 < items_.size() && !items_[at + 1].focusable());
        draw_menu_row(quads, row, focused, scale, described || last,
                      in_group_ && index == focus_ + 1 && row_description(focus_) == nullptr);

        // Label on the left, value on the right, both on one baseline. The
        // value is elided rather than allowed to run past the row, which is
        // what a long path would otherwise do.
        const float pad = menu_row_pad(line);
        const float baseline = page_row_text_y(row, scale);
        text.draw(quads, item.text, row.x + pad, baseline, scale, menu_row_text(focused));
        if (const char* description = row_description(index)) {
            text.draw(quads, description, row.x + pad, row.y + row.h + menu_row_reach(scale) + line * 0.4f,
                      scale * 0.8f, theme.text_dim);
        }
        const Row setting = item.kind == Item::Kind::Setting ? item.row : Row::Count;
#ifndef __SWITCH__
        if (setting == Row::ConfirmButtonRow) {
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
        if (setting == Row::DsFavoriteColor) {
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
        const bool has_choices = setting == Row::RunAheadDs;
        if (is_toggle_value(item.value) && !has_choices) {
            draw_toggle(quads, row.x + row.w - pad, row.y + row.h * 0.5f, item.value == "On", line);
        } else if (!item.value.empty()) {
            const Color value_color = theme.text_dim;
            const float label_width = TextRenderer::measure_width(item.text, scale);
            const float available = row.w - label_width - pad * 4.0f;
            const std::string value = TextRenderer::elide_left(item.value, available, scale * 0.9f);
            text.draw(quads, value, row.x + row.w - pad, page_row_text_y(row, scale * 0.9f),
                      scale * 0.9f, value_color, TextAlign::Right);
        }
    }

    // Each console's heading -- its name, then its emulator's, dimmer -- and
    // on the line under it where that emulator was found.
    // In line with the rows' own words.
    const float text_x = geometry.detail.x + menu_row_pad(line);
    const float detail_width = geometry.detail.w - scroll_bar_width(scale) - line * 0.5f - menu_row_pad(line) * 2.0f;
    for (const Geometry::Text& placed : geometry.texts) {
        const Item& item = items_[placed.item];
        if (item.kind == Item::Kind::Heading) {
            text.draw(quads, item.text, text_x, placed.y, scale, theme.text);
            if (!item.value.empty()) {
                const float name_width = TextRenderer::measure_width(item.text + "  ", scale);
                text.draw(quads, item.value, text_x + name_width, placed.y, scale, theme.text_dim);
            }
        } else {
            text.draw(quads, TextRenderer::elide_left(item.text, detail_width, scale * 0.85f), text_x, placed.y,
                      scale * 0.85f, theme.text_dim);
        }
    }
    const std::string note = footnote();
    float note_height = 0.0f;
    if (!note.empty()) {
        note_height = text.draw_wrapped(quads, note, text_x, geometry.footnote_top, detail_width, scale * 0.85f,
                                        theme.text_dim);
    }
    if (!melonds_error_.empty() && group() == SettingsGroup::Consoles) {
        text.draw(quads, TextRenderer::elide_right(melonds_error_, detail_width, scale * 0.85f), text_x,
                  geometry.footnote_top + note_height + line * 0.4f, scale * 0.85f, theme.danger);
    }

    // Everything above scrolled and was clipped. The prompts below it are
    // fixed to the window, so the clip comes off here.
    quads.clear_clip();

    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen}},
                    &guide_hits_);

    // Something has to say there is more, or a list that stops at the bottom
    // edge looks like a list that ends there.
    if (geometry.max_scroll() > 0.5f) {
        const float span = std::max(geometry.view_height, geometry.content_height);
        draw_scroll_bar(quads, geometry.detail, scroll_ / span, geometry.view_height / span, line, scale);
    }
    glyph_aspect_ = glyphs.width_for(1.0f);
    choice_.render(quads, text, output_width, output_height, &glyphs);
    text_entry_.render(quads, text, output_width, output_height);
    if (asking_remote_start_) {
        const PopupGeometry popup = remote_start_geometry(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, remote_start_buttons(), alert_cursor_.focus(),
                   alert_cursor_.shown());
        draw_popup_text(quads, text, remote_start_text(), popup.body.x, popup.body.y, popup.body.w, output_height);
    } else if (!notice_.empty()) {
        const PopupGeometry popup = notice_geometry(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, {{"OK", ""}}, alert_cursor_.focus(),
                   alert_cursor_.shown());
        draw_popup_text(quads, text, PopupText{notice_, {}, 0}, popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

void SettingsScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    mix_sound_state(view.focus, static_cast<uint64_t>(group_ * 2 + (in_group_ ? 1 : 0) + 3000));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
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
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.wii_remote_rumble));
    mix_sound_state(view.rest, settings_.ui_sounds ? 1u : 0u);
    mix_sound_state(view.rest, settings_.discord_status ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(tuning_.cpu_mhz));
    mix_sound_state(view.rest, choice_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, text_entry_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, remote_start_on_ ? 1u : 0u);
    mix_sound_state(view.rest, asking_remote_start_ ? 1u : 0u);
    mix_sound_state(view.rest, notice_.empty() ? 0u : 1u);
    if (asking_remote_start_ || !notice_.empty()) mix_sound_state(view.focus, static_cast<uint64_t>(alert_cursor_.focus() + 2000));
    mix_sound_state(view.rest, settings_.ds_nickname);
    mix_sound_state(view.rest, static_cast<uint64_t>(settings_.ds_favorite_color + 1));
    if (choice_.is_open()) mix_sound_state(view.focus, static_cast<uint64_t>(choice_.focus() + 1000));
    mix_sound_state(view.rest, tuning_.pipeline ? 1u : 0u);
    // The switches: the rows drawn as one -- see render().
    if (!choice_.is_open() && !text_entry_.is_open() && !asking_remote_start_ && notice_.empty()) {
        switch (focused_row()) {
            case Row::Vsync: view.toggle = settings_.vsync ? 1 : 0; break;
#if DS_PLAYS_WII_U
            case Row::PauseWiiUInMenu: view.toggle = settings_.pause_wii_u_in_menu ? 1 : 0; break;
            case Row::DiscordStatus: view.toggle = settings_.discord_status ? 1 : 0; break;
#endif
            case Row::UiSounds: view.toggle = settings_.ui_sounds ? 1 : 0; break;
            default: break;
        }
    }
}

}  // namespace ds
