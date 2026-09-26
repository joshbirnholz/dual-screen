#include "local_play.hpp"

#if DS_LOCAL_PLAY

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "app.hpp"
#include "util/log.hpp"

namespace ds::sw {

int play_here(const LocalPlayRequest& request) {
    AppOptions options;
    options.game_path = request.game_path;
    options.verbose = request.verbose;
    options.windowed = request.windowed;
    options.exit_after_seconds = request.exit_after_seconds;
    options.screenshot_at_seconds = request.screenshot_at_seconds;
    options.screenshot_path = request.screenshot_path;
    // Whatever would take the player back to the desktop app's own main menu
    // leaves instead: the menu they came from is this program's, and main()
    // brings it back. See AppOptions::leave_instead_of_launcher.
    options.leave_instead_of_launcher = true;
    options.keep_log_file = true;
    log_info(request.game_path.empty() ? "playing here: opening the game browser"
                                       : "playing here: " + request.game_path);
    App app;
    const int status = app.run(options);
    log_info("finished playing here (status " + std::to_string(status) + ")");
    return status;
}

bool relaunch(const std::string& self_path, const std::string& arguments) {
#ifdef __SWITCH__
    // Only a program started by hbloader -- the homebrew menu, or a
    // forwarder -- has somewhere to ask, and only it can be started again.
    if (self_path.empty() || !envHasNextLoad()) {
        log_warn("cannot ask to be started again: this was not started by the homebrew loader");
        return false;
    }
    // The argument string is the whole command line, the program's own path
    // first, the same as argv. Quoted, because a folder on an SD card can have
    // a space in its name.
    std::string command_line = "\"" + self_path + "\"";
    if (!arguments.empty()) {
        command_line += " " + arguments;
    }
    const Result result = envSetNextLoad(self_path.c_str(), command_line.c_str());
    if (R_FAILED(result)) {
        log_warn("the loader would not start this again (error " + std::to_string(result) + ")");
        return false;
    }
    log_info("starting again as: " + command_line);
    return true;
#else
    (void)self_path;
    (void)arguments;
    return false;
#endif
}

}  // namespace ds::sw

#endif  // DS_LOCAL_PLAY
