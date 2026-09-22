#pragma once

#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "net/discovery.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "ui/control_guide.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// What the player picked on the opening screen.
enum class LauncherChoice {
    None,
    LoadGame,
    ConnectAsTopScreen,
    ConnectAsBottomScreen,
    Settings,
    Quit,
};

struct LauncherResult {
    LauncherChoice choice = LauncherChoice::None;
};

// The opening screen when no ROM was named: play a game here, or join one
// already running on another device and be one of its screens.
class LauncherScreen {
public:
    // `runs_games` is false on a companion client, which has no ROMs of its
    // own to load and nothing in Settings that would mean anything there --
    // folders, cores and what a new game starts with all belong to the device
    // actually running one. What is left is joining a game running somewhere
    // else, and leaving.
    void open(bool runs_games = true);
    void close() { open_ = false; }
    bool is_open() const { return open_; }

    // Whether plus leaves the app, which the guide then says. Set on the
    // Switch, where every piece of homebrew exits that way and a player will
    // try it before looking for a row to select.
    void set_plus_quits(bool quits) { plus_quits_ = quits; }
    bool plus_quits() const { return plus_quits_; }

    // Whether leaving the app is something this screen offers at all.
    //
    // False on iOS, iPadOS and tvOS, where an app is left with the Home
    // gesture or the app switcher and never from inside. Apple's guidelines
    // say not to quit programmatically, and a player who has just used Quit
    // is left looking at a screen that vanished for no visible reason -- on
    // an Apple TV, at a home screen they did not ask for. Read before open(),
    // which is where the rows are decided.
    void set_offers_quit(bool offers) { offers_quit_ = offers; }
    bool offers_quit() const { return offers_quit_; }

    // What the control guide at the foot of the screen says, in the order it
    // says it. Exposed rather than built where it is drawn so the order is
    // something that can be checked.
    std::vector<GuidePrompt> guide_prompts() const;

    LauncherResult update(const std::vector<UiAction>& actions,
                          const std::vector<PointerEvent>& pointers,
                          float output_width,
                          float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                int hosts_found);

public:
    // Where the cursor is, so the shell can tell when it moved.
    int focus() const { return focus_; }

private:
    struct Row {
        LauncherChoice choice;
        const char* title;
        const char* detail;
    };

    std::vector<PixelRect> compute_rows(float output_width, float output_height, float& out_scale)
        const;

    bool open_ = false;
    int focus_ = 0;
    // Which entries this device is offering, built when the screen opens.
    std::vector<LauncherChoice> rows_;
    bool runs_games_ = true;
    bool plus_quits_ = false;
    bool offers_quit_ = true;
    std::vector<GuideHit> guide_hits_;

public:
    // Where this screen's prompts ended up, for a finger. See
    // guide_button_at() in control_guide.hpp.
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }
};

// Picks which host to join, from whatever discovery has found.
struct HostPickerResult {
    bool cancelled = false;
    bool chosen = false;
    ds::net::DiscoveredHost host;
};

// The line under a host's game title in the connection list. Its address is
// deliberately not in it: the player picks a device by its name, and an address
// is both meaningless to them and a detail of their network to leave on screen.
std::string host_row_detail(const ds::net::DiscoveredHost& host, bool available);

class HostPickerScreen {
public:
    void open(ScreenId wanted_screen);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    ScreenId wanted_screen() const { return wanted_screen_; }

    HostPickerResult update(const std::vector<ds::net::DiscoveredHost>& hosts,
                            const std::vector<UiAction>& actions,
                            const std::vector<PointerEvent>& pointers,
                            float output_width,
                            float output_height);

    void render(QuadRenderer& quads,
                TextRenderer& text,
                const ButtonGlyphSheet& glyphs,
                float output_width,
                float output_height,
                const std::vector<ds::net::DiscoveredHost>& hosts,
                const std::string& status);

public:
    // Where the cursor is, so the shell can tell when it moved.
    int focus() const { return focus_; }

private:
    std::vector<PixelRect> compute_rows(std::size_t count,
                                        float output_width,
                                        float output_height,
                                        float& out_scale) const;

    bool open_ = false;
    int focus_ = 0;
    ScreenId wanted_screen_ = ScreenId::Bottom;
    std::vector<GuideHit> guide_hits_;

public:
    // Where this screen's prompts ended up, for a finger. See
    // guide_button_at() in control_guide.hpp.
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }
};

}  // namespace ds
