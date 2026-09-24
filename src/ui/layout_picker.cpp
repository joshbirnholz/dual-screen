#include "ui/layout_picker.hpp"

#include "ui/control_guide.hpp"

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
    for (std::size_t index = 0; index < registry_->size(); ++index) {
        if (layout_fits_system(registry_->at(index), system_, screens_)) {
            tiles_.push_back(index);
        }
    }
    // The console's own default leads its own menu, if it has one and it is
    // actually offered -- most do not say, and are left in the registry's own
    // order. See SystemProfile::default_layout_id.
    const std::string& default_id = system_profile(system_).default_layout_id;
    if (!default_id.empty()) {
        const auto leader = std::find_if(tiles_.begin(), tiles_.end(), [&](std::size_t index) {
            return registry_->at(index).id == default_id;
        });
        if (leader != tiles_.end() && leader != tiles_.begin()) {
            std::rotate(tiles_.begin(), leader, leader + 1);
        }
    }
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
    open_ = true;
    confirm_ = MenuConfirm::None;
    state_ = state;
    selected_layout_id_ = state.layout_id;
    save_status_.clear();
    save_status_failed_ = false;

    rebuild_footer();
    focus_ = 0;
    // The list, at its top -- the way NSO's menu opens -- with the layouts
    // already showing beside it.
    section_ = sections_.empty() ? MenuSection::Layout : sections_.front();
    on_content_ = false;
    item_ = 0;
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
    footer_.clear();
    sections_.clear();
    options_.clear();
    if (system_ == SystemId::WiiU) {
        // A plain list, the same shape as the main menu -- see LauncherScreen
        // and render()'s Wii U branch. Nothing here is a value to cycle from a
        // control, unlike everything below: a real GamePad has none of these
        // as face-button shortcuts either, so neither does this one. See
        // update()'s Wii U branch for the controls this deliberately ignores.
        //
        // No Resume: Back does that where the menu is opened over the game,
        // and where it is shown in place of it -- see LayoutPickerState::
        // embedded -- the game was never paused and is one window away.
        footer_.push_back(Footer::OpenControllers);
        footer_.push_back(Footer::OpenSettings);
        if (state_.session_active) {
            footer_.push_back(Footer::Disconnect);
        }
        footer_.push_back(Footer::Quit);
        if (focus_ >= footer_count()) {
            focus_ = std::max(0, footer_count() - 1);
        }
        return;
    }

    // The sections, in the order the player asked for them: back to the game,
    // what the game looks like, then saving and loading, then everything
    // else, and last the three that end something.
    sections_.push_back(MenuSection::Continue);
    if (tile_count() > 0) sections_.push_back(MenuSection::Layout);
    if (state_.save_points_available) {
        sections_.push_back(MenuSection::CreateSavePoint);
        sections_.push_back(MenuSection::LoadSavePoint);
    }
    sections_.push_back(MenuSection::Options);
    if (state_.session_active) sections_.push_back(MenuSection::Disconnect);
    if (state_.reset_available) sections_.push_back(MenuSection::Reset);
    sections_.push_back(MenuSection::Quit);

    // Absent only where there is nothing to choose: a system whose core draws
    // at one fixed resolution, or no game at all. A companion gets it like
    // every other entry here -- one menu, reachable from either device -- and
    // asks the host to make the change.
    if (state_.max_render_scale > 1) options_.push_back(Option::RenderScale);
    // Which device makes the sound is only a question when there are two of
    // them, so the entry appears with the session and not before.
    if (state_.session_active) options_.push_back(Option::Sound);
    // The pointer is this device's own, and it is the one thing worth reaching
    // quickly while playing: a trackpad you cannot see is a trackpad you cannot
    // aim.
    if (state_.cursor_choosable) options_.push_back(Option::Cursor);
    // What the right stick does, where the console has one of its own to have
    // an opinion about -- a 3DS.
    if (state_.right_stick_choosable) options_.push_back(Option::RightStick);
    // What L3 does, where the console has a microphone for it to be.
    if (state_.left_stick_choosable) options_.push_back(Option::LeftStick);
    // The gap is one choice for every layout that has one -- Vertical Screens
    // and the two focus layouts; see LayoutComposition::gap_toggleable -- so
    // it is offered whichever layout is in use, and waits for those.
    options_.push_back(Option::ScreenGap);
    // R3 still does this without opening anything; this is where it is seen.
    if (state_.smoothing_available) options_.push_back(Option::Smoothing);

    // The cursor can be sitting on something that just went away.
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
        case MenuSection::Layout: return tile_count() + (hand_button_usable() ? 1 : 0);
        case MenuSection::CreateSavePoint:
        case MenuSection::LoadSavePoint: return kSavePointSlots;
        case MenuSection::Options: return static_cast<int>(options_.size());
        case MenuSection::Continue:
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

std::string LayoutPicker::footer_label(Footer kind) const {
    switch (kind) {
        case Footer::OpenControllers: return "Controller Order";
        case Footer::OpenSettings: return "Settings";
        case Footer::Disconnect: return "Disconnect";
        case Footer::Quit: return "Quit";
    }
    return std::string();
}

std::string LayoutPicker::option_name(Option option) const {
    switch (option) {
        case Option::RenderScale: return "Render Scale";
        case Option::Sound: return "Sound";
        case Option::Cursor: return "Pointer";
        case Option::RightStick: return "Right Stick";
        case Option::LeftStick: return "Left Stick Click";
        case Option::ScreenGap: return "Screen Gap";
        case Option::Smoothing: return "Screen Smoothing";
    }
    return std::string();
}

std::string LayoutPicker::option_value(Option option) const {
    switch (option) {
        case Option::RenderScale: return std::to_string(state_.render_scale) + "x";
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
        case Option::LeftStick: return to_string(state_.left_stick_press);
        case Option::ScreenGap: return state_.screen_gap ? "On" : "Off";
        case Option::Smoothing: return state_.bilinear_filtering ? "On" : "Off";
    }
    return std::string();
}

LayoutPicker::Geometry LayoutPicker::compute_geometry(float output_width, float output_height) const {
    // A plain vertical list, laid out the same way LauncherScreen lays out the
    // main menu's rows -- see render()'s Wii U branch, which draws these with
    // draw_menu_row().
    Geometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float margin = std::round(output_width * 0.12f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.10f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.20f)};
    const float line = TextRenderer::line_height(geometry.scale);
    const float row_height = line * 2.4f;
    const float row_gap = line * 0.5f;
    // Room for the game title above the list.
    const float top = geometry.panel.y + line * 3.5f;
    for (std::size_t index = 0; index < footer_.size(); ++index) {
        geometry.footer.push_back(PixelRect{
            geometry.panel.x,
            top + static_cast<float>(index) * (row_height + row_gap),
            geometry.panel.w,
            row_height,
        });
    }
    return geometry;
}

namespace {

const char* section_label(MenuSection section) {
    switch (section) {
        case MenuSection::Continue: return "Continue";
        case MenuSection::Layout: return "Screen Layout";
        case MenuSection::CreateSavePoint: return "Create Save State";
        case MenuSection::LoadSavePoint: return "Load Save State";
        case MenuSection::Options: return "Options";
        case MenuSection::Disconnect: return "Disconnect";
        case MenuSection::Reset: return "Reset Game";
        case MenuSection::Quit: return "Quit";
    }
    return "";
}

}  // namespace

LayoutPicker::MenuGeometry LayoutPicker::compute_menu_geometry(float output_width,
                                                               float output_height) const {
    MenuGeometry geometry;
    geometry.scale = UiTheme::base_scale(output_height);
    const float scale = geometry.scale;
    const float line = TextRenderer::line_height(scale);
    const float margin = std::round(output_width * 0.06f);
    geometry.panel = PixelRect{margin, std::round(output_height * 0.06f),
                               output_width - margin * 2.0f,
                               output_height - std::round(output_height * 0.12f)};
    geometry.content_top = geometry.panel.y + line * 3.2f;
    geometry.content_bottom = geometry.panel.y + geometry.panel.h -
                              control_guide_height(scale * 0.9f) - line * 1.2f;

    // The list is as wide as its longest entry needs and no wider, so the
    // pictures get the rest.
    float label_width = 0.0f;
    for (const MenuSection section : sections_) {
        label_width =
            std::max(label_width, TextRenderer::measure_width(section_label(section), scale * 1.1f));
    }
    const float left_width = std::min(geometry.panel.w * 0.34f, label_width + line * 2.0f);
    const float row_height = line * 2.2f;
    const float row_step = row_height + line * 0.25f;
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
        case MenuSection::Options: {
            const float option_height = line * 2.2f;
            for (int index = 0; index < count; ++index) {
                geometry.items.push_back(PixelRect{area.x, area.y + index * (option_height + line * 0.25f),
                                                   area.w, option_height});
            }
            break;
        }
        case MenuSection::Continue:
        case MenuSection::Disconnect:
        case MenuSection::Reset:
        case MenuSection::Quit: break;
    }
    return geometry;
}

void LayoutPicker::show(const LayoutPickerState& state, int focus, MenuConfirm confirm) {
    // What the menu has depends on what is being offered, and the layout in
    // use decides whether one option is there at all, so it is rebuilt before
    // the cursor is placed -- a focus means nothing until the entries it
    // points at are the right ones.
    const bool entries_changed = state.layout_id != selected_layout_id_ ||
                                 state.session_active != state_.session_active ||
                                 state.max_render_scale != state_.max_render_scale ||
                                 state.cursor_choosable != state_.cursor_choosable ||
                                 state.right_stick_choosable != state_.right_stick_choosable ||
                                 state.left_stick_choosable != state_.left_stick_choosable ||
                                 state.save_points_available != state_.save_points_available ||
                                 state.reset_available != state_.reset_available ||
                                 state.smoothing_available != state_.smoothing_available;
    state_ = state;
    selected_layout_id_ = state.layout_id;
    if (entries_changed) {
        rebuild_footer();
    }
    set_focus(focus);
    confirm_ = confirm;
}

void LayoutPicker::set_session_active(bool active) {
    if (active == state_.session_active) {
        return;
    }
    state_.session_active = active;
    rebuild_footer();
}

int LayoutPicker::focus() const {
    if (system_ == SystemId::WiiU) return focus_;
    return focus_of(section_, on_content_ ? item_ : -1);
}

void LayoutPicker::set_focus(int focus) {
    if (system_ == SystemId::WiiU) {
        if (focus >= 0 && focus < footer_count()) {
            focus_ = focus;
        }
        return;
    }
    if (focus < 0) return;
    const auto section = static_cast<MenuSection>(focus / kSectionStride);
    const int item = focus % kSectionStride - 1;
    if (!has_section(section) || item >= content_count(section)) return;
    section_ = section;
    on_content_ = item >= 0;
    item_ = std::max(0, item);
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
    if (system_ == SystemId::WiiU) {
        const Geometry geometry = compute_geometry(output_width, output_height);
        for (std::size_t index = 0; index < geometry.footer.size(); ++index) {
            if (geometry.footer[index].contains(x, y)) return static_cast<int>(index);
        }
        return -1;
    }
    const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
    for (std::size_t index = 0; index < geometry.sections.size(); ++index) {
        if (geometry.sections[index].contains(x, y)) return focus_of(sections_[index]);
    }
    for (std::size_t index = 0; index < geometry.items.size(); ++index) {
        if (geometry.items[index].contains(x, y)) {
            return focus_of(section_, static_cast<int>(index));
        }
    }
    return -1;
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

void LayoutPicker::move_focus(int delta_y) {
    const int next = focus_ + delta_y;
    if (next >= 0 && next < footer_count()) {
        focus_ = next;
    }
}

bool LayoutPicker::activate_focus(LayoutPickerResult& result) {
    if (focus_ < 0 || focus_ >= footer_count()) {
        return false;
    }
    switch (footer_[static_cast<std::size_t>(focus_)]) {
        case Footer::OpenControllers:
            result.controllers_requested = true;
            return true;
        case Footer::OpenSettings:
            result.settings_requested = true;
            return true;
        case Footer::Disconnect:
            if (state_.session_active) result.disconnect_requested = true;
            return true;
        case Footer::Quit:
            confirm_ = MenuConfirm::Quit;
            return true;
    }
    return false;
}

void LayoutPicker::activate_section(LayoutPickerResult& result) {
    switch (section_) {
        case MenuSection::Layout:
        case MenuSection::CreateSavePoint:
        case MenuSection::LoadSavePoint:
        case MenuSection::Options:
            if (content_count(section_) > 0) {
                on_content_ = true;
                item_ = 0;
            }
            break;
        // Back to the game, the same as B on the list.
        case MenuSection::Continue:
            if (!state_.embedded) result.close_requested = true;
            break;
        // Not asked about: unlike resetting or quitting, reconnecting is a
        // menu away and costs nothing but the walk back.
        case MenuSection::Disconnect:
            if (state_.session_active) result.disconnect_requested = true;
            break;
        case MenuSection::Reset:
            if (state_.reset_available) confirm_ = MenuConfirm::Reset;
            break;
        case MenuSection::Quit: confirm_ = MenuConfirm::Quit; break;
    }
}

void LayoutPicker::activate_item(LayoutPickerResult& result) {
    switch (section_) {
        case MenuSection::Layout: {
            if (item_ == tile_count()) {
                // The stylus button: the caller swaps hands and says so.
                if (hand_button_usable()) result.handedness_changed = true;
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
                confirm_ = MenuConfirm::OverwriteSavePoint;
            } else {
                result.save_slot = item_;
            }
            return;
        case MenuSection::LoadSavePoint:
            // An empty slot has nothing to load, and says Empty.
            if (slot_filled(item_)) confirm_ = MenuConfirm::LoadSavePoint;
            return;
        case MenuSection::Options:
            if (item_ >= static_cast<int>(options_.size())) return;
            // Each is cycled by the caller, which tells the menu what it
            // landed on -- so the menu never shows a value that was not
            // actually applied, and on a companion the host decides.
            switch (options_[static_cast<std::size_t>(item_)]) {
                case Option::RenderScale: result.render_scale_changed = true; break;
                case Option::Sound: result.audio_target_changed = true; break;
                case Option::Cursor: result.cursor_mode_changed = true; break;
                case Option::RightStick: result.right_stick_mode_changed = true; break;
                case Option::LeftStick: result.left_stick_press_changed = true; break;
                case Option::ScreenGap: result.screen_gap_toggled = true; break;
                case Option::Smoothing: result.filtering_toggled = true; break;
            }
            return;
        case MenuSection::Continue:
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
        section_ = sections_[static_cast<std::size_t>((index + delta + size) % size)];
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
                item_ = (item_ + delta + count) % count;
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
        // One step at a time, the way NSO backs out: off the right-hand side,
        // then out of the menu.
        case UiAction::Cancel:
        case UiAction::ToggleMenu:
            if (on_content_) {
                on_content_ = false;
            } else if (!state_.embedded) {
                result.close_requested = true;
            }
            break;
        // The app's own quit key, which is not a face button and is not
        // asked about twice.
        case UiAction::Quit: result.quit_requested = true; break;
        // The face buttons that used to be these entries still are them,
        // for hands that remember where they were.
        case UiAction::MenuQuit: confirm_ = MenuConfirm::Quit; break;
        case UiAction::MenuReset:
            if (state_.reset_available) confirm_ = MenuConfirm::Reset;
            break;
        case UiAction::MenuDisconnect:
            if (state_.session_active) result.disconnect_requested = true;
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
            if (tapped(pointer, confirm.accept)) accepted = true;
            else if (tapped(pointer, confirm.dismiss)) dismissed = true;
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
                case ButtonGlyph::Minus: pressed.push_back(UiAction::MenuSettings); break;
                default: break;
            }
        }
    }

    if (system_ == SystemId::WiiU) {
        const Geometry geometry = compute_geometry(output_width, output_height);
        for (const UiAction action : pressed) {
            switch (action) {
                case UiAction::Up: move_focus(-1); break;
                case UiAction::Down: move_focus(1); break;
                case UiAction::Confirm: activate_focus(result); break;
                // Nothing to close where the menu is the screen itself.
                case UiAction::Cancel:
                case UiAction::ToggleMenu:
                    if (!state_.embedded) result.close_requested = true;
                    break;
                case UiAction::Quit: result.quit_requested = true; break;
                // Everything else is in the list, reached by A; a controller
                // that sends one of the DS menu's shortcuts anyway (a shared
                // keybinding, a macro) should not reach past it.
                default: break;
            }
        }
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released) continue;
            for (std::size_t index = 0; index < geometry.footer.size(); ++index) {
                if (tapped(pointer, geometry.footer[index])) {
                    focus_ = static_cast<int>(index);
                    activate_focus(result);
                }
            }
        }
    } else {
        for (const UiAction action : pressed) {
            handle_menu_action(action, result);
            if (confirming()) break;
        }
        // Touch: a tap picks the thing under the finger directly.
        const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
        for (const PointerEvent& pointer : pointers) {
            if (!pointer.released || confirming()) continue;
            bool hit = false;
            for (std::size_t index = 0; index < geometry.sections.size() && !hit; ++index) {
                if (!tapped(pointer, geometry.sections[index])) continue;
                section_ = sections_[index];
                on_content_ = false;
                activate_section(result);
                hit = true;
            }
            for (std::size_t index = 0; index < geometry.items.size() && !hit; ++index) {
                if (!tapped(pointer, geometry.items[index])) continue;
                on_content_ = true;
                item_ = static_cast<int>(index);
                activate_item(result);
                hit = true;
            }
        }
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

Layout LayoutPicker::resolved_layout_for_preview(const Layout& layout) const {
    Layout resolved = resolve_for_handedness(resolve_gap_choice(layout, state_.screen_gap),
                                             handedness_of(layout.id));
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
        const bool focused = current && !on_content_;
        if (focused) {
            quads.draw_solid(row, theme.text);
        } else if (current) {
            quads.draw_solid(row, theme.panel_focused);
        }
        const Color& ink = focused ? theme.background : theme.text;
        text.draw(quads, section_label(section), row.x + row.w - line * 0.8f,
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
        const bool focused = on_content_ && static_cast<int>(index) == item_;
        const bool selected = layout.id == selected_layout_id_;

        // Outlines everywhere except under the cursor and on the layout in
        // use, so the blurred game stays visible through the menu over it.
        draw_menu_tile(quads, tile, focused, selected, scale);

        const float padding = std::round(std::min(tile.w, tile.h) * 0.07f);
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

        // Shrunk rather than let run past the edge of a narrow tile.
        float label_scale = scale;
        const float room = tile.w - padding * 2.0f;
        const float natural = TextRenderer::measure_width(layout.display_name, scale);
        if (natural > room && natural > 0.0f) label_scale = scale * (room / natural);
        text.draw(quads, layout.display_name, tile.x + tile.w * 0.5f,
                  tile.y + tile.h - padding - TextRenderer::line_height(label_scale), label_scale,
                  menu_row_text(focused || selected), TextAlign::Center);
    }

    // The stylus button: the Wii U's own symbol in a square, outlined like a
    // layout tile, and mirrored to show which hand the layout in use is
    // for. Not there at all where that layout has no hand to choose.
    const PixelRect& square = geometry.hand_button;
    if (square.w <= 0.0f || !hand_button_usable()) return;
    const bool focused = on_content_ && item_ == tile_count();
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
        const bool focused = on_content_ && slot == item_;
        const bool filled = slot_filled(slot);
        draw_menu_tile(quads, cell, focused, false, scale);

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

        const Color& ink = focused ? theme.background : theme.text;
        const Color dim = focused ? theme.panel : theme.text_dim;
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
    for (std::size_t index = 0; index < geometry.items.size() && index < options_.size(); ++index) {
        const PixelRect& row = geometry.items[index];
        const bool focused = on_content_ && static_cast<int>(index) == item_;
        draw_menu_row(quads, row, focused, scale);
        const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
        text.draw(quads, option_name(options_[index]), row.x + line * 0.8f, baseline, row_scale,
                  menu_row_text(focused));
        text.draw(quads, option_value(options_[index]), row.x + row.w - line * 0.8f, baseline,
                  row_scale, focused ? theme.background : theme.text_dim, TextAlign::Right);
    }
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
    const float scale = UiTheme::base_scale(output_height);
    const float line = TextRenderer::line_height(scale);
    const bool wii_u = system_ == SystemId::WiiU;
    const PixelRect panel = wii_u ? compute_geometry(output_width, output_height).panel
                                  : compute_menu_geometry(output_width, output_height).panel;

    // The game the menu was opened over, where the heading used to say what
    // the screen already looks like.
    if (!game_title.empty()) {
        text.draw(quads, game_title, panel.x, panel.y, scale * 1.6f, theme.text);
    }
    if (!state_.subtitle.empty()) {
        text.draw(quads, state_.subtitle, panel.x, panel.y + line * 1.9f, scale * 0.9f,
                  theme.text_dim);
    }

    std::vector<GuidePrompt> guide;
    if (wii_u) {
        // A plain list, drawn the same way LauncherScreen draws the main
        // menu's rows.
        const Geometry geometry = compute_geometry(output_width, output_height);
        for (std::size_t index = 0; index < geometry.footer.size(); ++index) {
            const PixelRect& row = geometry.footer[index];
            const bool focused = (static_cast<int>(index) == focus_);
            draw_menu_row(quads, row, focused, scale);

            const float pad = line * 0.7f;
            text.draw(quads, footer_label(footer_[index]), row.x + pad,
                      row.y + (row.h - TextRenderer::line_height(scale * 1.2f)) * 0.5f, scale * 1.2f,
                      menu_row_text(focused));
        }
        // Everything this menu can do lives in the list above, reached by A.
        guide.push_back({ButtonGlyph::A, "OK", PromptRole::ActsOnSelection});
        if (!state_.embedded) {
            guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
        }
    } else {
        const MenuGeometry geometry = compute_menu_geometry(output_width, output_height);
        render_sections(quads, text, geometry);
        switch (section_) {
            case MenuSection::Layout:
                render_layouts(quads, text, glyphs, geometry, output_width, output_height,
                               textures);
                break;
            case MenuSection::CreateSavePoint:
            case MenuSection::LoadSavePoint: render_slots(quads, text, geometry); break;
            case MenuSection::Options: render_options(quads, text, geometry); break;
            // Nothing to choose between, so nothing on the right: these are
            // only things to pick.
            case MenuSection::Continue:
            case MenuSection::Disconnect:
            case MenuSection::Reset:
            case MenuSection::Quit: break;
        }
        const bool saving = section_ == MenuSection::CreateSavePoint ||
                            section_ == MenuSection::LoadSavePoint;
        if (saving && !save_status_.empty()) {
            text.draw(quads, save_status_, geometry.panel.x,
                      geometry.panel.y + geometry.panel.h - control_guide_height(scale * 0.9f) +
                          line * 0.1f,
                      scale * 0.95f, save_status_failed_ ? theme.danger : theme.success);
        }

        // Leftmost, and always: it is the one prompt here that is about the
        // machine rather than about the game, so it sits apart from the rest.
        // Everything else the face buttons used to do is in the list now.
        guide.push_back({ButtonGlyph::Minus, "Settings"});
        // Back either way: off the right-hand side, or out of the menu. One
        // word for B wherever the cursor is, rather than one that changes.
        guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
        // A tile is tapped directly, so this names a button nobody has.
        guide.push_back({ButtonGlyph::A, "OK", PromptRole::ActsOnSelection});
    }
    draw_control_guide(quads, text, glyphs, guide, panel.x + panel.w,
                       panel.y + panel.h - control_guide_height(scale * 0.9f), scale * 0.9f,
                       theme.text, TextAlign::Right, &guide_hits_);

    if (confirming()) {
        const ConfirmGeometry confirm = compute_confirm_geometry(output_width, output_height);
        // Everything the confirmation is asking about goes back another step,
        // so there is one thing on screen to answer.
        quads.draw_solid(PixelRect{0.0f, 0.0f, output_width, output_height},
                         Color{0.0f, 0.0f, 0.0f, 0.5f});
        quads.draw_solid(confirm.dialog, theme.text);

        const char* question = "Are you sure you want to quit?\nUnsaved progress will be lost.";
        switch (confirm_) {
            case MenuConfirm::Reset:
                question = "Are you sure you want to reset?\nUnsaved progress will be lost.";
                break;
            case MenuConfirm::LoadSavePoint: question = "Load this save state?"; break;
            case MenuConfirm::OverwriteSavePoint: question = "Overwrite this save state?"; break;
            case MenuConfirm::Quit:
            case MenuConfirm::None: break;
        }
        text.draw_wrapped(quads, question, confirm.dialog.x + confirm.dialog.w * 0.5f,
                          confirm.dialog.y + line * 1.2f, confirm.dialog.w - line * 2.0f,
                          scale * 1.15f, theme.background, TextAlign::Center);

        // Both halves keep the default role, and must. Everywhere else Back
        // is the way out of a screen and a remote has a button for it; here
        // the two prompts *are* the two answers, and dropping one would leave
        // the question above a lone OK.
        draw_control_guide(quads, text, glyphs,
                           {{ButtonGlyph::A, "OK"}, {ButtonGlyph::B, "Back"}},
                           confirm.dialog.x + confirm.dialog.w * 0.5f, confirm.accept.y,
                           scale * 1.05f, theme.background);
    }
}

}  // namespace ds
