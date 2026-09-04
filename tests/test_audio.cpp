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

// The property the limit exists for, which an earlier version of this file had
// upside down.
//
// That version asserted the correction could reach 2.6%, so that a queue which
// kept growing on one device would converge. It did converge, at a rate bent
// far enough to hear: both companions came back a good half-semitone sharp
// against the same game playing out of the host's speakers. A correction that
// is audible has stopped being a correction, whatever it does to the queue --
// so what has to be pinned down is the other direction.
void test_the_correction_stays_inaudible_however_far_off_the_queue_is() {
    // Pitch, in cents, of the widest correction this will ever apply. A
    // semitone is 100; the ear starts picking a sustained tone apart somewhere
    // above 20 or so, and game audio is not a sustained tone. 1% is about 17.
    const double cents = 1200.0 * std::log2(1.0 + kAudioMaxRateAdjust);
    CHECK_MSG(cents < 20.0, "the playback correction is wide enough to hear as a change in pitch");

    // And it really is the ceiling, driven from a queue as far out as one can
    // get: nothing about the controller lets it wind past the clamp given
    // enough time.
    constexpr std::size_t target = 8192;
    double ratio = 1.0;
    for (int step = 0; step < 5000; ++step) {
        ratio = audio_drift_ratio(target * 20, target, ratio);
    }
    CHECK(ratio <= 1.0 + kAudioMaxRateAdjust + 1e-9);

    // A queue that far out is not drift and is not this control's problem: it
    // gets cut back rather than played off, which is what kAudioPanicQueueMs
    // is for. Stated here so the two stay in step -- the panic has to sit
    // above the target by a margin the correction could never work off on its
    // own, or the two fight and the correction loses audibly.
    CHECK(kAudioPanicQueueMs > kAudioTargetQueueMs);
}

}  // namespace

int main() {
    test_the_correction_stays_inaudible_however_far_off_the_queue_is();
    test_a_ratio_of_one_passes_samples_through_unchanged();
    test_a_faster_ratio_emits_fewer_frames_and_a_slower_one_more();
    test_consecutive_blocks_stay_continuous_across_the_join();
    test_a_deep_queue_speeds_playback_up_and_a_shallow_one_slows_it_down();
    test_the_correction_moves_gradually_rather_than_stepping();
    return test::finish("audio");
}
