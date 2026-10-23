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

// The console buttons -- ExternalButton bits -- that `controls` (PadControl
// bits) press on `controller` under `mapping`, at `ms` milliseconds for a
// Turbo's beat -- a Wii Remote held sideways, its D-Pad turned.
uint32_t wii_u_buttons(uint32_t controls, const WiiUMapping& mapping, WiiUController controller, uint32_t ms);

// What the picker offers for `controller`, in its order: its buttons, then
// Disabled.
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
