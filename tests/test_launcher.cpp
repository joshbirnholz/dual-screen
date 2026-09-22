#include <algorithm>
#include <vector>

#include "test_support.hpp"
#include "ui/launcher_screen.hpp"

using namespace ds;

namespace {

constexpr float kWidth = 1280.0f;
constexpr float kHeight = 720.0f;

bool plus_leaves(LauncherScreen& launcher) {
    return launcher.update({UiAction::MenuDisconnect}, {}, kWidth, kHeight).choice ==
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

}  // namespace

int main() {
    test_plus_only_quits_where_the_platform_says_so();
    test_the_other_face_buttons_do_nothing_here();
    test_quit_leads_the_control_guide();
    test_leaving_is_not_offered_where_the_system_does_it();
    test_no_key_leaves_where_leaving_is_not_offered();
    test_the_connection_list_does_not_show_addresses();
    return test::finish("launcher");
}
