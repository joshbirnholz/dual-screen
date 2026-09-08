#include "input/switch_motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ds {
namespace {

SwitchSixAxis cross(const SwitchSixAxis& a, const SwitchSixAxis& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(const SwitchSixAxis& a, const SwitchSixAxis& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const SwitchSixAxis& v) { return std::sqrt(dot(v, v)); }

SwitchSixAxis normalized(const SwitchSixAxis& v) {
    const float size = length(v);
    if (size <= 1e-6f) return {0.0f, 0.0f, 0.0f};
    return {v.x / size, v.y / size, v.z / size};
}

SwitchSixAxis scaled(const SwitchSixAxis& v, float by) { return {v.x * by, v.y * by, v.z * by}; }

SwitchSixAxis difference(const SwitchSixAxis& a, const SwitchSixAxis& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

float component(const SwitchSixAxis& v, int axis) {
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

// Deciding needs the direction of the ground to have travelled this far
// through the device's frame, in radians.
//
// Measured off the accelerometer rather than off the rotation reading, which
// is the correction to a first version that never fired once on hardware. It
// counted the rotation reading's own magnitude against a threshold written in
// radians -- but the whole point of this class is that the unit of that
// reading is unknown, and the console turned out to report revolutions, so
// "one radian" silently asked for a full turn about each of three axes.
// Nobody does that with a games console.
//
// The arc the ground travels is unitless and cannot be got wrong that way. It
// is also the better measure: it is exactly the movement this can learn from,
// since rotation about the axis pointing at the ground moves the ground not at
// all and says nothing about anything.
constexpr float kNeededArc = 0.7f;
// And the winner has to explain the movement this much better than whichever
// candidate came second. Rotations differ from one another by a lot, so a
// real answer wins comfortably; anything close means the movement so far
// cannot tell two of them apart.
constexpr float kNeededMargin = 2.0f;
constexpr std::size_t kNeededSamples = 40;
// A reading is only about gravity while nothing else is pushing the device.
// The unit vector towards the ground cannot swing faster than the device
// turns, so anything quicker than a very brisk wrist flick is a knock.
constexpr float kFastestTurn = 20.0f;
// And how far the accelerometer's own magnitude may stray from what the device
// weighs at rest before the reading is about a hand rather than about the
// ground.
constexpr float kRestingTolerance = 0.08f;
// How far apart the two readings differenced for dg/dt are, in seconds, and
// how many are kept to find such a pair. See the comment where these are used.
constexpr float kBaselineSeconds = 0.08f;
constexpr std::size_t kHistoryDepth = 32;
// Below this the ground is not moving through the device's frame, so the
// reading is noise however large the rotation alongside it looks. In radians a
// second, off the accelerometer, for the same reason as kNeededArc.
constexpr float kMovingRate = 0.05f;
// And below this fraction of the loudest turn seen so far, the rotation
// reading is not saying anything either -- see where it is used. A fraction
// rather than a number, since the unit is the thing being solved for.
constexpr float kQuietRotation = 0.1f;
// Sanity on the fitted unit: revolutions, radians and degrees a second are
// 6.28, 1 and 0.017 respectively, so anything outside this is not a unit.
constexpr float kSmallestScale = 0.002f;
constexpr float kLargestScale = 100.0f;
constexpr std::size_t kMaxSamples = 900;

}  // namespace

SwitchSixAxis switch_accel_to_sdl(const SwitchSixAxis& native) {
    // Same axes, opposite vector: libnx points at the ground where SDL points
    // at the sky. See the header for the three poses that settle it.
    SwitchSixAxis out;
    out.x = -native.x;
    out.y = -native.y;
    out.z = -native.z;
    return out;
}

SwitchSixAxis switch_gyro_to_sdl(const SwitchSixAxis& native) {
    // Nothing at all: the axes are already the ones everything downstream
    // uses, and only the unit differs. Measured twice over, by two methods
    // sharing no machinery -- see the header.
    return native;
}

SwitchSixAxis MotionAxes::apply(const SwitchSixAxis& in) const {
    SwitchSixAxis out;
    out.x = sign[0] * component(in, source[0]);
    out.y = sign[1] * component(in, source[1]);
    out.z = sign[2] * component(in, source[2]);
    return out;
}

bool MotionAxes::operator==(const MotionAxes& other) const {
    for (int axis = 0; axis < 3; ++axis) {
        if (source[axis] != other.source[axis] || sign[axis] != other.sign[axis]) return false;
    }
    return true;
}

std::string MotionAxes::describe() const {
    const char* names = "xyz";
    std::string text = "(";
    for (int axis = 0; axis < 3; ++axis) {
        if (axis != 0) text += ", ";
        if (sign[axis] < 0) text += "-";
        text += names[source[axis]];
    }
    return text + ")";
}

namespace {

std::vector<MotionAxes> build_signed_permutations(int wanted_determinant) {
    std::vector<MotionAxes> found;
    const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    for (const auto& order : orders) {
        for (int bits = 0; bits < 8; ++bits) {
            MotionAxes axes;
            int determinant = 1;
            // A permutation's own sign, by counting the swaps that undo it.
            int permutation[3] = {order[0], order[1], order[2]};
            for (int i = 0; i < 3; ++i) {
                for (int j = i + 1; j < 3; ++j) {
                    if (permutation[i] > permutation[j]) {
                        std::swap(permutation[i], permutation[j]);
                        determinant = -determinant;
                    }
                }
            }
            for (int axis = 0; axis < 3; ++axis) {
                axes.source[axis] = static_cast<int8_t>(order[axis]);
                axes.sign[axis] = ((bits >> axis) & 1) != 0 ? -1 : 1;
                determinant *= axes.sign[axis];
            }
            if (determinant == wanted_determinant) found.push_back(axes);
        }
    }
    return found;
}

// The best scale for one candidate is a division rather than a search, since
// the prediction is linear in it:
//     predicted = s * p, where p = -(axes . w) x g
//     s = sum(p . dg) / sum(p . p)
struct Attempt {
    float scale = 0.0f;
    double residual = 0.0;
    bool usable = false;
};

Attempt try_candidate(const MotionAxes& axes, const std::vector<SwitchSixAxis>& gravity,
                      const std::vector<SwitchSixAxis>& change,
                      const std::vector<SwitchSixAxis>& rotation, std::size_t count) {
    Attempt attempt;
    double numerator = 0.0;
    double denominator = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
        const SwitchSixAxis predicted =
            scaled(cross(axes.apply(rotation[index]), gravity[index]), -1.0f);
        numerator += dot(predicted, change[index]);
        denominator += dot(predicted, predicted);
    }
    if (denominator <= 1e-12) return attempt;
    attempt.scale = static_cast<float>(numerator / denominator);
    // A negative scale is the candidate with every sign flipped, which is the
    // other parity and therefore not this list's business.
    if (attempt.scale < kSmallestScale || attempt.scale > kLargestScale) return attempt;

    for (std::size_t index = 0; index < count; ++index) {
        const SwitchSixAxis predicted =
            scaled(cross(axes.apply(rotation[index]), gravity[index]), -attempt.scale);
        attempt.residual +=
            dot(difference(change[index], predicted), difference(change[index], predicted));
    }
    attempt.usable = true;
    return attempt;
}

}  // namespace

const std::vector<MotionAxes>& motion_axis_candidates() {
    static const std::vector<MotionAxes> all = [] {
        std::vector<MotionAxes> both = build_signed_permutations(1);
        const std::vector<MotionAxes> flipped = build_signed_permutations(-1);
        both.insert(both.end(), flipped.begin(), flipped.end());
        return both;
    }();
    return all;
}

std::string MotionFit::describe() const {
    if (!decided) {
        char text[128];
        std::snprintf(text, sizeof(text), "undecided; %s leads, but only by %.1fx",
                      axes.describe().c_str(), static_cast<double>(margin));
        return text;
    }
    char text[128];
    std::snprintf(text, sizeof(text), "%s at %.3f (%.1fx clearer than the next)",
                  axes.describe().c_str(), static_cast<double>(scale),
                  static_cast<double>(margin));
    return text;
}

MotionFit fit_motion_axes(const std::vector<SwitchSixAxis>& gravity,
                          const std::vector<SwitchSixAxis>& change,
                          const std::vector<SwitchSixAxis>& rotation) {
    MotionFit fit;
    const std::size_t count = std::min({gravity.size(), change.size(), rotation.size()});
    if (count < kNeededSamples) {
        return fit;
    }

    double best = 0.0;
    double second = 0.0;
    bool have_best = false;
    for (const MotionAxes& axes : motion_axis_candidates()) {
        const Attempt attempt = try_candidate(axes, gravity, change, rotation, count);
        if (!attempt.usable) continue;
        if (!have_best || attempt.residual < best) {
            second = have_best ? best : 0.0;
            best = attempt.residual;
            fit.axes = axes;
            fit.scale = attempt.scale;
            have_best = true;
        } else if (second == 0.0 || attempt.residual < second) {
            second = attempt.residual;
        }
    }

    if (!have_best || second <= 0.0) {
        return fit;
    }
    fit.margin = best > 1e-12 ? static_cast<float>(second / best) : kNeededMargin;
    fit.decided = fit.margin >= kNeededMargin;

    return fit;
}

void MotionFitter::reset() {
    samples_.clear();
    history_.clear();
    elapsed_ = 0.0f;
    at_rest_ = 0.0f;
    loudest_rotation_ = 0.0f;
    seen_arc_ = 0.0f;
    fit_ = MotionFit();
}

float MotionFitter::progress() const {
    return std::min({1.0f, seen_arc_ / kNeededArc,
                     static_cast<float>(samples_.size()) /
                         static_cast<float>(kNeededSamples)});
}

void MotionFitter::add(const SwitchSixAxis& gravity, const SwitchSixAxis& rotation,
                       float dt_seconds) {
    if (fit_.decided || dt_seconds <= 0.0f || dt_seconds > 0.25f) {
        return;
    }
    const float size = length(gravity);
    if (size <= 1e-6f) return;
    elapsed_ += dt_seconds;

    // How heavy the device feels on average, which is gravity plus whatever
    // the hand holding it is doing. Tracked slowly so one shove does not move
    // it.
    at_rest_ = at_rest_ <= 0.0f ? size : at_rest_ + (size - at_rest_) * 0.02f;

    // A reading is only about the ground while nothing else is pushing the
    // device, and the readings with the most rotation in them are exactly the
    // ones a hand is also shoving -- a tenth of the liveliest samples in a
    // real session were out by more than this. Keeping them is what left the
    // solver unable to separate any candidate from any other: the movement it
    // was being asked to explain was mostly somebody's wrist.
    if (std::abs(size - at_rest_) > kRestingTolerance * at_rest_) {
        history_.clear();  // the run is broken; do not difference across it
        return;
    }

    history_.push_back({scaled(gravity, 1.0f / size), rotation, elapsed_});
    while (history_.size() > kHistoryDepth) history_.erase(history_.begin());

    // Differenced across a window rather than between neighbours. The ground's
    // direction is measured, not given, so differencing two readings 8ms apart
    // divides their noise by 8ms as well -- and that noise is what the solver
    // was drowning in. Over a longer baseline the turn grows and the noise
    // does not.
    const Entry& now = history_.back();
    const Entry* then = nullptr;
    for (const Entry& entry : history_) {
        if (now.at - entry.at >= kBaselineSeconds) then = &entry;
    }
    if (then == nullptr) return;

    const float span = now.at - then->at;
    const SwitchSixAxis change = scaled(difference(now.gravity, then->gravity), 1.0f / span);
    const float swing = length(change);
    if (swing > kFastestTurn || swing < kMovingRate) return;

    // The rotation over the same window, averaged, so the two sides of the
    // equation describe the same stretch of time.
    SwitchSixAxis total_rotation;
    int counted = 0;
    for (const Entry& entry : history_) {
        if (entry.at < then->at) continue;
        total_rotation = SwitchSixAxis{total_rotation.x + entry.rotation.x,
                                       total_rotation.y + entry.rotation.y,
                                       total_rotation.z + entry.rotation.z};
        ++counted;
    }
    if (counted == 0) return;
    const SwitchSixAxis middle_rotation = scaled(total_rotation, 1.0f / counted);

    // And there has to be a rotation reading to fit. A still console still
    // wobbles: its acceleration drifts by about a hundredth of a g between
    // reports, which across the window above is a tenth of a radian a second
    // of apparent turning -- comfortably past the movement gate, and pure
    // noise. Those samples predict nothing for every candidate alike, so every
    // candidate scores the same on them, and enough of them drag the margin to
    // exactly 1.0 however good the real samples are. Which is what a real
    // session did: ninety seconds of "undecided, leading by 1.0x".
    //
    // Measured against the loudest turn seen so far rather than against a
    // number, since the unit is the thing being solved for.
    loudest_rotation_ = std::max(loudest_rotation_, length(middle_rotation));
    if (length(middle_rotation) < loudest_rotation_ * kQuietRotation) return;
    const SwitchSixAxis middle_gravity = normalized(SwitchSixAxis{
        now.gravity.x + then->gravity.x, now.gravity.y + then->gravity.y,
        now.gravity.z + then->gravity.z});

    seen_arc_ += swing * dt_seconds;
    if (samples_.size() < kMaxSamples) {
        samples_.push_back({middle_gravity, change, middle_rotation});
    }

    // Solving is cheap but not free, and there is nothing to decide from until
    // the ground has moved about the frame. Whether it has moved in enough
    // *directions* is not checked here and does not need to be: movement
    // confined to one plane leaves the candidates that differ only outside it
    // fitting equally well, which shows up as a margin too small to decide on.
    // The solver already refuses that case, so it is the one gate rather than
    // two.
    if (samples_.size() < kNeededSamples || samples_.size() % 15 != 0) return;
    if (seen_arc_ < kNeededArc) return;
    solve();
}

void MotionFitter::solve() {
    std::vector<SwitchSixAxis> gravity;
    std::vector<SwitchSixAxis> change;
    std::vector<SwitchSixAxis> rotation;
    gravity.reserve(samples_.size());
    change.reserve(samples_.size());
    rotation.reserve(samples_.size());
    for (const Sample& sample : samples_) {
        gravity.push_back(sample.gravity);
        change.push_back(sample.change);
        rotation.push_back(sample.rotation);
    }
    fit_ = fit_motion_axes(gravity, change, rotation);
}

}  // namespace ds
