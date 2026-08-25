// The Switch client's own logic, on this machine. Everything here is plain
// C++ with no libnx in it -- the client is one program with a small guarded
// section for the console's own start-up -- so the parts worth getting right
// can be tested where they can actually be run.

#include "test_support.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "core/system.hpp"
#include "host_list.hpp"
#include "layout/layout_registry.hpp"
#include "screen_quad.hpp"
#include "session_menu.hpp"

using namespace ds;
using namespace ds::sw;

namespace {

net::DiscoveredHost host_named(const std::string& address,
                               const std::string& name,
                               bool top_available = true,
                               bool bottom_available = true) {
    net::DiscoveredHost host;
    host.address = address;
    host.device_name = name;
    host.game_title = name + "'s game";
    host.top_available = top_available;
    host.bottom_available = bottom_available;
    return host;
}

// Hosts come and go on their own schedule -- a beacon every second, dropped
// after five quiet ones -- so the list reshuffles under the player while they
// are reading it. The selection has to stay on the machine it was on, not on
// the row that machine happened to be sitting in.
void test_the_focus_follows_the_host_and_not_the_row() {
    HostList list;
    list.update({host_named("10.0.0.1", "Desk PC"), host_named("10.0.0.2", "Deck"),
                 host_named("10.0.0.3", "Laptop")});
    list.move_focus(1);
    CHECK(list.focused() != nullptr);
    CHECK_STR_EQ(list.focused()->device_name, "Deck");

    // The host above it goes quiet: everything shifts up a row, and a focus
    // that was an index would now be pointing at the laptop.
    list.update({host_named("10.0.0.2", "Deck"), host_named("10.0.0.3", "Laptop")});
    CHECK_STR_EQ(list.focused()->device_name, "Deck");
    CHECK_EQ(list.focus(), 0);

    // A new host announcing itself ahead of it must not move it either.
    list.update({host_named("10.0.0.0", "New PC"), host_named("10.0.0.2", "Deck"),
                 host_named("10.0.0.3", "Laptop")});
    CHECK_STR_EQ(list.focused()->device_name, "Deck");
    CHECK_EQ(list.focus(), 1);

    // When the focused host is the one that goes quiet there is nothing to
    // follow, so the position it held is what is left -- and it has to be a
    // real row, including when it was the last one.
    list.move_focus(1);
    CHECK_STR_EQ(list.focused()->device_name, "Laptop");
    list.update({host_named("10.0.0.0", "New PC")});
    CHECK(list.focused() != nullptr);
    CHECK_STR_EQ(list.focused()->device_name, "New PC");
}

void test_moving_the_focus_wraps_and_survives_an_empty_list() {
    HostList list;
    // Nothing being heard from at all: the picker still gets input, and it
    // must not go looking into an empty list.
    list.move_focus(1);
    CHECK(list.focused() == nullptr);
    CHECK(list.empty());
    CHECK(!list.focused_is_joinable());

    list.update({host_named("10.0.0.1", "One"), host_named("10.0.0.2", "Two")});
    CHECK_STR_EQ(list.focused()->device_name, "One");
    list.move_focus(-1);
    CHECK_STR_EQ(list.focused()->device_name, "Two");
    list.move_focus(1);
    CHECK_STR_EQ(list.focused()->device_name, "One");
}

// A host whose bottom screen is already somebody else's device can still be
// listed -- it is running a game, and this device may want the other screen --
// but it cannot be joined as that screen.
void test_a_host_is_joinable_only_for_a_screen_it_still_has() {
    HostList list;
    list.set_wanted_screen(ScreenId::Bottom);
    list.update({host_named("10.0.0.1", "Taken", true, false),
                 host_named("10.0.0.2", "Free", true, true)});

    CHECK(!list.focused_is_joinable());
    CHECK(list.first_joinable() != nullptr);
    CHECK_STR_EQ(list.first_joinable()->device_name, "Free");

    // Asking to be the top screen instead makes the first one joinable, which
    // is the whole reason it is still on the list.
    list.set_wanted_screen(ScreenId::Top);
    CHECK(list.focused_is_joinable());
    CHECK_STR_EQ(list.first_joinable()->device_name, "Taken");

    // Nothing on offer at all: joining has to report that rather than picking
    // a host that would refuse.
    list.update({host_named("10.0.0.1", "Taken", false, false)});
    CHECK(list.first_joinable() == nullptr);
}

SessionMenuState three_ds_session() {
    SessionMenuState state;
    state.layout_id = "vertical";
    state.screen_gap_toggleable = true;
    state.max_render_scale = system_profile(SystemId::ThreeDs).max_render_scale;
    state.right_stick_choosable = system_profile(SystemId::ThreeDs).has_analog_stick;
    return state;
}

int row_index(const SessionMenu& menu, const std::string& prefix) {
    for (std::size_t index = 0; index < menu.rows().size(); ++index) {
        if (menu.rows()[index].rfind(prefix, 0) == 0) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

// Presses Down until the row starting with `prefix` is focused, then Confirm,
// and hands back what that asked the host for.
SessionMenuResult choose(SessionMenu& menu, const std::string& prefix) {
    const int wanted = row_index(menu, prefix);
    if (wanted < 0) {
        return SessionMenuResult();
    }
    while (menu.focus() != wanted) {
        menu.update({UiAction::Down});
    }
    return menu.update({UiAction::Confirm});
}

// A player holding the companion is holding the controller, so the menu there
// has to carry what the host's own does -- reaching for a second pad plugged
// into the machine across the room to change the layout is the thing this
// exists to avoid.
void test_the_session_menu_offers_the_whole_session() {
    LayoutRegistry registry;
    SessionMenu menu(registry);
    menu.open(three_ds_session());

    for (const char* prefix :
         {"Layout:", "Smoothing:", "Render:", "Right Stick:", "Right-Handed", "Gap:", "Sound:",
          "Reset Game", "Resume", "Disconnect", "Quit"}) {
        CHECK_MSG(row_index(menu, prefix) >= 0, std::string("no row for ") + prefix);
    }

    // Each one asks the host rather than deciding anything here.
    CHECK(choose(menu, "Smoothing:").wants_filtering);
    CHECK(choose(menu, "Render:").wants_render_scale);
    CHECK(choose(menu, "Right Stick:").wants_right_stick_mode);
    CHECK(choose(menu, "Right-Handed").wants_handedness);
    CHECK(choose(menu, "Gap:").wants_screen_gap);
    CHECK(choose(menu, "Sound:").wants_audio_target);
    CHECK(choose(menu, "Reset Game").reset);
    CHECK(choose(menu, "Resume").resume);
    CHECK(choose(menu, "Disconnect").disconnect);
    CHECK(choose(menu, "Quit").quit);
}

// The rows follow the session. A console with nothing to choose does not get
// the row, exactly as on the host's own menu.
void test_rows_appear_only_where_there_is_something_to_choose() {
    LayoutRegistry registry;
    SessionMenu menu(registry);

    SessionMenuState ds;
    ds.layout_id = "standard";
    ds.max_render_scale = system_profile(SystemId::Ds).max_render_scale;
    ds.right_stick_choosable = system_profile(SystemId::Ds).has_analog_stick;
    ds.screen_gap_toggleable = false;
    menu.open(ds);
    CHECK_EQ(row_index(menu, "Render:"), -1);
    CHECK_EQ(row_index(menu, "Right Stick:"), -1);
    CHECK_EQ(row_index(menu, "Gap:"), -1);
    CHECK(row_index(menu, "Layout:") >= 0);

    // And they arrive when the session changes under the open menu -- picking
    // Vertical Screens is what makes the gap a question at all.
    SessionMenuState with_gap = ds;
    with_gap.layout_id = "vertical";
    with_gap.screen_gap_toggleable = true;
    menu.set_state(with_gap);
    CHECK_MSG(row_index(menu, "Gap:") >= 0, "the gap row did not arrive with the layout");
}

// Layout is the one row with somewhere to go rather than something to toggle,
// and cycling it has to reach every layout and come back.
void test_the_layout_row_cycles_through_every_layout() {
    LayoutRegistry registry;
    SessionMenu menu(registry);
    SessionMenuState state = three_ds_session();
    state.layout_id = registry.at(0).id;
    menu.open(state);

    CHECK_EQ(row_index(menu, "Layout:"), 0);

    std::vector<std::string> seen;
    for (std::size_t step = 0; step < registry.size(); ++step) {
        const SessionMenuResult result = menu.update({UiAction::Right});
        CHECK(result.wants_layout);
        seen.push_back(result.layout_id);
        // The host would grant it and broadcast; this is that arriving back.
        state.layout_id = result.layout_id;
        menu.set_state(state);
    }
    CHECK_EQ(seen.size(), registry.size());
    CHECK_MSG(seen.back() == registry.at(0).id, "cycling did not come back round to the start");

    // Left goes the other way rather than being a second Right.
    const SessionMenuResult back = menu.update({UiAction::Left});
    CHECK(back.wants_layout);
    CHECK_MSG(back.layout_id == registry.at(registry.size() - 1).id,
              "left went to '" + back.layout_id + "'");
}

// Cycling along a list must not fire the rows that do something rather than
// change something: leaning on the stick should never quit the game.
void test_cycling_does_not_trip_the_actions() {
    LayoutRegistry registry;
    SessionMenu menu(registry);
    menu.open(three_ds_session());

    for (const char* prefix : {"Reset Game", "Resume", "Disconnect", "Quit"}) {
        const int wanted = row_index(menu, prefix);
        CHECK(wanted >= 0);
        while (menu.focus() != wanted) {
            menu.update({UiAction::Down});
        }
        for (const UiAction action : {UiAction::Left, UiAction::Right}) {
            const SessionMenuResult result = menu.update({action});
            CHECK_MSG(!result.reset && !result.resume && !result.disconnect && !result.quit,
                      std::string("cycling past ") + prefix + " acted on it");
        }
    }
}

float centre_x(const PixelRect& rect) { return rect.x + rect.w * 0.5f; }
float centre_y(const PixelRect& rect) { return rect.y + rect.h * 0.5f; }

// A screen the layout does not turn is drawn exactly where the layout put it.
void test_an_upright_screen_is_drawn_where_the_layout_says() {
    PlacedScreen placed;
    placed.rect = PixelRect{10.0f, 20.0f, 300.0f, 200.0f};
    placed.rotation = ScreenRotation::None;

    const ScreenQuad quad = screen_quad_for(placed);
    CHECK_NEAR(quad.x, 10.0f);
    CHECK_NEAR(quad.y, 20.0f);
    CHECK_NEAR(quad.w, 300.0f);
    CHECK_NEAR(quad.h, 200.0f);
    CHECK_NEAR(static_cast<float>(quad.angle_degrees), 0.0f);
}

// A turned one is not: the layout reports the box the turned screen fills, and
// SDL wants the box the picture goes into before being turned. Same centre,
// sides swapped -- and drawing it into the layout's box directly, which is the
// obvious thing to write, silently stretches the picture to the wrong shape.
void test_a_turned_screen_is_drawn_into_the_box_it_is_turned_out_of() {
    for (const ScreenRotation rotation :
         {ScreenRotation::Clockwise, ScreenRotation::CounterClockwise}) {
        PlacedScreen placed;
        placed.rect = PixelRect{10.0f, 20.0f, 300.0f, 200.0f};
        placed.rotation = rotation;

        const ScreenQuad quad = screen_quad_for(placed);
        CHECK_NEAR(quad.w, 200.0f);
        CHECK_NEAR(quad.h, 300.0f);
        CHECK_NEAR(quad.x + quad.w * 0.5f, centre_x(placed.rect));
        CHECK_NEAR(quad.y + quad.h * 0.5f, centre_y(placed.rect));
        CHECK_NEAR(static_cast<float>(quad.angle_degrees),
                   rotation == ScreenRotation::Clockwise ? 90.0f : -90.0f);
    }
}

// The same thing against a real layout rather than a made-up rect: Side-by-Side
// is the DS held sideways, so both its screens are turned. Whatever the layout
// hands over, the picture has to end up the screen's own shape -- a 4:3 screen
// drawn 3:4 is the bug this catches.
void test_side_by_side_draws_its_screens_the_right_way_up() {
    LayoutRegistry registry;
    const SystemScreens screens = system_profile(SystemId::Ds).screens;
    const std::vector<PlacedScreen> placed =
        place_screens(*registry.find("side_by_side"), 1280.0f, 720.0f, screens);
    CHECK_EQ(placed.size(), static_cast<std::size_t>(2));

    for (const PlacedScreen& screen : placed) {
        CHECK(rotation_is_quarter_turn(screen.rotation));
        const ScreenQuad quad = screen_quad_for(screen);
        const ScreenSize size = screens.size_of(screen.screen);
        const float wanted = static_cast<float>(size.width) / static_cast<float>(size.height);
        const float got = quad.w / quad.h;
        CHECK_MSG(std::abs(got - wanted) < 0.02f,
                  "the picture would be drawn at " + std::to_string(got) + ":1, not " +
                      std::to_string(wanted) + ":1");
        CHECK_NEAR(quad.x + quad.w * 0.5f, centre_x(screen.rect));
        CHECK_NEAR(quad.y + quad.h * 0.5f, centre_y(screen.rect));
    }
}

}  // namespace

int main() {
    test_the_focus_follows_the_host_and_not_the_row();
    test_moving_the_focus_wraps_and_survives_an_empty_list();
    test_a_host_is_joinable_only_for_a_screen_it_still_has();
    test_the_session_menu_offers_the_whole_session();
    test_rows_appear_only_where_there_is_something_to_choose();
    test_the_layout_row_cycles_through_every_layout();
    test_cycling_does_not_trip_the_actions();
    test_an_upright_screen_is_drawn_where_the_layout_says();
    test_a_turned_screen_is_drawn_into_the_box_it_is_turned_out_of();
    test_side_by_side_draws_its_screens_the_right_way_up();
    return test::finish("switch-client");
}
