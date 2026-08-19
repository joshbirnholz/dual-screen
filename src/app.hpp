#pragma once

#include <memory>
#include <string>
#include <vector>

namespace ds {

// Everything the command line can say. Kept as plain data so the launch path is
// testable without starting SDL.
struct AppOptions {
    std::string game_path;
    std::string core_path;    // overrides settings.json / the search paths
    std::string layout_id;    // overrides the game's saved layout for this run
    std::string system_dir;   // overrides settings.json
    bool windowed = false;    // for development on a desktop
    bool verbose = false;
    bool show_help = false;
    bool list_layouts = false;
    // Runs for this many frames and then exits. Used by the headless smoke
    // test; 0 means run until the player quits.
    int exit_after_frames = 0;
    // Exits after this long. More robust than a frame count for tests that
    // need two processes to overlap, since frame rate is not bounded when the
    // display has no vsync.
    double exit_after_seconds = 0.0;
    // Writes a raw PPM screenshot here just before exiting, for tests.
    std::string screenshot_path;
    // Takes that screenshot this many seconds in instead of at exit. Needed
    // whenever what is on screen depends on another process still being alive.
    double screenshot_at_seconds = 0.0;
    // Opens a menu at startup instead of going straight into the game. Used to
    // capture the UI in tests; "layout", "settings" or "" .
    std::string start_screen;
    // Holds a synthetic touch at this normalized window position every frame,
    // so the touch path can be exercised without a real device. Negative means
    // no simulated touch.
    float simulate_touch_x = -1.0f;
    float simulate_touch_y = -1.0f;
    // Joins the first host discovered, as "top" or "bottom", without going
    // through the picker. Used by the two-process test; also genuinely handy
    // when there is only ever one host on the network.
    std::string auto_connect_screen;
    std::string parse_error;
};

AppOptions parse_command_line(int argc, char** argv);
std::string usage_text();

class App {
public:
    App();
    ~App();

    // Returns a process exit code.
    int run(const AppOptions& options);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ds
