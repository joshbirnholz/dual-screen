// Names too long for their rows: a row's room shared between a setting and
// its value as each needs, and the highlighted one sliding along to show the
// rest, still at each end for a moment.

#include "test_support.hpp"
#include "ui/fitted_text.hpp"

using namespace ds;

namespace {

void test_room_is_shared_by_need() {
    float label = 0.0f;
    float value = 0.0f;
    // Both fit: each has what it needs.
    split_row_room(500.0f, 200.0f, 100.0f, label, value);
    CHECK_NEAR(label, 200.0f);
    CHECK_NEAR(value, 100.0f);
    // A short value keeps all of it; the long name has the rest.
    split_row_room(500.0f, 600.0f, 100.0f, label, value);
    CHECK_NEAR(value, 100.0f);
    CHECK_NEAR(label, 400.0f);
    // A short name keeps all of it; the long value has the rest.
    split_row_room(500.0f, 120.0f, 900.0f, label, value);
    CHECK_NEAR(label, 120.0f);
    CHECK_NEAR(value, 380.0f);
    // Both long: half each.
    split_row_room(500.0f, 700.0f, 900.0f, label, value);
    CHECK_NEAR(label, 250.0f);
    CHECK_NEAR(value, 250.0f);
}

void test_it_holds_slides_and_holds() {
    const float speed = 100.0f;
    // Nothing to slide when it fits.
    CHECK_NEAR(fitted_text_offset(0.0f, 5.0f, speed), 0.0f);
    // Still at first...
    CHECK_NEAR(fitted_text_offset(200.0f, 0.5f, speed), 0.0f);
    // ...then moving at its pace...
    CHECK_NEAR(fitted_text_offset(200.0f, kFittedTextHoldSeconds + 1.0f, speed), 100.0f);
    // ...still at its end...
    CHECK_NEAR(fitted_text_offset(200.0f, kFittedTextHoldSeconds + 2.5f, speed), 200.0f);
    // ...and from the start again.
    const float cycle = kFittedTextHoldSeconds * 2.0f + 2.0f;
    CHECK_NEAR(fitted_text_offset(200.0f, cycle + 0.5f, speed), 0.0f);
}

// Two too long on one row take turns: the first a round, then the second.
void test_two_take_turns() {
    float seconds = 0.0f;
    CHECK(fitted_text_turn(4.0f, 6.0f, 1.0f, true, seconds));
    CHECK_NEAR(seconds, 1.0f);
    CHECK(!fitted_text_turn(4.0f, 6.0f, 1.0f, false, seconds));
    CHECK(!fitted_text_turn(4.0f, 6.0f, 5.0f, true, seconds));
    CHECK(fitted_text_turn(4.0f, 6.0f, 5.0f, false, seconds));
    CHECK_NEAR(seconds, 1.0f);
    // And round again.
    CHECK(fitted_text_turn(4.0f, 6.0f, 11.0f, true, seconds));
    CHECK_NEAR(seconds, 1.0f);
    // One that fits never takes a turn from the other.
    CHECK(fitted_text_turn(4.0f, 0.0f, 5.0f, true, seconds));
    CHECK_NEAR(seconds, 5.0f);
    CHECK_NEAR(fitted_text_cycle(0.0f, 100.0f), 0.0f);
    CHECK_NEAR(fitted_text_cycle(200.0f, 100.0f), kFittedTextHoldSeconds * 2.0f + 2.0f);
}

}  // namespace

int main() {
    test_room_is_shared_by_need();
    test_it_holds_slides_and_holds();
    test_two_take_turns();
    return test::finish("fitted-text");
}
