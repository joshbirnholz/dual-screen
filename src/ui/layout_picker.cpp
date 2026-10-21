#include "ui/layout_picker.hpp"

#include "core/render_scale.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/page_frame.hpp"
#include "ui/ui_icons.hpp"
#include "util/battery.hpp"

#include <algorithm>
#include <string>
#include <cmath>

namespace ds {

LayoutPicker::LayoutPicker(const LayoutRegistry& registry) : registry_(&registry) {
    set_screens(screens_);
}

void LayoutPicker::set_screens(const SystemScreens& screens, SystemId system) {
    screens_ = screens;
    system_ = system;
    rebuild_tiles();
}

void LayoutPicker::rebuild_tiles() {
    tiles_.clear();
    if (system_ == SystemId::WiiU) {
        // Which single screen shows here is decided once, at connect, and
        // does not change for the rest of the session -- see the host poll
        // loop in app.cpp. There is nothing left to pick a layout between.
        return;
    }
    tiles_ = menu_layouts(*registry_, system_, screens_);
}

const Layout& LayoutPicker::tile_layout(int index) const {
    return registry_->at(tiles_[static_cast<std::size_t>(index)]);
}

int LayoutPicker::tile_index_of(const std::string& layout_id) const {
    for (std::size_t index = 0; index < tiles_.size(); ++index) {
        if (registry_->at(tiles_[index]).id == layout_id) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void LayoutPicker::open(const LayoutPickerState& state) {
    // Where it was closed, held before anything below can move it.
    const MenuSection closed_on = section_;
    const bool closed_inside = on_content_;
    const int closed_item = item_;
    const bool reopening = opened_before_;
    opened_before_ = true;
    open_ = true;
    lingering_ = false;
    confirm_ = MenuConfirm::None;
    state_ = state;
    selected_layout_id_ = state.layout_id;
    save_status_.clear();
    save_status_failed_ = false;

    rebuild_footer();
    content_scroll_ = 0.0f;
    // The first time, the list at its top -- the way NSO's menu opens -- with
    // the layouts already showing beside it. After that, exactly where it was
    // closed: the section, and the entry inside it when the cursor was in
    // there. Whoever comes back to the menu mostly comes back for the same
    // thing. A section this game or this session does not have starts from
    // the top instead.
    on_content_ = false;
    item_ = 0;
    // Where it was closed may not be in this menu -- Continue is not in the
    // one a Wii U shows in place of the game -- and is kept for the next one
    // it is in, unless the cursor is moved in the meantime.
    MenuSection wanted = closed_on;
    bool wanted_inside = closed_inside;
    int wanted_item = closed_item;
    if (held_place_ && has_section(held_place_->section)) {
        wanted = held_place_->section;
        wanted_inside = held_place_->inside;
        wanted_item = held_place_->item;
        held_place_.reset();
    }
    if (sections_.empty()) {
        section_ = MenuSection::Layout;
    } else if (reopening && has_section(wanted)) {
        section_ = wanted;
        if (wanted_inside && wanted_item < content_count(section_)) {
            on_content_ = true;
            item_ = std::max(0, wanted_item);
        }
    } else {
        if (reopening && !held_place_) held_place_ = HeldPlace{wanted, wanted_inside, wanted_item};
        section_ = sections_.front();
    }
    if (state_.amiibo_wanted && section_ != MenuSection::Amiibo && has_section(MenuSection::Amiibo) &&
        content_count(MenuSection::Amiibo) > 0) {
        section_ = MenuSection::Amiibo;
        on_content_ = true;
        item_ = 0;
    }
}

void LayoutPicker::set_save_slots(std::vector<SavePointInfo> slots) {
    slots.resize(kSavePointSlots);
    save_slots_ = std::move(slots);
    slot_pictures_dirty_.fill(true);
}

void LayoutPicker::set_save_status(std::string status, bool failed) {
    save_status_ = std::move(status);
    save_status_failed_ = failed;
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
    sections_.clear();
    options_.clear();
    controls_.clear();
    if (system_ == SystemId::WiiU) {
        // The same two columns as a DS or 3DS game's, with a Wii U's own
        // things in them: there is no layout to pick (see rebuild_tiles()),
        // and no save states -- Cemu has none -- but there are its pages,
        // and amiibo.
        //
        // Continue at the top, except where the menu is shown in place of
        // the game (see LayoutPickerState::embedded): there is no menu to
        // leave there, the game being one window away. Settings is the plus
        // button, as on every other console's menu, rather than a section.
        if (!state_.embedded) sections_.push_back(MenuSection::Continue);
        sections_.push_back(MenuSection::Options);
        sections_.push_back(MenuSection::ControllerOrder);
        if (state_.amiibo_available) sections_.push_back(MenuSection::Amiibo);
        if (state_.session_active) sections_.push_back(MenuSection::Disconnect);
        if (state_.reset_available) sections_.push_back(MenuSection::Reset);
        sections_.push_back(MenuSection::Quit);

        // This game's Options: first its graphics packs, a page of their
        // own, then its screen, kept for it, the pointer, which is this
        // device's own as on a DS menu, and whatever Cemu offers.
        if (state_.graphic_packs_available) options_.push_back(OptionRow{Option::GraphicPacks});
        if (state_.screens_swappable) options_.push_back(OptionRow{Option::SwapScreens});
        if (state_.cursor_choosable) options_.push_back(OptionRow{Option::Cursor});
        // Cemu's own after them, but for the players' controllers, which are
        // Controls', under Controller Order.
        const int producers = static_cast<int>(state_.producer_options.size());
        const int controls_from = state_.producer_controls_from >= 0
                                      ? std::min(state_.producer_controls_from, producers)
                                      : producers;
        for (int index = 0; index < controls_from; ++index) {
            options_.push_back(OptionRow{Option::Producer, index});
        }
        controls_.push_back(OptionRow{Option::ControllerOrder});
        for (int index = controls_from; index < producers; ++index) {
            controls_.push_back(OptionRow{Option::Producer, index});
        }
    } else {

    // The sections, in the order the player asked for them: back to the game,
    // what the game looks like, then saving and loading, then everything
    // else, and last the three that end something.
    sections_.push_back(MenuSection::Continue);
    if (tile_count() > 0) sections_.push_back(MenuSection::Layout);
    if (state_.save_points_available) {
        sections_.push_back(MenuSection::CreateSavePoint);
        sections_.push_back(MenuSection::LoadSavePoint);
    }
    if (state_.save_sync_available) sections_.push_back(MenuSection::SaveSync);
    sections_.push_back(MenuSection::Options);
    // How the game is played: its buttons, and the right stick.
    if (state_.button_mapping_available) controls_.push_back(OptionRow{Option::ButtonMapping});
    // What the right stick does, where the console has one of its own to have
    // an opinion about -- a 3DS -- or a stylus to be, on a DS.
    if (state_.right_stick_choosable) controls_.push_back(OptionRow{Option::RightStick});
    if (!controls_.empty()) sections_.push_back(MenuSection::ControllerOrder);
    if (state_.session_active) sections_.push_back(MenuSection::Disconnect);
    if (state_.reset_available) sections_.push_back(MenuSection::Reset);
    sections_.push_back(MenuSection::Quit);

    // Absent only where there is nothing to choose: a system whose core draws
    // at one fixed resolution, or no game at all. A companion gets it like
    // every other entry here -- one menu, reachable from either device -- and
    // asks the host to make the change.
    if (state_.max_render_scale > 1) options_.push_back(OptionRow{Option::RenderScale});
    // Which device makes the sound is only a question when there are two of
    // them, so the entry appears with the session and not before.
    if (state_.session_active) options_.push_back(OptionRow{Option::Sound});
    // The pointer is this device's own, and it is the one thing worth reaching
    // quickly while playing: a trackpad you cannot see is a trackpad you cannot
    // aim.
    if (state_.cursor_choosable) options_.push_back(OptionRow{Option::Cursor});
    // A DS's Rumble Pak, where the core can give it one.
    if (state_.rumble_choosable) options_.push_back(OptionRow{Option::Rumble});
    // The gap is one choice for every layout that has one -- Vertical Screens
    // and the two focus layouts; see LayoutComposition::gap_toggleable -- so
    // it is offered whichever layout is in use, and waits for those.
    options_.push_back(OptionRow{Option::ScreenGap});
    // R3 still does this without opening anything; this is where it is seen.
    if (state_.smoothing_available) options_.push_back(OptionRow{Option::Smoothing});
    // A DS's screens with the pixel grid of its own.
    if (state_.lcd_filter_choosable) options_.push_back(OptionRow{Option::LcdFilter});
    // A 3DS game's texture pack, where the core can load one.
    if (state_.custom_textures_choosable) options_.push_back(OptionRow{Option::CustomTextures});
    // Where the game starts next time, which is about the game rather than
    // about how it looks -- so last. Only where save states are.
    if (state_.resume_choosable) options_.push_back(OptionRow{Option::Resume});

    }

    // The cursor can be sitting on something that just went away.
    if (sections_.empty()) return;
    if (!has_section(section_)) {
        section_ = sections_.front();
        on_content_ = false;
        item_ = 0;
    } else if (on_content_ && item_ >= content_count(section_)) {
        item_ = std::max(0, content_count(section_) - 1);
        on_content_ = content_count(section_) > 0;
    }
}

bool LayoutPicker::has_section(MenuSection section) const {
    return std::find(sections_.begin(), sections_.end(), section) != sections_.end();
}

int LayoutPicker::content_count(MenuSection section) const {
    switch (section) {
        case MenuSection::Layout: return tile_count() + (corner_button_usable() ? 1 : 0);
        case MenuSection::CreateSavePoint:
        case MenuSection::LoadSavePoint: return kSavePointSlots;
        case MenuSection::Options:
        case MenuSection::ControllerOrder: return static_cast<int>(rows_of(section).size());
        // Each amiibo, then a row to look in another folder.
        case MenuSection::Amiibo: return static_cast<int>(state_.amiibo.size()) + 1;
        case MenuSection::Continue:
        case MenuSection::SaveSync:
        case MenuSection::GraphicPacks:
        case MenuSection::Disconnect:
        case MenuSection::Reset:
        case MenuSection::Quit: break;
    }
    return 0;
}

bool LayoutPicker::slot_filled(int slot) const {
    return slot >= 0 && slot < static_cast<int>(save_slots_.size()) &&
           save_slots_[static_cast<std::size_t>(slot)].exists;
}

int LayoutPicker::layout_columns() const { return tile_count() <= 4 ? 2 : 3; }

int LayoutPicker::content_columns(MenuSection section) const {
    switch (section) {
        case MenuSection::Layout: return layout_columns();
        case MenuSection::CreateSavePoint:
        case MenuSection::LoadSavePoint: return 2;
        default: return 1;
    }
}

int LayoutPicker::grid_count(MenuSection section) const {
    return section == MenuSection::Layout ? tile_count() : content_count(section);
}

bool LayoutPicker::hand_button_usable() const {
    return tile_count() > 0 && layout_has_handedness(registry_->resolve(selected_layout_id_));
}

bool LayoutPicker::swap_button_usable() const {
    return tile_count() > 0 && registry_->resolve(selected_layout_id_).swaps_screens;
}

std::string LayoutPicker::option_name(const OptionRow& row) const {
    switch (row.option) {
        case Option::RenderScale: return "Render Scale";
        case Option::Sound: return "Sound";
        case Option::Cursor: return "Hide Cursor";
        case Option::RightStick: return "Right Stick";
        case Option::Rumble: return "Rumble";
        case Option::ScreenGap: return "Screen Gap";
        case Option::Smoothing: return "Screen Smoothing";
        case Option::LcdFilter: return "LCD Filter";
        case Option::CustomTextures: return "Load Custom Textures";
        case Option::Resume: return "Resume from Save State";
        case Option::SwapScreens: return "Swap Screens";
        case Option::GraphicPacks: return "Graphics Packs";
        case Option::ControllerOrder: return "Controller Order";
        case Option::ButtonMapping: return "Change Controls";
        case Option::Producer:
            if (row.index >= 0 && row.index < static_cast<int>(state_.producer_options.size())) {
                return state_.producer_options[static_cast<std::size_t>(row.index)].first;
            }
            return std::string();
    }
    return std::string();
}

std::vector<std::string> LayoutPicker::option_choices(const OptionRow& row, int& current) const {
    current = -1;
    std::vector<std::string> choices;
    switch (row.option) {
        case Option::RenderScale:
            // Auto, then 1x up to the most: the order the caller steps
            // them, and each one's place its number.
            choices.push_back("Auto");
            for (int scale = 1; scale <= std::max(1, state_.max_render_scale); ++scale) {
                choices.push_back(std::to_string(scale) + "x");
            }
            current = std::clamp(state_.render_scale, 0, std::max(1, state_.max_render_scale));
            break;
        case Option::Sound: {
            // From each to the next, as next_audio_target() goes.
            const AudioTarget targets[] = {AudioTarget::TopScreen, AudioTarget::BottomScreen,
                                           AudioTarget::Both};
            for (const AudioTarget target : targets) {
                if (target == state_.audio_target) current = static_cast<int>(choices.size());
                choices.push_back(target == AudioTarget::TopScreen      ? "Top"
                                  : target == AudioTarget::BottomScreen ? "Bottom"
                                                                        : "Both");
            }
            break;
        }
        case Option::RightStick: {
            const std::vector<RightStickMode> modes =
                state_.right_stick_modes.empty() ? right_stick_modes(true) : state_.right_stick_modes;
            for (const RightStickMode mode : modes) {
                if (mode == state_.right_stick_mode) current = static_cast<int>(choices.size());
                choices.push_back(to_string(mode));
            }
            break;
        }
        case Option::Producer:
            if (row.index >= 0 && row.index < static_cast<int>(state_.producer_choices.size())) {
                choices = state_.producer_choices[static_cast<std::size_t>(row.index)];
                const std::string value = option_value(row);
                for (std::size_t at = 0; at < choices.size(); ++at) {
                    if (choices[at] == value) current = static_cast<int>(at);
                }
            }
            break;
        // Switches, and a page.
        case Option::Cursor:
        case Option::Rumble:
        case Option::ScreenGap:
        case Option::Smoothing:
        case Option::LcdFilter:
        case Option::CustomTextures:
        case Option::Resume:
        case Option::SwapScreens:
        case Option::GraphicPacks:
        case Option::ControllerOrder:
        case Option::ButtonMapping: break;
    }
    // One value is nothing to choose between.
    if (choices.size() < 2) choices.clear();
    return choices;
}

void LayoutPicker::open_choice() {
    if ((section_ != MenuSection::Options && section_ != MenuSection::ControllerOrder) || item_ < 0 ||
        item_ >= static_cast<int>(rows().size())) {
        return;
    }
    int current = -1;
    std::vector<std::string> choices = option_choices(rows()[static_cast<std::size_t>(item_)], current);
    if (choices.empty()) return;
    const float width = last_width_ > 0.0f ? last_width_ : 1280.0f;
    const float height = last_height_ > 0.0f ? last_height_ : 800.0f;
    const MenuGeometry geometry = compute_menu_geometry(width, height);
    const PixelRect row = item_ < static_cast<int>(geometry.items.size())
                              ? geometry.items[static_cast<std::size_t>(item_)]
                              : geometry.content;
    choice_.open(std::move(choices), current, row);
    confirm_ = MenuConfirm::Choice;
}

std::string LayoutPicker::option_value(const OptionRow& row) const {
    switch (row.option) {
        case Option::RenderScale:
            return state_.render_scale == kAutoRenderScale ? std::string("Auto")
                                                           : std::to_string(state_.render_scale) + "x";
        case Option::Sound:
            switch (state_.audio_target) {
                case AudioTarget::TopScreen: return "Top";
                case AudioTarget::BottomScreen: return "Bottom";
                case AudioTarget::Both: break;
                case AudioTarget::Off: return "Off";  // DS/3DS never set this
            }
            return "Both";
        case Option::Cursor: return to_string(state_.cursor_mode);
        case Option::RightStick: return to_string(state_.right_stick_mode);
        case Option::Rumble: return state_.rumble_pak ? "On" : "Off";
        case Option::ScreenGap: return state_.screen_gap ? "On" : "Off";
        case Option::Smoothing: return state_.bilinear_filtering ? "On" : "Off";
        case Option::LcdFilter: return state_.lcd_filter ? "On" : "Off";
        case Option::CustomTextures: return state_.custom_textures ? "On" : "Off";
        case Option::Resume: return state_.resume_from_save_state ? "On" : "Off";
        // Alone, whether this game shows the other screen from the setting's;
        // with a second device, nothing to say -- it swaps them, there and then.
        case Option::SwapScreens:
            return state_.screens_swap_value_shown ? (state_.screens_swapped ? "On" : "Off") : "";
        // A page, not a value.
        case Option::GraphicPacks:
        case Option::ControllerOrder: return std::string();
        // A page, which says whether this game has made any changes in it.
        case Option::ButtonMapping: return std::string();
        case Option::Producer:
            if (row.index >= 0 && row.index < static_cast<int>(state_.producer_options.size())) {
                return state_.producer_options[static_cast<std::size_t>(row.index)].second;
            }
            return std::string();
    }
    return std::string();
}

namespace {

const char* section_label(MenuSection section) {
    switch (section) {
        case MenuSection::Continue: return "Continue";
        case MenuSection::Layout: return "Screen Layout";
        case MenuSection::CreateSavePoint: return "Create Save State";
        case MenuSection::LoadSavePoint: return "Load Save State";
        case MenuSection::SaveSync: return "Save Sync";
        case MenuSection::Options: return "Game Options";
        case MenuSection::Disconnect: return "Disconnect";
        case MenuSection::Reset: return "Reset Game";
        case MenuSection::Quit: return "Quit";
        case MenuSection::ControllerOrder: return "Controls";
        case MenuSection::GraphicPacks: return "Graphics Packs";
        case MenuSection::Amiibo: return "amiibo";
    }
    return "";
}

}  // namespace

LayoutPicker::MenuGeometry LayoutPicker::compute_menu_geometry(float output_width,
                                                               float output_height) const {
    // The same frame as every other page: see ui/page_frame.hpp.
    MenuGeometry geometry;
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    const float scale = geometry.scale;
    const float line = frame.line;
    geometry.panel = frame.panel;
    geometry.content_top = frame.content_top;
    geometry.content_bottom = frame.content_bottom;

    // The list is as wide as its longest entry needs and no wider, so the
    // pictures get the rest.
    float label_width = 0.0f;
    for (const MenuSection section : sections_) {
        label_width =
            std::max(label_width, TextRenderer::measure_width(section_label(section), scale * 1.1f));
    }
    const float left_width =
        std::min(geometry.panel.w * 0.34f, label_width + menu_row_pad(line) * 2.0f);
    // All of them down the screen, there being nowhere for them to scroll
    // to: closer together where there are many.
    float gap = quick_menu_section_gap(line);
    float row_height = quick_menu_section_height(line);
    const float room = geometry.content_bottom - geometry.content_top;
    const float rows = static_cast<float>(std::max<std::size_t>(1, sections_.size()));
    if (row_height * rows + gap * (rows - 1.0f) > room) {
        const float shrink = room / (row_height * rows + gap * (rows - 1.0f));
        row_height = std::floor(row_height * shrink);
        gap = std::floor(gap * shrink);
    }
    const float row_step = row_height + gap;
    float y = geometry.content_top;
    for (std::size_t index = 0; index < sections_.size(); ++index) {
        geometry.sections.push_back(PixelRect{geometry.panel.x, y, left_width, row_height});
        y += row_step;
    }
    geometry.divider_x = std::round(geometry.panel.x + left_width + line * 1.0f);

    const float content_x = geometry.divider_x + line * 1.0f;
    geometry.content = PixelRect{content_x, geometry.content_top,
                                 geometry.panel.x + geometry.panel.w - content_x,
                                 geometry.content_bottom - geometry.content_top};
    const PixelRect& area = geometry.content;
    const int count = content_count(section_);
    switch (section_) {
        case MenuSection::Layout: {
            // The layouts, and under them the round button that says which
            // hand holds the stylus -- room is kept for it whether or not the
            // layout in use has a hand, so the grid does not jump about.
            const int tiles = tile_count();
            const float button = std::round(line * 3.2f);
            geometry.columns = layout_columns();
            const int rows = (tiles + geometry.columns - 1) / geometry.columns;
            const float gutter = std::round(output_width * 0.012f);
            const float grid_height = area.h - button - gutter;
            const float tile_width =
                (area.w - gutter * (geometry.columns - 1)) / static_cast<float>(geometry.columns);
            float tile_height =
                rows > 0 ? (grid_height - gutter * (rows - 1)) / static_cast<float>(rows)
                         : grid_height;
            // Not taller than a picture of the display and its name need.
            tile_height = std::min(tile_height, tile_width * 0.75f + line * 1.6f);
            for (int index = 0; index < tiles; ++index) {
                const int column = index % geometry.columns;
                const int row = index / geometry.columns;
                geometry.items.push_back(PixelRect{area.x + column * (tile_width + gutter),
                                                   area.y + row * (tile_height + gutter),
                                                   tile_width, tile_height});
            }
            // Under the grid's right-hand end, where the Wii U puts it.
            geometry.hand_button = PixelRect{area.x + area.w - button,
                                             area.y + rows * (tile_height + gutter), button,
                                             button};
            if (count > tiles) geometry.items.push_back(geometry.hand_button);
            break;
        }
        case MenuSection::CreateSavePoint:
        case MenuSection::LoadSavePoint: {
            // A square of them, two by two, in reading order.
            geometry.columns = 2;
            const int rows = (kSavePointSlots + 1) / 2;
            const float gutter = std::round(output_width * 0.012f);
            const float width = (area.w - gutter) / 2.0f;
            const float height = (area.h - gutter * (rows - 1)) / static_cast<float>(rows);
            for (int slot = 0; slot < kSavePointSlots; ++slot) {
                geometry.items.push_back(PixelRect{area.x + (slot % 2) * (width + gutter),
                                                   area.y + (slot / 2) * (height + gutter), width,
                                                   height});
            }
            break;
        }
        case MenuSection::Options:
        case MenuSection::ControllerOrder:
        case MenuSection::Amiibo: {
            // A list, as tall as the sections' rows. Scan amiibo's can run
            // past the bottom, so it starts from the first of it in view.
            const float option_height = menu_row_height(line);
            const float step = option_height + menu_row_gap(line);
            const int fitting = std::max(1, static_cast<int>((area.h + menu_row_gap(line)) / step));
            const bool scrolls = count > fitting;
            const float first = scrolls ? std::clamp(content_scroll_, 0.0f, static_cast<float>(count - fitting)) : 0.0f;
            // Short of the right-hand edge by a scroll bar, where there is one.
            const float width = scrolls ? area.w - scroll_bar_width(scale) - line * 0.5f : area.w;
            for (int index = 0; index < count; ++index) {
                geometry.items.push_back(PixelRect{
                    area.x, std::round(area.y + (static_cast<float>(index) - first) * step), width, option_height});
            }
            break;
        }
        case MenuSection::Continue:
        case MenuSection::SaveSync:
        case MenuSection::GraphicPacks:
        case MenuSection::Disconnect:
        case MenuSection::Reset:
        case MenuSection::Quit: break;
    }
    return geometry;
}

void LayoutPicker::show(const LayoutPickerState& state, int focus, MenuConfirm confirm,
                        int confirm_focus_wire) {
    // What the menu has depends on what is being offered, and the layout in
    // use decides whether one option is there at all, so it is rebuilt before
    // the cursor is placed -- a focus means nothing until the entries it
    // points at are the right ones.
    const bool entries_changed = state.layout_id != selected_layout_id_ ||
                                 state.session_active != state_.session_active ||
                                 state.max_render_scale != state_.max_render_scale ||
                                 state.screens_swappable != state_.screens_swappable ||
                                 state.cursor_choosable != state_.cursor_choosable ||
                                 state.right_stick_choosable != state_.right_stick_choosable ||
                                 state.button_mapping_available != state_.button_mapping_available ||
                                 state.rumble_choosable != state_.rumble_choosable ||
                                 state.custom_textures_choosable != state_.custom_textures_choosable ||
                                 state.save_points_available != state_.save_points_available ||
                                 state.save_sync_available != state_.save_sync_available ||
                                 state.resume_choosable != state_.resume_choosable ||
                                 state.reset_available != state_.reset_available ||
                                 state.smoothing_available != state_.smoothing_available;
    state_ = state;
    selected_layout_id_ = state.layout_id;
    if (entries_changed) {
        rebuild_footer();
    }
    set_focus(focus);
    if (confirm != confirm_) {
        if (confirm == MenuConfirm::None) {
            confirm_ = MenuConfirm::None;
            choice_.close();
        } else if (confirm == MenuConfirm::Choice) {
            open_choice();
        } else {
            ask(confirm);
        }
    }
    // The host's cursor, on the host's popup.
    if (confirm_ == MenuConfirm::Choice) {
        if (confirm_focus_wire > 0) choice_.set_focus(confirm_focus_wire - 1);
    } else if (confirming()) {
        if (confirm_focus_wire > 0) {
            confirm_cursor_.set_focus(confirm_focus_wire - 1);
        } else {
            confirm_cursor_.hide();
        }
    }
}

void LayoutPicker::set_session_active(bool active) {
    if (active == state_.session_active) {
        return;
    }
    state_.session_active = active;
    rebuild_footer();
}

int LayoutPicker::focus() const { return focus_of(section_, on_content_ ? item_ : -1); }

int LayoutPicker::content_rows_fitting(const MenuGeometry& geometry) const {
    const float line = TextRenderer::line_height(geometry.scale);
    const float step = menu_row_height(line) + menu_row_gap(line);
    return std::max(1, static_cast<int>((geometry.content.h + menu_row_gap(line)) / step));
}

float LayoutPicker::content_row_step(const MenuGeometry& geometry) const {
    const float line = TextRenderer::line_height(geometry.scale);
    return menu_row_height(line) + menu_row_gap(line);
}

float LayoutPicker::content_max_scroll(const MenuGeometry& geometry) const {
    if (!content_is_list()) return 0.0f;
    return std::max(0.0f, static_cast<float>(content_count(section_) - content_rows_fitting(geometry)));
}

bool LayoutPicker::content_is_list() const {
    return section_ == MenuSection::Options || section_ == MenuSection::ControllerOrder ||
           section_ == MenuSection::Amiibo;
}

void LayoutPicker::keep_item_in_view(float output_width, float output_height) {
    if (!content_is_list()) {
        content_scroll_ = 0.0f;
        return;
    }
    const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
    const float rows = static_cast<float>(content_rows_fitting(geometry));
    if (on_content_) {
        const float at = static_cast<float>(item_);
        if (at < content_scroll_) content_scroll_ = at;
        if (at + 1.0f > content_scroll_ + rows) content_scroll_ = at + 1.0f - rows;
    }
    content_scroll_ = std::clamp(content_scroll_, 0.0f, content_max_scroll(geometry));
}

void LayoutPicker::set_focus(int focus) {
    // One of the popup's buttons, from the other device's finger.
    if (confirm_ == MenuConfirm::Choice) {
        if (focus >= kPopupFocusBase && focus < kWideFocusBase) choice_.set_focus(focus - kPopupFocusBase);
        return;
    }
    if (confirming() && focus >= kPopupFocusBase &&
        focus < kPopupFocusBase + confirm_cursor_.count()) {
        confirm_cursor_.set_focus(focus - kPopupFocusBase);
        return;
    }
    MenuSection section = MenuSection::Continue;
    int item = -1;
    if (!decode_focus(focus, section, item)) return;
    const bool moved = section != section_ || std::max(0, item) != item_ || (item >= 0) != on_content_;
    if (section != section_) content_scroll_ = 0.0f;
    section_ = section;
    on_content_ = item >= 0;
    item_ = std::max(0, item);
    // Another device's cursor, or one put here: the list follows it when it
    // moves, and is otherwise where a finger here left it.
    if (moved) {
        content_drag_.stop();
        keep_item_in_view(last_width_, last_height_);
    }
}

ButtonGlyph LayoutPicker::guide_button_at(float x, float y) const {
    for (const GuideHit& hit : guide_hits_) {
        if (hit.rect.contains(x, y)) {
            return hit.glyph;
        }
    }
    return ButtonGlyph::Count;
}

int LayoutPicker::entry_at(float x, float y, float output_width, float output_height) const {
    if (confirm_ == MenuConfirm::Choice) {
        const int choice = choice_.choice_at(x, y, output_width, output_height);
        return choice >= 0 ? kPopupFocusBase + choice : -1;
    }
    if (confirming()) {
        const int button = popup_button_at(confirm_popup(output_width, output_height), x, y);
        return button >= 0 ? kPopupFocusBase + button : -1;
    }
    const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
    for (std::size_t index = 0; index < geometry.sections.size(); ++index) {
        if (geometry.sections[index].contains(x, y)) return focus_of(sections_[index]);
    }
    for (std::size_t index = 0; index < geometry.items.size(); ++index) {
        // A long list's rows past the area are not on screen to be tapped.
        if (geometry.items[index].contains(x, y) && geometry.content.contains(x, y)) {
            return focus_of(section_, static_cast<int>(index));
        }
    }
    return -1;
}

std::vector<float> LayoutPicker::scroll_state(float, float) const {
    return {content_scroll_, choice_.is_open() ? choice_.scroll_rows() : 0.0f};
}

void LayoutPicker::set_scroll_state(const std::vector<float>& state, float output_width, float output_height) {
    const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
    content_scroll_ = std::clamp(scroll_at(state, 0), 0.0f, content_max_scroll(geometry));
    content_drag_.stop();
    if (choice_.is_open()) choice_.set_scroll_rows(scroll_at(state, 1));
}

bool LayoutPicker::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                          float output_height) {
    if (confirm_ == MenuConfirm::Choice) return choice_.scroll(pointers, output_width, output_height);
    if (confirming() || !content_is_list()) return false;
    // A list on the right, dragged by a finger or turned by the wheel.
    const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
    const float step = content_row_step(geometry);
    float pixels = content_scroll_ * step;
    content_drag_.set_wheel_row(step);
    content_drag_.update(pointers, geometry.content, pixels, content_max_scroll(geometry) * step);
    content_scroll_ = step > 0.0f ? pixels / step : 0.0f;
    return content_drag_.release_was_drag();
}

bool LayoutPicker::off_popup(float x, float y, float output_width, float output_height) const {
    return off_choice_popup(x, y, output_width, output_height);
}

bool LayoutPicker::decode_focus(int focus, MenuSection& section, int& item) const {
    if (focus < 0 || (focus >= kPopupFocusBase && focus < kWideFocusBase && focus != kSaveSyncFocus)) {
        return false;
    }
    if (focus >= kWideFocusBase) {
        section = static_cast<MenuSection>(static_cast<int>(MenuSection::ControllerOrder) +
                                           (focus - kWideFocusBase) / kWideStride);
        item = (focus - kWideFocusBase) % kWideStride - 1;
    } else if (focus == kSaveSyncFocus) {
        section = MenuSection::SaveSync;
        item = -1;
    } else {
        section = static_cast<MenuSection>(focus / kSectionStride);
        item = focus % kSectionStride - 1;
    }
    return has_section(section) && item < content_count(section);
}

void LayoutPicker::handle_tap(int entry, LayoutPickerResult& result) {
    MenuSection section = MenuSection::Continue;
    int item = -1;
    if (!decode_focus(entry, section, item)) return;
    if (item < 0) {
        if (section != section_) content_scroll_ = 0.0f;
        section_ = section;
        on_content_ = false;
        // A section with things of its own shows them, as a tap on a
        // Switch's settings' left-hand list does, the cursor staying on the
        // section; any other -- Continue, Reset, a page of its own -- does
        // what it says.
        if (content_count(section) == 0) activate_section(result);
        return;
    }
    if (section != section_) content_scroll_ = 0.0f;
    section_ = section;
    on_content_ = true;
    item_ = item;
    // Chosen by the tap, as a Switch's are: the finger held on it showed
    // which it was.
    activate_item(result);
}

void LayoutPicker::ask(MenuConfirm confirm) {
    confirm_ = confirm;
    const int count = static_cast<int>(confirm_buttons().size());
    // On the answer, so A then A does what was asked, as it always has; B is
    // always the way back out.
    confirm_cursor_.reset(count, count - 1);
}

PopupText LayoutPicker::confirm_text() const {
    PopupText words;
    // With Resume from Save State on, quitting loses nothing: the game
    // starts from right here next time, so the warning would be untrue.
    const bool quitting_keeps_progress = state_.resume_choosable && state_.resume_from_save_state;
    switch (confirm_) {
        case MenuConfirm::Quit:
            words.message = "Are you sure you want to quit?";
            if (!quitting_keeps_progress) words.lines.push_back("Unsaved progress will be lost.");
            break;
        case MenuConfirm::Reset:
            words.message = "Are you sure you want to reset?";
            words.lines.push_back("Unsaved progress will be lost.");
            break;
        case MenuConfirm::LoadSavePoint: words.message = "Load this save state?"; break;
        case MenuConfirm::OverwriteSavePoint: words.message = "Overwrite this save state?"; break;
        case MenuConfirm::Notice: words.message = save_status_; break;
        case MenuConfirm::Choice:
        case MenuConfirm::None: break;
    }
    return words;
}

std::vector<PopupButton> LayoutPicker::confirm_buttons() const {
    switch (confirm_) {
        case MenuConfirm::Quit: return {{"Cancel", ""}, {"Quit", ""}};
        case MenuConfirm::Reset: return {{"Cancel", ""}, {"Reset", ""}};
        case MenuConfirm::LoadSavePoint: return {{"Cancel", ""}, {"Load", ""}};
        case MenuConfirm::OverwriteSavePoint: return {{"Cancel", ""}, {"Overwrite", ""}};
        case MenuConfirm::Notice: return {{"OK", ""}};
        case MenuConfirm::Choice:
        case MenuConfirm::None: break;
    }
    return {};
}

PopupGeometry LayoutPicker::confirm_popup(float output_width, float output_height) const {
    const float width = popup_body_width(output_width, output_height);
    return popup_geometry(output_width, output_height,
                          popup_text_height(confirm_text(), width, output_height),
                          confirm_buttons());
}

void LayoutPicker::activate_section(LayoutPickerResult& result) {
    switch (section_) {
        case MenuSection::Layout:
        case MenuSection::CreateSavePoint:
        case MenuSection::LoadSavePoint:
        case MenuSection::Options:
        case MenuSection::ControllerOrder:
        case MenuSection::Amiibo:
            if (content_count(section_) > 0) {
                on_content_ = true;
                item_ = 0;
            }
            break;
        // Pages of the device running Cemu, which the caller puts up.
        case MenuSection::GraphicPacks:
            if (state_.graphic_packs_available) result.graphic_packs_requested = true;
            break;
        // Back to the game, the same as B on the list.
        case MenuSection::Continue:
            if (!state_.embedded) result.close_requested = true;
            break;
        // The caller puts the page up, the way it does Settings.
        case MenuSection::SaveSync:
            if (state_.save_sync_available) result.save_sync_requested = true;
            break;
        // Not asked about: unlike resetting or quitting, reconnecting is a
        // menu away and costs nothing but the walk back.
        case MenuSection::Disconnect:
            if (state_.session_active) result.disconnect_requested = true;
            break;
        case MenuSection::Reset:
            if (state_.reset_available) ask(MenuConfirm::Reset);
            break;
        case MenuSection::Quit: ask(MenuConfirm::Quit); break;
    }
}

void LayoutPicker::activate_item(LayoutPickerResult& result) {
    switch (section_) {
        case MenuSection::Layout: {
            if (item_ == tile_count()) {
                // The stylus button: the caller swaps hands and says so. Or,
                // for Separate Screens, the screens.
                if (swap_button_usable()) {
                    result.screens_swapped = true;
                } else if (hand_button_usable()) {
                    result.handedness_changed = true;
                }
                return;
            }
            if (item_ >= tile_count()) return;
            const std::string id = tile_layout(item_).id;
            if (id != selected_layout_id_) {
                set_selected_layout(id);
                result.selection_changed = true;
            }
            return;
        }
        case MenuSection::CreateSavePoint:
            // Saving into an empty slot loses nothing, so it is not asked
            // about; saving over one does.
            if (slot_filled(item_)) {
                ask(MenuConfirm::OverwriteSavePoint);
            } else {
                result.save_slot = item_;
            }
            return;
        case MenuSection::LoadSavePoint:
            // An empty slot has nothing to load, and says Empty.
            if (slot_filled(item_)) ask(MenuConfirm::LoadSavePoint);
            return;
        case MenuSection::Options:
        case MenuSection::ControllerOrder: {
            if (item_ >= static_cast<int>(rows().size())) return;
            // One with several values to choose between opens them beside
            // its row; a switch flips; a page opens.
            int current = -1;
            if (!option_choices(rows()[static_cast<std::size_t>(item_)], current).empty()) {
                open_choice();
                return;
            }
        }
            // Each is cycled by the caller, which tells the menu what it
            // landed on -- so the menu never shows a value that was not
            // actually applied, and on a companion the host decides.
            switch (rows()[static_cast<std::size_t>(item_)].option) {
                case Option::RenderScale: result.render_scale_changed = true; break;
                case Option::Sound: result.audio_target_changed = true; break;
                case Option::Cursor: result.cursor_mode_changed = true; break;
                case Option::RightStick: result.right_stick_mode_changed = true; break;
                case Option::Rumble: result.rumble_pak_toggled = true; break;
                case Option::ScreenGap: result.screen_gap_toggled = true; break;
                case Option::Smoothing: result.filtering_toggled = true; break;
                case Option::LcdFilter: result.lcd_filter_toggled = true; break;
                case Option::CustomTextures: result.custom_textures_toggled = true; break;
                case Option::Resume: result.resume_toggled = true; break;
                case Option::SwapScreens: result.screens_swap_toggled = true; break;
                // Cemu's, on the device running it, which the caller puts up.
                case Option::GraphicPacks:
                    if (state_.graphic_packs_available) result.graphic_packs_requested = true;
                    break;
                case Option::ControllerOrder: result.controllers_requested = true; break;
                case Option::ButtonMapping: result.button_mapping_requested = true; break;
                case Option::Producer:
                    result.producer_option = rows()[static_cast<std::size_t>(item_)].index;
                    break;
            }
            return;
        case MenuSection::Amiibo:
            if (item_ < static_cast<int>(state_.amiibo.size())) {
                result.amiibo_chosen = item_;
            } else {
                result.amiibo_folder_requested = true;
            }
            return;
        case MenuSection::Continue:
        case MenuSection::SaveSync:
        case MenuSection::GraphicPacks:
        case MenuSection::Disconnect:
        case MenuSection::Reset:
        case MenuSection::Quit: return;
    }
}

void LayoutPicker::handle_menu_action(UiAction action, LayoutPickerResult& result) {
    const int count = content_count(section_);
    const auto move_section = [&](int delta) {
        const auto at = std::find(sections_.begin(), sections_.end(), section_);
        const int index = static_cast<int>(at - sections_.begin());
        const int size = static_cast<int>(sections_.size());
        if (size == 0) return;
        // Round from one end to the other on a press -- the one list in the
        // app that does, being short and always the same -- but stopping at
        // the end while a direction is held, so holding it to get to the
        // bottom leaves the cursor there rather than spinning past.
        const bool held = repeating_ && repeating_(delta < 0 ? UiAction::Up : UiAction::Down);
        const int next = held ? std::clamp(index + delta, 0, size - 1) : ((index + delta) % size + size) % size;
        section_ = sections_[static_cast<std::size_t>(next)];
    };
    switch (action) {
        case UiAction::Up:
        case UiAction::Down: {
            const int delta = action == UiAction::Up ? -1 : 1;
            if (!on_content_) {
                move_section(delta);
            } else if (content_columns(section_) > 1) {
                // Up and down a grid, stopping at its edges -- except that
                // down from the layouts' bottom row is the stylus button
                // beneath it, and up from that is the layout nearest it.
                const int cells = grid_count(section_);
                const int next = item_ + delta * content_columns(section_);
                if (item_ >= cells) {
                    if (delta < 0 && cells > 0) item_ = cells - 1;
                } else if (next >= 0 && next < cells) {
                    item_ = next;
                } else if (delta > 0 && count > cells) {
                    item_ = cells;
                }
            } else if (count > 0) {
                item_ = std::clamp(item_ + delta, 0, count - 1);
            }
            break;
        }
        case UiAction::Left:
            // Back to the list from the left-hand edge of whatever is shown.
            if (on_content_ && item_ < grid_count(section_) &&
                item_ % content_columns(section_) != 0) {
                --item_;
            } else {
                on_content_ = false;
            }
            break;
        case UiAction::Right:
            if (!on_content_) {
                // Onto the first of them, top left, whichever is in use.
                if (count > 0) {
                    on_content_ = true;
                    item_ = 0;
                }
            } else if (item_ < grid_count(section_) &&
                       item_ % content_columns(section_) != content_columns(section_) - 1 &&
                       item_ + 1 < grid_count(section_)) {
                ++item_;
            }
            break;
        case UiAction::Confirm:
            if (on_content_) {
                activate_item(result);
            } else {
                activate_section(result);
            }
            break;
        // B backs out a step at a time, the way a Switch's own menus do:
        // from inside a section to the list on the left, and from the list
        // out of the menu. The section is remembered, so the menu opens on it
        // again -- see open().
        case UiAction::Cancel:
            if (on_content_) {
                on_content_ = false;
            } else if (!state_.embedded) {
                result.close_requested = true;
            }
            break;
        // The menu's own button closes it from wherever the cursor is: it is
        // the button that opened it.
        case UiAction::ToggleMenu:
            if (!state_.embedded) {
                result.close_requested = true;
            } else if (on_content_) {
                // Shown in place of the game, there is nothing to close to.
                on_content_ = false;
            }
            break;
        // The app's own quit key, which is not a face button and is not
        // asked about twice.
        case UiAction::Quit: result.quit_requested = true; break;
        // The face buttons that used to be these entries still are them,
        // for hands that remember where they were.
        // Not on a Wii U's: a real GamePad has none of these as face-button
        // shortcuts either, and the game behind it reads every button.
        case UiAction::MenuQuit:
            if (system_ != SystemId::WiiU) ask(MenuConfirm::Quit);
            break;
        case UiAction::MenuReset:
            if (state_.reset_available && system_ != SystemId::WiiU) ask(MenuConfirm::Reset);
            break;
        case UiAction::MenuDisconnect:
            if (state_.session_active && system_ != SystemId::WiiU) result.disconnect_requested = true;
            break;
        // Not a value this menu holds: the caller puts the screen up and this
        // one gets out of the way.
        case UiAction::MenuSettings: result.settings_requested = true; break;
        // R3 is handled by the app, so that it means the same thing here as it
        // does in a game; the app pushes the result back in with
        // set_filtering(). Toggling it again here would undo that.
        case UiAction::ToggleFilter: break;
        // L3 only swaps layouts with a game on screen: here every layout is a
        // move away already.
        case UiAction::NextLayout:
        case UiAction::SwapLayout: break;
        // Escape's second meaning, for screens with a game on them; here its
        // first one -- Cancel, just above -- already closes the menu.
        case UiAction::OpenMenu: break;
        case UiAction::PageUp:
        case UiAction::PageDown: break;  // nothing here scrolls
    }
}

LayoutPickerResult LayoutPicker::update(const std::vector<UiAction>& actions,
                                        const std::vector<PointerEvent>& pointers,
                                        float output_width,
                                        float output_height) {
    LayoutPickerResult result;
    if (!open_) {
        return result;
    }
    last_width_ = output_width;
    last_height_ = output_height;
    // Moved by hand: where the menu was closed before no longer stands.
    if (!actions.empty() || !pointers.empty()) held_place_.reset();

    // An option's choices, beside its row: the answer is the option changed
    // by however far along the choice is from what it was.
    if (confirm_ == MenuConfirm::Choice) {
        const int was = choice_.current();
        const int count = static_cast<int>(choice_.choices().size());
        ChoicePopupResult answer = choice_.update(actions, {}, output_width, output_height);
        // A tap, from this device or the other, heard the one way every tap
        // is: a choice is chosen at once, and off the popup puts it away.
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            if (answer.chosen >= 0 || answer.closed) break;
            if (tap == kTapOffPopup) {
                choice_.close();
                answer.closed = true;
            } else if (tap >= kPopupFocusBase && tap - kPopupFocusBase < count) {
                choice_.close();
                answer.chosen = tap - kPopupFocusBase;
            }
        }
        if (answer.chosen >= 0 && item_ < static_cast<int>(rows().size()) && count > 0) {
            const int steps = was < 0 ? answer.chosen : ((answer.chosen - was) % count + count) % count;
            if (steps > 0 || was < 0) {
                result.option_steps = was < 0 ? answer.chosen + 1 : steps;
                switch (rows()[static_cast<std::size_t>(item_)].option) {
                    case Option::RenderScale: result.render_scale_changed = true; break;
                    case Option::Sound: result.audio_target_changed = true; break;
                    case Option::RightStick: result.right_stick_mode_changed = true; break;
                        case Option::Producer:
                        result.producer_option = rows()[static_cast<std::size_t>(item_)].index;
                        break;
                    default: break;
                }
            }
        }
        if (!choice_.is_open()) confirm_ = MenuConfirm::None;
        if (answer.close_everything) {
            result.close_requested = true;
            open_ = false;
        }
        return result;
    }

    // A confirmation takes the whole menu: while one is up it is the only
    // question on screen, so nothing behind it moves, cycles or opens.
    if (confirming()) {
        const PopupGeometry popup = confirm_popup(output_width, output_height);
        int answer = confirm_cursor_.update(actions, {}, popup);
        const int count = static_cast<int>(popup.buttons.size());
        // A tap on one of its buttons, from this device or the other, presses
        // it.
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            if (answer != PopupCursor::kNone) break;
            if (tap >= kPopupFocusBase && tap - kPopupFocusBase < count) answer = tap - kPopupFocusBase;
        }
        // The last button is the thing asked about -- a notice's only one,
        // its OK -- and the first, where there are two, is Cancel. B is
        // Cancel, and for a notice, with only the one answer, is OK.
        bool accepted = answer == count - 1;
        bool dismissed = answer == PopupCursor::kCancelled || (answer == 0 && count > 1);
        if (confirm_ == MenuConfirm::Notice && dismissed) {
            accepted = true;
        }
        if (accepted) {
            switch (confirm_) {
                case MenuConfirm::Reset:
                    result.reset_requested = true;
                    open_ = false;
                    break;
                case MenuConfirm::Quit:
                    result.quit_requested = true;
                    open_ = false;
                    break;
                case MenuConfirm::LoadSavePoint: result.load_slot = item_; break;
                case MenuConfirm::OverwriteSavePoint: result.save_slot = item_; break;
                case MenuConfirm::Notice:
                case MenuConfirm::Choice:
                case MenuConfirm::None: break;
            }
            confirm_ = MenuConfirm::None;
        } else if (dismissed) {
            confirm_ = MenuConfirm::None;
        }
        return result;
    }

    // A finger on one of the control prompts is that button being pressed.
    std::vector<UiAction> pressed(actions.begin(), actions.end());
    for (const PointerEvent& pointer : pointers) {
        for (const GuideHit& hit : guide_hits_) {
            if (!tapped(pointer, hit.rect)) continue;
            switch (hit.glyph) {
                case ButtonGlyph::A: pressed.push_back(UiAction::Confirm); break;
                case ButtonGlyph::B: pressed.push_back(UiAction::Cancel); break;
                case ButtonGlyph::Plus: pressed.push_back(UiAction::MenuSettings); break;
                default: break;
            }
        }
    }

    {
        const MenuSection section_before = section_;
        const int item_before = item_;
        const bool on_content_before = on_content_;
        for (const UiAction action : pressed) {
            handle_menu_action(action, result);
            if (confirming()) break;
        }
        // The cursor brings a list with it when it moves -- and only then, so
        // a list a finger or the wheel moved stays where it was left.
        if (section_ != section_before || item_ != item_before || on_content_ != on_content_before) {
            if (section_ != section_before) content_scroll_ = 0.0f;
            content_drag_.stop();
            keep_item_in_view(output_width, output_height);
        }
        // Touch: a tap chooses what it lands on -- see handle_tap(). Heard
        // the one way every tap is, this device's and the other's alike.
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            if (confirming()) break;
            handle_tap(tap, result);
        }
        const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
        content_drag_.settle();
        content_scroll_ = std::clamp(content_scroll_, 0.0f, content_max_scroll(geometry));
    }

    if (confirming()) {
        // Raised by a press this frame. Anything else that press implied is
        // dropped: the menu is now waiting on an answer.
        return LayoutPickerResult();
    }

    // Leaving the session is deliberately not one of these. Whether the menu
    // goes away with it is the app's to decide -- it stays up when the press
    // came from the other device -- and a menu that had closed itself here
    // would leave that decision with nothing behind it: a blurred game, no
    // menu on top, and no press this can still answer.
    if (result.close_requested || result.quit_requested || result.reset_requested) {
        open_ = false;
    }
    return result;
}

Handedness LayoutPicker::handedness_of(const std::string& layout_id) const {
    const auto& left = state_.left_handed_layouts;
    return std::find(left.begin(), left.end(), layout_id) != left.end() ? Handedness::Left
                                                                        : Handedness::Right;
}

float LayoutPicker::preview_aspect(float output_width, float output_height) const {
    const float width = display_width_ > 0.0f ? display_width_ : output_width;
    const float height = display_height_ > 0.0f ? display_height_ : output_height;
    return width > 0.0f && height > 0.0f ? width / height : 16.0f / 9.0f;
}

Layout LayoutPicker::resolved_layout_for_preview(const Layout& layout) const {
    Layout resolved = resolve_for_handedness(resolve_gap_choice(layout, state_.screen_gap),
                                             handedness_of(layout.id));
    resolved.fill_display = fill_display_;
    // A screen of each device's own: this one's, alone.
    if (resolved.per_device) resolved = resolve_for_device(resolved, preview_screen_);
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

void LayoutPicker::render_sections(QuadRenderer& quads, TextRenderer& text,
                                   const MenuGeometry& geometry) {
    const UiTheme& theme = ui_theme();
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float row_scale = scale * 1.1f;
    // Right-aligned against the line, the way NSO sets them. The one whose
    // things are showing stays marked while the cursor is over among them.
    for (std::size_t index = 0; index < sections_.size(); ++index) {
        const PixelRect& row = geometry.sections[index];
        const MenuSection section = sections_[index];
        const bool current = section == section_;
        const bool focused = cursor_drawn(current && !on_content_) ||
                             pointer_holding(PixelRect{row.x, row.y, geometry.divider_x - row.x, row.h}, true);
        // As a Switch's own game menus draw theirs: the row under the cursor
        // solid white, running right up to the line, its words in the accent;
        // the one whose things are showing, while the cursor is over among
        // them, a fainter white with dark words.
        const PixelRect bar{row.x, row.y, geometry.divider_x - row.x, row.h};
        Color ink = theme.text;
        if (focused) {
            quads.draw_solid(bar, theme.text);
            ink = theme.accent;
        } else if (current) {
            Color faint = theme.text;
            faint.a = kSelectedTileOpacity;
            quads.draw_solid(bar, faint);
            ink = theme.background;
        }
        text.draw(quads, section_label(section), row.x + row.w - menu_row_pad(line),
                  row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f, row_scale, ink,
                  TextAlign::Right);
    }
    if (!geometry.sections.empty()) {
        quads.draw_solid(PixelRect{geometry.divider_x, geometry.content_top,
                                   std::max(1.0f, std::round(scale * 0.5f)),
                                   geometry.content_bottom - geometry.content_top},
                         theme.text_dim);
    }
}

void LayoutPicker::render_layouts(QuadRenderer& quads, TextRenderer& text,
                                  const ButtonGlyphSheet& glyphs, const MenuGeometry& geometry,
                                  float output_width, float output_height,
                                  const ScreenTextures* textures) {
    const UiTheme& theme = ui_theme();
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const std::size_t tiles = std::min(geometry.items.size(), tiles_.size());
    for (std::size_t index = 0; index < tiles; ++index) {
        const PixelRect& tile = geometry.items[index];
        const Layout& layout = tile_layout(static_cast<int>(index));
        const bool focused =
            cursor_drawn(on_content_ && static_cast<int>(index) == item_) || pointer_holding(tile, true);
        const bool selected = layout.id == selected_layout_id_;

        // Outlines everywhere except under the cursor and on the layout in
        // use, so the blurred game stays visible through the menu over it.
        draw_menu_tile(quads, tile, focused, selected, scale);

        const float padding = std::round(std::min(tile.w, tile.h) * 0.07f);
        const float label_height = line * 1.4f;
        // Shaped like the display the menu is on -- a 16:10 Deck's previews
        // are a little taller than a 16:9 TV's -- and centred in the space the
        // tile has for it, so a layout looks the way it will once picked. The
        // tiles themselves stay the same size whatever the display.
        const PixelRect space{
            tile.x + padding,
            tile.y + padding,
            tile.w - padding * 2.0f,
            tile.h - padding * 2.0f - label_height,
        };
        const float aspect = preview_aspect(output_width, output_height);
        const float preview_width = std::min(space.w, space.h * aspect);
        const float preview_height = std::min(space.h, space.w / aspect);
        const PixelRect preview{
            std::round(space.x + (space.w - preview_width) * 0.5f),
            std::round(space.y + (space.h - preview_height) * 0.5f),
            std::round(preview_width),
            std::round(preview_height),
        };
        draw_preview(quads, resolved_layout_for_preview(layout), preview,
                     display_width_ > 0.0f ? display_width_ : output_width,
                     display_height_ > 0.0f ? display_height_ : output_height, screens_, textures,
                     bezel_art_, smoothing_);

        // Shrunk rather than let run past the edge of a narrow tile.
        float label_scale = scale;
        const float room = tile.w - padding * 2.0f;
        const float natural = TextRenderer::measure_width(layout.display_name, scale);
        if (natural > room && natural > 0.0f) label_scale = scale * (room / natural);
        text.draw(quads, layout.display_name, tile.x + tile.w * 0.5f,
                  tile.y + tile.h - padding - TextRenderer::line_height(label_scale), label_scale,
                  quick_menu_row_text(focused || selected), TextAlign::Center);
    }

    // The stylus button: the Wii U's own symbol in a square, outlined like a
    // layout tile, and mirrored to show which hand the layout in use is
    // for. Not there at all where that layout has no hand to choose.
    const PixelRect& square = geometry.hand_button;
    if (square.w <= 0.0f || !corner_button_usable()) return;
    const bool focused = cursor_drawn(on_content_ && item_ == tile_count()) || pointer_holding(square, true);
    const float border = menu_border(scale);
    // Under the cursor its background takes a wash of white and the symbol
    // stays white on it -- a highlight, not the tiles' full inversion, which
    // on a symbol this size reads as a different symbol.
    if (focused) {
        Color wash = theme.text;
        wash.a = 0.3f;
        quads.draw_solid(square, wash);
    }
    quads.draw_outline(square, theme.text, border);
    if (swap_button_usable()) {
        // Separate Screens' swap button, in the same place.
        glyphs.draw_swap_screens(quads,
                                 PixelRect{square.x + border, square.y + border, square.w - border * 2.0f,
                                           square.h - border * 2.0f},
                                 theme.text);
        return;
    }
    glyphs.draw_stylus_hand(quads,
                            PixelRect{square.x + border, square.y + border,
                                      square.w - border * 2.0f, square.h - border * 2.0f},
                            theme.text, handedness_of(selected_layout_id_) == Handedness::Left);
}

void LayoutPicker::render_slots(QuadRenderer& quads, TextRenderer& text,
                                const MenuGeometry& geometry) {
    const UiTheme& theme = ui_theme();
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);

    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        if (!slot_pictures_dirty_[index]) continue;
        slot_pictures_dirty_[index] = false;
        if (slot_filled(slot) && save_slots_[index].thumbnail.valid()) {
            const SavePointThumbnail& picture = save_slots_[index].thumbnail;
            slot_pictures_[index].upload_xrgb8888(picture.pixels.data(), picture.width,
                                                  picture.height, picture.width);
        } else {
            slot_pictures_[index] = Texture();
        }
    }

    const float border = menu_border(scale);
    for (int slot = 0; slot < kSavePointSlots && slot < static_cast<int>(geometry.items.size());
         ++slot) {
        const auto index = static_cast<std::size_t>(slot);
        const PixelRect& cell = geometry.items[index];
        const bool focused = cursor_drawn(on_content_ && slot == item_) || pointer_holding(cell, true);
        const bool filled = slot_filled(slot);
        // Square, as a Switch's own suspend points are: solid white under
        // the cursor, outlined otherwise.
        if (focused) {
            quads.draw_solid(cell, theme.text);
        } else {
            quads.draw_outline(cell, theme.text, border);
        }

        // The picture takes the tile's height, its two screens one above the
        // other the way the console holds them; its name and time go beside
        // it, a line apart.
        const float name_line = TextRenderer::line_height(scale * 1.1f);
        const float date_line = TextRenderer::line_height(scale * 0.95f);
        const PixelRect inside{cell.x + border, cell.y + border, cell.w - border * 2.0f,
                               cell.h - border * 2.0f};
        const float box_width = std::min(inside.w * 0.5f, inside.h * 0.82f);
        const PixelRect box{inside.x, inside.y, box_width, inside.h};
        quads.draw_solid(box, Color{0.0f, 0.0f, 0.0f, 1.0f});
        if (filled && slot_pictures_[index].valid()) {
            const SavePointThumbnail& picture = save_slots_[index].thumbnail;
            // Fitted into the box without stretching.
            const float fit = std::min(box.w / picture.width, box.h / picture.height);
            const float width = picture.width * fit;
            const float height = picture.height * fit;
            quads.draw_screen(PixelRect{box.x + (box.w - width) * 0.5f,
                                        box.y + (box.h - height) * 0.5f, width, height},
                              slot_pictures_[index]);
        } else if (!filled) {
            quads.draw_solid(box, focused ? theme.text_dim : theme.panel);
        }
        quads.draw_solid(PixelRect{box.x + box.w, cell.y, border, cell.h},
                         focused ? theme.background : theme.text);

        // On the white under the cursor, its words in the favorite color.
        const Color& ink = focused ? theme.accent : theme.text;
        const Color& dim = focused ? theme.accent : theme.text_dim;
        const float text_x = box.x + box.w + border + line * 0.8f;
        const float text_room = cell.x + cell.w - border - line * 0.6f - text_x;
        // The time on two lines, date over clock, where one will not fit.
        std::vector<std::string> when;
        if (filled) {
            const std::string stamp = format_save_time(save_slots_[index].saved_at);
            const std::size_t split = stamp.find(" - ");
            if (TextRenderer::measure_width(stamp, scale * 0.95f) <= text_room ||
                split == std::string::npos) {
                when.push_back(stamp);
            } else {
                when.push_back(stamp.substr(0, split));
                when.push_back(stamp.substr(split + 3));
            }
        } else {
            when.push_back("Empty");
        }
        const float name_step = name_line + line * 0.35f;
        const float date_step = date_line + line * 0.2f;
        const float block = name_step + date_step * static_cast<float>(when.size());
        const float name_y = cell.y + (cell.h - block) * 0.5f;
        text.draw(quads, "Save slot " + std::to_string(slot + 1), text_x, name_y, scale * 1.1f,
                  ink);
        for (std::size_t row = 0; row < when.size(); ++row) {
            text.draw(quads, when[row], text_x,
                      name_y + name_step + date_step * static_cast<float>(row), scale * 0.95f,
                      dim);
        }
    }
}

void LayoutPicker::render_options(QuadRenderer& quads, TextRenderer& text,
                                  const MenuGeometry& geometry) {
    const UiTheme& theme = ui_theme();
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float row_scale = scale * 1.1f;
    // Rows past the bottom are scrolled to, not drawn over the prompts.
    quads.set_clip(menu_list_clip(geometry.content, scale));
    for (std::size_t index = 0; index < geometry.items.size() && index < rows().size(); ++index) {
        const PixelRect& row = geometry.items[index];
        const bool focused = on_content_ && static_cast<int>(index) == item_;
        draw_menu_row(quads, row, focused, scale, index + 1 == std::min(geometry.items.size(), rows().size()),
                      on_content_ && static_cast<int>(index) == item_ + 1);
        const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
        text.draw(quads, option_name(rows()[index]), row.x + menu_row_pad(line), baseline, row_scale,
                  menu_row_text(focused));
        const std::string value = option_value(rows()[index]);
        if (is_switch(rows()[index])) {
            draw_toggle(quads, row.x + row.w - menu_row_pad(line), row.y + row.h * 0.5f, value == "On", line);
        } else {
            text.draw(quads, value, row.x + row.w - menu_row_pad(line), baseline, row_scale,
                      theme.text_dim, TextAlign::Right);
        }
    }
    quads.clear_clip();
    const int fitting = content_rows_fitting(geometry);
    const int count = static_cast<int>(rows().size());
    if (count > fitting) {
        const float span = static_cast<float>(count);
        draw_scroll_bar(quads, geometry.content, std::clamp(content_scroll_, 0.0f, content_max_scroll(geometry)) / span,
                        static_cast<float>(fitting) / span, line, scale);
    }
}

void LayoutPicker::render_amiibo(QuadRenderer& quads, TextRenderer& text,
                                 const MenuGeometry& geometry) {
    const UiTheme& theme = ui_theme();
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float row_scale = scale * 1.1f;
    // Rows past the bottom are scrolled to, not drawn over the prompts.
    quads.set_clip(menu_list_clip(geometry.content, scale));
    const int count = static_cast<int>(geometry.items.size());
    for (int index = 0; index < count; ++index) {
        const PixelRect& row = geometry.items[static_cast<std::size_t>(index)];
        const bool focused = on_content_ && index == item_;
        draw_menu_row(quads, row, focused, scale, index == count - 1, on_content_ && index == item_ + 1);
        const bool folder_row = index >= static_cast<int>(state_.amiibo.size());
        const std::string label =
            folder_row ? std::string("Choose a Folder...") : state_.amiibo[static_cast<std::size_t>(index)];
        const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
        text.draw(quads, TextRenderer::elide_right(label, row.w - line * 1.6f, row_scale),
                  row.x + menu_row_pad(line), baseline, row_scale,
                  folder_row ? theme.text_dim : menu_row_text(focused));
    }
    quads.clear_clip();
    // A word under the folder row when it is the only one: why there is
    // nothing else, and where it looked.
    if (state_.amiibo.empty() && count > 0) {
        const PixelRect& row = geometry.items.back();
        const std::string why = state_.amiibo_folder.empty()
                                    ? "Choose the folder your amiibo are kept in."
                                    : "No amiibo in " + state_.amiibo_folder + ".";
        text.draw_wrapped(quads, why, row.x, row.y + row.h + line * 0.6f, row.w, scale * 0.9f,
                          theme.text_dim);
    }
    const int rows = content_rows_fitting(geometry);
    if (count > rows) {
        const float span = static_cast<float>(count);
        draw_scroll_bar(quads, geometry.content, std::clamp(content_scroll_, 0.0f, content_max_scroll(geometry)) / span,
                        static_cast<float>(rows) / span, line, scale);
    }
}

namespace {

// How wide draw_battery() is at `scale`, so the heading can leave it room.
float battery_width(const BatteryReading& battery, float scale) {
    const float line = TextRenderer::line_height(scale);
    return TextRenderer::measure_width(std::to_string(battery.percent) + "%", scale) +
           line * 0.35f + line * 1.7f;
}

// The percentage, then the battery with as much of it filled as is left, as a
// Switch's HOME menu has them in its top corner: green while charging, red
// once it is low. `right` is where it ends; it is centred on `centre_y`.
//
// Rounded all over and drawn as fills, not lines: the body a faint white, the
// charge a solid one inside it. An outline would need its middle cut out of
// whatever is behind the menu, which here is the blurred game.
void draw_battery(QuadRenderer& quads, TextRenderer& text, const BatteryReading& battery,
                  float right, float centre_y, float scale) {
    const UiTheme& theme = ui_theme();
    const float line = TextRenderer::line_height(scale);
    const float body_w = std::round(line * 1.5f);
    const float body_h = std::round(line * 0.74f);
    const float nub_w = std::max(2.0f, std::round(line * 0.1f));
    const float nub_gap = std::max(1.0f, std::round(line * 0.04f));
    const PixelRect body{std::round(right - nub_w - nub_gap - body_w),
                         std::round(centre_y - body_h * 0.5f), body_w, body_h};
    Color shell = theme.text;
    shell.a = 0.35f;
    quads.draw_rounded_rect(body, body_h * 0.3f, shell);
    const float nub_h = std::round(body_h * 0.4f);
    quads.draw_rounded_rect(PixelRect{body.x + body.w + nub_gap, std::round(centre_y - nub_h * 0.5f),
                                      nub_w, nub_h},
                            nub_w * 0.5f, shell);
    const float inset = std::max(1.5f, std::round(body_h * 0.14f));
    const float room = body.w - inset * 2.0f;
    const float filled = std::max(body_h * 0.2f, room * battery.percent / 100.0f);
    const Color& fill = battery.charging          ? theme.success
                        : battery.percent <= 15   ? theme.danger
                                                  : theme.text;
    const float fill_h = body.h - inset * 2.0f;
    quads.draw_rounded_rect(PixelRect{body.x + inset, body.y + inset, std::round(filled), fill_h},
                            fill_h * 0.3f, fill);
    text.draw(quads, std::to_string(battery.percent) + "%", body.x - line * 0.35f,
              std::round(centre_y - line * 0.5f), scale, theme.text, TextAlign::Right);
}

}  // namespace

void LayoutPicker::render(QuadRenderer& quads,
                          TextRenderer& text,
                          const ButtonGlyphSheet& glyphs,
                          float output_width,
                          float output_height,
                          const std::string& game_title,
                          const ScreenTextures* textures) {
    if (!is_drawn()) {
        return;
    }
    const ShownScroll shown(content_scroll_, content_ease_, content_drag_.dragging(), static_cast<int>(section_),
                            0.01f);
    last_width_ = output_width;
    last_height_ = output_height;
    // Over the row it belongs to, wherever that is drawn now.
    if (confirm_ == MenuConfirm::Choice) {
        const MenuGeometry placed = compute_menu_geometry(output_width, output_height);
        if (item_ >= 0 && item_ < static_cast<int>(placed.items.size())) {
            choice_.set_row(placed.items[static_cast<std::size_t>(item_)]);
        }
    }

    const float scale = UiTheme::base_scale(output_height);
    const bool wii_u = system_ == SystemId::WiiU;

    // The game the menu was opened over, where the heading used to say what
    // the screen already looks like -- in the heading every page has.
    const PageFrame frame = page_frame(output_width, output_height);
    const BatteryReading battery = read_battery();
    const float battery_room =
        battery.present ? battery_width(battery, scale) + frame.line * 1.2f : 0.0f;
    // The game's icon left of its title, as on its Data Management page.
    float title_indent = 0.0f;
    if (game_icon_ != nullptr && game_icon_->valid()) {
        const float icon_size = std::round(frame.line * 1.7f);
        quads.draw_image(PixelRect{frame.panel.x,
                                   std::round(frame.panel.y +
                                              (frame.header_bottom - frame.panel.y - icon_size) * 0.5f),
                                   icon_size, icon_size},
                         *game_icon_);
        title_indent = icon_size + std::round(frame.line * 0.6f);
    }
    draw_page_header(quads, text, frame, game_title, state_.subtitle, title_indent, false,
                     battery_room, /*rules=*/false);
    if (battery.present) {
        draw_battery(quads, text, battery, frame.panel.x + frame.panel.w,
                     std::round((frame.panel.y + frame.header_bottom) * 0.5f), scale);
    }

    std::vector<GuidePrompt> guide;
    {
        const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
        render_sections(quads, text, geometry);
        switch (section_) {
            case MenuSection::Layout:
                render_layouts(quads, text, glyphs, geometry, output_width, output_height,
                               textures);
                break;
            case MenuSection::CreateSavePoint:
            case MenuSection::LoadSavePoint: render_slots(quads, text, geometry); break;
            case MenuSection::Options:
            case MenuSection::ControllerOrder: render_options(quads, text, geometry); break;
            case MenuSection::Amiibo: render_amiibo(quads, text, geometry); break;
            // Nothing to choose between, so nothing on the right: these are
            // only things to pick.
            case MenuSection::Continue:
            case MenuSection::SaveSync:
            case MenuSection::GraphicPacks:
            case MenuSection::Disconnect:
            case MenuSection::Reset:
            case MenuSection::Quit: break;
        }

        // Leftmost, and always: it is the one prompt here that is about the
        // machine rather than about the game, so it sits apart from the rest.
        // Everything else the face buttons used to do is in the list now.
        guide.push_back({ButtonGlyph::Plus, "Settings"});
        // Back either way: off the right-hand side, or out of the menu. One
        // word for B wherever the cursor is, rather than one that changes --
        // and none on the list shown in place of a Wii U game, which B does
        // not leave.
        if (!state_.embedded || on_content_) {
            guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
        }
        // A tile is tapped directly, so this names a button nobody has.
        guide.push_back({ButtonGlyph::A, "OK", PromptRole::ActsOnSelection});
    }
    draw_page_guide(quads, text, glyphs, frame, guide, &guide_hits_);
    // A Wii U's: which screen this device is, at the left of the prompts.
    if (wii_u && device_screen_ && load_ui_icons(ui_icons_, ui_icons_tried_)) {
        const float guide_height = control_guide_height(frame.guide_scale());
        const float size = std::round(guide_height * 1.5f);
        const PixelRect cell{frame.panel.x, std::round(frame.guide_y + (guide_height - size) * 0.5f),
                             size, size};
        draw_ui_icon(quads, ui_icons_,
                     *device_screen_ == ScreenId::Bottom ? UiIcon::WiiUGamePad : UiIcon::Tv, cell,
                     ui_theme().text);
    }

    if (confirm_ == MenuConfirm::Choice) {
        choice_.render(quads, text, output_width, output_height);
    } else if (confirming()) {
        // The popup every question in the app is asked in: see ui/popup.hpp.
        const PopupGeometry popup = confirm_popup(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, confirm_buttons(),
                   confirm_cursor_.focus(), confirm_cursor_.shown());
        draw_popup_text(quads, text, confirm_text(), popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

bool LayoutPicker::is_switch(const OptionRow& row) const {
    int current = -1;
    return is_toggle_value(option_value(row)) && option_choices(row, current).empty();
}

void LayoutPicker::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(section_));
    mix_sound_state(view.focus, on_content_ ? 1u : 0u);
    mix_sound_state(view.focus, static_cast<uint64_t>(item_));
    if (confirm_ != MenuConfirm::None) mix_sound_state(view.focus, static_cast<uint64_t>(confirm_cursor_.focus()));
    if (confirm_ == MenuConfirm::Choice) mix_sound_state(view.focus, static_cast<uint64_t>(choice_.focus() + 1000));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(confirm_));
    mix_sound_state(view.rest, selected_layout_id_);
    mix_sound_state(view.rest, smoothing_ ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(preview_screen_));
    mix_sound_state(view.rest, state_.layout_id);
    mix_sound_state(view.rest, state_.bilinear_filtering ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(state_.audio_target));
    mix_sound_state(view.rest, static_cast<uint64_t>(state_.cursor_mode));
    for (const std::string& id : state_.left_handed_layouts) mix_sound_state(view.rest, id);
    mix_sound_state(view.rest, static_cast<uint64_t>(state_.render_scale));
    mix_sound_state(view.rest, state_.screen_gap ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(state_.right_stick_mode));
    mix_sound_state(view.rest, state_.rumble_pak ? 1u : 0u);
    mix_sound_state(view.rest, state_.custom_textures ? 1u : 0u);
    mix_sound_state(view.rest, state_.resume_from_save_state ? 1u : 0u);
    mix_sound_state(view.rest, save_status_);
    mix_sound_state(view.rest, state_.screens_swapped ? 1u : 0u);
    for (const auto& option : state_.producer_options) mix_sound_state(view.rest, option.second);
    for (const SavePointInfo& slot : save_slots_) {
        mix_sound_state(view.rest, static_cast<uint64_t>(slot.exists ? slot.saved_at : -1));
    }
    view.error = view.error || (!save_status_.empty() && save_status_failed_);
    if (confirm_ != MenuConfirm::None) {
        const std::vector<PopupButton> buttons = confirm_buttons();
        const int at = confirm_cursor_.focus();
        view.on_cancel = at >= 0 && at < static_cast<int>(buttons.size()) &&
                         buttons[static_cast<std::size_t>(at)].label == "Cancel";
    }
    const bool on_list = section_ == MenuSection::Options || section_ == MenuSection::ControllerOrder;
    if (on_list && on_content_ && confirm_ == MenuConfirm::None && item_ >= 0 &&
        item_ < static_cast<int>(rows().size()) && is_switch(rows()[static_cast<std::size_t>(item_)])) {
        view.toggle = option_value(rows()[static_cast<std::size_t>(item_)]) == "On" ? 1 : 0;
    }
}

}  // namespace ds
