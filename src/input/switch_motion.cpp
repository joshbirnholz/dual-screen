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

// Deciding needs this much rotation to have gone past, in radians of arc,
// about each of the three axes. A second of ordinary handling clears it; a
// device left on a table never does, which is the point.
constexpr float kNeededArc = 1.0f;
// And the winner has to explain the movement this much better than whichever
// candidate came second. Rotations differ from one another by a lot, so a
// real answer wins comfortably; anything close means the movement so far
// cannot tell two of them apart.
constexpr float kNeededMargin = 2.0f;
constexpr std::size_t kNeededSamples = 60;
// A reading is only about gravity while nothing else is pushing the device.
// The unit vector towards the ground cannot swing faster than the device
// turns, so anything quicker than a very brisk wrist flick is a knock.
constexpr float kFastestTurn = 20.0f;
// Below this there is no rotation worth fitting to, only noise.
constexpr float kMovingRate = 0.02f;
// Sanity on the fitted unit: revolutions, radians and degrees a second are
// 6.28, 1 and 0.017 respectively, so anything outside this is not a unit.
constexpr float kSmallestScale = 0.002f;
constexpr float kLargestScale = 100.0f;
constexpr std::size_t kMaxSamples = 900;

}  // namespace

SwitchSixAxis switch_accel_to_sdl(const SwitchSixAxis& native) {
    // Right is right in both frames; up and out-of-the-screen are each the
    // other way round. See the header for the two readings this came from.
    SwitchSixAxis out;
    out.x = native.x;
    out.y = -native.y;
    out.z = -native.z;
    return out;
}

SwitchSixAxis switch_gyro_to_sdl(const SwitchSixAxis& native) {
    // A half turn about z, which is not the half turn about x that the
    // acceleration takes. The best of five guesses and still only a guess --
    // see the header, and MotionFitter for the thing that does not guess.
    SwitchSixAxis out;
    out.x = -native.x;
    out.y = -native.y;
    out.z = native.z;
    return out;
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

const std::vector<MotionAxes>& motion_axis_candidates() {
    static const std::vector<MotionAxes> candidates = [] {
        std::vector<MotionAxes> found;
        const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                  {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
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
                // Rotations only. A reflection describes a mirrored device.
                if (determinant == 1) found.push_back(axes);
            }
        }
        return found;
    }();
    return candidates;
}

std::string MotionFit::describe() const {
    if (!decided) return "undecided";
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

    float best_residual = 0.0f;
    float second_residual = 0.0f;
    bool have_best = false;
    for (const MotionAxes& axes : motion_axis_candidates()) {
        // For this candidate the prediction is linear in the scale, so the
        // best scale is one division rather than a search:
        //     predicted = s * p, where p = -(axes . w) x g
        //     s = sum(p . dg) / sum(p . p)
        double numerator = 0.0;
        double denominator = 0.0;
        for (std::size_t index = 0; index < count; ++index) {
            const SwitchSixAxis predicted =
                scaled(cross(axes.apply(rotation[index]), gravity[index]), -1.0f);
            numerator += dot(predicted, change[index]);
            denominator += dot(predicted, predicted);
        }
        if (denominator <= 1e-12) continue;
        const float scale = static_cast<float>(numerator / denominator);

        double residual = 0.0;
        for (std::size_t index = 0; index < count; ++index) {
            const SwitchSixAxis predicted =
                scaled(cross(axes.apply(rotation[index]), gravity[index]), -scale);
            residual += dot(difference(change[index], predicted),
                            difference(change[index], predicted));
        }

        // A negative scale is the same as the candidate with every sign
        // flipped, which is a reflection and therefore not among these -- so
        // it means this candidate is wrong rather than that the unit is.
        if (scale < kSmallestScale || scale > kLargestScale) continue;

        if (!have_best || residual < best_residual) {
            second_residual = have_best ? best_residual : 0.0f;
            best_residual = static_cast<float>(residual);
            fit.axes = axes;
            fit.scale = scale;
            have_best = true;
        } else if (second_residual == 0.0f || residual < second_residual) {
            second_residual = static_cast<float>(residual);
        }
    }

    if (!have_best || second_residual <= 0.0f) {
        return fit;
    }
    fit.margin = best_residual > 1e-12f ? second_residual / best_residual : kNeededMargin;
    fit.decided = fit.margin >= kNeededMargin;
    return fit;
}

void MotionFitter::reset() {
    samples_.clear();
    have_previous_ = false;
    seen_[0] = seen_[1] = seen_[2] = 0.0f;
    fit_ = MotionFit();
}

float MotionFitter::progress() const {
    float least = 1.0f;
    for (const float arc : seen_) {
        least = std::min(least, arc / kNeededArc);
    }
    return std::min(least, static_cast<float>(samples_.size()) /
                               static_cast<float>(kNeededSamples));
}

void MotionFitter::add(const SwitchSixAxis& gravity, const SwitchSixAxis& rotation,
                       float dt_seconds) {
    if (fit_.decided || dt_seconds <= 0.0f || dt_seconds > 0.25f) {
        return;
    }
    const float size = length(gravity);
    if (size <= 1e-6f) return;
    const SwitchSixAxis unit = scaled(gravity, 1.0f / size);

    const SwitchSixAxis previous_unit = previous_gravity_;
    const SwitchSixAxis previous_rotation = previous_rotation_;
    const bool had_previous = have_previous_;
    previous_gravity_ = unit;
    previous_rotation_ = rotation;
    have_previous_ = true;
    if (!had_previous) return;

    // Only while the device is being turned rather than shoved. An
    // accelerometer measuring a shove is not measuring the ground, and the
    // whole method rests on it measuring the ground -- so a reading whose
    // direction jumped further in one step than any hand could turn it is
    // one this cannot learn anything from.
    const SwitchSixAxis change = scaled(difference(unit, previous_unit), 1.0f / dt_seconds);
    if (length(change) > kFastestTurn) return;

    const SwitchSixAxis middle_rotation = scaled(
        SwitchSixAxis{rotation.x + previous_rotation.x, rotation.y + previous_rotation.y,
                      rotation.z + previous_rotation.z},
        0.5f);
    if (length(middle_rotation) < kMovingRate) return;

    const SwitchSixAxis middle_gravity = normalized(
        SwitchSixAxis{unit.x + previous_unit.x, unit.y + previous_unit.y, unit.z + previous_unit.z});

    for (int axis = 0; axis < 3; ++axis) {
        seen_[axis] += std::abs(component(middle_rotation, axis)) * dt_seconds;
    }
    if (samples_.size() < kMaxSamples) {
        samples_.push_back({middle_gravity, change, middle_rotation});
    }

    // Solving is cheap but not free, and nothing can be decided before there
    // is movement about every axis to decide it from.
    if (samples_.size() < kNeededSamples || samples_.size() % 15 != 0) return;
    for (const float arc : seen_) {
        if (arc < kNeededArc) return;
    }
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
