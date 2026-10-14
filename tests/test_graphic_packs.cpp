// The Wii U quick menu's Graphics Packs page, driven the way a player drives
// it -- presses in, the changes it asks Cemu for out: how the packs are
// grouped down the left, moving into a pack and back out as in the quick
// menu, what a press on its rows asks for, where the cursor goes when Cemu's
// answer arrives, and when the page says a change waits for the game to be
// reset. Nothing here draws, so none of it needs a GL context.

#include "test_support.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "ui/graphic_packs_screen.hpp"
#include "ui/list_touch.hpp"

#include <chrono>
#include <thread>

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 800.0f;

// As Cemu lists a game with some of the community's packs installed, in the
// order it happens to have loaded them.
std::vector<ExternalGraphicPack> game_packs() {
    std::vector<ExternalGraphicPack> packs(4);
    packs[0].id = "fps";
    packs[0].path = "Some Game/Mods/FPS++";
    packs[0].name = "FPS++";
    packs[0].choices.push_back({"", {"30FPS", "60FPS"}, 0});
    packs[1].id = "graphics";
    packs[1].path = "Some Game/Graphics";
    packs[1].name = "Graphics";
    packs[1].enabled = true;
    packs[1].restart_needed = true;
    packs[1].choices.push_back({"TV Resolution", {"720p", "1080p", "1440p"}, 1});
    packs[1].choices.push_back({"Shadows", {"Low", "High"}, -1});
    packs[2].id = "bloom";
    packs[2].path = "Some Game/Enhancements/No Bloom";
    packs[2].name = "No Bloom";
    packs[3].id = "aa";
    packs[3].path = "Some Game/Enhancements/Anti-Aliasing";
    packs[3].name = "Anti-Aliasing";
    return packs;
}

GraphicPacksScreenResult press(GraphicPacksScreen& screen, UiAction action) {
    return screen.update({action}, {}, kWidth, kHeight);
}

std::vector<std::string> describe(const std::vector<GraphicPackLine>& lines) {
    std::vector<std::string> out;
    for (const GraphicPackLine& line : lines) {
        out.push_back((line.kind == GraphicPackLine::Kind::Heading ? "# " : "") + line.text);
    }
    return out;
}

void test_packs_are_grouped_as_cemu_files_them() {
    const std::vector<std::string> lines = describe(graphic_pack_lines(game_packs()));
    // The game's name, which every pack shares, is left off; packs in no
    // group come first; each group alphabetically.
    const std::vector<std::string> expected = {
        "Graphics", "# Enhancements", "Anti-Aliasing", "No Bloom", "# Mods", "FPS++",
    };
    CHECK_MSG(lines == expected, "the packs were not laid out as Cemu's window files them");
}

void test_into_a_pack_and_back_out_as_in_the_quick_menu() {
    GraphicPacksScreen screen;
    screen.open();
    CHECK(screen.is_open());
    // Nothing to go into until Cemu answers.
    press(screen, UiAction::Right);
    CHECK(!screen.in_pack());
    screen.set_packs(game_packs());

    // Down the list -- packs only, headings passed over -- stopping at the end.
    for (int step = 0; step < 6; ++step) press(screen, UiAction::Down);
    CHECK_EQ(screen.focus(), 3);
    press(screen, UiAction::Up);
    press(screen, UiAction::Up);
    press(screen, UiAction::Up);
    CHECK_EQ(screen.focus(), 0);

    // Right goes into Graphics, on its first row; Left comes back out.
    CHECK(!press(screen, UiAction::Right).change);
    CHECK(screen.in_pack());
    CHECK_EQ(screen.item(), 0);
    press(screen, UiAction::Left);
    CHECK(!screen.in_pack());
    CHECK_EQ(screen.focus(), 0);
    // A as well; B back out, and B again leaves the page.
    CHECK(!press(screen, UiAction::Confirm).change);
    CHECK(screen.in_pack());
    CHECK(!press(screen, UiAction::Cancel).close_requested);
    CHECK(!screen.in_pack());
    CHECK(press(screen, UiAction::Cancel).close_requested);
    CHECK(!screen.is_open());

    // The menu's own button leaves from inside a pack too.
    screen.open();
    screen.set_packs(game_packs());
    press(screen, UiAction::Right);
    CHECK(press(screen, UiAction::ToggleMenu).close_requested);
}

void test_a_pack_is_turned_on_and_off() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    press(screen, UiAction::Down);  // Anti-Aliasing
    press(screen, UiAction::Right);
    GraphicPacksScreenResult result = press(screen, UiAction::Confirm);
    CHECK_MSG(result.change.has_value(), "A on Enabled asked Cemu for nothing");
    if (result.change) {
        CHECK(result.change->id == "aa");
        CHECK(result.change->kind == ExternalGraphicPackChange::Kind::Enabled);
        CHECK(result.change->enabled);
    }
    CHECK(screen.in_pack());
    // Only A turns it on and off: the d-pad changes nothing.
    CHECK(!press(screen, UiAction::Right).change);
    CHECK(!press(screen, UiAction::Left).change);
    press(screen, UiAction::Right);  // back into it
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && !result.change->enabled);
    CHECK_MSG(!screen.restart_pending(), "a pack that changes at once said it waits for a reset");

    // Until Cemu, having read it on the way in, answers that it does.
    std::vector<ExternalGraphicPack> answered = game_packs();
    answered[3].restart_needed = true;
    screen.set_packs(answered);
    CHECK_MSG(screen.restart_pending(), "Cemu's word that the pack waits for a reset went unsaid");
    // And the cursor is where it was, in Anti-Aliasing.
    CHECK_EQ(screen.focus(), 1);
    CHECK(screen.in_pack());
}

void test_presets_are_chosen_from_a_popup_and_say_when_they_wait_for_a_reset() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    press(screen, UiAction::Right);  // into Graphics
    press(screen, UiAction::Down);   // TV Resolution, on 1080p
    // A opens its choices, the one in effect under the cursor; nothing yet.
    GraphicPacksScreenResult result = press(screen, UiAction::Confirm);
    CHECK(!result.change);
    CHECK(screen.choosing());
    CHECK_EQ(screen.choice_focus(), 1);
    // Choosing the one in effect changes nothing.
    CHECK(!press(screen, UiAction::Confirm).change);
    CHECK(!screen.choosing());
    // The last, and no further; chosen.
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.choice_focus(), 2);
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && result.change->kind == ExternalGraphicPackChange::Kind::Preset &&
          result.change->category == "TV Resolution" && result.change->preset == "1440p");
    CHECK_MSG(screen.restart_pending(), "a resolution pack's change did not say it waits for a reset");
    // B puts the popup away, changing nothing, and leaves the cursor in the pack.
    press(screen, UiAction::Confirm);
    CHECK(!press(screen, UiAction::Cancel).change);
    CHECK(!screen.choosing());
    CHECK(screen.in_pack());
    // The d-pad's sideways presses step nothing.
    CHECK(!press(screen, UiAction::Right).change);

    // None chosen yet counts as the first, which is what is shown.
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    CHECK_EQ(screen.choice_focus(), 0);
    press(screen, UiAction::Down);
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && result.change->category == "Shadows" && result.change->preset == "High");
    // The rows stop at the last.
    press(screen, UiAction::Down);
    CHECK_EQ(screen.item(), 2);

    // A pack's one unnamed choice is a preset like any other.
    press(screen, UiAction::Left);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);  // FPS++
    press(screen, UiAction::Right);
    press(screen, UiAction::Down);
    press(screen, UiAction::Confirm);
    press(screen, UiAction::Down);
    result = press(screen, UiAction::Confirm);
    CHECK(result.change && result.change->id == "fps" && result.change->category.empty() &&
          result.change->preset == "60FPS");

    // The menu's own button puts the popup and the page away together.
    press(screen, UiAction::Confirm);
    CHECK(press(screen, UiAction::ToggleMenu).close_requested);
    CHECK(!screen.is_open());
}

void test_the_cursor_stays_put_when_cemu_answers() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    // On Shadows, Graphics' second preset.
    press(screen, UiAction::Right);
    press(screen, UiAction::Down);
    press(screen, UiAction::Down);
    CHECK_EQ(screen.item(), 2);

    // Cemu's answer, in a new order and with Shadows first: still on Shadows.
    std::vector<ExternalGraphicPack> packs = game_packs();
    std::swap(packs[0], packs[3]);
    for (ExternalGraphicPack& pack : packs) {
        if (pack.id == "graphics") std::swap(pack.choices[0], pack.choices[1]);
    }
    screen.set_packs(packs);
    CHECK_EQ(screen.focus(), 0);
    CHECK(screen.in_pack());
    CHECK_EQ(screen.item(), 1);

    // Shadows hidden by a condition: the nearest row left.
    for (ExternalGraphicPack& pack : packs) {
        if (pack.id == "graphics") pack.choices.erase(pack.choices.begin());
    }
    screen.set_packs(packs);
    CHECK(screen.in_pack());
    CHECK_EQ(screen.item(), 1);

    // The pack itself gone: back to the list, on the nearest pack.
    packs.erase(std::remove_if(packs.begin(), packs.end(),
                               [](const ExternalGraphicPack& pack) { return pack.id == "graphics"; }),
                packs.end());
    screen.set_packs(packs);
    CHECK(!screen.in_pack());
    CHECK_EQ(screen.focus(), 0);

    // Nothing at all for this game: nothing to go into, and A asks for nothing.
    screen.set_packs({});
    CHECK(!press(screen, UiAction::Confirm).change);
    CHECK(!screen.in_pack());
}

void test_a_menus_face_buttons_are_not_this_pages() {
    GraphicPacksScreen screen;
    screen.open();
    screen.set_packs(game_packs());
    const GraphicPacksScreenResult result = press(screen, UiAction::MenuQuit);
    CHECK(!result.close_requested && !result.change);
}

}  // namespace

// The other device draws the host's page from a message: the same packs, the
// cursor in the same place, and nothing it can change by itself.
void test_the_page_is_drawn_on_the_other_device_as_it_is() {
    GraphicPacksScreen host;
    host.open();
    host.set_packs(game_packs());
    press(host, UiAction::Down);   // Anti-Aliasing
    press(host, UiAction::Down);   // No Bloom
    press(host, UiAction::Down);   // FPS++
    press(host, UiAction::Right);  // into it
    press(host, UiAction::Down);   // its preset
    const net::GraphicPacksViewMessage view = graphic_packs_view_of(host);
    CHECK(view.open && !view.waiting);
    CHECK_EQ(view.packs.size(), game_packs().size());

    GraphicPacksScreen mirror;
    mirror.show_mirrored(view, kWidth, kHeight);
    CHECK(mirror.is_open());
    CHECK_EQ(mirror.focus(), 3);
    CHECK(mirror.in_pack());
    CHECK_EQ(mirror.item(), 1);
    CHECK_EQ(mirror.packs().size(), game_packs().size());

    // Before Cemu has answered, the other device says so too.
    GraphicPacksScreen asking;
    asking.open();
    GraphicPacksScreen asked_mirror;
    asked_mirror.show_mirrored(graphic_packs_view_of(asking), kWidth, kHeight);
    CHECK(asked_mirror.waiting());

    // A preset's popup, cursor and all.
    press(host, UiAction::Confirm);
    press(host, UiAction::Down);
    const net::GraphicPacksViewMessage choosing = graphic_packs_view_of(host);
    CHECK_EQ(choosing.choice_focus, 2);
    GraphicPacksScreen choosing_mirror;
    choosing_mirror.show_mirrored(choosing, kWidth, kHeight);
    CHECK(choosing_mirror.choosing());
    CHECK_EQ(choosing_mirror.choice_focus(), 1);
    press(host, UiAction::Cancel);
    choosing_mirror.show_mirrored(graphic_packs_view_of(host), kWidth, kHeight);
    CHECK(!choosing_mirror.choosing());

    // A page that is down is a message that says so.
    host.close();
    CHECK(!graphic_packs_view_of(host).open);
}

// A finger dragging up from (x, from_y) by `distance`, slowly, on a page drawn
// from the other device's.
void drag_up(GraphicPacksScreen& page, float x, float from_y, float distance) {
    const auto pass = [&](const std::vector<PointerEvent>& pointers) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        page.drag(pointers, kWidth, kHeight);
    };
    PointerEvent down;
    down.x = down.press_x = x;
    down.y = down.press_y = from_y;
    down.pressed = true;
    down.normalized = false;
    set_live_pointer(true, x, from_y);
    pass({down});
    float y = from_y;
    for (int step = 0; step < 10; ++step) {
        y -= distance / 10.0f;
        set_live_pointer(true, x, y);
        pass({});
    }
    // Held still before lifting, so it does not glide on.
    for (int step = 0; step < 8; ++step) pass({});
    PointerEvent up = down;
    up.pressed = false;
    up.released = true;
    up.y = y;
    set_live_pointer(false, x, y);
    pass({up});
}

// On the device drawing the other's page, a finger scrolls either side of
// it; the page follows the other device's cursor only when that moves.
void test_the_other_devices_page_scrolls_under_a_finger() {
    std::vector<ExternalGraphicPack> packs;
    for (int index = 0; index < 30; ++index) {
        ExternalGraphicPack pack;
        pack.id = "pack" + std::to_string(index);
        pack.path = "Some Game/Mods/Pack " + std::to_string(100 + index);
        pack.name = "Pack " + std::to_string(100 + index);
        pack.description = "What this pack does.";
        for (int word = 0; word < 40; ++word) pack.description += "\nA line of what this pack does.";
        packs.push_back(pack);
    }
    GraphicPacksScreen host;
    host.open();
    host.set_packs(packs);

    GraphicPacksScreen mirror;
    mirror.show_mirrored(graphic_packs_view_of(host), kWidth, kHeight);
    CHECK_EQ(mirror.list_scroll(), 0.0f);
    // Up the list, on the left.
    drag_up(mirror, kWidth * 0.2f, kHeight * 0.7f, 200.0f);
    const float dragged = mirror.list_scroll();
    CHECK_MSG(dragged > 50.0f, "the list did not scroll under a finger");
    // Up the pack's side, on the right.
    drag_up(mirror, kWidth * 0.75f, kHeight * 0.7f, 200.0f);
    CHECK_MSG(mirror.detail_scroll() > 50.0f, "the pack's side did not scroll under a finger");

    // The same view again leaves it where the finger did...
    mirror.show_mirrored(graphic_packs_view_of(host), kWidth, kHeight);
    CHECK_EQ(mirror.list_scroll(), dragged);
    // ...and the other device's cursor moving brings it back to the cursor.
    press(host, UiAction::Down);
    press(host, UiAction::Up);
    press(host, UiAction::Down);
    mirror.show_mirrored(graphic_packs_view_of(host), kWidth, kHeight);
    CHECK(mirror.list_scroll() < dragged);
    CHECK_EQ(mirror.detail_scroll(), 0.0f);
}

int main() {
    test_packs_are_grouped_as_cemu_files_them();
    test_into_a_pack_and_back_out_as_in_the_quick_menu();
    test_a_pack_is_turned_on_and_off();
    test_presets_are_chosen_from_a_popup_and_say_when_they_wait_for_a_reset();
    test_the_cursor_stays_put_when_cemu_answers();
    test_a_menus_face_buttons_are_not_this_pages();
    test_the_page_is_drawn_on_the_other_device_as_it_is();
    test_the_other_devices_page_scrolls_under_a_finger();
    return test::finish("graphic-packs");
}
