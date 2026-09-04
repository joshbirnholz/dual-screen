#pragma once

#include <cstdint>
#include <vector>

#include <SDL.h>

#include "input/ds_input.hpp"
#include "input/dsu_client.hpp"

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
// And one more that is not the device's own sensors at all: a DSU server, for
// the machine that has sensors none of the three above can reach. That is a
// Steam Deck under Steam Input, which hides the real pad behind a virtual one
// with no motion on it -- see DsuClient. Tried last, because it is the only
// one that depends on the player having installed something.
//
// All three are read into the one shape and the one convention -- SDL's, see
// DsMotionState -- so that everything downstream, on this device and on the
// far end, deals with a Wii U GamePad's motion rather than with whichever
// handheld happens to be standing in for one.
//
// Nothing here integrates anything. What comes out is the raw pair of
// readings and the moment they were taken; see DsMotionState for why that is
// deliberate.
// A Switch's six-axis unit reports in the console's own frame, which is not
// SDL's -- and everything downstream of DsMotionState is in SDL's (see the
// comment there). This is the conversion between them:
//
//     +x is right in both; libnx's +y points down the screen and +z into it,
//     where SDL's +y points up and +z out towards the player.
//
// Measured, on the third attempt, and the first two are worth recording
// because both were reasonable and both were wrong. Assuming the two frames
// agreed put gravity on the screen-normal axis, so a console held upright read
// as one lying flat -- which is what a Wii U game insisting the GamePad was
// "too flat" was reporting. Copying the shuffle out of SDL's own driver for
// this hardware (SDL_hidapi_switch.c) was wrong for a subtler reason: that
// driver reads a Joy-Con's raw HID report, whose axes sit in the Joy-Con's own
// body, and libnx hands back the console's frame instead. Same hardware,
// different starting point.
//
// What settled it was two readings from a real console, one pose each:
//
//   resting flat on a table, face up:  libnx (0.00, 0.00, -9.81)
//   held upright, tilted back ~16deg:  libnx (-0.75, -9.40, -2.72)
//
// An accelerometer at rest reads the up direction. Flat and face up, up is out
// of the screen, and libnx puts it on -z; held upright, up is towards the top
// of the console, and libnx puts it on -y with the tilt showing as a little
// more -z. Hence +y down and +z into the screen; x follows from those two,
// since a frame with +y down and +z into the screen is only right-handed if
// +x points right. Under SDL's convention those same two poses have to read
// (0, 0, +9.81) and (-0.75, +9.40, +2.72), which is exactly what negating y
// and z gives.
//
// Rotation does not use that same conversion, and assuming it did is what left
// a Wii U game steering the wrong way while tilting perfectly correctly.
//
//     rotation is (-x, -y, z) where acceleration is (x, -y, -z)
//
// libnx documents neither the units nor the axes of either reading -- the
// struct is six floats and three comments repeating their own field names --
// so this was settled by what three builds did on real hardware:
//
//   ( x, -y, -z)   tilting left and right reversed, otherwise fine
//   (-x,  y,  z)   never points the right way, drifts and is dragged back
//   ( x,  y, -z)   backwards on every axis
//
// Read against the mapping above, each of those is what it should be. The
// first has x and z wrong: pitch and roll, both of which whatever consumes
// this pulls back towards gravity, so they read as tilt going the wrong way
// rather than as anything unstable. The second has only y wrong: yaw, which
// nothing corrects, because no part of a six-axis reading knows which way a
// device is pointing -- so the heading wanders off and stays there. The third
// is the exact negative, and was reported in exactly those words.
//
// Only one assignment fits all three, and the useful check is that it is a
// rotation: its determinant is +1, a half turn about z. The two wrong guesses
// above both had determinant -1, which is a reflection, and no rigid frame
// differs from another by a reflection. That rules out half the candidates
// before any of them reaches hardware, and would have caught both.
//
// Why the two readings sit in frames a half turn apart is not something this
// can answer from outside. It does mean the tempting shortcut -- convert one
// and reuse it for the other -- is wrong, which is the mistake worth not
// repeating.
//
// Free and separate from the reader so both can be tested without a console.
struct SwitchSixAxis {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};
SwitchSixAxis switch_accel_to_sdl(const SwitchSixAxis& native);
SwitchSixAxis switch_gyro_to_sdl(const SwitchSixAxis& native);

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
    // Unlike the three above, this one carries its own timestamp: the server
    // knows when the sample was taken and this app only knows when it drained
    // the socket, which on a device sampling far quicker than a frame is not
    // the same instant.
    bool read_dsu_sensors();

    bool enabled_ = false;
    DsMotionState state_;
    // When the reading was last written to the log; see poll().
    uint32_t reported_at_ms_ = 0;
    // The controllers this has switched sensors on for, so they can be
    // switched off again and so one arriving mid-session is not missed.
    std::vector<SDL_GameController*> controllers_;
    // Standalone device sensors, opened only when no controller has any --
    // see the class comment.
    SDL_Sensor* device_gyro_ = nullptr;
    SDL_Sensor* device_accel_ = nullptr;
    // Opened lazily, and only once nothing on the device itself has answered
    // -- so a machine whose own sensors work never speaks to the network at
    // all.
    DsuClient dsu_;
    bool tried_dsu_ = false;
};

}  // namespace ds
