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

}  // namespace

int main() {
    test_plus_only_quits_where_the_platform_says_so();
    test_the_other_face_buttons_do_nothing_here();
    test_quit_leads_the_control_guide();
    return test::finish("launcher");
}
