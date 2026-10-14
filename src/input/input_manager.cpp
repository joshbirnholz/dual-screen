#include "input/input_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

#include "util/log.hpp"

#if defined(__linux__) && !SDL_VERSION_ATLEAST(2, 30, 0)
#include <dlfcn.h>
#endif

#ifdef __SWITCH__
#include <switch.h>

#include "input/switch_console.hpp"
#endif

#if defined(__ANDROID__)
#include <SDL_system.h>
#include <jni.h>
#endif

namespace ds {
namespace {

// How far either finger may travel and still be a tap, as a fraction of the
// display -- SDL reports touches normalised, so this is the same gesture on a
// phone and on a tablet.
constexpr float kTwoFingerTapSlip = 0.03f;
// And how long it may last. Long enough not to punish a deliberate press,
// short enough that two fingers resting on the glass are not it.
constexpr uint32_t kTwoFingerTapMs = 700;

}  // namespace

namespace {

DsButton ds_button_for_key(SDL_Keycode key, bool& found) {
    found = true;
    switch (key) {
        case SDLK_z: return DsButton::B;
        case SDLK_x: return DsButton::A;
        case SDLK_a: return DsButton::Y;
        case SDLK_s: return DsButton::X;
        case SDLK_q: return DsButton::L;
        case SDLK_w: return DsButton::R;
        case SDLK_RETURN: return DsButton::Start;
        case SDLK_RSHIFT: return DsButton::Select;
        case SDLK_UP: return DsButton::Up;
        case SDLK_DOWN: return DsButton::Down;
        case SDLK_LEFT: return DsButton::Left;
        case SDLK_RIGHT: return DsButton::Right;
        default: break;
    }
    found = false;
    return DsButton::A;
}

}  // namespace

InputManager::~InputManager() { shutdown(); }

void InputManager::initialize(const Settings& settings) {
    settings_ = settings;
    settings_.stick_deadzone = std::clamp(settings_.stick_deadzone, 0.05f, 0.9f);
    settings_.trigger_deadzone = std::clamp(settings_.trigger_deadzone, 0.05f, 0.9f);

    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (SDL_IsGameController(index)) {
            open_controller(index);
        }
    }
}

#ifdef __SWITCH__
namespace {

// Which of the console's controller slots a pad is. The Switch port of SDL
// opens all eight slots at start-up, whether anything is in them or not,
// with the instance id being the slot -- except that its first one is player
// 1 and the handheld controls together, merged. Here they are two: 0 is
// player 1 alone, and the handheld controls are kSwitchHandheldId, an id SDL
// never hands out. A Pro Controller paired to a Switch Lite is player 1, and
// merged with the Lite's own controls it could never be anybody but whoever
// was holding the Lite.
constexpr SDL_JoystickID kSwitchHandheldId = 8;

HidNpadIdType npad_for(SDL_JoystickID id) {
    if (id == kSwitchHandheldId) {
        return HidNpadIdType_Handheld;
    }
    return static_cast<HidNpadIdType>(HidNpadIdType_No1 + id);
}

// The two SDL merges, read apart, straight off the console.
struct SplitPads {
    PadState player_one;
    PadState handheld;
};

SplitPads& split_pads() {
    static SplitPads pads = [] {
        SplitPads made{};
        padInitialize(&made.player_one, HidNpadIdType_No1);
        padInitialize(&made.handheld, HidNpadIdType_Handheld);
        return made;
    }();
    return pads;
}

// The Switch port's joystick buttons, in its own order -- what SDL's mapping
// for the pad numbers its buttons by.
constexpr u64 kJoystickButtons[] = {
    HidNpadButton_A,          HidNpadButton_B,          HidNpadButton_X,
    HidNpadButton_Y,          HidNpadButton_StickL,     HidNpadButton_StickR,
    HidNpadButton_L,          HidNpadButton_R,          HidNpadButton_ZL,
    HidNpadButton_ZR,         HidNpadButton_Plus,       HidNpadButton_Minus,
    HidNpadButton_Left,       HidNpadButton_Up,         HidNpadButton_Right,
    HidNpadButton_Down,       HidNpadButton_StickLLeft, HidNpadButton_StickLUp,
    HidNpadButton_StickLRight, HidNpadButton_StickLDown, HidNpadButton_StickRLeft,
    HidNpadButton_StickRUp,   HidNpadButton_StickRRight, HidNpadButton_StickRDown,
    HidNpadButton_LeftSL,     HidNpadButton_LeftSR,     HidNpadButton_RightSL,
    HidNpadButton_RightSR,
};

int16_t clamp_axis(int value) {
    return static_cast<int16_t>(std::max(-32767, std::min(32767, value)));
}

// One of the split pads, as SDL would have read it: the console's buttons
// and sticks put through the same binding SDL uses for its pad `like`, so
// that the mapping after this cannot tell the difference.
RawPadState split_raw_state(const PadState& pad, SDL_GameController* like) {
    const u64 held = padGetButtons(&pad);
    const HidAnalogStickState left = padGetStickPos(&pad, 0);
    const HidAnalogStickState right = padGetStickPos(&pad, 1);
    // The Switch port's axes: y turned to point down, as SDL's do.
    const int16_t joystick_axes[4] = {clamp_axis(left.x), clamp_axis(-left.y),
                                      clamp_axis(right.x), clamp_axis(-right.y)};
    const auto joystick_button = [&](int index) {
        return index >= 0 && index < static_cast<int>(sizeof(kJoystickButtons) / sizeof(u64)) &&
               (held & kJoystickButtons[index]) != 0;
    };

    RawPadState raw;
    for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
        const SDL_GameControllerButtonBind bind =
            SDL_GameControllerGetBindForButton(like, static_cast<SDL_GameControllerButton>(index));
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) {
            raw.buttons[index] = joystick_button(bind.value.button);
        }
    }
    for (int index = 0; index < SDL_CONTROLLER_AXIS_MAX; ++index) {
        const SDL_GameControllerButtonBind bind =
            SDL_GameControllerGetBindForAxis(like, static_cast<SDL_GameControllerAxis>(index));
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_AXIS && bind.value.axis >= 0 &&
            bind.value.axis < 4) {
            raw.axes[index] = joystick_axes[bind.value.axis];
        } else if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) {
            raw.axes[index] = joystick_button(bind.value.button) ? 32767 : 0;
        }
    }
    return raw;
}

// Runs or stops one slot's motors through the console itself.
//
// Not SDL_GameControllerRumble: the Switch port only sets vibration up for
// slots 1 to 7, and only for whatever was in them at start-up. Slot 0 --
// the handheld controls, and player 1 -- never rumbles through it at all,
// and a controller connected later never does either. So the handles are
// asked for here, fresh for whatever style the slot holds now.
//
// A Switch Lite has no rumble motor, and this does nothing there however it
// is asked.
void switch_vibrate(SDL_JoystickID id, bool on) {
    const HidNpadIdType npad = npad_for(id);
    const u32 styles = hidGetNpadStyleSet(npad);
    HidNpadStyleTag style = HidNpadStyleTag_NpadFullKey;
    s32 handles_wanted = 2;
    if (styles & HidNpadStyleTag_NpadHandheld) {
        style = HidNpadStyleTag_NpadHandheld;
    } else if (styles & HidNpadStyleTag_NpadFullKey) {
        style = HidNpadStyleTag_NpadFullKey;
    } else if (styles & HidNpadStyleTag_NpadJoyDual) {
        style = HidNpadStyleTag_NpadJoyDual;
    } else if (styles & HidNpadStyleTag_NpadJoyLeft) {
        style = HidNpadStyleTag_NpadJoyLeft;
        handles_wanted = 1;
    } else if (styles & HidNpadStyleTag_NpadJoyRight) {
        style = HidNpadStyleTag_NpadJoyRight;
        handles_wanted = 1;
    } else {
        return;  // nothing in the slot
    }
    HidVibrationDeviceHandle handles[2];
    if (R_FAILED(hidInitializeVibrationDevices(handles, handles_wanted, npad, style))) {
        return;
    }
    HidVibrationValue values[2];
    for (HidVibrationValue& value : values) {
        value.amp_low = on ? 0.6f : 0.0f;
        value.freq_low = 160.0f;
        value.amp_high = on ? 0.6f : 0.0f;
        value.freq_high = 320.0f;
    }
    hidSendVibrationValues(handles, values, handles_wanted);
}

// What is in a slot, in words a player recognises. SDL calls every one of
// them "Switch Controller", which tells two apart not at all. Which slot it
// is travels separately -- see switch_pad_slot().
std::string switch_pad_description(SDL_JoystickID id) {
    const HidNpadIdType npad = npad_for(id);
    const u32 styles = hidGetNpadStyleSet(npad);
    if (npad == HidNpadIdType_Handheld) {
        // A Lite's controls are the console; anything else's are two Joy-Con
        // slid onto it.
        return switch_is_lite() ? "Switch Lite" : "Joy-Con (attached)";
    }
    const char* kind = "Controller";
    if (styles & HidNpadStyleTag_NpadFullKey) kind = "Pro Controller";
    else if (styles & HidNpadStyleTag_NpadJoyDual) kind = "Joy-Con pair";
    else if (styles & HidNpadStyleTag_NpadJoyLeft) kind = "Left Joy-Con";
    else if (styles & HidNpadStyleTag_NpadJoyRight) kind = "Right Joy-Con";
    else if (styles & HidNpadStyleTag_NpadGc) kind = "GameCube controller";
    else if (styles & HidNpadStyleTag_NpadLucia) kind = "SNES controller";
    else if (styles & HidNpadStyleTag_NpadLagon) kind = "N64 controller";
    else if (styles & HidNpadStyleTag_NpadLager) kind = "Genesis controller";
    else if (styles & (HidNpadStyleTag_NpadLark | HidNpadStyleTag_NpadHandheldLark)) {
        kind = "NES controller";
    } else if (styles & HidNpadStyleTag_NpadPalma) kind = "Poke Ball Plus";
    return kind;
}

// The player slot the console has it in, which is the number its own
// lights show -- none for the handheld controls, which have no lights.
int switch_pad_slot(SDL_JoystickID id) {
    return npad_for(id) == HidNpadIdType_Handheld ? -1 : static_cast<int>(id);
}

}  // namespace
#endif

#ifndef __SWITCH__
namespace {

// SDL's Steam Input handle for a pad: which physical controller Steam has
// behind one of its virtual ones, or 0 for none. Asked for by name at run
// time where the SDL this was built against predates it -- the SteamOS build
// compiles against Debian 12's SDL 2.26, and runs against SteamOS's own,
// which has it.
using SteamHandleFn = Uint64 (*)(SDL_GameController*);

SteamHandleFn steam_handle_fn() {
#if SDL_VERSION_ATLEAST(2, 30, 0)
    return &SDL_GameControllerGetSteamHandle;
#elif defined(__linux__)
    static const SteamHandleFn fn = reinterpret_cast<SteamHandleFn>(
        dlsym(RTLD_DEFAULT, "SDL_GameControllerGetSteamHandle"));
    return fn;
#else
    return nullptr;
#endif
}

// Whether Steam started this app, and so may be about to present its
// controllers as its own.
bool launched_by_steam() {
    static const bool launched = std::getenv("SteamGameId") != nullptr ||
                                 std::getenv("SteamAppId") != nullptr ||
                                 std::getenv("SteamDeck") != nullptr;
    return launched;
}

// One of the virtual pads Steam Input presents in place of the real ones.
bool is_steam_virtual_pad(SDL_GameController* controller) {
    return SDL_GameControllerGetVendor(controller) == 0x28de &&
           SDL_GameControllerGetProduct(controller) == 0x11ff;
}

}  // namespace
#endif


bool InputManager::reads_pad(SDL_JoystickID id) const {
#ifdef __SWITCH__
    (void)id;
    return true;
#else
    // Not one Steam is also presenting -- see pad_connected(). A pad not
    // opened here is nobody's copy.
    const auto it = controllers_.find(id);
    return it == controllers_.end() || it->second == nullptr || pad_connected(id);
#endif
}

bool InputManager::pad_connected(SDL_JoystickID id) const {
#ifdef __SWITCH__
    // An empty slot has no style at all.
    return ((id >= 0 && id < 8) || id == kSwitchHandheldId) &&
           hidGetNpadStyleSet(npad_for(id)) != 0;
#else
    // Steam keeps a virtual pad for a slot whose controller has gone -- and
    // sometimes one or two spare -- and SDL lists every one, named plain
    // "Steam Virtual Gamepad" because there is nothing behind it to name it
    // after. Such a slot has no Steam handle. But nothing has a handle when
    // Steam has not told SDL about its pads at all (the app started outside
    // Steam), so a slot is only taken for empty when another of Steam's
    // pads here does have one.
    const auto it = controllers_.find(id);
    if (it == controllers_.end() || it->second == nullptr) {
        return false;
    }
    const SteamHandleFn handle_of = steam_handle_fn();
    if (!is_steam_virtual_pad(it->second)) {
        // Steam Input is the one word on what a button is. While Steam
        // presents controllers here, every controller it can see arrives
        // through one of its own, with the player's mapping in it -- and SDL
        // may still show the controller underneath as well, raw. That copy is
        // never read: read from both, a press arrived twice and meant
        // different things depending on which copy got there first. A pad
        // that has just arrived waits a moment for Steam's to appear before
        // it is taken for one Steam does not present.
        for (const auto& [other_id, other] : controllers_) {
            if (other != nullptr && is_steam_virtual_pad(other)) return false;
        }
        return std::chrono::steady_clock::now() >= steam_settled_at_ || !launched_by_steam();
    }
    if (handle_of == nullptr || handle_of(it->second) != 0) {
        return true;
    }
    for (const auto& [other_id, other] : controllers_) {
        if (other != nullptr && is_steam_virtual_pad(other) && handle_of(other) != 0) {
            return false;
        }
    }
    return true;
#endif
}

std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> InputManager::connected_pads() const {
    std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> pads;
#ifdef __SWITCH__
    // The handheld controls first: on a Switch Lite, the console itself, and
    // held by whoever is holding the screen. It has no SDL pad of its own --
    // see kSwitchHandheldId -- so SDL's first stands in wherever one is asked
    // for, for its mapping.
    const auto first = controllers_.find(0);
    if (first != controllers_.end() && first->second != nullptr &&
        pad_connected(kSwitchHandheldId)) {
        pads.emplace_back(kSwitchHandheldId, first->second);
    }
#endif
    for (const auto& [id, controller] : controllers_) {
        if (controller != nullptr && pad_connected(id)) pads.emplace_back(id, controller);
    }
    return pads;
}

SDL_GameController* InputManager::pad_controller(SDL_JoystickID id) const {
    for (const auto& [pad, controller] : connected_pads()) {
        if (pad == id) return controller;
    }
    return nullptr;
}

int InputManager::connected_controllers() const {
    return static_cast<int>(connected_pads().size());
}

#if defined(__ANDROID__)
static void android_rumble(int ms) {
    static bool said = false;
    if (ms > 0 && !said) {
        said = true;
        log_info("rumble: through Android's vibrator");
    }
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) return;
    jclass type = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(type, "rumble", "(I)V");
    if (method != nullptr) env->CallVoidMethod(activity, method, static_cast<jint>(ms));
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
}
#endif

void InputManager::drive_motor(SDL_JoystickID id, SDL_GameController* controller, bool on,
                               uint32_t ms) {
#ifdef __SWITCH__
    (void)controller;
    switch_vibrate(id, on);
    motor_until_[id] = on ? SDL_GetTicks() + ms : 0;
#elif defined(__ANDROID__)
    // SDL's Android port has no controller rumble -- SDL_GameControllerRumble
    // answers "unsupported" for every pad -- so the activity runs the motors
    // itself: see DualscreenActivity.rumble(). Every controller at once, since
    // Android does not say which of its devices is which of SDL's.
    (void)id;
    (void)controller;
    android_rumble(on ? static_cast<int>(ms) : 0);
#else
    if (SDL_GameControllerRumble(controller, on ? 0xffffu : 0u, on ? 0xffffu : 0u,
                                 on ? ms : 0u) != 0 &&
        on && !said_no_rumble_[id]) {
        said_no_rumble_[id] = true;
        // Said, because "nothing buzzed" is otherwise indistinguishable from
        // "the buzz was never asked for".
        log_info("controller " + std::to_string(id) + " cannot rumble: " + SDL_GetError());
    }
#endif
}

void InputManager::shutdown() {
    // Before the controllers close: switching a sensor off asks the
    // controller about itself, which a closed one cannot answer.
    motion_.set_enabled(false);
    // A motor left running would outlive the app on a Switch, whose vibration
    // is the console's rather than SDL's to stop.
    for (const auto& [id, controller] : connected_pads()) {
        drive_motor(id, controller, false, 0);
    }
    for (auto& entry : controllers_) {
        if (entry.second != nullptr) {
            SDL_GameControllerClose(entry.second);
        }
    }
    controllers_.clear();
    pad_rumble_on_.clear();
    motor_until_.clear();
}

void InputManager::open_controller(int device_index) {
    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (controller == nullptr) {
        log_warn(std::string("could not open controller: ") + SDL_GetError());
        return;
    }
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    const SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    if (controllers_.count(id) != 0) {
        SDL_GameControllerClose(controller);
        return;
    }
    controllers_[id] = controller;
    // A pad arriving -- or Steam's arriving -- gives Steam a moment to say
    // which of its pads is which before a pad it may be presenting is read
    // directly. See pad_connected().
    steam_settled_at_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // Sensors are per controller and have to be switched on before SDL will
    // report them, so one arriving mid-session has to be told -- see
    // MotionReader::controller_added().
    motion_.controller_added(controller);

    // Logged in full because a mis-mapped pad is otherwise very hard to
    // diagnose remotely: this one line says what SDL thinks the pad is, which
    // decides how its face buttons are reported.
    const char* name = SDL_GameControllerName(controller);
    // The button and axis counts are the joystick's own, underneath whatever
    // mapping SDL built on top. They are here because the question "is this a
    // pad or a television remote" has already been answered wrongly once from
    // the mapping, and the next time it needs answering this log will have the
    // numbers in it rather than needing another trip to the hardware.
    log_info(std::string("controller connected: ") + (name != nullptr ? name : "unknown") +
             " (SDL type: " + to_string(SDL_GameControllerGetType(controller)) + ", " +
             std::to_string(SDL_JoystickNumButtons(joystick)) + " buttons, " +
             std::to_string(SDL_JoystickNumAxes(joystick)) + " axes, taken for " +
             (controller_is_a_remote(name) ? "a television remote" : "a pad") + ")");

    char* mapping = SDL_GameControllerMapping(controller);
    if (mapping != nullptr) {
        log_debug(std::string("controller mapping: ") + mapping);
        SDL_free(mapping);
    }

    if (controllers_.size() > 1) {
        // Normal under Steam Input, which adds a virtual pad alongside the real
        // device. The real one is passed over where Steam has one of its own
        // for it -- see pad_connected() -- and duplicate menu actions dropped.
        log_info("now tracking " + std::to_string(controllers_.size()) +
                 " controllers; duplicate presses are merged");
    }
#ifndef __SWITCH__
    // Either order of arriving can make one of them Steam's copy of the other.
    for (const auto& [pad, opened] : controllers_) {
        if (opened == nullptr || reads_pad(pad)) continue;
        const char* shadowed = SDL_GameControllerName(opened);
        log_info(std::string("reading ") + (shadowed != nullptr ? shadowed : "a controller") +
                 " through Steam Input only, with its remapping");
    }
#endif
}

bool controller_is_a_remote(const char* sdl_name) {
    if (sdl_name == nullptr) {
        return false;
    }
    std::string lowered(sdl_name);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered.find("remote") != std::string::npos;
}

bool InputManager::single_joycon_in_use() const {
#ifdef __SWITCH__
    const auto single = [](SDL_JoystickID id) {
        const u32 styles = hidGetNpadStyleSet(npad_for(id));
        return (styles & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) != 0 &&
               (styles & (HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey |
                          HidNpadStyleTag_NpadHandheld)) == 0;
    };
    if (last_pad_pressed_ >= 0 && pad_connected(last_pad_pressed_)) {
        return single(last_pad_pressed_);
    }
    bool any = false;
    for (SDL_JoystickID id = 0; id <= kSwitchHandheldId; ++id) {
        if (!pad_connected(id)) continue;
        if (!single(id)) return false;
        any = true;
    }
    return any;
#else
    return false;
#endif
}

int InputManager::connected_gamepads() const {
    int count = 0;
    for (const auto& [instance_id, controller] : connected_pads()) {
        (void)instance_id;
        if (!controller_is_a_remote(SDL_GameControllerName(controller))) {
            ++count;
        }
    }
    return count;
}

void InputManager::close_controller(SDL_JoystickID instance_id) {
    const auto it = controllers_.find(instance_id);
    if (it == controllers_.end()) {
        return;
    }
    if (it->second != nullptr) {
        motion_.controller_removed(it->second);
        SDL_GameControllerClose(it->second);
    }
    controllers_.erase(it);
    log_info("controller disconnected");
}

void InputManager::set_rumble(bool active) {
    // SDL has no "rumble until told otherwise" mode, only a duration -- so a
    // short one is used and refreshed by every repeated "on" this is called
    // with while the far end's motor stays on, which is what makes a run of
    // such calls feel continuous rather than pulsed. If an "off" is ever
    // lost in transit this stops on its own within a quarter second instead
    // of buzzing forever, the same self-healing shape as everything else
    // this link tolerates dropping.
    constexpr uint32_t kRumbleMs = 250;
    for (const auto& [instance_id, controller] : connected_pads()) {
        drive_motor(instance_id, controller, active, kRumbleMs);
    }
}

void InputManager::ping_pad(SDL_JoystickID id, uint32_t ms) {
    for (const auto& [instance_id, controller] : connected_pads()) {
        if (id != -1 && instance_id != id) continue;
        // A game's rumble already running says more than a ping could, and
        // cutting it short at the end of the ping would be wrong.
        if (pad_rumble_on_.count(instance_id) != 0) {
            log_info("controller " + std::to_string(instance_id) +
                     ": not pinged, a game already has its motor running");
            continue;
        }
        drive_motor(instance_id, controller, true, ms);
#ifndef __SWITCH__
        // What SDL made of it, for a pad that should have buzzed and did not:
        // whether SDL thinks it can rumble at all, which slot it is in -- a
        // Steam Input slot, under Steam, whose physical pad Steam decides --
        // and what SDL called it.
        const char* name = SDL_GameControllerName(controller);
        log_info("controller " + std::to_string(instance_id) + " pinged: slot " +
                 std::to_string(SDL_GameControllerGetPlayerIndex(controller)) + ", " +
                 (name != nullptr ? name : "unnamed") + ", SDL says it " +
                 (SDL_GameControllerHasRumble(controller) == SDL_TRUE ? "can" : "cannot") +
                 " rumble");
#endif
    }
}

DsInputState InputManager::gated(DsInputState held) const {
    // The same gate ds_state() goes through, so the press that closed a menu
    // is no more a player's press than it is the GamePad's.
    held.buttons &= held_input_gate_.buttons();
    if (!held_input_gate_.stick_live()) {
        held.circle_x = 0;
        held.circle_y = 0;
    }
    if (!held_input_gate_.c_stick_live()) {
        held.c_stick_x = 0;
        held.c_stick_y = 0;
    }
    return held;
}

std::vector<PadReading> InputManager::pad_readings() const {
    // In the order the pads' own player numbers put them, which under Steam
    // Input is Steam's slot order -- the order a player arranges in Steam and
    // expects to see as Controller 1, 2, 3. A pad without one comes after, in
    // the order it arrived.
    // Only what is actually plugged in: a Switch lists every slot it has.
    std::vector<std::pair<SDL_JoystickID, SDL_GameController*>> ordered = connected_pads();
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        const int a = SDL_GameControllerGetPlayerIndex(left.second);
        const int b = SDL_GameControllerGetPlayerIndex(right.second);
        if ((a < 0) != (b < 0)) return b < 0;
        return a < b;
    });
    std::vector<PadReading> readings;
    readings.reserve(ordered.size() + 1);
    for (const auto& [id, controller] : ordered) {
        PadReading reading;
        reading.id = id;
#ifdef __SWITCH__
        reading.name = switch_pad_description(id);
        reading.slot = switch_pad_slot(id);
#else
        const char* name = SDL_GameControllerName(controller);
        reading.name = name != nullptr ? name : "";
        // Steam's slot under Steam Input; SDL's own numbering otherwise.
        reading.slot = SDL_GameControllerGetPlayerIndex(controller);
#endif
        if (game_input_enabled_) {
#ifdef __SWITCH__
            // Player 1 and the handheld controls, which SDL reads as one pad,
            // read apart -- see kSwitchHandheldId.
            if (id == 0 || id == kSwitchHandheldId) {
                const SplitPads& split = split_pads();
                reading.held = gated(held_state_from(
                    split_raw_state(id == 0 ? split.player_one : split.handheld, controller)));
            } else {
                reading.held = gated(held_state_of(controller));
            }
#else
            reading.held = gated(held_state_of(controller));
#endif
            // Its own motion, where it has its own sensors switched on. Both
            // or neither, for the reason MotionReader gives.
            float gyro[3] = {0.0f, 0.0f, 0.0f};
            float accel[3] = {0.0f, 0.0f, 0.0f};
            if (SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_GYRO) == SDL_TRUE &&
                SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_ACCEL) == SDL_TRUE &&
                SDL_GameControllerGetSensorData(controller, SDL_SENSOR_GYRO, gyro, 3) == 0 &&
                SDL_GameControllerGetSensorData(controller, SDL_SENSOR_ACCEL, accel, 3) == 0 &&
                (accel[0] != 0.0f || accel[1] != 0.0f || accel[2] != 0.0f)) {
                DsMotionState& motion = reading.held.motion;
                motion.present = true;
                motion.gyro_x = gyro[0];
                motion.gyro_y = gyro[1];
                motion.gyro_z = gyro[2];
                motion.accel_x = accel[0];
                motion.accel_y = accel[1];
                motion.accel_z = accel[2];
                motion.sampled_us = static_cast<uint32_t>(SDL_GetTicks64() * 1000u);
            }
        }
        readings.push_back(std::move(reading));
    }
    return readings;
}

void InputManager::set_pad_rumble(SDL_JoystickID id, bool active) {
    SDL_GameController* controller = pad_controller(id);
    if (controller == nullptr) {
        pad_rumble_on_.erase(id);
        return;
    }
    if (active) {
        pad_rumble_on_[id] = true;
    } else {
        pad_rumble_on_.erase(id);
    }
    // The same short, renewed rumble set_rumble() uses, and for its reason.
    constexpr uint32_t kRumbleMs = 250;
    drive_motor(id, controller, active, kRumbleMs);
}

void InputManager::refresh_pad_rumble() {
    // What SDL would do with a duration, done here for the motors it is not
    // trusted with -- see drive_motor().
    if (!motor_until_.empty()) {
        const uint32_t now = SDL_GetTicks();
        for (auto& [id, until] : motor_until_) {
            if (until == 0 || static_cast<int32_t>(now - until) < 0) continue;
            until = 0;
            if (SDL_GameController* controller = pad_controller(id)) {
                drive_motor(id, controller, false, 0);
            }
        }
    }
    if (pad_rumble_on_.empty()) {
        return;
    }
    // Renewed well inside the 250ms each one lasts, so a motor left on stays
    // on without a gap -- and stops on its own within a quarter second of this
    // app going away, whatever else happens.
    const uint32_t now = SDL_GetTicks();
    if (now - pad_rumble_refreshed_ms_ < 100) {
        return;
    }
    pad_rumble_refreshed_ms_ = now;
    for (auto it = pad_rumble_on_.begin(); it != pad_rumble_on_.end();) {
        SDL_GameController* controller = pad_controller(it->first);
        if (controller == nullptr) {
            it = pad_rumble_on_.erase(it);
            continue;
        }
        drive_motor(it->first, controller, true, 250);
        ++it;
    }
}

void InputManager::set_game_input_enabled(bool enabled) {
    if (enabled && !game_input_enabled_) {
        // Anything already held when the game starts listening again was meant
        // for the menu -- most obviously the A press that chose "Resume".
        held_input_gate_.arm();
    } else if (!enabled) {
        held_input_gate_.reset();
    }
    game_input_enabled_ = enabled;
}

void InputManager::set_text_entry(bool on) {
    if (on == text_entry_) return;
    text_entry_ = on;
    if (on) {
        SDL_StartTextInput();
    } else {
        SDL_StopTextInput();
    }
}

void InputManager::begin_frame() {
    ui_actions_.clear();
    typed_text_.clear();
    backspaces_ = 0;
    wheel_ = 0.0f;
    pointer_events_.clear();
    window_size_changed_ = false;
#ifdef __SWITCH__
    // The two pads SDL merges, read apart this frame -- see split_pads().
    SplitPads& split = split_pads();
    padUpdate(&split.player_one);
    padUpdate(&split.handheld);
#endif
    refresh_pad_rumble();
}

void InputManager::forget_touches() {
    fingers_.clear();
    has_primary_finger_ = false;
    two_finger_tap_live_ = false;
    mouse_down_ = false;
    pointer_active_ = false;
}

void InputManager::push_action(UiAction action) {
    // Deduplicated within the frame. Steam Input presents a virtual pad while
    // the physical device often stays visible too, so a single press arrives
    // as two identical button-down events. Acting on both is actively wrong in
    // a menu: two Confirms in the browser step into a folder and straight back
    // out of it, which looks like the button doing nothing at all. No menu
    // wants the same discrete action twice in one frame regardless.
    if (std::find(ui_actions_.begin(), ui_actions_.end(), action) != ui_actions_.end()) {
        return;
    }
    ui_actions_.push_back(action);
}

void InputManager::handle_event(const SDL_Event& event) {
    switch (event.type) {
        case SDL_QUIT:
            quit_requested_ = true;
            break;

        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                event.window.event == SDL_WINDOWEVENT_RESIZED) {
                window_size_changed_ = true;
            }
            // Whatever was being touched when the window went away will never
            // be lifted here: the release goes wherever the focus went. A
            // finger left down forever is a finger the count never loses, and
            // the gestures that ask how many are down stop working for the rest
            // of the session.
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                forget_touches();
            }
            break;

        // The same on a phone, where the window is taken away rather than
        // merely unfocused.
        case SDL_APP_WILLENTERBACKGROUND:
            forget_touches();
            break;

        // Controllers are opened and closed as they appear, so hot-plugging a
        // pad mid-session (common on a Deck docked to a TV) just works.
        case SDL_CONTROLLERDEVICEADDED:
            open_controller(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            close_controller(event.cdevice.which);
            break;

        case SDL_CONTROLLERBUTTONDOWN: {
            // The same press again, from a controller Steam presents too.
            if (!reads_pad(event.cbutton.which)) break;
            SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
            last_pad_pressed_ = event.cbutton.which;
            // Every button is what SDL says it is -- for a Steam Input pad,
            // what the player mapped it to in Steam -- and Confirm/Cancel
            // alone says which of the two lower face buttons answers yes.
            const ConfirmButton confirm_choice = confirm_button_;

            // Every press is logged under --verbose with the name SDL gives it,
            // so a pad whose buttons arrive as the wrong thing can be
            // identified from a log rather than guessed at.
            const char* sdl_name = SDL_GameControllerGetStringForButton(button);
            log_debug(std::string("controller button down: ") +
                      (sdl_name != nullptr ? sdl_name : "?") + " (" +
                      std::to_string(static_cast<int>(button)) + ")");

            // Which of the two lower face buttons means yes is the player's
            // to choose -- east by default, south for anyone who grew up on a
            // PlayStation. The other one of the pair is always the other
            // answer, so the two can never both mean the same thing.
            const char* meaning = "nothing in a menu";
            if (button == confirm_button_for(confirm_choice)) {
                push_action(UiAction::Confirm);
                meaning = "yes";
            } else if (button == cancel_button_for(confirm_choice)) {
                push_action(UiAction::Cancel);
                meaning = "no / back";
            } else if (button == kMenuQuitButton) {
                push_action(UiAction::MenuQuit);
                meaning = "the menu's quit";
            } else if (button == kMenuResetButton) {
                push_action(UiAction::MenuReset);
                meaning = "the menu's reset";
            } else if (button == kMenuSettingsButton) {
                push_action(UiAction::MenuSettings);
                meaning = "settings";
            }

            // And the first press of each button, at a level that reaches the
            // console rather than only the file.
            //
            // The debug line above has been there all along and was no use on
            // an Apple TV, where there is no command line to turn verbosity on
            // with and the log file needs Xcode and a container download to
            // read. What it would have said the first time somebody clicked a
            // Siri Remote is that the click arrives as south -- which this app
            // was reading as "no". Once per button is a mapping; every press
            // is noise.
            if (button >= 0 && button < 32 &&
                (said_buttons_ & (1u << static_cast<unsigned>(button))) == 0) {
                said_buttons_ |= 1u << static_cast<unsigned>(button);
                log_info(std::string("controller: a press arrived as ") +
                         (sdl_name != nullptr ? sdl_name : "?") + ", which means " + meaning +
                         " here");
            }

            switch (button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: push_action(UiAction::Up); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: push_action(UiAction::Down); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: push_action(UiAction::Left); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: push_action(UiAction::Right); break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: push_action(UiAction::PageUp); break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: push_action(UiAction::PageDown); break;
                // L3 and R3 are not acted on here. Alone they are still the
                // layout swap and the smoothing toggle, but together they are
                // the menu, and which of the three a press turns out to be is
                // not known until it is released -- see end_frame().
#ifdef __ANDROID__
                // A pad's own home button, which on Android reaches the app.
                // Elsewhere the system takes it first -- Steam on a Deck, the
                // console itself on a Switch -- so there is nothing to bind.
                case SDL_CONTROLLER_BUTTON_GUIDE: push_action(UiAction::ToggleMenu); break;
#endif
                default: break;
            }
            break;
        }

        case SDL_TEXTINPUT:
            if (text_entry_) typed_text_ += event.text.text;
            break;

        case SDL_KEYDOWN: {
            // Typing into a field: Backspace deletes, held as it repeats, and
            // only Enter and Escape mean anything more.
            if (text_entry_) {
                switch (event.key.keysym.sym) {
                    case SDLK_BACKSPACE: ++backspaces_; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                        if (event.key.repeat == 0) push_action(UiAction::Confirm);
                        break;
                    case SDLK_ESCAPE:
                        if (event.key.repeat == 0) push_action(UiAction::Cancel);
                        break;
                    default: break;
                }
                break;
            }
            if (event.key.repeat != 0) break;

            // Logged for the same reason controller buttons are: a Steam Input
            // layout that sends keystrokes instead of gamepad buttons is
            // otherwise invisible from inside the app.
            log_debug(std::string("key down: ") + SDL_GetKeyName(event.key.keysym.sym));

            switch (event.key.keysym.sym) {
                // Back, everywhere -- and, while a game is being played, the
                // way into the quick menu too. See UiAction::OpenMenu.
                case SDLK_ESCAPE:
                    push_action(UiAction::Cancel);
                    push_action(UiAction::OpenMenu);
                    break;
                case SDLK_BACKSPACE: push_action(UiAction::Cancel); break;
                // F1 is the menu key. No controller button opens the menu:
                // bind F1 in Steam Input instead, which is the only way that
                // survives Steam's own claim on the pad.
                case SDLK_F1:
                case SDLK_TAB: push_action(UiAction::ToggleMenu); break;
                // The keyboard equivalent, so a Steam Input layout that sends
                // keystrokes rather than pad buttons can reach it too.
                case SDLK_F2: push_action(UiAction::ToggleFilter); break;
                case SDLK_F3: push_action(UiAction::SwapLayout); break;
                // Keyboard equivalents of the menu's north and west face
                // buttons, in the same undiscoverable-but-there spirit as the
                // function keys above: without them a machine with no pad
                // could not reset or quit a game from the menu at all.
                case SDLK_F5: push_action(UiAction::MenuReset); break;
                case SDLK_F6: push_action(UiAction::MenuQuit); break;
                case SDLK_F4: push_action(UiAction::MenuSettings); break;
                case SDLK_F7: push_action(UiAction::MenuDisconnect); break;
                case SDLK_F10: push_action(UiAction::Quit); break;
                // Return doubles as DS Start in game, but game input is off
                // while a menu is open, so there is no conflict.
                case SDLK_SPACE:
                case SDLK_RETURN:
                case SDLK_KP_ENTER: push_action(UiAction::Confirm); break;
                default: break;
            }
            // Arrow keys double as menu navigation; they are also DS d-pad keys,
            // and the menu/game split decides which one actually applies.
            switch (event.key.keysym.sym) {
                case SDLK_UP: push_action(UiAction::Up); break;
                case SDLK_DOWN: push_action(UiAction::Down); break;
                case SDLK_LEFT: push_action(UiAction::Left); break;
                case SDLK_RIGHT: push_action(UiAction::Right); break;
                case SDLK_PAGEUP: push_action(UiAction::PageUp); break;
                case SDLK_PAGEDOWN: push_action(UiAction::PageDown); break;
                default: break;
            }
            break;
        }

        case SDL_FINGERDOWN: {
            // A desktop that turns the mouse into a finger for us: the real
            // mouse events are handled below, and taking both would count every
            // click twice.
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID) break;
            FingerState state;
            state.x = event.tfinger.x;
            state.y = event.tfinger.y;
            state.press_x = state.x;
            state.press_y = state.y;
            fingers_[event.tfinger.fingerId] = state;

            if (!has_primary_finger_) {
                has_primary_finger_ = true;
                primary_finger_ = event.tfinger.fingerId;
                PointerEvent pointer;
                pointer.x = state.x;
                pointer.y = state.y;
                pointer.press_x = state.press_x;
                pointer.press_y = state.press_y;
                pointer.pressed = true;
                pointer_events_.push_back(pointer);
            } else if (fingers_.size() == 2) {
                // Two fingers tapped together is the way into the menu on a
                // device with no buttons, and it is the only way in there: a
                // button drawn over the game would sit on the picture for the
                // whole session.
                //
                // Not gated on game_owns_controls_, which covers the stick
                // clicks a Wii U game reads. No console this streams has a
                // multi-touch screen -- a DS, a 3DS and a GamePad are all
                // single-touch, and the wire carries one point -- so a second
                // finger is never something the game could have wanted.
                //
                // Armed here and fired on release, rather than the moment the
                // second finger lands. A finger resting while the other one
                // draws is not a tap, and on the screen a stylus belongs to
                // that is a thing people do.
                two_finger_tap_live_ = true;
                two_finger_tap_began_ms_ = SDL_GetTicks();
            } else if (fingers_.size() > 2) {
                // Three is not two.
                two_finger_tap_live_ = false;
            }
            break;
        }

        case SDL_FINGERMOTION: {
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID) break;
            const auto it = fingers_.find(event.tfinger.fingerId);
            if (it != fingers_.end()) {
                it->second.x = event.tfinger.x;
                it->second.y = event.tfinger.y;
                // A tap does not travel. Anything that does is a drag, a
                // pinch or a hand resting on the glass, and none of those
                // means "open the menu".
                if (two_finger_tap_live_ &&
                    (std::abs(it->second.x - it->second.press_x) > kTwoFingerTapSlip ||
                     std::abs(it->second.y - it->second.press_y) > kTwoFingerTapSlip)) {
                    two_finger_tap_live_ = false;
                }
            }
            break;
        }

        case SDL_FINGERUP: {
            if (event.tfinger.touchId == SDL_MOUSE_TOUCHID) break;
            const auto lifted = fingers_.find(event.tfinger.fingerId);
            const bool known = lifted != fingers_.end();
            const float press_x = known ? lifted->second.press_x : event.tfinger.x;
            const float press_y = known ? lifted->second.press_y : event.tfinger.y;
            if (known) fingers_.erase(lifted);
            if (has_primary_finger_ && event.tfinger.fingerId == primary_finger_) {
                has_primary_finger_ = false;
                PointerEvent pointer;
                pointer.x = event.tfinger.x;
                pointer.y = event.tfinger.y;
                pointer.press_x = press_x;
                pointer.press_y = press_y;
                pointer.released = true;
                pointer_events_.push_back(pointer);
            }
            // Lifting the first of the two is the tap. Checked as the count
            // falls below two rather than once every last finger has gone: a
            // finger that stays put, or one whose release was lost, would
            // otherwise leave the gesture armed forever.
            if (two_finger_tap_live_ && fingers_.size() < 2) {
                two_finger_tap_live_ = false;
                if (SDL_GetTicks() - two_finger_tap_began_ms_ <= kTwoFingerTapMs) {
                    push_action(UiAction::ToggleMenu);
                }
            }
            break;
        }

        // Mouse events keep the app usable on a desktop, where they stand in
        // for the touchscreen.
        // A phone reports one tap twice: as a finger, and again as a mouse
        // synthesized from that finger. The finger is the real one -- it is the
        // only one that says how many are down -- so the synthetic mouse is
        // dropped rather than counted a second time in the other's units.
        case SDL_MOUSEBUTTONDOWN:
            if (event.button.which == SDL_TOUCH_MOUSEID) break;
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = true;
                mouse_x_ = static_cast<float>(event.button.x);
                mouse_y_ = static_cast<float>(event.button.y);
                mouse_press_x_ = mouse_x_;
                mouse_press_y_ = mouse_y_;
                PointerEvent pointer;
                pointer.x = mouse_x_;
                pointer.y = mouse_y_;
                pointer.press_x = mouse_press_x_;
                pointer.press_y = mouse_press_y_;
                pointer.pressed = true;
                pointer.normalized = false;
                pointer_events_.push_back(pointer);
            }
            break;

        case SDL_MOUSEBUTTONUP:
            if (event.button.which == SDL_TOUCH_MOUSEID) break;
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_down_ = false;
                mouse_x_ = static_cast<float>(event.button.x);
                mouse_y_ = static_cast<float>(event.button.y);
                PointerEvent pointer;
                pointer.x = mouse_x_;
                pointer.y = mouse_y_;
                pointer.press_x = mouse_press_x_;
                pointer.press_y = mouse_press_y_;
                pointer.released = true;
                pointer.normalized = false;
                pointer_events_.push_back(pointer);
            }
            break;

        case SDL_MOUSEMOTION:
            mouse_x_ = static_cast<float>(event.motion.x);
            mouse_y_ = static_cast<float>(event.motion.y);
            break;

        case SDL_MOUSEWHEEL: {
            if (event.wheel.which == SDL_TOUCH_MOUSEID) break;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            float notches = event.wheel.preciseY;
#else
            float notches = static_cast<float>(event.wheel.y);
#endif
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) notches = -notches;
            wheel_ += notches;
#if SDL_VERSION_ATLEAST(2, 26, 0)
            mouse_x_ = static_cast<float>(event.wheel.mouseX);
            mouse_y_ = static_cast<float>(event.wheel.mouseY);
#endif
            break;
        }

        default:
            break;
    }
}

void InputManager::emit_navigation_from_axes() {
    float x = 0.0f;
    float y = 0.0f;
    // Either stick: whichever is pushed further.
    const SDL_GameControllerAxis sticks[2][2] = {
        {SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY},
        {SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY}};
    for (const auto& entry : controllers_) {
        if (entry.second == nullptr || !reads_pad(entry.first)) continue;
        for (const auto& stick : sticks) {
            const float axis_x = SDL_GameControllerGetAxis(entry.second, stick[0]) / 32767.0f;
            const float axis_y = SDL_GameControllerGetAxis(entry.second, stick[1]) / 32767.0f;
            if (std::fabs(axis_x) > std::fabs(x)) x = axis_x;
            if (std::fabs(axis_y) > std::fabs(y)) y = axis_y;
        }
    }

    const float deadzone = settings_.stick_deadzone;
    const bool up = y < -deadzone;
    const bool down = y > deadzone;
    const bool left = x < -deadzone;
    const bool right = x > deadzone;

    // A step on the way in; holding it on is repeat_held_directions()'s, at a
    // pace a list can be read at rather than one step per frame.
    if (up && !axis_up_) push_action(UiAction::Up);
    if (down && !axis_down_) push_action(UiAction::Down);
    // Said once, for the same reason as the buttons above: on a device with no
    // command line, "the menu does not move" and "the menu moves and something
    // else is wrong" are the same report until something says which.
    if ((up || down) && !said_axis_navigation_) {
        said_axis_navigation_ = true;
        log_info("controller: a stick or touch surface is moving the menu");
    }
    if (left && !axis_left_) push_action(UiAction::Left);
    if (right && !axis_right_) push_action(UiAction::Right);
    axis_up_ = up;
    axis_down_ = down;
    axis_left_ = left;
    axis_right_ = right;

    // Everything that can be holding a direction down right now, for the
    // repeat below: the stick, as above, any pad's d-pad, and the arrow keys.
    bool held[4] = {up, down, left, right};
    const SDL_GameControllerButton dpad[4] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    for (const auto& entry : controllers_) {
        if (entry.second == nullptr || !reads_pad(entry.first)) continue;
        for (int direction = 0; direction < 4; ++direction) {
            if (SDL_GameControllerGetButton(entry.second, dpad[direction]) != 0) {
                held[direction] = true;
            }
        }
    }
    int key_count = 0;
    const Uint8* keys = SDL_GetKeyboardState(&key_count);
    const SDL_Scancode arrows[4] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT,
                                    SDL_SCANCODE_RIGHT};
    for (int direction = 0; direction < 4; ++direction) {
        if (keys != nullptr && arrows[direction] < key_count && keys[arrows[direction]] != 0) {
            held[direction] = true;
        }
        if (forwarded_held_[direction]) held[direction] = true;
    }
    repeat_held_directions(held);
}

void InputManager::repeat_held_directions(const bool held[4]) {
    // Long enough that one press is one step for anybody who meant one, short
    // enough that running down a long list does not feel like waiting.
    constexpr uint32_t kRepeatDelayMs = 400;
    constexpr uint32_t kRepeatEveryMs = 75;
    static const UiAction kActions[4] = {UiAction::Up, UiAction::Down, UiAction::Left,
                                         UiAction::Right};
    const uint32_t now = SDL_GetTicks();
    for (int direction = 0; direction < 4; ++direction) {
        if (!held[direction]) {
            direction_held_[direction] = false;
            direction_repeated_[direction] = false;
            continue;
        }
        if (!direction_held_[direction]) {
            direction_held_[direction] = true;
            direction_repeated_[direction] = false;
            direction_next_repeat_[direction] = now + kRepeatDelayMs;
            continue;
        }
        if (static_cast<int32_t>(now - direction_next_repeat_[direction]) >= 0) {
            direction_repeated_[direction] = true;
            push_action(kActions[direction]);
            direction_next_repeat_[direction] = now + kRepeatEveryMs;
        }
    }
}

bool InputManager::direction_repeating(UiAction direction) const {
    switch (direction) {
        case UiAction::Up: return direction_repeated_[0];
        case UiAction::Down: return direction_repeated_[1];
        case UiAction::Left: return direction_repeated_[2];
        case UiAction::Right: return direction_repeated_[3];
        default: return false;
    }
}

MenuButtonLabels InputManager::menu_labels() const {
    for (const auto& entry : controllers_) {
        if (entry.second != nullptr) {
            return menu_button_labels(pad_letter_layout(SDL_GameControllerGetType(entry.second)));
        }
    }
    return MenuButtonLabels();  // Xbox letters: the common case, and Steam Input's
}

bool InputManager::stick_buttons_held(SDL_GameControllerButton button) const {
    for (const auto& entry : controllers_) {
        if (entry.second != nullptr && reads_pad(entry.first) &&
            SDL_GameControllerGetButton(entry.second, button) != 0) {
            return true;
        }
    }
    return false;
}

RawPadState InputManager::raw_state_of(SDL_GameController* controller) {
    RawPadState raw;
    for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
        raw.buttons[index] = SDL_GameControllerGetButton(
                                 controller, static_cast<SDL_GameControllerButton>(index)) != 0;
    }
    for (int index = 0; index < SDL_CONTROLLER_AXIS_MAX; ++index) {
        raw.axes[index] = SDL_GameControllerGetAxis(controller, static_cast<SDL_GameControllerAxis>(index));
    }
    return raw;
}

DsInputState InputManager::held_state_of(SDL_GameController* controller) const {
    return held_state_from(raw_state_of(controller));
}

DsInputState InputManager::held_state_from(const RawPadState& raw) const {
    DsInputState held;

    for (int index = 0; index < SDL_CONTROLLER_BUTTON_MAX; ++index) {
        const SDL_GameControllerButton button = static_cast<SDL_GameControllerButton>(index);
        DsButton ds_button = DsButton::A;
        if (ds_button_for_controller_button(button, ds_button) && raw.buttons[index]) {
            held.set(ds_button, true);
        }
    }
    // The left stick is always the circle pad. Whether it is *also* the
    // d-pad depends on the console: a DS has no stick, so reaching for one
    // has to work, but a 3DS has both and pressing both at once is what
    // made its circle pad unusable -- a core that reads the d-pad sees a
    // direction held down for as long as the stick is off centre, and
    // walking becomes the eight directions the d-pad has.
    const int16_t raw_x = raw.axes[SDL_CONTROLLER_AXIS_LEFTX];
    const int16_t raw_y = raw.axes[SDL_CONTROLLER_AXIS_LEFTY];
    const float axis_x = raw_x / 32767.0f;
    const float axis_y = raw_y / 32767.0f;
    const float deadzone = settings_.stick_deadzone;
    const int16_t deadzone_threshold = static_cast<int16_t>(deadzone * 32767.0f);
    if (stick_is_dpad_) {
        if (axis_y < -deadzone) held.set(DsButton::Up, true);
        if (axis_y > deadzone) held.set(DsButton::Down, true);
        if (axis_x < -deadzone) held.set(DsButton::Left, true);
        if (axis_x > deadzone) held.set(DsButton::Right, true);
    }

    // The strongest deflection across every pad wins, so two controllers do
    // not average each other out into standing still.
    if (std::abs(raw_x) > std::abs(held.circle_x)) held.circle_x = raw_x;
    if (std::abs(raw_y) > std::abs(held.circle_y)) held.circle_y = raw_y;

    // What the right stick is depends on what the game wants -- a C-Stick,
    // a second circle pad, a d-pad, or nothing. See RightStickMode.
    const int16_t raw_c_x = raw.axes[SDL_CONTROLLER_AXIS_RIGHTX];
    const int16_t raw_c_y = raw.axes[SDL_CONTROLLER_AXIS_RIGHTY];
    switch (right_stick_mode_) {
        case RightStickMode::CStick:
            if (std::abs(raw_c_x) > std::abs(held.c_stick_x)) held.c_stick_x = raw_c_x;
            if (std::abs(raw_c_y) > std::abs(held.c_stick_y)) held.c_stick_y = raw_c_y;
            break;
        case RightStickMode::CirclePad:
            // Into the same axes as the left stick, competing on
            // deflection like a second pad does: two sticks that averaged
            // each other out would leave a player standing still while
            // pushing one of them.
            if (std::abs(raw_c_x) > std::abs(held.circle_x)) held.circle_x = raw_c_x;
            if (std::abs(raw_c_y) > std::abs(held.circle_y)) held.circle_y = raw_c_y;
            break;
        case RightStickMode::DPad:
            if (raw_c_y < -deadzone_threshold) held.set(DsButton::Up, true);
            if (raw_c_y > deadzone_threshold) held.set(DsButton::Down, true);
            if (raw_c_x < -deadzone_threshold) held.set(DsButton::Left, true);
            if (raw_c_x > deadzone_threshold) held.set(DsButton::Right, true);
            break;
        case RightStickMode::Off:
            break;
    }

    const float trigger_deadzone = settings_.trigger_deadzone;
    const float left_trigger =
        raw.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] / 32767.0f;
    const float right_trigger =
        raw.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] / 32767.0f;
    if (game_owns_controls_) {
        // A console with four shoulder buttons and two sticks that press
        // has somewhere for all of them to go, and the game reads them all.
        // See set_game_owns_controls().
        if (left_trigger > trigger_deadzone) held.set(DsButton::Zl, true);
        if (right_trigger > trigger_deadzone) held.set(DsButton::Zr, true);
        if (raw.buttons[SDL_CONTROLLER_BUTTON_LEFTSTICK]) {
            held.set(DsButton::LeftStick, true);
        }
        if (raw.buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK]) {
            held.set(DsButton::RightStick, true);
        }
    } else {
        // The triggers double as L and R. The DS's shoulder buttons are
        // digital, but plenty of people reach for the triggers first, and a
        // pad that has both should honour either. The stick presses are
        // this app's own shortcuts here, so they are not the game's.
        if (left_trigger > trigger_deadzone) held.set(DsButton::L, true);
        if (right_trigger > trigger_deadzone) held.set(DsButton::R, true);
        // Except this one, on a console that has a microphone: it goes to
        // the game as L3, which is where melonDS listens for it.
        if (left_stick_is_mic_ &&
            raw.buttons[SDL_CONTROLLER_BUTTON_LEFTSTICK]) {
            held.set(DsButton::LeftStick, true);
        }
    }
    return held;
}

namespace {

// One pad's presses added to others': any button on either is pressed, and
// whichever stick is pushed further wins, so two pads never average each
// other out into standing still.
void merge_held(DsInputState& into, const DsInputState& from) {
    into.buttons |= from.buttons;
    if (std::abs(from.circle_x) > std::abs(into.circle_x)) into.circle_x = from.circle_x;
    if (std::abs(from.circle_y) > std::abs(into.circle_y)) into.circle_y = from.circle_y;
    if (std::abs(from.c_stick_x) > std::abs(into.c_stick_x)) into.c_stick_x = from.c_stick_x;
    if (std::abs(from.c_stick_y) > std::abs(into.c_stick_y)) into.c_stick_y = from.c_stick_y;
}

}  // namespace

DsInputState InputManager::keyboard_held_state() const {
    DsInputState held;
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    if (keys != nullptr) {
        static const SDL_Keycode kKeys[] = {SDLK_z, SDLK_x, SDLK_a, SDLK_s, SDLK_q, SDLK_w,
                                            SDLK_RETURN, SDLK_RSHIFT, SDLK_UP, SDLK_DOWN,
                                            SDLK_LEFT, SDLK_RIGHT};
        for (const SDL_Keycode key : kKeys) {
            bool found = false;
            const DsButton button = ds_button_for_key(key, found);
            if (found && keys[SDL_GetScancodeFromKey(key)] != 0) {
                held.set(button, true);
            }
        }
    }

    return held;
}

DsInputState InputManager::poll_held_state() const {
    // Polled unconditionally, even while a menu is open: the gate needs the
    // true physical state to know what to suppress when the game resumes.
    DsInputState held;

    for (const auto& entry : controllers_) {
        SDL_GameController* controller = entry.second;
        if (controller == nullptr || !reads_pad(entry.first)) continue;
        merge_held(held, held_state_of(controller));
    }

    merge_held(held, keyboard_held_state());
    return held;
}

bool InputManager::update_pointer(float output_width, float output_height) {
    float point_x = 0.0f;
    float point_y = 0.0f;
    bool have_point = false;

    if (has_primary_finger_) {
        const auto it = fingers_.find(primary_finger_);
        if (it != fingers_.end()) {
            // SDL touch coordinates are normalized to the window.
            point_x = it->second.x * output_width;
            point_y = it->second.y * output_height;
            have_point = true;
        }
    } else if (mouse_down_) {
        point_x = mouse_x_;
        point_y = mouse_y_;
        have_point = true;
    }

    // Kept up to date whether or not the game is listening, because the menus
    // need it for drag scrolling.
    pointer_active_ = have_point;
    pointer_x_ = point_x;
    pointer_y_ = point_y;
    return have_point;
}

void InputManager::end_frame(const Layout& layout,
                             float output_width,
                             float output_height,
                             const SystemScreens& screens,
                             float display_width,
                             float display_height) {
    // Normalize every pointer event into output pixels now that the size is known.
    for (PointerEvent& pointer : pointer_events_) {
        if (pointer.normalized) {
            pointer.x *= output_width;
            pointer.y *= output_height;
            pointer.press_x *= output_width;
            pointer.press_y *= output_height;
            pointer.normalized = false;
        }
    }

    emit_navigation_from_axes();

    // Once per frame, alongside the buttons, so that the reading sent with a
    // pad update is the one taken at the same moment as the buttons on it.
    motion_.poll();

    const DsInputState held = poll_held_state();

    // L3 and R3 together open the menu. Neither reaches a DS or 3DS button, so
    // the pair costs a game nothing -- unlike Start and Select, which plenty
    // of DS games read together as a soft reset, a poor thing to do to someone
    // on their way into the menu.
    //
    // Apart they keep their own meanings, but a press only becomes one of
    // those once it is released without the other having joined it. Otherwise
    // reaching for the chord would swap the layout on the way in.
    const bool left = stick_buttons_held(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    const bool right = stick_buttons_held(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    if (left && right && !stick_chord_used_) {
        // Still marked used either way, so that letting go of the pair does not
        // then fire the two singles below on a console that allows them.
        stick_chord_used_ = true;
        if (!game_owns_controls_) {
            push_action(UiAction::ToggleMenu);
        }
    }
    // Only where the game does not want them. See set_game_owns_controls().
    if (!game_owns_controls_) {
        if (left_stick_held_ && !left && !stick_chord_used_ && !left_stick_is_mic_) {
            push_action(UiAction::SwapLayout);
        }
        if (right_stick_held_ && !right && !stick_chord_used_) {
            push_action(UiAction::ToggleFilter);
        }
    }
    if (!left && !right) {
        stick_chord_used_ = false;
    }
    left_stick_held_ = left;
    right_stick_held_ = right;

    const bool pointer_down = update_pointer(output_width, output_height);
    // Off centre by enough that the menu would have moved on it, which is the
    // deflection worth holding back after the menu closes.
    const int16_t deadzone_threshold = static_cast<int16_t>(settings_.stick_deadzone * 32767.0f);
    const bool stick_off_centre =
        std::abs(held.circle_x) > deadzone_threshold || std::abs(held.circle_y) > deadzone_threshold;
    const bool c_stick_off_centre = std::abs(held.c_stick_x) > deadzone_threshold ||
                                    std::abs(held.c_stick_y) > deadzone_threshold;
    held_input_gate_.update(held.buttons, pointer_down, stick_off_centre, c_stick_off_centre);

    ds_state_.clear();
    if (game_input_enabled_) {
        ds_state_.buttons = held_input_gate_.buttons();
        // Not gated the way buttons and sticks are. The gate exists to stop
        // the press that dismissed a menu also landing in the game, and
        // there is no equivalent for how the device is being held: it was
        // being held that way through the menu too, and a game handed a
        // frozen orientation for a moment after resuming would lurch when it
        // caught up.
        ds_state_.motion = motion_.state();
        // The sticks reach the game as themselves. Every layer below this one
        // -- the wire, the core's analog callback -- already carries them;
        // nothing filled them in, so a 3DS could be handed a stick that was
        // always centred.
        if (held_input_gate_.stick_live()) {
            ds_state_.circle_x = held.circle_x;
            ds_state_.circle_y = held.circle_y;
        }
        if (held_input_gate_.c_stick_live()) {
            ds_state_.c_stick_x = held.c_stick_x;
            ds_state_.c_stick_y = held.c_stick_y;
        }
        // Not where the interface has claimed the pixels -- see
        // set_ui_reserved_rect(). Checked before the mapping rather than
        // after, because the mapping's job is to say where in the console's
        // screen a point is, not whether it should have been asked.
        // One finger only. Every console this streams has a single-touch
        // screen, so a second finger was never going to be a second press --
        // and while two are down the player is talking to this app, not to
        // the game.
        if (held_input_gate_.touching() && fingers_.size() < 2) {
            DsTouchPoint point;
            if (map_output_point_to_ds(layout, output_width, output_height, pointer_x_, pointer_y_,
                                       point, screens, display_width, display_height)) {
                ds_state_.touching = true;
                ds_state_.touch_x = point.x;
                ds_state_.touch_y = point.y;
            }
        }
    }

    for (const UiAction action : ui_actions_) {
        if (action == UiAction::Quit) {
            quit_requested_ = true;
        }
    }
}

}  // namespace ds
