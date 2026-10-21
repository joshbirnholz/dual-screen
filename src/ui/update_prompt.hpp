#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "net/protocol.hpp"
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
    // On a companion: what to tell the host it asked -- see host_requests().
    std::optional<net::UpdateAnswerMessage> answer_host;
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
    // Whether a game is being played as an offer is made, whether the
    // restart that ends an update of Dualscreen starts it again -- as it does
    // for a game started from a shortcut, whose command line names it -- and
    // whether closing it keeps the player's progress, as Resume from Save
    // State does. The offer says which, and an update that will close the
    // game is asked about again, as Quit is.
    void set_game_playing(bool playing, bool comes_back, bool progress_kept) {
        game_playing_ = playing;
        game_comes_back_ = comes_back;
        game_progress_kept_ = progress_kept;
    }

    // The device connected to this host, by its name -- empty for none --
    // and how to ask it. Settings' Updates row asks it to check too, and
    // the offer waits for its answer, so that an update of its own is
    // offered beside this one's and installed there, as its own Settings
    // would. See net::UpdateRequestMessage.
    void set_peer_asker(std::function<void(net::UpdateRequest)> ask) { peer_ask_ = std::move(ask); }
    void set_peer_name(const std::string& name);
    // What it said.
    void peer_answered(const net::UpdateAnswerMessage& answer);
    // On a companion, the host asking: a check is answered through poll(),
    // as it starts and as it ends; an install shows how it goes here, as
    // Settings' Updates row would.
    void host_requests(net::UpdateRequest request);

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
    enum class Popup { None, Offer, CloseGame, Progress, Notice };

    // `own_failed`: this device could not check, and only the peer's update
    // is offered.
    void show_offer(const update::Status& status, bool own_failed = false);
    void show_notice(std::string message, std::vector<std::string> lines = {});
    // Whether installing what is offered restarts Dualscreen, and so closes
    // the game being played.
    bool install_closes_game() const;
    void start_install();
    // Whether the offer is still waiting on the peer's answer: briefly for
    // it to say it is checking, and longer once it has.
    bool waiting_on_peer() const;
    // Tells the peer to install what it found.
    void install_on_peer();
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
    bool game_playing_ = false;
    bool game_comes_back_ = false;
    bool game_progress_kept_ = false;
    // The round of the install the progress popup is showing, whether Cemu
    // is part of it, and whether the restart it ended in has been passed on.
    int install_round_ = 0;
    std::vector<update::Offer> installing_;
    bool restart_reported_ = false;
    // The device connected to this host, and what it said of its own
    // update, asked when Settings' Updates row was.
    enum class PeerState { None, Asked, Checking, Answered };
    std::string peer_name_;
    std::function<void(net::UpdateRequest)> peer_ask_;
    PeerState peer_state_ = PeerState::None;
    std::chrono::steady_clock::time_point peer_asked_at_;
    net::UpdateAnswerMessage peer_answer_;
    // What the open offer is of: this device's update, the peer's, or both
    // -- and, once one is chosen with the game's closing to ask about,
    // whether the peer's goes with it.
    bool offer_own_ = true;
    bool offer_peer_ = false;
    net::UpdateAnswerMessage offer_peer_answer_;
    bool peer_too_ = false;
    // On a companion: the check the host asked for, and the answer to give
    // it at the next poll.
    int host_round_ = 0;
    std::optional<net::UpdateAnswerMessage> host_answer_;
    // The status as of the last poll, for drawing.
    update::Status status_;
};

// What a build is called to the player: "build 15", or "a local build".
std::string describe_build(int build);

}  // namespace ds
