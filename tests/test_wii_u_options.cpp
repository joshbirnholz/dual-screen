// The Wii U quick menu's Options page -- Cemu's own settings -- driven the way
// a player drives it: presses in, the changes it asks Cemu for out, and where
// the cursor goes when Cemu's answer arrives. Nothing here draws, so none of it
// needs a GL context.

#include "test_support.hpp"

#include <string>
#include <vector>

#include "ui/wii_u_options_screen.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 800.0f;

std::vector<ExternalOption> cemu_options() {
    std::vector<ExternalOption> options(3);
    options[0] = {"upscale_filter", "Upscaling", {"Bilinear", "Bicubic", "Hermite", "Nearest neighbor"}, 1, false};
    options[1] = {"fullscreen_scaling", "Aspect ratio", {"Original", "Stretched"}, 0, false};
    options[2] = {"language", "Console language", {"English", "Japanese"}, -1, true};
    return options;
}

WiiUOptionsScreenResult press(WiiUOptionsScreen& screen, UiAction action) {
    return screen.update({action}, {}, kWidth, kHeight);
}

void test_values_step_both_ways_and_go_round() {
    WiiUOptionsScreen screen;
    screen.open();
    // Nothing to change until Cemu answers.
    CHECK(!press(screen, UiAction::Right).change);
    screen.set_options(cemu_options());

    WiiUOptionsScreenResult result = press(screen, UiAction::Right);
    CHECK(result.change && result.change->first == "upscale_filter" && result.change->second == 2);
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && result.change->second == 3);
    // Round from the last to the first, and back, as Settings' values go.
    result = press(screen, UiAction::Right);
    CHECK(result.change && result.change->second == 0);
    result = press(screen, UiAction::Left);
    CHECK(result.change && result.change->second == 3);
    CHECK(!screen.restart_pending());

    // Down to the last, and no further.
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 2);
    // None chosen yet counts from the first; this one waits for a reset.
    result = press(screen, UiAction::Right);
    CHECK(result.change && result.change->first == "language" && result.change->second == 1);
    CHECK_MSG(screen.restart_pending(), "an option that waits for a reset did not say so");
}

void test_the_cursor_stays_on_its_option_when_cemu_answers() {
    WiiUOptionsScreen screen;
    screen.open();
    screen.set_options(cemu_options());
    press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 1);
    std::vector<ExternalOption> reordered = cemu_options();
    std::swap(reordered[0], reordered[2]);
    screen.set_options(reordered);
    CHECK_EQ(screen.focus(), 1);
    std::swap(reordered[1], reordered[2]);
    screen.set_options(reordered);
    CHECK_EQ(screen.focus(), 2);
    // Gone: the nearest that is left.
    reordered.pop_back();
    screen.set_options(reordered);
    CHECK_EQ(screen.focus(), 1);
}

void test_back_and_the_menu_button_leave() {
    WiiUOptionsScreen screen;
    screen.open();
    screen.set_options(cemu_options());
    CHECK(press(screen, UiAction::Cancel).close_requested);
    CHECK(!screen.is_open());
    screen.open();
    CHECK(press(screen, UiAction::ToggleMenu).close_requested);
    screen.open();
    screen.set_options(cemu_options());
    const WiiUOptionsScreenResult result = press(screen, UiAction::MenuQuit);
    CHECK(!result.close_requested && !result.change);
}

}  // namespace

int main() {
    test_values_step_both_ways_and_go_round();
    test_the_cursor_stays_on_its_option_when_cemu_answers();
    test_back_and_the_menu_button_leave();
    return test::finish("wii-u-options");
}
