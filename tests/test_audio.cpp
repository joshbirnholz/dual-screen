#include "test_support.hpp"

#include <cmath>
#include <vector>

#include "audio/audio_output.hpp"

using namespace ds;

namespace {

// A few hundred frames of a smooth tone: any discontinuity a resampler
// introduces stands out against it in a way white noise or a constant would
// hide.
std::vector<int16_t> make_tone(std::size_t frames, double phase_offset = 0.0) {
    std::vector<int16_t> samples(frames * 2);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double angle = (static_cast<double>(frame) + phase_offset) * 0.05;
        const auto value = static_cast<int16_t>(std::lround(std::sin(angle) * 10000.0));
        samples[frame * 2] = value;
        samples[frame * 2 + 1] = value;
    }
    return samples;
}

// The largest step between one output frame and the next. A resampler running
// near a ratio of 1 over a smooth input should never produce a step much
// bigger than the input's own.
int largest_step(const std::vector<int16_t>& samples) {
    int worst = 0;
    for (std::size_t frame = 1; frame * 2 + 1 < samples.size(); ++frame) {
        const int step = std::abs(static_cast<int>(samples[frame * 2]) -
                                  static_cast<int>(samples[(frame - 1) * 2]));
        worst = std::max(worst, step);
    }
    return worst;
}

void test_a_ratio_of_one_passes_samples_through_unchanged() {
    const std::vector<int16_t> input = make_tone(256);
    AudioResampler resampler;
    std::vector<int16_t> output;
    resampler.resample(input.data(), 256, 1.0, output);

    // One frame of startup latency is inherent to interpolating across the
    // join with a previous block, so the count is allowed to be one short.
    CHECK(output.size() >= (256 - 1) * 2);
    CHECK(output.size() <= 256 * 2);
    for (std::size_t frame = 0; frame + 1 < output.size() / 2; ++frame) {
        CHECK(std::abs(static_cast<int>(output[frame * 2]) -
                       static_cast<int>(input[frame * 2])) <= 1);
    }
}

void test_a_faster_ratio_emits_fewer_frames_and_a_slower_one_more() {
    const std::vector<int16_t> input = make_tone(1000);

    AudioResampler fast;
    std::vector<int16_t> fast_out;
    fast.resample(input.data(), 1000, 1.0 + kAudioMaxRateAdjust, fast_out);

    AudioResampler slow;
    std::vector<int16_t> slow_out;
    slow.resample(input.data(), 1000, 1.0 - kAudioMaxRateAdjust, slow_out);

    CHECK(fast_out.size() / 2 < 1000);
    CHECK(slow_out.size() / 2 > 995);
    CHECK(slow_out.size() > fast_out.size());
}

void test_consecutive_blocks_stay_continuous_across_the_join() {
    // The whole point of the resampler carrying state: a click at a block
    // boundary is exactly the pop this replaced a packet-dropping queue to
    // avoid, so the join has to be no sharper than the signal itself.
    const std::vector<int16_t> first = make_tone(512, 0.0);
    const std::vector<int16_t> second = make_tone(512, 512.0);

    AudioResampler resampler;
    std::vector<int16_t> output;
    resampler.resample(first.data(), 512, 1.0 + kAudioMaxRateAdjust, output);
    const std::size_t join = output.size();
    resampler.resample(second.data(), 512, 1.0 + kAudioMaxRateAdjust, output);

    CHECK(join > 0);
    CHECK(output.size() > join);
    // The input's own largest step, with a little room: the join must not
    // stand out against it.
    const int input_step = largest_step(first);
    const int step_at_join =
        std::abs(static_cast<int>(output[join]) - static_cast<int>(output[join - 2]));
    CHECK(step_at_join <= input_step + 2);
}

void test_a_deep_queue_speeds_playback_up_and_a_shallow_one_slows_it_down() {
    constexpr std::size_t target = 2880;  // 60ms at 48kHz

    // Repeatedly applying the correction settles at the clamp rather than
    // running away past it.
    double deep = 1.0;
    for (int step = 0; step < 500; ++step) {
        deep = audio_drift_ratio(target * 2, target, deep);
    }
    CHECK(deep > 1.0);
    CHECK(deep <= 1.0 + kAudioMaxRateAdjust + 1e-9);

    double shallow = 1.0;
    for (int step = 0; step < 500; ++step) {
        shallow = audio_drift_ratio(0, target, shallow);
    }
    CHECK(shallow < 1.0);
    CHECK(shallow >= 1.0 - kAudioMaxRateAdjust - 1e-9);

    // On target it neither speeds up nor slows down.
    CHECK(std::abs(audio_drift_ratio(target, target, 1.0) - 1.0) < 1e-9);
}

void test_the_correction_moves_gradually_rather_than_stepping() {
    // A single block must not swing the rate to the clamp: an audible step in
    // pitch would be its own artefact.
    constexpr std::size_t target = 2880;
    const double after_one = audio_drift_ratio(target * 4, target, 1.0);
    CHECK(after_one > 1.0);
    CHECK(after_one < 1.0 + kAudioMaxRateAdjust / 4.0);
}

// The one the suite missed. A limit was raised to fix a queue that could not
// converge, the edit silently did not land, and every test still passed --
// because nothing here asserted the limit was big enough to do its job, only
// that the clamp clamped to whatever it was set to.
void test_the_correction_can_outrun_a_device_whose_clock_is_off() {
    // Measured on a Switch: the queue gains about 26ms a second against a
    // 170ms target, which is a sink running some 2.6% slow. A correction that
    // cannot reach that never closes the gap -- it just holds the queue's
    // climb down to a slower climb, and the backlog grows until something
    // flushes it.
    constexpr double kMeasuredDrift = 0.026;
    CHECK(kAudioMaxRateAdjust > kMeasuredDrift);

    // And the controller actually gets there, rather than the clamp merely
    // permitting it: a queue sitting well above target has to drive the rate
    // past that drift, or the two never meet.
    constexpr std::size_t target = 8192;
    double ratio = 1.0;
    for (int step = 0; step < 2000; ++step) {
        ratio = audio_drift_ratio(target * 2, target, ratio);
    }
    CHECK(ratio - 1.0 >= kMeasuredDrift);
}

}  // namespace

int main() {
    test_the_correction_can_outrun_a_device_whose_clock_is_off();
    test_a_ratio_of_one_passes_samples_through_unchanged();
    test_a_faster_ratio_emits_fewer_frames_and_a_slower_one_more();
    test_consecutive_blocks_stay_continuous_across_the_join();
    test_a_deep_queue_speeds_playback_up_and_a_shallow_one_slows_it_down();
    test_the_correction_moves_gradually_rather_than_stepping();
    return test::finish("audio");
}
