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

}  // namespace

int main() {
    test_plus_only_quits_where_the_platform_says_so();
    test_the_other_face_buttons_do_nothing_here();
    return test::finish("launcher");
}
