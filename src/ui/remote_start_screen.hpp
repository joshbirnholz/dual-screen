#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "input/input_manager.hpp"
#include "launch/game_catalog.hpp"
#include "net/launch_service.hpp"
#include "render/button_glyphs.hpp"
#include "render/text_renderer.hpp"
#include "render/texture.hpp"
#include "ui/control_guide.hpp"
#include "ui/list_touch.hpp"
#include "ui/menu_page.hpp"
#include "ui/popup.hpp"
#include "ui/sound_view.hpp"
#include "ui/text_entry.hpp"

namespace ds {

struct RemoteStartResult {
    // B: back to the host picker.
    bool back = false;
    // A game was started over there: the picker joins it once it shows up.
    bool started = false;
    std::string started_title;
    // A token was given, by pairing, or the one held was refused: the caller
    // keeps it (empty to forget it) under the computer's id.
    bool token_changed = false;
    std::string token;
    // Y switched between the grid and the list: the caller keeps which.
    bool view_changed = false;
};

// From the companion's host picker, a computer running Dualscreen's
// background service (see net/launch_service.hpp): the games it has, one
// chosen to start over there. A device not yet paired with it is asked for
// the code its Settings show under Pair a Device first.
//
// Everything asked over the network is asked on a thread of its own, so the
// page never stops drawing while the computer answers.
class RemoteStartScreen : public MenuPage {
public:
    ~RemoteStartScreen() override;

    // `token` is what this device was given when it last paired there, or
    // empty, in which case it asks for a code straight away.
    void open(const net::LaunchHost& host, std::string token, std::string device_name);
    void close();
    bool is_open() const { return open_; }
    const net::LaunchHost& host() const { return host_; }
    // The games as a grid of their icons, as a Wii U GamePad's Quick Start
    // shows them, or as a list of titles. Y switches. Set before open().
    void set_grid(bool grid) { grid_ = grid; }
    bool grid() const { return grid_; }
    // What a press here could have changed -- see UiSoundJudge.
    void describe_for_sound(UiSoundView& view) const;

    // The code being typed: the keyboard types into it -- see
    // InputManager::set_text_entry() -- and what it typed comes in through
    // type(), before update().
    bool wants_text() const { return code_entry_.is_open() && !text_editor_; }
    void type(const std::string& typed, int backspaces) {
        typed_ = typed;
        backspaces_ = backspaces;
    }
    // A platform's own keyboard -- a Switch's -- in place of the popup.
    void set_text_editor(std::function<bool(const std::string& header, const std::string& initial,
                                            std::size_t max, std::string& out)>
                             editor) {
        text_editor_ = std::move(editor);
    }

    RemoteStartResult update(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers,
                             float output_width, float output_height);

    bool scroll(const std::vector<PointerEvent>& pointers, float output_width, float output_height) override;
    // A game by its index. A popup takes its own taps, and the prompts are
    // pressed through take_guide_taps().
    int entry_at(float x, float y, float output_width, float output_height) const override;
    bool off_popup(float, float, float, float) const override { return false; }
    std::vector<float> scroll_state(float output_width, float output_height) const override;
    void set_scroll_state(const std::vector<float>& state, float output_width, float output_height) override;

    void render(QuadRenderer& quads, TextRenderer& text, const ButtonGlyphSheet& glyphs, float output_width,
                float output_height);

    int focus() const { return focus_; }
    const std::vector<GuideHit>& guide_hits() const { return guide_hits_; }

    // What it says each game is, at the right of its row.
    static std::string system_label(const std::string& system);

private:
    enum class Phase {
        // Asking for the list, or for a game to start, or pairing.
        Waiting,
        List,
        // The code popup is up.
        Code,
        // Something went wrong: an alert says what, and OK goes on to
        // `after_alert_`.
        Alert,
    };

    // What a job on the network thread came back with.
    struct Outcome {
        enum class Kind { Listed, Paired, Started } kind = Kind::Listed;
        bool ok = false;
        bool unpaired = false;
        std::string error;
        std::vector<LaunchableGame> games;
        std::string token;
        std::string title;
    };
    // Shared with its thread, which is let go rather than waited for: a
    // page put away while a computer is slow to answer goes at once, and the
    // answer, when it comes, is dropped.
    struct Job {
        std::atomic<bool> done{false};
        Outcome outcome;
    };

    // The icons, fetched one after another on a thread of their own once
    // the list is in, each drawn as soon as it lands.
    struct Icons {
        std::mutex mutex;
        std::atomic<bool> cancelled{false};
        struct Icon {
            bool done = false;
            int width = 0;
            int height = 0;
            std::vector<uint8_t> rgba;
        };
        std::vector<Icon> icons;
    };
    void fetch_icons();
    const Texture* icon_texture(int index);

    struct Geometry {
        PixelRect list;
        float row_height = 0.0f;
        float scale = 1.0f;
        // The grid: how many to a row, a tile's side, and where the first
        // column starts so the grid sits in the middle.
        int columns = 1;
        float tile = 0.0f;
        float gap = 0.0f;
        float left = 0.0f;
        PixelRect tile_at(int index, float scroll) const {
            const int column = index % std::max(1, columns);
            const int row = index / std::max(1, columns);
            return PixelRect{left + column * (tile + gap), list.y + gap * 0.5f + row * row_height - scroll, tile, tile};
        }
    };
    Geometry compute_geometry(float output_width, float output_height) const;
    void render_grid(QuadRenderer& quads, TextRenderer& text, const Geometry& geometry, bool cursor);
    float max_scroll(const Geometry& geometry) const;
    void scroll_focus_into_view(const Geometry& geometry);

    void run(std::function<Outcome()> work, std::string waiting_for);
    void finish_job();
    void list_games();
    void ask_for_code();
    // `leaves`: OK goes back to the host picker rather than to `after`.
    void alert(std::string message, Phase after, bool leaves = false);
    void handle_outcome(const Outcome& outcome, RemoteStartResult& result);
    void start_game(int index);

    bool open_ = false;
    bool grid_ = true;
    std::shared_ptr<Icons> icons_;
    std::map<int, Texture> textures_;
    net::LaunchHost host_;
    std::string token_;
    std::string device_name_;
    Phase phase_ = Phase::Waiting;
    Phase after_alert_ = Phase::List;
    bool alert_leaves_ = false;
    std::string waiting_for_;
    std::string alert_;
    PopupCursor alert_cursor_;
    std::shared_ptr<Job> job_;
    std::vector<LaunchableGame> games_;
    bool listed_ = false;
    int focus_ = 0;
    float scroll_ = 0.0f;
    ListDrag drag_;
    ScrollEase scroll_ease_;
    std::vector<GuideHit> guide_hits_;

    TextEntryPopup code_entry_;
    std::string typed_;
    int backspaces_ = 0;
    std::function<bool(const std::string& header, const std::string& initial, std::size_t max,
                       std::string& out)>
        text_editor_;
    // Set when the platform's keyboard should come up on the next update:
    // never from inside the pass that put the page up, which is still
    // drawing the last screen.
    bool editor_pending_ = false;
};

}  // namespace ds
