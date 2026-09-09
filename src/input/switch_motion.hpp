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
// What is solved for is deliberately not a general matrix: one of the
// forty-eight signed permutations, and a single positive scale for the units.
// Each has a one-parameter least squares for the scale, so trying all of them
// and keeping the best is cheap.
//
// Forty-eight, not twenty-four. An earlier version searched only the ones with
// determinant +1, on the grounds that a frame differs from a frame by a
// rotation and never by a reflection. That is true of frames and beside the
// point here, because what separates these two readings is not only their axes
// but their *sign convention* -- which way round each of them calls positive.
// A gyroscope reported clockwise-positive rather than by the right-hand rule
// is negated on all three axes, which is a determinant of -1 and a perfectly
// ordinary thing for a vendor to do. Restricting to rotations can therefore
// exclude the true answer outright, and the console this was written for turns
// out to report acceleration that way -- pointing at the ground rather than at
// the sky, which is exactly such a negation.
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
// That is not a hypothetical. The accelerometer this was solved against was a
// quarter turn out for months, the rotation fitted itself to match, and the
// pair stayed perfectly consistent -- good enough to aim a bow with in one
// game while another's camera hung upside down. What ended it was three named
// poses, held with a device already known to be right, and nothing else could
// have: see switch_accel_to_sdl().
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
//     SDL's reading is libnx's negated and turned a quarter of a turn
//     about x: (-x, -z, y).
//
// Two separate things are going on in that, and separating them is what took
// so long.
//
// The negation is the easy half. What libnx reports is the direction of the
// *ground*, where SDL reports the direction of up. Three poses on a real
// console say so and only this reading fits all three:
//
//   flat on a table, face up     libnx (0.00, 0.00, -1.00)
//   held upright                 libnx (0.00, -0.98, -0.21)
//   upright, right edge dipped   libnx (0.60, -0.74, -0.34)
//
// The quarter turn is the hard half, and it is a misreading of SDL's own
// documentation rather than anything about a Switch. SDL defines these axes
// for "phones held in portrait mode and game controllers held in front of
// you", and libnx's axes are the phone reading of that: +y up the screen, +z
// out of the screen. A controller's are not the same, because a controller's
// "top" is its top *face*, not its top edge -- so +y comes out of the face
// and +z runs down towards the handles. A device that is both, like a Switch
// or a Steam Deck, has to pick one, and everything downstream here wants the
// controller one:
//
//   flat on a table, face up     up is out of the face, so +y
//   held upright, screen at you  up is towards the top edge, so -z
//   upright, right edge dipped   up leans left, so -x
//
// Which is measured, not argued: a Steam Deck, whose motion has been correct
// throughout, was held against this console in exactly those three poses and
// read (0.30, 9.88, 0.04), (0.80, 0.67, -9.69) and (-5.72, 1.18, -7.80) while
// the console, converted the old way, read (0.16, 1.13, 10.05),
// (1.11, 9.69, -0.25) and (-5.35, 8.25, 0.44). One mapping takes each of
// those to its neighbour, and it is (x, z, -y).
//
// That quarter turn is why aiming in a game worked while "up" did not. The
// rotation reading was fitted against this one, so it inherited the same turn
// and the two stayed consistent with each other -- consistent enough to aim
// with, and both of them a quarter turn from the truth. A game that only
// integrates rotation (Wind Waker's telescope, Majora's Mask's bow) never
// notices; one that reads gravity outright (Nintendo Land's plaza camera, its
// Donkey Kong tilt game) is wrong from the first frame.
SwitchSixAxis switch_accel_to_sdl(const SwitchSixAxis& native);

// And rotation, which is the same quarter turn without the negation.
//
//     rotation is (x, z, -y), in revolutions a second.
//
// So the rotation conversion is still the negative of the acceleration one,
// which is the same relationship Cemu's DSU and SDL readers have with each
// other, and the same one Azahar's SDL path has -- three independent places,
// all agreeing that an accelerometer and a gyroscope from the same part are
// handed over pointing opposite ways.
//
// The unit is the only other thing it needs, and that is tau: a hand turning
// the console peaks around a fifth of a turn a second, and the console's own
// accelerometer, watched by MotionFitter while the ground swung through its
// frame, put the scale at 6.166 of a radian a second -- tau to within two
// percent. The A/B against a Steam Deck agreed independently, measuring the
// converted rotation at 1.016 of the Deck's own radians a second.
//
// That A/B is also what found the quarter turn, and the way it found it is
// worth keeping. It measured acceleration differing from the Deck's by
// (x, z, -y) and rotation differing by the same (x, z, -y). Equal, where
// earlier rounds had them disagreeing -- and equal is what says the two
// conversions are consistent with each other, since all that is left between
// two devices held as one is how they were held. It was a mistake to read
// that agreement as correctness: a mounting can only be divided out, never
// seen, so an error the two conversions share survives it untouched. Both
// were a quarter turn out, together, and it took reading a *known* pose off
// the device that was already right to see it.
SwitchSixAxis switch_gyro_to_sdl(const SwitchSixAxis& native);

// One mapping of the axes, as a signed permutation: for each output component,
// which input component it comes from and with which sign. Both parities --
// see the note above about why restricting to rotations is wrong here.
struct MotionAxes {
    int8_t source[3] = {0, 1, 2};
    int8_t sign[3] = {1, 1, 1};

    SwitchSixAxis apply(const SwitchSixAxis& in) const;
    // The mapping that undoes this one, and the one that applies `first` and
    // then this. Together they let two measured mappings be divided by one
    // another, which is how a difference that does not depend on how two
    // devices happened to be held is got at.
    MotionAxes inverse() const;
    MotionAxes after(const MotionAxes& first) const;
    // Written the way this file's comments and commit messages write them,
    // e.g. "(-x, -y, z)".
    std::string describe() const;
    bool operator==(const MotionAxes& other) const;
};

// All forty-eight.
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
    // The largest turn seen so far, so a sample with no turn in it can be
    // recognised without knowing the unit. See kQuietRotation.
    float loudest_rotation_ = 0.0f;
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
