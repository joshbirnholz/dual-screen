#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "launch/game_catalog.hpp"
#include "net/launch_service.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/control_guide.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"

namespace ds {

struct RemoteStartResult {
    // B, or OK on an alert: back to the first screen.
    bool back = false;
    // The game was started over there: the caller joins it once it shows up,
    // the page still up -- see connecting() and close().
    bool started = false;
    std::string started_title;
    // The computer no longer knows this device's token: the caller forgets it.
    bool token_refused = false;
    // The computer answered, as host() now has it: the caller remembers it,
    // to wake it another time -- see net::remember_computer().
    bool answered = false;
};

// A game chosen from the first screen's recently played, being started on
// the computer it was played on -- see net/launch_service.hpp -- shown as
// one page from the choice to the game: its icon and "Starting <title>..."
// in the middle, with what is happening under it and a spinner, through
// waking that computer when it is asleep, starting the game, waiting for it
// to come up and joining it. B leaves at any point; an alert says when the
// computer cannot be woken or will not start it. Nothing else is asked here;
// a device is let start games on a computer by having joined a game there
// -- see RemoteStartGrant.
//
// Everything asked over the network is asked on a thread of its own, so the
// page never stops drawing while the computer answers.
class RemoteStartScreen : public MenuPage {
public:
    ~RemoteStartScreen() override;

    // `host.asleep`: woken first, then asked to start `game`. `icon_rgba`
    // is the game's icon, `icon_width` by `icon_height`, or empty.
    void open_and_start(const net::LaunchHost& host, std::string token, const LaunchableGame& game,
                        int icon_width = 0, int icon_height = 0, std::vector<uint8_t> icon_rgba = {});
    // Started, and now being joined: said under the title until close().
    void connecting();
    // Back to waiting for the game, after a join that did not take -- one
    // still loading, say.
    void awaiting();
    // It never came up, or the joining failed: an alert says so, and OK
    // leaves.
    void fail(std::string message);
    void close();
    // Waiting on the game to come up after starting it -- see
    // RemoteStartResult::started -- for the caller to join it.
    bool awaiting_game() const { return open_ && step_ == Step::Awaiting && alert_.empty(); }
    bool is_open() const { return open_; }
    const net::LaunchHost& host() const { return host_; }
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;

    RemoteStartResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                             float output_width, float output_height);

    bool scroll(const std::vector<PointerEvent>&, float, float) override { return false; }
    // The alert's taps are its own, and the prompts are pressed through
    // take_guide_taps(): nothing else to land on.
    int entry_at(float, float, float, float) const override { return -1; }
    bool off_popup(float, float, float, float) const override { return false; }
    // Nothing scrolls.
    std::vector<float> scroll_state(float, float) const override { return {}; }
    void set_scroll_state(const std::vector<float>&, float, float) override {}

    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                float output_height);

    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }

    // What it says each game is.
    static std::string system_label(const std::string& system);

private:
    // What a job on the network thread came back with.
    struct Outcome {
        enum class Kind { Started, Woken } kind = Kind::Started;
        net::LaunchHost woken;
        bool ok = false;
        bool unpaired = false;
        // Started: that game running there already, or another one, by
        // its title -- see net::StartAnswer.
        bool already = false;
        std::string running;
        std::string error;
    };
    // Shared with its thread, which is let go rather than waited for: a
    // page put away while a computer is slow to answer goes at once, and the
    // answer, when it comes, is dropped.
    struct Job {
        std::atomic<bool> done{false};
        // Set when the page is put away, for a job that goes on a while --
        // waking a computer -- to stop.
        std::atomic<bool> cancelled{false};
        Outcome outcome;
    };

    void run(std::function<Outcome(const std::atomic<bool>& cancelled)> work, std::string waiting_for);
    void drop_job();
    void wake();
    // `replace`: the game running there closed first.
    void start(bool replace = false);
    void alert(std::string message);
    void handle_outcome(const Outcome& outcome, RemoteStartResult& result);
    std::string status() const;

    // Where it has got to, said under the title.
    enum class Step { Waking, Starting, Closing, Awaiting, Connecting };
    Step step_ = Step::Starting;
    bool open_ = false;
    int icon_width_ = 0;
    int icon_height_ = 0;
    std::vector<uint8_t> icon_rgba_;
    Texture icon_;
    std::chrono::steady_clock::time_point opened_at_{};
    net::LaunchHost host_;
    std::string token_;
    LaunchableGame game_;
    std::string waiting_for_;
    // Set when something went wrong: an alert says what, and OK leaves.
    // Or, with `asking_close_`, another game is running there: Cancel
    // leaves, and Close It closes that one and starts this one.
    std::string alert_;
    bool asking_close_ = false;
    std::string running_title_;
    PopupCursor alert_cursor_;
    std::shared_ptr<Job> job_;
    std::vector<GuideHit> guide_hits_;
};

}  // namespace ds
