#include <algorithm>
#include <vector>

#include "test_support.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/launcher_screen.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 720.0f;

bool plus_leaves(LauncherScreen& launcher) {
    return launcher.update({UiAction::MenuSettings}, {}, kWidth, kHeight).choice ==
           LauncherChoice::Quit;
}

// Plus leaves the app on the Switch, where every piece of homebrew does, and
// nowhere else -- on a pad in a game it is a button somebody is holding.
void test_plus_only_quits_where_the_platform_says_so() {
    LauncherScreen elsewhere;
    elsewhere.open();
    CHECK_MSG(!plus_leaves(elsewhere), "plus left the app on a platform that never asked it to");

    LauncherScreen console;
    console.set_plus_quits(true);
    console.open();
    CHECK_MSG(console.plus_quits(), "the launcher forgot it had been told about plus");
    CHECK_MSG(plus_leaves(console), "plus did not leave the app where that is the convention");
}

// The other menu-only face buttons still mean nothing here. They used to fall
// through into Quit, which made pressing X on the launcher close the app.
void test_the_other_face_buttons_do_nothing_here() {
    LauncherScreen launcher;
    launcher.set_plus_quits(true);
    launcher.open();
    for (const UiAction action : {UiAction::MenuQuit, UiAction::MenuReset}) {
        CHECK_MSG(launcher.update({action}, {}, kWidth, kHeight).choice == LauncherChoice::None,
                  "a menu's face button did something on the launcher");
    }
}

// The prompt that ends something leads the row, the same as Disconnect does in
// the quick menu.
void test_quit_leads_the_control_guide() {
    LauncherScreen launcher;
    launcher.open();
    for (const GuidePrompt& prompt : launcher.guide_prompts()) {
        CHECK_MSG(prompt.glyph != ButtonGlyph::Plus,
                  "plus was offered as a way out on a platform that does not leave that way");
    }

    launcher.set_plus_quits(true);
    const std::vector<GuidePrompt> guide = launcher.guide_prompts();
    CHECK_MSG(!guide.empty(), "the control guide said nothing at all");
    CHECK_MSG(guide.front().glyph == ButtonGlyph::Plus,
              "something other than Quit came first in the control guide");
    CHECK_MSG(guide.front().action == std::string("Quit"),
              "plus led the row saying '" + std::string(guide.front().action) + "'");
    CHECK_MSG(guide.size() > 1 && guide[1].glyph == ButtonGlyph::A,
              "Select stopped being offered once Quit moved in front of it");
}

// Every row this launcher will let you land on, found by walking down it.
// Behavioural on purpose: a row that is built and not reachable, or reachable
// and not built, are both wrong and only one of them shows up in a list.
std::vector<LauncherChoice> rows_you_can_reach(LauncherScreen& launcher) {
    std::vector<LauncherChoice> found;
    for (int step = 0; step < 16; ++step) {
        LauncherScreen probe;
        probe.set_plus_quits(launcher.plus_quits());
        probe.set_offers_quit(launcher.offers_quit());
        probe.set_offers_load_game(launcher.offers_load_game());
        probe.open(/*runs_games=*/false);
        for (int down = 0; down < step; ++down) {
            probe.update({UiAction::Down}, {}, kWidth, kHeight);
        }
        const LauncherChoice choice =
            probe.update({UiAction::Confirm}, {}, kWidth, kHeight).choice;
        if (std::find(found.begin(), found.end(), choice) == found.end()) {
            found.push_back(choice);
        }
    }
    return found;
}

bool offers(const std::vector<LauncherChoice>& rows, LauncherChoice choice) {
    return std::find(rows.begin(), rows.end(), choice) != rows.end();
}

// An Apple app is left with the Home gesture or the app switcher, never from
// inside. A Quit row there closes the window for no reason the player can see
// -- on an Apple TV, dropping them at a home screen they did not ask for.
void test_leaving_is_not_offered_where_the_system_does_it() {
    LauncherScreen ordinary;
    ordinary.open(/*runs_games=*/false);
    const std::vector<LauncherChoice> with_quit = rows_you_can_reach(ordinary);
    CHECK_MSG(offers(with_quit, LauncherChoice::Quit),
              "the launcher stopped offering a way out where there should be one");
    CHECK_MSG(offers(with_quit, LauncherChoice::ConnectAsBottomScreen),
              "walking the rows did not reach the ones that are always there");

    LauncherScreen apple;
    apple.set_offers_quit(false);
    apple.open(/*runs_games=*/false);
    const std::vector<LauncherChoice> rows = rows_you_can_reach(apple);
    CHECK_MSG(!offers(rows, LauncherChoice::Quit),
              "Quit was still reachable on a platform that leaves apps to the system");
    // And what is left is still there. A row removed by making the list
    // shorter is one thing; one removed by breaking the walk is another.
    CHECK_MSG(offers(rows, LauncherChoice::ConnectAsTopScreen) &&
                  offers(rows, LauncherChoice::ConnectAsBottomScreen),
              "removing Quit took the rest of the launcher with it");
}

// A Switch carrying its own DS emulator: a companion that can also play a game
// itself offers to, and offers Settings too.
void test_a_companion_that_plays_games_offers_to_load_one() {
    LauncherScreen plain;
    plain.open(/*runs_games=*/false);
    CHECK_MSG(!offers(rows_you_can_reach(plain), LauncherChoice::LoadGame),
              "a companion with no emulator offered to load a game");

    LauncherScreen switch_with_core;
    switch_with_core.set_offers_load_game(true);
    switch_with_core.open(/*runs_games=*/false);
    const std::vector<LauncherChoice> rows = rows_you_can_reach(switch_with_core);
    CHECK_MSG(offers(rows, LauncherChoice::LoadGame), "Load a game was not offered");
    CHECK_MSG(offers(rows, LauncherChoice::ConnectAsTopScreen) &&
                  offers(rows, LauncherChoice::ConnectAsBottomScreen),
              "offering to load a game took the connect rows away");
    // And Settings, with Data Management inside, as a device that plays games
    // has everywhere.
    CHECK_MSG(offers(rows, LauncherChoice::Settings), "Settings was not offered");
    CHECK_MSG(!offers(rows_you_can_reach(plain), LauncherChoice::Settings),
              "Settings appeared on the opening screen of a companion with no emulator");
}

// Not through a keyboard either. An iPad with a keyboard attached is still an
// iPad, and F10 is not a licence to close it.
void test_no_key_leaves_where_leaving_is_not_offered() {
    LauncherScreen apple;
    apple.set_offers_quit(false);
    apple.open(/*runs_games=*/false);
    CHECK_MSG(apple.update({UiAction::Quit}, {}, kWidth, kHeight).choice != LauncherChoice::Quit,
              "a keypress quit an app that does not offer quitting");

    // And plus does not sneak past it either, on the off chance both are set.
    LauncherScreen both;
    both.set_plus_quits(true);
    both.set_offers_quit(false);
    both.open(/*runs_games=*/false);
    CHECK_MSG(!plus_leaves(both), "plus quit an app that does not offer quitting");
}

// The connection list names devices, not addresses. An address means nothing to
// the player choosing between two consoles in the same room, and it is a detail
// of their network sitting on a screen.
void test_the_connection_list_does_not_show_addresses() {
    ds::net::DiscoveredHost host;
    host.device_name = "Josh's Steam Deck";
    host.address = "192.168.1.57";
    host.game_title = "Some Game";

    for (const bool available : {true, false}) {
        const std::string detail = host_row_detail(host, available);
        CHECK_MSG(detail.find(host.address) == std::string::npos,
                  "the connection list showed " + host.address);
        CHECK_MSG(detail.find(host.device_name) != std::string::npos,
                  "the connection list stopped naming the device: '" + detail + "'");
    }
    CHECK_MSG(host_row_detail(host, false).find("already taken") != std::string::npos,
              "a screen someone else already holds no longer says so");
}

// A host on another build is listed, but choosing it says which device
// needs the update instead of joining -- and offers this device's own update
// only when this device is the one behind and has an updater.
void test_a_host_that_needs_an_update_is_explained_not_joined() {
    ds::net::DiscoveredHost host;
    host.device_name = "Josh's PC";
    host.game_title = "Some Game";
    CHECK_MSG(host_row_detail(host, false).find("already taken") != std::string::npos, "an available row changed");
    for (const int behind : {1, -1}) {
        host.protocol_version = static_cast<uint16_t>(ds::net::kProtocolVersion + behind);
        CHECK_MSG(host_row_detail(host, false).find("already taken") == std::string::npos,
                  "a host on another build says its screen is taken");
        for (const bool can_update : {true, false}) {
            HostPickerScreen picker;
            picker.open(ScreenId::Bottom);
            picker.set_can_update(can_update);
            const std::vector<ds::net::DiscoveredHost> hosts{host};
            HostPickerResult result = picker.update(hosts, {UiAction::Confirm}, {}, 1280.0f, 720.0f);
            CHECK_MSG(!result.chosen, "a host on another build was joined");
            CHECK_MSG(picker.is_open(), "the picker closed under the alert");
            CHECK_MSG(picker.off_popup(0.0f, 0.0f, 1280.0f, 720.0f), "no alert went up");
            result = picker.update(hosts, {UiAction::Confirm}, {}, 1280.0f, 720.0f);
            const bool offered = behind > 0 && can_update;
            CHECK_MSG(result.check_for_updates == offered,
                      std::string("Check for Updates was ") + (offered ? "not " : "") + "offered");
            CHECK_MSG(!result.chosen, "answering the alert joined the host");
            CHECK_MSG(!picker.off_popup(0.0f, 0.0f, 1280.0f, 720.0f), "the alert stayed up");
        }
    }
}

LauncherTile recent_tile(const std::string& key, const std::string& computer) {
    LauncherTile tile;
    tile.key = key;
    tile.title = "Kirby";
    tile.computer = computer;
    return tile;
}

// Choosing a recent game that only one computer has asks whether to start it
// there; one more than one computer could start asks which.
void test_a_game_on_more_than_one_device_asks_which() {
    LauncherScreen launcher;
    launcher.open(false);
    launcher.set_recent({recent_tile("den/1", "Den PC")});
    // Down off the menu onto the game, then choose it.
    for (int at = 0; at < 4; ++at) launcher.update({UiAction::Down}, {}, kWidth, kHeight);
    launcher.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK_STR_EQ(launcher.alert_message(), "Start Kirby on Den PC?");
    CHECK_EQ(launcher.alert_buttons().size(), 2u);
    LauncherResult result = launcher.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.choice == LauncherChoice::StartRecent);
    CHECK_STR_EQ(result.recent, "den/1");

    LauncherTile shared = recent_tile("den/1", "Den PC");
    shared.places = {{"den/1", "Den PC", false}, {"office/9", "Office PC", true}};
    launcher.open(false);
    launcher.set_recent({shared});
    for (int at = 0; at < 4; ++at) launcher.update({UiAction::Down}, {}, kWidth, kHeight);
    launcher.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK_STR_EQ(launcher.alert_message(), "Which device do you want to start Kirby on?");
    const std::vector<PopupButton> buttons = launcher.alert_buttons();
    CHECK_EQ(buttons.size(), 3u);
    if (buttons.size() == 3u) {
        CHECK_STR_EQ(buttons[0].label, "Den PC");
        CHECK_STR_EQ(buttons[1].label, "Office PC");
        CHECK_STR_EQ(buttons[1].detail, "Asleep");
        CHECK_STR_EQ(buttons[2].label, "Cancel");
    }
    const std::vector<GuidePrompt> guide = launcher.guide_prompts();
    CHECK_EQ(guide.size(), 2u);
    if (guide.size() == 2u) {
        CHECK_STR_EQ(guide[0].action, "OK");
        CHECK_STR_EQ(guide[1].action, "Back");
    }
    // B takes it away, starting nothing.
    result = launcher.update({UiAction::Cancel}, {}, kWidth, kHeight);
    CHECK(result.choice == LauncherChoice::None);
    CHECK(launcher.alert_message().empty());
    // Asked again, the second computer.
    launcher.update({UiAction::Confirm}, {}, kWidth, kHeight);
    launcher.update({UiAction::Down}, {}, kWidth, kHeight);
    result = launcher.update({UiAction::Confirm}, {}, kWidth, kHeight);
    CHECK(result.choice == LauncherChoice::StartRecent);
    CHECK_STR_EQ(result.recent, "office/9");
}

// A click on the middle of a prompt that was drawn, as a mouse makes one:
// down and up in the same place.
std::vector<PointerEvent> click_on(const std::vector<GuideHit>& hits, ButtonGlyph glyph) {
    for (const GuideHit& hit : hits) {
        if (hit.glyph != glyph) continue;
        PointerEvent click;
        click.x = click.press_x = hit.rect.x + hit.rect.w * 0.5f;
        click.y = click.press_y = hit.rect.y + hit.rect.h * 0.5f;
        click.released = true;
        click.normalized = false;
        return {click};
    }
    return {};
}

// Clicking Back along the foot of the connection list goes back, as B does.
// It used to do nothing on a computer: only the companion's own shell turned
// clicks on these prompts into presses, and the desktop app never did.
void test_clicking_back_leaves_the_connection_list() {
    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    HostPickerScreen picker;
    picker.open(ScreenId::Bottom);
    const std::vector<ds::net::DiscoveredHost> none;
    picker.render(quads, text, glyphs, kWidth, kHeight, none, "Looking");
    const std::vector<PointerEvent> click = click_on(picker.guide_hits(), ButtonGlyph::B);
    CHECK_MSG(!click.empty(), "the connection list drew no Back prompt");
    const HostPickerResult result = picker.update(none, {}, click, kWidth, kHeight);
    CHECK_MSG(result.cancelled, "clicking Back did not leave the connection list");
    CHECK_MSG(!picker.is_open(), "the connection list stayed open after Back");
}

// And Quit along the foot of the first screen, where it is offered.
void test_clicking_quit_on_the_first_screen_quits() {
    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    LauncherScreen launcher;
    launcher.set_plus_quits(true);
    launcher.open();
    launcher.render(quads, text, glyphs, kWidth, kHeight, 0);
    const std::vector<PointerEvent> click = click_on(launcher.guide_hits(), ButtonGlyph::Plus);
    CHECK_MSG(!click.empty(), "the first screen drew no Quit prompt");
    CHECK_MSG(launcher.update({}, click, kWidth, kHeight).choice == LauncherChoice::Quit,
              "clicking Quit did not quit");
}

}  // namespace

int main() {
    test_a_host_that_needs_an_update_is_explained_not_joined();
    test_plus_only_quits_where_the_platform_says_so();
    test_the_other_face_buttons_do_nothing_here();
    test_quit_leads_the_control_guide();
    test_leaving_is_not_offered_where_the_system_does_it();
    test_a_companion_that_plays_games_offers_to_load_one();
    test_no_key_leaves_where_leaving_is_not_offered();
    test_the_connection_list_does_not_show_addresses();
    test_a_game_on_more_than_one_device_asks_which();
    test_clicking_back_leaves_the_connection_list();
    test_clicking_quit_on_the_first_screen_quits();
    return test::finish("launcher");
}
