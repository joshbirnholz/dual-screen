#include "ui/wii_u_player_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/pad_slot_view.hpp"
#include "ui/page_frame.hpp"
#include "ui/ui_icons.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

void WiiUPlayerScreen::open(PadSlot slot) {
    open_ = true;
    slot_ = slot;
    focus_ = 0;
    scroll_ = 0.0f;
    drag_.stop();
    choice_.close();
    adding_ = false;
    pending_type_ = -1;
}

void WiiUPlayerScreen::open_adding(PadSlot slot) {
    open(slot);
    adding_ = true;
}

void WiiUPlayerScreen::set_controllers(const std::vector<ControllerRowView>& controllers,
                                       std::vector<std::string> types, int type_current) {
    controllers_ = controllers;
    types_ = slot_.kind == PadSlot::Kind::Player ? std::move(types) : std::vector<std::string>();
    type_current_ = type_current;
    if (adding_) {
        // What it starts as, or the first there is -- never None.
        const auto none = [&](int type) {
            return type < 0 || type >= static_cast<int>(types_.size()) ||
                   !offered_type(types_[static_cast<std::size_t>(type)]);
        };
        if (none(pending_type_)) pending_type_ = none(type_current_) ? offered_instead("None", types_) : type_current_;
    }
    focus_ = std::clamp(focus_, 0, static_cast<int>(rows().size()) - 1);
}

void WiiUPlayerScreen::set_profile_switching(ProfileSwitching switching, std::vector<std::string> profiles,
                                             std::string mapping_profile) {
    switching_ = std::move(switching);
    profile_names_ = std::move(profiles);
    mapping_profile_ = std::move(mapping_profile);
    focus_ = std::clamp(focus_, 0, static_cast<int>(rows().size()) - 1);
}

std::vector<WiiUPlayerScreen::Row> WiiUPlayerScreen::rows() const {
    std::vector<Row> rows;
    if (!types_.empty()) rows.push_back(Row::Type);
    if (adding_) {
        rows.push_back(Row::Add);
        return rows;
    }
    rows.push_back(Row::Mapping);
    rows.push_back(Row::Switching);
    if (switching_.on()) {
        for (std::size_t index = 0; index < profile_names_.size(); ++index) rows.push_back(Row::Profile);
    }
    if (removable()) rows.push_back(Row::Remove);
    return rows;
}

int WiiUPlayerScreen::profile_of_row(int row) const {
    const std::vector<Row> all = rows();
    if (row < 0 || row >= static_cast<int>(all.size()) || all[static_cast<std::size_t>(row)] != Row::Profile) return -1;
    int profile = 0;
    for (int index = 0; index < row; ++index) {
        if (all[static_cast<std::size_t>(index)] == Row::Profile) ++profile;
    }
    return profile;
}

WiiUPlayerScreen::Row WiiUPlayerScreen::focused_row() const {
    const std::vector<Row> all = rows();
    return all[static_cast<std::size_t>(std::clamp(focus_, 0, static_cast<int>(all.size()) - 1))];
}

std::string WiiUPlayerScreen::title() const { return pad_slot_name(slot_); }

const ControllerRowView* WiiUPlayerScreen::own_row() const {
    if (adding_) return nullptr;
    for (const ControllerRowView& row : controllers_) {
        if (row.slot == slot_ && row.slot.kind != PadSlot::Kind::NotUsed) return &row;
    }
    return nullptr;
}

WiiUPlayerScreen::Geometry WiiUPlayerScreen::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    geometry.line = frame.line;
    geometry.list = PixelRect{frame.panel.x, frame.content_top, frame.panel.w, frame.content_bottom - frame.content_top};
    const float row_height = menu_row_height(frame.line);
    const float step = row_height + menu_row_gap(frame.line);
    const std::vector<Row> all = rows();
    // Room for Switch Between over the first profile.
    const float heading = std::round(frame.line * 2.6f);
    float y = 0.0f;
    std::vector<float> tops;
    for (std::size_t index = 0; index < all.size(); ++index) {
        if (all[index] == Row::Profile && (index == 0 || all[index - 1] != Row::Profile)) {
            geometry.heading_y = y;
            y += heading;
        }
        tops.push_back(y);
        y += step;
    }
    geometry.content_height = y;
    const bool scrolls = geometry.max_scroll() > 0.5f;
    const float row_width = frame.panel.w - (scrolls ? scroll_bar_width(frame.scale) + frame.line * 0.6f : 0.0f);
    for (const float top : tops) {
        geometry.rows.push_back(PixelRect{frame.panel.x, frame.content_top - scroll_ + top, row_width, row_height});
    }
    geometry.heading_y += frame.content_top - scroll_;
    return geometry;
}

void WiiUPlayerScreen::scroll_focus_into_view(const Geometry& geometry) {
    if (focus_ < 0 || focus_ >= static_cast<int>(geometry.rows.size())) return;
    const PixelRect& focused = geometry.rows[static_cast<std::size_t>(focus_)];
    // The first profile brings Switch Between with it.
    const float top = profile_of_row(focus_) == 0 ? geometry.heading_y : focused.y;
    if (top < geometry.list.y) {
        scroll_ -= geometry.list.y - top;
    } else if (focused.y + focused.h > geometry.list.y + geometry.list.h) {
        scroll_ += focused.y + focused.h - geometry.list.y - geometry.list.h;
    }
    scroll_ = std::clamp(scroll_, 0.0f, geometry.max_scroll());
}

void WiiUPlayerScreen::open_choice(float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    const PixelRect& row = geometry.rows[static_cast<std::size_t>(std::clamp(focus_, 0, static_cast<int>(geometry.rows.size()) - 1))];
    choice_types_.clear();
    if (focused_row() == Row::Switching) {
        // Off, then each of the two it can be, drawn as the buttons are.
        const int current = switching_.button == PadControl::Minus  ? 1
                            : switching_.button == PadControl::Plus ? 2
                                                                    : 0;
        choice_.open({"Off", "Hold", "Hold"}, current, row);
        choice_.set_trailing_glyphs({ButtonGlyph::Count, ButtonGlyph::Minus, ButtonGlyph::Plus}, glyph_aspect_);
        return;
    }
    if (focused_row() != Row::Type) return;
    // Cemu's, but for None -- a player there is no more is Remove
    // Controller's -- and a Classic Controller alone, which a Wii U never
    // sees: one is always plugged into a Wii Remote, which is Wii Remote +
    // Classic Controller.
    std::vector<std::string> labels;
    int current = 0;
    const int now = adding_ ? pending_type_ : type_current_;
    std::vector<UiIcon> icons;
    for (std::size_t index = 0; index < types_.size(); ++index) {
        if (!offered_type(types_[index])) continue;
        if (static_cast<int>(index) == now) current = static_cast<int>(labels.size());
        labels.push_back(wii_u_type_name(types_[index]));
        icons.push_back(wii_u_type_icon(types_[index]));
        choice_types_.push_back(static_cast<int>(index));
    }
    choice_.open(std::move(labels), current, row);
    // Each drawn as the controller it is; the strip is loaded as the page is
    // drawn.
    choice_.set_icons(&icons_, std::move(icons));
}

void WiiUPlayerScreen::choose(int index, WiiUPlayerScreenResult& result) {
    if (focused_row() == Row::Switching) {
        const PadControl button = index == 1 ? PadControl::Minus : index == 2 ? PadControl::Plus : PadControl::Count;
        if (button == switching_.button) return;
        switching_.button = button;
        result.switching_changed = switching_;
        return;
    }
    if (focused_row() != Row::Type || index < 0 || index >= static_cast<int>(choice_types_.size())) return;
    const int type = choice_types_[static_cast<std::size_t>(index)];
    if (adding_) {
        pending_type_ = type;
    } else if (type != type_current_) {
        result.type_chosen = type;
    }
}

void WiiUPlayerScreen::act(WiiUPlayerScreenResult& result, float output_width, float output_height) {
    if (focused_row() == Row::Remove) {
        if (!removable()) return;
        // A player there is no more has no page.
        result.removed = true;
        result.close_requested = true;
    } else if (focused_row() == Row::Mapping) {
        result.edit_mapping = true;
    } else if (focused_row() == Row::Profile) {
        // A profile in, or out, kept in the order they were saved.
        const int profile = profile_of_row(focus_);
        if (profile < 0) return;
        const std::string& name = profile_names_[static_cast<std::size_t>(profile)];
        ProfileSwitching changed = switching_;
        changed.profiles.clear();
        for (const std::string& each : profile_names_) {
            const bool in = each == name ? !switching_.includes(name) : switching_.includes(each);
            if (in) changed.profiles.push_back(each);
        }
        switching_ = changed;
        result.switching_changed = switching_;
    } else if (focused_row() == Row::Add) {
        result.added = true;
        result.type_chosen = pending_type_;
        result.close_requested = true;
    } else {
        open_choice(output_width, output_height);
    }
}

bool WiiUPlayerScreen::removable() const { return !adding_ && slot_.kind == PadSlot::Kind::Player; }

WiiUPlayerScreenResult WiiUPlayerScreen::update(const std::vector<UiAction>& pressed,
                                                const std::vector<PointerEvent>& touched, float output_width,
                                                float output_height) {
    WiiUPlayerScreenResult result;
    if (!open_) return result;
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    // A tap on a prompt at the foot of the page is that button pressed --
    // but not while the popup is up over them.
    if (!choice_.is_open()) take_guide_taps(guide_hits_, actions, pointers);
    if (choice_.is_open()) {
        const ChoicePopupResult answer = choice_.update(actions, {}, output_width, output_height);
        if (answer.close_everything) {
            result.close_requested = true;
            result.close_everything = true;
            close();
            return result;
        }
        if (answer.chosen >= 0) choose(answer.chosen, result);
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
        return result;
    }
    const int last = static_cast<int>(rows().size()) - 1;
    for (const UiAction action : actions) {
        switch (action) {
            // No going round: the list stops at its ends.
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(last, focus_ + 1); break;
            case UiAction::Confirm: act(result, output_width, output_height); break;
            case UiAction::Cancel: result.close_requested = true; break;
            case UiAction::ToggleMenu:
                result.close_requested = true;
                result.close_everything = true;
                break;
            default: break;
        }
        if (choice_.is_open() || result.close_requested) break;
    }
    // The cursor brings the list with it when it is moved -- and only then,
    // so a list dragged by a finger stays where the finger left it.
    if (!actions.empty()) {
        drag_.stop();
        scroll_focus_into_view(compute_geometry(output_width, output_height));
    }
    if (!result.close_requested) {
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
    }
    drag_.settle();
    if (result.close_requested) close();
    return result;
}

void WiiUPlayerScreen::handle_tap(int entry, WiiUPlayerScreenResult& result, float output_width,
                                  float output_height) {
    if (!open_) return;
    if (entry == kTapOffPopup) {
        choice_.close();
        return;
    }
    if (entry < 0) return;
    if (entry >= kChoiceEntryBase) {
        // A dropdown's choice is chosen by its first tap.
        if (!choice_.is_open()) return;
        const int index = entry - kChoiceEntryBase;
        if (index >= static_cast<int>(choice_.choices().size()) || choice_.heading(index)) return;
        choice_.close();
        choose(index, result);
        return;
    }
    if (choice_.is_open() || entry >= static_cast<int>(rows().size())) return;
    if (tap_chooses(focus_, entry)) act(result, output_width, output_height);
}

void WiiUPlayerScreen::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                              float output_width, float output_height) {
    if (!open_) return;
    glyph_aspect_ = glyphs.width_for(1.0f);
    const ShownScroll shown(scroll_, scroll_ease_, drag_.dragging());
    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = geometry.line;
    const PageFrame frame = page_frame(output_width, output_height);
    draw_page_background(quads, output_width, output_height, over_game_);

    // The player's lights, or the GamePad, in front of its name.
    const PadSlotView slot_view(line);
    const bool icons = load_ui_icons(icons_, icons_tried_);
    const float header_centre = std::round((frame.panel.y + frame.header_bottom) * 0.5f);
    slot_view.draw(quads, text, icons ? &icons_ : nullptr, slot_, frame.panel.x + slot_view.width * 0.5f,
                   header_centre, scale, theme.text);
    draw_page_header(quads, text, frame, title(), std::string(), slot_view.width + std::round(line * 0.8f));

    const float pad = menu_row_pad(line);
    const float row_scale = scale * 1.1f;
    const std::vector<Row> all = rows();
    quads.set_clip(menu_list_clip(geometry.list, scale));
    if (switching_.on() && !profile_names_.empty()) {
        // Over the profiles, what their switches are.
        const float heading_scale = scale * 0.9f;
        text.draw(quads, "Switch Between", geometry.list.x + std::round(line * 0.5f),
                  std::round(geometry.heading_y + std::round(line * 2.6f) - TextRenderer::line_height(heading_scale) -
                             line * 0.5f),
                  heading_scale, theme.text_dim);
    }
    for (std::size_t index = 0; index < all.size(); ++index) {
        const PixelRect& row = geometry.rows[index];
        const bool focused = static_cast<int>(index) == focus_;
        // A row after a heading is the first of its block: it draws its own
        // top rule, as the first row of the page does.
        const bool first_profile = all[index] == Row::Profile && (index == 0 || all[index - 1] != Row::Profile);
        const bool before_profiles = index + 1 < all.size() && all[index + 1] == Row::Profile && all[index] != Row::Profile;
        draw_menu_row(quads, row, focused, scale, index + 1 == all.size() || before_profiles,
                      static_cast<int>(index) == focus_ + 1 && !first_profile);
        const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
        const float right = row.x + row.w - pad;
        if (all[index] == Row::Remove) {
            text.draw(quads, "Remove Controller", row.x + pad, baseline, row_scale, menu_row_text(focused));
            continue;
        }
        if (all[index] == Row::Mapping) {
            // And the profile its mapping is, if it is one.
            text.draw(quads, "Edit Button Mapping", row.x + pad, baseline, row_scale, menu_row_text(focused));
            text.draw(quads, mapping_profile_, right, baseline, row_scale, theme.text_dim, TextAlign::Right);
            continue;
        }
        if (all[index] == Row::Switching) {
            text.draw(quads, "Profile Switching", row.x + pad, baseline, row_scale, menu_row_text(focused));
            if (!switching_.on()) {
                text.draw(quads, "Off", right, baseline, row_scale, theme.text_dim, TextAlign::Right);
                continue;
            }
            // "Hold" and the button, as the pad draws it.
            const float height = std::round(line * 1.5f);
            const float width = glyphs.width_for(height);
            glyphs.draw(quads, switching_.button == PadControl::Minus ? ButtonGlyph::Minus : ButtonGlyph::Plus,
                        right - width, std::round(row.y + row.h * 0.5f - height * glyphs.button_centre()), height,
                        theme.text_dim);
            text.draw(quads, "Hold", right - width - std::round(line * 0.4f), baseline, row_scale, theme.text_dim,
                      TextAlign::Right);
            continue;
        }
        if (all[index] == Row::Profile) {
            const int profile = profile_of_row(static_cast<int>(index));
            const std::string& name = profile_names_[static_cast<std::size_t>(profile)];
            text.draw(quads, name, row.x + pad, baseline, row_scale, menu_row_text(focused));
            draw_toggle(quads, right, row.y + row.h * 0.5f, switching_.includes(name), line);
            continue;
        }
        if (all[index] == Row::Add) {
            text.draw(quads, "Add Controller", row.x + pad, baseline, row_scale, menu_row_text(focused));
            continue;
        }
        if (all[index] == Row::Type) {
            text.draw(quads, "Controller Type", row.x + pad, baseline, row_scale, menu_row_text(focused));
            const int type = adding_ ? pending_type_ : type_current_;
            const std::string value = type >= 0 && type < static_cast<int>(types_.size())
                                          ? wii_u_type_name(types_[static_cast<std::size_t>(type)])
                                          : std::string();
            // Drawn as well, at the end of the row, as it is in the list.
            float value_right = right;
            if (icons && !value.empty()) {
                const float size = std::round(row.h * 0.8f);
                draw_ui_icon(quads, icons_, wii_u_type_icon(types_[static_cast<std::size_t>(type)]),
                             PixelRect{right - size, std::round(row.y + (row.h - size) * 0.5f), size, size},
                             theme.text_dim);
                value_right -= size + std::round(line * 0.4f);
            }
            text.draw(quads, value, value_right, baseline, row_scale, theme.text_dim, TextAlign::Right);
        }
    }
    quads.clear_clip();
    if (geometry.content_height > geometry.list.h + 0.5f) {
        draw_scroll_bar(quads, geometry.list, scroll_ / geometry.content_height,
                        geometry.list.h / geometry.content_height, line, scale);
    }

    std::vector<GuidePrompt> guide;
    guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    const Row focused = focused_row();
    guide.push_back({ButtonGlyph::A,
                     focused == Row::Remove    ? "Remove"
                     : focused == Row::Add     ? "Add"
                     : focused == Row::Mapping ? "Open"
                     : focused == Row::Profile ? "OK"
                                               : "Change",
                     PromptRole::ActsOnSelection});
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, guide, &guide_hits_);

    if (choice_.is_open()) {
        choice_.set_row(geometry.rows[static_cast<std::size_t>(std::clamp(focus_, 0, static_cast<int>(all.size()) - 1))]);
        choice_.render(quads, text, output_width, output_height, &glyphs);
    }
}

void WiiUPlayerScreen::show_mirrored(const net::WiiUPlayerViewMessage& view, float output_width,
                                     float output_height) {
    if (!open_ || !(slot_ == view.slot) || adding_ != view.adding) {
        if (view.adding) {
            open_adding(view.slot);
        } else {
            open(view.slot);
        }
    }
    if (adding_) pending_type_ = view.pending_type - 1;
    ProfileSwitching switching;
    switching.button = view.switch_button == 1   ? PadControl::Minus
                       : view.switch_button == 2 ? PadControl::Plus
                                                 : PadControl::Count;
    switching.profiles = view.switch_profiles;
    set_profile_switching(switching, view.profile_names, view.mapping_profile);
    const int was = focus_;
    focus_ = std::clamp(static_cast<int>(view.focus), 0, static_cast<int>(rows().size()) - 1);
    // The other device's cursor brings the list with it when it moves.
    if (focus_ != was) {
        drag_.stop();
        scroll_focus_into_view(compute_geometry(output_width, output_height));
    }
    if (view.choice_focus == 0) {
        choice_.close();
    } else {
        if (!choice_.is_open()) open_choice(output_width, output_height);
        if (choice_.focus() != view.choice_focus - 1) choice_.set_focus(view.choice_focus - 1);
    }
}

bool WiiUPlayerScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width, float output_height) {
    if (choice_.is_open()) return choice_.scroll(pointers, output_width, output_height);
    const Geometry geometry = compute_geometry(output_width, output_height);
    drag_.update(pointers, geometry.list, scroll_, geometry.max_scroll());
    return drag_.release_was_drag();
}

int WiiUPlayerScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (choice_.is_open()) {
        const int choice = choice_.choice_at(x, y, output_width, output_height);
        return choice >= 0 ? kChoiceEntryBase + choice : -1;
    }
    const Geometry geometry = compute_geometry(output_width, output_height);
    if (!geometry.list.contains(x, y)) return -1;
    for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
        if (geometry.rows[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
}

bool WiiUPlayerScreen::off_popup(float x, float y, float output_width, float output_height) const {
    if (choice_.is_open()) return !choice_.contains(x, y, output_width, output_height);
    return false;
}

std::vector<float> WiiUPlayerScreen::scroll_state(float output_width, float output_height) const {
    const Geometry geometry = compute_geometry(output_width, output_height);
    return {scroll_in_lines(scroll_, geometry.scale), choice_.is_open() ? choice_.scroll_rows() : 0.0f};
}

void WiiUPlayerScreen::set_scroll_state(const std::vector<float>& state, float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 0), geometry.scale), 0.0f, geometry.max_scroll());
    drag_.stop();
    if (choice_.is_open()) choice_.set_scroll_rows(scroll_at(state, 1));
}

void WiiUPlayerScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    if (choice_.is_open()) mix_sound_state(view.focus, static_cast<uint64_t>(choice_.focus() + 1000));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, choice_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(type_current_));
    mix_sound_state(view.rest, static_cast<uint64_t>(pending_type_ + 1));
    if (const ControllerRowView* own = own_row()) mix_sound_state(view.rest, own->pad);
    mix_sound_state(view.rest, static_cast<uint64_t>(switching_.button));
    mix_sound_state(view.rest, static_cast<uint64_t>(switching_.profiles.size()));
    for (const std::string& name : switching_.profiles) {
        for (const char c : name) mix_sound_state(view.rest, static_cast<uint64_t>(static_cast<unsigned char>(c)));
    }
    // A profile's switch, for the sound it makes turned on or off.
    const int profile = profile_of_row(focus_);
    if (profile >= 0 && !choice_.is_open()) {
        view.toggle = switching_.includes(profile_names_[static_cast<std::size_t>(profile)]) ? 1 : 0;
    }
}

net::WiiUPlayerViewMessage wii_u_player_view_of(const WiiUPlayerScreen& screen) {
    net::WiiUPlayerViewMessage view;
    view.open = screen.is_open();
    if (!view.open) return view;
    view.slot = screen.slot();
    view.focus = static_cast<uint8_t>(screen.focus());
    view.choice_focus = screen.choosing() ? static_cast<uint8_t>(screen.choice_focus() + 1) : 0;
    view.adding = screen.adding();
    const ProfileSwitching& switching = screen.switching();
    view.switch_button = switching.button == PadControl::Minus  ? 1
                         : switching.button == PadControl::Plus ? 2
                                                                : 0;
    view.switch_profiles = switching.profiles;
    view.profile_names = screen.profile_names();
    view.mapping_profile = screen.mapping_profile();
    if (view.adding) {
        view.pending_type = static_cast<uint8_t>(std::clamp(screen.pending_type() + 1, 0, 255));
    }
    return view;
}

}  // namespace ds
