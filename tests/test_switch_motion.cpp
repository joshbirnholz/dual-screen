// Working out a device's rotation frame from its accelerometer.
//
// The reason this file exists is that five builds shipped a guess at those
// axes and every one of them was wrong, because there was no way to check one
// without a console in someone's hands and a game to feel it in. A synthetic
// device has no such problem: rotate one, work out what its accelerometer and
// its gyroscope would report under some convention, hand those to the solver,
// and see whether it names the convention back. That is a test, and it can be
// run twenty-four times over -- once per frame a device could possibly use --
// in the time it takes to read this comment.

#include "test_support.hpp"

#include <cmath>
#include <vector>

#include "input/switch_motion.hpp"

using namespace ds;

namespace {

SwitchSixAxis cross(const SwitchSixAxis& a, const SwitchSixAxis& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

SwitchSixAxis plus(const SwitchSixAxis& a, const SwitchSixAxis& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

SwitchSixAxis times(const SwitchSixAxis& v, float by) { return {v.x * by, v.y * by, v.z * by}; }

SwitchSixAxis unit(const SwitchSixAxis& v) {
    const float size = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return size <= 1e-9f ? SwitchSixAxis{0.0f, 0.0f, 0.0f} : times(v, 1.0f / size);
}

// The inverse of a signed permutation: what the device must report for the
// solver's answer to come out as `axes`.
MotionAxes inverse(const MotionAxes& axes) {
    MotionAxes back;
    for (int axis = 0; axis < 3; ++axis) {
        back.source[axes.source[axis]] = static_cast<int8_t>(axis);
        back.sign[axes.source[axis]] = axes.sign[axis];
    }
    return back;
}

// A device being turned by hand: several seconds of rotation that changes
// axis as it goes, because a device turned only one way cannot reveal more
// than one way's worth of its frame.
struct Recording {
    std::vector<SwitchSixAxis> gravity;
    std::vector<SwitchSixAxis> change;
    std::vector<SwitchSixAxis> rotation;
};

// `device` is the convention being simulated: the frame the device reports
// rotation in, relative to the frame its acceleration is in. `unit_scale` is
// what one of the device's rotation units is worth in radians a second -- 1
// for radians, 1/(2*pi) for revolutions, 180/pi for degrees.
Recording record(const MotionAxes& device, float unit_scale, float noise = 0.0f,
                 int steps = 720, float briskness = 1.0f) {
    Recording out;
    // Gravity starts pointing along -y, which is a device held upright.
    SwitchSixAxis gravity = {0.0f, -1.0f, 0.0f};
    const float dt = 1.0f / 120.0f;
    unsigned seed = 12345u;
    const auto jitter = [&]() {
        seed = seed * 1103515245u + 12345u;
        return (static_cast<float>((seed >> 16) & 0x7fff) / 16383.5f - 1.0f) * noise;
    };

    for (int step = 0; step < steps; ++step) {
        // A turn whose axis wanders through all three, so every part of the
        // frame is exercised. Real handling does this without being asked.
        const float t = static_cast<float>(step) * dt;
        const SwitchSixAxis truth = {briskness * 1.6f * std::sin(t * 1.7f),
                                     briskness * 1.9f * std::sin(t * 1.1f + 0.6f),
                                     briskness * 1.4f * std::sin(t * 2.3f + 1.9f)};

        // What the accelerometer does about it: the ground swings the other
        // way, in the device's own frame.
        const SwitchSixAxis rate = times(cross(truth, gravity), -1.0f);
        const SwitchSixAxis next = unit(plus(gravity, times(rate, dt)));
        const SwitchSixAxis middle = unit(plus(gravity, next));

        out.gravity.push_back(middle);
        out.change.push_back(times(cross(truth, middle), -1.0f));
        // And what the gyroscope reports: the same rotation, in the device's
        // own axes and its own unit, which is what has to be recovered.
        const SwitchSixAxis reported = inverse(device).apply(times(truth, 1.0f / unit_scale));
        out.rotation.push_back({reported.x + jitter(), reported.y + jitter(),
                                reported.z + jitter()});
        gravity = next;
    }
    return out;
}

void test_every_frame_a_device_could_use_is_recovered() {
    // All twenty-four, each at each of the three units anyone reports rotation
    // in. If the solver can name every one of these, it can name whichever one
    // a real console turns out to be -- which is the whole point, since asking
    // has not worked.
    const struct {
        const char* name;
        float scale;
    } units[] = {
        {"radians a second", 1.0f},
        {"revolutions a second", 1.0f / 6.283185307f},
        {"degrees a second", 57.29577951f},
    };

    int checked = 0;
    for (const MotionAxes& device : motion_axis_candidates()) {
        for (const auto& reported_in : units) {
            const Recording taken = record(device, reported_in.scale);
            const MotionFit fit = fit_motion_axes(taken.gravity, taken.change, taken.rotation);
            CHECK_MSG(fit.decided, std::string("gave up on a device reporting ") +
                                       device.describe() + " in " + reported_in.name);
            CHECK_MSG(fit.axes == device, std::string("a device reporting ") + device.describe() +
                                              " in " + reported_in.name + " was read as " +
                                              fit.axes.describe());
            // And the unit, which is the half of this that no amount of
            // staring at a game could ever have established.
            CHECK_MSG(std::abs(fit.scale / reported_in.scale - 1.0f) < 0.02f,
                      std::string("the unit came out wrong for ") + device.describe() + " in " +
                          reported_in.name + ": " + fit.describe());
            ++checked;
        }
    }
    CHECK_EQ(checked, 72);
}

void test_it_still_works_on_a_reading_that_is_not_perfect() {
    // Real sensors are noisy and real hands are not smooth. The margin can
    // narrow; the answer must not change.
    const MotionAxes device = {{0, 1, 2}, {-1, -1, 1}};
    const Recording taken = record(device, 1.0f / 6.283185307f, 0.05f);
    const MotionFit fit = fit_motion_axes(taken.gravity, taken.change, taken.rotation);
    CHECK(fit.decided);
    CHECK(fit.axes == device);
    CHECK(std::abs(fit.scale * 6.283185307f - 1.0f) < 0.05f);
}

void test_it_says_nothing_rather_than_guessing() {
    // The failure that matters. A device sitting still, or turned only about
    // the axis pointing at the ground, cannot say which frame it is in --
    // crossing a vector with itself gives nothing back. Answering anyway would
    // be another guess, which is what this was built to stop.
    Recording still;
    for (int step = 0; step < 400; ++step) {
        still.gravity.push_back({0.0f, -1.0f, 0.0f});
        still.change.push_back({0.0f, 0.0f, 0.0f});
        still.rotation.push_back({0.0f, 0.0f, 0.0f});
    }
    CHECK_MSG(!fit_motion_axes(still.gravity, still.change, still.rotation).decided,
              "a device lying still was read as evidence of something");

    // Too few readings to mean anything, however clean they are.
    const Recording taken = record({{0, 1, 2}, {1, 1, 1}}, 1.0f);
    const std::vector<SwitchSixAxis> few_gravity(taken.gravity.begin(), taken.gravity.begin() + 10);
    const std::vector<SwitchSixAxis> few_change(taken.change.begin(), taken.change.begin() + 10);
    const std::vector<SwitchSixAxis> few_rotation(taken.rotation.begin(),
                                                  taken.rotation.begin() + 10);
    CHECK(!fit_motion_axes(few_gravity, few_change, few_rotation).decided);
}

// The realistic way to get a wrong answer, as opposed to no answer.
//
// A console turned about one axis only says a great deal about that axis and
// nothing whatever about the other two, so several candidates explain the
// movement exactly as well as each other. Refusing is the only correct
// behaviour, and it is the behaviour that keeps this from becoming a sixth
// guess with extra steps.
void test_movement_in_one_direction_only_is_refused() {
    Recording out;
    SwitchSixAxis gravity = {0.0f, -1.0f, 0.0f};
    const float dt = 1.0f / 120.0f;
    for (int step = 0; step < 600; ++step) {
        // Rolled back and forth about x and nothing else: plenty of gravity
        // travel, all of it in one plane.
        const SwitchSixAxis truth = {1.5f * std::sin(static_cast<float>(step) * dt * 2.0f), 0.0f,
                                     0.0f};
        const SwitchSixAxis rate = times(cross(truth, gravity), -1.0f);
        const SwitchSixAxis next = unit(plus(gravity, times(rate, dt)));
        const SwitchSixAxis middle = unit(plus(gravity, next));
        out.gravity.push_back(middle);
        out.change.push_back(times(cross(truth, middle), -1.0f));
        out.rotation.push_back(truth);
        gravity = next;
    }
    CHECK_MSG(!fit_motion_axes(out.gravity, out.change, out.rotation).decided,
              "a console rolled about one axis was taken as evidence about all three");
}

void test_the_candidates_are_the_rotations_and_only_those() {
    const std::vector<MotionAxes>& candidates = motion_axis_candidates();
    CHECK_EQ(static_cast<int>(candidates.size()), 24);
    for (const MotionAxes& axes : candidates) {
        // Every one is a rotation: determinant +1. A reflection describes a
        // mirrored device, and shipping two of those is how this problem got
        // as far as it did.
        const SwitchSixAxis x = axes.apply({1.0f, 0.0f, 0.0f});
        const SwitchSixAxis y = axes.apply({0.0f, 1.0f, 0.0f});
        const SwitchSixAxis z = axes.apply({0.0f, 0.0f, 1.0f});
        const float determinant = x.x * (y.y * z.z - y.z * z.y) - x.y * (y.x * z.z - y.z * z.x) +
                                  x.z * (y.x * z.y - y.y * z.x);
        CHECK_MSG(test::nearly_equal(determinant, 1.0), "a candidate that is not a rotation");
    }
    // And no duplicates.
    for (std::size_t a = 0; a < candidates.size(); ++a) {
        for (std::size_t b = a + 1; b < candidates.size(); ++b) {
            CHECK(!(candidates[a] == candidates[b]));
        }
    }
}

void test_the_accumulator_holds_off_until_it_has_been_moved_every_way() {
    // The same thing through the interface the reader actually uses, which
    // has to hold its tongue until the movement has covered all three axes.
    const MotionAxes device = {{2, 0, 1}, {1, -1, -1}};
    MotionFitter fitter;

    // Held still first: no amount of this decides anything.
    for (int step = 0; step < 300; ++step) {
        fitter.add({0.0f, -9.81f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f / 60.0f);
    }
    CHECK(!fitter.fit().decided);
    CHECK(fitter.progress() < 1.0f);

    const Recording taken = record(device, 1.0f / 6.283185307f);
    for (std::size_t index = 0; index < taken.gravity.size(); ++index) {
        fitter.add(times(taken.gravity[index], 9.81f), taken.rotation[index], 1.0f / 120.0f);
    }
    CHECK_MSG(fitter.fit().decided, "moving it every way still did not settle it");
    CHECK(fitter.fit().axes == device);
}

// The bug that got this shipped without ever running once.
//
// Deciding used to be gated on the rotation reading's own magnitude measured
// against a threshold written in radians -- in a class whose entire purpose is
// that the unit of that reading is unknown. The console reports revolutions,
// so the gate quietly asked for a full turn about each of three axes before it
// would say anything, and on hardware it said nothing at all: a log with a
// silence in it exactly where the answer should have been.
//
// So the check is that a realistic amount of handling is enough, in the unit
// that broke it. Three seconds, at the speed the console's own log showed a
// hand actually turning it: peaks between a tenth and half a revolution a
// second. The gate that shipped wanted a full revolution about each of three
// axes, which this is nowhere near, and which is the point.
void test_a_few_seconds_of_ordinary_handling_is_enough() {
    const MotionAxes device = {{0, 1, 2}, {-1, -1, 1}};
    const float revolutions = 1.0f / 6.283185307f;
    // 360 steps at 120Hz is three seconds; half the briskness above puts the
    // peaks near 0.9 rad/s, which read as 0.14 of a revolution a second.
    const Recording taken = record(device, revolutions, 0.0f, 360, 0.5f);

    MotionFitter fitter;
    for (std::size_t index = 0; index < taken.gravity.size(); ++index) {
        fitter.add(times(taken.gravity[index], 9.81f), taken.rotation[index], 1.0f / 120.0f);
    }
    CHECK_MSG(fitter.fit().decided,
              "a few seconds of ordinary handling did not settle it, which is what shipped");
    CHECK(fitter.fit().axes == device);
    CHECK(std::abs(fitter.fit().scale / revolutions - 1.0f) < 0.05f);
}

}  // namespace

int main() {
    test_the_candidates_are_the_rotations_and_only_those();
    test_every_frame_a_device_could_use_is_recovered();
    test_it_still_works_on_a_reading_that_is_not_perfect();
    test_it_says_nothing_rather_than_guessing();
    test_movement_in_one_direction_only_is_refused();
    test_the_accumulator_holds_off_until_it_has_been_moved_every_way();
    test_a_few_seconds_of_ordinary_handling_is_enough();
    return test::finish("motion-fit");
}
