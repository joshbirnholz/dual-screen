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
//     SDL's reading is the negative of libnx's, on all three axes.
//
// libnx's axes are SDL's -- +x right, +y up, +z out of the screen -- and what
// it reports along them is the direction of the *ground*, where SDL reports
// the direction of up. Nothing says so; three poses on a real console do, and
// only this reading fits all three:
//
//   flat on a table, face up     libnx (0.00, 0.00, -1.00)
//   held upright                 libnx (0.00, -0.98, -0.21)
//   upright, right edge dipped   libnx (0.60, -0.74, -0.34)
//
// Face up, the ground is through the screen, which is -z. Upright, it is
// towards the bottom of the screen, which is -y. Dip the right-hand edge and
// it leans to the right, which is +x. Three poses, one story.
//
// The first two of those were measured a long time before the third, and two
// poses cannot see the third axis: both leave x free, because x is near zero
// in each. That gap was filled with reasoning instead -- that a frame with +y
// down and +z into the screen must have +x to the right to stay right-handed
// -- which is true of a frame and false here, because this is not a change of
// frame at all. It is the same frame reporting the opposite vector. Every
// rotation answer fitted against it inherited the mirror.
SwitchSixAxis switch_accel_to_sdl(const SwitchSixAxis& native);

// And rotation, which needs nothing done to it but its unit.
//
//     rotation is (x, y, z), in revolutions a second.
//
// So the rotation conversion is the negative of the acceleration one, which is
// the same relationship Cemu's DSU and SDL readers have with each other, and
// the same one Azahar's SDL path has -- three independent places, all agreeing
// that an accelerometer and a gyroscope from the same part are handed over
// pointing opposite ways.
//
// This took eight attempts, and none of the first seven could have found it,
// because they were all searching the rotation while the *acceleration* was
// mirrored on x. Every rotation fitted against a mirrored frame inherits the
// mirror, so the whole space was wrong and walking all forty-eight of it found
// nothing -- which is what finally said the thing being varied was not the
// thing that was broken.
//
// What settled it was two measurements that share no machinery:
//
//   The console's own accelerometer, once x was fixed. MotionFitter watched
//   the ground swing through the frame and reported (x, y, z) at 6.166 of a
//   radian a second, which is tau to within two percent.
//
//   A Steam Deck, whose motion has been right throughout, held against this
//   console and moved with it. MotionComparison found acceleration differing
//   by (x, z, -y) and rotation by (-x, z, y) -- and those disagreeing is the
//   whole point. Had both conversions been right the two would be equal, each
//   being nothing but how the devices were held relative to each other. The
//   difference between them is the error, and dividing one out of the other
//   leaves (x, y, z) exactly.
SwitchSixAxis switch_gyro_to_sdl(const SwitchSixAxis& native);

// One mapping of the axes, as a signed permutation: for each output component,
// which input component it comes from and with which sign. Both parities --
// see the note above about why restricting to rotations is wrong here.
struct MotionAxes {
    int8_t source[3] = {0, 1, 2};
    int8_t sign[3] = {1, 1, 1};

    SwitchSixAxis apply(const SwitchSixAxis& in) const;
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
