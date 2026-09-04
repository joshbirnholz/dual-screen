#include "input/motion_compare.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "util/log.hpp"

namespace ds {
namespace {

float dot(const SwitchSixAxis& a, const SwitchSixAxis& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length(const SwitchSixAxis& v) { return std::sqrt(dot(v, v)); }

// The fewest pairs worth fitting at all. A signed permutation is a small thing
// to determine and two dozen spread-out readings settle it; what stops a wrong
// answer is the margin below, not the count.
constexpr std::size_t kMinimumPairs = 24;
// What the accumulator waits for, which is not the same thing. Turns arrive
// sixty a second while the pair is being moved, so plenty of them cost
// nothing. Poses have to be reached one at a time by actually holding the pair
// in a new orientation, and each must be a real distance from every one
// already held -- at that spacing only a few hundred exist on the whole
// sphere, so asking for ninety would mean tumbling the pair through most
// orientations there are. Thirty is a comfortable half-minute of turning it
// over in the hands, and is already more than the fit needs.
constexpr std::size_t kNeededPoses = 30;
constexpr std::size_t kNeededTurns = 90;
constexpr float kNeededMargin = 2.0f;
// Below this a rotation pair is two devices sitting still, which agrees with
// every candidate equally and says nothing. In radians a second, on the
// reference, which is the device whose units are known.
constexpr float kMovingRate = 0.35f;
// Acceleration is always present, so the only pairs worth keeping are ones
// that differ from what has already been seen -- otherwise a device left on a
// table fills the buffer with one pose.
constexpr float kNewPose = 1.5f;
constexpr std::size_t kMaxPairs = 600;
constexpr float kSmallestScale = 0.002f;
constexpr float kLargestScale = 500.0f;

}  // namespace

std::string MotionComparison::Answer::describe() const {
    if (!decided) return "not yet";
    char text[128];
    std::snprintf(text, sizeof(text), "%s times %.3f (%.1fx clearer than the next)",
                  axes.describe().c_str(), static_cast<double>(scale),
                  static_cast<double>(margin));
    return text;
}

MotionComparison::Answer compare_motion_axes(const std::vector<SwitchSixAxis>& reference,
                                             const std::vector<SwitchSixAxis>& subject) {
    MotionComparison::Answer answer;
    const std::size_t count = std::min(reference.size(), subject.size());
    if (count < kMinimumPairs) {
        return answer;
    }

    double best = 0.0;
    double second = 0.0;
    bool have_best = false;
    for (const MotionAxes& axes : motion_axis_candidates()) {
        // The prediction is linear in the scale, so the best scale is a
        // division rather than a search: reference ~= s * (axes . subject).
        double numerator = 0.0;
        double denominator = 0.0;
        for (std::size_t index = 0; index < count; ++index) {
            const SwitchSixAxis turned = axes.apply(subject[index]);
            numerator += dot(turned, reference[index]);
            denominator += dot(turned, turned);
        }
        if (denominator <= 1e-12) continue;
        const float scale = static_cast<float>(numerator / denominator);
        if (scale < kSmallestScale || scale > kLargestScale) continue;

        double residual = 0.0;
        for (std::size_t index = 0; index < count; ++index) {
            const SwitchSixAxis turned = axes.apply(subject[index]);
            const SwitchSixAxis error = {reference[index].x - scale * turned.x,
                                         reference[index].y - scale * turned.y,
                                         reference[index].z - scale * turned.z};
            residual += dot(error, error);
        }

        if (!have_best || residual < best) {
            second = have_best ? best : 0.0;
            best = residual;
            answer.axes = axes;
            answer.scale = scale;
            have_best = true;
        } else if (second == 0.0 || residual < second) {
            second = residual;
        }
    }

    if (!have_best || second <= 0.0) {
        return answer;
    }
    answer.margin = best > 1e-9 ? static_cast<float>(second / best) : kNeededMargin;
    answer.decided = answer.margin >= kNeededMargin;
    return answer;
}

void MotionComparison::reset() {
    acceleration_pairs_.clear();
    rotation_pairs_.clear();
    acceleration_ = Answer();
    rotation_ = Answer();
    have_last_ = false;
    said_answer_ = false;
}

void MotionComparison::add(const DsMotionState& reference, const DsMotionState& subject) {
    if (!reference.present || !subject.present) {
        return;
    }
    last_reference_ = reference;
    last_subject_ = subject;
    have_last_ = true;

    const SwitchSixAxis reference_accel = {reference.accel_x, reference.accel_y, reference.accel_z};
    const SwitchSixAxis subject_accel = {subject.accel_x, subject.accel_y, subject.accel_z};
    const SwitchSixAxis reference_gyro = {reference.gyro_x, reference.gyro_y, reference.gyro_z};
    const SwitchSixAxis subject_gyro = {subject.gyro_x, subject.gyro_y, subject.gyro_z};

    // A pose only teaches something if it is not one already held. Without
    // this a device resting on a table fills the buffer with the same reading
    // and every candidate that agrees about that one direction ties.
    bool fresh = true;
    for (const Pair& pair : acceleration_pairs_) {
        const SwitchSixAxis apart = {pair.reference.x - reference_accel.x,
                                     pair.reference.y - reference_accel.y,
                                     pair.reference.z - reference_accel.z};
        if (length(apart) < kNewPose) {
            fresh = false;
            break;
        }
    }
    if (fresh && acceleration_pairs_.size() < kMaxPairs) {
        acceleration_pairs_.push_back({reference_accel, subject_accel});
    }

    // Rotation only exists while the two are being moved, and only the
    // reference's magnitude can be judged, since the subject's unit is one of
    // the things being solved for.
    if (length(reference_gyro) >= kMovingRate && rotation_pairs_.size() < kMaxPairs) {
        rotation_pairs_.push_back({reference_gyro, subject_gyro});
    }

    if (acceleration_pairs_.size() >= kNeededPoses && acceleration_pairs_.size() % 5 == 0) {
        std::vector<SwitchSixAxis> a, b;
        for (const Pair& pair : acceleration_pairs_) {
            a.push_back(pair.reference);
            b.push_back(pair.subject);
        }
        acceleration_ = compare_motion_axes(a, b);
    }
    if (rotation_pairs_.size() >= kNeededTurns && rotation_pairs_.size() % 10 == 0) {
        std::vector<SwitchSixAxis> a, b;
        for (const Pair& pair : rotation_pairs_) {
            a.push_back(pair.reference);
            b.push_back(pair.subject);
        }
        rotation_ = compare_motion_axes(a, b);
    }
}

void MotionComparison::report(uint32_t now_ms) {
    if (!have_last_) return;
    if (reported_at_ms_ != 0 && now_ms - reported_at_ms_ < 1000) return;
    reported_at_ms_ = now_ms;

    // Both readings, side by side, at the same instant. These are the point:
    // an answer worked out from them can be checked against them by hand,
    // which is what none of the earlier attempts allowed.
    char text[256];
    std::snprintf(text, sizeof(text),
                  "motion A/B: this device accel %.2f,%.2f,%.2f gyro %.2f,%.2f,%.2f | the other "
                  "accel %.2f,%.2f,%.2f gyro %.2f,%.2f,%.2f",
                  static_cast<double>(last_reference_.accel_x),
                  static_cast<double>(last_reference_.accel_y),
                  static_cast<double>(last_reference_.accel_z),
                  static_cast<double>(last_reference_.gyro_x),
                  static_cast<double>(last_reference_.gyro_y),
                  static_cast<double>(last_reference_.gyro_z),
                  static_cast<double>(last_subject_.accel_x),
                  static_cast<double>(last_subject_.accel_y),
                  static_cast<double>(last_subject_.accel_z),
                  static_cast<double>(last_subject_.gyro_x),
                  static_cast<double>(last_subject_.gyro_y),
                  static_cast<double>(last_subject_.gyro_z));
    log_debug(text);
    log_debug("motion A/B: " + std::to_string(acceleration_pairs_.size()) + " poses and " +
              std::to_string(rotation_pairs_.size()) + " turns compared; acceleration is " +
              acceleration_.describe() + ", rotation is " + rotation_.describe());

    if (!said_answer_ && acceleration_.decided && rotation_.decided) {
        said_answer_ = true;
        log_info("motion A/B: the other device's acceleration is " + acceleration_.describe() +
                 " of this one's, and its rotation is " + rotation_.describe());
    }
}

}  // namespace ds
