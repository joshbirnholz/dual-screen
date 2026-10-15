// The quick menu coming up: the backdrop first, quickly; the menu just after,
// fading in and growing the last little way; all of it over in a moment, and
// nothing animated until it is started.

#include <chrono>

#include "test_support.hpp"
#include "ui/menu_entrance.hpp"

using namespace ds;

namespace {

MenuEntrance::Clock::time_point at(MenuEntrance::Clock::time_point start, int ms) {
    return start + std::chrono::milliseconds(ms);
}

void test_the_backdrop_comes_first_then_the_menu() {
    MenuEntrance entrance;
    const auto start = MenuEntrance::Clock::now();
    entrance.start(start);
    CHECK_EQ(entrance.backdrop(start), 0.0f);
    CHECK_EQ(entrance.contents(start), 0.0f);
    CHECK(entrance.scale(start) < 1.0f);
    // Partway into the backdrop, the menu has not begun.
    CHECK(entrance.backdrop(at(start, 50)) > 0.3f);
    CHECK_EQ(entrance.contents(at(start, 50)), 0.0f);
    // The backdrop is done before the menu is.
    CHECK_EQ(entrance.backdrop(at(start, 130)), 1.0f);
    const float midway = entrance.contents(at(start, 130));
    CHECK(midway > 0.0f && midway < 1.0f);
    CHECK(entrance.animating(at(start, 130)));
    // And all of it in under a quarter of a second.
    CHECK_EQ(entrance.contents(at(start, 240)), 1.0f);
    CHECK_EQ(entrance.scale(at(start, 240)), 1.0f);
    CHECK(!entrance.animating(at(start, 240)));
}

void test_nothing_moves_until_it_is_started() {
    MenuEntrance entrance;
    CHECK_EQ(entrance.backdrop(), 1.0f);
    CHECK_EQ(entrance.contents(), 1.0f);
    CHECK_EQ(entrance.scale(), 1.0f);
    entrance.start();
    entrance.finish();
    CHECK_EQ(entrance.contents(), 1.0f);
}

// Back to the game: the menu goes first, then the backdrop, quickly, and it
// is gone; while it goes it is "leaving", and the game is going underneath.
void test_it_goes_away_menu_first() {
    MenuEntrance entrance;
    const auto start = MenuEntrance::Clock::now();
    entrance.leave(start);
    CHECK(entrance.leaving(start));
    CHECK_EQ(entrance.contents(start), 1.0f);
    CHECK_EQ(entrance.backdrop(start), 1.0f);
    // The menu is fading before the backdrop has begun to.
    CHECK(entrance.contents(at(start, 30)) < 1.0f);
    CHECK_EQ(entrance.backdrop(at(start, 30)), 1.0f);
    CHECK(entrance.scale(at(start, 30)) < 1.0f);
    // Gone in well under a quarter of a second.
    CHECK_EQ(entrance.contents(at(start, 150)), 0.0f);
    CHECK_EQ(entrance.backdrop(at(start, 150)), 0.0f);
    CHECK(!entrance.leaving(at(start, 150)));
    CHECK(!entrance.animating(at(start, 50)));
}

}  // namespace

int main() {
    test_the_backdrop_comes_first_then_the_menu();
    test_nothing_moves_until_it_is_started();
    test_it_goes_away_menu_first();
    return test::finish("menu-entrance");
}
