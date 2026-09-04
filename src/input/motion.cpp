#include "input/motion.hpp"

#include "input/switch_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    // And stop asking the network, so a session that turns motion off stops
    // sending its keepalive rather than subscribing forever. Cleared too, so
    // that turning it back on looks for a server again -- one may have been
    // started in between.
    dsu_.stop();
    tried_dsu_ = false;
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
    // Which sensor to read is not decided once. A Switch Lite has one built
    // in and never changes; a regular console does -- Joy-Cons attached is a
    // different handle set from the same pair detached, and either can happen
    // in the middle of a session. Acquired once and kept forever, this worked
    // for whichever way the console happened to be held at startup and went
    // silent for the other.
    //
    // So the handles are re-taken whenever nothing is coming back, and the
    // three ways a Switch is actually held are tried in turn: the console
    // itself (a Lite, or a regular one with Joy-Cons on), a detached pair, and
    // a Pro Controller.
    static HidSixAxisSensorHandle handles[2] = {};
    static s32 handle_count = 0;
    static uint32_t retry_at_ms = 0;
    static int quiet_reads = 0;
    static const char* source_name = nullptr;

    const uint32_t now_ms = SDL_GetTicks();
    if (handle_count == 0 && (retry_at_ms == 0 || now_ms >= retry_at_ms)) {
        // A second between attempts: this is only reached when nothing else
        // reported motion, and asking the system for handles it does not have
        // sixty times a second is pure waste on a console with no sensors at
        // all.
        retry_at_ms = now_ms + 1000;
        struct Candidate {
            HidNpadIdType id;
            // libnx's own types, not the integers underneath them: the style
            // is an enum and the count is signed, and spelling either as
            // something wider makes the call ambiguous rather than merely
            // ugly.
            HidNpadStyleTag style;
            s32 count;
            const char* name;
        };
        const Candidate candidates[] = {
            {HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld, 1, "this console's own"},
            {HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual, 2, "a pair of Joy-Cons"},
            {HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey, 1, "a Pro Controller"},
        };
        for (const Candidate& candidate : candidates) {
            if (R_SUCCEEDED(hidGetSixAxisSensorHandles(&handles[0], candidate.count, candidate.id,
                                                       candidate.style))) {
                handle_count = candidate.count;
                source_name = candidate.name;
                break;
            }
        }
        for (s32 index = 0; index < handle_count; ++index) {
            hidStartSixAxisSensor(handles[index]);
        }
        if (handle_count > 0) {
            log_info(std::string("motion: reading it from ") + source_name + " six-axis sensor");
            quiet_reads = 0;
        }
    }
    if (handle_count == 0) {
        return false;
    }
    HidSixAxisSensorState sample = {};
    if (hidGetSixAxisSensorStates(handles[0], &sample, 1) < 1) {
        // The handles have gone stale -- Joy-Cons came off, or went on. Given
        // a moment in case it is a single dropped read, then taken again.
        if (++quiet_reads > 60) {
            for (s32 index = 0; index < handle_count; ++index) {
                hidStopSixAxisSensor(handles[index]);
            }
            handle_count = 0;
            retry_at_ms = 0;
            log_info("motion: that six-axis sensor stopped reporting; looking again");
        }
        return false;
    }
    quiet_reads = 0;
    // Acceleration into SDL's units and axes, which is what everything
    // downstream expects -- see DsMotionState. libnx reports it in g and puts
    // its axes elsewhere; see switch_accel_to_sdl(), which was measured from
    // two poses on a real console and is the one part of this that has stayed
    // put.
    const SwitchSixAxis native_accel = {sample.acceleration.x, sample.acceleration.y,
                                        sample.acceleration.z};
    const SwitchSixAxis native_gyro = {sample.angular_velocity.x, sample.angular_velocity.y,
                                       sample.angular_velocity.z};
    const SwitchSixAxis accel = switch_accel_to_sdl(native_accel);
    state_.accel_x = accel.x * kGravity;
    state_.accel_y = accel.y * kGravity;
    state_.accel_z = accel.z * kGravity;

    // Rotation is not guessed at any more. Five builds guessed, on five
    // readings of what a game felt like, and a rotation that is out by a
    // factor as well as by its axes cannot be told apart from one that is
    // merely out by its axes by feel -- both are just wrong all the time. So
    // the accelerometer settles it instead: while the console turns, the
    // direction it says the ground lies in turns with it, and only one frame
    // and one scale make the two agree. See MotionFitter.
    //
    // Until it has been moved enough to be sure, the last guess stands. It
    // costs nothing to leave it there and it is the only thing to fall back
    // on for a console left flat on a table, which never settles anything.
    static uint32_t fit_at_ms = 0;
    if (fit_at_ms != 0) {
        motion_fitter_.add(accel, native_gyro,
                           static_cast<float>(now_ms - fit_at_ms) / 1000.0f);
    }
    fit_at_ms = now_ms;
    if (motion_fitter_.fit().decided && !said_fit_) {
        said_fit_ = true;
        log_info("motion: this console reports rotation as " + motion_fitter_.fit().describe() +
                 " of a radian a second, worked out from its own accelerometer");
    } else if (!motion_fitter_.fit().decided) {
        // And how far along it is otherwise. The first version of this said
        // nothing until it decided, and then never decided, and a log with a
        // silence in it where the answer should be cannot be told from a log
        // of something that was never reached. Once a second, and only while
        // the console is actually being moved, so a session spent sitting
        // still stays quiet.
        static uint32_t progress_at_ms = 0;
        if (progress_at_ms == 0 || now_ms - progress_at_ms >= 1000) {
            progress_at_ms = now_ms;
            const int percent = static_cast<int>(motion_fitter_.progress() * 100.0f + 0.5f);
            if (percent > 0) {
                log_debug("motion: working out this console's rotation frame, " +
                          std::to_string(percent) + "% of the way there");
            }
        }
    }

    constexpr float kTau = 6.283185307f;
    const SwitchSixAxis gyro = motion_fitter_.fit().decided
                                   ? motion_fitter_.fit().axes.apply(native_gyro)
                                   : switch_gyro_to_sdl(native_gyro);
    const float rate = motion_fitter_.fit().decided ? motion_fitter_.fit().scale : kTau;
    state_.gyro_x = gyro.x * rate;
    state_.gyro_y = gyro.y * rate;
    state_.gyro_z = gyro.z * rate;

    // The reading as libnx gave it, before any of the above.
    //
    // The fit is what decides now, so this is no longer how the answer gets
    // found -- it is how the answer gets checked, which is worth keeping
    // separate. A log with only converted numbers in it can say "this is
    // wrong" and never "then what is", and four rounds went by on exactly
    // that.
    //
    // Reported as the strongest sample since the last line rather than
    // whatever the last one happened to be, so a deliberate turn lasting a
    // second or two lands in the log at its peak instead of falling between
    // two reports.
    static uint32_t raw_at_ms = 0;
    static HidSixAxisSensorState loudest = {};
    static float loudest_rate = -1.0f;
    const float turn_rate = std::abs(sample.angular_velocity.x) +
                            std::abs(sample.angular_velocity.y) +
                            std::abs(sample.angular_velocity.z);
    if (turn_rate > loudest_rate) {
        loudest_rate = turn_rate;
        loudest = sample;
    }
    if (raw_at_ms == 0 || now_ms - raw_at_ms >= 1000) {
        raw_at_ms = now_ms;
        if (loudest_rate >= 0.0f) {
            // The peak magnitude names the unit, which libnx also does not
            // document and which this multiplies by tau on the assumption
            // that it is revolutions a second. A deliberate turn of the
            // console is somewhere around a quarter to a whole turn a second,
            // so a peak near 1 is revolutions, near 5 is radians, and in the
            // hundreds is degrees. Moonlight-Switch's own comment says the
            // rotation it forwards is radians a second, which is a reason to
            // check rather than a reason to change it blind.
            const char* unit = loudest_rate > 60.0f    ? "degrees"
                               : loudest_rate > 2.5f   ? "radians"
                                                       : "revolutions";
            char text[224];
            std::snprintf(text, sizeof(text),
                          "motion: libnx gave accel %.2f,%.2f,%.2f and rotation "
                          "%.3f,%.3f,%.3f unconverted; that peak looks like %s a second",
                          static_cast<double>(loudest.acceleration.x),
                          static_cast<double>(loudest.acceleration.y),
                          static_cast<double>(loudest.acceleration.z),
                          static_cast<double>(loudest.angular_velocity.x),
                          static_cast<double>(loudest.angular_velocity.y),
                          static_cast<double>(loudest.angular_velocity.z), unit);
            log_debug(text);
        }
        loudest_rate = -1.0f;
    }
    return true;
#else
    return false;
#endif
}

bool MotionReader::read_dsu_sensors() {
    // Only reached when nothing on this device itself answered, and only
    // started on the first time that happens -- a machine with working
    // sensors of its own never opens this socket.
    if (!tried_dsu_) {
        tried_dsu_ = true;
        std::string error;
        if (!dsu_.start(&error)) {
            log_info("motion: not asking a DSU server for it (" + error + ")");
        } else {
            log_debug("motion: no sensors on this device, so also asking " +
                      DsuClient::configured_server() + " over DSU");
        }
    }
    if (!dsu_.started()) {
        return false;
    }
    dsu_.poll();
    if (!dsu_.connected()) {
        return false;
    }
    state_ = dsu_.state();
    return true;
}

void MotionReader::poll() {
    if (!enabled_) {
        return;
    }
    // Whether anything was being read before this poll, which is the only way
    // to tell a first reading from the next one -- a source that fills state_
    // wholesale has already set present by the time it returns.
    const bool was_reading = state_.present;
    // In the order a device is most likely to have them, and stopping at the
    // first that answers: a pad in the player's hands beats the machine's own
    // sensors, which beat the console's built-in ones.
    if (read_controller_sensors() || read_device_sensors() || read_console_sensors()) {
        // Stamped here rather than at the sensor, because none of the three
        // says when it took the reading -- so the moment it was asked for is
        // the closest thing available.
        state_.sampled_us = now_micros();
    } else if (!read_dsu_sensors()) {
        // Nothing anywhere. Not an error: plenty of devices have no sensors.
        state_.present = false;
        return;
    }
    if (!was_reading) {
        log_info("motion: this device is reporting how it is being held");
    }
    state_.present = true;

    // What is actually being read, once a second.
    //
    // Axis conventions are the one thing here that cannot be settled by
    // reading code: every device has its own, the documentation for several
    // of them is other people's reverse engineering, and being wrong looks
    // exactly like being right except the game leans the other way. Two poses
    // and this line settle it -- held upright facing the player, gravity
    // belongs almost entirely in accel_y; laid flat face up, in accel_z.
    // Anything else means the axes need swapping, and says which for which.
    constexpr uint32_t kReportEveryMs = 1000;
    const uint32_t now = SDL_GetTicks();
    if (reported_at_ms_ == 0 || now - reported_at_ms_ >= kReportEveryMs) {
        reported_at_ms_ = now;
        const auto one = [](float value) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.2f", static_cast<double>(value));
            return std::string(text);
        };
        log_debug("motion: accel " + one(state_.accel_x) + "," + one(state_.accel_y) + "," +
                  one(state_.accel_z) + " m/s2, gyro " + one(state_.gyro_x) + "," +
                  one(state_.gyro_y) + "," + one(state_.gyro_z) + " rad/s");
    }
}

}  // namespace ds
