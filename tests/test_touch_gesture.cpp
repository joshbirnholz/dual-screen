// The two-finger tap, which on a device with no buttons is the only way into
// the menu while a game is on screen.
//
// It replaced a button drawn over the picture, and it has to be a tap rather
// than "a second finger landed": on the screen a stylus belongs to, a hand
// resting on the glass while the other one draws is a thing people do, and
// opening the menu underneath them would be worse than having no way in.
//
// Driven through InputManager::handle_event with synthesised finger events,
// so the real state machine runs -- the arming, the slip, the timeout and the
// release -- with no hardware and no window.

#include "test_support.hpp"

#include <SDL.h>

#include <algorithm>
#include <vector>

#include "config/settings.hpp"
#include "input/input_manager.hpp"

using namespace ds;

namespace {

// Any touch device that is not the one SDL invents for the mouse: a real
// finger is what says how many are down, and the synthetic mouse touch is
// deliberately ignored by the code under test.
constexpr SDL_TouchID kScreen = 1;

SDL_Event finger(uint32_t type, SDL_FingerID id, float x, float y) {
    SDL_Event event{};
    event.type = type;
    event.tfinger.touchId = kScreen;
    event.tfinger.fingerId = id;
    event.tfinger.x = x;
    event.tfinger.y = y;
    return event;
}

bool opened_the_menu(const InputManager& input) {
    const std::vector<UiAction>& actions = input.ui_actions();
    return std::find(actions.begin(), actions.end(), UiAction::ToggleMenu) != actions.end();
}

// Two fingers down, then up, with whatever happens in between.
void tap_with(InputManager& input, const std::vector<SDL_Event>& between) {
    input.handle_event(finger(SDL_FINGERDOWN, 1, 0.4f, 0.5f));
    input.handle_event(finger(SDL_FINGERDOWN, 2, 0.6f, 0.5f));
    for (const SDL_Event& event : between) {
        input.handle_event(event);
    }
    input.handle_event(finger(SDL_FINGERUP, 1, 0.4f, 0.5f));
    input.handle_event(finger(SDL_FINGERUP, 2, 0.6f, 0.5f));
}

void test_two_fingers_tapped_together_open_the_menu() {
    InputManager input;
    input.initialize(Settings());
    input.begin_frame();
    tap_with(input, {});
    CHECK(opened_the_menu(input));
    input.shutdown();
}

// The whole reason it waits for the release.
void test_a_finger_that_travels_is_not_a_tap() {
    InputManager input;
    input.initialize(Settings());
    input.begin_frame();
    tap_with(input, {finger(SDL_FINGERMOTION, 2, 0.9f, 0.5f)});
    CHECK_MSG(!opened_the_menu(input),
              "a second finger dragged across the screen opened the menu");
    input.shutdown();
}

// A small wobble is not travel. A finger resting on glass never holds still.
void test_a_wobble_is_still_a_tap() {
    InputManager input;
    input.initialize(Settings());
    input.begin_frame();
    tap_with(input, {finger(SDL_FINGERMOTION, 2, 0.605f, 0.502f)});
    CHECK(opened_the_menu(input));
    input.shutdown();
}

void test_three_fingers_are_not_two() {
    InputManager input;
    input.initialize(Settings());
    input.begin_frame();
    input.handle_event(finger(SDL_FINGERDOWN, 1, 0.3f, 0.5f));
    input.handle_event(finger(SDL_FINGERDOWN, 2, 0.5f, 0.5f));
    input.handle_event(finger(SDL_FINGERDOWN, 3, 0.7f, 0.5f));
    input.handle_event(finger(SDL_FINGERUP, 1, 0.3f, 0.5f));
    input.handle_event(finger(SDL_FINGERUP, 2, 0.5f, 0.5f));
    input.handle_event(finger(SDL_FINGERUP, 3, 0.7f, 0.5f));
    CHECK(!opened_the_menu(input));
    input.shutdown();
}

void test_one_finger_is_an_ordinary_tap_and_nothing_else() {
    InputManager input;
    input.initialize(Settings());
    input.begin_frame();
    input.handle_event(finger(SDL_FINGERDOWN, 1, 0.4f, 0.5f));
    input.handle_event(finger(SDL_FINGERUP, 1, 0.4f, 0.5f));
    CHECK(!opened_the_menu(input));
    // And it is still a pointer, which is what every prompt in the app is
    // pressed with.
    CHECK(input.pointer_events().size() == 2);
    CHECK(input.pointer_events().front().pressed);
    CHECK(input.pointer_events().back().released);
    input.shutdown();
}

// Two fingers parked on the glass are not a gesture, however long you leave
// them there.
void test_fingers_left_down_are_not_a_tap() {
    InputManager input;
    input.initialize(Settings());
    input.begin_frame();
    input.handle_event(finger(SDL_FINGERDOWN, 1, 0.4f, 0.5f));
    input.handle_event(finger(SDL_FINGERDOWN, 2, 0.6f, 0.5f));
    SDL_Delay(900);
    input.handle_event(finger(SDL_FINGERUP, 1, 0.4f, 0.5f));
    input.handle_event(finger(SDL_FINGERUP, 2, 0.6f, 0.5f));
    CHECK_MSG(!opened_the_menu(input), "two fingers resting on the glass opened the menu");
    input.shutdown();
}

// It works more than once -- the arming used to be cleared only when every
// finger had lifted, so a hand that never quite let go broke it for good.
void test_it_works_again_and_again() {
    InputManager input;
    input.initialize(Settings());
    for (int round = 0; round < 3; ++round) {
        input.begin_frame();
        tap_with(input, {});
        CHECK(opened_the_menu(input));
    }
    input.shutdown();
}

}  // namespace

int main() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        test::skip(std::string("SDL would not start: ") + SDL_GetError());
        return test::finish("touch-gesture");
    }
    test_two_fingers_tapped_together_open_the_menu();
    test_a_finger_that_travels_is_not_a_tap();
    test_a_wobble_is_still_a_tap();
    test_three_fingers_are_not_two();
    test_one_finger_is_an_ordinary_tap_and_nothing_else();
    test_fingers_left_down_are_not_a_tap();
    test_it_works_again_and_again();
    SDL_Quit();
    return test::finish("touch-gesture");
}
