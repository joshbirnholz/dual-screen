#pragma once

#include <string>
#include <vector>

#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"

namespace ds::sw {

// The in-session menu, as a list of rows rather than as pixels.
//
// A player holding the companion is holding the controller: reaching for a
// second pad plugged into the machine across the room to change the layout is
// not a thing anyone should have to do. So this offers what the desktop's own
// quick menu offers, and asks the host for all of it the same way -- the host
// owns every one of these, applies it, and broadcasts the result, which is
// what keeps the two devices from ever disagreeing.
//
// The rows are built from the session's current state, so an entry that means
// nothing right now is simply not there: no render scale on a console whose
// core draws at one size, no gap on a layout whose gap is not a choice.
struct SessionMenuState {
    std::string layout_id;
    bool filtering = false;
    bool screen_gap = true;
    bool screen_gap_toggleable = false;
    int render_scale = 1;
    int max_render_scale = 1;
    RightStickMode right_stick_mode = RightStickMode::CStick;
    bool right_stick_choosable = false;
    Handedness handedness = Handedness::Right;
    AudioTarget audio_target = AudioTarget::Both;
};

// What one press asked for. Everything that names a value is a request: this
// device does not apply any of it, it asks and then follows what comes back.
struct SessionMenuResult {
    bool resume = false;
    bool reset = false;
    bool disconnect = false;
    bool quit = false;

    bool wants_layout = false;
    std::string layout_id;
    bool wants_filtering = false;
    bool filtering = false;
    bool wants_screen_gap = false;
    bool screen_gap = true;
    bool wants_render_scale = false;
    int render_scale = 1;
    bool wants_right_stick_mode = false;
    RightStickMode right_stick_mode = RightStickMode::CStick;
    bool wants_handedness = false;
    Handedness handedness = Handedness::Right;
    bool wants_audio_target = false;
    AudioTarget audio_target = AudioTarget::Both;
};

class SessionMenu {
public:
    explicit SessionMenu(const LayoutRegistry& registry) : registry_(&registry) {}

    void open(const SessionMenuState& state);
    bool is_open() const { return open_; }
    void close() { open_ = false; }

    // Kept in step while the menu is up: every one of these belongs to the
    // host, which can change any of them from its own end at any time.
    void set_state(const SessionMenuState& state);

    SessionMenuResult update(const std::vector<UiAction>& actions);

    // What to draw, top to bottom, and which row the focus is on.
    const std::vector<std::string>& rows() const { return rows_; }
    int focus() const { return focus_; }

private:
    // The rows this menu can carry, in the order they appear.
    enum class Row {
        Layout,
        Smoothing,
        RenderScale,
        RightStick,
        Hands,
        ScreenGap,
        Sound,
        Reset,
        Resume,
        Disconnect,
        Quit,
    };

    void rebuild();
    std::string label_for(Row row) const;
    // `step` is +1 or -1 for the rows that cycle through values, and 0 for a
    // plain confirm.
    void activate(int step, SessionMenuResult& result);

    const LayoutRegistry* registry_;
    bool open_ = false;
    int focus_ = 0;
    SessionMenuState state_;
    std::vector<Row> layout_rows_;
    std::vector<std::string> rows_;
};

}  // namespace ds::sw
