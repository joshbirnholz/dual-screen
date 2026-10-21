// A DS or 3DS game's Change Controls page, driven the way a player drives it --
// presses and taps in, the mapping it asks the game to keep out: the cursor
// down the list, a button given another job from the picker beside it, the
// standard job kept as Standard, Reset to Default after asking, and the
// page drawn on the other device and worked from there. Nothing here draws,
// so none of it needs a GL context.

#include "test_support.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "ui/button_mapping_screen.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 800.0f;

PadConsole ds_console() { return pad_console_for(SystemId::Ds, RightStickMode::CStick); }

ButtonMappingScreen open_screen(const ButtonMapping& mapping = ButtonMapping(), SystemId system = SystemId::Ds,
                                RightStickMode right_stick = RightStickMode::CStick) {
    ButtonMappingScreen screen;
    screen.set_mapping(mapping, system, pad_console_for(system, right_stick), "Some Game");
    screen.open();
    return screen;
}

ButtonMappingScreenResult press(ButtonMappingScreen& screen, UiAction action) {
    return screen.update({action}, {}, kWidth, kHeight);
}

ButtonMappingScreenResult tap_entry(ButtonMappingScreen& screen, int entry) {
    screen.press_entry(entry);
    return screen.update({}, {}, kWidth, kHeight);
}

// The row `control` is on.
int row_of(PadControl control) {
    for (std::size_t row = 0; row < kPadControls; ++row) {
        if (kPadControlRows[row] == control) return static_cast<int>(row);
    }
    return -1;
}

void move_to(ButtonMappingScreen& screen, PadControl control) {
    while (screen.focus() > row_of(control)) press(screen, UiAction::Up);
    while (screen.focus() < row_of(control)) press(screen, UiAction::Down);
}

// Where `action` is in the picker: one list, no headings.
int choice_index(const ButtonMappingScreen& screen, PadAction action) {
    const std::vector<PadAction> choices = pad_action_choices(ds_console());
    for (std::size_t index = 0; index < choices.size(); ++index) {
        if (choices[index] == action) return static_cast<int>(index);
    }
    (void)screen;
    return -1;
}

void test_the_cursor_stops_at_the_ends() {
    ButtonMappingScreen screen = open_screen();
    CHECK_EQ(screen.focus(), 0);
    press(screen, UiAction::Up);
    CHECK_EQ(screen.focus(), 0);
    for (int step = 0; step < 30; ++step) press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), static_cast<int>(kPadControls) - 1);
    // Left and right are nothing here.
    press(screen, UiAction::Left);
    press(screen, UiAction::Right);
    CHECK_EQ(screen.focus(), static_cast<int>(kPadControls) - 1);
    CHECK(!screen.choosing());
}

void test_a_button_is_given_another_job_and_put_back() {
    ButtonMappingScreen screen = open_screen();
    move_to(screen, PadControl::Zr);
    press(screen, UiAction::Confirm);
    CHECK_MSG(screen.choosing(), "A did not bring up the picker");
    // The cursor starts on what it does now: Disabled, as a DS has no ZR.
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::Nothing));

    const ButtonMappingScreenResult changed =
        tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::QuickMenu));
    CHECK(!screen.choosing());
    CHECK(changed.changed.has_value());
    if (changed.changed) {
        CHECK((*changed.changed)[PadControl::Zr] == PadAction::QuickMenu);
        CHECK(!changed.changed->is_standard(ds_console()));
    }
    // Back to Disabled, its standard job, which is kept as Standard.
    press(screen, UiAction::Confirm);
    const ButtonMappingScreenResult back =
        tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::Nothing));
    CHECK(back.changed.has_value());
    if (back.changed) {
        CHECK((*back.changed)[PadControl::Zr] == PadAction::Standard);
        CHECK(back.changed->is_standard(ds_console()));
    }
    // And choosing what it already does changes nothing.
    press(screen, UiAction::Confirm);
    CHECK(!tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::Nothing))
               .changed.has_value());
}

void test_b_puts_the_picker_away_then_leaves() {
    ButtonMappingScreen screen = open_screen();
    press(screen, UiAction::Confirm);
    CHECK(screen.choosing());
    const ButtonMappingScreenResult away = press(screen, UiAction::Cancel);
    CHECK(!screen.choosing());
    CHECK(!away.close_requested && !away.changed.has_value());
    CHECK(screen.is_open());
    const ButtonMappingScreenResult left = press(screen, UiAction::Cancel);
    CHECK(left.close_requested && !left.close_everything);
    CHECK(!screen.is_open());

    // The menu button out of everything, picker up or not.
    ButtonMappingScreen again = open_screen();
    press(again, UiAction::Confirm);
    const ButtonMappingScreenResult out = press(again, UiAction::ToggleMenu);
    CHECK(out.close_requested && out.close_everything);
    CHECK(!again.is_open());
}

void test_revert_asks_first() {
    // X asks even with nothing to put back, as its prompt is always there.
    ButtonMappingScreen standard = open_screen();
    press(standard, UiAction::MenuQuit);
    CHECK(standard.confirming());

    ButtonMapping mapping;
    mapping[PadControl::A] = PadAction::B;
    mapping[PadControl::B] = PadAction::A;
    ButtonMappingScreen screen = open_screen(mapping);
    press(screen, UiAction::MenuQuit);
    CHECK_MSG(screen.confirming(), "X did not ask before putting every button back");
    // The cursor on Reset; B is Cancel, and changes nothing.
    CHECK_EQ(screen.confirm_focus(), 1);
    CHECK(!press(screen, UiAction::Cancel).changed.has_value());
    CHECK(!screen.confirming() && screen.is_open());
    press(screen, UiAction::MenuQuit);
    const ButtonMappingScreenResult reverted = press(screen, UiAction::Confirm);
    CHECK(!screen.confirming());
    CHECK(reverted.changed.has_value() && reverted.changed->is_standard(ds_console()));
}

void test_a_tap_chooses() {
    ButtonMappingScreen screen = open_screen();
    tap_entry(screen, row_of(PadControl::Plus));
    CHECK_EQ(screen.focus(), row_of(PadControl::Plus));
    CHECK(screen.choosing());
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::Start));
    // Off the picker puts it away.
    tap_entry(screen, MenuPage::kTapOffPopup);
    CHECK(!screen.choosing());
}

bool point_on(const ButtonMappingScreen& page, int entry, float width, float height, float& x, float& y) {
    for (y = 2.0f; y < height; y += 3.0f) {
        for (x = 2.0f; x < width; x += 6.0f) {
            if (page.entry_at(x, y, width, height) == entry) return true;
        }
    }
    return false;
}

PointerEvent tap_at(float x, float y) {
    PointerEvent event;
    event.x = x;
    event.y = y;
    event.press_x = x;
    event.press_y = y;
    event.released = true;
    event.normalized = false;
    return event;
}

// The page as the other device draws it, at its own size: the host's cursor,
// picker and question, and taps there answered by the host's page.
void test_the_other_device_shows_and_works_the_page() {
    constexpr float kOtherWidth = 1920.0f;
    constexpr float kOtherHeight = 1080.0f;
    ButtonMappingScreen host = open_screen();
    ButtonMappingScreen mirror;
    const auto show = [&] {
        mirror.set_mapping(ButtonMapping(), SystemId::Ds, ds_console(), "Some Game");
        mirror.show_mirrored(button_mapping_view_of(host), kOtherWidth, kOtherHeight);
    };
    const auto tap_on_mirror = [&](int entry) {
        show();
        float x = 0.0f;
        float y = 0.0f;
        CHECK_MSG(point_on(mirror, entry, kOtherWidth, kOtherHeight, x, y), "no such entry on the page");
        const std::vector<int> taps = MenuPage::taps_on(mirror, {tap_at(x, y)}, kOtherWidth, kOtherHeight);
        CHECK_EQ(taps.size(), 1u);
        if (!taps.empty()) host.press_entry(taps.front());
        return host.update({}, {}, kWidth, kHeight);
    };

    show();
    CHECK(mirror.is_open());
    press(host, UiAction::Down);
    show();
    CHECK_EQ(mirror.focus(), 1);

    tap_on_mirror(row_of(PadControl::Y));
    CHECK_EQ(host.focus(), row_of(PadControl::Y));
    CHECK(host.choosing());
    show();
    CHECK(mirror.choosing());
    CHECK_EQ(mirror.choice_focus(), host.choice_focus());
    const ButtonMappingScreenResult chosen =
        tap_on_mirror(ButtonMappingScreen::kChoiceEntryBase + choice_index(host, PadAction::X));
    CHECK(chosen.changed.has_value() && (*chosen.changed)[PadControl::Y] == PadAction::X);
    show();
    CHECK(!mirror.choosing());

    // Reset to Default's question, there too.
    press(host, UiAction::MenuQuit);
    show();
    CHECK(mirror.confirming());
    CHECK_EQ(mirror.confirm_focus(), 1);
    const ButtonMappingScreenResult reverted = tap_on_mirror(ButtonMappingScreen::kConfirmEntryBase + 1);
    CHECK(reverted.changed.has_value() && reverted.changed->is_standard(ds_console()));
    show();
    CHECK(!mirror.confirming());

    press(host, UiAction::Cancel);
    CHECK(!button_mapping_view_of(host).open);
}

// A 3DS offers its ZL and ZR, a DS its microphone; the Touch Cursor's tip
// only while the right stick is the cursor.
void test_the_picker_offers_what_the_console_has() {
    const std::vector<PadAction> ds = pad_action_choices(ds_console());
    const auto has = [](const std::vector<PadAction>& choices, PadAction action) {
        for (const PadAction choice : choices) {
            if (choice == action) return true;
        }
        return false;
    };
    CHECK(!has(ds, PadAction::Zl) && has(ds, PadAction::BlowIntoMic) && !has(ds, PadAction::TouchCursor));
    const std::vector<PadAction> three_ds =
        pad_action_choices(pad_console_for(SystemId::ThreeDs, RightStickMode::TouchCursor));
    CHECK(has(three_ds, PadAction::Zl) && has(three_ds, PadAction::Zr));
    CHECK(!has(three_ds, PadAction::BlowIntoMic) && has(three_ds, PadAction::TouchCursor));
    CHECK(three_ds.back() == PadAction::Nothing);

    // On a 3DS the triggers are a New 3DS's ZL and ZR as standard, just
    // past L and R in the list.
    ButtonMappingScreen screen = open_screen(ButtonMapping(), SystemId::ThreeDs, RightStickMode::TouchCursor);
    move_to(screen, PadControl::Zr);
    press(screen, UiAction::Confirm);
    CHECK(screen.choosing());
    CHECK_EQ(screen.choice_focus(), 7);
    const ButtonMappingScreenResult r = tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + 5);
    CHECK(r.changed.has_value() && (*r.changed)[PadControl::Zr] == PadAction::R);
}

// Y turns turbo on for a button that presses one of the console's, and off
// again; its row says so. A button on turbo keeps it for another of the
// console's buttons, and Turbo is no choice of its own.
void test_y_toggles_turbo() {
    ButtonMappingScreen screen = open_screen();
    move_to(screen, PadControl::X);
    const ButtonMappingScreenResult on = press(screen, UiAction::MenuReset);
    CHECK(on.changed.has_value());
    if (on.changed) {
        CHECK((*on.changed)[PadControl::X] == PadAction::Turbo);
        CHECK(on.changed->turbo(PadControl::X) == PadAction::X);
    }
    CHECK(screen.action_label(PadControl::X) == "X Button (turbo)");

    // The list: no Turbo, and the cursor on the button it presses.
    press(screen, UiAction::Confirm);
    CHECK(screen.choosing());
    CHECK_EQ(choice_index(screen, PadAction::Turbo), -1);
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::X));
    const ButtonMappingScreenResult other =
        tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::B));
    CHECK(other.changed.has_value() && (*other.changed)[PadControl::X] == PadAction::Turbo &&
          other.changed->turbo(PadControl::X) == PadAction::B);
    CHECK(screen.action_label(PadControl::X) == "B Button (turbo)");

    // Off: back to pressing B once.
    const ButtonMappingScreenResult off = press(screen, UiAction::MenuReset);
    CHECK(off.changed.has_value() && (*off.changed)[PadControl::X] == PadAction::B &&
          off.changed->turbo(PadControl::X) == PadAction::Standard);

    // Nothing for a button that presses none of the console's.
    press(screen, UiAction::Confirm);
    tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::QuickMenu));
    CHECK(!press(screen, UiAction::MenuReset).changed.has_value());
}

// A Touch Point goes on turbo too, touching its spot over and over, and a
// spot placed again while on turbo stays on it.
void test_y_toggles_turbo_for_a_touch_point() {
    ButtonMapping mapping;
    mapping[PadControl::L] = PadAction::TouchPoint;
    mapping.spot(PadControl::L) = TouchSpot{0.25f, 0.75f};
    ButtonMappingScreen screen = open_screen(mapping);
    move_to(screen, PadControl::L);
    const ButtonMappingScreenResult on = press(screen, UiAction::MenuReset);
    CHECK(on.changed.has_value());
    if (on.changed) {
        CHECK((*on.changed)[PadControl::L] == PadAction::Turbo);
        CHECK(on.changed->turbo(PadControl::L) == PadAction::TouchPoint);
    }
    CHECK(screen.action_label(PadControl::L) == "Touch Point (turbo)");

    press(screen, UiAction::Confirm);
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::TouchPoint));
    press(screen, UiAction::Confirm);
    CHECK(screen.placing());
    press(screen, UiAction::Right);
    const ButtonMappingScreenResult moved = press(screen, UiAction::Confirm);
    CHECK(moved.changed.has_value() && (*moved.changed)[PadControl::L] == PadAction::Turbo &&
          moved.changed->turbo(PadControl::L) == PadAction::TouchPoint);

    const ButtonMappingScreenResult off = press(screen, UiAction::MenuReset);
    CHECK(off.changed.has_value() && (*off.changed)[PadControl::L] == PadAction::TouchPoint &&
          off.changed->turbo(PadControl::L) == PadAction::Standard);
}

// Each of the d-pad's directions is a row of its own, pressing its own
// direction as standard.
void test_the_d_pad_is_mapped_a_direction_at_a_time() {
    ButtonMappingScreen screen = open_screen();
    move_to(screen, PadControl::Down);
    CHECK_EQ(screen.focus(), 5);
    CHECK(screen.action_label(PadControl::Down) == "Down Button");
    press(screen, UiAction::Confirm);
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::Down));
    const ButtonMappingScreenResult changed =
        tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::A));
    CHECK(changed.changed.has_value() && (*changed.changed)[PadControl::Down] == PadAction::A);
    CHECK(screen.action_label(PadControl::Down) == "A Button");
}

// Touch Point opens a page of its own: the spot moved by the pad or put down
// by a tap, A keeping it and B going back to the list, nothing changed.
void test_a_touch_point_is_placed() {
    ButtonMappingScreen screen = open_screen();
    move_to(screen, PadControl::L);
    press(screen, UiAction::Confirm);
    tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::TouchPoint));
    CHECK_MSG(screen.placing(), "Touch Point did not open its page");
    CHECK(!screen.choosing());
    press(screen, UiAction::Right);
    press(screen, UiAction::Right);
    press(screen, UiAction::Up);
    CHECK(std::fabs(screen.spot().u - (0.5f + 2.0f / 32.0f)) < 0.0001f);
    CHECK(std::fabs(screen.spot().v - (0.5f - 1.0f / 32.0f)) < 0.0001f);
    // B: back to the list, on Touch Point, and nothing kept.
    CHECK(!press(screen, UiAction::Cancel).changed.has_value());
    CHECK(!screen.placing() && screen.choosing());
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::TouchPoint));
    // Again, from where it was kept -- the middle -- and put down by a tap.
    press(screen, UiAction::Confirm);
    CHECK(screen.placing());
    CHECK_EQ(screen.spot().u, 0.5f);
    // A finger that slid a little between going down and coming up still
    // places it, where it came up.
    {
        float x = 0.0f;
        float y = 0.0f;
        int entry = -1;
        for (y = 2.0f; y < kHeight && entry < ButtonMappingScreen::kSpotEntryBase; y += 3.0f) {
            entry = screen.entry_at(kWidth * 0.5f, y, kWidth, kHeight);
        }
        x = kWidth * 0.5f;
        PointerEvent slid = tap_at(x + 6.0f, y + 40.0f);
        slid.press_x = x;
        slid.press_y = y + 30.0f;
        screen.update({}, {slid}, kWidth, kHeight);
        CHECK(screen.placing());
        CHECK_MSG(screen.spot().v > 0.02f && screen.spot().v < 0.3f, "a tap that slid did not place the spot");
    }
    tap_entry(screen, ButtonMappingScreen::spot_entry(TouchSpot{0.25f, 0.75f}));
    CHECK(std::fabs(screen.spot().u - 0.25f) < 0.001f && std::fabs(screen.spot().v - 0.75f) < 0.001f);
    const ButtonMappingScreenResult kept = press(screen, UiAction::Confirm);
    CHECK(!screen.placing() && !screen.choosing());
    CHECK(kept.changed.has_value());
    if (kept.changed) {
        CHECK((*kept.changed)[PadControl::L] == PadAction::TouchPoint);
        CHECK(std::fabs(kept.changed->spot(PadControl::L).u - 0.25f) < 0.001f);
    }
    // The menu button out of everything from there too.
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Confirm);
    CHECK(screen.placing());
    const ButtonMappingScreenResult out = press(screen, UiAction::ToggleMenu);
    CHECK(out.close_everything && !screen.is_open());
}

// The other device draws Touch Point's page too, and a tap on its screen
// puts the spot down on the host's.
// The spot follows a finger dragged over the touch screen as it goes, and
// stays where it lifts; the OK under the screen keeps it, as A does.
void test_a_touch_point_is_dragged_and_kept_with_ok() {
    ButtonMappingScreen screen = open_screen();
    move_to(screen, PadControl::L);
    press(screen, UiAction::Confirm);
    tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::TouchPoint));
    CHECK(screen.placing());
    // Where the touch screen and the OK are, found by asking.
    float left = kWidth, top = kHeight, right = 0.0f, bottom = 0.0f;
    bool ok_found = false;
    float ok_x = 0.0f, ok_y = 0.0f;
    for (float y = 0.0f; y < kHeight; y += 4.0f) {
        for (float x = 0.0f; x < kWidth; x += 4.0f) {
            const int entry = screen.entry_at(x, y, kWidth, kHeight);
            if (entry >= ButtonMappingScreen::kSpotEntryBase) {
                left = std::min(left, x);
                top = std::min(top, y);
                right = std::max(right, x);
                bottom = std::max(bottom, y);
            } else if (entry == ButtonMappingScreen::kPlacingOkEntry && !ok_found) {
                ok_found = true;
                ok_x = x;
                ok_y = y;
            }
        }
    }
    CHECK_MSG(right > left && bottom > top, "no touch screen to place on");
    CHECK_MSG(ok_found && ok_y > bottom, "no OK under the touch screen");

    // Down a quarter of the way across, then moved on, still down: the spot
    // is where the finger is now.
    const float x0 = left + (right - left) * 0.25f;
    const float y0 = top + (bottom - top) * 0.25f;
    const float x1 = left + (right - left) * 0.75f;
    const float y1 = top + (bottom - top) * 0.5f;
    set_live_pointer(true, x0, y0);
    set_live_pointer(true, x1, y1);
    screen.update({}, {}, kWidth, kHeight);
    CHECK(std::fabs(screen.spot().u - 0.75f) < 0.02f && std::fabs(screen.spot().v - 0.5f) < 0.02f);
    set_live_pointer(false, x1, y1);

    // OK keeps it.
    const ButtonMappingScreenResult kept = tap_entry(screen, ButtonMappingScreen::kPlacingOkEntry);
    CHECK(!screen.placing());
    CHECK(kept.changed.has_value() && (*kept.changed)[PadControl::L] == PadAction::TouchPoint);
    if (kept.changed) CHECK(std::fabs(kept.changed->spot(PadControl::L).u - 0.75f) < 0.02f);
    (void)ok_x;
}

void test_the_other_device_places_a_touch_point() {
    constexpr float kOtherWidth = 800.0f;
    constexpr float kOtherHeight = 480.0f;
    ButtonMappingScreen host = open_screen();
    press(host, UiAction::Confirm);
    tap_entry(host, ButtonMappingScreen::kChoiceEntryBase + choice_index(host, PadAction::TouchPoint));
    CHECK(host.placing());
    ButtonMappingScreen mirror;
    mirror.set_mapping(ButtonMapping(), SystemId::Ds, ds_console(), "Some Game");
    mirror.show_mirrored(button_mapping_view_of(host), kOtherWidth, kOtherHeight);
    CHECK(mirror.placing());
    // Somewhere on its touch screen, down the middle.
    float y = 0.0f;
    int entry = -1;
    for (y = 2.0f; y < kOtherHeight && entry < ButtonMappingScreen::kSpotEntryBase; y += 3.0f) {
        entry = mirror.entry_at(kOtherWidth * 0.5f, y, kOtherWidth, kOtherHeight);
    }
    CHECK_MSG(entry >= ButtonMappingScreen::kSpotEntryBase, "no touch screen on the other device's page");
    const std::vector<int> taps =
        MenuPage::taps_on(mirror, {tap_at(kOtherWidth * 0.5f, y)}, kOtherWidth, kOtherHeight);
    CHECK_EQ(taps.size(), 1u);
    if (!taps.empty()) host.press_entry(taps.front());
    host.update({}, {}, kWidth, kHeight);
    CHECK(std::fabs(host.spot().u - 0.5f) < 0.01f);
    CHECK(host.spot().v < 0.1f);
    mirror.show_mirrored(button_mapping_view_of(host), kOtherWidth, kOtherHeight);
    CHECK(std::fabs(mirror.spot().v - host.spot().v) < 0.001f);
    press(host, UiAction::Cancel);
    mirror.show_mirrored(button_mapping_view_of(host), kOtherWidth, kOtherHeight);
    CHECK(!mirror.placing() && mirror.choosing());
}

}  // namespace

int main() {
    test_the_cursor_stops_at_the_ends();
    test_a_button_is_given_another_job_and_put_back();
    test_b_puts_the_picker_away_then_leaves();
    test_revert_asks_first();
    test_a_tap_chooses();
    test_the_other_device_shows_and_works_the_page();
    test_the_picker_offers_what_the_console_has();
    test_y_toggles_turbo();
    test_y_toggles_turbo_for_a_touch_point();
    test_the_d_pad_is_mapped_a_direction_at_a_time();
    test_a_touch_point_is_placed();
    test_a_touch_point_is_dragged_and_kept_with_ok();
    test_the_other_device_places_a_touch_point();
    return test::finish("button-mapping-screen");
}
