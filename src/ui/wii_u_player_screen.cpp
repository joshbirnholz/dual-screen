#include "ui/wii_u_player_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/pad_slot_view.hpp"
#include "ui/page_frame.hpp"
#include "ui/ui_icons.hpp"
#include "ui/ui_theme.hpp"

namespace ds {
namespace {

// Controller's popup: Detect Controller first, a line that is not a
// controller at all.
const char* const kDetectChoice = "<detect>";

// What another controller is now, beside its name in the popup.
std::string slot_hint(PadSlot slot) {
    return slot.kind == PadSlot::Kind::NotUsed ? std::string("Not Used") : pad_slot_name(slot);
}

}  // namespace

void WiiUPlayerScreen::open(PadSlot slot) {
    open_ = true;
    slot_ = slot;
    focus_ = 0;
    choice_.close();
    detecting_ = false;
}

void WiiUPlayerScreen::set_controllers(const std::vector<ControllerRowView>& controllers,
                                       std::vector<std::string> types, int type_current) {
    controllers_ = controllers;
    types_ = slot_.kind == PadSlot::Kind::Player ? std::move(types) : std::vector<std::string>();
    type_current_ = type_current;
    focus_ = std::clamp(focus_, 0, static_cast<int>(rows().size()) - 1);
}

std::vector<WiiUPlayerScreen::Row> WiiUPlayerScreen::rows() const {
    std::vector<Row> rows;
    if (!types_.empty()) rows.push_back(Row::Type);
    rows.push_back(Row::Controller);
    return rows;
}

WiiUPlayerScreen::Row WiiUPlayerScreen::focused_row() const {
    const std::vector<Row> all = rows();
    return all[static_cast<std::size_t>(std::clamp(focus_, 0, static_cast<int>(all.size()) - 1))];
}

std::string WiiUPlayerScreen::title() const { return pad_slot_name(slot_); }

const ControllerRowView* WiiUPlayerScreen::own_row() const {
    for (const ControllerRowView& row : controllers_) {
        if (row.slot == slot_ && row.slot.kind != PadSlot::Kind::NotUsed) return &row;
    }
    return nullptr;
}

int WiiUPlayerScreen::detect_seconds() const {
    return std::max(0, static_cast<int>(std::ceil(detect_left_)));
}

void WiiUPlayerScreen::tick(float seconds) {
    if (!detecting_) return;
    detect_left_ -= std::clamp(seconds, 0.0f, 0.25f);
    if (detect_left_ <= 0.0f) detecting_ = false;
}

WiiUPlayerScreen::Geometry WiiUPlayerScreen::compute_geometry(float output_width, float output_height) const {
    Geometry geometry;
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    geometry.line = frame.line;
    geometry.list = PixelRect{frame.panel.x, frame.content_top, frame.panel.w, frame.content_bottom - frame.content_top};
    const float row_height = menu_row_height(frame.line);
    const float step = row_height + menu_row_gap(frame.line);
    const std::size_t count = rows().size();
    for (std::size_t index = 0; index < count; ++index) {
        geometry.rows.push_back(
            PixelRect{frame.panel.x, frame.content_top + static_cast<float>(index) * step, frame.panel.w, row_height});
    }
    return geometry;
}

void WiiUPlayerScreen::open_choice(float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    const PixelRect& row = geometry.rows[static_cast<std::size_t>(std::clamp(focus_, 0, static_cast<int>(geometry.rows.size()) - 1))];
    choice_types_.clear();
    choice_pads_.clear();
    std::vector<std::string> labels;
    if (focused_row() == Row::Type) {
        // Cemu's, but for None: a player with no controller is one taken out
        // of play, which is Remove Player's.
        int current = 0;
        for (std::size_t index = 0; index < types_.size(); ++index) {
            if (types_[index] == "None") continue;
            if (static_cast<int>(index) == type_current_) current = static_cast<int>(labels.size());
            labels.push_back(wii_u_type_name(types_[index]));
            choice_types_.push_back(static_cast<int>(index));
        }
        choice_.open(std::move(labels), current, row);
        return;
    }
    // Detect Controller, None, then every controller under the device it is
    // on, each saying what it is now where that is somewhere else.
    std::vector<bool> headings;
    std::vector<std::string> hints;
    const auto add = [&](std::string label, std::string pad, bool heading, std::string hint) {
        labels.push_back(std::move(label));
        choice_pads_.push_back(std::move(pad));
        headings.push_back(heading);
        hints.push_back(std::move(hint));
    };
    add("Detect Controller…", kDetectChoice, false, "");
    add("None", "", false, "");
    const ControllerRowView* own = own_row();
    int current = own != nullptr && !own->pad.empty() ? -1 : 1;
    std::vector<std::string> devices;
    for (const ControllerRowView& row : controllers_) {
        if (row.pad.empty()) continue;
        if (std::find(devices.begin(), devices.end(), row.device) == devices.end()) devices.push_back(row.device);
    }
    for (const std::string& device : devices) {
        add(device, "", true, "");
        for (const ControllerRowView& row : controllers_) {
            if (row.pad.empty() || row.device != device) continue;
            const bool here = own != nullptr && row.pad == own->pad;
            if (here) current = static_cast<int>(labels.size());
            add(row.name, row.pad, false, here ? std::string() : slot_hint(row.slot));
        }
    }
    choice_.open(std::move(labels), std::max(0, current), row);
    choice_.set_headings(std::move(headings));
    choice_.set_hints(std::move(hints));
}

void WiiUPlayerScreen::choose(int index, WiiUPlayerScreenResult& result) {
    if (focused_row() == Row::Type) {
        if (index < 0 || index >= static_cast<int>(choice_types_.size())) return;
        const int type = choice_types_[static_cast<std::size_t>(index)];
        if (type != type_current_) result.type_chosen = type;
        return;
    }
    if (index < 0 || index >= static_cast<int>(choice_pads_.size()) || choice_.heading(index)) return;
    const std::string& pad = choice_pads_[static_cast<std::size_t>(index)];
    if (pad == kDetectChoice) {
        detecting_ = true;
        detect_left_ = kDetectSeconds;
        result.detect_started = true;
        return;
    }
    const ControllerRowView* own = own_row();
    if (own != nullptr && own->pad == pad) return;
    if (pad.empty() && (own == nullptr || own->pad.empty())) return;
    result.controller_chosen = pad;
}

bool WiiUPlayerScreen::removable() const {
    const ControllerRowView* own = own_row();
    return own != nullptr && !own->pad.empty();
}

void WiiUPlayerScreen::remove(WiiUPlayerScreenResult& result) {
    if (!removable()) return;
    result.removed = true;
    // A player taken out of play is no longer there to have a page; the
    // GamePad is, with nothing holding it.
    if (slot_.kind == PadSlot::Kind::Player) result.close_requested = true;
}

PopupText WiiUPlayerScreen::detect_text() const {
    PopupText words;
    words.message = "Press a button on the controller " + title() + " will use.";
    const int seconds = detect_seconds();
    words.lines.push_back(std::to_string(seconds) + (seconds == 1 ? " second" : " seconds"));
    return words;
}

PopupGeometry WiiUPlayerScreen::detect_popup(float output_width, float output_height) const {
    const PopupText words = detect_text();
    // Room under the words for the time left as a bar.
    const float line = TextRenderer::line_height(popup_scale(output_height));
    return popup_geometry(output_width, output_height,
                          popup_text_height(words, popup_body_width(output_width, output_height), output_height) +
                              std::round(line * 1.4f),
                          {});
}

WiiUPlayerScreenResult WiiUPlayerScreen::update(const std::vector<UiAction>& pressed,
                                                const std::vector<PointerEvent>& touched, float output_width,
                                                float output_height) {
    WiiUPlayerScreenResult result;
    if (!open_) return result;
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    // A tap on a prompt at the foot of the page is that button pressed --
    // but not while the popup is up over them. Detect Controller's Cancel is
    // only there.
    if (!choice_.is_open()) take_guide_taps(guide_hits_, actions, pointers);

    // Detect Controller: only B, which puts it away, and the menu button.
    if (detecting_) {
        for (const UiAction action : actions) {
            if (action == UiAction::Cancel) detecting_ = false;
            if (action == UiAction::ToggleMenu) {
                result.close_requested = true;
                result.close_everything = true;
                close();
                return result;
            }
        }
        take_taps(pointers, output_width, output_height);
        return result;
    }
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
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(last, focus_ + 1); break;
            case UiAction::Confirm: open_choice(output_width, output_height); break;
            case UiAction::Cancel: result.close_requested = true; break;
            case UiAction::ToggleMenu:
                result.close_requested = true;
                result.close_everything = true;
                break;
            // X, where a Nintendo pad has it.
            case UiAction::MenuQuit: remove(result); break;
            default: break;
        }
        if (choice_.is_open() || result.close_requested) break;
    }
    if (!result.close_requested) {
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
    }
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
    if (tap_chooses(focus_, entry)) open_choice(output_width, output_height);
}

void WiiUPlayerScreen::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                              float output_width, float output_height) {
    if (!open_) return;
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
    const ControllerRowView* own = own_row();
    for (std::size_t index = 0; index < all.size(); ++index) {
        const PixelRect& row = geometry.rows[index];
        const bool focused = static_cast<int>(index) == focus_;
        draw_menu_row(quads, row, focused, scale, index + 1 == all.size(), static_cast<int>(index) == focus_ + 1);
        const float baseline = row.y + (row.h - TextRenderer::line_height(row_scale)) * 0.5f;
        const float right = row.x + row.w - pad;
        if (all[index] == Row::Type) {
            text.draw(quads, "Controller Type", row.x + pad, baseline, row_scale, menu_row_text(focused));
            const std::string value = type_current_ >= 0 && type_current_ < static_cast<int>(types_.size())
                                          ? wii_u_type_name(types_[static_cast<std::size_t>(type_current_)])
                                          : std::string();
            text.draw(quads, value, right, baseline, row_scale, theme.text_dim, TextAlign::Right);
            continue;
        }
        text.draw(quads, "Controller", row.x + pad, baseline, row_scale, menu_row_text(focused));
        // The controller, and where it is under it.
        if (own == nullptr || own->pad.empty()) {
            text.draw(quads, "None", right, baseline, row_scale, theme.text_dim, TextAlign::Right);
            continue;
        }
        const float place_scale = scale * 0.8f;
        const float name_height = TextRenderer::line_height(row_scale);
        const float gap = std::round(line * 0.15f);
        const float top = std::round(row.y + (row.h - name_height - gap - TextRenderer::line_height(place_scale)) * 0.5f);
        const float room = row.w * 0.6f;
        text.draw(quads, TextRenderer::elide_right(own->name, room, row_scale), right, top, row_scale, theme.text_dim,
                  TextAlign::Right);
        text.draw(quads, TextRenderer::elide_right(own->place, room, place_scale), right, top + name_height + gap,
                  place_scale, theme.text_dim, TextAlign::Right);
    }

    std::vector<GuidePrompt> guide;
    if (detecting_) {
        guide.push_back({ButtonGlyph::B, "Cancel", PromptRole::LeavesScreen});
    } else {
        if (removable()) {
            guide.push_back(
                {ButtonGlyph::X, slot_.kind == PadSlot::Kind::Player ? "Remove Player" : "Remove Controller"});
        }
        guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
        guide.push_back({ButtonGlyph::A, "OK", PromptRole::ActsOnSelection});
    }
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, guide, &guide_hits_);

    if (choice_.is_open()) {
        choice_.set_row(geometry.rows[static_cast<std::size_t>(std::clamp(focus_, 0, static_cast<int>(all.size()) - 1))]);
        choice_.render(quads, text, output_width, output_height);
    }
    if (detecting_) {
        const PopupGeometry popup = detect_popup(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, {}, -1, false);
        const float bottom = draw_popup_text(quads, text, detect_text(), popup.body.x, popup.body.y, popup.body.w,
                                             output_height);
        // And the time left, going down.
        const float popup_line = TextRenderer::line_height(popup_scale(output_height));
        const float bar_h = std::max(3.0f, std::round(popup_line * 0.3f));
        const PixelRect track{popup.body.x, std::round(bottom + popup_line * 0.9f), popup.body.w, bar_h};
        Color empty = theme.text_dim;
        empty.a = 0.3f;
        quads.draw_rounded_rect(track, bar_h * 0.5f, empty);
        const float share = std::clamp(detect_left_ / kDetectSeconds, 0.0f, 1.0f);
        if (share > 0.0f) {
            quads.draw_rounded_rect(PixelRect{track.x, track.y, std::max(bar_h, std::round(track.w * share)), bar_h},
                                    bar_h * 0.5f, theme.accent);
        }
    }
}

void WiiUPlayerScreen::show_mirrored(const net::WiiUPlayerViewMessage& view, float output_width,
                                     float output_height) {
    if (!open_ || !(slot_ == view.slot)) open(view.slot);
    focus_ = std::clamp(static_cast<int>(view.focus), 0, static_cast<int>(rows().size()) - 1);
    if (view.choice_focus == 0) {
        choice_.close();
    } else {
        if (!choice_.is_open()) open_choice(output_width, output_height);
        if (choice_.focus() != view.choice_focus - 1) choice_.set_focus(view.choice_focus - 1);
    }
    detecting_ = view.detecting;
    detect_left_ = static_cast<float>(view.detect_seconds);
}

bool WiiUPlayerScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width, float output_height) {
    if (choice_.is_open()) return choice_.scroll(pointers, output_width, output_height);
    return false;
}

int WiiUPlayerScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (detecting_) return -1;
    if (choice_.is_open()) {
        const int choice = choice_.choice_at(x, y, output_width, output_height);
        return choice >= 0 ? kChoiceEntryBase + choice : -1;
    }
    const Geometry geometry = compute_geometry(output_width, output_height);
    for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
        if (geometry.rows[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
}

bool WiiUPlayerScreen::off_popup(float x, float y, float output_width, float output_height) const {
    if (choice_.is_open()) return !choice_.contains(x, y, output_width, output_height);
    return false;
}

std::vector<float> WiiUPlayerScreen::scroll_state(float, float) const {
    return {0.0f, choice_.is_open() ? choice_.scroll_rows() : 0.0f};
}

void WiiUPlayerScreen::set_scroll_state(const std::vector<float>& state, float, float) {
    if (choice_.is_open()) choice_.set_scroll_rows(scroll_at(state, 1));
}

void WiiUPlayerScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    if (choice_.is_open()) mix_sound_state(view.focus, static_cast<uint64_t>(choice_.focus() + 1000));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, choice_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, detecting_ ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(type_current_));
    if (const ControllerRowView* own = own_row()) mix_sound_state(view.rest, own->pad);
}

net::WiiUPlayerViewMessage wii_u_player_view_of(const WiiUPlayerScreen& screen) {
    net::WiiUPlayerViewMessage view;
    view.open = screen.is_open();
    if (!view.open) return view;
    view.slot = screen.slot();
    view.focus = static_cast<uint8_t>(screen.focus());
    view.choice_focus = screen.choosing() ? static_cast<uint8_t>(screen.choice_focus() + 1) : 0;
    view.detecting = screen.detecting();
    view.detect_seconds = static_cast<uint8_t>(screen.detect_seconds());
    return view;
}

}  // namespace ds
