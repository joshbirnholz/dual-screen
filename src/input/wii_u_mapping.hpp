#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "input/button_mapping.hpp"

namespace ds {

// A Wii U game's controls, one mapping for each controller it has: what each
// of a pad's buttons presses on the GamePad, a Wii U Pro Controller, a Wii
// Remote -- with a Nunchuk or a Classic Controller plugged into it, or alone.
// Applied here, on the device running the game, to the buttons a pad has under
// Steam Input; Cemu is handed the controller's own buttons and maps nothing.
// See ExternalButton for how they cross.

// What a game has a controller as.
enum class WiiUController : uint8_t {
    GamePad,
    Pro,
    Remote,
    RemoteNunchuk,
    // A Classic Controller is always plugged into a Wii Remote, whose own
    // buttons a game can read as well -- see offered_type().
    RemoteClassic,
};

// Where a game config keeps the mapping of the GamePad (`player` -1) or a
// player as `controller`: "gamepad", "player1.remote" -- a player's own for
// each thing it is, so one changed and back keeps what it had.
std::string wii_u_mapping_key(int player, WiiUController controller);

// A key kept for a player -- a mapping's, "player3.pro", or Cemu's option
// for its type, "player3_type" -- once player `removed` (from 0) is gone and
// every player after it has moved one place up: the same for any other
// player's or for none, empty for the one removed.
std::string player_key_after_removing(const std::string& key, int removed);

// What a player Cemu has as `type` -- its name for it, or the menus' --
// is.
WiiUController wii_u_controller_for(const std::string& type);
// As a game config keeps it: "gamepad", "pro", "remote", "nunchuk",
// "classic".
const char* wii_u_controller_key(WiiUController controller);
// As the menus name it: "Wii U Pro Controller", "Wii Remote + Nunchuk".
const char* wii_u_controller_name(WiiUController controller);

// What one of a pad's buttons presses. The console's buttons are numbered as
// ExternalButton is, one on; saved by key, so the numbers are free to change
// but for that.
enum class WiiUAction : uint8_t {
    // Whatever the controller's standard mapping gives it -- see
    // standard_wii_u_action().
    Standard = 0,
    A, B, X, Y,
    L, R, Zl, Zr,
    Plus, Minus,
    Up, Down, Left, Right,
    LeftStick, RightStick,
    Home,
    One, Two,
    NunchukC, NunchukZ,
    RemoteA, RemoteB,
    RemotePlus, RemoteMinus, RemoteHome,
    RemoteUp, RemoteDown, RemoteLeft, RemoteRight,
    // Presses nothing.
    Nothing,
    // Another of the controller's buttons pressed over and over while this
    // is held -- which, WiiUMapping::turbo_of says.
    Turbo,
    // This app's own, which no controller's standard mapping has: the quick
    // menu opened; the screens swapped, as the menu's Swap Screens does; and
    // the other screen shown while it is held -- see wii_u_function_held().
    OpenMenu,
    SwapScreens,
    Peek,
    Count,
};

// Whether `action` is one of the console's buttons, and which, as an
// ExternalButton's number.
bool wii_u_action_button(WiiUAction action, unsigned& button);

// The standard mapping: a pad's buttons where a Nintendo pad has them -- the
// GamePad's and a Pro Controller's straight across, a Classic Controller's
// too less the stick presses it lacks. A Wii Remote is held upright: A and B
// as they are, 1 on X and 2 on Y -- 1 above 2, as they sit on the remote --
// Plus and Minus, and the d-pad; a Nunchuk's C on L and Z on ZL. Home is on
// nothing: a pad's own is Steam's.
WiiUAction standard_wii_u_action(PadControl control, WiiUController controller);

// One controller's mapping: Standard for each button never changed.
struct WiiUMapping {
    std::array<WiiUAction, kPadControls> actions{};
    // For a button that is Turbo, the console button it presses; Standard
    // while none is chosen, which presses nothing.
    std::array<WiiUAction, kPadControls> turbo_of{};
    // A Wii Remote alone held on its side, as an NES controller is -- the
    // D-Pad on the left, 1 and 2 on the right: its D-Pad's directions as the
    // player holds it, so a button on Up presses the remote's Right, which
    // points up held so. What each button is chosen as is the same either way.
    bool sideways = false;

    WiiUAction& operator[](PadControl control) { return actions[static_cast<std::size_t>(control)]; }
    WiiUAction operator[](PadControl control) const { return actions[static_cast<std::size_t>(control)]; }
    WiiUAction& turbo(PadControl control) { return turbo_of[static_cast<std::size_t>(control)]; }
    WiiUAction turbo(PadControl control) const { return turbo_of[static_cast<std::size_t>(control)]; }
    // What `control` does, Standard looked up.
    WiiUAction resolved(PadControl control, WiiUController controller) const;
    // Whether every button does what the standard mapping has it do, held
    // sideways or not.
    bool is_standard(WiiUController controller) const;

    bool operator==(const WiiUMapping& other) const {
        return actions == other.actions && turbo_of == other.turbo_of && sideways == other.sideways;
    }
    bool operator!=(const WiiUMapping& other) const { return !(*this == other); }
};

// Profile Switching, on a player's page: - or + held moves the player on to
// the next of `profiles` -- by name, in the order they were saved -- after
// the one it has now, round from the last to the first. Kept with the game,
// apart from any mapping, for each player as each controller.
struct ProfileSwitching {
    // Minus or Plus; Count for Off.
    PadControl button = PadControl::Count;
    std::vector<std::string> profiles;

    bool on() const { return button == PadControl::Minus || button == PadControl::Plus; }
    bool includes(const std::string& name) const;
    // Which button switches, as PadControl bits: none while Off or with no
    // profile to switch to.
    uint32_t hold_controls() const;
    bool operator==(const ProfileSwitching& other) const {
        return button == other.button && profiles == other.profiles;
    }
};

// The profile Profile Switching moves on to from `current` (empty for none):
// the next of `saved` -- every profile there is for the controller, in order
// -- that `switching` includes, round from the last to the first. Empty if
// none is.
std::string next_switching_profile(const ProfileSwitching& switching, const std::vector<std::string>& saved,
                                   const std::string& current);

// A controller's buttons that switch profile when held, as Steam Input's long
// press is: each one's press held back from the game until it is let go --
// a tap reaching it then, as a press kTapMs long -- or held kHoldMs, when it
// switches instead and the game never sees it. One for each controller.
class ProfileHold {
public:
    static constexpr uint32_t kHoldMs = 500;
    static constexpr uint32_t kTapMs = 100;
    // `controls`, the PadControl bits held now, and `holding`, which of them
    // switch: what the game is to see instead. `switched` is the control
    // held long enough just now, or -1. Not called for a while -- a menu
    // up -- it starts afresh.
    uint32_t update(uint32_t controls, uint32_t holding, uint32_t now_ms, int& switched);

private:
    // Down and held back; of those, switched already; tapped, passed on
    // until their time.
    uint32_t down_ = 0;
    uint32_t switched_ = 0;
    uint32_t tapping_ = 0;
    std::array<uint32_t, kPadControls> since_{};
    std::array<uint32_t, kPadControls> tap_until_{};
    uint32_t last_ms_ = 0;
    bool started_ = false;
};

// The console buttons -- ExternalButton bits -- that `controls` (PadControl
// bits) press on `controller` under `mapping`, at `ms` milliseconds for a
// Turbo's beat -- a Wii Remote held sideways, its D-Pad turned.
uint32_t wii_u_buttons(uint32_t controls, const WiiUMapping& mapping, WiiUController controller, uint32_t ms);

// Whether `controls` (PadControl bits) hold `function` -- OpenMenu,
// SwapScreens or Peek -- under `mapping`.
bool wii_u_function_held(uint32_t controls, const WiiUMapping& mapping, WiiUController controller,
                         WiiUAction function);

// What the picker offers for `controller`, in its order: its buttons, this
// app's own, then Disabled. Not HOME, which the menu and the game's own
// HOME menu are reached by on a pad.
std::vector<WiiUAction> wii_u_action_choices(WiiUController controller);
// What Turbo can press: the controller's buttons.
std::vector<WiiUAction> wii_u_turbo_choices(WiiUController controller);

// As the picker lists it, for `controller`: a Wii Remote's own are only
// "Wii Remote ..." beside a Classic Controller's.
std::string wii_u_action_name(WiiUAction action, WiiUController controller);
// Round-trips through a game's config file; unknown text is Standard.
const char* wii_u_action_key(WiiUAction action);
WiiUAction wii_u_action_from_key(const std::string& key);

// The PadControl bits a pad's DsButton bits mean under the standard mapping a
// Wii U game's pad has -- for a companion too old to say which it pressed.
uint32_t pad_controls_from_ds_buttons(uint32_t buttons);

}  // namespace ds
