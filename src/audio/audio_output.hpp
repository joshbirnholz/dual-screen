#pragma once

#include <SDL.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ds {

// How deep the outgoing queue is aimed at, and how far off it has to get
// before the correction below is running as hard as it ever will. Both are
// milliseconds of audio.
//
// The target is the latency this deliberately carries: enough to ride out the
// jitter between a packet arriving and the device asking for it (a poll cadence
// of ~11ms, plus whatever the network adds on top), and no more, because every
// millisecond of it is a millisecond between pressing a button and hearing the
// result.
inline constexpr int kAudioTargetQueueMs = 60;
// Past this the queue is not drifting, it is stalled -- a device that stopped
// consuming, a session that was suspended -- and no gentle correction is going
// to walk back a backlog that size in any reasonable time. Started clean
// instead. Well clear of the target so ordinary drift never reaches it.
//
// A ceiling rather than a multiple of the target, which is the mistake the
// first version of the device-buffer floor made: four times a 170ms target put
// this at 682ms, so a queue that was running away had most of a second to run
// away into before anything stopped it. Whatever the target ends up being,
// what matters here is how far behind is too far to be worth keeping, and that
// is a duration.
//
// With kAudioTrimMarginMs below holding the queue near its target this should
// now only ever fire on a genuine stall, which is what it was always for. It
// spent a long time doing the trim's job badly instead: a source sending more
// than real time walked the queue all the way up here, over and over, and
// every cycle of that was most of half a second of lag followed by an audible
// gap.
inline constexpr int kAudioPanicQueueMs = 400;

// The most the playback rate is ever bent to correct drift, as a fraction.
//
// 1% is about 17 cents of pitch, which nobody hears on game audio. This was
// briefly 5%, to let the correction converge against a queue that kept growing
// on a Switch, and that was a mistake worth writing down: 5% is most of a
// semitone, and a queue that stays above target holds the correction there
// permanently, so what it bought was not a converged queue but a whole session
// transposed. Both companions came back higher-pitched than the same game
// playing out of the host's own speakers, which is exactly what a saturated
// correction sounds like.
//
// The argument for the wider limit -- that a sink genuinely running slow needs
// a correction its own size, and that resampling to the rate the device really
// runs at is what makes a second of game audio take a second -- is sound as
// far as it goes, and it does not go far enough. It only holds when the rates
// differ because the *sink* is off. A source handing over more than a second
// of audio per second is the other case entirely, and there the queue is not
// drift to be resampled away, it is surplus: pitching everything up to burn it
// off plays the game sharp for as long as the surplus lasts. Both companions
// showing it at once points at the source, not at two unrelated clocks.
//
// So this stays inaudible, and a queue the correction cannot hold is dealt
// with by throwing the surplus away (see kAudioTrimMarginMs) rather than by
// bending pitch further. report() says "(at the limit)" whenever the
// correction is saturated, which is what makes the difference visible.
inline constexpr double kAudioMaxRateAdjust = 0.01;

// How far past the target the queue may wander before the surplus is thrown
// away rather than resampled off, in milliseconds of audio.
//
// This is the other half of what kAudioMaxRateAdjust deliberately will not do.
// A source handing over more audio per second than a second holds -- measured
// at 2 to 5 parts per hundred from a Cemu running a little fast -- is surplus,
// and a correction limited to one part per hundred cannot take it off however
// long it is given. The queue therefore grows, and with only the panic ceiling
// above it the whole surplus is carried for fourteen seconds, at up to 400ms
// of lag, before being dropped in one audible gap. Measured on a Mac companion:
// the queue walked 169, 202, 225, ... 361ms and started again, over and over,
// with "(at the limit)" on every line in between.
//
// So the surplus is taken off as it appears instead. The total thrown away is
// the same either way -- it is however much more than real time the source is
// sending -- and this is the choice of paying it in small change or in one
// note. Small change wins twice: the queue stays near the target, which is the
// whole of the latency this carries, and a few milliseconds spliced out (see
// audio_splice) is a tick where a third of a second is a skip.
//
// The size of it was forty for a while, and forty was a job already being
// done. When this margin was written the trim read the queue at the instant a
// block arrived, so it had to be wide enough to see past the sawtooth of the
// device draining in whole buffers -- otherwise the surplus it threw away was
// the device's own cadence. AudioLowWater arrived afterwards and does exactly
// that, and does it better: it reads the lowest the queue has been over a
// quarter-second, which has no sawtooth left in it at all. The margin kept its
// width anyway, and since the trim never fires below target plus margin, the
// whole of that width is where the queue comes to rest.
//
// Measured on a Mac companion, wired, Wind Waker HD: the queue sat at 111ms
// against its 60ms target and never once dropped below 79ms across nineteen
// minutes, with no underrun and nothing cut short -- the floor the trim was
// holding it to, which is 60 plus 40. Meanwhile the picture on the same link
// was being held 24ms. Sound trailed it by the difference.
//
// So the margin is now only what the low-water mark cannot already see past.
// Jitter cannot push it down -- late audio makes the queue deeper, not
// shallower, and a genuine gap only stops the trim rather than provoking it --
// so what is left to cover is small.
inline constexpr int kAudioTrimMarginMs = 15;

// How much audio passes through before the low-water mark below starts a fresh
// bucket, in milliseconds.
//
// Two buckets are kept, so what the trim reads is the lowest the queue has
// been over somewhere between a quarter of a second and half of one.
//
// Long enough to contain a trough: packets arrive a frame apart at worst, so
// this holds fifteen or more of the bursts it has to see past. Short enough
// that a real surplus is caught before it has grown into latency -- the mark
// lags the queue by whatever the surplus adds while it is looking back, which
// at the 2 to 5 percent a Cemu produces is under twenty milliseconds of depth.
inline constexpr int kAudioLowWaterWindowMs = 250;

// Frames crossfaded across a splice, at the rate the device is running.
// About three milliseconds at 48kHz: long enough that the join is a ramp
// rather than a step, short enough to be over before it is heard as anything
// of its own.
inline constexpr std::size_t kAudioSpliceFrames = 128;

// Cover bought after the queue runs dry, on top of the target, in milliseconds
// of audio -- the sound's counterpart of the picture's hold_extra_ms().
//
// Over wifi a link does not only jitter, it now and then holds everything back
// for a tenth of a second and hands it over at once. Measured, Steam Deck as
// the GamePad for a PC playing Wind Waker HD: the queue sat at its 60ms target
// with nothing wrong for seconds at a time, then ran dry one to three times a
// second for a spell, each time followed by a lump of audio arriving together
// ("-900 parts per ten thousand" one second, "+900" the next). A G Cloud on
// the same host did the same.
//
// What made that sound broken rather than merely gappy was what came after.
// A dry spell plays silence in place of audio, so once the held-back audio
// lands the queue is deeper by exactly the length of the spell -- which is the
// cover the next spell needed. The trim read that as surplus and cut it back
// out in a dozen or more splices a second while the rate correction pinned
// itself at the limit chasing the old target: crackle and warble, every time,
// and nothing left to absorb the next one.
//
// So a dry spell is now kept. The queue running empty raises the target by
// how long it ran empty, which is what the lump about to arrive brings the
// queue to anyway, so nothing is trimmed and nothing is resampled; the next
// spell that size finds audio waiting. A calm spell gives it back slowly
// enough for the rate correction to absorb, well inside its limit, with no
// splices at all.
//
// The ceiling is the most delay sound is allowed to trail the picture by for
// the sake of never stopping: gaps longer than this are still heard as gaps.
inline constexpr int kAudioCoverCeilingMs = 120;
// The least one dry spell buys, for one too short to measure well.
inline constexpr int kAudioCoverStepMs = 10;
// How long without running dry before any cover is given back, and how much a
// second after that. Two milliseconds a second is a fifth of what
// kAudioMaxRateAdjust lets the correction take off, so giving back never
// saturates it -- and 120ms of cover still takes only a minute to return.
// Asymmetric on purpose, for the same reason as the picture's: buying costs
// delay once, giving back too eagerly costs a gap every time the link twitches.
inline constexpr int kAudioCalmMsBeforeGivingBack = 3000;
inline constexpr int kAudioCoverGivenBackPerSecondMs = 2;
// A wait between pushes this long is the stream stopping -- a menu up on the
// host, the game paused -- not the link holding it back, and buys no cover:
// a stall this long is past anything the ceiling could cover anyway.
inline constexpr int kAudioStreamStoppedMs = 500;
// Frames faded in when sound resumes after the queue ran dry. The device has
// been playing silence, so the first sample back is otherwise a step from
// nothing to wherever the waveform happens to be -- a click on top of the gap.
inline constexpr std::size_t kAudioResumeFadeFrames = 64;

// The cover to hold after a dry spell of `dry_frames`, from `current_frames`
// already held: the spell's own length on top, at least `step_frames`, and
// never past `ceiling_frames`.
std::size_t audio_cover_after_running_dry(std::size_t current_frames, std::size_t dry_frames,
                                          std::size_t step_frames, std::size_t ceiling_frames);

// How long the device went without anything to play, in frames, given that
// `since_last_push_frames` worth of time has passed since the last push and
// that push left `queued_after_last_push_frames` waiting. Zero when the queue
// should still have had something in it, which is a spell too short to see.
std::size_t audio_dry_spell_frames(std::size_t since_last_push_frames,
                                   std::size_t queued_after_last_push_frames);

// The lowest the queue has been lately, which is the difference between a
// queue that is genuinely too deep and one that is merely spiky.
//
// Audio does not arrive evenly. A companion drains its socket once per pass of
// the frame loop, so several packets land together and then nothing lands for
// a whole frame; the queue jumps on every burst and drains between them. Read
// at the moment of a push, the depth is whatever the burst just made it, and a
// trim that fires on that is throwing away jitter -- which is exactly what the
// queue's depth exists to absorb. Measured on a Mac companion: 12 to 82ms
// discarded every second across up to twenty splices, while the source was
// sending only about 3ms a second more than real time, and the queue still ran
// dry several times in the same second.
//
// A surplus is what survives the low point. Whatever a burst adds, it is gone
// again by the next trough, so only audio that is still queued when the queue
// is at its emptiest is audio nobody is ever going to want.
//
// The same shape as the video jitter estimate, which takes the fastest transit
// in its window as the link with nothing queued on it, for the same reason.
class AudioLowWater {
public:
    // `window_frames` is how much audio fills one bucket -- see
    // kAudioLowWaterWindowMs -- and `queued` is where the queue is starting
    // from, so a fresh stream does not read as empty before anything has
    // happened.
    void reset(std::size_t window_frames, std::size_t queued);
    // Records a push of `frames` against a queue currently `queued` deep, and
    // returns the low-water mark as it now stands.
    std::size_t note(std::size_t queued, std::size_t frames);
    std::size_t depth() const;

private:
    std::size_t window_frames_ = 0;
    std::size_t filled_frames_ = 0;
    std::size_t current_ = 0;
    std::size_t previous_ = 0;
};

// How many frames of a newly arrived block to throw away, given how deep the
// queue has been staying. Zero until that is more than `margin_frames` past
// the target.
//
// `sustained_frames` is the low-water mark rather than the depth at this
// instant -- see AudioLowWater for why the difference is the whole point.
//
// Capped at half the block so there is always audio on both sides of the join
// for audio_splice() to cross between -- a whole block dropped is a hard cut,
// and paying a large surplus off over several blocks costs nothing, since the
// queue is read afresh on each one.
std::size_t audio_trim_frames(std::size_t sustained_frames, std::size_t target_frames,
                              std::size_t margin_frames, std::size_t block_frames);

// How deep the queue is held, in the device's frames, for a device running at
// `device_rate` that takes `device_buffer_frames` from the queue at a time.
//
// kAudioTargetQueueMs, where the device takes little at a time. A device that
// takes a lot at once empties the queue in steps of that much, so its depth is
// a sawtooth around the target: it has to hold a whole buffer at the moment of
// each take, which on average is half a buffer above where it bottoms out --
// so the target is half a buffer plus the usual cover. Not two whole buffers,
// which is what this used to be: on a Switch, whose device takes 4096 frames
// at a time, that kept a quarter of a second queued that never came near
// running dry, and the sound that far behind the picture.
std::size_t audio_target_frames(int device_rate, std::size_t device_buffer_frames);

// Appends `frames` of stereo int16 to `out` with the first `skip` of them
// dropped, crossfading from what was dropped into what follows so the join is
// not a step. With `skip` of zero this is a plain copy.
void audio_splice(const int16_t* interleaved_stereo, std::size_t frames, std::size_t skip,
                  std::vector<int16_t>& out);

// Playback rate to run at given how deep the queue currently is. Above the
// target it returns slightly more than 1 (consume input faster, emit fewer
// frames, so the queue drains); below it, slightly less. Clamped to
// kAudioMaxRateAdjust either way and eased towards from `current` rather than
// jumped to, so the rate itself never steps audibly.
double audio_drift_ratio(std::size_t queued_frames, std::size_t target_frames, double current);

// Linear-interpolating resampler for stereo int16 at a ratio very near 1.
//
// Split out from AudioOutput because it is the part worth testing on its own:
// it has no device, no SDL and no timing, just samples in and samples out. The
// state it does carry -- a fractional read position and the previous block's
// last frame -- is exactly what keeps one block continuous with the next, so
// correcting drift never itself becomes the discontinuity it exists to avoid.
class AudioResampler {
public:
    void reset();
    // Appends to `out` rather than clearing it.
    void resample(const int16_t* interleaved_stereo, std::size_t frames, double ratio,
                  std::vector<int16_t>& out);

private:
    double phase_ = 0.0;
    int16_t last_frame_[2] = {0, 0};
    bool have_last_ = false;
};

// Stereo 16-bit audio out, fed by the core's sample batches or by a host's
// streamed ones.
//
// Two clocks are always involved and they are never quite the same: whatever
// produces the samples (an emulated console's audio rate, or a host machine's,
// arriving over a network) and the sound device actually playing them. Left
// alone the queue between them drifts -- one way until it is deep enough to be
// audible as lag, the other until it runs dry.
//
// Drift -- two clocks that are near enough the same and slowly parting -- is
// not handled by throwing samples away. Discarding a whole packet because the
// queue got long puts a step discontinuity in the middle of a waveform, which
// is precisely what a click or a pop is, and it does it silently and
// repeatedly for as long as the drift continues -- so the symptom is a stream
// that pops every few seconds while every packet arrives intact and nothing
// anywhere reports a loss. Instead the queue is held near
// kAudioTargetQueueMs by bending the playback rate a fraction of a percent
// (see audio_drift_ratio), which is inaudible and self-correcting.
//
// Surplus -- a source genuinely sending more than a second of audio per second
// -- is the other case, and it cannot be resampled away at any rate worth
// listening to. That much is thrown away, a few milliseconds at a time and
// crossfaded across the join, which is the least damage available: see
// kAudioTrimMarginMs and audio_splice.
class AudioOutput {
public:
    ~AudioOutput();

    bool open(double sample_rate, std::string* error);
    void close();
    bool is_open() const { return device_ != 0; }

    void push(const int16_t* interleaved_stereo, std::size_t frames);
    void set_paused(bool paused);
    // Drops everything queued — used when resuming from suspend, where the
    // queue is both stale and arbitrarily deep.
    void flush();

    double sample_rate() const { return sample_rate_; }
    std::size_t queued_frames() const;
    // The rate correction currently being applied, for tests and diagnostics.
    double rate_ratio() const { return rate_ratio_; }

    // Says once a second what the queue has been doing, if anything has been
    // pushed since the last time. `now_ms` is the caller's own clock, the same
    // one the session's stream report runs on -- the two lines are meant to be
    // read side by side.
    //
    // Cheap to call every frame: it does nothing at all until a second has
    // passed.
    void report(uint64_t now_ms);

private:
    // Queues target_frames_ of silence so playback starts from the depth the
    // correction is aiming to hold, rather than from empty -- which would
    // otherwise underrun immediately and have to climb out of it.
    void prime();
    // Buys cover for a dry spell found at this push, or gives some back after
    // a calm one. See kAudioCoverCeilingMs.
    void update_cover(uint64_t now_ms, std::size_t queued, std::size_t frames);
    std::size_t aimed_frames() const { return target_frames_ + cover_frames_; }

    SDL_AudioDeviceID device_ = 0;
    SDL_AudioSpec spec_ {};
    SDL_AudioStream* stream_ = nullptr;  // only when the device refused our rate
    double sample_rate_ = 0.0;
    std::size_t target_frames_ = 0;
    // Cover bought after running dry, on top of target_frames_; see
    // kAudioCoverCeilingMs. What the queue is aimed at is the sum of the two.
    std::size_t cover_frames_ = 0;
    std::size_t cover_ceiling_frames_ = 0;
    std::size_t cover_step_frames_ = 0;
    // When the last push happened and how much it left queued, which is what
    // says how long a dry spell lasted; when the queue last ran dry, and when
    // cover was last given back. Milliseconds on a steady clock; 0 is never.
    uint64_t last_push_ms_ = 0;
    std::size_t queued_after_last_push_ = 0;
    uint64_t last_dry_ms_ = 0;
    uint64_t last_given_back_ms_ = 0;
    std::size_t panic_frames_ = 0;
    std::size_t panic_margin_frames_ = 0;
    std::size_t trim_margin_frames_ = 0;
    // What the trim is actually read against -- see AudioLowWater.
    AudioLowWater low_water_;
    std::size_t low_water_window_frames_ = 0;
    double rate_ratio_ = 1.0;
    // What report() says. Cleared every time it does.
    uint64_t stats_at_ms_ = 0;
    uint64_t stats_queue_total_ = 0;
    std::size_t stats_queue_worst_ = 0;
    // The lowest the queue got in the second just gone, which says whether a
    // deep average is a queue that is genuinely running late or one that is
    // merely arriving in lumps. Sentinel-high so the first push sets it.
    std::size_t stats_queue_lowest_ = static_cast<std::size_t>(-1);
    unsigned stats_pushes_ = 0;
    unsigned stats_empty_ = 0;
    unsigned stats_panics_ = 0;
    unsigned stats_trims_ = 0;
    uint64_t stats_trimmed_frames_ = 0;
    // Frames handed in since the last report, against the wall clock it took
    // to hand them in. The queue depth says the two ends disagree; only this
    // says which way and by how much, and whether the disagreement is the
    // sink's or the source's -- a source sending 49000 frames a second of
    // "48000 Hz" audio is a surplus no playback rate should be asked to
    // absorb. See kAudioMaxRateAdjust.
    uint64_t stats_frames_in_ = 0;
    AudioResampler resampler_;
    std::vector<int16_t> spliced_;
    std::vector<int16_t> scratch_;
    bool primed_ = false;
    bool paused_ = true;
};

}  // namespace ds
