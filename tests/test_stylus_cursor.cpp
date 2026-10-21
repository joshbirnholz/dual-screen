// The stylus on the right stick: the stick sets how fast the cursor goes, it
// stays on the screen, R3 is its tip, and it shows only while it is in use.

#include "input/controller_mapping.hpp"
#include "input/stylus_cursor.hpp"
#include "test_support.hpp"

using namespace ds;

namespace {

constexpr float kDeadzone = 0.2f;
constexpr float kFrame = 1.0f / 60.0f;

void test_it_starts_in_the_middle_and_hidden() {
    StylusCursor cursor;
    CHECK_NEAR(cursor.opacity(), 0.0f);
    cursor.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    CHECK_EQ(cursor.x(), 128);
    CHECK_EQ(cursor.y(), 96);
    CHECK_NEAR(cursor.opacity(), 0.0f);
    CHECK(!cursor.pressed());
}

void test_a_push_moves_it_and_a_small_one_barely() {
    StylusCursor cursor;
    cursor.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    // Inside the deadzone: nothing.
    cursor.update(5000, 0, false, kDeadzone, kFrame, 256, 192);
    CHECK_EQ(cursor.x(), 128);

    // Full right for a tenth of a second: about a sixth of the way across.
    for (int frame = 0; frame < 6; ++frame) cursor.update(32767, 0, false, kDeadzone, kFrame, 256, 192);
    CHECK_MSG(cursor.x() >= 162 && cursor.x() <= 172, "a full push moved to " + std::to_string(cursor.x()));
    CHECK_EQ(cursor.y(), 96);
    CHECK_NEAR(cursor.opacity(), 1.0f);

    // A little past the deadzone for the same time: a pixel or two.
    StylusCursor creeping;
    creeping.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    for (int frame = 0; frame < 6; ++frame) creeping.update(0, -9000, false, kDeadzone, kFrame, 256, 192);
    CHECK(creeping.y() < 96 && creeping.y() >= 93);
}

void test_it_stays_on_the_screen() {
    StylusCursor cursor;
    for (int frame = 0; frame < 300; ++frame) cursor.update(-32767, 32767, false, kDeadzone, kFrame, 256, 192);
    CHECK_EQ(cursor.x(), 0);
    CHECK_EQ(cursor.y(), 191);
}

void test_r3_touches_and_it_fades_when_left_alone() {
    StylusCursor cursor;
    cursor.update(0, 0, true, kDeadzone, kFrame, 256, 192);
    CHECK(cursor.pressed());
    CHECK_NEAR(cursor.opacity(), 1.0f);
    cursor.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    CHECK(!cursor.pressed());
    // Still there a moment later, gone a few seconds on.
    for (int frame = 0; frame < 60; ++frame) cursor.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    CHECK_NEAR(cursor.opacity(), 1.0f);
    for (int frame = 0; frame < 180; ++frame) cursor.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    CHECK_NEAR(cursor.opacity(), 0.0f);

    cursor.update(0, 0, true, kDeadzone, kFrame, 256, 192);
    cursor.hide();
    CHECK(!cursor.pressed());
    CHECK_NEAR(cursor.opacity(), 0.0f);
}

void test_a_3ds_screen_is_its_own_size() {
    StylusCursor cursor;
    cursor.update(0, 0, false, kDeadzone, kFrame, 256, 192);
    cursor.update(0, 0, false, kDeadzone, kFrame, 320, 240);
    CHECK_EQ(cursor.x(), 160);
    CHECK_EQ(cursor.y(), 120);
    CHECK_NEAR(cursor.u(), 160.5f / 320.0f);
}

// What the right stick can be: a DS has no stick of its own, so the stylus,
// the Touch Analog Stick or nothing; a 3DS everything, its C-Stick first.
void test_each_console_offers_what_it_can() {
    const std::vector<RightStickMode> ds = right_stick_modes(false);
    CHECK_EQ(ds.size(), 3u);
    CHECK(ds[1] == RightStickMode::TouchAnalog);
    CHECK(ds.front() == RightStickMode::TouchCursor);
    CHECK(right_stick_mode_for(RightStickMode::CStick, false) == RightStickMode::TouchCursor);
    CHECK(right_stick_mode_for(RightStickMode::Off, false) == RightStickMode::Off);
    CHECK(next_right_stick_mode(RightStickMode::Off, false) == RightStickMode::TouchCursor);

    const std::vector<RightStickMode> three_ds = right_stick_modes(true);
    CHECK_EQ(three_ds.size(), 6u);
    CHECK(three_ds.front() == RightStickMode::CStick);
    CHECK(right_stick_mode_for(RightStickMode::TouchCursor, true) == RightStickMode::TouchCursor);
    CHECK(next_right_stick_mode(RightStickMode::DPad, true) == RightStickMode::TouchCursor);
    CHECK(next_right_stick_mode(RightStickMode::Off, true) == RightStickMode::CStick);

    CHECK(right_stick_mode_from_key(right_stick_mode_key(RightStickMode::TouchCursor)) ==
          RightStickMode::TouchCursor);
    CHECK_STR_EQ(to_string(RightStickMode::TouchCursor), "Touch Cursor");
}

}  // namespace

int main() {
    test_it_starts_in_the_middle_and_hidden();
    test_a_push_moves_it_and_a_small_one_barely();
    test_it_stays_on_the_screen();
    test_r3_touches_and_it_fades_when_left_alone();
    test_a_3ds_screen_is_its_own_size();
    test_each_console_offers_what_it_can();
    return test::finish("stylus-cursor");
}
