// A 3DS game's render scale: how Auto chooses one for the screens as they are
// drawn, and how far a core's own option says it goes.

#include "test_support.hpp"

#include "core/render_scale.hpp"
#include "core/system.hpp"

using namespace ds;

namespace {

const SystemScreens& three_ds() { return system_profile(SystemId::ThreeDs).screens; }

void test_auto_is_the_least_scale_nothing_is_magnified_at() {
    // Nothing drawn anywhere yet: the console's own.
    CHECK_EQ(auto_render_scale(three_ds(), {}, 18), 1);
    // A Steam Deck's 1280x800 in Vertical Screens: the upper screen drawn at
    // 533x320 or so, a third again over 400x240 -- 2x.
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 533, 320}, {ScreenId::Bottom, 427, 320}}, 18), 2);
    // A 4K television with the upper screen filling its height: 2160 / 240.
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 3600, 2160}}, 18), 9);
    // The largest of the screens wins, wherever it is drawn: here the touch
    // screen, on a tablet.
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 400, 240}, {ScreenId::Bottom, 1280, 960}}, 18), 4);
    // A hair over a whole scale is not worth the next one up.
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 820, 492}}, 18), 2);
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 880, 528}}, 18), 3);
    // Never past what the core goes to, nor under 1x for a small window.
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 7680, 4320}}, 10), 10);
    CHECK_EQ(auto_render_scale(three_ds(), {{ScreenId::Top, 200, 120}}, 18), 1);
}

void test_a_cores_largest_scale_is_read_from_its_values() {
    // Azahar's values are the numbers; an older Citra's, words.
    CHECK_EQ(largest_offered_scale({"1", "2", "10", "18"}), 18);
    CHECK_EQ(largest_offered_scale({"1x (Native)", "2x", "3x", "10x"}), 10);
    CHECK_EQ(largest_offered_scale({"1x (Native 400x240)", "4x (1600x960)"}), 4);
    CHECK_EQ(largest_offered_scale({"disabled", "enabled"}), 0);
    CHECK_EQ(largest_offered_scale({}), 0);
}

}  // namespace

int main() {
    test_auto_is_the_least_scale_nothing_is_magnified_at();
    test_a_cores_largest_scale_is_read_from_its_values();
    return test::finish("render-scale");
}
