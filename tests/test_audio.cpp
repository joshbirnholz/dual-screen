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

// A queue this deep is nothing to do with drift: the source is handing over
// more audio per second than a second holds, and the trim is what takes the
// difference back off.
void test_a_queue_near_its_target_is_left_alone_and_a_deep_one_is_trimmed() {
    constexpr std::size_t target = 2880;   // 60ms at 48kHz
    constexpr std::size_t margin = 1920;   // 40ms at 48kHz
    constexpr std::size_t block = 480;     // 10ms at 48kHz

    // At the target, below it, and inside the margin: all ordinary drift, and
    // the rate correction's business rather than this one's.
    CHECK(audio_trim_frames(target, target, margin, block) == 0);
    CHECK(audio_trim_frames(target / 2, target, margin, block) == 0);
    CHECK(audio_trim_frames(target + margin, target, margin, block) == 0);

    // Past it, and the surplus goes -- but never more than half a block, so
    // there is always audio either side of the join to cross between.
    CHECK(audio_trim_frames(target + margin + 1, target, margin, block) == block / 2);
    CHECK(audio_trim_frames(target * 4, target, margin, block) == block / 2);

    // A surplus smaller than half a block is taken in one go.
    constexpr std::size_t long_block = 4800;  // 100ms at 48kHz
    CHECK(audio_trim_frames(target + margin + 40, target, margin, long_block) == margin + 40);

    // And a block too short to cut in half is left whole rather than cut
    // badly.
    CHECK(audio_trim_frames(target * 4, target, margin, 1) == 0);
}

// The point of splicing rather than simply dropping: the join is a ramp, not
// an edge. An edge in a waveform is exactly what a click is.
void test_a_splice_joins_two_places_in_a_tone_without_a_step() {
    const std::vector<int16_t> input = make_tone(512);
    const int natural = largest_step(input);
    constexpr std::size_t skip = 32;

    // The step that matters is between what was already played and whatever
    // comes next, so both candidates are measured with the frame before the
    // join in front of them. Without it the cut looks perfectly smooth: it is
    // continuous with itself, just not with the stream it is spliced into.
    const auto after = [&input](const std::vector<int16_t>& tail) {
        std::vector<int16_t> whole{input[0], input[1]};
        whole.insert(whole.end(), tail.begin(), tail.end());
        return whole;
    };

    std::vector<int16_t> spliced;
    audio_splice(input.data(), 512, skip, spliced);
    CHECK(spliced.size() == (512 - skip) * 2);

    // A hard cut at the same place, for the comparison that makes the number
    // above mean something.
    const std::vector<int16_t> cut(input.begin() + skip * 2, input.end());

    CHECK(largest_step(after(cut)) > natural * 10);
    CHECK(largest_step(after(spliced)) <= natural * 3);

    // Nothing is dropped when there is no surplus.
    std::vector<int16_t> whole;
    audio_splice(input.data(), 512, 0, whole);
    CHECK(whole == input);
}

// The whole of what the trim exists for, run as the loop it actually sits in:
// a source sending 4% more than real time, a device draining at real time, and
// the rate correction doing all it is allowed to do. Without the trim this
// walks the queue to the panic ceiling and starts again -- which is what a Mac
// companion's log showed, 169ms to 361ms and round once more.
void test_a_sending_source_is_paid_off_in_splices_rather_than_at_the_panic() {
    constexpr std::size_t target = 2880;
    constexpr std::size_t margin = 1920;
    constexpr std::size_t panic = 48000 * kAudioPanicQueueMs / 1000;
    constexpr std::size_t block = 480;
    constexpr double surplus = 1.04;

    const auto run = [&](bool trimming) {
        double queued = target;
        double ratio = 1.0;
        double worst = queued;
        for (int step = 0; step < 4000; ++step) {
            const auto depth = static_cast<std::size_t>(queued);
            ratio = audio_drift_ratio(depth, target, ratio);
            const std::size_t skipped =
                trimming ? audio_trim_frames(depth, target, margin, block) : 0;
            // What the resampler emits for what arrived, then what the device
            // took out of the queue in the time that block covered.
            queued += static_cast<double>(block - skipped) / ratio;
            queued -= static_cast<double>(block) / surplus;
            queued = std::max(0.0, queued);
            worst = std::max(worst, queued);
        }
        return worst;
    };

    // Untrimmed, the correction is outrun and the queue reaches the panic --
    // the behaviour this replaces.
    CHECK(run(false) > static_cast<double>(panic));

    // Trimmed, it never leaves the margin by more than the block it is being
    // fed in.
    const double worst = run(true);
    CHECK(worst < static_cast<double>(target + margin + block));
    CHECK(worst < static_cast<double>(panic));
}

// Audio does not arrive evenly: a companion drains its socket once per pass of
// the frame loop, so half a dozen packets land together and then nothing lands
// for a whole frame. Read at the moment of a push, the queue is whatever the
// burst just made it -- and trimming on that throws away the very jitter the
// queue's depth exists to absorb.
//
// Measured on a Mac companion: 12 to 82ms discarded every second over as many
// as twenty splices, while the source was sending about 3ms a second more than
// real time, and the queue still ran dry several times in the same second.
void test_audio_arriving_in_lumps_is_not_a_surplus() {
    constexpr std::size_t rate = 48000;
    constexpr std::size_t target = rate * 60 / 1000;   // 60ms
    constexpr std::size_t margin = rate * 40 / 1000;   // 40ms
    constexpr std::size_t block = rate * 10 / 1000;    // 10ms a packet
    constexpr std::size_t window = rate * kAudioLowWaterWindowMs / 1000;
    constexpr std::size_t burst = 6;  // 60ms of audio landing in one pass

    AudioLowWater low_water;
    low_water.reset(window, target);
    std::size_t queued = target;
    std::size_t trimmed = 0;
    std::size_t deepest = queued;

    for (int pass = 0; pass < 600; ++pass) {
        for (std::size_t packet = 0; packet < burst; ++packet) {
            const std::size_t sustained = low_water.note(queued, block);
            const std::size_t skipped = audio_trim_frames(sustained, target, margin, block);
            trimmed += skipped;
            queued += block - skipped;
            deepest = std::max(deepest, queued);
        }
        // Real time out: exactly as much as arrived, so there is no surplus
        // here at all -- only lumps.
        queued -= std::min(queued, burst * block);
    }

    // The burst alone puts the queue well past where a trim used to fire...
    CHECK(deepest > target + margin);
    CHECK(audio_trim_frames(deepest, target, margin, block) > 0);
    // ...and nothing is thrown away, because none of it was ever surplus.
    CHECK_EQ(trimmed, static_cast<std::size_t>(0));
}

// And the case the trim is actually for, run through the same low-water mark:
// a source sending 4% more than real time really does have to lose some, and
// the mark must not be so slow to notice that the queue runs away first.
void test_a_real_surplus_is_still_found_through_the_low_water_mark() {
    constexpr std::size_t rate = 48000;
    constexpr std::size_t target = rate * 60 / 1000;
    constexpr std::size_t margin = rate * 40 / 1000;
    constexpr std::size_t panic = rate * kAudioPanicQueueMs / 1000;
    constexpr std::size_t block = rate * 10 / 1000;
    constexpr std::size_t window = rate * kAudioLowWaterWindowMs / 1000;
    constexpr double surplus = 1.04;

    AudioLowWater low_water;
    low_water.reset(window, target);
    double queued = target;
    double ratio = 1.0;
    double worst = queued;
    double settled = queued;
    for (int step = 0; step < 6000; ++step) {
        const auto depth = static_cast<std::size_t>(queued);
        ratio = audio_drift_ratio(depth, target, ratio);
        const std::size_t sustained = low_water.note(depth, block);
        const std::size_t skipped = audio_trim_frames(sustained, target, margin, block);
        queued += static_cast<double>(block - skipped) / ratio;
        queued -= static_cast<double>(block) / surplus;
        queued = std::max(0.0, queued);
        worst = std::max(worst, queued);
        settled = queued;
    }

    // Nowhere near the ceiling, which is where the whole surplus used to be
    // paid off in one audible gap.
    CHECK(worst < static_cast<double>(panic));
    // The mark looks back about a second, so the queue is allowed to climb by
    // roughly what a second of this surplus adds before the trim engages --
    // and no further.
    CHECK(worst < static_cast<double>(target + margin) + static_cast<double>(window) * 0.1);
    // And it holds there rather than sawing back up to the ceiling.
    CHECK(settled < static_cast<double>(target + margin) + static_cast<double>(window) * 0.1);
}

// The mark itself: the lowest the queue has been across about a second, with
// both buckets starting where the queue is so that a stream which has only
// just been primed is not read as empty.
void test_the_low_water_mark_follows_the_trough_not_the_peak() {
    constexpr std::size_t window = 1000;
    AudioLowWater mark;
    mark.reset(window, 500);
    CHECK_EQ(mark.depth(), static_cast<std::size_t>(500));

    // A spike passes straight over it.
    CHECK_EQ(mark.note(9000, 100), static_cast<std::size_t>(500));
    // A trough is what it takes.
    CHECK_EQ(mark.note(120, 100), static_cast<std::size_t>(120));
    // And that trough keeps counting until it has aged out of both buckets --
    // one window closes the bucket it is in, the next retires it.
    for (int push = 0; push < 10; ++push) {
        mark.note(4000, 100);
    }
    CHECK_EQ(mark.depth(), static_cast<std::size_t>(120));
    for (int push = 0; push < 10; ++push) {
        mark.note(4000, 100);
    }
    CHECK_EQ(mark.depth(), static_cast<std::size_t>(4000));
}

}  // namespace

int main() {
    test_the_correction_stays_inaudible_however_far_off_the_queue_is();
    test_a_ratio_of_one_passes_samples_through_unchanged();
    test_a_faster_ratio_emits_fewer_frames_and_a_slower_one_more();
    test_consecutive_blocks_stay_continuous_across_the_join();
    test_a_deep_queue_speeds_playback_up_and_a_shallow_one_slows_it_down();
    test_the_correction_moves_gradually_rather_than_stepping();
    test_a_queue_near_its_target_is_left_alone_and_a_deep_one_is_trimmed();
    test_a_splice_joins_two_places_in_a_tone_without_a_step();
    test_a_sending_source_is_paid_off_in_splices_rather_than_at_the_panic();
    test_audio_arriving_in_lumps_is_not_a_surplus();
    test_a_real_surplus_is_still_found_through_the_low_water_mark();
    test_the_low_water_mark_follows_the_trough_not_the_peak();
    return test::finish("audio");
}
