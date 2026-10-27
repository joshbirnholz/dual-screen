#include "ui/button_mapping_screen.hpp"

#include <algorithm>
#include <cmath>

#include "config/mapping_profiles.hpp"
#include "input/stylus_cursor.hpp"
#include "ui/page_frame.hpp"
#include "ui/wii_u_controller_drawing.hpp"
#include "ui/ui_theme.hpp"

namespace ds {
namespace {

ButtonGlyph glyph_for(PadControl control) {
    switch (control) {
        case PadControl::A: return ButtonGlyph::A;
        case PadControl::B: return ButtonGlyph::B;
        case PadControl::X: return ButtonGlyph::X;
        case PadControl::Y: return ButtonGlyph::Y;
        case PadControl::L: return ButtonGlyph::L;
        case PadControl::R: return ButtonGlyph::R;
        case PadControl::Zl: return ButtonGlyph::ZL;
        case PadControl::Zr: return ButtonGlyph::ZR;
        case PadControl::Plus: return ButtonGlyph::Plus;
        case PadControl::Minus: return ButtonGlyph::Minus;
        case PadControl::LeftStick: return ButtonGlyph::LeftStick;
        case PadControl::RightStick: return ButtonGlyph::RightStick;
        case PadControl::Up: return ButtonGlyph::DpadUp;
        case PadControl::Down: return ButtonGlyph::DpadDown;
        case PadControl::Left: return ButtonGlyph::DpadLeft;
        case PadControl::Right: return ButtonGlyph::DpadRight;
        case PadControl::Count: break;
    }
    return ButtonGlyph::A;
}

// The parts of a console the page can pick out: its buttons, the touch
// screen and the microphone.
enum class ConsolePart { None, A, B, X, Y, L, R, Zl, Zr, Start, Select, Up, Down, Left, Right, Touch, Mic };

ConsolePart part_for(PadAction action) {
    switch (action) {
        case PadAction::A: return ConsolePart::A;
        case PadAction::B: return ConsolePart::B;
        case PadAction::X: return ConsolePart::X;
        case PadAction::Y: return ConsolePart::Y;
        case PadAction::L: return ConsolePart::L;
        case PadAction::R: return ConsolePart::R;
        case PadAction::Zl: return ConsolePart::Zl;
        case PadAction::Zr: return ConsolePart::Zr;
        case PadAction::Start: return ConsolePart::Start;
        case PadAction::Select: return ConsolePart::Select;
        case PadAction::Up: return ConsolePart::Up;
        case PadAction::Down: return ConsolePart::Down;
        case PadAction::Left: return ConsolePart::Left;
        case PadAction::Right: return ConsolePart::Right;
        case PadAction::TouchCursor: return ConsolePart::Touch;
        case PadAction::BlowIntoMic: return ConsolePart::Mic;
        default: return ConsolePart::None;
    }
}

}  // namespace

void ButtonMappingScreen::open() {
    open_ = true;
    focus_ = 0;
    scroll_ = 0.0f;
    drag_.stop();
    choice_.close();
    confirming_ = false;
    placing_ = false;
    placing_stick_ = false;
    mirrored_focus_ = -1;
    profiles_up_ = false;
    holding_up_ = false;
    naming_.close();
}

void ButtonMappingScreen::set_mapping(const ButtonMapping& mapping, SystemId system,
                                      const PadConsole& console) {
    wii_u_ = false;
    mapping_ = mapping;
    system_ = system;
    console_ = console;
}

void ButtonMappingScreen::set_wii_u_mapping(const WiiUMapping& mapping, WiiUController controller,
                                            std::string detail) {
    wii_u_ = true;
    wii_u_mapping_ = mapping;
    // Another controller under the picker: what it lists is no longer this.
    if (controller != wii_u_controller_) choice_.close();
    wii_u_controller_ = controller;
    detail_ = std::move(detail);
    system_ = SystemId::WiiU;
    focus_ = std::clamp(focus_, 0, row_count() - 1);
}

std::string ButtonMappingScreen::console_name() const {
    return system_ == SystemId::ThreeDs ? "3DS" : "DS";
}

ButtonMappingScreen::Geometry ButtonMappingScreen::compute_geometry(float output_width,
                                                                    float output_height) const {
    Geometry geometry;
    // The same frame as every other page: see ui/page_frame.hpp.
    const PageFrame frame = page_frame(output_width, output_height);
    geometry.scale = frame.scale;
    geometry.line = frame.line;
    geometry.view_top = frame.content_top;
    geometry.view_bottom = frame.content_bottom;
    const float row_height = menu_row_height(frame.line);
    const float step = row_height + menu_row_gap(frame.line);
    // The list down the left, a little over half the page, room for a long
    // name and what it does side by side; the console in the rest, past a gap.
    const float list_width = std::round(frame.panel.w * 0.58f);
    geometry.list = PixelRect{frame.panel.x, geometry.view_top, list_width,
                              geometry.view_bottom - geometry.view_top};
    geometry.content_height = static_cast<float>(row_count()) * step;
    const bool scrolls = geometry.max_scroll() > 0.5f;
    const float row_width =
        list_width - (scrolls ? scroll_bar_width(geometry.scale) + frame.line * 0.6f : 0.0f);
    for (std::size_t index = 0; index < static_cast<std::size_t>(row_count()); ++index) {
        geometry.rows.push_back(PixelRect{frame.panel.x, geometry.view_top - scroll_ + index * step,
                                          row_width, row_height});
    }
    const float diagram_x = frame.panel.x + list_width + frame.line * 2.0f;
    // Two lines of words under it, for the longer things a button can do.
    const float caption_room = frame.line * 3.6f;
    geometry.diagram = PixelRect{diagram_x, geometry.view_top,
                                 frame.panel.x + frame.panel.w - diagram_x,
                                 geometry.view_bottom - geometry.view_top - caption_room};
    geometry.caption_y = std::round(geometry.view_bottom - caption_room + frame.line * 0.6f);
    return geometry;
}

void ButtonMappingScreen::scroll_focus_into_view(const Geometry& geometry) {
    const PixelRect& focused = geometry.rows[static_cast<std::size_t>(focus_)];
    if (focused.y < geometry.view_top) {
        scroll_ -= geometry.view_top - focused.y;
    } else if (focused.y + focused.h > geometry.view_bottom) {
        scroll_ += focused.y + focused.h - geometry.view_bottom;
    }
    scroll_ = std::clamp(scroll_, 0.0f, geometry.max_scroll());
}

void ButtonMappingScreen::open_choice(float output_width, float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    choice_actions_.clear();
    choice_modes_.clear();
    choice_wii_u_.clear();
    profiles_up_ = false;
    holding_up_ = false;
    if (wii_u_) {
        open_wii_u_choice(geometry.rows[static_cast<std::size_t>(focus_)]);
        return;
    }
    if (on_right_stick()) {
        // The right stick's: what it can be on this console, the Touch
        // Analog Stick going on to a page of its own.
        std::vector<std::string> labels;
        int current = 0;
        for (const RightStickMode mode : stick_modes()) {
            if (mode == console_.right_stick) current = static_cast<int>(labels.size());
            labels.push_back(mode == RightStickMode::TouchAnalog ? std::string(to_string(mode)) + "\u2026"
                                                                 : std::string(to_string(mode)));
            choice_modes_.push_back(mode);
        }
        choice_.open(std::move(labels), current, geometry.rows[static_cast<std::size_t>(focus_)]);
        return;
    }
    // A button on turbo shows the button it presses as its choice.
    const PadAction resolved = mapping_.resolved(focused_control(), console_);
    const PadAction now = resolved == PadAction::Turbo ? mapping_.turbo(focused_control()) : resolved;
    std::vector<std::string> labels;
    int current = -1;
    // One list, no headings: the console's buttons, then the rest.
    for (const PadAction action : pad_action_choices(console_)) {
        if (action == now) current = static_cast<int>(labels.size());
        // Touch Point goes on to a page of its own, which the words say.
        labels.push_back(action == PadAction::TouchPoint ? std::string("Touch Point\u2026")
                                                         : std::string(pad_action_name(action)));
        choice_actions_.push_back(action);
    }
    choice_.open(std::move(labels), std::max(0, current), geometry.rows[static_cast<std::size_t>(focus_)]);
}

void ButtonMappingScreen::back_to_choice(PadAction on, float output_width, float output_height) {
    placing_ = false;
    placing_stick_ = false;
    open_choice(output_width, output_height);
    for (std::size_t index = 0; index < choice_modes_.size(); ++index) {
        if (choice_modes_[index] == RightStickMode::TouchAnalog) choice_.set_focus(static_cast<int>(index));
    }
    for (std::size_t index = 0; index < choice_actions_.size(); ++index) {
        if (choice_actions_[index] == on) {
            choice_.set_focus(static_cast<int>(index));
        }
    }
}

void ButtonMappingScreen::set_profiles(std::vector<std::string> names, int current, std::string heading) {
    const bool same = names == profile_names_ && current == profile_current_ && heading == profile_heading_;
    profile_names_ = std::move(names);
    profile_current_ = current >= 0 && current < static_cast<int>(profile_names_.size()) ? current : -1;
    profile_heading_ = std::move(heading);
    if (same || !profiles_open()) return;
    // The list changed under the popup: drawn again, the cursor where it
    // was -- or on one just saved, now it is there.
    int focus = choice_.focus();
    for (std::size_t index = 0; index < profile_names_.size(); ++index) {
        if (!profile_saved_.empty() && profile_names_[index] == profile_saved_) focus = static_cast<int>(index) + 2;
    }
    profile_saved_.clear();
    open_profiles(focus);
}

void ButtonMappingScreen::open_profiles(int focus) {
    const Geometry geometry = compute_geometry(last_width_, last_height_);
    // Save as New Profile..., then the profiles under their heading.
    std::vector<std::string> lines{"Save as New Profile\u2026"};
    std::vector<bool> headings{false};
    if (!profile_names_.empty()) {
        lines.push_back(profile_heading_);
        headings.push_back(true);
        for (const std::string& name : profile_names_) {
            lines.push_back(name);
            headings.push_back(false);
        }
    }
    const PixelRect top{geometry.list.x, geometry.view_top, geometry.list.w,
                        geometry.rows.empty() ? geometry.line * 2.0f : geometry.rows.front().h};
    choice_actions_.clear();
    choice_modes_.clear();
    choice_wii_u_.clear();
    choice_.open(std::move(lines), profile_current_ >= 0 ? profile_current_ + 2 : -1, top);
    choice_.set_headings(std::move(headings));
    profiles_up_ = true;
    const int count = static_cast<int>(choice_.choices().size());
    if (focus >= 0) choice_.set_focus(std::clamp(focus, 0, count - 1));
    // Never on the heading.
    if (choice_.heading(choice_.focus())) choice_.set_focus(std::min(count - 1, choice_.focus() + 1));
}

int ButtonMappingScreen::profile_of_line(int line) const {
    const int profile = line - 2;
    return profile >= 0 && profile < static_cast<int>(profile_names_.size()) ? profile : -1;
}

void ButtonMappingScreen::choose_profile(int line, ButtonMappingScreenResult& result) {
    if (line == 0) {
        start_naming();
        return;
    }
    const int profile = profile_of_line(line);
    if (profile >= 0) result.profile_load = profile;
}

void ButtonMappingScreen::start_naming() {
    choice_.close();
    profiles_up_ = false;
    holding_up_ = false;
    // A name not taken yet, to keep or type over.
    std::string name;
    for (int number = static_cast<int>(profile_names_.size()) + 1;; ++number) {
        name = "Profile " + std::to_string(number);
        if (std::find(profile_names_.begin(), profile_names_.end(), name) == profile_names_.end()) break;
    }
    naming_.open("Name the new profile.", name, MappingProfiles::kMaxName);
    typed_.clear();
    backspaces_ = 0;
}

void ButtonMappingScreen::finish_naming(const TextEntryResult& answer, ButtonMappingScreenResult& result) {
    if (!answer.done && !answer.cancelled) return;
    std::string name = naming_.text();
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    if (answer.done && !name.empty()) {
        result.profile_save_new = name;
        profile_saved_ = name;
    }
    naming_.close();
    // Back to the list: on the new one once it is there.
    open_profiles(0);
}

void ButtonMappingScreen::ask_about_profile(Asking asking) {
    const int profile = profile_of_line(choice_.focus());
    if (profile < 0) return;
    asking_profile_ = profile;
    choice_.close();
    profiles_up_ = false;
    holding_up_ = false;
    asking_ = asking;
    confirming_ = true;
    confirm_cursor_.reset(2, 1);
}

bool ButtonMappingScreen::type_mirrored(const std::string& typed, int backspaces) {
    if (!naming_.is_open() || (typed.empty() && backspaces == 0)) return false;
    const std::string was = naming_.text();
    std::string text = was;
    for (int erased = 0; erased < backspaces; ++erased) text = utf8_without_last(text);
    text += typed;
    naming_.set_text(text);
    // Kept for the next few views, while the other device catches up.
    own_name_views_ = 30;
    return naming_.text() != was;
}

void ButtonMappingScreen::open_wii_u_choice(const PixelRect& row) {
    // A button on turbo shows the button it presses as its choice.
    const PadControl control = focused_control();
    const WiiUAction resolved = wii_u_mapping_.resolved(control, wii_u_controller_);
    const WiiUAction now = resolved == WiiUAction::Turbo ? wii_u_mapping_.turbo(control) : resolved;
    std::vector<std::string> labels;
    int current = -1;
    for (const WiiUAction action : wii_u_action_choices(wii_u_controller_)) {
        if (action == now) current = static_cast<int>(labels.size());
        labels.push_back(wii_u_action_name(action, wii_u_controller_));
        choice_wii_u_.push_back(action);
    }
    // Last, what it does when held, on to a picker of its own -- Count
    // standing for it among the actions.
    labels.push_back("When Held\u2026");
    choice_wii_u_.push_back(WiiUAction::Count);
    choice_.open(std::move(labels), std::max(0, current), row);
}

void ButtonMappingScreen::open_hold_choice() {
    const Geometry geometry = compute_geometry(last_width_, last_height_);
    const std::string& now = wii_u_mapping_.held(focused_control());
    std::vector<std::string> labels{"Nothing"};
    int current = 0;
    for (const std::string& name : profile_names_) {
        if (name == now) current = static_cast<int>(labels.size());
        labels.push_back("Switch to " + name);
    }
    choice_wii_u_.clear();
    choice_.open(std::move(labels), current, geometry.rows[static_cast<std::size_t>(focus_)]);
    holding_up_ = true;
}

void ButtonMappingScreen::choose_hold(int index, ButtonMappingScreenResult& result) {
    holding_up_ = false;
    if (index < 0 || index > static_cast<int>(profile_names_.size())) return;
    const std::string name = index == 0 ? std::string() : profile_names_[static_cast<std::size_t>(index - 1)];
    std::string& held = wii_u_mapping_.held(focused_control());
    if (held == name) return;
    held = name;
    result.wii_u_changed = wii_u_mapping_;
}

void ButtonMappingScreen::toggle_sideways(ButtonMappingScreenResult& result) {
    wii_u_mapping_.sideways = !wii_u_mapping_.sideways;
    result.wii_u_changed = wii_u_mapping_;
}

void ButtonMappingScreen::activate(ButtonMappingScreenResult& result, float output_width, float output_height) {
    if (on_sideways()) {
        toggle_sideways(result);
        return;
    }
    open_choice(output_width, output_height);
}

void ButtonMappingScreen::choose_wii_u(int index, ButtonMappingScreenResult& result) {
    if (index < 0 || index >= static_cast<int>(choice_wii_u_.size())) return;
    const PadControl control = focused_control();
    const WiiUAction action = choice_wii_u_[static_cast<std::size_t>(index)];
    if (action == WiiUAction::Count) {
        open_hold_choice();
        return;
    }
    // A button on turbo stays on it for another of the controller's buttons.
    if (wii_u_mapping_[control] == WiiUAction::Turbo && is_wii_u_turbo_button(action)) {
        if (wii_u_mapping_.turbo(control) == action) return;
        wii_u_mapping_.turbo(control) = action;
        result.wii_u_changed = wii_u_mapping_;
        return;
    }
    // What the standard mapping already does is kept as Standard, as a DS
    // game's is.
    const WiiUAction kept =
        action == standard_wii_u_action(control, wii_u_controller_) ? WiiUAction::Standard : action;
    if (kept == wii_u_mapping_[control]) return;
    wii_u_mapping_[control] = kept;
    wii_u_mapping_.turbo(control) = WiiUAction::Standard;
    result.wii_u_changed = wii_u_mapping_;
}

bool ButtonMappingScreen::is_wii_u_turbo_button(WiiUAction action) const {
    const std::vector<WiiUAction> buttons = wii_u_turbo_choices(wii_u_controller_);
    return std::find(buttons.begin(), buttons.end(), action) != buttons.end();
}

void ButtonMappingScreen::toggle_wii_u_turbo(ButtonMappingScreenResult& result) {
    const PadControl control = focused_control();
    const WiiUAction action = wii_u_mapping_.resolved(control, wii_u_controller_);
    if (action == WiiUAction::Turbo) {
        const WiiUAction of = wii_u_mapping_.turbo(control);
        wii_u_mapping_[control] = of == standard_wii_u_action(control, wii_u_controller_) ? WiiUAction::Standard : of;
        wii_u_mapping_.turbo(control) = WiiUAction::Standard;
    } else if (is_wii_u_turbo_button(action)) {
        wii_u_mapping_[control] = WiiUAction::Turbo;
        wii_u_mapping_.turbo(control) = action;
    } else {
        return;
    }
    result.wii_u_changed = wii_u_mapping_;
}

std::string ButtonMappingScreen::wii_u_caption() const {
    const PadControl control = focused_control();
    const WiiUAction action = wii_u_mapping_.resolved(control, wii_u_controller_);
    // The controller as the game has it: a Wii Remote's own buttons are the
    // remote's, whatever is plugged into it.
    const auto on = [&](WiiUAction button) -> std::string {
        const bool remote_own = button == WiiUAction::One || button == WiiUAction::Two ||
                                (button >= WiiUAction::RemoteA && button <= WiiUAction::RemoteRight);
        switch (wii_u_controller_) {
            case WiiUController::GamePad: return "the GamePad";
            case WiiUController::Pro: return "the Pro Controller";
            case WiiUController::Remote: return "the Wii Remote";
            case WiiUController::RemoteNunchuk:
                return button == WiiUAction::NunchukC || button == WiiUAction::NunchukZ ? "the Nunchuk"
                                                                                          : "the Wii Remote";
            case WiiUController::RemoteClassic: return remote_own ? "the Wii Remote" : "the Classic Controller";
        }
        return "the controller";
    };
    // Named as the controller prints it, without the remote's own "Wii
    // Remote" in front where the sentence says so already.
    const auto name = [&](WiiUAction button) {
        std::string said = wii_u_action_name(button, wii_u_controller_);
        const std::string prefix = "Wii Remote ";
        if (said.rfind(prefix, 0) == 0) said = said.substr(prefix.size());
        return said;
    };
    if (action == WiiUAction::Nothing) return "is disabled in this game.";
    if (action == WiiUAction::OpenMenu) return "opens the menu.";
    if (action == WiiUAction::SwapScreens) return "swaps the screens.";
    if (action == WiiUAction::Peek) return "shows the other screen while it's held.";
    if (action == WiiUAction::Turbo) {
        const WiiUAction of = wii_u_mapping_.turbo(control);
        if (of == WiiUAction::Standard) return "does nothing until Turbo has a button.";
        return "presses the " + name(of) + " on " + on(of) + " over and over while it's held.";
    }
    unsigned bit = 0;
    if (wii_u_action_button(action, bit)) return "presses the " + name(action) + " on " + on(action) + ".";
    return std::string();
}

void ButtonMappingScreen::start_placing() {
    choice_.close();
    placing_ = true;
    placing_stick_ = false;
    spot_ = mapping_.spot(focused_control());
}

void ButtonMappingScreen::start_placing_stick() {
    choice_.close();
    placing_ = true;
    placing_stick_ = true;
    spot_ = mapping_.stick_centre;
    stick_radius_ = mapping_.stick_radius;
}

std::vector<RightStickMode> ButtonMappingScreen::stick_modes() const {
    return right_stick_modes(system_ == SystemId::ThreeDs);
}

ButtonGlyph ButtonMappingScreen::row_glyph(int row) const {
    if (row == kRightStickRow) return ButtonGlyph::RightStickMove;
    return glyph_for(kPadControlRows[static_cast<std::size_t>(std::clamp(row, 0, kRightStickRow - 1))]);
}

void ButtonMappingScreen::finish_placing(ButtonMappingScreenResult& result) {
    placing_ = false;
    if (placing_stick_) {
        placing_stick_ = false;
        // The stick where it was put and as big, and the right stick made it.
        if (!(mapping_.stick_centre == spot_) || mapping_.stick_radius != stick_radius_) {
            mapping_.stick_centre = spot_;
            mapping_.stick_radius = stick_radius_;
            result.changed = mapping_;
        }
        if (console_.right_stick != RightStickMode::TouchAnalog) result.right_stick = RightStickMode::TouchAnalog;
        return;
    }
    const PadControl control = focused_control();
    // A button on turbo stays on it, touching there over and over.
    const bool turbo = mapping_[control] == PadAction::Turbo;
    const bool unchanged = turbo ? mapping_.turbo(control) == PadAction::TouchPoint
                                 : mapping_[control] == PadAction::TouchPoint;
    if (unchanged && mapping_.spot(control) == spot_) return;
    if (turbo) {
        mapping_.turbo(control) = PadAction::TouchPoint;
    } else {
        mapping_[control] = PadAction::TouchPoint;
    }
    mapping_.spot(control) = spot_;
    result.changed = mapping_;
}

void ButtonMappingScreen::move_spot(float du, float dv) {
    spot_.u = std::clamp(spot_.u + du, 0.0f, 1.0f);
    spot_.v = std::clamp(spot_.v + dv, 0.0f, 1.0f);
}

int ButtonMappingScreen::spot_entry(const TouchSpot& spot) {
    const int u = std::clamp(static_cast<int>(std::lround(spot.u * 1023.0f)), 0, 1023);
    const int v = std::clamp(static_cast<int>(std::lround(spot.v * 1023.0f)), 0, 1023);
    return kSpotEntryBase + u * 1024 + v;
}

TouchSpot ButtonMappingScreen::entry_spot(int entry) {
    const int at = std::clamp(entry - kSpotEntryBase, 0, 1024 * 1024 - 1);
    return TouchSpot{static_cast<float>(at / 1024) / 1023.0f, static_cast<float>(at % 1024) / 1023.0f};
}

PixelRect ButtonMappingScreen::touch_rect(float output_width, float output_height) const {
    const Geometry geometry = compute_geometry(output_width, output_height);
    const PageFrame frame = page_frame(output_width, output_height);
    // The console's touch screen is 4:3, a DS's and a 3DS's alike; as big as
    // fits over the words beneath it.
    const float top = geometry.view_top + geometry.line;
    const float bottom = geometry.view_bottom - geometry.line * 5.4f;
    const float height = std::max(1.0f, std::min(bottom - top, frame.panel.w * 0.6f * 3.0f / 4.0f));
    const float width = std::round(height * 4.0f / 3.0f);
    return PixelRect{std::round(frame.panel.x + (frame.panel.w - width) * 0.5f), std::round(top), width,
                     std::round(height)};
}

PixelRect ButtonMappingScreen::placing_ok_rect(float output_width, float output_height) const {
    const PixelRect touch = touch_rect(output_width, output_height);
    const float line = TextRenderer::line_height(page_frame(output_width, output_height).scale);
    const float width = std::round(line * 9.0f);
    const float height = std::round(line * 2.2f);
    return PixelRect{std::round(touch.x + (touch.w - width) * 0.5f), std::round(touch.y + touch.h + line * 2.6f),
                     width, height};
}

bool ButtonMappingScreen::steer(float push_x, float push_y, float seconds) {
    seconds = std::clamp(seconds, 0.0f, 0.1f);
    own_spot_seconds_ = std::max(0.0f, own_spot_seconds_ - seconds);
    if (!placing_) return false;
    const float length = std::sqrt(push_x * push_x + push_y * push_y);
    if (length <= 0.0f) return false;
    // Squared, as the Touch Cursor's: fine control near the middle, speed at
    // the edge -- and as fast down the screen as across it, in its pixels.
    const float past = std::min(1.0f, length);
    const float speed = past * past / StylusCursor::kCrossingSeconds;
    const TouchSpot was = spot_;
    move_spot(push_x / length * speed * seconds, push_y / length * speed * seconds * 4.0f / 3.0f);
    constexpr float kOwnSpotSeconds = 0.5f;
    own_spot_seconds_ = kOwnSpotSeconds;
    return !(spot_ == was);
}

bool ButtonMappingScreen::drag_spot(float output_width, float output_height) {
    const LivePointer& pointer = live_pointer();
    if (!placing_ || !pointer.down) return false;
    const PixelRect touch = touch_rect(output_width, output_height);
    if (!touch.contains(pointer.press_x, pointer.press_y)) return false;
    spot_ = TouchSpot{std::clamp((pointer.x - touch.x) / touch.w, 0.0f, 1.0f),
                      std::clamp((pointer.y - touch.y) / touch.h, 0.0f, 1.0f)};
    return true;
}

std::string ButtonMappingScreen::action_label(PadControl control) const {
    if (wii_u_) {
        const WiiUAction action = wii_u_mapping_.resolved(control, wii_u_controller_);
        if (action == WiiUAction::Turbo) {
            const WiiUAction of = wii_u_mapping_.turbo(control);
            return of == WiiUAction::Standard ? std::string("Turbo")
                                              : wii_u_action_name(of, wii_u_controller_) + " (turbo)";
        }
        std::string label = wii_u_action_name(action, wii_u_controller_);
        // And the profile it switches to when held.
        if (!wii_u_mapping_.held(control).empty()) label += " \u00b7 Hold: " + wii_u_mapping_.held(control);
        return label;
    }
    const PadAction action = mapping_.resolved(control, console_);
    if (action == PadAction::Turbo) {
        const PadAction of = mapping_.turbo(control);
        return of == PadAction::Standard ? std::string("Turbo") : std::string(pad_action_name(of)) + " (turbo)";
    }
    return pad_action_name(action);
}

void ButtonMappingScreen::choose(int index, ButtonMappingScreenResult& result) {
    if (profiles_up_) {
        choose_profile(index, result);
        return;
    }
    if (holding_up_) {
        choose_hold(index, result);
        return;
    }
    if (wii_u_) {
        choose_wii_u(index, result);
        return;
    }
    if (on_right_stick()) {
        if (index < 0 || index >= static_cast<int>(choice_modes_.size())) return;
        const RightStickMode mode = choice_modes_[static_cast<std::size_t>(index)];
        if (mode == RightStickMode::TouchAnalog) {
            start_placing_stick();
            return;
        }
        if (mode != console_.right_stick) result.right_stick = mode;
        return;
    }
    if (index < 0 || index >= static_cast<int>(choice_actions_.size())) return;
    const PadControl control = focused_control();
    const PadAction action = choice_actions_[static_cast<std::size_t>(index)];
    // A button on turbo stays on it for another of the console's buttons.
    if (mapping_[control] == PadAction::Turbo && is_turbo_button(action)) {
        if (mapping_.turbo(control) == action) return;
        mapping_.turbo(control) = action;
        result.changed = mapping_;
        return;
    }
    if (action == PadAction::TouchPoint) {
        start_placing();
        return;
    }
    // What the standard mapping already does is kept as Standard, so a game
    // that put a button back by hand has nothing changed to save.
    const PadAction kept = action == standard_pad_action(control, console_) ? PadAction::Standard : action;
    if (kept == mapping_[control]) return;
    mapping_[control] = kept;
    mapping_.turbo(control) = PadAction::Standard;
    result.changed = mapping_;
}

bool ButtonMappingScreen::is_turbo_button(PadAction action) const {
    const std::vector<PadAction> buttons = turbo_choices(console_);
    return std::find(buttons.begin(), buttons.end(), action) != buttons.end();
}

bool ButtonMappingScreen::can_toggle_turbo() const {
    if (wii_u_) {
        if (on_sideways()) return false;
        const WiiUAction action = wii_u_mapping_.resolved(focused_control(), wii_u_controller_);
        return action == WiiUAction::Turbo || is_wii_u_turbo_button(action);
    }
    if (on_right_stick()) return false;
    const PadAction action = mapping_.resolved(focused_control(), console_);
    return action == PadAction::Turbo || action == PadAction::TouchPoint || is_turbo_button(action);
}

void ButtonMappingScreen::toggle_turbo(ButtonMappingScreenResult& result) {
    if (!can_toggle_turbo()) return;
    if (wii_u_) {
        toggle_wii_u_turbo(result);
        return;
    }
    const PadControl control = focused_control();
    const PadAction action = mapping_.resolved(control, console_);
    if (action == PadAction::Turbo) {
        const PadAction of = mapping_.turbo(control);
        mapping_[control] = of == standard_pad_action(control, console_) ? PadAction::Standard : of;
        mapping_.turbo(control) = PadAction::Standard;
    } else {
        mapping_[control] = PadAction::Turbo;
        mapping_.turbo(control) = action;
    }
    result.changed = mapping_;
}

void ButtonMappingScreen::ask_revert() {
    // Asked whatever the mapping is, so X always does what its prompt says.
    asking_ = Asking::Reset;
    confirming_ = true;
    confirm_cursor_.reset(2, 1);
}

void ButtonMappingScreen::answer_revert(int button, ButtonMappingScreenResult& result) {
    confirming_ = false;
    // A profile's question goes back to the profiles, on the one it was
    // about -- or, deleted, where it was.
    if (asking_ != Asking::Reset) {
        if (button == 1 && asking_ == Asking::Delete) result.profile_delete = asking_profile_;
        if (button == 1 && asking_ == Asking::SaveOver) result.profile_save_over = asking_profile_;
        open_profiles(asking_profile_ + 2);
        return;
    }
    if (button != 1) return;
    if (wii_u_) {
        // Every button, but not how the remote is held.
        const bool sideways = wii_u_mapping_.sideways;
        wii_u_mapping_ = WiiUMapping();
        wii_u_mapping_.sideways = sideways;
        result.wii_u_changed = wii_u_mapping_;
        return;
    }
    mapping_ = ButtonMapping();
    result.changed = mapping_;
}

PopupText ButtonMappingScreen::confirm_text() const {
    PopupText words;
    const std::string name = asking_profile_ >= 0 && asking_profile_ < static_cast<int>(profile_names_.size())
                                 ? profile_names_[static_cast<std::size_t>(asking_profile_)]
                                 : std::string("This profile");
    if (asking_ == Asking::Delete) {
        words.message = "\u201c" + name + "\u201d will be deleted.";
        return words;
    }
    if (asking_ == Asking::SaveOver) {
        words.message = "\u201c" + name + "\u201d will be replaced with the mapping as it is now.";
        return words;
    }
    words.message = wii_u_ ? "Every button will go back to the standard mapping for this controller."
                           : "Every button will go back to the standard mapping for this game.";
    return words;
}

std::vector<PopupButton> ButtonMappingScreen::confirm_buttons() const {
    if (asking_ == Asking::Delete) return {PopupButton{"Cancel", ""}, PopupButton{"Delete", ""}};
    if (asking_ == Asking::SaveOver) return {PopupButton{"Cancel", ""}, PopupButton{"Save", ""}};
    return {PopupButton{"Cancel", ""}, PopupButton{"Reset", ""}};
}

PopupGeometry ButtonMappingScreen::confirm_popup(float output_width, float output_height) const {
    const PopupText words = confirm_text();
    return popup_geometry(output_width, output_height,
                          popup_text_height(words, popup_body_width(output_width, output_height),
                                            output_height),
                          confirm_buttons());
}

ButtonMappingScreenResult ButtonMappingScreen::update(const std::vector<UiAction>& pressed,
                                                      const std::vector<PointerEvent>& touched,
                                                      float output_width, float output_height) {
    ButtonMappingScreenResult result;
    if (!open_) return result;
    // A tap on a prompt at the foot of the page is that button pressed --
    // but not while a popup is up over them.
    std::vector<UiAction> actions = pressed;
    std::vector<PointerEvent> pointers = touched;
    if ((!choice_.is_open() || profiles_up_) && !confirming_ && !naming_.is_open()) {
        take_guide_taps(guide_hits_, actions, pointers);
    }
    // A tap on Touch Point's page lands on the screen, wherever it is.
    last_width_ = output_width;
    last_height_ = output_height;

    // A new profile's name: typed, then OK or Cancel.
    if (naming_.is_open()) {
        for (const UiAction action : actions) {
            if (action == UiAction::ToggleMenu) {
                result.close_requested = true;
                result.close_everything = true;
                close();
                return result;
            }
        }
        const TextEntryResult answer =
            naming_.update(actions, {}, typed_, backspaces_, output_width, output_height);
        typed_.clear();
        backspaces_ = 0;
        finish_naming(answer, result);
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
        return result;
    }
    // The picker takes every press until put away -- the buttons here, a tap
    // the one way every tap is heard, below. Over Mapping Profiles, - puts
    // it away and Y and X ask about the profile the cursor is on.
    if (choice_.is_open()) {
        if (profiles_up_) {
            std::vector<UiAction> rest;
            for (const UiAction action : actions) {
                if (action == UiAction::MenuMinus) {
                    choice_.close();
                    profiles_up_ = false;
    holding_up_ = false;
                } else if (action == UiAction::MenuQuit) {
                    ask_about_profile(Asking::Delete);
                } else if (action == UiAction::MenuReset) {
                    ask_about_profile(Asking::SaveOver);
                } else {
                    rest.push_back(action);
                }
                if (!choice_.is_open()) break;
            }
            actions = rest;
            if (!choice_.is_open()) {
                take_taps(pointers, output_width, output_height);
                return result;
            }
        }
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
    // And Touch Point's page: the spot kept or not. The pad moves it
    // smoothly, through steer(), rather than a step a press.
    if (placing_) {
        drag_spot(output_width, output_height);
        for (const UiAction action : actions) {
            switch (action) {
                case UiAction::Confirm: finish_placing(result); break;
                case UiAction::Cancel: back_to_choice(PadAction::TouchPoint, output_width, output_height); break;
                // L and R: the Touch Analog Stick smaller and larger.
                case UiAction::PageUp:
                case UiAction::PageDown:
                    if (placing_stick_) {
                        constexpr float kStep = 0.02f;
                        stick_radius_ = clamp_stick_radius(stick_radius_ +
                                                           (action == UiAction::PageUp ? -kStep : kStep));
                    }
                    break;
                case UiAction::ToggleMenu:
                    result.close_requested = true;
                    result.close_everything = true;
                    close();
                    return result;
                default: break;
            }
            if (!placing_) break;
        }
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
        return result;
    }
    // And so does Reset to Default's question.
    if (confirming_) {
        for (const UiAction action : actions) {
            if (action == UiAction::ToggleMenu) {
                result.close_requested = true;
                result.close_everything = true;
                close();
                return result;
            }
        }
        const int answer =
            confirm_cursor_.update(actions, {}, confirm_popup(output_width, output_height));
        if (answer == PopupCursor::kCancelled) {
            answer_revert(0, result);
        } else if (answer >= 0) {
            answer_revert(answer, result);
        }
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
        return result;
    }

    const int last = row_count() - 1;
    for (const UiAction action : actions) {
        switch (action) {
            // No going round: the list stops at its ends.
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(last, focus_ + 1); break;
            case UiAction::Confirm: activate(result, output_width, output_height); break;
            // B a step back, to the menu; the menu's own button out of
            // everything.
            case UiAction::Cancel: result.close_requested = true; break;
            case UiAction::ToggleMenu:
                result.close_requested = true;
                result.close_everything = true;
                break;
            // X, where a Nintendo pad has it.
            case UiAction::MenuQuit: ask_revert(); break;
            // Y, where a Nintendo pad has it.
            case UiAction::MenuReset: toggle_turbo(result); break;
            // -: Mapping Profiles, on the one in use.
            case UiAction::MenuMinus: open_profiles(-1); break;
            case UiAction::Left:
            case UiAction::Right:
            case UiAction::PageUp:
            case UiAction::PageDown:
            case UiAction::MenuDisconnect:
            case UiAction::MenuSettings:
            case UiAction::OpenMenu:
            case UiAction::Quit:
            case UiAction::ToggleFilter:
            case UiAction::NextLayout:
            case UiAction::SwapLayout: break;
        }
        if (choice_.is_open() || confirming_ || result.close_requested) break;
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

void ButtonMappingScreen::handle_tap(int entry, ButtonMappingScreenResult& result, float output_width,
                                     float output_height) {
    if (!open_) return;
    if (entry == kTapOffPopup) {
        // Off the picker or the question: put away, nothing changed -- a
        // profile's question back to the profiles. A name being typed stays.
        if (naming_.is_open()) return;
        if (confirming_ && asking_ != Asking::Reset) {
            confirming_ = false;
            open_profiles(asking_profile_ + 2);
            return;
        }
        choice_.close();
        profiles_up_ = false;
    holding_up_ = false;
        confirming_ = false;
        return;
    }
    if (entry < 0) return;
    if (entry >= kSpotEntryBase) {
        // The spot goes where the finger was; A keeps it.
        if (placing_) spot_ = entry_spot(entry);
        return;
    }
    if (placing_) {
        if (entry == kPlacingOkEntry) finish_placing(result);
        return;
    }
    if (entry >= kNamingEntryBase) {
        if (naming_.is_open()) finish_naming(naming_.tap(entry - kNamingEntryBase), result);
        return;
    }
    if (naming_.is_open()) return;
    if (entry >= kConfirmEntryBase) {
        if (!confirming_) return;
        const int answer = confirm_cursor_.tap(entry - kConfirmEntryBase);
        if (answer >= 0) answer_revert(answer, result);
        return;
    }
    if (entry >= kChoiceEntryBase) {
        // A dropdown's choice is chosen by its first tap.
        if (!choice_.is_open()) return;
        const int index = entry - kChoiceEntryBase;
        if (index >= static_cast<int>(choice_.choices().size())) return;
        choice_.close();
        choose(index, result);
        return;
    }
    if (choice_.is_open() || confirming_ || entry >= row_count()) return;
    // A tap chooses.
    if (tap_chooses(focus_, entry)) activate(result, output_width, output_height);
}

std::string ButtonMappingScreen::caption() const {
    if (wii_u_) return wii_u_caption();
    if (on_right_stick()) {
        switch (console_.right_stick) {
            case RightStickMode::CStick: return "is the 3DS's C-Stick.";
            case RightStickMode::CirclePad: return "moves the Circle Pad, as the left stick does.";
            case RightStickMode::DPad: return "presses the D-Pad's directions.";
            case RightStickMode::TouchCursor: return "moves the Touch Cursor over the touch screen.";
            case RightStickMode::TouchAnalog: return "drags a touch analog stick on the touch screen.";
            case RightStickMode::Off: break;
        }
        return "is disabled in this game.";
    }
    const PadControl control = focused_control();
    const PadAction action = mapping_.resolved(control, console_);
    DsButton button = DsButton::A;
    switch (action) {
        case PadAction::QuickMenu: return "opens the menu.";
        case PadAction::SwapScreens: return "swaps the screens on certain screen layouts.";
        case PadAction::BlowIntoMic: return "blows into the microphone while it's held.";
        case PadAction::TouchCursor: return "touches the screen at the Touch Cursor.";
        case PadAction::Nothing: return "is disabled in this game.";
        case PadAction::NextLayout: return "moves on to the next screen layout.";
        case PadAction::TouchPoint: return "touches the touch screen here while it's held.";
        case PadAction::Turbo: {
            const PadAction of = mapping_.turbo(control);
            if (of == PadAction::Standard) return "does nothing until Turbo has a button.";
            if (of == PadAction::TouchPoint) return "touches the touch screen here over and over while it's held.";
            return std::string("presses the ") + pad_action_name(of) + " over and over while it's held.";
        }
        default: break;
    }
    if (ds_button_for_pad_action(action, button)) {
        return std::string("presses the ") + pad_action_name(action) + " on the " + console_name() + ".";
    }
    return std::string();
}

void ButtonMappingScreen::draw_console(QuadRenderer& quads, const PixelRect& box, PadControl control) const {
    const PadAction resolved = mapping_.resolved(control, console_);
    // Turbo picks out the button it presses; the right stick's row the touch
    // screen, for a stick that touches it.
    const RightStickMode stick = console_.right_stick;
    const bool stick_touches = stick == RightStickMode::TouchCursor || stick == RightStickMode::TouchAnalog;
    const PadAction action = on_right_stick() ? (stick_touches ? PadAction::TouchCursor : PadAction::Standard)
                             : resolved == PadAction::Turbo ? mapping_.turbo(control)
                                                            : resolved;
    const UiTheme& theme = ui_theme();
    const bool three_ds = system_ == SystemId::ThreeDs;
    // Drawn on a grid of 460 by 520, scaled to fit the room it has.
    constexpr float kWidth = 460.0f;
    constexpr float kHeight = 520.0f;
    const float unit = std::min(box.w / kWidth, box.h / kHeight);
    const float left = box.x + (box.w - kWidth * unit) * 0.5f;
    const float top = box.y + (box.h - kHeight * unit) * 0.5f;
    const auto at = [&](float x, float y, float w, float h) {
        return PixelRect{std::round(left + x * unit), std::round(top + y * unit), std::round(w * unit),
                         std::round(h * unit)};
    };
    const float stroke = std::max(1.5f, std::round(unit * 3.0f));
    const float thin = std::max(1.0f, std::round(unit * 2.0f));
    const ConsolePart picked = part_for(action);
    Color faint = theme.accent;
    faint.a = 0.25f;
    Color dim = theme.text_dim;

    const auto ring = [&](const PixelRect& rect, float radius, float width, const Color& color) {
        quads.draw_rounded_ring(rect, radius, width, color, color);
    };
    // A part: picked out in the accent colour when it is the one, drawn in
    // outline otherwise.
    const auto part = [&](ConsolePart which, const PixelRect& rect, float radius) {
        if (which == picked) {
            quads.draw_rounded_rect(rect, radius, theme.accent);
        } else {
            ring(rect, radius, thin, theme.text);
        }
    };
    const auto circle = [&](ConsolePart which, float cx, float cy, float r) {
        part(which, at(cx - r, cy - r, r * 2.0f, r * 2.0f), std::round(r * unit));
    };

    // The two halves, the touch screen below, and the top screen above it.
    ring(at(40, 10, 380, 236), std::round(18 * unit), stroke, theme.text);
    ring(at(40, 270, 380, 236), std::round(18 * unit), stroke, theme.text);
    ring(three_ds ? at(90, 36, 280, 168) : at(118, 38, 224, 168), std::round(3 * unit), thin, dim);
    const PixelRect touch = three_ds ? at(128, 292, 204, 153) : at(118, 298, 224, 168);
    if (picked == ConsolePart::Touch || action == PadAction::TouchPoint) {
        quads.draw_rounded_rect(touch, std::round(3 * unit), faint);
        ring(touch, std::round(3 * unit), stroke, theme.accent);
    } else {
        ring(touch, std::round(3 * unit), thin, dim);
    }
    // And Touch Point where it touches.
    if (action == PadAction::TouchPoint) {
        const TouchSpot& spot = mapping_.spot(control);
        const float r = std::round(11.0f * unit);
        const PixelRect dot{std::round(touch.x + spot.u * touch.w - r), std::round(touch.y + spot.v * touch.h - r),
                            r * 2.0f, r * 2.0f};
        quads.draw_rounded_rect(dot, r, theme.accent);
        ring(dot, r, thin, theme.text);
    }
    // And the Touch Analog Stick where it sits, as big as it is.
    if (on_right_stick() && stick == RightStickMode::TouchAnalog) {
        const float r = std::max(2.0f, std::round(mapping_.stick_radius * touch.h));
        const float cx = std::round(touch.x + mapping_.stick_centre.u * touch.w);
        const float cy = std::round(touch.y + mapping_.stick_centre.v * touch.h);
        quads.set_clip(touch);
        ring(PixelRect{cx - r, cy - r, r * 2.0f, r * 2.0f}, r, thin, theme.text);
        const float dot = std::round(5.0f * unit);
        quads.draw_rounded_rect(PixelRect{cx - dot, cy - dot, dot * 2.0f, dot * 2.0f}, dot, theme.accent);
        quads.clear_clip();
    }
    // The hinge, and on a DS the microphone in the middle of it.
    quads.draw_solid(at(60, 257, 340, 2), dim);
    if (!three_ds) circle(ConsolePart::Mic, 230, 258, 5);

    // The shoulders, along the top of the lower half; a 3DS's ZL and ZR
    // inside them, as a New 3DS has them.
    part(ConsolePart::L, at(44, 262, 56, 12), std::round(6 * unit));
    part(ConsolePart::R, at(360, 262, 56, 12), std::round(6 * unit));
    if (three_ds) {
        part(ConsolePart::Zl, at(108, 262, 36, 10), std::round(5 * unit));
        part(ConsolePart::Zr, at(316, 262, 36, 10), std::round(5 * unit));
    }

    // The d-pad, each arm on its own, under a 3DS's circle pad.
    const float pad_x = 82.0f;
    const float pad_y = three_ds ? 386.0f : 368.0f;
    part(ConsolePart::Up, at(pad_x - 6, pad_y - 18, 12, 13), std::round(2 * unit));
    part(ConsolePart::Down, at(pad_x - 6, pad_y + 5, 12, 13), std::round(2 * unit));
    part(ConsolePart::Left, at(pad_x - 18, pad_y - 6, 13, 12), std::round(2 * unit));
    part(ConsolePart::Right, at(pad_x + 5, pad_y - 6, 13, 12), std::round(2 * unit));
    if (three_ds) {
        ring(at(pad_x - 16, 316 - 16, 32, 32), std::round(16 * unit), thin, dim);
    }

    // The face buttons, where a Nintendo console has them.
    const float face_x = 384.0f;
    const float face_y = three_ds ? 340.0f : 366.0f;
    circle(ConsolePart::X, face_x, face_y - 20, 9);
    circle(ConsolePart::Y, face_x - 20, face_y, 9);
    circle(ConsolePart::A, face_x + 20, face_y, 9);
    circle(ConsolePart::B, face_x, face_y + 20, 9);

    // Start and Select: a DS's beneath the face buttons, a 3DS's under its
    // touch screen either side of HOME.
    if (three_ds) {
        circle(ConsolePart::Select, 190, 470, 6);
        ring(at(224, 464, 12, 12), std::round(6 * unit), thin, dim);
        circle(ConsolePart::Start, 270, 470, 6);
    } else {
        circle(ConsolePart::Start, face_x, 432, 5);
        circle(ConsolePart::Select, face_x, 452, 5);
    }
}

void ButtonMappingScreen::render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                 float output_width, float output_height) {
    if (!open_) return;
    if (placing_) {
        render_placing(quads, text, glyphs, output_width, output_height);
        return;
    }
    const ShownScroll shown(scroll_, scroll_ease_, drag_.dragging());
    const UiTheme& theme = ui_theme();
    const Geometry geometry = compute_geometry(output_width, output_height);
    const float scale = geometry.scale;
    const float line = geometry.line;
    const PageFrame frame = page_frame(output_width, output_height);
    draw_page_background(quads, output_width, output_height, over_game_);
    // And the profile the mapping is, if it is one.
    std::string detail = wii_u_ ? detail_ : std::string();
    if (profile_current_ >= 0) {
        detail += (detail.empty() ? "" : " \u00b7 ") + std::string("Profile: ") +
                  profile_names_[static_cast<std::size_t>(profile_current_)];
    }
    draw_page_header(quads, text, frame, wii_u_ ? "Button Mapping" : "Change Controls", detail);

    quads.set_clip(menu_list_clip(PixelRect{geometry.list.x, geometry.view_top, geometry.list.w,
                                            geometry.view_bottom - geometry.view_top},
                                  scale));
    const float pad = menu_row_pad(line);
    const float glyph_height = std::round(line * 1.7f);
    const std::size_t rows = static_cast<std::size_t>(row_count());
    for (std::size_t index = 0; index < rows; ++index) {
        const PixelRect& row = geometry.rows[index];
        const bool focused = static_cast<int>(index) == focus_;
        draw_menu_row(quads, row, focused, scale, index + 1 == rows,
                      static_cast<int>(index) == focus_ + 1);
        // Sideways Wii Remote: its name and its switch -- the remote drawn
        // on its side says what it does.
        if (has_sideways_row() && static_cast<int>(index) == kRightStickRow) {
            const float baseline = page_row_text_y(row, scale);
            text.draw(quads, "Sideways Wii Remote", row.x + pad, baseline, scale, menu_row_text(focused));
            draw_toggle(quads, row.x + row.w - pad, row.y + row.h * 0.5f, wii_u_mapping_.sideways, line);
            continue;
        }
        // The button as its icon alone, and what it does on the right; the
        // right stick, what it is.
        const bool stick_row = static_cast<int>(index) == kRightStickRow;
        const std::string value =
            stick_row ? std::string(to_string(console_.right_stick)) : action_label(kPadControlRows[index]);
        const float centre = row.y + row.h * 0.5f;
        glyphs.draw(quads, row_glyph(static_cast<int>(index)), row.x + pad,
                    std::round(centre - glyph_height * glyphs.button_centre()), glyph_height, theme.text);
        const float value_left = row.x + pad + glyphs.width_for(glyph_height) + line * 1.2f;
        const float baseline = page_row_text_y(row, scale);
        text.draw(quads, TextRenderer::elide_right(value, row.x + row.w - pad - value_left, scale),
                  row.x + row.w - pad, baseline, scale, theme.text_dim, TextAlign::Right);
    }
    quads.clear_clip();
    const float view = geometry.view_bottom - geometry.view_top;
    if (geometry.content_height > view + 0.5f) {
        draw_scroll_bar(quads, geometry.list, scroll_ / geometry.content_height,
                        view / geometry.content_height, line, scale);
    }

    // The console, what the highlighted button presses picked out on it.
    if (wii_u_) {
        // Sideways Wii Remote's row picks out nothing.
        const WiiUAction resolved = on_sideways() ? WiiUAction::Standard
                                                  : wii_u_mapping_.resolved(focused_control(), wii_u_controller_);
        draw_wii_u_controller(quads, geometry.diagram, wii_u_controller_,
                              resolved == WiiUAction::Turbo ? wii_u_mapping_.turbo(focused_control()) : resolved,
                              wii_u_mapping_.sideways);
    } else {
        draw_console(quads, geometry.diagram, focused_control());
    }
    const float caption_scale = scale * 0.85f;
    // The button leads, as its icon: wrapped with a word as wide standing in
    // for it, then drawn in that word's place.
    const float icon_height = std::round(TextRenderer::line_height(caption_scale) * 1.4f);
    const float icon_width = glyphs.width_for(icon_height);
    std::string stand_in;
    while (TextRenderer::measure_width(stand_in, caption_scale) < icon_width) stand_in += "M";
    // Sideways Wii Remote's row is no button: nothing said.
    std::vector<std::string> words;
    if (!on_sideways()) words = TextRenderer::wrap(stand_in + " " + caption(), geometry.diagram.w, caption_scale);
    if (words.size() > 2) {
        words[1] = TextRenderer::elide_right(words[1] + " " + words[2], geometry.diagram.w, caption_scale);
        words.resize(2);
    }
    for (std::size_t at = 0; at < words.size(); ++at) {
        const float y = geometry.caption_y + TextRenderer::wrapped_line_step(caption_scale) * static_cast<float>(at);
        const float centre_x = geometry.diagram.x + geometry.diagram.w * 0.5f;
        if (at == 0 && words[0].rfind(stand_in, 0) == 0) {
            const std::string rest = words[0].substr(stand_in.size());
            const float width = icon_width + TextRenderer::measure_width(rest, caption_scale);
            const float left = std::round(centre_x - width * 0.5f);
            const float text_centre = y + TextRenderer::line_height(caption_scale) * 0.5f;
            glyphs.draw(quads, row_glyph(focus_), left,
                        std::round(text_centre - icon_height * glyphs.button_centre()), icon_height, theme.text_dim);
            text.draw(quads, rest, left + icon_width, y, caption_scale, theme.text_dim);
            continue;
        }
        text.draw(quads, words[at], centre_x, y, caption_scale, theme.text_dim, TextAlign::Center);
    }

    std::vector<GuidePrompt> guide;
    if (profiles_open()) {
        // What the profile the cursor is on can have done to it.
        if (profile_of_line(choice_.focus()) >= 0) {
            guide.push_back({ButtonGlyph::X, "Delete"});
            guide.push_back({ButtonGlyph::Y, "Save Over"});
        }
        guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
        guide.push_back({ButtonGlyph::A, choice_.focus() == 0 ? "OK" : "Load", PromptRole::ActsOnSelection});
    } else {
        guide.push_back({ButtonGlyph::Minus, "Profiles"});
        guide.push_back({ButtonGlyph::X, "Reset to Default"});
        if (can_toggle_turbo()) guide.push_back({ButtonGlyph::Y, "Toggle Turbo"});
        guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
        guide.push_back({ButtonGlyph::A, on_sideways() ? "OK" : "Change", PromptRole::ActsOnSelection});
    }
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, guide, &guide_hits_);

    if (choice_.is_open()) {
        // Mapping Profiles stay where they opened, at the top of the page.
        if (!profiles_up_) choice_.set_row(geometry.rows[static_cast<std::size_t>(focus_)]);
        choice_.render(quads, text, output_width, output_height);
    }
    if (confirming_) {
        const PopupGeometry popup = confirm_popup(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, confirm_buttons(),
                   confirm_cursor_.focus(), confirm_cursor_.shown());
        draw_popup_text(quads, text, confirm_text(), popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
    naming_.render(quads, text, output_width, output_height);
}

void ButtonMappingScreen::render_placing(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                         float output_width, float output_height) {
    const UiTheme& theme = ui_theme();
    const PageFrame frame = page_frame(output_width, output_height);
    const float line = frame.line;
    const float scale = frame.scale;
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame,
                     placing_stick_ ? std::string("Touch Analog Stick")
                                    : std::string("Touch Point for ") + pad_control_name(focused_control()));
    const PixelRect touch = touch_rect(output_width, output_height);
    if (touch_screen_ != nullptr && touch_screen_->valid()) {
        quads.draw_screen(touch, *touch_screen_);
    } else {
        quads.draw_solid(touch, Color{0.0f, 0.0f, 0.0f, 1.0f});
    }
    const float border = std::max(1.0f, std::round(scale * 0.8f));
    quads.draw_rounded_ring(touch, std::round(line * 0.15f), border, theme.text_dim, theme.text_dim);
    // Where it touches: a ring on the spot, and a cross through it to aim by
    // -- or the Touch Analog Stick, as big as it is, its centre marked.
    const float cx = std::round(touch.x + spot_.u * touch.w);
    const float cy = std::round(touch.y + spot_.v * touch.h);
    // White edged in black, to show on whatever the game has there.
    const float bar = std::max(2.0f, std::round(scale * 1.2f));
    const float edge = std::max(1.0f, std::round(scale * 0.6f));
    const Color white{1.0f, 1.0f, 1.0f, 1.0f};
    const Color shade{0.0f, 0.0f, 0.0f, 0.7f};
    Color fill = theme.accent;
    fill.a = placing_stick_ ? 0.25f : 0.55f;
    quads.set_clip(touch);
    const float r = placing_stick_ ? std::max(4.0f, std::round(stick_radius_ * touch.h)) : std::round(line * 1.1f);
    const PixelRect ring{cx - r, cy - r, r * 2.0f, r * 2.0f};
    if (!placing_stick_) {
        quads.draw_solid(
            PixelRect{cx - bar * 0.5f - edge, cy - r * 1.8f - edge, bar + edge * 2.0f, r * 3.6f + edge * 2.0f}, shade);
        quads.draw_solid(
            PixelRect{cx - r * 1.8f - edge, cy - bar * 0.5f - edge, r * 3.6f + edge * 2.0f, bar + edge * 2.0f}, shade);
    }
    quads.draw_rounded_rect(ring, r, fill);
    quads.draw_rounded_ring(PixelRect{ring.x - edge, ring.y - edge, ring.w + edge * 2.0f, ring.h + edge * 2.0f},
                            r + edge, bar + edge * 2.0f, shade, shade);
    quads.draw_rounded_ring(ring, r, bar, white, white);
    if (placing_stick_) {
        const float dot = std::round(line * 0.4f);
        const PixelRect centre{cx - dot, cy - dot, dot * 2.0f, dot * 2.0f};
        quads.draw_rounded_rect(PixelRect{centre.x - edge, centre.y - edge, centre.w + edge * 2.0f,
                                          centre.h + edge * 2.0f},
                                dot + edge, shade);
        quads.draw_rounded_rect(centre, dot, white);
    } else {
        quads.draw_solid(PixelRect{cx - bar * 0.5f, cy - r * 1.8f, bar, r * 3.6f}, white);
        quads.draw_solid(PixelRect{cx - r * 1.8f, cy - bar * 0.5f, r * 3.6f, bar}, white);
    }
    quads.clear_clip();
    text.draw(quads,
              placing_stick_ ? "Choose a location for the center of the touch analog stick."
                             : "Choose a point for this button to tap.",
              frame.panel.x + frame.panel.w * 0.5f,
              std::round(touch.y + touch.h + line * 0.8f), scale * 0.9f, theme.text_dim, TextAlign::Center);
    // OK, ringed as the one thing A presses here -- while the cursor is
    // drawn at all; filled while a finger is held on it.
    const PixelRect ok = placing_ok_rect(output_width, output_height);
    // Its corners those of the cursor on any row, not a capsule's.
    const float ok_radius = std::round(std::min(ok.h * 0.22f, scale * 3.5f));
    quads.draw_rounded_rect(ok, ok_radius, Color{1.0f, 1.0f, 1.0f, 0.10f});
    if (pointer_holding(ok)) {
        quads.draw_rounded_rect(ok, ok_radius, menu_highlight_fill());
    } else if (menu_cursor_shown()) {
        draw_menu_cursor(quads, ok, ok_radius, scale);
    }
    text.draw(quads, "OK", ok.x + ok.w * 0.5f, std::round(ok.y + (ok.h - TextRenderer::line_height(scale)) * 0.5f),
              scale, theme.text, TextAlign::Center);

    guide_hits_.clear();
    std::vector<GuidePrompt> guide;
    if (placing_stick_) {
        guide.push_back({ButtonGlyph::L, "Smaller"});
        guide.push_back({ButtonGlyph::R, "Larger"});
    }
    guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    guide.push_back({ButtonGlyph::A, "OK", PromptRole::ActsOnSelection});
    draw_page_guide(quads, text, glyphs, frame, guide, &guide_hits_);
}

void ButtonMappingScreen::show_mirrored(const net::ButtonMappingViewMessage& view, float output_width,
                                        float output_height) {
    if (!open_) open();
    last_width_ = output_width;
    last_height_ = output_height;
    focus_ = std::clamp(static_cast<int>(view.focus), 0, row_count() - 1);
    // The other device's cursor brings the list with it when it moves;
    // otherwise the list is where a finger here left it.
    if (focus_ != mirrored_focus_) {
        drag_.stop();
        scroll_focus_into_view(compute_geometry(output_width, output_height));
        mirrored_focus_ = focus_;
    }
    set_profiles(view.profile_names, view.profile_current, view.profile_heading);
    if (view.choice_focus == 0) {
        choice_.close();
        profiles_up_ = false;
    holding_up_ = false;
    } else if (view.profiles) {
        if (!profiles_open()) open_profiles(view.choice_focus - 1);
        if (choice_.focus() != view.choice_focus - 1) choice_.set_focus(view.choice_focus - 1);
    } else if (view.holding) {
        if (!hold_choice_open()) open_hold_choice();
        if (choice_.focus() != view.choice_focus - 1) choice_.set_focus(view.choice_focus - 1);
    } else {
        if (!choice_.is_open() || profiles_up_ || holding_up_) open_choice(output_width, output_height);
        if (choice_.focus() != view.choice_focus - 1) choice_.set_focus(view.choice_focus - 1);
    }
    // A new profile's name, as the other device has it -- unless typed here
    // just now.
    if (view.naming) {
        if (!naming_.is_open()) {
            naming_.open("Name the new profile.", view.name_text, MappingProfiles::kMaxName);
            own_name_views_ = 0;
        }
        if (own_name_views_ > 0) {
            --own_name_views_;
        } else {
            naming_.set_text(view.name_text);
        }
        naming_.set_focus(view.naming_focus);
    } else {
        naming_.close();
    }
    asking_ = static_cast<Asking>(std::min<uint8_t>(view.asking, static_cast<uint8_t>(Asking::SaveOver)));
    asking_profile_ = view.asking_profile;
    // Where the other device says -- unless the pad here has just moved it,
    // which the other device has yet to hear.
    if (view.placing && (!placing_ || own_spot_seconds_ <= 0.0f)) spot_ = TouchSpot{view.spot_u, view.spot_v};
    placing_ = view.placing;
    placing_stick_ = view.placing_stick;
    stick_radius_ = view.stick_radius;
    // A finger here moving it is shown as it goes; where it lifts is told.
    drag_spot(output_width, output_height);
    if (view.confirm_focus == 0) {
        confirming_ = false;
    } else {
        if (!confirming_) {
            confirming_ = true;
            confirm_cursor_.reset(2, 1);
        }
        confirm_cursor_.set_focus(view.confirm_focus - 1);
    }
}

bool ButtonMappingScreen::scroll(const std::vector<PointerEvent>& pointers, float output_width,
                                 float output_height) {
    if (naming_.is_open()) return false;
    if (choice_.is_open()) return choice_.scroll(pointers, output_width, output_height);
    if (confirming_ || placing_) return false;
    const Geometry geometry = compute_geometry(output_width, output_height);
    drag_.update(pointers, geometry.list, scroll_, geometry.max_scroll());
    return drag_.release_was_drag();
}

int ButtonMappingScreen::entry_at(float x, float y, float output_width, float output_height) const {
    if (placing_) {
        if (placing_ok_rect(output_width, output_height).contains(x, y)) return kPlacingOkEntry;
        const PixelRect touch = touch_rect(output_width, output_height);
        if (!touch.contains(x, y)) return -1;
        return spot_entry(TouchSpot{std::clamp((x - touch.x) / touch.w, 0.0f, 1.0f),
                                    std::clamp((y - touch.y) / touch.h, 0.0f, 1.0f)});
    }
    if (naming_.is_open()) {
        const int button = naming_.button_at(x, y, output_width, output_height);
        return button >= 0 ? kNamingEntryBase + button : -1;
    }
    if (choice_.is_open()) {
        const int choice = choice_.choice_at(x, y, output_width, output_height);
        return choice >= 0 ? kChoiceEntryBase + choice : -1;
    }
    if (confirming_) {
        const int button = popup_button_at(confirm_popup(output_width, output_height), x, y);
        return button >= 0 ? kConfirmEntryBase + button : -1;
    }
    const Geometry geometry = compute_geometry(output_width, output_height);
    if (!geometry.list.contains(x, y)) return -1;
    for (std::size_t index = 0; index < geometry.rows.size(); ++index) {
        if (geometry.rows[index].contains(x, y)) return static_cast<int>(index);
    }
    return -1;
}

bool ButtonMappingScreen::off_popup(float x, float y, float output_width, float output_height) const {
    if (naming_.is_open()) return false;
    if (choice_.is_open()) return !choice_.contains(x, y, output_width, output_height);
    if (confirming_) return !confirm_popup(output_width, output_height).box.contains(x, y);
    return false;
}

std::vector<float> ButtonMappingScreen::scroll_state(float output_width, float output_height) const {
    const Geometry geometry = compute_geometry(output_width, output_height);
    return {scroll_in_lines(scroll_, geometry.scale), choice_.is_open() ? choice_.scroll_rows() : 0.0f};
}

void ButtonMappingScreen::set_scroll_state(const std::vector<float>& state, float output_width,
                                           float output_height) {
    const Geometry geometry = compute_geometry(output_width, output_height);
    scroll_ = std::clamp(scroll_from_lines(scroll_at(state, 0), geometry.scale), 0.0f, geometry.max_scroll());
    drag_.stop();
    if (choice_.is_open()) choice_.set_scroll_rows(scroll_at(state, 1));
}

void ButtonMappingScreen::describe_for_sound(UiSoundView& view) const {
    mix_sound_state(view.focus, static_cast<uint64_t>(focus_));
    if (choice_.is_open()) mix_sound_state(view.focus, static_cast<uint64_t>(choice_.focus() + 1000));
    if (confirming_) mix_sound_state(view.focus, static_cast<uint64_t>(confirm_cursor_.focus() + 2000));
    if (naming_.is_open()) mix_sound_state(view.focus, static_cast<uint64_t>(naming_.focus() + 3000));
    mix_sound_state(view.rest, profiles_up_ ? 1u : 0u);
    mix_sound_state(view.rest, naming_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, static_cast<uint64_t>(profile_current_ + 1));
    mix_sound_state(view.rest, static_cast<uint64_t>(profile_names_.size()));
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, choice_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, confirming_ ? 1u : 0u);
    mix_sound_state(view.rest, placing_ ? 1u : 0u);
    if (placing_) mix_sound_state(view.focus, static_cast<uint64_t>(spot_entry(spot_)));
    if (placing_stick_) mix_sound_state(view.focus, static_cast<uint64_t>(std::lround(stick_radius_ * 1000.0f)));
    mix_sound_state(view.rest, static_cast<uint64_t>(console_.right_stick));
    for (const PadAction action : mapping_.actions) mix_sound_state(view.rest, static_cast<uint64_t>(action));
    for (const PadAction action : mapping_.turbo_of) mix_sound_state(view.rest, static_cast<uint64_t>(action));
    mix_sound_state(view.rest, wii_u_ ? 1u : 0u);
    for (const WiiUAction action : wii_u_mapping_.actions) mix_sound_state(view.rest, static_cast<uint64_t>(action));
    for (const WiiUAction action : wii_u_mapping_.turbo_of) mix_sound_state(view.rest, static_cast<uint64_t>(action));
    mix_sound_state(view.rest, wii_u_mapping_.sideways ? 1u : 0u);
    if (on_sideways() && !choice_.is_open() && !confirming_ && !naming_.is_open()) {
        view.toggle = wii_u_mapping_.sideways ? 1 : 0;
    }
}

net::ButtonMappingViewMessage button_mapping_view_of(const ButtonMappingScreen& screen) {
    net::ButtonMappingViewMessage view;
    view.open = screen.is_open();
    if (!view.open) return view;
    view.focus = static_cast<uint8_t>(screen.focus());
    view.choice_focus = screen.choosing() ? static_cast<uint8_t>(screen.choice_focus() + 1) : 0;
    view.confirm_focus = screen.confirming() ? static_cast<uint8_t>(screen.confirm_focus() + 1) : 0;
    view.profiles = screen.profiles_open();
    view.holding = screen.hold_choice_open();
    view.profile_names = screen.profile_names();
    view.profile_current = screen.profile_current();
    view.profile_heading = screen.profile_heading();
    view.naming = screen.naming();
    if (view.naming) {
        view.name_text = screen.name_text();
        view.naming_focus = static_cast<uint8_t>(std::max(0, screen.naming_focus()));
    }
    view.asking = static_cast<uint8_t>(screen.asking());
    view.asking_profile = screen.asking_profile();
    view.wii_u = screen.wii_u();
    if (view.wii_u) {
        view.wii_u_controller = static_cast<uint8_t>(screen.wii_u_controller());
        view.wii_u_detail = screen.detail();
        view.wii_u_mapping = screen.wii_u_mapping();
    }
    view.placing = screen.placing();
    if (view.placing) {
        view.spot_u = screen.spot().u;
        view.spot_v = screen.spot().v;
        view.placing_stick = screen.placing_stick();
        view.stick_radius = screen.stick_radius();
    }
    return view;
}

}  // namespace ds
