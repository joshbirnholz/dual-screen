#include "input/motion.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "util/log.hpp"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace ds {
namespace {

// SDL reports acceleration in metres per second squared and rotation in
// radians per second, and DsMotionState carries exactly that onward -- so
// there is no conversion here at all for the two SDL paths. The console path
// below converts into these units, which is the only reason they are named.
constexpr float kGravity = 9.80665f;

uint32_t now_micros() {
    // SDL's own high-resolution counter rather than a wall clock: this is a
    // duration between samples, and a clock that can be stepped by a time
    // sync would put a jump in the middle of somebody's aim.
    const uint64_t frequency = SDL_GetPerformanceFrequency();
    if (frequency == 0) {
        return static_cast<uint32_t>(SDL_GetTicks() * 1000u);
    }
    // Reduced before scaling so the multiply cannot overflow on a machine
    // that has been up for a while -- a counter at nanosecond resolution
    // passes 2^64/1000000 in about six months, and this app has no business
    // being the thing that breaks then.
    const uint64_t counter = SDL_GetPerformanceCounter();
    const uint64_t seconds = counter / frequency;
    const uint64_t remainder = counter % frequency;
    return static_cast<uint32_t>(seconds * 1000000ull + (remainder * 1000000ull) / frequency);
}

}  // namespace

MotionReader::~MotionReader() { close_sensors(); }

void MotionReader::set_enabled(bool enabled) {
    if (enabled == enabled_) {
        return;
    }
    enabled_ = enabled;
    if (enabled_) {
        open_sensors();
    } else {
        close_sensors();
        // Nothing is being read any more, so nothing should keep being sent.
        state_ = DsMotionState();
    }
}

void MotionReader::controller_added(SDL_GameController* controller) {
    if (controller == nullptr) {
        return;
    }
    if (std::find(controllers_.begin(), controllers_.end(), controller) != controllers_.end()) {
        return;
    }
    controllers_.push_back(controller);
    if (!enabled_) {
        return;
    }
    // Switched on straight away rather than waiting for the next enable: a pad
    // plugged in mid-session is exactly the case where the player has just
    // picked up the thing they intend to aim with.
    const bool has_gyro = SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE;
    const bool has_accel = SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE;
    if (has_gyro) {
        SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, SDL_TRUE);
    }
    if (has_accel) {
        SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, SDL_TRUE);
    }
    if (has_gyro || has_accel) {
        const char* name = SDL_GameControllerName(controller);
        log_info(std::string("motion: reading it from ") + (name != nullptr ? name : "a controller"));
    }
}

void MotionReader::controller_removed(SDL_GameController* controller) {
    const auto it = std::find(controllers_.begin(), controllers_.end(), controller);
    if (it == controllers_.end()) {
        return;
    }
    controllers_.erase(it);
    // Not switched off here: SDL has already closed it by the time a removal
    // is reported, and asking a closed controller for anything is exactly the
    // use-after-free this exists to avoid.
    //
    // Whatever it last said stops being true the moment it is gone.
    if (controllers_.empty()) {
        state_ = DsMotionState();
    }
}

void MotionReader::open_sensors() {
    int with_sensors = 0;
    for (SDL_GameController* controller : controllers_) {
        const bool has_gyro = SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE;
        const bool has_accel = SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE;
        if (has_gyro) {
            SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, SDL_TRUE);
        }
        if (has_accel) {
            SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, SDL_TRUE);
        }
        if (has_gyro && has_accel) {
            ++with_sensors;
        }
    }
    // Said out loud, once, because this is the question somebody will have
    // when aiming does not work and there is nothing else to look at: a pad
    // whose gyro SDL cannot see is indistinguishable from a game that ignores
    // motion. The commonest cause is a controller reaching SDL through
    // something that re-presents it without its sensors -- Steam Input does
    // this to a Steam Deck's own pad -- rather than anything here.
    log_info("motion: the host reads it; " + std::to_string(with_sensors) + " of " +
             std::to_string(controllers_.size()) + " connected controllers report it");

    // The machine's own sensors, for a device whose motion is not a
    // controller's -- see the class comment. Opened whatever the controllers
    // reported, because a pad with no sensors attached to a phone that has
    // them should still aim: read_controller_sensors() is tried first every
    // poll and these are only reached when it comes back with nothing.
    if (SDL_WasInit(SDL_INIT_SENSOR) == 0 && SDL_InitSubSystem(SDL_INIT_SENSOR) != 0) {
        log_info(std::string("motion: this device has no sensor support (") + SDL_GetError() + ")");
        return;
    }
    for (int index = 0; index < SDL_NumSensors(); ++index) {
        const SDL_SensorType type = SDL_SensorGetDeviceType(index);
        if (type == SDL_SENSOR_GYRO && device_gyro_ == nullptr) {
            device_gyro_ = SDL_SensorOpen(index);
        } else if (type == SDL_SENSOR_ACCEL && device_accel_ == nullptr) {
            device_accel_ = SDL_SensorOpen(index);
        }
    }
    if (device_gyro_ != nullptr && device_accel_ != nullptr) {
        log_info("motion: this device has sensors of its own to fall back on");
    } else if (with_sensors == 0) {
        log_info("motion: nothing here reports how it is being held, so this session has none");
    }
}

void MotionReader::close_sensors() {
    for (SDL_GameController* controller : controllers_) {
        if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO) == SDL_TRUE) {
            SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_GYRO, SDL_FALSE);
        }
        if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL) == SDL_TRUE) {
            SDL_GameControllerSetSensorEnabled(controller, SDL_SENSOR_ACCEL, SDL_FALSE);
        }
    }
    if (device_gyro_ != nullptr) {
        SDL_SensorClose(device_gyro_);
        device_gyro_ = nullptr;
    }
    if (device_accel_ != nullptr) {
        SDL_SensorClose(device_accel_);
        device_accel_ = nullptr;
    }
}

bool MotionReader::read_controller_sensors() {
    for (SDL_GameController* controller : controllers_) {
        float gyro[3] = {0.0f, 0.0f, 0.0f};
        float accel[3] = {0.0f, 0.0f, 0.0f};
        const bool got_gyro =
            SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_GYRO) == SDL_TRUE &&
            SDL_GameControllerGetSensorData(controller, SDL_SENSOR_GYRO, gyro, 3) == 0;
        const bool got_accel =
            SDL_GameControllerIsSensorEnabled(controller, SDL_SENSOR_ACCEL) == SDL_TRUE &&
            SDL_GameControllerGetSensorData(controller, SDL_SENSOR_ACCEL, accel, 3) == 0;
        // Both or neither. Gravity is what tells the far end's sensor fusion
        // which way is down, and without it a gyro on its own drifts away
        // within seconds with nothing to correct it.
        if (!got_gyro || !got_accel) {
            continue;
        }
        state_.gyro_x = gyro[0];
        state_.gyro_y = gyro[1];
        state_.gyro_z = gyro[2];
        state_.accel_x = accel[0];
        state_.accel_y = accel[1];
        state_.accel_z = accel[2];
        return true;
    }
    return false;
}

bool MotionReader::read_device_sensors() {
    if (device_gyro_ == nullptr || device_accel_ == nullptr) {
        return false;  // both or neither, for the reason above
    }
    float gyro[3] = {0.0f, 0.0f, 0.0f};
    float accel[3] = {0.0f, 0.0f, 0.0f};
    if (SDL_SensorGetData(device_gyro_, gyro, 3) != 0 ||
        SDL_SensorGetData(device_accel_, accel, 3) != 0) {
        return false;
    }
    state_.gyro_x = gyro[0];
    state_.gyro_y = gyro[1];
    state_.gyro_z = gyro[2];
    state_.accel_x = accel[0];
    state_.accel_y = accel[1];
    state_.accel_z = accel[2];
    return true;
}

bool MotionReader::read_console_sensors() {
#ifdef __SWITCH__
    // libnx rather than SDL. The Switch port's SDL exposes the pads' buttons
    // and sticks but not their six-axis unit, and this is the only way to it.
    //
    // Started once, on the first read that gets this far -- which is only
    // ever reached when neither SDL path found anything, so a build of SDL
    // that does grow sensor support takes over on its own with nothing here
    // to undo.
    static bool started = false;
    static HidSixAxisSensorHandle handles[2] = {};
    static unsigned handle_count = 0;
    if (!started) {
        started = true;
        // Handheld and the pair of joined Joy-Cons are the two ways a player
        // actually holds this console; a detached pair reports as the same
        // handle set. Failures leave handle_count at zero and this reports no
        // motion for the session, which is what a console with its sensors
        // switched off should do.
        if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&handles[0], 1, HidNpadIdType_Handheld,
                                                   HidNpadStyleTag_NpadHandheld))) {
            handle_count = 1;
        } else if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&handles[0], 2, HidNpadIdType_No1,
                                                          HidNpadStyleTag_NpadJoyDual))) {
            handle_count = 2;
        }
        for (unsigned index = 0; index < handle_count; ++index) {
            hidStartSixAxisSensor(handles[index]);
        }
        if (handle_count > 0) {
            log_info("motion: reading it from this console's six-axis sensor");
        }
    }
    if (handle_count == 0) {
        return false;
    }
    HidSixAxisSensorState sample = {};
    if (hidGetSixAxisSensorStates(handles[0], &sample, 1) < 1) {
        return false;
    }
    // Into SDL's units and axes, which is what everything downstream expects
    // -- see DsMotionState. libnx reports acceleration in g and rotation in
    // revolutions per second; SDL uses metres per second squared and radians
    // per second. The axes themselves already agree: both are right-handed
    // with +x right, +y up and +z towards the player when the console is held
    // normally.
    constexpr float kTau = 6.283185307f;
    state_.accel_x = sample.acceleration.x * kGravity;
    state_.accel_y = sample.acceleration.y * kGravity;
    state_.accel_z = sample.acceleration.z * kGravity;
    state_.gyro_x = sample.angular_velocity.x * kTau;
    state_.gyro_y = sample.angular_velocity.y * kTau;
    state_.gyro_z = sample.angular_velocity.z * kTau;
    return true;
#else
    return false;
#endif
}

void MotionReader::poll() {
    if (!enabled_) {
        return;
    }
    // In the order a device is most likely to have them, and stopping at the
    // first that answers: a pad in the player's hands beats the machine's own
    // sensors, which beat the console's built-in ones.
    const bool got = read_controller_sensors() || read_device_sensors() || read_console_sensors();
    if (!got) {
        state_.present = false;
        return;
    }
    if (!state_.present) {
        log_info("motion: this device is reporting how it is being held");
    }
    state_.present = true;
    state_.sampled_us = now_micros();
}

}  // namespace ds
