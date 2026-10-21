#include "ui/button_mapping_screen.hpp"

#include <algorithm>
#include <cmath>

#include "ui/page_frame.hpp"
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

const char* group_heading(PadActionGroup group) {
    switch (group) {
        case PadActionGroup::Console: break;
        case PadActionGroup::TouchScreen: return "Touch Screen";
        case PadActionGroup::Dualscreen: return "Dualscreen";
    }
    return "";
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
    mirrored_focus_ = -1;
}

void ButtonMappingScreen::set_mapping(const ButtonMapping& mapping, SystemId system,
                                      const PadConsole& console, const std::string& game_title) {
    mapping_ = mapping;
    system_ = system;
    console_ = console;
    game_title_ = game_title;
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
    geometry.content_height = static_cast<float>(kPadControls) * step;
    const bool scrolls = geometry.max_scroll() > 0.5f;
    const float row_width =
        list_width - (scrolls ? scroll_bar_width(geometry.scale) + frame.line * 0.6f : 0.0f);
    for (std::size_t index = 0; index < kPadControls; ++index) {
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
    // A button on turbo shows the button it presses as its choice.
    const PadAction resolved = mapping_.resolved(focused_control(), console_);
    const PadAction now = resolved == PadAction::Turbo ? mapping_.turbo(focused_control()) : resolved;
    std::vector<std::string> labels;
    std::vector<bool> headings;
    std::vector<std::string> hints;
    choice_actions_.clear();
    int current = -1;
    bool first = true;
    PadActionGroup group = PadActionGroup::Console;
    for (const PadAction action : pad_action_choices(console_)) {
        // A heading over each group, the console's own first.
        const PadActionGroup here = pad_action_group(action);
        if (first || here != group) {
            labels.push_back(here == PadActionGroup::Console ? console_name() + " Buttons"
                                                              : group_heading(here));
            headings.push_back(true);
            hints.emplace_back();
            choice_actions_.push_back(PadAction::Standard);
            group = here;
            first = false;
        }
        if (action == now) current = static_cast<int>(labels.size());
        // Touch Point goes on to a page of its own, which the words say.
        labels.push_back(action == PadAction::TouchPoint ? std::string("Touch Point\u2026")
                                                         : std::string(pad_action_name(action)));
        hints.emplace_back();
        headings.push_back(false);
        choice_actions_.push_back(action);
    }
    choice_.open(std::move(labels), current >= 0 ? current : 1,
                 geometry.rows[static_cast<std::size_t>(focus_)]);
    choice_.set_headings(std::move(headings));
    choice_.set_hints(std::move(hints));
}

void ButtonMappingScreen::back_to_choice(PadAction on, float output_width, float output_height) {
    placing_ = false;
    open_choice(output_width, output_height);
    for (std::size_t index = 0; index < choice_actions_.size(); ++index) {
        if (choice_actions_[index] == on && !choice_.heading(static_cast<int>(index))) {
            choice_.set_focus(static_cast<int>(index));
        }
    }
}

void ButtonMappingScreen::start_placing() {
    choice_.close();
    placing_ = true;
    spot_ = mapping_.spot(focused_control());
}

void ButtonMappingScreen::finish_placing(ButtonMappingScreenResult& result) {
    placing_ = false;
    const PadControl control = focused_control();
    if (mapping_[control] == PadAction::TouchPoint && mapping_.spot(control) == spot_) return;
    mapping_[control] = PadAction::TouchPoint;
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

void ButtonMappingScreen::drag_spot(float output_width, float output_height) {
    const LivePointer& pointer = live_pointer();
    if (!placing_ || !pointer.down) return;
    const PixelRect touch = touch_rect(output_width, output_height);
    if (!touch.contains(pointer.press_x, pointer.press_y)) return;
    spot_ = TouchSpot{std::clamp((pointer.x - touch.x) / touch.w, 0.0f, 1.0f),
                      std::clamp((pointer.y - touch.y) / touch.h, 0.0f, 1.0f)};
}

std::string ButtonMappingScreen::action_label(PadControl control) const {
    const PadAction action = mapping_.resolved(control, console_);
    if (action == PadAction::Turbo) {
        const PadAction of = mapping_.turbo(control);
        return of == PadAction::Standard ? std::string("Turbo") : std::string(pad_action_name(of)) + " (turbo)";
    }
    return pad_action_name(action);
}

void ButtonMappingScreen::choose(int index, ButtonMappingScreenResult& result) {
    if (index < 0 || index >= static_cast<int>(choice_actions_.size()) || choice_.heading(index)) return;
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
    const PadAction action = mapping_.resolved(focused_control(), console_);
    return action == PadAction::Turbo || is_turbo_button(action);
}

void ButtonMappingScreen::toggle_turbo(ButtonMappingScreenResult& result) {
    if (!can_toggle_turbo()) return;
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
    if (mapping_.is_standard(console_)) return;
    confirming_ = true;
    confirm_cursor_.reset(2, 1);
}

void ButtonMappingScreen::answer_revert(int button, ButtonMappingScreenResult& result) {
    confirming_ = false;
    if (button != 1) return;
    mapping_ = ButtonMapping();
    result.changed = mapping_;
}

PopupText ButtonMappingScreen::confirm_text() const {
    PopupText words;
    words.message = "Every button will go back to the standard mapping for this game.";
    return words;
}

std::vector<PopupButton> ButtonMappingScreen::confirm_buttons() const {
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
    if (!choice_.is_open() && !confirming_) take_guide_taps(guide_hits_, actions, pointers);
    // A tap on Touch Point's page lands on the screen, wherever it is.
    last_width_ = output_width;
    last_height_ = output_height;

    // The picker takes every press until put away -- the buttons here, a tap
    // the one way every tap is heard, below.
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
    // And Touch Point's page: the spot moved, and kept or not.
    if (placing_) {
        drag_spot(output_width, output_height);
        constexpr float kStep = 1.0f / 32.0f;
        for (const UiAction action : actions) {
            switch (action) {
                case UiAction::Up: move_spot(0.0f, -kStep); break;
                case UiAction::Down: move_spot(0.0f, kStep); break;
                case UiAction::Left: move_spot(-kStep, 0.0f); break;
                case UiAction::Right: move_spot(kStep, 0.0f); break;
                case UiAction::Confirm: finish_placing(result); break;
                case UiAction::Cancel: back_to_choice(PadAction::TouchPoint, output_width, output_height); break;
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
            confirming_ = false;
        } else if (answer >= 0) {
            answer_revert(answer, result);
        }
        for (const int tap : take_taps(pointers, output_width, output_height)) {
            handle_tap(tap, result, output_width, output_height);
        }
        return result;
    }

    const int last = static_cast<int>(kPadControls) - 1;
    for (const UiAction action : actions) {
        switch (action) {
            // No going round: the list stops at its ends.
            case UiAction::Up: focus_ = std::max(0, focus_ - 1); break;
            case UiAction::Down: focus_ = std::min(last, focus_ + 1); break;
            case UiAction::Confirm: open_choice(output_width, output_height); break;
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
        // Off the picker or the question: put away, nothing changed.
        choice_.close();
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
        if (index >= static_cast<int>(choice_.choices().size()) || choice_.heading(index)) return;
        choice_.close();
        choose(index, result);
        return;
    }
    if (choice_.is_open() || confirming_ || entry >= static_cast<int>(kPadControls)) return;
    // A first tap highlights, a second chooses.
    if (tap_chooses(focus_, entry)) {
        open_choice(output_width, output_height);
    } else {
        scroll_focus_into_view(compute_geometry(output_width, output_height));
    }
}

std::string ButtonMappingScreen::caption() const {
    const PadControl control = focused_control();
    const std::string name = pad_control_name(control);
    const PadAction action = mapping_.resolved(control, console_);
    DsButton button = DsButton::A;
    switch (action) {
        case PadAction::QuickMenu: return name + " opens the quick menu.";
        case PadAction::SwapScreens: return name + " swaps the screens.";
        case PadAction::BlowIntoMic: return "Holding " + name + " blows into the microphone.";
        case PadAction::TouchCursor: return name + " touches the screen at the Touch Cursor.";
        case PadAction::Nothing: return name + " is disabled in this game.";
        case PadAction::NextLayout: return name + " moves on to the next screen layout.";
        case PadAction::TouchPoint: return name + " touches the touch screen here while it's held.";
        case PadAction::Turbo: {
            const PadAction of = mapping_.turbo(control);
            if (of == PadAction::Standard) return name + " does nothing until Turbo has a button.";
            return name + " presses " + pad_action_name(of) + " over and over while it's held.";
        }
        default: break;
    }
    if (ds_button_for_pad_action(action, button)) {
        return name + " presses " + pad_action_name(action) + " on the " + console_name() + ".";
    }
    return std::string();
}

void ButtonMappingScreen::draw_console(QuadRenderer& quads, const PixelRect& box, PadControl control) const {
    const PadAction resolved = mapping_.resolved(control, console_);
    // Turbo picks out the button it presses.
    const PadAction action = resolved == PadAction::Turbo ? mapping_.turbo(control) : resolved;
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
    draw_page_header(quads, text, frame, "Button Mapping", game_title_);

    quads.set_clip(menu_list_clip(PixelRect{geometry.list.x, geometry.view_top, geometry.list.w,
                                            geometry.view_bottom - geometry.view_top},
                                  scale));
    const float pad = menu_row_pad(line);
    const float glyph_height = std::round(line * 1.7f);
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PixelRect& row = geometry.rows[index];
        const bool focused = static_cast<int>(index) == focus_;
        draw_menu_row(quads, row, focused, scale, index + 1 == kPadControls,
                      static_cast<int>(index) == focus_ + 1);
        const PadControl control = static_cast<PadControl>(index);
        const float centre = row.y + row.h * 0.5f;
        glyphs.draw(quads, glyph_for(control), row.x + pad,
                    std::round(centre - glyph_height * glyphs.button_centre()), glyph_height, theme.text);
        const float name_x = row.x + pad + glyphs.width_for(glyph_height) + line * 0.6f;
        const float baseline = page_row_text_y(row, scale);
        const std::string name = pad_control_name(control);
        text.draw(quads, name, name_x, baseline, scale, menu_row_text(focused));
        // What it does, in what room the name leaves.
        const float value_room =
            row.x + row.w - pad - (name_x + TextRenderer::measure_width(name, scale) + line * 1.2f);
        text.draw(quads, TextRenderer::elide_right(action_label(control), value_room, scale), row.x + row.w - pad,
                  baseline, scale, theme.text_dim, TextAlign::Right);
    }
    quads.clear_clip();
    const float view = geometry.view_bottom - geometry.view_top;
    if (geometry.content_height > view + 0.5f) {
        draw_scroll_bar(quads, geometry.list, scroll_ / geometry.content_height,
                        view / geometry.content_height, line, scale);
    }

    // The console, what the highlighted button presses picked out on it.
    draw_console(quads, geometry.diagram, focused_control());
    const float caption_scale = scale * 0.85f;
    std::vector<std::string> words = TextRenderer::wrap(caption(), geometry.diagram.w, caption_scale);
    if (words.size() > 2) {
        words[1] = TextRenderer::elide_right(words[1] + " " + words[2], geometry.diagram.w, caption_scale);
        words.resize(2);
    }
    for (std::size_t at = 0; at < words.size(); ++at) {
        text.draw(quads, words[at], geometry.diagram.x + geometry.diagram.w * 0.5f,
                  geometry.caption_y + TextRenderer::wrapped_line_step(caption_scale) * static_cast<float>(at),
                  caption_scale, theme.text_dim, TextAlign::Center);
    }

    std::vector<GuidePrompt> guide;
    if (!mapping_.is_standard(console_)) guide.push_back({ButtonGlyph::X, "Reset to Default"});
    if (can_toggle_turbo()) guide.push_back({ButtonGlyph::Y, "Toggle Turbo"});
    guide.push_back({ButtonGlyph::B, "Back", PromptRole::LeavesScreen});
    guide.push_back({ButtonGlyph::A, "Change", PromptRole::ActsOnSelection});
    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame, guide, &guide_hits_);

    if (choice_.is_open()) {
        choice_.set_row(geometry.rows[static_cast<std::size_t>(focus_)]);
        choice_.render(quads, text, output_width, output_height);
    }
    if (confirming_) {
        const PopupGeometry popup = confirm_popup(output_width, output_height);
        draw_popup(quads, text, output_width, output_height, popup, confirm_buttons(),
                   confirm_cursor_.focus(), confirm_cursor_.shown());
        draw_popup_text(quads, text, confirm_text(), popup.body.x, popup.body.y, popup.body.w,
                        output_height);
    }
}

void ButtonMappingScreen::render_placing(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs,
                                         float output_width, float output_height) {
    const UiTheme& theme = ui_theme();
    const PageFrame frame = page_frame(output_width, output_height);
    const float line = frame.line;
    const float scale = frame.scale;
    draw_page_background(quads, output_width, output_height, over_game_);
    draw_page_header(quads, text, frame,
                     std::string("Touch Point for ") + pad_control_name(focused_control()), game_title_);
    const PixelRect touch = touch_rect(output_width, output_height);
    if (touch_screen_ != nullptr && touch_screen_->valid()) {
        quads.draw_screen(touch, *touch_screen_);
    } else {
        quads.draw_solid(touch, Color{0.0f, 0.0f, 0.0f, 1.0f});
    }
    const float border = std::max(1.0f, std::round(scale * 0.8f));
    quads.draw_rounded_ring(touch, std::round(line * 0.15f), border, theme.text_dim, theme.text_dim);
    // Where it touches: a ring on the spot, and a cross through it to aim by.
    const float cx = std::round(touch.x + spot_.u * touch.w);
    const float cy = std::round(touch.y + spot_.v * touch.h);
    // White edged in black, to show on whatever the game has there.
    const float r = std::round(line * 1.1f);
    const float bar = std::max(2.0f, std::round(scale * 1.2f));
    const float edge = std::max(1.0f, std::round(scale * 0.6f));
    const Color white{1.0f, 1.0f, 1.0f, 1.0f};
    const Color shade{0.0f, 0.0f, 0.0f, 0.7f};
    Color fill = theme.accent;
    fill.a = 0.55f;
    quads.set_clip(touch);
    const PixelRect ring{cx - r, cy - r, r * 2.0f, r * 2.0f};
    quads.draw_solid(PixelRect{cx - bar * 0.5f - edge, cy - r * 1.8f - edge, bar + edge * 2.0f, r * 3.6f + edge * 2.0f},
                     shade);
    quads.draw_solid(PixelRect{cx - r * 1.8f - edge, cy - bar * 0.5f - edge, r * 3.6f + edge * 2.0f, bar + edge * 2.0f},
                     shade);
    quads.draw_rounded_rect(ring, r, fill);
    quads.draw_rounded_ring(PixelRect{ring.x - edge, ring.y - edge, ring.w + edge * 2.0f, ring.h + edge * 2.0f},
                            r + edge, bar + edge * 2.0f, shade, shade);
    quads.draw_rounded_ring(ring, r, bar, white, white);
    quads.draw_solid(PixelRect{cx - bar * 0.5f, cy - r * 1.8f, bar, r * 3.6f}, white);
    quads.draw_solid(PixelRect{cx - r * 1.8f, cy - bar * 0.5f, r * 3.6f, bar}, white);
    quads.clear_clip();
    text.draw(quads, "Choose a point for this button to tap.", frame.panel.x + frame.panel.w * 0.5f,
              std::round(touch.y + touch.h + line * 0.8f), scale * 0.9f, theme.text_dim, TextAlign::Center);
    // OK, ringed as the one thing A presses here.
    const PixelRect ok = placing_ok_rect(output_width, output_height);
    quads.draw_rounded_rect(ok, std::round(ok.h * 0.5f), Color{1.0f, 1.0f, 1.0f, 0.10f});
    draw_menu_cursor(quads, ok, std::round(ok.h * 0.5f), scale);
    text.draw(quads, "OK", ok.x + ok.w * 0.5f, std::round(ok.y + (ok.h - TextRenderer::line_height(scale)) * 0.5f),
              scale, theme.text, TextAlign::Center);

    guide_hits_.clear();
    draw_page_guide(quads, text, glyphs, frame,
                    {{ButtonGlyph::B, "Back", PromptRole::LeavesScreen},
                     {ButtonGlyph::A, "OK", PromptRole::ActsOnSelection}},
                    &guide_hits_);
}

void ButtonMappingScreen::show_mirrored(const net::ButtonMappingViewMessage& view, float output_width,
                                        float output_height) {
    if (!open_) open();
    last_width_ = output_width;
    last_height_ = output_height;
    focus_ = std::clamp(static_cast<int>(view.focus), 0, static_cast<int>(kPadControls) - 1);
    // The other device's cursor brings the list with it when it moves;
    // otherwise the list is where a finger here left it.
    if (focus_ != mirrored_focus_) {
        drag_.stop();
        scroll_focus_into_view(compute_geometry(output_width, output_height));
        mirrored_focus_ = focus_;
    }
    if (view.choice_focus == 0) {
        choice_.close();
    } else {
        if (!choice_.is_open()) open_choice(output_width, output_height);
        if (choice_.focus() != view.choice_focus - 1) choice_.set_focus(view.choice_focus - 1);
    }
    placing_ = view.placing;
    if (placing_) spot_ = TouchSpot{view.spot_u, view.spot_v};
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
    mix_sound_state(view.rest, open_ ? 1u : 0u);
    mix_sound_state(view.rest, choice_.is_open() ? 1u : 0u);
    mix_sound_state(view.rest, confirming_ ? 1u : 0u);
    mix_sound_state(view.rest, placing_ ? 1u : 0u);
    if (placing_) mix_sound_state(view.focus, static_cast<uint64_t>(spot_entry(spot_)));
    for (const PadAction action : mapping_.actions) mix_sound_state(view.rest, static_cast<uint64_t>(action));
    for (const PadAction action : mapping_.turbo_of) mix_sound_state(view.rest, static_cast<uint64_t>(action));
}

net::ButtonMappingViewMessage button_mapping_view_of(const ButtonMappingScreen& screen) {
    net::ButtonMappingViewMessage view;
    view.open = screen.is_open();
    if (!view.open) return view;
    view.focus = static_cast<uint8_t>(screen.focus());
    view.choice_focus = screen.choosing() ? static_cast<uint8_t>(screen.choice_focus() + 1) : 0;
    view.confirm_focus = screen.confirming() ? static_cast<uint8_t>(screen.confirm_focus() + 1) : 0;
    view.placing = screen.placing();
    if (view.placing) {
        view.spot_u = screen.spot().u;
        view.spot_v = screen.spot().v;
    }
    return view;
}

}  // namespace ds
