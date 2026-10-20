// A DS or 3DS game's Button Mapping page, driven the way a player drives it --
// presses and taps in, the mapping it asks the game to keep out: the cursor
// down the list, a button given another job from the picker beside it, the
// standard job kept as Standard, Revert to Default after asking, and the
// page drawn on the other device and worked from there. Nothing here draws,
// so none of it needs a GL context.

#include "test_support.hpp"

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

void move_to(ButtonMappingScreen& screen, PadControl control) {
    while (screen.focus() > static_cast<int>(control)) press(screen, UiAction::Up);
    while (screen.focus() < static_cast<int>(control)) press(screen, UiAction::Down);
}

// Where `action` is in the picker as it is for the focused row: past the
// headings, which take a line each.
int choice_index(const ButtonMappingScreen& screen, PadAction action) {
    const PadConsole console = ds_console();
    int index = 0;
    bool first = true;
    PadActionGroup group = PadActionGroup::Console;
    for (const PadAction choice : pad_action_choices(console)) {
        if (first || pad_action_group(choice) != group) {
            ++index;
            group = pad_action_group(choice);
            first = false;
        }
        if (choice == action) return index;
        ++index;
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
    // The cursor starts on what it does now: R, as a DS has no ZR.
    CHECK_EQ(screen.choice_focus(), choice_index(screen, PadAction::R));

    const ButtonMappingScreenResult changed =
        tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::QuickMenu));
    CHECK(!screen.choosing());
    CHECK(changed.changed.has_value());
    if (changed.changed) {
        CHECK((*changed.changed)[PadControl::Zr] == PadAction::QuickMenu);
        CHECK(!changed.changed->is_standard(ds_console()));
    }
    // A heading is not a choice.
    press(screen, UiAction::Confirm);
    CHECK(!tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase).changed.has_value());
    CHECK(screen.choosing());
    // Back to R, its standard job, which is kept as Standard.
    const ButtonMappingScreenResult back =
        tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::R));
    CHECK(back.changed.has_value());
    if (back.changed) {
        CHECK((*back.changed)[PadControl::Zr] == PadAction::Standard);
        CHECK(back.changed->is_standard(ds_console()));
    }
    // And choosing what it already does changes nothing.
    press(screen, UiAction::Confirm);
    CHECK(!tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + choice_index(screen, PadAction::R))
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
    // Nothing to put back: X does nothing.
    ButtonMappingScreen standard = open_screen();
    press(standard, UiAction::MenuQuit);
    CHECK(!standard.confirming());

    ButtonMapping mapping;
    mapping[PadControl::A] = PadAction::B;
    mapping[PadControl::B] = PadAction::A;
    ButtonMappingScreen screen = open_screen(mapping);
    press(screen, UiAction::MenuQuit);
    CHECK_MSG(screen.confirming(), "X did not ask before putting every button back");
    // The cursor on Revert; B is Cancel, and changes nothing.
    CHECK_EQ(screen.confirm_focus(), 1);
    CHECK(!press(screen, UiAction::Cancel).changed.has_value());
    CHECK(!screen.confirming() && screen.is_open());
    press(screen, UiAction::MenuQuit);
    const ButtonMappingScreenResult reverted = press(screen, UiAction::Confirm);
    CHECK(!screen.confirming());
    CHECK(reverted.changed.has_value() && reverted.changed->is_standard(ds_console()));
    // And now there is nothing to put back.
    press(screen, UiAction::MenuQuit);
    CHECK(!screen.confirming());
}

void test_a_first_tap_highlights_and_a_second_chooses() {
    ButtonMappingScreen screen = open_screen();
    tap_entry(screen, static_cast<int>(PadControl::Plus));
    CHECK_EQ(screen.focus(), static_cast<int>(PadControl::Plus));
    CHECK(!screen.choosing());
    tap_entry(screen, static_cast<int>(PadControl::Plus));
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

    tap_on_mirror(static_cast<int>(PadControl::Y));
    CHECK_EQ(host.focus(), static_cast<int>(PadControl::Y));
    tap_on_mirror(static_cast<int>(PadControl::Y));
    CHECK(host.choosing());
    show();
    CHECK(mirror.choosing());
    CHECK_EQ(mirror.choice_focus(), host.choice_focus());
    const ButtonMappingScreenResult chosen =
        tap_on_mirror(ButtonMappingScreen::kChoiceEntryBase + choice_index(host, PadAction::X));
    CHECK(chosen.changed.has_value() && (*chosen.changed)[PadControl::Y] == PadAction::X);
    show();
    CHECK(!mirror.choosing());

    // Revert to Default's question, there too.
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

    // On a 3DS too the triggers are a second L and R as standard, as they
    // always were; ZR is there to be chosen, just past them.
    ButtonMappingScreen screen = open_screen(ButtonMapping(), SystemId::ThreeDs, RightStickMode::TouchCursor);
    move_to(screen, PadControl::Zr);
    press(screen, UiAction::Confirm);
    CHECK(screen.choosing());
    CHECK_EQ(screen.choice_focus(), 6);
    const ButtonMappingScreenResult zr = tap_entry(screen, ButtonMappingScreen::kChoiceEntryBase + 8);
    CHECK(zr.changed.has_value() && (*zr.changed)[PadControl::Zr] == PadAction::Zr);
}

}  // namespace

int main() {
    test_the_cursor_stops_at_the_ends();
    test_a_button_is_given_another_job_and_put_back();
    test_b_puts_the_picker_away_then_leaves();
    test_revert_asks_first();
    test_a_first_tap_highlights_and_a_second_chooses();
    test_the_other_device_shows_and_works_the_page();
    test_the_picker_offers_what_the_console_has();
    return test::finish("button-mapping-screen");
}
