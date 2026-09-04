#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "input/ds_input.hpp"
#include "input/switch_motion.hpp"

namespace ds {

// Holds one device's motion reading up against another's and works out what
// stands between them.
//
// This exists because a Switch's six-axis frame took seven rounds of guessing
// and never came out right, while a Steam Deck's has been correct throughout.
// Every one of those rounds tried to derive the answer from first principles
// and then asked a person how a game felt, which is a slow, lossy channel: a
// wrong scale and a wrong sign feel identical through it, and so do a wrong
// axis and a wrong handedness.
//
// A device that is already right is a far better instrument than any of that.
// Hold the two together, screens facing the same way, and move them as one:
// they are then in the same orientation undergoing the same rotation, so
// whatever maps one reading onto the other is exactly the conversion that is
// wrong. No poses to describe, no feel to report, no physics to be clever
// about -- just two readings of one motion.
//
// The host is where this belongs, because the host already has both at the
// same instant: its own, and the companion's as it arrives. Nothing has to be
// lined up after the fact from two logs and two clocks.
//
// What comes out is a signed permutation and a scale per reading -- the same
// small space MotionAxes describes, for the same reason -- fitted directly
// rather than through the accelerometer, which is what makes it well
// conditioned. Both vectors are known here; there is no cross product and
// nothing unobservable.
class MotionComparison {
public:
    struct Answer {
        bool decided = false;
        MotionAxes axes;
        // Multiplies the subject's reading, after the axes, to land on the
        // reference's. 1 means the two agree about units.
        float scale = 1.0f;
        // How much better this beat the runner-up, as a ratio of residuals.
        float margin = 0.0f;
        std::string describe() const;
    };

    // One pair, taken at the same moment. `reference` is the device known to
    // be right; `subject` is the one being characterised.
    void add(const DsMotionState& reference, const DsMotionState& subject);

    // Says what it has, roughly once a second, and both readings whenever they
    // are worth looking at -- the raw pairs are the point, since they let the
    // answer be checked by hand rather than trusted.
    void report(uint32_t now_ms);

    const Answer& acceleration() const { return acceleration_; }
    const Answer& rotation() const { return rotation_; }
    void reset();

private:
    struct Pair {
        SwitchSixAxis reference;
        SwitchSixAxis subject;
    };
    std::vector<Pair> acceleration_pairs_;
    std::vector<Pair> rotation_pairs_;
    Answer acceleration_;
    Answer rotation_;
    DsMotionState last_reference_;
    DsMotionState last_subject_;
    bool have_last_ = false;
    uint32_t reported_at_ms_ = 0;
    bool said_answer_ = false;
};

// The fit itself, over pairs already collected. Separate so it can be given
// two synthetic devices that disagree in a known way and asked to name the
// disagreement.
MotionComparison::Answer compare_motion_axes(const std::vector<SwitchSixAxis>& reference,
                                             const std::vector<SwitchSixAxis>& subject);

}  // namespace ds
