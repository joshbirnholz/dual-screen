#pragma once

#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "render/button_glyphs.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"
#include "update/updater.hpp"

namespace ds {

// What the shell does once an update prompt has been answered.
struct UpdatePromptResult {
    // Dualscreen was replaced: the app shuts down and starts again.
    bool restart = false;
    // Cemu was installed where Dualscreen keeps it -- the shell makes it the
    // Wii U program if none was set.
    bool cemu_installed = false;
    // Every component installed: Cemu, or a core, which the shell makes the
    // one its console is played with.
    std::vector<update::Component> installed;
};

// The popups that go with the updater (update/updater.hpp): a newer build
// offered, how far its download has got, and how it went. Shared by every
// shell that has an updater -- the desktop app, and the companion's menu on a
// Switch or an Android device -- which put it up from two places: their main
// menu, when the check made at start-up finds something, and Settings'
// Updates row, which checks again.
class UpdatePrompt {
public:
    // Null is a device with no updater, on which nothing here ever opens.
    void set_updater(update::Updater* updater) { updater_ = updater; }
    // Where "Not Now" on an offer made at start-up is remembered, so the
    // same build is not offered again at every start -- which on a Switch,
    // whose menu starts again after every game, is after every game. Settings
    // still offers it; a newer build is offered at start-up as usual.
    void set_declined_file(std::string path) { declined_file_ = std::move(path); }
    bool available() const { return updater_ != nullptr; }

    // The check made at start-up, whose answer is offered the first time
    // the shell is somewhere it can be -- see poll().
    void check_at_start();
    // Settings' Updates row was chosen: checks again, and says what it
    // finds -- or, while one is already found, offers it. With `only`, the
    // Emulators page's row for that one component instead -- Cemu's.
    void choose(std::optional<update::Component> only = std::nullopt);
    // What a component's row says: its build, "Not installed", or what the
    // updater is doing with it.
    std::string component_value(update::Component id) const;

    // Every frame. `may_offer` is whether the shell is somewhere a newer
    // build found at start-up can be offered -- its main menu -- rather than
    // over a game.
    UpdatePromptResult poll(bool may_offer);

    bool open() const { return popup_ != Popup::None; }
    // While open, takes every press.
    UpdatePromptResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                              float output_width, float output_height);
    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                float output_height);
    void describe_for_sound(UiSoundView& view) const;

    // What Settings' Updates row says: which build this is, or what the
    // updater is doing.
    std::string row_value() const;

private:
    enum class Popup { None, Offer, Progress, Notice };

    void show_offer(const update::Status& status);
    void show_notice(std::string message, std::vector<std::string> lines = {});
    std::vector<PopupButton> buttons() const;
    PopupText words() const;
    PopupGeometry geometry(float output_width, float output_height) const;

    update::Updater* updater_ = nullptr;
    Popup popup_ = Popup::None;
    PopupCursor cursor_;
    // What the open popup says, kept rather than rebuilt so the status
    // changing under it does not move its words.
    std::string message_;
    std::vector<std::string> lines_;
    // The round of the check whose answer the shell is waiting on, and
    // whether that is the start-up one, offered only when there is
    // something, or one asked for, which is answered either way.
    int waiting_round_ = 0;
    bool waiting_asked_ = false;
    std::optional<update::Component> waiting_only_;
    bool offered_at_start_ = false;
    // Whether the offer up is the start-up one, and what it offered.
    bool offer_from_start_ = false;
    std::string offer_key_;
    std::string declined_file_;
    // The round of the install the progress popup is showing, whether Cemu
    // is part of it, and whether the restart it ended in has been passed on.
    int install_round_ = 0;
    std::vector<update::Offer> installing_;
    bool restart_reported_ = false;
    // The status as of the last poll, for drawing.
    update::Status status_;
};

// What a build is called to the player: "build 15", or "a local build".
std::string describe_build(int build);

}  // namespace ds
