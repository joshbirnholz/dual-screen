#pragma once

#include <cstdint>
#include <vector>

#include <SDL.h>

#include "input/ds_input.hpp"

namespace ds {

// Where a device's own sense of how it is being held comes from.
//
// Three places, because the three devices this runs on put it in three
// different ones, and only the first is the obvious one:
//
//   A controller with sensors in it -- a Steam Deck, a DualSense, a Switch
//   Pro pad. SDL reports these against the game controller itself, and they
//   have to be switched on per controller before it will.
//
//   The machine itself. A phone or tablet's gyroscope belongs to no
//   controller at all, so SDL exposes it as a standalone sensor instead. This
//   is the whole story on an Android handheld with no pad attached, and the
//   fallback on one where the pad has no sensors of its own.
//
//   The console's own six-axis unit. A Switch's is behind libnx rather than
//   anything SDL offers, and reaching it directly is the only way to get it.
//
// All three are read into the one shape and the one convention -- SDL's, see
// DsMotionState -- so that everything downstream, on this device and on the
// far end, deals with a Wii U GamePad's motion rather than with whichever
// handheld happens to be standing in for one.
//
// Nothing here integrates anything. What comes out is the raw pair of
// readings and the moment they were taken; see DsMotionState for why that is
// deliberate.
class MotionReader {
public:
    ~MotionReader();

    // Starts or stops reading. Sensors cost power to leave running -- a
    // phone's gyroscope is the clearest case -- so this is off until
    // something says it is actually being read, and goes off again the
    // moment nothing is. Calling it with what is already set does nothing.
    //
    // Enabling can find nothing at all, which is not a failure: a device with
    // no sensors simply reports no motion for the whole session.
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }

    // Whether a real sensor was found and is being read. False before the
    // first sample arrives even when one was, since a sensor that is open but
    // has said nothing yet has nothing to report.
    bool reading() const { return state_.present; }

    // A controller arriving or leaving. Sensors live on the controller, so one
    // plugged in mid-session has to be switched on, and one unplugged has to
    // stop being read from.
    void controller_added(SDL_GameController* controller);
    void controller_removed(SDL_GameController* controller);

    // Reads whatever the sensors say right now. Polled rather than driven by
    // SDL's sensor events: this is sent once per frame at most, so the newest
    // reading is the only one worth having, and a queue of them between
    // frames is a queue to throw away.
    void poll();

    // The most recent reading, in SDL's units and axes. Absent (present ==
    // false) until enabled and a sensor has actually reported.
    const DsMotionState& state() const { return state_; }

private:
    void open_sensors();
    void close_sensors();
    // True if the reading came from somewhere; fills state_.
    bool read_controller_sensors();
    bool read_device_sensors();
    bool read_console_sensors();

    bool enabled_ = false;
    DsMotionState state_;
    // The controllers this has switched sensors on for, so they can be
    // switched off again and so one arriving mid-session is not missed.
    std::vector<SDL_GameController*> controllers_;
    // Standalone device sensors, opened only when no controller has any --
    // see the class comment.
    SDL_Sensor* device_gyro_ = nullptr;
    SDL_Sensor* device_accel_ = nullptr;
};

}  // namespace ds
