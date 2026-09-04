#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// Works out how a device's rotation reading relates to its acceleration one,
// by watching the two disagree.
//
// This exists because a Switch would not say. libnx returns six floats and
// documents neither the axes nor the units of any of them, and five builds
// guessed -- each one reasoning about what a reading ought to look like, each
// one shipped to hardware, each one wrong in a way that read as a different
// bug. Somewhere in there the reports stopped being evidence: a rotation
// reading that is six times too large makes every orientation wrong all the
// time, and "tilt is reversed" and "it drifts" are both just what chaos looks
// like from the outside. There is no way to tell a wrong sign from a wrong
// scale by asking someone how a game feels.
//
// So this stops asking. An accelerometer at rest points at the ground, and
// while a device turns, the direction it points turns with it -- in the
// device's own frame, exactly as fast and exactly as far. That is a physical
// identity, not a convention:
//
//     dg/dt = -w x g
//
// for g the unit vector towards the ground and w the true rotation, both in
// the device's frame. The left-hand side comes from the accelerometer, which
// is known good here because it is what makes a still device read upright.
// The right-hand side is the rotation reading, in whatever frame and units the
// device felt like using. Everything unknown is on one side, and the equation
// says what it has to be.
//
// What is solved for is deliberately not a general matrix. A frame differs
// from a frame by a rotation, so the answer is one of the twenty-four signed
// permutations with determinant +1 -- a reflection would describe a mirrored
// device -- and a single positive scale for the units. Twenty-four candidates,
// each with a one-parameter least squares for the scale, is small enough to
// simply try all of them and keep the best.
//
// What it cannot do is check the frame it is solved against. Everything here
// is relative to the accelerometer, so if the accelerometer's own axes are
// wrong the rotation is fitted to be consistent with them and the whole
// orientation comes out wrong together -- steadily, with nothing fighting
// anything, which is the hardest kind of wrong to see. It was tempting to
// think a mirrored accelerometer frame would at least show up as nothing
// fitting, since a reflection changes the handedness of a cross product; it
// does not. Mirror the accelerometer and the rotation that fits is simply a
// different rotation, because angular velocity picks up the reflection's sign
// too and the two cancel. There is a test for that, so the idea does not get
// had twice. Settling it needs one reading in a known pose, which is a
// question for a person and not for this.
//
// It also has to know when it does not know. A device lying still says
// nothing, and a turn about the axis pointing at the ground says nothing about
// that axis, since a vector crossed with itself is zero. So a fit is only
// reported once enough real movement has gone past, in enough directions, and
// once one candidate explains it clearly better than the next best.

// A six-axis reading: acceleration or rotation, three components, in whatever
// frame and unit the thing that produced it happened to use.
struct SwitchSixAxis {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// A Switch's acceleration, into the frame everything downstream uses.
//
//     +x is right in both; libnx's +y points down the screen and +z into it,
//     where SDL's +y points up and +z out towards the player.
//
// Measured, from two poses on a real console:
//
//   resting flat on a table, face up:  libnx (0.00, 0.00, -9.81)
//   held upright, tilted back ~16deg:  libnx (-0.75, -9.40, -2.72)
//
// An accelerometer at rest reads the up direction. Flat and face up, up is out
// of the screen, and libnx puts it on -z; held upright, up is towards the top
// of the console, and libnx puts it on -y with the tilt showing as a little
// more -z. Hence +y down and +z into the screen; x follows from those two,
// since a frame with +y down and +z into the screen is only right-handed if
// +x points right.
SwitchSixAxis switch_accel_to_sdl(const SwitchSixAxis& native);

// And rotation, for a console that has not been moved enough for the fit
// below to have worked it out yet.
//
// This is the fifth guess at it and there will not be a sixth. The first four
// each reasoned about what a reading ought to look like, shipped, and came
// back wrong in a way that read as a different bug -- and the fifth is only
// here as something to use for the second or two before the accelerometer
// answers the question properly. Reaching for a constant is what made this a
// five-round problem; see MotionFitter for what replaced it.
SwitchSixAxis switch_gyro_to_sdl(const SwitchSixAxis& native);

// One rotation of the axes, as a signed permutation: for each output
// component, which input component it comes from and with which sign.
struct MotionAxes {
    int8_t source[3] = {0, 1, 2};
    int8_t sign[3] = {1, 1, 1};

    SwitchSixAxis apply(const SwitchSixAxis& in) const;
    // Written the way this file's comments and commit messages write them,
    // e.g. "(-x, -y, z)".
    std::string describe() const;
    bool operator==(const MotionAxes& other) const;
};

// The twenty-four that are rotations rather than reflections.
const std::vector<MotionAxes>& motion_axis_candidates();

struct MotionFit {
    // False until there has been enough movement to be sure.
    bool decided = false;
    MotionAxes axes;
    // Multiplies the reading after the axes are applied, turning whatever unit
    // the device reports into radians a second.
    float scale = 1.0f;
    // How much better the winner explained the movement than the runner-up,
    // as a ratio of residuals. Larger is more certain; the threshold for
    // deciding at all is inside.
    float margin = 0.0f;
    std::string describe() const;
};

// Accumulates readings and reports the fit once it has one.
//
// Fed the acceleration already in the frame everything downstream uses, and
// the rotation exactly as the device reported it -- that is the whole point,
// since the conversion between them is what is being solved for.
class MotionFitter {
public:
    // `gravity` need not be normalised or in any particular unit; only its
    // direction is used, and samples where the device is being shaken rather
    // than turned are dropped, since the accelerometer stops pointing at the
    // ground when it is also measuring a shove.
    void add(const SwitchSixAxis& gravity, const SwitchSixAxis& rotation, float dt_seconds);
    const MotionFit& fit() const { return fit_; }
    // How much usable movement has gone past, as a fraction of what deciding
    // needs. Only for reporting progress.
    float progress() const;
    void reset();

private:
    struct Sample {
        SwitchSixAxis gravity;      // unit, midway between two readings
        SwitchSixAxis change;       // dg/dt
        SwitchSixAxis rotation;     // as reported, midway between two readings
    };
    void solve();

    // A recent reading, kept only long enough to be differenced against one
    // far enough in the past to be worth differencing against.
    struct Entry {
        SwitchSixAxis gravity;   // unit
        SwitchSixAxis rotation;  // as reported
        float at = 0.0f;         // seconds since this fitter started
    };

    std::vector<Sample> samples_;
    std::vector<Entry> history_;
    float elapsed_ = 0.0f;
    // What the device weighs when nothing but gravity is acting on it, in
    // whatever unit the accelerometer reports. Learned rather than assumed,
    // since this never sees the unit.
    float at_rest_ = 0.0f;
    // How far the ground has travelled through the device's frame, in
    // radians. Unitless, unlike the rotation reading, which is the point --
    // see kNeededArc.
    float seen_arc_ = 0.0f;
    MotionFit fit_;
};

// The solver itself, over samples already prepared. Separate so it can be
// given synthetic readings from a known device and asked to find it again,
// which is the test this whole problem never had.
MotionFit fit_motion_axes(const std::vector<SwitchSixAxis>& gravity,
                          const std::vector<SwitchSixAxis>& change,
                          const std::vector<SwitchSixAxis>& rotation);

}  // namespace ds
