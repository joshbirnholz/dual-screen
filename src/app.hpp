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
    // Logs every OpenGL call a GPU-rendering core makes, and which of them the
    // driver rejected. Slow, and only useful when such a core is drawing
    // nothing and not saying why.
    bool trace_core_gl = false;
    std::string system_dir;   // overrides settings.json
    bool windowed = false;    // for development on a desktop
    bool verbose = false;
    bool show_help = false;
    bool list_layouts = false;
    // Runs for this many frames and then exits. Used by the headless smoke
    // test; 0 means run until the player quits.
    int exit_after_frames = 0;
    // Overrides whichever console is played this run's own streaming format
    // setting: "tiles" or "h264". Empty leaves the setting alone, which is the
    // usual case -- this is here so a test, or somebody comparing two runs,
    // does not have to edit a config file between them.
    std::string video_codec_override;
    // Where a console played by another program hands its screens over. Empty
    // means the usual place; a second copy of this app, or a test, needs its
    // own. See ExternalFrameLink.
    std::string external_socket;
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
    // Taps the window on its own, about eight times a second at alternating
    // spots, so a companion keeps measuring how long an input takes to come
    // back as a picture (see CompanionSession's "input to picture" line)
    // without anybody touching it. For measuring, not for playing.
    bool latency_probe = false;
    // Taps this one key (an SDL key name, e.g. "a" or "Return") a couple of
    // frames in, so a core's own keyboard callback can be exercised
    // headless. Empty means no simulated key.
    std::string simulate_key;
    // Joins the first host discovered, as "top" or "bottom", without going
    // through the picker. Used by the two-process test; also genuinely handy
    // when there is only ever one host on the network.
    std::string auto_connect_screen;
    // For a device whose main menu is another program's -- the Switch's,
    // where this app is started from Dualscreen's own companion menu or from a
    // game's forwarder. With no --game, the app opens straight to the game
    // browser; and wherever it would go back to its own main menu -- the
    // browser left, a game that would not start -- it leaves instead, for the
    // caller to take the player back to theirs.
    bool leave_instead_of_launcher = false;
    // The log is already open, in the caller's place: carry on writing to it
    // rather than starting one of this app's own, so a device has one log and
    // not two.
    bool keep_log_file = false;
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

    // After run(): whether it ended because the player chose Quit, or the
    // system closed the app, rather than to go back to a menu. A caller whose
    // own menu would come back next (see AppOptions::leave_instead_of_launcher)
    // leaves as well when this is true.
    bool quit_chosen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ds
