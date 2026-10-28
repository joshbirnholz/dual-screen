#include "input/wii_u_mapping.hpp"

#include <algorithm>

#include "core/external_frame_link.hpp"
#include "input/ds_input.hpp"

namespace ds {

static_assert(static_cast<unsigned>(WiiUAction::RemoteRight) - 1 ==
                  static_cast<unsigned>(ExternalButton::RemoteRight),
              "the console's buttons are ExternalButton's, one on");

std::string player_key_after_removing(const std::string& key, int removed) {
    static const std::string kPlayer = "player";
    if (key.compare(0, kPlayer.size(), kPlayer) != 0) return key;
    std::size_t end = kPlayer.size();
    while (end < key.size() && key[end] >= '0' && key[end] <= '9') ++end;
    if (end == kPlayer.size()) return key;
    const int player = std::stoi(key.substr(kPlayer.size(), end - kPlayer.size())) - 1;
    if (player < removed) return key;
    if (player == removed) return std::string();
    return kPlayer + std::to_string(player) + key.substr(end);
}

WiiUController wii_u_controller_for(const std::string& type) {
    if (type == "Wii U GamePad") return WiiUController::GamePad;
    if (type == "Wii Remote") return WiiUController::Remote;
    if (type == "Wii Remote + Nunchuk") return WiiUController::RemoteNunchuk;
    if (type == "Wii Remote + Classic Controller" || type == "Wii Classic Controller" ||
        type == "Classic Controller") {
        return WiiUController::RemoteClassic;
    }
    return WiiUController::Pro;
}

std::string wii_u_mapping_key(int player, WiiUController controller) {
    if (player < 0 || controller == WiiUController::GamePad) return "gamepad";
    return "player" + std::to_string(player + 1) + "." + wii_u_controller_key(controller);
}

const char* wii_u_controller_key(WiiUController controller) {
    switch (controller) {
        case WiiUController::GamePad: return "gamepad";
        case WiiUController::Pro: return "pro";
        case WiiUController::Remote: return "remote";
        case WiiUController::RemoteNunchuk: return "nunchuk";
        case WiiUController::RemoteClassic: return "classic";
    }
    return "pro";
}

const char* wii_u_controller_name(WiiUController controller) {
    switch (controller) {
        case WiiUController::GamePad: return "Wii U GamePad";
        case WiiUController::Pro: return "Wii U Pro Controller";
        case WiiUController::Remote: return "Wii Remote";
        case WiiUController::RemoteNunchuk: return "Wii Remote + Nunchuk";
        case WiiUController::RemoteClassic: return "Wii Classic Controller";
    }
    return "Wii U Pro Controller";
}

bool wii_u_action_button(WiiUAction action, unsigned& button) {
    if (action == WiiUAction::Standard || action >= WiiUAction::Nothing) return false;
    button = static_cast<unsigned>(action) - 1;
    return true;
}

namespace {

bool is_remote(WiiUController controller) {
    return controller == WiiUController::Remote || controller == WiiUController::RemoteNunchuk ||
           controller == WiiUController::RemoteClassic;
}

}  // namespace

WiiUAction standard_wii_u_action(PadControl control, WiiUController controller) {
    if (controller == WiiUController::Remote || controller == WiiUController::RemoteNunchuk) {
        const bool nunchuk = controller == WiiUController::RemoteNunchuk;
        switch (control) {
            case PadControl::A: return WiiUAction::RemoteA;
            case PadControl::B: return WiiUAction::RemoteB;
            case PadControl::X: return WiiUAction::One;
            case PadControl::Y: return WiiUAction::Two;
            case PadControl::Plus: return WiiUAction::RemotePlus;
            case PadControl::Minus: return WiiUAction::RemoteMinus;
            case PadControl::Up: return WiiUAction::RemoteUp;
            case PadControl::Down: return WiiUAction::RemoteDown;
            case PadControl::Left: return WiiUAction::RemoteLeft;
            case PadControl::Right: return WiiUAction::RemoteRight;
            case PadControl::L: return nunchuk ? WiiUAction::NunchukC : WiiUAction::Nothing;
            case PadControl::Zl: return nunchuk ? WiiUAction::NunchukZ : WiiUAction::Nothing;
            default: return WiiUAction::Nothing;
        }
    }
    switch (control) {
        case PadControl::A: return WiiUAction::A;
        case PadControl::B: return WiiUAction::B;
        case PadControl::X: return WiiUAction::X;
        case PadControl::Y: return WiiUAction::Y;
        case PadControl::L: return WiiUAction::L;
        case PadControl::R: return WiiUAction::R;
        case PadControl::Zl: return WiiUAction::Zl;
        case PadControl::Zr: return WiiUAction::Zr;
        case PadControl::Plus: return WiiUAction::Plus;
        case PadControl::Minus: return WiiUAction::Minus;
        case PadControl::Up: return WiiUAction::Up;
        case PadControl::Down: return WiiUAction::Down;
        case PadControl::Left: return WiiUAction::Left;
        case PadControl::Right: return WiiUAction::Right;
        case PadControl::LeftStick:
            return controller == WiiUController::RemoteClassic ? WiiUAction::Nothing : WiiUAction::LeftStick;
        case PadControl::RightStick:
            return controller == WiiUController::RemoteClassic ? WiiUAction::Nothing : WiiUAction::RightStick;
        case PadControl::Count: break;
    }
    return WiiUAction::Nothing;
}

WiiUAction WiiUMapping::resolved(PadControl control, WiiUController controller) const {
    const WiiUAction action = (*this)[control];
    return action == WiiUAction::Standard ? standard_wii_u_action(control, controller) : action;
}

bool ProfileSwitching::includes(const std::string& name) const {
    return std::find(profiles.begin(), profiles.end(), name) != profiles.end();
}

uint32_t ProfileSwitching::hold_controls() const {
    if (!on() || profiles.empty()) return 0;
    return 1u << static_cast<unsigned>(button);
}

std::string next_switching_profile(const ProfileSwitching& switching, const std::vector<std::string>& saved,
                                   const std::string& current) {
    const auto now = std::find(saved.begin(), saved.end(), current);
    const std::size_t count = saved.size();
    // From the one after the current -- or, with none, from the first.
    const std::size_t from = now == saved.end() ? 0 : static_cast<std::size_t>(now - saved.begin()) + 1;
    for (std::size_t step = 0; step < count; ++step) {
        const std::string& name = saved[(from + step) % count];
        if (switching.includes(name) && name != current) return name;
    }
    return std::string();
}

uint32_t ProfileHold::update(uint32_t controls, uint32_t holding, uint32_t now_ms, int& switched) {
    switched = -1;
    // Away a while -- the game paused under a menu -- nothing is still down.
    if (!started_ || now_ms - last_ms_ > 250) {
        down_ = switched_ = tapping_ = 0;
        started_ = true;
    }
    last_ms_ = now_ms;
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const uint32_t bit = 1u << index;
        const bool pressed = (controls & bit) != 0;
        if ((down_ & bit) == 0) {
            if (pressed && (holding & bit) != 0) {
                down_ |= bit;
                since_[index] = now_ms;
            }
            continue;
        }
        if (pressed) {
            if ((switched_ & bit) == 0 && now_ms - since_[index] >= kHoldMs) {
                switched_ |= bit;
                if (switched < 0) switched = static_cast<int>(index);
            }
            continue;
        }
        // Let go: a tap is passed on now.
        if ((switched_ & bit) == 0) {
            tapping_ |= bit;
            tap_until_[index] = now_ms + kTapMs;
        }
        down_ &= ~bit;
        switched_ &= ~bit;
    }
    uint32_t taps = 0;
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const uint32_t bit = 1u << index;
        if ((tapping_ & bit) == 0) continue;
        if (static_cast<int32_t>(now_ms - tap_until_[index]) >= 0) {
            tapping_ &= ~bit;
        } else {
            taps |= bit;
        }
    }
    return (controls & ~down_) | taps;
}

bool WiiUMapping::is_standard(WiiUController controller) const {
    for (std::size_t index = 0; index < kPadControls; ++index) {
        const PadControl control = static_cast<PadControl>(index);
        if (resolved(control, controller) != standard_wii_u_action(control, controller)) return false;
    }
    return true;
}

uint32_t wii_u_buttons(uint32_t controls, const WiiUMapping& mapping, WiiUController controller, uint32_t ms) {
    uint32_t buttons = 0;
    for (std::size_t index = 0; index < kPadControls; ++index) {
        if ((controls & (1u << index)) == 0) continue;
        const PadControl control = static_cast<PadControl>(index);
        WiiUAction action = mapping.resolved(control, controller);
        if (action == WiiUAction::Turbo) {
            if (!turbo_down_at(ms)) continue;
            action = mapping.turbo(control);
        }
        unsigned button = 0;
        if (wii_u_action_button(action, button)) buttons |= 1u << button;
    }
    if (mapping.sideways && controller == WiiUController::Remote) {
        // Turned a quarter left, its top to the left: what points up held so
        // is the remote's Right, and so round.
        const auto bit = [](WiiUAction direction) {
            unsigned button = 0;
            wii_u_action_button(direction, button);
            return 1u << button;
        };
        const uint32_t up = bit(WiiUAction::RemoteUp);
        const uint32_t down = bit(WiiUAction::RemoteDown);
        const uint32_t left = bit(WiiUAction::RemoteLeft);
        const uint32_t right = bit(WiiUAction::RemoteRight);
        const uint32_t held = buttons;
        buttons &= ~(up | down | left | right);
        if (held & up) buttons |= right;
        if (held & right) buttons |= down;
        if (held & down) buttons |= left;
        if (held & left) buttons |= up;
    }
    return buttons;
}

std::vector<WiiUAction> wii_u_turbo_choices(WiiUController controller) {
    std::vector<WiiUAction> choices;
    if (is_remote(controller)) {
        choices = {WiiUAction::RemoteA,  WiiUAction::RemoteB,    WiiUAction::One,
                   WiiUAction::Two,      WiiUAction::RemotePlus, WiiUAction::RemoteMinus,
                   WiiUAction::RemoteUp, WiiUAction::RemoteDown, WiiUAction::RemoteLeft,
                   WiiUAction::RemoteRight};
        if (controller == WiiUController::RemoteNunchuk) {
            choices.push_back(WiiUAction::NunchukC);
            choices.push_back(WiiUAction::NunchukZ);
        }
        if (controller != WiiUController::RemoteClassic) return choices;
        // A Classic Controller's first, the remote it is plugged into after.
        std::vector<WiiUAction> classic = {WiiUAction::A,    WiiUAction::B,     WiiUAction::X,  WiiUAction::Y,
                                           WiiUAction::Up,   WiiUAction::Down,  WiiUAction::Left,
                                           WiiUAction::Right, WiiUAction::L,    WiiUAction::R,  WiiUAction::Zl,
                                           WiiUAction::Zr,   WiiUAction::Plus,  WiiUAction::Minus};
        classic.insert(classic.end(), choices.begin(), choices.end());
        return classic;
    }
    return {WiiUAction::A,     WiiUAction::B,         WiiUAction::X,         WiiUAction::Y,
            WiiUAction::Up,    WiiUAction::Down,      WiiUAction::Left,      WiiUAction::Right,
            WiiUAction::L,     WiiUAction::R,         WiiUAction::Zl,        WiiUAction::Zr,
            WiiUAction::Plus,  WiiUAction::Minus,     WiiUAction::LeftStick, WiiUAction::RightStick};
}

bool wii_u_function_held(uint32_t controls, const WiiUMapping& mapping, WiiUController controller,
                         WiiUAction function) {
    for (std::size_t index = 0; index < kPadControls; ++index) {
        if ((controls & (1u << index)) == 0) continue;
        if (mapping.resolved(static_cast<PadControl>(index), controller) == function) return true;
    }
    return false;
}

std::vector<WiiUAction> wii_u_action_choices(WiiUController controller) {
    std::vector<WiiUAction> choices = wii_u_turbo_choices(controller);
    choices.push_back(WiiUAction::OpenMenu);
    choices.push_back(WiiUAction::SwapScreens);
    choices.push_back(WiiUAction::Peek);
    choices.push_back(WiiUAction::Nothing);
    return choices;
}

std::string wii_u_action_name(WiiUAction action, WiiUController controller) {
    // A Wii Remote's own, named as the remote prints them -- and only called
    // the Wii Remote's beside a Classic Controller's of the same name.
    const bool beside_classic = controller == WiiUController::RemoteClassic;
    const auto remote = [&](const char* name) {
        return beside_classic ? std::string("Wii Remote ") + name : std::string(name);
    };
    switch (action) {
        case WiiUAction::Standard: return "Standard";
        case WiiUAction::A: return "A Button";
        case WiiUAction::B: return "B Button";
        case WiiUAction::X: return "X Button";
        case WiiUAction::Y: return "Y Button";
        case WiiUAction::L: return "L Button";
        case WiiUAction::R: return "R Button";
        case WiiUAction::Zl: return "ZL Button";
        case WiiUAction::Zr: return "ZR Button";
        case WiiUAction::Plus: return "Plus Button";
        case WiiUAction::Minus: return "Minus Button";
        case WiiUAction::Up: return "Up Button";
        case WiiUAction::Down: return "Down Button";
        case WiiUAction::Left: return "Left Button";
        case WiiUAction::Right: return "Right Button";
        case WiiUAction::LeftStick: return "Left Stick Button";
        case WiiUAction::RightStick: return "Right Stick Button";
        case WiiUAction::Home: return "HOME Button";
        case WiiUAction::One: return "1 Button";
        case WiiUAction::Two: return "2 Button";
        case WiiUAction::NunchukC: return "C Button";
        case WiiUAction::NunchukZ: return "Z Button";
        case WiiUAction::RemoteA: return remote("A Button");
        case WiiUAction::RemoteB: return remote("B Button");
        case WiiUAction::RemotePlus: return remote("Plus Button");
        case WiiUAction::RemoteMinus: return remote("Minus Button");
        case WiiUAction::RemoteHome: return remote("HOME Button");
        case WiiUAction::RemoteUp: return remote("Up Button");
        case WiiUAction::RemoteDown: return remote("Down Button");
        case WiiUAction::RemoteLeft: return remote("Left Button");
        case WiiUAction::RemoteRight: return remote("Right Button");
        case WiiUAction::Nothing: return "Disabled";
        case WiiUAction::Turbo: return "Turbo";
        case WiiUAction::OpenMenu: return "Open Menu";
        case WiiUAction::SwapScreens: return "Swap Screens";
        case WiiUAction::Peek: return "Peek";
        case WiiUAction::Count: break;
    }
    return "?";
}

namespace {

constexpr const char* kActionKeys[] = {
    "standard", "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "up", "down", "left", "right",
    "left_stick", "right_stick", "home", "one", "two", "c", "z", "remote_a", "remote_b", "remote_plus",
    "remote_minus", "remote_home", "remote_up", "remote_down", "remote_left", "remote_right", "nothing", "turbo",
    "open_menu", "swap_screens", "peek",
};
static_assert(sizeof(kActionKeys) / sizeof(kActionKeys[0]) == static_cast<std::size_t>(WiiUAction::Count),
              "every action has a key");

}  // namespace

const char* wii_u_action_key(WiiUAction action) {
    const auto index = static_cast<std::size_t>(action);
    return index < static_cast<std::size_t>(WiiUAction::Count) ? kActionKeys[index] : "standard";
}

WiiUAction wii_u_action_from_key(const std::string& key) {
    for (std::size_t index = 0; index < static_cast<std::size_t>(WiiUAction::Count); ++index) {
        if (key == kActionKeys[index]) return static_cast<WiiUAction>(index);
    }
    return WiiUAction::Standard;
}

uint32_t pad_controls_from_ds_buttons(uint32_t buttons) {
    static constexpr std::pair<DsButton, PadControl> kPairs[] = {
        {DsButton::A, PadControl::A},          {DsButton::B, PadControl::B},
        {DsButton::X, PadControl::X},          {DsButton::Y, PadControl::Y},
        {DsButton::L, PadControl::L},          {DsButton::R, PadControl::R},
        {DsButton::Zl, PadControl::Zl},        {DsButton::Zr, PadControl::Zr},
        {DsButton::Start, PadControl::Plus},   {DsButton::Select, PadControl::Minus},
        {DsButton::Up, PadControl::Up},        {DsButton::Down, PadControl::Down},
        {DsButton::Left, PadControl::Left},    {DsButton::Right, PadControl::Right},
        {DsButton::LeftStick, PadControl::LeftStick}, {DsButton::RightStick, PadControl::RightStick},
    };
    uint32_t controls = 0;
    for (const auto& [button, control] : kPairs) {
        if ((buttons & (1u << static_cast<unsigned>(button))) != 0) {
            controls |= 1u << static_cast<unsigned>(control);
        }
    }
    return controls;
}

}  // namespace ds
