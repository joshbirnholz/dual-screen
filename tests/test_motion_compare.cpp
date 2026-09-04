// Characterising one device's motion against another's.
//
// Seven rounds went into a Switch's six-axis frame by deriving it and then
// asking a person how a game felt. That channel cannot separate a wrong scale
// from a wrong sign, or a wrong axis from a wrong handedness -- all four feel
// like "it's wrong somehow", which is exactly what came back every time.
//
// A device already known to be right is a better instrument than any of that,
// and the test for using one is the same shape as the problem: make two
// synthetic devices that disagree in a way chosen here, hand over what each
// would report, and check that the disagreement comes back named.

#include "test_support.hpp"

#include <cmath>
#include <vector>

#include "input/motion_compare.hpp"

using namespace ds;

namespace {

// What a device reports for a motion, given the frame and unit it uses.
SwitchSixAxis as_reported(const MotionAxes& frame, float unit, const SwitchSixAxis& truth) {
    // The inverse of the frame, since `frame` is what turns its reading back
    // into the truth.
    MotionAxes back;
    for (int axis = 0; axis < 3; ++axis) {
        back.source[frame.source[axis]] = static_cast<int8_t>(axis);
        back.sign[frame.source[axis]] = frame.sign[axis];
    }
    const SwitchSixAxis turned = back.apply(truth);
    return {turned.x / unit, turned.y / unit, turned.z / unit};
}

// A stretch of the two being moved together: same orientation, same turns,
// because they are held as one.
void record(const MotionAxes& frame, float unit, std::vector<SwitchSixAxis>& reference,
            std::vector<SwitchSixAxis>& subject, int steps = 200) {
    for (int step = 0; step < steps; ++step) {
        const float t = static_cast<float>(step) * 0.05f;
        const SwitchSixAxis truth = {2.4f * std::sin(t * 1.3f), 3.1f * std::sin(t * 0.7f + 1.1f),
                                     1.8f * std::sin(t * 1.9f + 0.4f)};
        reference.push_back(truth);
        subject.push_back(as_reported(frame, unit, truth));
    }
}

void test_a_disagreement_chosen_here_comes_back_named() {
    // Every frame a device could be in, at each of the units rotation is
    // reported in. If the comparison can name all of these it can name
    // whichever one a real console turns out to be -- and unlike working it
    // out from the accelerometer, both vectors are known here, so there is no
    // cross product and nothing about the motion that cannot be seen.
    const float units[] = {1.0f, 1.0f / 6.283185307f, 57.29577951f};
    int checked = 0;
    for (const MotionAxes& frame : motion_axis_candidates()) {
        for (const float unit : units) {
            std::vector<SwitchSixAxis> reference;
            std::vector<SwitchSixAxis> subject;
            record(frame, unit, reference, subject);

            const MotionComparison::Answer answer = compare_motion_axes(reference, subject);
            CHECK_MSG(answer.decided,
                      "gave up on a device reporting " + frame.describe() + " at " +
                          std::to_string(unit));
            CHECK_MSG(answer.axes == frame, "a device reporting " + frame.describe() +
                                                " was read as " + answer.axes.describe());
            CHECK_MSG(std::abs(answer.scale / unit - 1.0f) < 0.02f,
                      "the unit came out wrong for " + frame.describe() + ": " +
                          answer.describe());
            ++checked;
        }
    }
    CHECK_EQ(checked, 144);
}

void test_it_still_works_when_the_two_are_not_held_perfectly_still_together() {
    // Two devices taped together are not one rigid body, and neither sensor is
    // silent. The answer has to survive that.
    const MotionAxes frame = {{1, 2, 0}, {-1, 1, -1}};
    std::vector<SwitchSixAxis> reference;
    std::vector<SwitchSixAxis> subject;
    record(frame, 1.0f / 6.283185307f, reference, subject, 400);

    unsigned seed = 99u;
    const auto jitter = [&seed]() {
        seed = seed * 1103515245u + 12345u;
        return (static_cast<float>((seed >> 16) & 0x7fff) / 16383.5f - 1.0f) * 0.12f;
    };
    for (std::size_t index = 0; index < reference.size(); ++index) {
        reference[index] = {reference[index].x + jitter(), reference[index].y + jitter(),
                            reference[index].z + jitter()};
        subject[index] = {subject[index].x + jitter() * 0.2f, subject[index].y + jitter() * 0.2f,
                          subject[index].z + jitter() * 0.2f};
    }

    const MotionComparison::Answer answer = compare_motion_axes(reference, subject);
    CHECK(answer.decided);
    CHECK(answer.axes == frame);
}

void test_two_devices_lying_still_are_not_an_answer() {
    // The failure that would make this worse than useless: agreeing about one
    // direction is not agreeing about a frame, and a pair of devices resting
    // on a table agree about one direction all day. Several candidates explain
    // that equally well, so the only correct output is none.
    std::vector<SwitchSixAxis> reference;
    std::vector<SwitchSixAxis> subject;
    for (int step = 0; step < 300; ++step) {
        reference.push_back({0.0f, 0.0f, 9.81f});
        subject.push_back({0.0f, 0.0f, 9.81f});
    }
    CHECK_MSG(!compare_motion_axes(reference, subject).decided,
              "one direction held for five minutes was taken for a frame");
}

void test_the_pairs_it_keeps_are_the_ones_worth_keeping() {
    // Through the accumulator, which has to do the same refusing: a still pair
    // must not fill the buffer, and a moving one must be kept.
    MotionComparison comparison;
    DsMotionState still;
    still.present = true;
    still.accel_z = 9.81f;
    for (int step = 0; step < 500; ++step) {
        comparison.add(still, still);
    }
    CHECK_MSG(!comparison.acceleration().decided, "a still pair decided something");
    CHECK_MSG(!comparison.rotation().decided, "a pair with no rotation decided something");

    // And a real one, moved through many orientations and turns.
    const MotionAxes frame = {{2, 0, 1}, {1, -1, -1}};
    std::vector<SwitchSixAxis> reference;
    std::vector<SwitchSixAxis> subject;
    record(frame, 1.0f / 6.283185307f, reference, subject, 400);
    for (std::size_t index = 0; index < reference.size(); ++index) {
        DsMotionState a;
        DsMotionState b;
        a.present = b.present = true;
        // Orientation and turn both taken from the same swing, so the poses
        // are as varied as the turns.
        a.accel_x = reference[index].x * 3.0f;
        a.accel_y = reference[index].y * 3.0f;
        a.accel_z = reference[index].z * 3.0f;
        a.gyro_x = reference[index].x;
        a.gyro_y = reference[index].y;
        a.gyro_z = reference[index].z;
        const SwitchSixAxis reported_accel =
            as_reported(frame, 1.0f, {a.accel_x, a.accel_y, a.accel_z});
        b.accel_x = reported_accel.x;
        b.accel_y = reported_accel.y;
        b.accel_z = reported_accel.z;
        b.gyro_x = subject[index].x;
        b.gyro_y = subject[index].y;
        b.gyro_z = subject[index].z;
        comparison.add(a, b);
    }
    CHECK_MSG(comparison.rotation().decided, "moving the pair together settled nothing");
    CHECK(comparison.rotation().axes == frame);
    CHECK_MSG(comparison.acceleration().decided, "the poses settled nothing");
    CHECK(comparison.acceleration().axes == frame);
}

}  // namespace

int main() {
    test_a_disagreement_chosen_here_comes_back_named();
    test_it_still_works_when_the_two_are_not_held_perfectly_still_together();
    test_two_devices_lying_still_are_not_an_answer();
    test_the_pairs_it_keeps_are_the_ones_worth_keeping();
    return test::finish("motion-compare");
}
