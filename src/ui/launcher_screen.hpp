#pragma once

#include <map>
#include <string>
#include <vector>

#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "net/discovery.hpp"
#include "net/launch_service.hpp"
#include "render/button_glyphs.hpp"
#include "render/texture.hpp"
#include "render/text_renderer.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/control_guide.hpp"
#include "ui/sound_view.hpp"
#include "ui/ui_theme.hpp"

namespace ds {

// What the player picked on the opening screen.
enum class LauncherChoice {
    None,
    LoadGame,
    ConnectAsTopScreen,
    ConnectAsBottomScreen,
    // Data Management is in here, as its first row.
    Settings,
    Quit,
    // One of the recently played games along the foot, the start asked
    // about and agreed to: `recent` says which, by its LauncherTile::key.
    StartRecent,
};

struct LauncherResult {
    LauncherChoice choice = LauncherChoice::None;
    std::string recent;
};

// A game recently played on a computer this device can start games on, as
// the launcher shows it along its foot: only its icon.
struct LauncherTile {
    // The same game on the same computer: a texture made once.
    std::string key;
    std::string system;
    // For the alert asking before it is started: "Start <title> on
    // <computer>?"
    std::string title;
    std::string computer;
    int icon_width = 0;
    int icon_height = 0;
    std::vector<uint8_t> icon_rgba;
};

// The opening screen when no ROM was named: play a game here, or join one
// already running on another device and be one of its screens.
class LauncherScreen : public MenuPage {
public:
    // `runs_games` is false on a companion client, which has no ROMs of its
    // own to load and nothing in Settings that would mean anything there --
    // folders, cores and what a new game starts with all belong to the device
    // actually running one. What is left is joining a game running somewhere
    // else, and leaving.
    void open(bool runs_games = true);
    // The recently played games along the foot, one row of them, in the
    // order given -- newest first. Empty takes them away.
    void set_recent(std::vector<LauncherTile> tiles);
    std::size_t recent_count() const { return tiles_.size(); }
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;

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

    // Whether a companion client that also plays games itself offers to load
    // one, and Settings with it: a Switch carrying its own DS emulator. Read
    // by open(), like the flag above.
    void set_offers_load_game(bool offers) { offers_load_game_ = offers; }
    bool offers_load_game() const { return offers_load_game_; }

    // What the control guide at the foot of the screen says, in the order it
    // says it. Exposed rather than built where it is drawn so the order is
    // something that can be checked.
    std::vector<GuidePrompt> guide_prompts() const;

    LauncherResult update(const std::vector<UiAction>& actions,
                          const std::vector<PointerEvent>& pointers,
                          float output_width,
                          float output_height);

    // A short list, all on screen: nothing scrolls.
    bool scroll(const std::vector<PointerEvent>&, float, float) override { return false; }
    // A row by its index.
    int entry_at(float x, float y, float output_width, float output_height) const override;
    // Off the alert asking before a game is started: it goes away.
    bool off_popup(float x, float y, float output_width, float output_height) const override;
    // Nothing scrolls.
    std::vector<float> scroll_state(float, float) const override { return {}; }
    void set_scroll_state(const std::vector<float>&, float, float) override {}

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
    // Where each tile shown goes; fewer than given when two rows cannot hold
    // them all.
    std::vector<PixelRect> compute_tiles(float output_width, float output_height, int& columns) const;
    std::vector<LauncherTile> tiles_;
    std::map<std::string, Texture> tile_textures_;
    // How many tiles fit, last laid out: the cursor goes no further.
    int tiles_shown_ = 0;
    int tile_columns_ = 1;
    // The game chosen along the foot, being asked about -- "Start it on
    // that computer?" -- or -1.
    int confirming_ = -1;
    PopupCursor confirm_cursor_;
    std::string confirm_message() const;
    PopupGeometry confirm_geometry(float output_width, float output_height) const;

    bool open_ = false;
    int focus_ = 0;
    // Which entries this device is offering, built when the screen opens.
    std::vector<LauncherChoice> rows_;
    bool runs_games_ = true;
    bool plus_quits_ = false;
    bool offers_quit_ = true;
    bool offers_load_game_ = false;
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

class HostPickerScreen : public MenuPage {
public:
    void open(ScreenId wanted_screen);
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;
    ScreenId wanted_screen() const { return wanted_screen_; }

    HostPickerResult update(const std::vector<ds::net::DiscoveredHost>& hosts,
                            const std::vector<UiAction>& actions,
                            const std::vector<PointerEvent>& pointers,
                            float output_width,
                            float output_height);

    // A short list, all on screen: nothing scrolls.
    bool scroll(const std::vector<PointerEvent>&, float, float) override { return false; }
    // A host by its index in the list last handed to update().
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float, float, float, float) const override { return false; }
    // Nothing scrolls.
    std::vector<float> scroll_state(float, float) const override { return {}; }
    void set_scroll_state(const std::vector<float>&, float, float) override {}

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
    // How many hosts update() was last given, which
    // entry_at() lays out.
    std::size_t host_count_ = 0;
    std::vector<GuideHit> guide_hits_;
    // Each host's game icon, by its address and port, loaded the first time
    // its row is drawn with it.
    struct HostIcon {
        uint32_t icon_id = 0;
        Texture texture;
    };
    std::map<std::string, HostIcon> icons_;
    const Texture* icon_for(const ds::net::DiscoveredHost& host);

public:
    // Where this screen's prompts ended up, for a finger. See
    // guide_button_at() in control_guide.hpp.
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }
};

}  // namespace ds
