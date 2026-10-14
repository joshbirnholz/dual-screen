#include "net/host_session.hpp"

#include <algorithm>
#include <cstring>
#include <random>

#include "util/log.hpp"

namespace ds::net {
namespace {

// A fresh one per companion, unpredictable enough that a stray datagram from
// a previous session (or from anything else that happens to find this port)
// essentially never matches it -- see session_token_'s own comment. Not
// trying to be a security boundary, just a filter: this is a LAN between two
// of the player's own devices.
uint64_t random_session_token() {
    static std::mt19937_64 engine(std::random_device{}());
    std::uniform_int_distribution<uint64_t> distribution;
    return distribution(engine);
}

// Called on every FrameAck, for each screen's tile diffing -- see
// previous_top_/bottom_ and tile_snapshots_top_/bottom_'s own comments. Finds
// the newest still-held snapshot at or before `acked_serial`, promotes it to
// be the new confirmed baseline, and discards every snapshot up to and
// including it: an ack for serial N confirms the companion has everything a
// diff up to N could have carried, so anything this end was holding onto in
// case of an earlier loss is now moot.
void promote_tile_baseline(std::vector<uint8_t>& baseline,
                           std::deque<std::pair<uint64_t, std::vector<uint8_t>>>& snapshots,
                           uint64_t acked_serial) {
    while (!snapshots.empty() && snapshots.front().first <= acked_serial) {
        baseline = std::move(snapshots.front().second);
        snapshots.pop_front();
    }
}

// The far end's own display can never show more than it has pixels for, so
// streaming a picture wider or taller than that spends bandwidth (and, worse,
// keyframe size -- see kMaxMediaBytesPerPoll's own comment) on detail nobody
// will ever see, over the very link a giant keyframe most needs to not
// overwhelm. Scales the source down to fit inside the display, preserving its
// aspect ratio (so nothing is stretched, only reduced), and never scales up:
// a display bigger than the source is not a reason to invent detail that was
// never there. A multiple of 16 comes out either way, on both counts: 4:2:0
// only needs even dimensions, but a software x264 encode of a small,
// non-macroblock-aligned width has been observed to corrupt memory outright
// on some builds (this app's own test suite reproduced it: 428x240 crashed,
// 320x240 and 432x240 did not) rather than merely losing the partial edge
// macroblock the way a hardware encoder does -- and this is exactly the
// resolution range a display-size cap moves into, where the source itself
// (854 wide, and never a problem) would not have. Landing on a multiple of
// 16 sidesteps having a partial macroblock at all.
void compute_capped_encode_size(int source_width,
                                int source_height,
                                int display_width,
                                int display_height,
                                int& out_width,
                                int& out_height) {
    out_width = source_width;
    out_height = source_height;
    if (display_width <= 0 || display_height <= 0) {
        return;  // Nothing reported yet -- see companion_display_size().
    }
    const double scale = std::min(
        1.0, std::min(static_cast<double>(display_width) / source_width,
                      static_cast<double>(display_height) / source_height));
    if (scale >= 1.0) {
        return;  // The source already fits; scaling up is not this function's job.
    }
    // A floor rather than letting a tiny reported display size shrink this to
    // nothing: 64 is well below anything worth encoding at all, just large
    // enough that the codec and scaler never see a degenerate width or height
    // -- and already a multiple of 16, so it also holds as the floor below.
    constexpr int kMinEncodeDimension = 64;
    constexpr int kAlignment = 16;
    out_width = std::max(kMinEncodeDimension,
                         static_cast<int>(source_width * scale) / kAlignment * kAlignment);
    out_height = std::max(kMinEncodeDimension,
                          static_cast<int>(source_height * scale) / kAlignment * kAlignment);
}


// If the companion cannot keep up, dropping frames is the right failure: the
// alternative is an ever-growing queue and input that lags further behind.
//
// Counted in frames, not bytes. Bytes were a stand-in for frames back when a
// frame was the whole picture and they were all much of a size; once frames
// became differences from the last one they stopped being anything of the
// sort. A DS screen with little moving on it compresses to well under a
// hundred bytes, so a byte limit written to hold two or three frames was
// holding well over a thousand -- which is not a backlog limit at all, it is
// half a minute of delay that no frame is ever dropped to relieve.
//
// What the limit is really about is how far behind what the player sees is
// allowed to fall, and that has always been a number of frames.
//
// Four, and the number is a bandwidth-delay product rather than a taste
// setting. A frame cannot be sent until an earlier one is acked, so the most
// this end can ever sustain is the window divided by how long an ack takes to
// come back: at 60fps and the ~40ms an ack actually takes here (the paced send
// itself, the flight, the companion's own poll, and the flight back), anything
// under about 2.5 is a frame rate ceiling and nothing else.
//
// Two was that ceiling. Measured with the encoder no longer stalling the send
// loop, Bayonetta 2 produced 60 frames a second, sent 50, dropped 9, and sat
// at exactly two frames outstanding in every single sample -- the window full,
// permanently, which is what a window too small for the link looks like. The
// dropped frames were not frames the link could not carry: bandwidth was about
// 18 Mbps of a LAN's headroom.
//
// A bigger window does not queue frames at this end -- a frame that does not
// fit is dropped, not held -- so widening it converts drops into delivered
// frames rather than into delay. It costs only when the link genuinely cannot
// carry what is offered, and that case is bounded elsewhere: pacing spreads
// the bytes, and kAckStallMs recovers if the acks stop entirely.
//
// So four was the same mistake as two, one notch along: a bandwidth-delay
// product computed once, by hand, for a link with a 40ms round trip. A Steam
// Deck on wifi averages 40ms and has a tail at 110 to 225ms, and every excursion
// past 4/60fps = 66ms shuts the gate and drops frames in a burst until the tail
// passes. Measured there: 60 to 82 frames produced a second and 15 to 48 sent,
// with bandwidth *falling* as it happened, which is what a window-bound link
// looks like rather than a full one.
//
// The product is now actually computed, from the round trip this link is
// showing and the rate frames are really being produced at. On a link with a
// 40ms round trip at 60fps it works out at four, which is where it came in.
// See frames_in_flight_allowed().
constexpr std::size_t kMinFramesInFlight = 2;
constexpr std::size_t kMaxFramesInFlight = 8;
// Past this a round trip is not a delay to keep pace with, it is congestion,
// and matching it would be adding to it. At 60fps this ceiling is still eight
// frames, so the clamp above is what actually binds; it is here so that a link
// melting down cannot walk the window out on its own.
constexpr uint64_t kMaxCoveredRoundTripMs = 120;
// How fast the high-water round trip is given back, per ack. At the fifty-odd
// acks a second this sees, a spike is most of the way forgotten in a second
// and a half -- long enough that a tail arriving in bursts stays covered,
// short enough that a link which has settled stops paying for it.
constexpr double kRoundTripDecayPerAck = 0.99;
// How fast the quickest round trip seen is allowed to creep back up, per ack:
// a millisecond a second or so at the usual rate of acks, so a route that has
// really got slower is followed, and a single quick trip is not the floor for
// ever.
constexpr double kRoundTripFloorRisePerAck = 0.02;

// How long the oldest still-unacked frame is allowed to sit before this end
// stops waiting for its ack and treats it as gone rather than merely late.
// UDP never retries a lost datagram, so if every frame currently allowed to
// be outstanding (kMaxQueuedFrames of them) is lost together -- one bad
// burst is enough, since two is all this ever lets be in flight -- the ack
// that would open the gate again is never coming, and without this, nothing
// here would ever send another frame: sent_serial_ stuck at
// acked_serial_ + kMaxQueuedFrames forever, looking to both ends like a
// picture that simply stopped, audio included, with the link itself still
// up (this was reproduced live: a real packet-loss burst on real hardware,
// confirmed from both ends' own logs -- the companion's decode rate falling
// to zero and staying there, not recovering, while the host's own sent/s
// does the same). Three seconds is well clear of the round trips this link
// already tolerates while still recovering -- kVideoRetryCooldownMs below
// is the other constant already picked at this same rough scale.
constexpr uint64_t kAckStallMs = 3000;
// But three seconds is only right before this link has shown what it is.
// Once it has, a frame this many round trips overdue is not late, it is gone:
// measured on a Switch over wifi, a one-second dropout -- audio lost too, so
// nothing either end could have done about it -- froze the picture for
// three, because every frame in flight when the dropout began was lost with
// it and the wait for their acks ran its full course after the link was back.
// Six round trips of a 45ms link is 270ms, lifted to the floor below so a
// device that simply had a slow moment is not mistaken for a lost one.
constexpr double kAckStallRoundTrips = 6.0;
constexpr uint64_t kMinAckStallMs = 400;
// A companion repeats its newest ack while nothing newer arrives. One that
// still does not cover a frame sent this many round trips before it is a
// companion saying that frame is not coming -- the link is up, the ack got
// here, and the frame did not. Three round trips of slack for a device that
// was slow to look, and never under the floor, however quick the link.
constexpr double kLostAfterRoundTrips = 3.0;
constexpr uint64_t kMinLostAfterMs = 100;

// Audio needs no backlog limit of its own any more. The old one existed
// because a fat queued video frame sitting ahead of it in the same TCP
// connection could starve it out or force a byte-count guess at how much of
// that shared queue was really audio's; UDP has neither problem, since a send
// either goes out now or is simply gone -- there is nothing here for a queue
// to grow into in the first place.

// How much of one UDP media datagram is left for a Frame message's payload
// once the media header (encode_media_header()) and the fragment header
// (encode_frame_fragment()) are both accounted for -- see
// kMaxMediaDatagramBytes for why the datagram itself is capped where it is.
constexpr std::size_t kMediaHeaderBytes = 9;    // u64 session_token + u8 type
constexpr std::size_t kFragmentHeaderBytes = kFrameFragmentHeaderBytes;
// Less the parity prefix, so a recovery piece -- the prefix and a full
// piece's worth of XOR -- fits in the same datagram as the pieces it covers.
constexpr std::size_t kMaxFragmentPayload =
    kMaxMediaDatagramBytes - kMediaHeaderBytes - kFragmentHeaderBytes - kParityPrefixBytes;

// Audio's own fixed header inside encode_audio_payload(): u8 stream + u32
// sample_rate + u32 sample count.
constexpr std::size_t kAudioPayloadHeaderBytes = 9;
// A batch handed to publish_audio() can be much larger than one datagram
// safely holds -- unlike a video frame, audio has no fragment header or
// reassembly to fall back on, so it is capped at the source instead, split
// into as many independent packets as it takes. Independent is exactly what
// a run of PCM samples already is: there is no cross-packet state to carry,
// so this costs nothing beyond a few extra small sends.
constexpr std::size_t kMaxAudioFramesPerPacket =
    (kMaxMediaDatagramBytes - kMediaHeaderBytes - kAudioPayloadHeaderBytes) / (2 * sizeof(int16_t));

// How long a failed video encoder stays backed off to tiles before the next
// frame is allowed to try it again. Long enough that a genuinely broken
// backend is not reopened and warned about every frame, short enough that a
// cold GPU encoder or one bad frame costs a few seconds of tiles rather than
// tiles for the rest of the connection -- see use_video_for().
constexpr uint64_t kVideoRetryCooldownMs = 3000;
// How long a screen chosen for video goes back to tiles when video fails --
// long enough not to try a broken encoder every few seconds, since the tiles
// in its place are working.
constexpr uint64_t kVideoFallbackMs = 30000;

// drain_pending_media_sends() paces how fast pending_media_sends_ empties
// onto the socket by elapsed wall-clock time rather than a flat number of
// bytes per poll() call -- see its own definition. kPacingHeadroomMultiplier
// is how far above the encoders' own combined configured rate that pacing
// clock runs: comfortably above 1 so an ordinary frame, and even a moderately
// oversized one, still clears in a tick or two rather than perpetually
// trailing the rate new frames arrive at, while still spreading a much
// bigger keyframe burst across several poll() calls instead of one -- see
// kMaxMediaDatagramBytes and send_frame_fragments() for why a keyframe still
// has to leave as many small datagrams either way.
// The pacer exists for one thing: a keyframe is tens of times an ordinary
// frame, and firing all of it at the wire at once is a burst the link then has
// to absorb. It was never meant to slow an ordinary frame down.
//
// At four times the measured rate it was doing exactly that. Measured with the
// bitrate now actually binding: a typical frame is 52KB, the stream is 1740
// KB/s, and the pacing queue averaged 12.8ms -- a third of a frame period at
// 30fps, spent on frames that are not bursts by any definition. That is the
// pacer being the latency again, more quietly than the 35ms it managed at two
// times, but for no benefit at all on a frame this size.
//
// Sixteen puts an ordinary frame out in under two milliseconds and still
// spreads a 400KB keyframe across about fourteen -- which is the case this is
// for. It remains a ceiling on how fast a burst may leave rather than a rate
// anything sends at: nothing is ever sent that was not produced.
constexpr double kPacingHeadroomMultiplier = 16.0;
// A floor under the pacing rate for whenever nothing has told it a real
// number yet -- video not open (tiles, or nothing sent since a companion
// connected -- see drop_companion()). This app's old flat rate before
// per-resolution bitrate existed, so a tile-only or not-yet-started session
// paces exactly as it always used to rather than at whatever a zero would
// otherwise compute to.
constexpr uint32_t kMinPacingBitrateKbps = 8000;
// However little time has passed since the last drain, always let at least
// one fragment through. Otherwise two poll() calls close enough together to
// round to zero elapsed milliseconds -- easy on a machine running well above
// 1000 calls a second -- would starve the queue outright rather than merely
// pace it.
constexpr std::size_t kMinDrainBudgetBytes = kMediaHeaderBytes + kFragmentHeaderBytes + kMaxFragmentPayload;
// An upper bound on how much elapsed time one drain call is credited for, so
// that a long gap since the last one -- a companion that just connected, a
// slow test, a machine that stalled for a moment -- does not hand back a
// single enormous budget that empties the whole queue in one burst, which is
// exactly what pacing exists to avoid. 250 ms is generous next to a normal
// poll() cadence (tens of milliseconds at most) while still being nowhere
// near long enough to matter as a stall.
constexpr uint64_t kMaxPacingElapsedMs = 250;

// How long a window the produced-byte rate is measured over. Half a second is
// long enough to average across a keyframe and the deltas after it, short
// enough to follow a game that changes what it is drawing.
constexpr uint64_t kProducedRateWindowMs = 500;

// What produce_period_ms_ starts at before anything has been measured: one
// frame of 60fps.
constexpr double kAssumedProducePeriodMs = 16.7;

}  // namespace

// What to ask the encoder for, as a budget for the link rather than a quality
// target for a frame.
//
// This used to be bits per pixel per frame times a frame rate, and both halves
// of that were wrong in ways that cancelled into a number that did nothing.
//
// The frame rate was assumed to be 60 and the game runs at 30, so the ask came
// out at 43.5 Mbit/s for a 1080p stream. And the ask is a *ceiling*, not an
// instruction: h264_vaapi honours it perfectly well -- asked for 22.9 it made
// 23.0, asked for 22.7 it made 23.5 -- but at 43.5 it was being handed a
// ceiling twice as high as the content wanted, so it produced the 23 Mbit/s
// its quality settled at and the number never bound on anything. I read those
// same four measurements as an encoder ignoring its bitrate, and said so
// twice. It was not ignoring it; it was never being asked for less than it
// already wanted.
//
// So this is now a rate for the wire, which is the thing actually in short
// supply: 23 Mbit/s of UDP leaving a handheld's wifi in eighty-datagram bursts
// thirty times a second is what a far end measures as three quarters of frames
// within 16ms of the quietest and one in twenty 79ms behind it, with nothing
// lost at all. A tail without loss is queueing, and after ruling out this end's
// pacing, the other end's loop and the evenness of the frames, the air is what
// is left.
//
// Twelve bits per pixel per second puts 1080p at about 24 Mbit/s. Per second
// and not per frame, so there is no frame rate to assume and none to get
// wrong: a link budget is a link budget whether the game draws thirty pictures
// into it or sixty.
//
// It was six for one session, on the theory that the tail of late arrivals was
// air time and fewer bytes would shorten it. That is now answered, and the
// answer is no: the bytes went from 2900 KB/s to 959, a third of what they
// were, and the tail did not move at all -- 72.8ms at the ninety-fifth
// percentile before, 72.0ms after. It cost picture quality and bought nothing,
// so it is back up. Whatever that tail is, it is not this stream's volume.
//
// The floor matters as much as the slope. Bits per pixel has to rise as a
// picture gets smaller -- the same blockiness costs proportionally more at
// 480p than at 1080p -- and a straight line through the origin starves the
// small end, so a Wii U GamePad's native 854x480 is held up by the floor
// rather than by the slope.
int bitrate_kbps_for_encode_size(int width, int height, double frames_per_second) {
    constexpr double kBitsPerPixelPerSecond = 12.0;
    constexpr int kMinBitrateKbps = 4000;
    constexpr int kMaxBitrateKbps = 40000;
    // Kept in the signature because the caller measures it and because the
    // keyframe interval is still built from it; the budget itself no longer
    // depends on it, which is the point.
    (void)frames_per_second;
    const double bits_per_second =
        static_cast<double>(width) * static_cast<double>(height) * kBitsPerPixelPerSecond;
    const int kbps = static_cast<int>(bits_per_second / 1000.0);
    return std::clamp(kbps, kMinBitrateKbps, kMaxBitrateKbps);
}

// How fast the pacer is allowed to hand fragments to the socket, in bytes per
// millisecond.
//
// Built from what the encoder actually produces, not from what it was asked
// for, because those turn out not to be the same thing. Halving the number
// handed to h264_vaapi on a Steam Deck changed the bytes on the wire by
// nothing at all -- 43.5 Mbit/s asked, 20.1 produced; 22.9 asked, 23.0
// produced; 34.6 asked, 21.5 produced; 22.7 asked, 23.5 produced. The driver
// there does what it likes and the request is a suggestion.
//
// That made the declared rate an unsafe thing to pace from: it halved the
// budget while the traffic stayed where it was, so bursts that used to leave
// immediately queued instead, and the pacing queue -- latency this end adds,
// on top of everything the link does -- went from 14.6ms average to 34.9ms,
// worst cases of 120 and 159ms, with the round trip following it up from 32ms
// to 40ms.
//
// The measured rate cannot drift from reality that way. The declared one is
// only a stand-in for the first half second, before anything has been
// produced to measure.
double media_pacing_bytes_per_ms(uint32_t declared_kbps, double produced_bytes_per_ms) {
    constexpr double kFloorBytesPerMs = static_cast<double>(kMinPacingBitrateKbps) / 8.0;
    const double declared_bytes_per_ms = static_cast<double>(declared_kbps) / 8.0;
    const double base =
        produced_bytes_per_ms > 0.0 ? produced_bytes_per_ms : declared_bytes_per_ms;
    return std::max(base, kFloorBytesPerMs) * kPacingHeadroomMultiplier;
}

uint64_t ack_stall_ms(double round_trip_ms, bool measured) {
    if (!measured || !(round_trip_ms > 0.0)) {
        return kAckStallMs;
    }
    const double scaled = round_trip_ms * kAckStallRoundTrips;
    return static_cast<uint64_t>(std::max(static_cast<double>(kMinAckStallMs),
                                          std::min(scaled, static_cast<double>(kAckStallMs))));
}

std::size_t frames_in_flight_allowed(uint64_t round_trip_ms, double produce_period_ms,
                                     uint64_t return_delay_ms) {
    if (!(produce_period_ms > 0.0)) {
        return kMinFramesInFlight;
    }
    // The ceiling is for congestion, which is on the way out: frames piling
    // up in front of the companion. Delay on the way back -- acks held up
    // while the frames themselves arrive on time -- is not that, and a window
    // that shut on it dropped frames the companion would have shown. Seen on
    // a Deck over wifi at 30fps: the round trip jumping to 100-135ms every
    // so often, the window full at five, two to four frames a second not
    // sent, and an Apple TV at the far end reporting every frame on time.
    const double covered =
        std::min(static_cast<double>(round_trip_ms),
                 static_cast<double>(kMaxCoveredRoundTripMs + return_delay_ms));
    // One more than the product itself, so the gate opens as the ack for the
    // oldest frame lands rather than one frame after it -- at exactly the
    // product, every frame waits for an ack that is already on its way.
    const double frames = std::ceil(covered / produce_period_ms) + 1.0;
    if (frames <= static_cast<double>(kMinFramesInFlight)) {
        return kMinFramesInFlight;
    }
    if (frames >= static_cast<double>(kMaxFramesInFlight)) {
        return kMaxFramesInFlight;
    }
    return static_cast<std::size_t>(frames);
}

bool HostSession::start(const Config& config, std::string* error) {
    stop();
    start_encoder_thread();
    config_ = config;
    companion_screen_ = (config.host_screen == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;

    if (!listener_.start(kSessionPort, error)) {
        return false;
    }
    if (!media_socket_.start(kMediaPort, false, error)) {
        listener_.stop();
        return false;
    }
    // Port 0: this socket only ever sends, to an address/port the companion
    // already told media_socket_ above, so it needs no fixed port of its own
    // for the companion to find -- see audio_media_socket_'s own comment.
    if (!audio_media_socket_.start(0, false, error)) {
        listener_.stop();
        media_socket_.stop();
        return false;
    }
    std::string announce_error;
    if (!announcer_.start(&announce_error)) {
        // Discovery is a convenience; a session still works if a companion is
        // pointed at this address directly.
        log_warn("cannot announce this host on the network: " + announce_error);
    }
    refresh_beacon();
    log_info("hosting " + config_.game_title + " on port " + std::to_string(listener_.port()));
    return true;
}

void HostSession::stop(bool companion_quits_too) {
    if (companion_.open()) {
        std::vector<uint8_t> bye;
        encode_bye(bye, companion_quits_too);
        companion_.send(bye);
        std::string error;
        std::vector<uint8_t> ignored;
        companion_.pump(ignored, &error);  // best effort
    }
    companion_.close();
    // Before the sockets it sends on close under it.
    stop_encoder_thread();
    listener_.stop();
    media_socket_.stop();
    audio_media_socket_.stop();
    announcer_.stop();
    inbox_.clear();
    handshake_done_ = false;
    companion_name_.clear();
    companion_input_ = DsInputState();
    companion_input_fresh_ = false;
    input_sequence_known_ = false;
    echoed_any_ = false;
    touch_unseen_ = false;
    release_waiting_ = false;
    // Not reset by drop_companion() like everything else above -- that is the
    // whole point of it -- so a fresh hosting session has to clear it itself.
    companion_wants_quit_ = false;
}

void HostSession::set_codec_choice(CodecChoice choice) {
    if (choice == codec_choice_) {
        return;
    }
    codec_choice_ = choice;
    // Changed. This is called every frame with the same answer, so reaching
    // here at all means somebody changed their mind -- which is reason enough
    // to try an encoder that failed before.
    video_retry_at_ms_ = 0;
    video_error_reported_ = false;
}

bool screen_goes_as_video(const CodecEvidence& evidence) {
    if (!evidence.video_possible) {
        return false;
    }
    if (static_cast<long long>(evidence.width) * evidence.height >= kVideoFromPixels) {
        return true;
    }
    if (evidence.tile_kbytes_per_second >= kVideoFromTileKBytesPerSecond) {
        return true;
    }
    // Gone to video for being busy: kept there a while rather than measured
    // again every second, since each switch is a whole picture sent at once.
    return evidence.on_video_for_weight && evidence.ms_on_video < kVideoForWeightMs;
}

bool HostSession::video_possible() const {
    return video::can_encode() && companion_accepts_video_ &&
           monotonic_millis() >= video_retry_at_ms_;
}

CodecEvidence HostSession::codec_evidence(ScreenId screen, int width, int height) const {
    const CodecState& state = screen == ScreenId::Top ? codec_top_ : codec_bottom_;
    CodecEvidence evidence;
    evidence.width = width;
    evidence.height = height;
    evidence.video_possible = video_possible();
    evidence.tile_kbytes_per_second = state.video ? 0.0 : state.tile_kbytes_per_second;
    evidence.on_video_for_weight = state.video && state.video_for_weight;
    evidence.ms_on_video = state.video ? monotonic_millis() - state.since_ms : 0;
    return evidence;
}

bool HostSession::sends_video(ScreenId screen, int width, int height) const {
    switch (codec_choice_) {
        case CodecChoice::Tiles: return false;
        case CodecChoice::H264: return true;
        case CodecChoice::Automatic: break;
    }
    return screen_goes_as_video(codec_evidence(screen, width, height));
}

void HostSession::switch_codec(ScreenId screen, bool video, const char* why) {
    CodecState& state = screen == ScreenId::Top ? codec_top_ : codec_bottom_;
    if (state.video == video) {
        return;
    }
    state.video = video;
    state.since_ms = monotonic_millis();
    state.tile_bytes = 0;
    state.tile_window_start_ms = state.since_ms;
    state.tile_kbytes_per_second = 0.0;
    log_info("the " + std::string(to_string(screen)) + " screen goes as " +
             (video ? "video" : "tiles") + " now: " + why);
    if (video) {
        // Whatever it held from before means nothing to a decoder that has
        // been reading tiles since; the next frame stands alone.
        ((screen == ScreenId::Top) ? video_keyframe_owed_top_ : video_keyframe_owed_bottom_) = true;
        return;
    }
    // Back to tiles, which only send what changed since the last tiled frame
    // the companion confirmed -- and it has been showing video since then. So
    // no baseline at all: the next one is the whole picture. The encoder is
    // closed by the thread it belongs to, after anything it still has queued.
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        VideoJob close;
        close.close = true;
        close.screen = screen;
        video_jobs_.push_back(std::move(close));
    }
    video_jobs_cv_.notify_one();
    (screen == ScreenId::Top ? previous_top_ : previous_bottom_).clear();
    (screen == ScreenId::Top ? tile_snapshots_top_ : tile_snapshots_bottom_).clear();
}

void HostSession::set_host_screen(ScreenId screen) {
    const ScreenId companion = (screen == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;
    if (companion != companion_screen_) {
        companion_screen_ = companion;
        session_state_dirty_ = true;
        // The companion now wants the other screen, whole and at once --
        // Separate Screens' swap.
        resend_screens_ = true;
        refresh_beacon();
    }
    config_.host_screen = screen;
}

void HostSession::set_save_slots(const SaveSlotsMessage& slots) {
    save_slots_ = slots;
    save_slots_known_ = true;
    save_slots_dirty_ = true;
}

void HostSession::set_controllers_view(const ControllersViewMessage& view) {
    std::vector<uint8_t> encoded;
    encode_controllers_view(view, encoded);
    if (encoded == controllers_view_) return;
    controllers_view_ = std::move(encoded);
    controllers_view_dirty_ = true;
}

void HostSession::set_graphic_packs_view(const GraphicPacksViewMessage& view) {
    std::vector<uint8_t> encoded;
    encode_graphic_packs_view(view, encoded);
    if (encoded == graphic_packs_view_) return;
    graphic_packs_view_ = std::move(encoded);
    graphic_packs_view_dirty_ = true;
}

void HostSession::set_session_state(const SharedState& state) {
    if (state.per_device_layout != shared_.per_device_layout ||
        state.menu_open != shared_.menu_open) {
        // The set of screens the companion needs depends on both: a menu draws
        // every layout as a preview, whichever device it was opened from.
        resend_screens_ = true;
    }
    if (state.layout_id != shared_.layout_id ||
        state.per_device_layout != shared_.per_device_layout || state.paused != shared_.paused ||
        state.bilinear_filtering != shared_.bilinear_filtering ||
        state.audio_target != shared_.audio_target ||
        state.handedness != shared_.handedness ||
        state.right_stick_mode != shared_.right_stick_mode ||
        state.render_scale != shared_.render_scale ||
        state.screen_gap != shared_.screen_gap || state.menu_open != shared_.menu_open ||
        state.menu_focus != shared_.menu_focus || state.menu_confirm != shared_.menu_confirm ||
        state.menu_confirm_focus != shared_.menu_confirm_focus ||
        state.menu_unavailable_notice != shared_.menu_unavailable_notice ||
        state.graphic_packs_available != shared_.graphic_packs_available ||
        state.amiibo_available != shared_.amiibo_available ||
        state.producer_options != shared_.producer_options || state.amiibo != shared_.amiibo ||
        state.amiibo_folder != shared_.amiibo_folder ||
        state.max_render_scale != shared_.max_render_scale ||
        state.cursor_choosable != shared_.cursor_choosable ||
        state.right_stick_choosable != shared_.right_stick_choosable ||
        state.left_stick_choosable != shared_.left_stick_choosable ||
        state.left_stick_press != shared_.left_stick_press ||
        state.rumble_choosable != shared_.rumble_choosable ||
        state.rumble_pak != shared_.rumble_pak ||
        state.lcd_filter_choosable != shared_.lcd_filter_choosable ||
        state.lcd_filter != shared_.lcd_filter ||
        state.cursor_shown != shared_.cursor_shown ||
        state.settings_open != shared_.settings_open ||
        state.save_points_available != shared_.save_points_available ||
        state.save_sync_available != shared_.save_sync_available ||
        state.resume_choosable != shared_.resume_choosable ||
        state.resume_from_save_state != shared_.resume_from_save_state ||
        state.resume_notice != shared_.resume_notice ||
        state.left_handed_layouts != shared_.left_handed_layouts) {
        shared_ = state;
        session_state_dirty_ = true;
    }
}

void HostSession::refresh_beacon() {
    BeaconMessage beacon;
    beacon.device_name = config_.device_name;
    beacon.game_title = config_.game_title;
    beacon.rom_sha1 = config_.rom_sha1;
    beacon.session_port = listener_.port() != 0 ? listener_.port() : kSessionPort;
    // Both screens are on offer until a companion takes one: the host adapts to
    // whichever it is asked for. Once paired, only the one already taken is
    // shown as unavailable.
    const bool free_slot = !companion_.open();
    beacon.top_available = free_slot;
    beacon.bottom_available = free_slot;
    announcer_.set_beacon(beacon);
}

void HostSession::drop_companion(const std::string& reason) {
    if (companion_.open()) {
        log_info("companion disconnected: " + reason);
        last_notice_ = reason;
    }
    companion_.close();
    inbox_.clear();
    handshake_done_ = false;
    companion_name_.clear();
    companion_input_ = DsInputState();
    companion_input_fresh_ = false;
    input_sequence_known_ = false;
    echoed_any_ = false;
    touch_unseen_ = false;
    release_waiting_ = false;
    have_request_ = false;
    request_ = RequestMessage();
    menu_presses_.clear();
    companion_asked_for_menu_ = false;
    // Nobody is holding a menu open on a device that has gone.
    companion_menu_open_ = false;
    // The next one starts on tiles, and is judged afresh.
    codec_top_ = CodecState();
    codec_bottom_ = CodecState();
    // Whoever connects next says its own size; a stale one from this
    // companion is worse than not knowing, since it might be wrong in either
    // direction.
    companion_display_width_ = 0;
    companion_display_height_ = 0;
    frame_sent_at_.clear();
    in_flight_frames_lost_ = false;
    resend_screens_ = false;
    sent_serial_ = 0;
    submitted_serial_ = 0;
    acked_serial_ = 0;
    // Everything the encoder thread shares, under the lock it shares it by --
    // see media_mutex_.
    std::unique_lock<std::mutex> media_lock(media_mutex_);
    // Whatever it is in the middle of was for the companion that just left.
    ++media_generation_;
    video_jobs_.clear();
    finished_tiles_.clear();
    tile_jobs_outstanding_[0] = 0;
    tile_jobs_outstanding_[1] = 0;
    audio_sends_.clear();
    video_failures_.clear();
    video_worked_ = false;
    for (const ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
        // A stream's frames only mean anything after the keyframe they
        // descend from, so whoever connects next needs a stream of their own
        // rather than the middle of somebody else's.
        VideoJob close;
        close.close = true;
        close.screen = screen;
        video_jobs_.push_back(std::move(close));
    }
    video_jobs_cv_.notify_one();
    // Whoever connects next gets a fresh token and has to say hello on the
    // media channel again -- its address there is not carried over from a
    // companion that is gone.
    session_token_ = 0;
    companion_media_address_.clear();
    companion_media_port_ = 0;
    companion_media_known_ = false;
    // A request from a companion that has gone is not one to answer.
    keyframe_asked_top_ = false;
    keyframe_asked_bottom_ = false;
    // And the next companion counts sent frames from the start rather than
    // joining part-way through a run that means nothing to it.
    sent_index_top_ = 0;
    sent_index_bottom_ = 0;
    // Back to letting the routing table decide until the next companion says
    // which of this machine's addresses it reached.
    {
        std::lock_guard<std::mutex> sending(send_mutex_);
        media_socket_.set_source_address("");
        audio_media_socket_.set_source_address("");
    }
    // Fragments queued for the companion that just left are for a session
    // nobody at the other end will ever see the token of again.
    pending_media_sends_.clear();
    // Whoever connects next starts its own pacing clock -- see
    // drain_pending_media_sends() -- rather than inheriting a rate computed
    // for a companion, and a picture, that may no longer be relevant at all.
    media_pacing_last_ms_ = 0;
    // What the last companion's stream was producing says nothing about the
    // next one's, which may be a different screen at a different size.
    media_produced_bytes_per_ms_ = 0.0;
    media_produced_window_bytes_ = 0;
    media_produced_window_at_ms_ = 0;
    video_bitrate_kbps_top_ = 0;
    video_bitrate_kbps_bottom_ = 0;
    round_trip_high_ms_ = 0.0;
    round_trip_floor_ms_ = 0.0;
    return_delay_high_ms_ = 0.0;
    produce_period_ms_ = kAssumedProducePeriodMs;
    last_produced_at_ms_ = 0;
    previous_top_.clear();
    previous_bottom_.clear();
    tile_snapshots_top_.clear();
    tile_snapshots_bottom_.clear();
    sent_video_frames_.clear();
    encode_cost_top_ = video::VideoEncoder::EncodeCost();
    encode_cost_bottom_ = video::VideoEncoder::EncodeCost();
    media_lock.unlock();
    // Their answer about decoding video is theirs too.
    video_keyframe_owed_top_ = false;
    video_keyframe_owed_bottom_ = false;
    companion_accepts_video_ = false;
    companion_has_pointer_ = false;
    // A different device may be a different size, and the encoder is opened
    // for the size it is given, so the next one gets its own attempt.
    video_retry_at_ms_ = 0;
    video_error_reported_ = false;
    // Whoever connects next has not been told anything about the motor yet --
    // its own device starts at rest, same as this one's did, so there is
    // nothing to catch it up on until the GamePad's own state next changes.
    rumble_active_ = false;
    have_sent_rumble_ = false;
    // And its pads were its own.
    companion_reports_pads_ = false;
    companion_pads_.clear();
    pad_rumble_sent_.clear();
    refresh_beacon();
}

bool HostSession::companion_wants(ScreenId screen) const {
    if (!has_companion()) {
        return false;
    }
    // With its menu up the companion draws every layout in miniature, so it
    // needs both screens whatever the live layout is.
    if (companion_menu_open_) {
        return true;
    }
    // Under a per-device layout it only needs its own screen. Under any other it
    // draws the whole thing, so it needs both.
    if (shared_.per_device_layout) {
        return screen == companion_screen_;
    }
    return true;
}

void HostSession::handle_message(MessageType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MessageType::Hello: {
            HelloMessage hello;
            if (!decode_hello(payload.data(), payload.size(), hello)) {
                drop_companion("that device sent something we could not read");
                return;
            }
            if (hello.protocol_version != kProtocolVersion) {
                RejectMessage reject;
                reject.reason = "This device is running a different version of dualscreen (theirs " +
                                std::to_string(hello.protocol_version) + ", ours " +
                                std::to_string(kProtocolVersion) + ").";
                std::vector<uint8_t> out;
                encode_reject(reject, out);
                companion_.send(out);
                std::string ignored_error;
                std::vector<uint8_t> ignored;
                companion_.pump(ignored, &ignored_error);
                drop_companion(reject.reason);
                return;
            }

            // The companion picked which screen it wants to be, so it gets it
            // and the host takes the other. Ignoring this was why "connect as
            // top screen" and "connect as bottom screen" behaved identically.
            companion_screen_ = hello.wanted_screen;
            config_.host_screen =
                (companion_screen_ == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;
            companion_name_ = hello.device_name.empty() ? "companion" : hello.device_name;
            companion_accepts_video_ = hello.accepts_video;
            companion_has_pointer_ = hello.has_pointer;
            handshake_done_ = true;
            // Fresh for this companion; see session_token_'s own comment. The
            // companion's own address on the media channel is not known yet --
            // that arrives with its first datagram there, once it has this.
            {
                // Read by a send on the encoder thread -- see media_mutex_.
                std::lock_guard<std::mutex> lock(media_mutex_);
                session_token_ = random_session_token();
                companion_media_address_.clear();
                companion_media_port_ = 0;
            }
            companion_media_known_ = false;

            WelcomeMessage welcome;
            welcome.game_title = config_.game_title;
            welcome.rom_sha1 = config_.rom_sha1;
            welcome.granted_screen = companion_screen_;
            welcome.system = config_.system;
            welcome.audio_sample_rate = config_.audio_sample_rate;
            welcome.session_token = session_token_;
            welcome.icon_width = static_cast<uint16_t>(config_.game_icon_width);
            welcome.icon_height = static_cast<uint16_t>(config_.game_icon_height);
            welcome.icon_rgba = config_.game_icon_rgba;
            std::vector<uint8_t> out;
            encode_welcome(welcome, out);
            companion_.send(out);
            session_state_dirty_ = true;
            save_slots_dirty_ = save_slots_known_;
            controllers_view_dirty_ = !controllers_view_.empty();
            graphic_packs_view_dirty_ = !graphic_packs_view_.empty();
            last_notice_ = companion_name_ + " connected";
            log_info(std::string("that device ") +
                     (companion_accepts_video_ ? "can decode video" : "cannot decode video"));
            log_info(companion_name_ + " connected as the " +
                     std::string(to_string(companion_screen_)) + " screen");
            refresh_beacon();
            break;
        }
        case MessageType::Request: {
            RequestMessage request;
            if (!decode_request(payload.data(), payload.size(), request)) {
                return;  // not worth dropping a session over
            }
            // Not one of the choices the app adopts and broadcasts back --
            // just the companion telling us how big a picture it can use.
            // Applied immediately rather than merged sticky-field style: the
            // companion resends this whenever it actually changes, so the
            // latest value is always the current one and there is nothing to
            // carry forward from a request that did not mention it.
            if (request.wants_display_size) {
                companion_display_width_ = request.display_width;
                companion_display_height_ = request.display_height;
            }
            // Held for the app to validate: the host owns these choices and
            // will not adopt a layout id it does not recognise.
            if (have_request_ && request.layout_id.empty()) {
                // Do not let a later request without a layout erase an earlier
                // one that had it; each field means only itself.
                request.layout_id = request_.layout_id;
            }
            if (have_request_ && !request.wants_filtering && request_.wants_filtering) {
                request.wants_filtering = true;
                request.bilinear_filtering = request_.bilinear_filtering;
            }
            if (have_request_ && !request.wants_audio_target && request_.wants_audio_target) {
                request.wants_audio_target = true;
                request.audio_target = request_.audio_target;
            }
            if (have_request_ && !request.wants_right_stick_mode &&
                request_.wants_right_stick_mode) {
                request.wants_right_stick_mode = true;
                request.right_stick_mode = request_.right_stick_mode;
            }
            if (have_request_ && !request.wants_left_stick_press &&
                request_.wants_left_stick_press) {
                request.wants_left_stick_press = true;
                request.left_stick_press = request_.left_stick_press;
            }
            if (have_request_ && !request.wants_rumble_pak && request_.wants_rumble_pak) {
                request.wants_rumble_pak = true;
                request.rumble_pak = request_.rumble_pak;
            }
            if (have_request_ && !request.wants_lcd_filter && request_.wants_lcd_filter) {
                request.wants_lcd_filter = true;
                request.lcd_filter = request_.lcd_filter;
            }
            if (have_request_ && !request.wants_resume_from_save_state &&
                request_.wants_resume_from_save_state) {
                request.wants_resume_from_save_state = true;
                request.resume_from_save_state = request_.resume_from_save_state;
            }
            if (have_request_ && !request.wants_handedness && request_.wants_handedness) {
                request.wants_handedness = true;
                request.handedness = request_.handedness;
            }
            if (have_request_ && !request.wants_render_scale && request_.wants_render_scale) {
                request.wants_render_scale = true;
                request.render_scale = request_.render_scale;
            }
            if (have_request_ && !request.wants_reset && request_.wants_reset) {
                request.wants_reset = true;
            }
            if (have_request_ && !request.wants_screen_swap && request_.wants_screen_swap) {
                request.wants_screen_swap = true;
            }
            if (have_request_ && !request.wants_screen_gap && request_.wants_screen_gap) {
                request.wants_screen_gap = true;
                request.screen_gap = request_.screen_gap;
            }
            // Pulled out before the rest is merged: presses are events, and
            // sticky-merging one would make it happen again next time the
            // companion asked for anything at all.
            if (request.wants_menu_focus || request.wants_menu_action) {
                MenuPress press;
                press.has_focus = request.wants_menu_focus;
                press.focus = request.menu_focus;
                press.has_action = request.wants_menu_action;
                press.action = request.menu_action;
                if (menu_presses_.size() < kMaxQueuedMenuPresses) {
                    menu_presses_.push_back(press);
                }
                request.wants_menu_focus = false;
                request.wants_menu_action = false;
            }
            request_ = request;
            have_request_ = true;
            if (request.wants_quit) {
                // Latched immediately rather than left for take_request(): the
                // companion's Bye is typically already queued right behind
                // this in the same poll(), and drop_companion() below would
                // otherwise clear request_ before the app next reads it.
                companion_wants_quit_ = true;
            }
            if (request.menu_open && !companion_menu_open_) {
                companion_asked_for_menu_ = true;
            }
            if (request.menu_open != companion_menu_open_) {
                companion_menu_open_ = request.menu_open;
                // What it needs has just changed, and a paused game will not
                // produce a new frame to carry the change across.
                resend_screens_ = true;
                // A menu takes the companion's hands off the game. Whatever it
                // was holding when the menu opened would otherwise stay held
                // for as long as the menu is up, since only changes are sent.
                if (companion_menu_open_) {
                    companion_input_ = DsInputState();
                    companion_input_fresh_ = true;
                    touch_unseen_ = false;
                    release_waiting_ = false;
                }
            }
            break;
        }
        case MessageType::Bye:
            drop_companion("the other device left");
            break;
        case MessageType::Ping:
            break;
        default:
            // A message we do not expect from a companion. Ignored rather than
            // fatal, so a newer companion sending extra chatter still works.
            break;
    }
}

// Input and FrameAck, the companion->host half of the media channel -- see
// kMediaPort. Unlike handle_message() above, a malformed datagram here is
// simply ignored rather than treated as a reason to drop the whole session:
// UDP has no stream to desynchronise, so one bad packet is just one bad
// packet, not evidence the connection itself is broken.
void HostSession::handle_media_datagram(MessageType type, const uint8_t* payload,
                                        std::size_t payload_size) {
    switch (type) {
        case MessageType::Input: {
            InputMessage input;
            if (!decode_input(payload, payload_size, input)) {
                return;
            }
            // A copy of one already taken, or one overtaken on the way by a
            // newer one: nothing new either way. Numbered inputs only, which is
            // every companion that sends them twice.
            if (input.has_sequence) {
                if (input_sequence_known_ &&
                    static_cast<int32_t>(input.sequence - input_sequence_) <= 0) {
                    return;
                }
                input_sequence_known_ = true;
                input_sequence_ = input.sequence;
            }
            DsInputState next = companion_input_;
            next.buttons = input.buttons;
            next.touching = input.touching;
            // Clamped rather than trusted: these arrive from another machine,
            // and to this console's own touch screen rather than a DS's.
            const ScreenSize touch = system_profile(config_.system).screens.bottom;
            next.touch_x = std::min(std::max<int>(input.touch_x, 0), touch.width - 1);
            next.touch_y = std::min(std::max<int>(input.touch_y, 0), touch.height - 1);
            next.circle_x = input.circle_x;
            next.circle_y = input.circle_y;
            next.c_stick_x = input.c_stick_x;
            next.c_stick_y = input.c_stick_y;
            // Passed through untouched. These are raw sensor readings in the
            // sender's own units (see DsMotionState); anything this end did
            // to them would be a second opinion about how the device is being
            // held, and the one place that question is answered is the far
            // side of the frame link, in Cemu's own sensor fusion.
            next.motion = input.motion;
            // A release the game has not yet seen the press of waits for one
            // frame to see it -- a quick tap can land press and release both
            // between two frames, and reaching the game as the release alone
            // is a tap that never happened. Anything else goes straight in.
            if (!next.touching && (touch_unseen_ || release_waiting_) &&
                (companion_input_.touching || release_waiting_)) {
                released_input_ = next;
                released_sequence_ = input.sequence;
                release_waiting_ = true;
            } else {
                if (next.touching && !companion_input_.touching) touch_unseen_ = true;
                companion_input_ = next;
                applied_sequence_ = input.sequence;
                release_waiting_ = false;
            }
            companion_input_fresh_ = true;
            // Its pads, each on its own, when it says what they are.
            companion_reports_pads_ = input.reports_pads;
            companion_pads_ = std::move(input.pads);
            break;
        }
        case MessageType::FrameAck: {
            uint64_t serial = 0;
            uint16_t link_delay_ms = kUnknownLinkDelay;
            if (decode_frame_ack(payload, payload_size, serial, link_delay_ms)) {
                acked_serial_ = std::max(acked_serial_, serial);
                // Everything up to the acked frame has arrived, so time the
                // newest of them and discard the rest.
                const uint64_t now = monotonic_millis();
                while (!frame_sent_at_.empty() && frame_sent_at_.front().first <= serial) {
                    const uint64_t sent_at = frame_sent_at_.front().second;
                    frame_sent_at_.pop_front();
                    if (frame_sent_at_.empty() || frame_sent_at_.front().first > serial) {
                        const uint64_t trip = now - sent_at;
                        ++stats_round_trips_;
                        stats_round_trip_total_ms_ += trip;
                        stats_round_trip_worst_ms_ = std::max(stats_round_trip_worst_ms_, trip);
                        // Taken up at once and given back slowly. The window
                        // below has to cover the trips this link actually
                        // takes, not the ones it averages: an average that
                        // ignores the tail is a window that shuts on every
                        // excursion into it, which is where the dropped
                        // frames were coming from.
                        round_trip_high_ms_ =
                            std::max(static_cast<double>(trip),
                                     round_trip_high_ms_ * kRoundTripDecayPerAck);
                        // The quickest trip, and how much of this one's
                        // excess over it the companion saw on the way out.
                        // The rest was on the way back.
                        const double sample = static_cast<double>(trip);
                        round_trip_floor_ms_ =
                            (round_trip_floor_ms_ <= 0.0 || sample < round_trip_floor_ms_)
                                ? sample
                                : round_trip_floor_ms_ + kRoundTripFloorRisePerAck;
                        double return_delay = 0.0;
                        if (link_delay_ms != kUnknownLinkDelay) {
                            return_delay = std::max(
                                0.0, sample - round_trip_floor_ms_ - link_delay_ms);
                        }
                        return_delay_high_ms_ = std::max(
                            return_delay, return_delay_high_ms_ * kRoundTripDecayPerAck);
                        stats_return_delay_worst_ms_ =
                            std::max(stats_return_delay_worst_ms_, return_delay);
                    }
                }
                // What is still waiting after that, if it has waited well past
                // when this ack would have covered it, was lost -- see
                // kLostAfterRoundTrips. Acted on at the next publish, the same
                // as a timeout, but as soon as the link is back rather than
                // however long after.
                const uint64_t lost_after = std::max<uint64_t>(
                    kMinLostAfterMs,
                    static_cast<uint64_t>(round_trip_high_ms_ * kLostAfterRoundTrips));
                if (!frame_sent_at_.empty() && now - frame_sent_at_.front().second >= lost_after) {
                    in_flight_frames_lost_ = true;
                }
                promote_tile_baseline(previous_top_, tile_snapshots_top_, serial);
                promote_tile_baseline(previous_bottom_, tile_snapshots_bottom_, serial);
            }
            break;
        }
        case MessageType::RepairRequest: {
            RepairRequestMessage request;
            if (decode_repair_request_payload(payload, payload_size, request)) {
                resend_pieces(request);
            }
            break;
        }
        case MessageType::KeyframeRequest: {
            KeyframeRequestMessage request;
            if (!decode_keyframe_request_payload(payload, payload_size, request)) {
                return;
            }
            // Only a flag is set here, not a keyframe forced directly: the
            // encoder belongs to publish_screens()' own thread of control,
            // and the next frame for this screen is the earliest anything
            // could be sent anyway. Repeats are free -- a request the
            // companion resends because the first was lost, or the answer to
            // it was, simply finds the flag already set.
            bool& asked =
                (request.screen == ScreenId::Top) ? keyframe_asked_top_ : keyframe_asked_bottom_;
            if (!asked) {
                ++stats_keyframe_requests_;
            }
            asked = true;
            break;
        }
        default:
            // Frame and Audio are host->companion only; anything else on this
            // channel is not this end's to act on.
            break;
    }
}

void HostSession::poll(uint64_t now_ms) {
    announcer_.poll(now_ms);
    // Before any ack below is matched against the pictures in flight.
    if (media_off_thread_) take_finished_tiles();
    if (has_companion()) {
        report_stream(now_ms);
    }

    // Drained every call, companion or not: a stray datagram (an old
    // session's, or anything else that finds this port) should not sit in
    // the kernel's receive buffer crowding out real traffic, and there is
    // nothing here that needs a TCP handshake to already be done -- a
    // datagram whose token does not match whatever session_token_ currently
    // is (zero, before any companion has connected, matches nothing) is
    // simply ignored.
    std::vector<uint8_t> datagram;
    std::string from_address;
    uint16_t from_port = 0;
    while (media_socket_.receive(datagram, from_address, from_port)) {
        uint64_t token = 0;
        MessageType type = MessageType::Ping;
        const uint8_t* payload = nullptr;
        std::size_t payload_size = 0;
        if (!decode_media_header(datagram.data(), datagram.size(), token, type, payload,
                                 payload_size)) {
            continue;
        }
        if (!handshake_done_ || token != session_token_) {
            continue;
        }
        // Learned here rather than announced anywhere: the companion's media
        // socket is on an ephemeral port, and this is simply whichever
        // address its most recent datagram actually came from.
        if (from_address != companion_media_address_ || from_port != companion_media_port_) {
            std::lock_guard<std::mutex> lock(media_mutex_);
            companion_media_address_ = from_address;
            companion_media_port_ = from_port;
        }
        companion_media_known_ = true;
        handle_media_datagram(type, payload, payload_size);
    }

    // Paced rather than dumped on the socket all at once by
    // send_frame_fragments() itself -- see drain_pending_media_sends()'s own
    // comment on why a big frame's fragments are spread across several of
    // these calls instead. Run every poll(), companion or not, the same as
    // the receive loop above: draining is what lets a fresh companion's
    // fragments (queued for the one that just left, then cleared by
    // drop_companion()) never pile up either.
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        drain_pending_media_sends(now_ms);
    }

    if (!companion_.open()) {
        TcpStream incoming;
        if (listener_.accept(incoming)) {
            companion_ = std::move(incoming);
            inbox_.clear();
            handshake_done_ = false;
            // Whatever the last companion was sent means nothing to this one.
            previous_top_.clear();
            previous_bottom_.clear();
            tile_snapshots_top_.clear();
            tile_snapshots_bottom_.clear();
            log_info("a device is connecting from " + companion_.peer_address() + ", to this machine at " + companion_.local_address());
            // Video and audio leave from the same address the companion
            // reached us on. A machine that is on one subnet twice -- wired
            // and wireless, an ordinary desktop -- otherwise lets the routing
            // table pick, which can put the media on the wireless leg while
            // the session itself is on the wire, and can even change mid
            // session. See UdpSocket::set_source_address().
            {
                std::lock_guard<std::mutex> lock(media_mutex_);
                std::lock_guard<std::mutex> sending(send_mutex_);
                media_socket_.set_source_address(companion_.local_address());
                audio_media_socket_.set_source_address(companion_.local_address());
            }
            refresh_beacon();
        }
    }

    if (!companion_.open()) {
        return;
    }

    if (session_state_dirty_ && handshake_done_) {
        SessionStateMessage state;
        state.layout_id = shared_.layout_id;
        state.companion_screen = companion_screen_;
        state.paused = shared_.paused;
        state.bilinear_filtering = shared_.bilinear_filtering;
        state.audio_target = shared_.audio_target;
        state.handedness = shared_.handedness;
        state.right_stick_mode = shared_.right_stick_mode;
        state.render_scale = static_cast<uint8_t>(shared_.render_scale);
        state.screen_gap = shared_.screen_gap;
        state.menu_open = shared_.menu_open;
        state.menu_focus = static_cast<uint16_t>(std::clamp(shared_.menu_focus, 0, 65535));
        state.menu_confirm = shared_.menu_confirm;
        state.menu_confirm_focus = shared_.menu_confirm_focus;
        state.menu_unavailable_notice = shared_.menu_unavailable_notice;
        state.graphic_packs_available = shared_.graphic_packs_available;
        state.amiibo_available = shared_.amiibo_available;
        state.producer_options = shared_.producer_options;
        state.amiibo = shared_.amiibo;
        state.amiibo_folder = shared_.amiibo_folder;
        state.max_render_scale = static_cast<uint8_t>(std::max(1, shared_.max_render_scale));
        state.cursor_choosable = shared_.cursor_choosable;
        state.right_stick_choosable = shared_.right_stick_choosable;
        state.left_stick_choosable = shared_.left_stick_choosable;
        state.left_stick_press = shared_.left_stick_press;
        state.rumble_choosable = shared_.rumble_choosable;
        state.rumble_pak = shared_.rumble_pak;
        state.lcd_filter_choosable = shared_.lcd_filter_choosable;
        state.lcd_filter = shared_.lcd_filter;
        state.cursor_shown = shared_.cursor_shown;
        state.settings_open = shared_.settings_open;
        state.save_points_available = shared_.save_points_available;
        state.save_sync_available = shared_.save_sync_available;
        state.resume_choosable = shared_.resume_choosable;
        state.resume_from_save_state = shared_.resume_from_save_state;
        state.resume_notice = shared_.resume_notice;
        state.left_handed_layouts = shared_.left_handed_layouts;
        // Not a setting anyone chose -- it is a property of the console being
        // played, so it comes from the profile rather than from shared_. What
        // it does at the other end is switch a gyroscope on, which is why it
        // is told rather than assumed: a companion has no way to know which
        // console it joined has any use for one.
        state.motion_wanted = system_profile(config_.system).uses_motion;
        std::vector<uint8_t> out;
        encode_session_state(state, out);
        companion_.send(out);
        session_state_dirty_ = false;
    }
    if (save_slots_dirty_ && handshake_done_) {
        std::vector<uint8_t> out;
        encode_save_slots(save_slots_, out);
        companion_.send(out);
        save_slots_dirty_ = false;
    }
    if (controllers_view_dirty_ && handshake_done_) {
        companion_.send(controllers_view_);
        controllers_view_dirty_ = false;
    }
    if (graphic_packs_view_dirty_ && handshake_done_) {
        companion_.send(graphic_packs_view_);
        graphic_packs_view_dirty_ = false;
    }

    std::string error;
    const bool pumped = companion_.pump(inbox_, &error);

    // Processed even when pump() itself failed: a companion's closing message
    // -- typically a Bye, sent right behind whatever it was last asking for --
    // reads as a clean EOF the moment the socket is drained, and pump() reports
    // that as an error. Bailing out here on that error, before ever looking at
    // what was actually read, would silently drop a request that arrived in
    // the same batch but had not been parsed yet.
    while (true) {
        MessageType type = MessageType::Ping;
        std::vector<uint8_t> payload;
        bool fatal = false;
        if (!take_message(inbox_, type, payload, fatal)) {
            if (fatal) {
                drop_companion("that device sent a malformed message");
            }
            break;
        }
        handle_message(type, payload);
        if (!companion_.open()) {
            break;
        }
    }

    // Only reported now, and only if nothing already handled above (a Bye,
    // most often) beat it to closing the connection.
    if (!pumped && companion_.open()) {
        drop_companion(error);
    }
}

void HostSession::disconnect_companion(const std::string& reason) {
    if (!companion_.open()) {
        return;
    }
    // Say goodbye before closing, so the other end knows this was deliberate.
    std::vector<uint8_t> bye;
    encode_bye(bye);
    companion_.send(bye);
    std::string error;
    std::vector<uint8_t> ignored;
    companion_.pump(ignored, &error);
    drop_companion(reason);
}

bool HostSession::take_request(RequestMessage& out_request) {
    if (!have_request_) {
        return false;
    }
    out_request = request_;
    request_ = RequestMessage();
    have_request_ = false;
    return true;
}

bool HostSession::take_menu_open_request() {
    const bool asked = companion_asked_for_menu_;
    companion_asked_for_menu_ = false;
    return asked;
}

bool HostSession::take_menu_press(MenuPress& out) {
    if (menu_presses_.empty()) {
        return false;
    }
    out = menu_presses_.front();
    menu_presses_.pop_front();
    return true;
}

bool HostSession::take_companion_quit_request() {
    if (!companion_wants_quit_) {
        return false;
    }
    companion_wants_quit_ = false;
    return true;
}

bool HostSession::take_video_error(std::string& out_reason) {
    if (!has_pending_video_error_) {
        return false;
    }
    out_reason = pending_video_error_;
    has_pending_video_error_ = false;
    return true;
}

// Where a slow stream is actually being lost is not something that can be
// worked out from the far end, or from a machine that is not on the same
// network. This says, once a second: how many frames the game produced, how
// many were worth sending, how many were dropped for the companion being
// behind, the rate on the wire, and how far behind the companion says it is.
// Where the encode time above actually goes, per frame, since the last report.
// Only the screen that is being streamed has a cost to report, so the two are
// summed rather than shown apart -- one of them is always zero.
std::string HostSession::encode_cost_summary() {
    // Gathered by the encoder thread, whose encoders these are; the caller
    // holds media_mutex_.
    const video::VideoEncoder::EncodeCost top = encode_cost_top_;
    const video::VideoEncoder::EncodeCost bottom = encode_cost_bottom_;
    encode_cost_top_ = video::VideoEncoder::EncodeCost();
    encode_cost_bottom_ = video::VideoEncoder::EncodeCost();
    const unsigned frames = top.frames + bottom.frames;
    if (frames == 0) {
        return std::string();
    }
    const auto per_frame = [frames](double total) {
        return std::to_string(static_cast<int>(total * 100.0 / frames) / 100.0).substr(0, 4);
    };
    return " (scale " + per_frame(top.scale_ms + bottom.scale_ms) + "ms, upload " +
           per_frame(top.upload_ms + bottom.upload_ms) + "ms, submit " +
           per_frame(top.submit_ms + bottom.submit_ms) + "ms)";
}

void HostSession::report_stream(uint64_t now_ms) {
    // Most of what is counted here is counted on the encoder thread too.
    std::lock_guard<std::mutex> lock(media_mutex_);
    if (stats_at_ms_ == 0) {
        stats_at_ms_ = now_ms;
        stats_bytes_ = media_bytes_sent_;
        return;
    }
    const uint64_t elapsed = now_ms - stats_at_ms_;
    if (elapsed < 1000) {
        return;
    }
    const uint64_t bytes = media_bytes_sent_ - stats_bytes_;
    // A resend of an unchanged picture is sent without being produced again.
    const unsigned dropped = stats_produced_ > stats_sent_ ? stats_produced_ - stats_sent_ : 0;
    const uint64_t behind = static_cast<uint64_t>(frame_sent_at_.size());
    log_debug("stream: " + std::to_string(stats_produced_ * 1000 / elapsed) + " produced/s, " +
              std::to_string(stats_sent_ * 1000 / elapsed) + " sent/s, " +
              std::to_string(dropped) + " dropped, " +
              std::to_string(bytes * 1000 / elapsed / 1024) + " KB/s against the " +
              std::to_string((video_bitrate_kbps_top_ + video_bitrate_kbps_bottom_) / 8) +
              " KB/s it was asked for, biggest frame " +
              std::to_string(stats_frame_bytes_worst_ / 1024) + "KB against a typical " +
              std::to_string(stats_frames_counted_ > 0
                                 ? stats_frame_bytes_total_ / stats_frames_counted_ / 1024
                                 : 0) +
              "KB, companion " +
              std::to_string(behind) + " of " +
              std::to_string(frames_in_flight_allowed(
                  static_cast<uint64_t>(round_trip_high_ms_ + 0.5), produce_period_ms_,
                  static_cast<uint64_t>(return_delay_high_ms_ + 0.5))) +
              " frames in flight, round trip " +
              std::to_string(stats_round_trips_ > 0
                                 ? stats_round_trip_total_ms_ / stats_round_trips_
                                 : 0) +
              "ms avg " + std::to_string(stats_round_trip_worst_ms_) + "ms worst, encode " +
              std::to_string(stats_encodes_ > 0 ? stats_encode_total_ms_ / stats_encodes_ : 0) +
              "ms avg" + encode_cost_summary() + ", pacing queue " +
              std::to_string(stats_pacing_worst_ms_) + "ms worst, " +
              std::to_string(stats_keyframe_requests_ - stats_keyframe_requests_reported_) +
              " keyframes asked for");
    // The same second at a level the console sees: the host's half of any
    // freeze a companion reports. Every freeze chased so far stopped at "and
    // what was the Deck doing?", because this was a debug line.
    const unsigned keyframes_asked = stats_keyframe_requests_ - stats_keyframe_requests_reported_;
    log_info("sending: " + std::to_string(stats_sent_ * 1000 / elapsed) + " pictures a second of " +
             std::to_string(stats_produced_ * 1000 / elapsed) + " made, " +
             std::to_string(bytes * 1000 / elapsed / 1024) + " KB/s" +
             ((video_bitrate_kbps_top_ + video_bitrate_kbps_bottom_) > 0
                  ? " of the " +
                        std::to_string((video_bitrate_kbps_top_ + video_bitrate_kbps_bottom_) / 8) +
                        " KB/s video is allowed"
                  : std::string()) +
             ", biggest picture " + std::to_string(stats_frame_bytes_worst_ / 1024) + "KB" +
             ", round trip " +
             std::to_string(stats_round_trips_ > 0 ? stats_round_trip_total_ms_ / stats_round_trips_
                                                   : 0) +
             "ms" +
             // How much of the worst trip this second was acks coming back
             // late rather than frames going out late -- which the window
             // lets through rather than holding frames back for.
             (stats_return_delay_worst_ms_ >= 10.0
                  ? " (up to " + std::to_string(static_cast<int>(stats_return_delay_worst_ms_ + 0.5)) +
                        "ms of it acks coming back late)"
                  : std::string()) +
             ", " + std::to_string(behind) + " pictures unconfirmed, queued up to " +
             std::to_string(stats_pacing_worst_ms_) + "ms before leaving" +
             (keyframes_asked > 0 ? ", " + std::to_string(keyframes_asked) +
                                        (keyframes_asked == 1 ? " keyframe" : " keyframes") +
                                        " asked for"
                                  : std::string()) +
             // Said beside the asking, so an encoder that is asked and does
             // not deliver -- which a hardware one did -- shows
             // as the two numbers disagreeing.
             (stats_keyframes_sent_ > 0 || keyframes_asked > 0
                  ? ", " + std::to_string(stats_keyframes_sent_) +
                        (stats_keyframes_sent_ == 1 ? " keyframe" : " keyframes") + " sent"
                  : std::string()) +
             (stats_repairs_asked_ > 0
                  ? ", " + std::to_string(stats_pieces_resent_) + " lost pieces sent again for " +
                        std::to_string(stats_repairs_asked_) +
                        (stats_repairs_asked_ == 1 ? " request" : " requests")
                  : std::string()));
    stats_pieces_resent_ = 0;
    stats_repairs_asked_ = 0;
    stats_keyframes_sent_ = 0;
    stats_at_ms_ = now_ms;
    stats_bytes_ = media_bytes_sent_;
    stats_produced_ = 0;
    stats_sent_ = 0;
    stats_round_trips_ = 0;
    stats_encode_total_ms_ = 0;
    stats_encodes_ = 0;
    stats_pacing_worst_ms_ = 0;
    stats_keyframe_requests_reported_ = stats_keyframe_requests_;
    stats_round_trip_total_ms_ = 0;
    stats_round_trip_worst_ms_ = 0;
    stats_return_delay_worst_ms_ = 0.0;
    stats_frame_bytes_worst_ = 0;
    stats_frame_bytes_total_ = 0;
    stats_frames_counted_ = 0;
}

void HostSession::publish_audio(AudioStreamId stream, const int16_t* interleaved_stereo,
                                std::size_t frames, uint32_t sample_rate) {
    if (!has_companion() || !companion_media_known_ || interleaved_stereo == nullptr ||
        frames == 0) {
        return;
    }
    // Shared keeps using the coarse flag app.cpp derives from audio_target,
    // for a DS or 3DS's single, player-chosen stream. TopDevice/BottomDevice
    // need no such check: the caller (see the External source's own audio
    // callback in app.cpp) only ever reaches this for a screen Cemu itself
    // decided is not playing locally, so it is always meant for whoever is
    // connected -- already checked above.
    if (stream != AudioStreamId::TopDevice && stream != AudioStreamId::BottomDevice &&
        !audio_enabled_) {
        return;
    }
    // No backlog check here any more -- see the comment where
    // kMaxQueuedAudioMs used to be. A send is either handed to the socket now
    // or not at all; there is nothing this end could still be behind on.

    // Split at the source rather than trusting one send to carry it all --
    // see kMaxAudioFramesPerPacket. Each slice is a complete, independent
    // AudioMessage: there is nothing linking one packet to the next, so a
    // lost one is just a lost one, the same as any other audio loss here.
    for (std::size_t offset = 0; offset < frames; offset += kMaxAudioFramesPerPacket) {
        const std::size_t slice = std::min(kMaxAudioFramesPerPacket, frames - offset);
        AudioMessage message;
        message.stream = stream;
        message.sample_rate = sample_rate;
        message.sequence = audio_sequence_[static_cast<uint8_t>(stream)]++;
        message.samples.assign(interleaved_stereo + offset * 2,
                               interleaved_stereo + (offset + slice) * 2);

        std::vector<uint8_t> out;
        encode_media_header(session_token_, MessageType::Audio, out);
        encode_audio_payload(message, out);
        if (media_off_thread_) {
            // Sent by the encoder thread, first thing -- see
            // set_media_off_thread().
            {
                std::lock_guard<std::mutex> lock(media_mutex_);
                audio_sends_.push_back(std::move(out));
            }
            video_jobs_cv_.notify_one();
            continue;
        }
        // audio_media_socket_, not media_socket_ -- see its own comment on
        // why audio needs a send buffer a video keyframe burst can't fill.
        if (audio_media_socket_.send_to(companion_media_address_, companion_media_port_,
                                        out.data(), out.size())) {
            media_bytes_sent_ += out.size();
        }
    }
}

void HostSession::publish_rumble(bool active) {
    if (!has_companion()) {
        return;
    }
    if (have_sent_rumble_ && active == rumble_active_) {
        return;
    }
    RumbleMessage message;
    message.active = active;
    std::vector<uint8_t> out;
    encode_rumble(message, out);
    companion_.send(out);
    rumble_active_ = active;
    have_sent_rumble_ = true;
}

void HostSession::publish_menu_sound(MenuSound sound) {
    if (!has_companion() || !handshake_done_) {
        return;
    }
    MenuSoundMessage message;
    message.sound = sound;
    std::vector<uint8_t> out;
    encode_menu_sound(message, out);
    companion_.send(out);
}

void HostSession::publish_pad_rumble(uint32_t pad, bool active) {
    if (!has_companion() || !companion_reports_pads_) {
        return;
    }
    const auto sent = pad_rumble_sent_.find(pad);
    if (sent != pad_rumble_sent_.end() && sent->second == active) {
        return;
    }
    RumbleMessage message;
    message.active = active;
    message.pad = pad;
    std::vector<uint8_t> out;
    encode_rumble(message, out);
    companion_.send(out);
    pad_rumble_sent_[pad] = active;
}

void HostSession::publish_pad_ping(uint32_t pad, uint16_t ms) {
    if (!has_companion() || !companion_reports_pads_ || ms == 0) {
        return;
    }
    RumbleMessage message;
    message.active = true;
    message.pad = pad;
    message.ping_ms = ms;
    std::vector<uint8_t> out;
    encode_rumble(message, out);
    companion_.send(out);
}

// Which frame the bytes an encoder just handed back actually describe.
//
// A pipelining encoder returns the packet for an earlier submission than the
// one that went in with it, so the caller cannot assume what comes out is what
// it just put in -- and sending a picture under the wrong serial would tell the
// companion it is newer than it is, which its pacing, its acks and its tile
// baselines are all built on being true.
//
// `pending` is every frame still unaccounted for, oldest first, as (pts,
// serial). Anything older than the packet that arrived never came out at all
// (an encoder reset drops what it was holding) and is discarded rather than
// waited for.
bool resolve_encoded_serial(std::deque<std::pair<int64_t, uint64_t>>& pending, int64_t pts,
                            uint64_t& out_serial) {
    while (!pending.empty() && pending.front().first < pts) {
        pending.pop_front();
    }
    if (pending.empty() || pending.front().first != pts) {
        return false;
    }
    out_serial = pending.front().second;
    pending.pop_front();
    return true;
}

bool HostSession::queue_video(ScreenId screen, const ScreenPixels& source, uint64_t serial,
                              std::string& reason) {
    if (!video::can_encode()) {
        reason = "this build has no video encoder compiled in";
        return false;
    }
    if (!companion_accepts_video_) {
        reason = "the connected device cannot decode video";
        return false;
    }
    // 4:2:0 cannot represent an odd number of pixels. Rather than refuse to
    // stream at all over what is at most a single edge row/column, crop it
    // away -- Cemu has its own resolution settings this app does not
    // second-guess (see make_wii_u() in system.cpp), and they are not always
    // going to land on an even number.
    const int width = source.width & ~1;
    const int height = source.height & ~1;
    if (width <= 0 || height <= 0) {
        reason = "this screen is " + std::to_string(source.width) + "x" +
                 std::to_string(source.height) + ", too small to encode";
        return false;
    }

    VideoJob job;
    job.screen = screen;
    job.serial = serial;
    job.width = width;
    job.height = height;
    job.frame_width = static_cast<uint16_t>(source.width);
    job.frame_height = static_cast<uint16_t>(source.height);
    job.channel_order = source.channel_order;
    // Never sent any larger than the companion can actually show -- see
    // compute_capped_encode_size()'s own comment. Unknown display size (still
    // false before the companion has ever reported one) leaves encode_width/
    // height equal to width/height, the same as always.
    int display_width = 0;
    int display_height = 0;
    companion_display_size(display_width, display_height);
    job.encode_width = width;
    job.encode_height = height;
    compute_capped_encode_size(width, height, display_width, display_height, job.encode_width,
                               job.encode_height);
    // What the source is really producing at, which is what the bitrate and
    // the keyframe interval both have to be built from. produce_period_ms_ is
    // a smoothed measurement, so it is not worth anything on the first frame
    // of a session -- which is exactly when the first encoder has to open.
    job.produced_fps = produce_period_ms_ > 0.0 ? 1000.0 / produce_period_ms_ : 60.0;
    // A video frame means nothing without the frames before it, so a companion
    // that has just started needing this screen gets one that stands alone --
    // and so does one that has told us it lost a frame and cannot decode past
    // it, and a screen that has just switched to video. The second is what
    // makes the encoder's own periodic keyframe interval affordable to
    // lengthen; see kKeyframeSeconds.
    bool& asked = (screen == ScreenId::Top) ? keyframe_asked_top_ : keyframe_asked_bottom_;
    bool& owed = (screen == ScreenId::Top) ? video_keyframe_owed_top_ : video_keyframe_owed_bottom_;
    job.force_keyframe = resend_screens_ || asked || owed;
    asked = false;
    owed = false;
    // Copied, because the source's pixels are the game's and will be written
    // over by its next frame while this one is still being encoded. About a
    // millisecond for a 720p picture -- which is what this thread now spends
    // on video, where it used to spend the whole encode.
    job.pixels.resize(static_cast<std::size_t>(width) * height);
    for (int row = 0; row < height; ++row) {
        std::memcpy(job.pixels.data() + static_cast<std::size_t>(row) * width,
                    source.pixels + static_cast<std::size_t>(row) * source.stride_pixels,
                    static_cast<std::size_t>(width) * sizeof(uint32_t));
    }

    std::lock_guard<std::mutex> lock(media_mutex_);
    job.generation = media_generation_;
    // Only the newest picture of a screen is worth encoding: one still
    // waiting when the next arrives has been overtaken, and the encoder never
    // having seen it breaks nothing. Its keyframe, if it owed one, is owed by
    // the picture replacing it.
    for (auto waiting = video_jobs_.begin(); waiting != video_jobs_.end();) {
        if (!waiting->close && waiting->screen == screen) {
            job.force_keyframe = job.force_keyframe || waiting->force_keyframe;
            waiting = video_jobs_.erase(waiting);
        } else {
            ++waiting;
        }
    }
    video_jobs_.push_back(std::move(job));
    video_jobs_cv_.notify_one();
    return true;
}

bool HostSession::encode_video_job(const VideoJob& job, FrameMessage& frame, std::string& reason,
                                   uint32_t& opened_kbps) {
    opened_kbps = 0;
    const ScreenId screen = job.screen;
    const int width = job.width;
    const int height = job.height;
    const int encode_width = job.encode_width;
    const int encode_height = job.encode_height;
    video::VideoEncoder& encoder = (screen == ScreenId::Top) ? video_top_ : video_bottom_;
    std::deque<std::pair<int64_t, uint64_t>>& pending =
        (screen == ScreenId::Top) ? encode_serials_top_ : encode_serials_bottom_;
    // Reopens on a resolution change too, not just the first time -- a render
    // scale or layout change mid-session, or the companion's own window being
    // resized, leaves one of these dimensions different from what the encoder
    // (and the scaler and frame pool inside it) were last configured for, and
    // feeding it pixels of a new size through the old configuration is
    // exactly what produced the partial/duplicated pictures a render scale
    // change used to cause.
    // A source whose size changed but whose encoded size did not is followed
    // without disturbing the stream: only the scaler in front of the encoder
    // is rebuilt. Cemu's GamePad scan buffer alternates between two sizes on
    // some titles, and reopening for that was restarting the H.264 stream
    // several times a second -- new parameter sets, a fresh keyframe, and a
    // decoder at the far end reconfiguring itself mid-play, which is what a
    // corrupt picture on a Switch actually was.
    if (encoder.is_open() && encoder.width() == encode_width &&
        encoder.height() == encode_height &&
        (encoder.source_width() != width || encoder.source_height() != height)) {
        std::string scaler_error;
        if (encoder.set_source_size(width, height, &scaler_error)) {
            log_info("the " + std::string(to_string(screen)) + " screen is now " +
                     std::to_string(width) + "x" + std::to_string(height) +
                     "; resampling it into the same stream");
        } else {
            // Falls through to the full reopen below, which is the older and
            // more heavy-handed way of following the same change.
            encoder.close();
            pending.clear();
        }
    }
    // What the source is really producing at -- see queue_video().
    const double produced_fps = job.produced_fps;
    const int wanted_bitrate_kbps =
        bitrate_kbps_for_encode_size(encode_width, encode_height, produced_fps);

    // No reopening to correct this number once the rate is known. It was
    // tried, and measured: reopening cost a keyframe each time (three in one
    // session, one of them for a 48fps reading taken while the game was busy)
    // and the bytes on the wire did not move. See media_pacing_bytes_per_ms()
    // for what the encoder actually did with the number it was given.

    // And on a change of byte order: a screen read back a different way --
    // off the GPU rather than out of the core's own memory, say, when its size
    // moves across what a readback does on the CPU -- would otherwise go on
    // being read as the old order, red and blue swapped across the picture.
    const video::PixelOrder pixel_order = (job.channel_order == PixelChannelOrder::Rgba)
                                              ? video::PixelOrder::Rgba
                                              : video::PixelOrder::Bgra;
    if (!encoder.is_open() || encoder.source_width() != width || encoder.source_height() != height ||
        encoder.width() != encode_width || encoder.height() != encode_height ||
        encoder.pixel_order() != pixel_order) {
        video::EncoderConfig config;
        config.width = encode_width;
        config.height = encode_height;
        config.source_width = width;
        config.source_height = height;
        // The real rate, so the keyframe interval is the ten seconds it is
        // meant to be rather than twenty at half the assumed rate.
        config.fps = std::clamp(static_cast<int>(produced_fps + 0.5), 20, 144);
        config.bitrate_kbps = wanted_bitrate_kbps;
        // A GPU-read-back screen (3DS, or Cemu across the external frame
        // link) hands its pixels over R,G,B,A rather than this app's usual
        // XRGB8888/BGRA -- see PixelOrder's own comment. Missing this swapped
        // red and blue across the whole picture.
        config.pixel_order = pixel_order;
        std::string open_error;
        if (!encoder.open(config, &open_error)) {
            reason = "no video encoder would open: " + open_error;
            return false;
        }
        // Tells drain_pending_media_sends() what rate to expect from this
        // screen from now on -- see its own comment. Handed back rather than
        // set here: the drain reads it under media_mutex_.
        opened_kbps = static_cast<uint32_t>(config.bitrate_kbps);
        // A reopened encoder counts from zero again, so anything still mapped
        // against the old one's numbering would resolve to the wrong moment.
        pending.clear();
    }
    // Decided on the game's thread when the picture was queued -- see
    // queue_video().
    if (job.force_keyframe) {
        encoder.request_keyframe();
    }

    video::EncodedFrame encoded;
    std::string encode_error;
    if (!encoder.encode(job.pixels.data(), job.width, encoded, &encode_error)) {
        encoder.close();
        pending.clear();
        reason = "the video encoder failed: " + encode_error;
        return false;
    }
    // What went in, remembered against the moment it belongs to. The encoder
    // pipelines, so the bytes that come back below are usually an earlier
    // submission's -- sending them under this frame's serial would tell the
    // companion a picture is newer than it is, and its own pacing, acking and
    // tile baselines are all built on that number being true.
    pending.emplace_back(encoder.last_submitted_pts(), job.serial);
    // A frame whose packet never arrives (an encoder reset losing what it was
    // holding) must not leave its entry here for ever.
    while (pending.size() > 8) {
        pending.pop_front();
    }
    if (encoded.empty()) {
        return false;  // still in the encoder -- reason stays empty, not a failure
    }
    uint64_t resolved = 0;
    if (!resolve_encoded_serial(pending, encoded.pts, resolved)) {
        return false;  // cannot say which moment this is, so it does not go
    }
    frame.screen = screen;
    frame.serial = resolved;
    frame.width = job.frame_width;
    frame.height = job.frame_height;
    frame.encoding = FrameEncoding::H264;
    frame.keyframe = encoded.keyframe;
    frame.payload = std::move(encoded.bytes);
    return true;
}

void HostSession::note_tile_bytes(ScreenId screen, std::size_t bytes) {
    // What tiles are costing this screen, over a second at a time.
    CodecState& codec = screen == ScreenId::Top ? codec_top_ : codec_bottom_;
    const uint64_t now = monotonic_millis();
    if (codec.tile_window_start_ms == 0) codec.tile_window_start_ms = now;
    codec.tile_bytes += bytes;
    if (now - codec.tile_window_start_ms >= 1000) {
        // Bytes per millisecond, which is kilobytes per second.
        codec.tile_kbytes_per_second = static_cast<double>(codec.tile_bytes) /
                                       static_cast<double>(now - codec.tile_window_start_ms);
        codec.tile_bytes = 0;
        codec.tile_window_start_ms = now;
    }
}

void HostSession::queue_tiles(ScreenId screen, const ScreenPixels& source, uint64_t serial,
                              bool keyframe, std::vector<uint8_t>& previous) {
    VideoJob job;
    job.tiles = true;
    job.screen = screen;
    job.serial = serial;
    job.width = source.width;
    job.height = source.height;
    job.frame_width = static_cast<uint16_t>(source.width);
    job.frame_height = static_cast<uint16_t>(source.height);
    job.channel_order = source.channel_order;
    job.keyframe = keyframe;
    job.baseline = std::move(previous);
    previous.clear();
    // Copies, not pointers: an ack arriving while the job is encoded moves
    // these along underneath it.
    if (!job.baseline.empty()) {
        for (const auto& snapshot :
             screen == ScreenId::Top ? tile_snapshots_top_ : tile_snapshots_bottom_) {
            job.also_unlike.push_back(snapshot.second);
        }
    }
    // Copied now, since the caller's pixels are the game's and change with
    // its next frame.
    job.pixels.resize(static_cast<std::size_t>(source.width) * source.height);
    for (int row = 0; row < source.height; ++row) {
        std::memcpy(job.pixels.data() + static_cast<std::size_t>(row) * source.width,
                    source.pixels + static_cast<std::size_t>(row) * source.stride_pixels,
                    static_cast<std::size_t>(source.width) * sizeof(uint32_t));
    }
    ++tile_jobs_outstanding_[screen == ScreenId::Top ? 0 : 1];
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        job.generation = media_generation_;
        video_jobs_.push_back(std::move(job));
    }
    video_jobs_cv_.notify_one();
}

void HostSession::encode_tiles_job(VideoJob& job, std::unique_lock<std::mutex>& lock) {
    FrameMessage frame;
    frame.screen = job.screen;
    frame.serial = job.serial;
    frame.width = job.frame_width;
    frame.height = job.frame_height;
    frame.encoding = FrameEncoding::Rgb565DeflateTiles;
    frame.keyframe = job.keyframe;
    std::vector<const std::vector<uint8_t>*> in_flight;
    for (const std::vector<uint8_t>& other : job.also_unlike) {
        in_flight.push_back(&other);
    }
    const uint64_t started_ms = monotonic_millis();
    const bool encoded =
        encode_screen_tiles(job.pixels.data(), job.width, job.height, job.width, job.baseline,
                            frame.payload, job.channel_order, in_flight);
    const uint64_t ended_ms = monotonic_millis();

    lock.lock();
    stats_encode_total_ms_ += ended_ms > started_ms ? ended_ms - started_ms : 0;
    ++stats_encodes_;
    FinishedTiles done;
    done.screen = job.screen;
    done.serial = job.serial;
    done.bytes = frame.payload.size();
    // For a companion that has gone, or a session since restarted: not to be
    // sent to whoever is there now, nor kept as in flight to them.
    if (encoded && job.generation == media_generation_) {
        done.sent = send_frame_fragments(frame) > 0;
        if (done.sent) done.snapshot = std::move(job.baseline);
    }
    finished_tiles_.push_back(std::move(done));
}

void HostSession::take_finished_tiles() {
    std::vector<FinishedTiles> finished;
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        finished.swap(finished_tiles_);
    }
    for (FinishedTiles& done : finished) {
        int& outstanding = tile_jobs_outstanding_[done.screen == ScreenId::Top ? 0 : 1];
        if (outstanding > 0) --outstanding;
        note_tile_bytes(done.screen, done.bytes);
        if (done.sent) {
            (done.screen == ScreenId::Top ? tile_snapshots_top_ : tile_snapshots_bottom_)
                .emplace_back(done.serial, std::move(done.snapshot));
        }
    }
}

HostSession::Encoded HostSession::encode_one_screen(ScreenId screen,
                                                    const ScreenPixels& source,
                                                    std::vector<uint8_t>& previous,
                                                    FrameMessage& frame) {
    CodecState& codec = screen == ScreenId::Top ? codec_top_ : codec_bottom_;
    {
        const bool video = sends_video(screen, source.width, source.height);
        if (video != codec.video) {
            const CodecEvidence evidence = codec_evidence(screen, source.width, source.height);
            const bool for_size =
                static_cast<long long>(source.width) * source.height >= kVideoFromPixels;
            switch_codec(screen, video,
                         codec_choice_ != CodecChoice::Automatic ? "as this run was told to"
                         : !video && !evidence.video_possible    ? "video cannot be had here"
                         : !video                                 ? "back to tiles to see what it costs"
                         : for_size ? "it is too big to go as tiles"
                                    : "it is too busy to go as tiles");
            // Only a choice made here, from what tiles measured: a forced one
            // is not evidence of anything, and holding it for weight would
            // keep video on for a while after being told to choose again.
            codec.video_for_weight =
                video && !for_size && codec_choice_ == CodecChoice::Automatic;
            if (!video) {
                previous.clear();
            }
        }
    }
    if (!codec.video) {
        frame.encoding = FrameEncoding::Rgb565DeflateTiles;
        bool& asked = (screen == ScreenId::Top) ? keyframe_asked_top_ : keyframe_asked_bottom_;
        if (asked) {
            // The tile equivalent of an encoder keyframe: with no baseline to
            // diff against, what goes out is the whole picture.
            previous.clear();
            asked = false;
        }
        // A tile frame stands on its own exactly when it was built without a
        // baseline, which is what an empty `previous` means here.
        frame.keyframe = previous.empty();
        if (media_off_thread_) {
            queue_tiles(screen, source, frame.serial, frame.keyframe, previous);
            return Encoded::Queued;
        }
        // Every frame sent since that baseline and not yet confirmed: the
        // companion may be showing any of their tiles. See also_unlike.
        std::vector<const std::vector<uint8_t>*> in_flight;
        if (!previous.empty()) {
            for (const auto& snapshot :
                 screen == ScreenId::Top ? tile_snapshots_top_ : tile_snapshots_bottom_) {
                in_flight.push_back(&snapshot.second);
            }
        }
        const bool encoded = encode_screen_tiles(source.pixels, source.width, source.height,
                                                 source.stride_pixels, previous, frame.payload,
                                                 source.channel_order, in_flight);
        note_tile_bytes(screen, frame.payload.size());
        return encoded ? Encoded::Ready : Encoded::Nothing;
    }

    // Pinned to video by --video-codec, nothing past here falls back to
    // tiles: a screen this cannot actually send as H.264 is a failure that run
    // asked to be told about, through take_video_error(), since substituting
    // the other format would defeat the point of pinning one. Chosen here
    // instead, a failure is only a reason to go back to tiles for a while.
    if (codec_choice_ == CodecChoice::H264 && monotonic_millis() < video_retry_at_ms_) {
        return Encoded::Nothing;  // still backed off from the last failure -- see below
    }

    std::string reason;
    if (queue_video(screen, source, frame.serial, reason)) {
        return Encoded::Queued;
    }
    const Encoded instead = video_failed(screen, reason);
    if (instead == Encoded::Ready) {
        // Gone back to tiles, and this picture goes as tiles right now.
        previous.clear();
        return encode_one_screen(screen, source, previous, frame);
    }
    return instead;
}

HostSession::Encoded HostSession::video_failed(ScreenId screen, const std::string& reason) {
    if (codec_choice_ == CodecChoice::Automatic) {
        video_retry_at_ms_ = monotonic_millis() + kVideoFallbackMs;
        log_warn("video would not work for the " + std::string(to_string(screen)) +
                 " screen (" + reason + "); sending it as tiles instead");
        switch_codec(screen, false, "video would not work");
        return Encoded::Ready;
    }

    // A genuine failure. Backed off so it is retried and, if it is still
    // broken, reported again in a few seconds rather than sixty times a
    // second -- but reported only once per failure, not once per retry.
    video_retry_at_ms_ = monotonic_millis() + kVideoRetryCooldownMs;
    if (!video_error_reported_) {
        video_error_reported_ = true;
        pending_video_error_ =
            std::string(to_string(screen)) + " screen: " + reason;
        has_pending_video_error_ = true;
        log_warn("video streaming is not working, and nothing is sent in its place: " +
                 pending_video_error_);
    }
    return Encoded::Nothing;
}

void HostSession::take_video_outcomes() {
    std::vector<VideoFailure> failures;
    bool worked = false;
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        failures.swap(video_failures_);
        worked = video_worked_;
        video_worked_ = false;
    }
    if (worked) {
        video_error_reported_ = false;  // it is working, or working again
    }
    for (const VideoFailure& failure : failures) {
        // Found out on the encoder thread, a picture later than it would have
        // been here: the next picture goes as tiles instead, where that is
        // the answer. See video_failed().
        video_failed(failure.screen, failure.reason);
        if (codec_choice_ == CodecChoice::Automatic) {
            (failure.screen == ScreenId::Top ? previous_top_ : previous_bottom_).clear();
        }
    }
}

HostSession::~HostSession() { stop_encoder_thread(); }

void HostSession::start_encoder_thread() {
    stop_encoder_thread();
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        encoder_stopping_ = false;
        video_jobs_.clear();
        finished_tiles_.clear();
        audio_sends_.clear();
    }
    tile_jobs_outstanding_[0] = 0;
    tile_jobs_outstanding_[1] = 0;
    encoder_thread_ = std::thread([this] { encoder_loop(); });
}

void HostSession::stop_encoder_thread() {
    if (!encoder_thread_.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(media_mutex_);
        encoder_stopping_ = true;
        video_jobs_.clear();
        finished_tiles_.clear();
        audio_sends_.clear();
    }
    video_jobs_cv_.notify_all();
    encoder_thread_.join();
    tile_jobs_outstanding_[0] = 0;
    tile_jobs_outstanding_[1] = 0;
    // Nothing else touches them now.
    video_top_.close();
    video_bottom_.close();
    encode_serials_top_.clear();
    encode_serials_bottom_.clear();
}

void HostSession::encoder_loop() {
    std::unique_lock<std::mutex> lock(media_mutex_);
    while (true) {
        if (media_off_thread_ && !encoder_stopping_) {
            send_media_off_thread(lock);
        }
        const auto work = [this] {
            return encoder_stopping_ || !video_jobs_.empty() ||
                   (media_off_thread_ && !audio_sends_.empty());
        };
        if (media_off_thread_ && !pending_media_sends_.empty()) {
            // Pictures still being paced out: back for more of them shortly
            // even if nothing new arrives -- the game's thread's poll() did
            // this when it did the sending.
            video_jobs_cv_.wait_for(lock, std::chrono::milliseconds(1), work);
        } else {
            video_jobs_cv_.wait(lock, work);
        }
        if (encoder_stopping_) {
            return;
        }
        if (video_jobs_.empty()) {
            continue;
        }
        VideoJob job = std::move(video_jobs_.front());
        video_jobs_.pop_front();
        lock.unlock();

        if (job.tiles) {
            encode_tiles_job(job, lock);
            continue;
        }

        video::VideoEncoder& encoder = (job.screen == ScreenId::Top) ? video_top_ : video_bottom_;
        if (job.close) {
            encoder.close();
            ((job.screen == ScreenId::Top) ? encode_serials_top_ : encode_serials_bottom_).clear();
            lock.lock();
            continue;
        }
        FrameMessage frame;
        std::string reason;
        uint32_t opened_kbps = 0;
        const uint64_t started_ms = monotonic_millis();
        const bool encoded = encode_video_job(job, frame, reason, opened_kbps);
        const uint64_t ended_ms = monotonic_millis();
        const video::VideoEncoder::EncodeCost cost = encoder.take_cost();

        lock.lock();
        video::VideoEncoder::EncodeCost& total =
            (job.screen == ScreenId::Top) ? encode_cost_top_ : encode_cost_bottom_;
        total.scale_ms += cost.scale_ms;
        total.upload_ms += cost.upload_ms;
        total.submit_ms += cost.submit_ms;
        total.frames += cost.frames;
        stats_encode_total_ms_ += ended_ms > started_ms ? ended_ms - started_ms : 0;
        ++stats_encodes_;
        if (opened_kbps != 0) {
            ((job.screen == ScreenId::Top) ? video_bitrate_kbps_top_ : video_bitrate_kbps_bottom_) =
                opened_kbps;
        }
        // For a companion that has gone, or a session since restarted: not
        // to be sent to whoever is there now.
        if (job.generation != media_generation_) {
            continue;
        }
        if (encoded) {
            video_worked_ = true;
            // Out the moment it exists, rather than at the game's next pass.
            if (send_frame_fragments(frame) > 0) {
                drain_pending_media_sends(monotonic_millis());
            }
        } else if (!reason.empty()) {
            video_failures_.push_back(VideoFailure{job.screen, reason});
        }
    }
}

void HostSession::resend_pieces(const RepairRequestMessage& request) {
    // The pieces kept, and the queue they go back on, are shared with the
    // encoder thread -- see media_mutex_.
    std::lock_guard<std::mutex> lock(media_mutex_);
    ++stats_repairs_asked_;
    for (const SentVideoFrame& sent : sent_video_frames_) {
        if (sent.screen != request.screen) {
            continue;
        }
        // By the sending, not the serial: a picture sent again after a stall
        // goes out under the same serial, encoded afresh, and pieces of the
        // two mixed would be a frame that never existed.
        if (sent.sent_index != request.sent_index) {
            continue;
        }
        if (request.whole_frame) {
            for (auto piece = sent.pieces.rbegin(); piece != sent.pieces.rend(); ++piece) {
                pending_media_sends_.push_front(*piece);
                ++stats_pieces_resent_;
            }
            return;
        }
        // At the front, in the order asked for: the companion is holding
        // this frame, and every one behind it, until these land -- anything
        // already queued can wait the few microseconds these take.
        for (auto piece = request.pieces.rbegin(); piece != request.pieces.rend(); ++piece) {
            if (*piece < sent.pieces.size()) {
                pending_media_sends_.push_front(sent.pieces[*piece]);
                ++stats_pieces_resent_;
            }
        }
        return;
    }
    // Too old to still be held, or never sent as video: the companion gives
    // up on it and asks for a keyframe, which is what would happen anyway.
}

std::size_t HostSession::send_frame_fragments(const FrameMessage& frame) {
    if (frame.payload.empty()) {
        // Not itself a failure -- see encode_one_screen()'s own comment on an
        // empty payload meaning nothing changed -- but there is nothing here
        // to fragment or send, so this is done.
        return 0;
    }
    FrameFragmentHeader header;
    header.serial = frame.serial;
    header.screen = frame.screen;
    header.encoding = frame.encoding;
    header.width = frame.width;
    header.height = frame.height;
    header.keyframe = frame.keyframe;
    if (frame.keyframe && frame.encoding == FrameEncoding::H264) {
        ++stats_keyframes_sent_;
    }
    // Filled in here only so the field exists; the number that matters is
    // written again as each datagram actually leaves, in
    // drain_pending_media_sends(). Everything between the two is this
    // machine's own pacing, and the far end must not read that as the link --
    // see stamp_media_datagram_sent_at().
    header.sent_at_ms = static_cast<uint32_t>(monotonic_millis());
    // Counted per screen and only here, where a frame really does go out, so
    // that a gap in it at the far end means a frame was lost on the way and
    // never means this host chose to skip one. See FrameFragmentHeader's own
    // comment, and AudioMessage::sequence, which is the same idea for sound.
    uint16_t& sent_index =
        (frame.screen == ScreenId::Top) ? sent_index_top_ : sent_index_bottom_;
    header.sent_index = sent_index++;
    header.fragment_count = static_cast<uint16_t>(
        (frame.payload.size() + kMaxFragmentPayload - 1) / kMaxFragmentPayload);

    std::size_t queued_bytes = 0;
    std::size_t offset = 0;
    // Kept, for a video frame, in case a piece has to go again -- see
    // resend_pieces(). Thirty frames is half a second at 60 a second, longer
    // than any repair is waited for at the far end -- a keyframe's included,
    // which is waited for longest (kKeyframeRepairGiveUpMs).
    constexpr std::size_t kSentVideoFramesKept = 30;
    SentVideoFrame* kept = nullptr;
    if (frame.encoding == FrameEncoding::H264) {
        while (sent_video_frames_.size() >= kSentVideoFramesKept) {
            sent_video_frames_.pop_front();
        }
        sent_video_frames_.push_back(
            SentVideoFrame{frame.screen, frame.serial, header.sent_index, {}});
        kept = &sent_video_frames_.back();
        kept->pieces.reserve(header.fragment_count);
    }
    for (uint16_t index = 0; index < header.fragment_count; ++index) {
        header.fragment_index = index;
        const std::size_t chunk =
            std::min(kMaxFragmentPayload, frame.payload.size() - offset);
        std::vector<uint8_t> out;
        encode_media_header(session_token_, MessageType::Frame, out);
        encode_frame_fragment(header, frame.payload.data() + offset, chunk, out);
        queued_bytes += out.size();
        media_produced_window_bytes_ += out.size();
        if (kept != nullptr) {
            kept->pieces.push_back(out);
        }
        pending_media_sends_.push_back(std::move(out));
        offset += chunk;
    }
    // The recovery pieces, straight after -- see build_frame_parity(). Video
    // only: a lost piece of a video frame costs a keyframe and a frozen
    // picture while it is asked for, where a lost tile frame is simply
    // covered by the next one, which is built against what was confirmed.
    if (frame.encoding == FrameEncoding::H264) {
        const std::vector<std::vector<uint8_t>> parity =
            build_frame_parity(frame.payload, kMaxFragmentPayload);
        for (std::size_t group = 0; group < parity.size(); ++group) {
            header.fragment_index = static_cast<uint16_t>(
                frame_parity_first_index(header.fragment_count) + group);
            std::vector<uint8_t> out;
            encode_media_header(session_token_, MessageType::Frame, out);
            encode_frame_fragment(header, parity[group].data(), parity[group].size(), out);
            queued_bytes += out.size();
            media_produced_window_bytes_ += out.size();
            pending_media_sends_.push_back(std::move(out));
        }
    }
    // How even the frames are, which decides whether the link sees a steady
    // trickle or a run of lumps -- see the report for why that matters.
    stats_frame_bytes_worst_ = std::max(stats_frame_bytes_worst_, queued_bytes);
    stats_frame_bytes_total_ += queued_bytes;
    ++stats_frames_counted_;
    return queued_bytes;
}

// Folds the bytes handed to the pacer since the last window into a smoothed
// rate. Windowed rather than per-frame: a single frame's size says more about
// what was on screen than about what the encoder is doing.
void HostSession::note_produced_media_rate(uint64_t now_ms) {
    if (media_produced_window_at_ms_ == 0) {
        media_produced_window_at_ms_ = now_ms;
        return;
    }
    const uint64_t window_ms = now_ms - media_produced_window_at_ms_;
    if (window_ms < kProducedRateWindowMs) {
        return;
    }
    const double rate =
        static_cast<double>(media_produced_window_bytes_) / static_cast<double>(window_ms);
    media_produced_bytes_per_ms_ = (media_produced_bytes_per_ms_ <= 0.0)
                                       ? rate
                                       : media_produced_bytes_per_ms_ * 0.5 + rate * 0.5;
    media_produced_window_bytes_ = 0;
    media_produced_window_at_ms_ = now_ms;
}

void HostSession::drain_pending_media_sends(uint64_t now_ms) {
    if (media_off_thread_) {
        // The encoder thread's to send -- see send_media_off_thread().
        if (!pending_media_sends_.empty()) video_jobs_cv_.notify_one();
        return;
    }
    const std::size_t budget = media_drain_budget(now_ms);
    std::size_t drained = 0;
    while (!pending_media_sends_.empty() && drained < budget) {
        std::vector<uint8_t>& out = pending_media_sends_.front();
        // Now, not when it was built. The wait in this queue is this end's,
        // and the far end measures what comes after -- see
        // stamp_media_datagram_sent_at().
        stamp_media_datagram_sent_at(out.data(), out.size(), static_cast<uint32_t>(now_ms));
        if (!media_socket_.send_to(companion_media_address_, companion_media_port_, out.data(),
                                   out.size())) {
            // Try again next poll() rather than losing this fragment outright
            // -- a transient send failure (e.g. a momentarily full OS send
            // buffer) is exactly what pacing is meant to give room to
            // recover from, not a reason to give up on the frame.
            break;
        }
        media_bytes_sent_ += out.size();
        drained += out.size();
        pending_media_sends_.pop_front();
    }
}

void HostSession::send_media_off_thread(std::unique_lock<std::mutex>& lock) {
    std::deque<std::vector<uint8_t>> sound;
    sound.swap(audio_sends_);
    const uint64_t now_ms = monotonic_millis();
    const std::size_t budget = media_drain_budget(now_ms);
    std::vector<std::vector<uint8_t>> pictures;
    std::size_t taken = 0;
    while (!pending_media_sends_.empty() && taken < budget) {
        taken += pending_media_sends_.front().size();
        pictures.push_back(std::move(pending_media_sends_.front()));
        pending_media_sends_.pop_front();
    }
    if (sound.empty() && pictures.empty()) {
        return;
    }
    const std::string address = companion_media_address_;
    const uint16_t port = companion_media_port_;
    const uint64_t generation = media_generation_;

    // Let go while sending, so the game's thread never waits on a send --
    // which is the whole point of sending here.
    lock.unlock();
    std::size_t bytes = 0;
    std::size_t pictures_sent = 0;
    {
        std::lock_guard<std::mutex> sending(send_mutex_);
        for (const std::vector<uint8_t>& out : sound) {
            if (audio_media_socket_.send_to(address, port, out.data(), out.size())) {
                bytes += out.size();
            }
        }
        for (std::vector<uint8_t>& out : pictures) {
            stamp_media_datagram_sent_at(out.data(), out.size(),
                                         static_cast<uint32_t>(monotonic_millis()));
            if (!media_socket_.send_to(address, port, out.data(), out.size())) {
                break;  // tried again next time, as drain_pending_media_sends() does
            }
            bytes += out.size();
            ++pictures_sent;
        }
    }
    lock.lock();
    media_bytes_sent_ += bytes;
    if (generation == media_generation_) {
        for (std::size_t index = pictures.size(); index > pictures_sent; --index) {
            pending_media_sends_.push_front(std::move(pictures[index - 1]));
        }
    }
}

std::size_t HostSession::media_drain_budget(uint64_t now_ms) {
    if (pending_media_sends_.empty()) {
        // Nothing queued -- start the pacing clock from here rather than
        // whatever time has passed since it last had something to send,
        // which would otherwise hand the next real frame a huge one-off
        // budget built from an idle gap that was never actually a backlog.
        media_pacing_last_ms_ = now_ms;
        return 0;
    }
    // The rate this drains at follows what is really being produced, not a flat
    // guess and not what the encoder was asked to produce -- see
    // media_pacing_bytes_per_ms() for why those last two are different things.
    // Sized too low, a stream arrives faster than it can ever drain and the
    // backlog this exists to avoid simply builds here instead of in the
    // socket's send buffer, still growing for as long as the stream runs.
    note_produced_media_rate(now_ms);
    const double bytes_per_ms = media_pacing_bytes_per_ms(
        video_bitrate_kbps_top_ + video_bitrate_kbps_bottom_, media_produced_bytes_per_ms_);
    const uint64_t elapsed_ms = (media_pacing_last_ms_ == 0 || now_ms <= media_pacing_last_ms_)
                                    ? 0
                                    : std::min(now_ms - media_pacing_last_ms_, kMaxPacingElapsedMs);
    const std::size_t budget = std::max<std::size_t>(
        kMinDrainBudgetBytes, static_cast<std::size_t>(static_cast<double>(elapsed_ms) * bytes_per_ms));
    media_pacing_last_ms_ = now_ms;

    // What is still queued, expressed as the time it will take to go out at
    // the rate above. This is latency the pacing itself is adding, and the
    // only place it can be seen -- from either end it just looks like a frame
    // that arrived late.
    if (bytes_per_ms > 0.0) {
        std::size_t queued_bytes = 0;
        for (const std::vector<uint8_t>& pending : pending_media_sends_) {
            queued_bytes += pending.size();
        }
        const auto queued_ms = static_cast<uint64_t>(queued_bytes / bytes_per_ms);
        stats_pacing_worst_ms_ = std::max(stats_pacing_worst_ms_, queued_ms);
    }
    return budget;
}

void HostSession::note_frame_ran(uint64_t serial) {
    // Which input this picture is the first to carry, if it is a new one.
    // Before the waiting release below is let go: this frame ran with the
    // touch, not with its release.
    if (input_sequence_known_ && companion_media_known_ &&
        (!echoed_any_ || applied_sequence_ != echoed_sequence_)) {
        echoed_any_ = true;
        echoed_sequence_ = applied_sequence_;
        InputEchoMessage echo;
        echo.sequence = applied_sequence_;
        echo.serial = serial;
        std::vector<uint8_t> out;
        encode_media_header(session_token_, MessageType::InputEcho, out);
        encode_input_echo_payload(echo, out);
        media_socket_.send_to(companion_media_address_, companion_media_port_, out.data(),
                              out.size());
    }
    touch_unseen_ = false;
    if (release_waiting_) {
        companion_input_ = released_input_;
        applied_sequence_ = released_sequence_;
        release_waiting_ = false;
        companion_input_fresh_ = true;
    }
}

bool HostSession::give_up_on_stalled_frames() {
    const bool told = in_flight_frames_lost_;
    in_flight_frames_lost_ = false;
    const bool stalled = !frame_sent_at_.empty() &&
                         (told || monotonic_millis() - frame_sent_at_.front().second >=
                                      ack_stall_ms(round_trip_high_ms_, acked_serial_ > 0));
    if (!stalled) {
        return false;
    }
    // Recover by treating every outstanding frame as gone rather than
    // merely late: open the gate as if they had all been acked, but
    // without promoting the tile baselines those unconfirmed sends were
    // computed against -- doing that would risk exactly the picture
    // drift ack-based rebaselining exists to prevent, on baselines that
    // were never actually confirmed. Clearing them outright and forcing
    // a fresh full send instead is the tile equivalent of the keyframe
    // resend_screens_ already forces for video.
    const std::string waited =
        std::to_string(monotonic_millis() - frame_sent_at_.front().second) + "ms";
    const std::string frames = std::to_string(frame_sent_at_.size()) + " frames in flight";
    log_info(told ? "the companion is answering but never got the last " + frames + " (" +
                        waited + " old); starting again from a keyframe"
                  : "no ack for " + waited + " (" + frames +
                        "); taking them for lost and starting again from a keyframe");
    acked_serial_ = sent_serial_;
    frame_sent_at_.clear();
    previous_top_.clear();
    previous_bottom_.clear();
    tile_snapshots_top_.clear();
    tile_snapshots_bottom_.clear();
    resend_screens_ = true;
    return true;
}

void HostSession::publish_screens(const ScreenPixels& top,
                                  const ScreenPixels& bottom,
                                  uint64_t serial) {
    if (!has_companion() || !companion_media_known_) {
        return;
    }
    // Whatever the encoder thread found out since the last picture -- video
    // that would not work, or that works again -- before deciding how this
    // one goes.
    take_video_outcomes();
    // And what the encoder thread made of the last tiles it was given: see
    // set_media_off_thread(). A screen whose last picture is still being
    // encoded waits for it -- its tiles are measured against that one too --
    // and is offered again on the next pass, by then with whatever is newest.
    if (media_off_thread_) {
        take_finished_tiles();
        for (const ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
            if (tile_jobs_outstanding_[screen == ScreenId::Top ? 0 : 1] > 0 &&
                companion_wants(screen)) {
                return;
            }
        }
    }
    // Normally an unchanged frame costs nothing: the serial only moves when the
    // pixels do. But when the companion starts needing a screen it was not being
    // sent -- it opened its menu, whose previews show both -- the pixels may
    // never change again while it waits, since a menu pauses the game. So a
    // change in what it needs forces one send of whatever is current.
    //
    // Unless what was last sent never arrived. A game paused by the menu
    // produces nothing new, so the gate below -- the only other place a lost
    // frame is noticed -- is never reached, and a picture whose last pieces
    // were lost on the way stayed half-drawn on the companion for as long as
    // the menu was up. Measured: "no ack for 150579ms" on closing a menu that
    // had been open two and a half minutes.
    //
    // Nor while the companion is waiting on a picture that stands on its own.
    // Asked for, the answer went out with the next picture that changed --
    // and a still one (a paused game, a menu, a scene with nothing moving)
    // never changes, so the companion held a stale or broken picture until
    // something on screen happened to move.
    const bool keyframe_owed = (keyframe_asked_top_ && companion_wants(ScreenId::Top)) ||
                               (keyframe_asked_bottom_ && companion_wants(ScreenId::Bottom));
    if (serial == submitted_serial_ && !resend_screens_ && !keyframe_owed &&
        !give_up_on_stalled_frames()) {
        return;
    }
    // How fast frames are really arriving from the source, which with the
    // round trip above is the whole of the window's arithmetic. Measured
    // rather than taken from the console's nominal rate: a game that is not
    // holding its frame rate needs a smaller window, not the one 60fps would
    // have called for.
    //
    // Once per frame, not once per call: a frame the window below held back
    // is offered again on every pass until it goes or is replaced, and
    // counting each offer read a 60fps game as producing 67 to 80 frames a
    // second -- inflating "dropped" by the difference, and shrinking the
    // measured period, which widens the very window that was full.
    if (serial != last_produced_serial_) {
        last_produced_serial_ = serial;
        ++stats_produced_;
        const uint64_t now = monotonic_millis();
        if (last_produced_at_ms_ != 0 && now > last_produced_at_ms_) {
            const double gap = static_cast<double>(now - last_produced_at_ms_);
            if (gap >= 4.0 && gap <= 100.0) {
                produce_period_ms_ = produce_period_ms_ * 0.95 + gap * 0.05;
            }
        }
        last_produced_at_ms_ = now;
    }
    // Never let this end run further ahead than the companion can plausibly
    // still be watching: it is trusted only once it has acked at least one
    // frame, so a companion that never acks falls behind rather than being
    // starved of frames outright. UDP has no send queue of its own to also
    // bound the way a TCP-based backstop here once did -- see
    // acked_serial_'s own comment.
    // Frames actually in flight, not the distance between two serial numbers.
    // Those are the same thing only when every frame the source produces gets
    // sent. Serials advance at the source's rate whatever this end does with
    // them, so a 60fps game whose frames are being sent every third one sits
    // three serials apart after a single send -- and this gate, written in
    // frames and applied to serials, then refused to send with less than one
    // frame outstanding. Measured on Bayonetta 2: 60 produced a second, 20
    // sent, and a reported "3 frames behind" that was one frame and three
    // serials. Wind Waker sends every frame it produces, so the two counts
    // matched and the gate behaved as documented -- which is why this only
    // ever bit the games that produce faster than the link carries.
    // frame_sent_at_ holds one entry per frame sent and is pruned on every
    // ack, so its size is the count this was always meant to be about.
    // Not "only once an ack has arrived", which is what this used to say. A
    // companion slow enough to take a second to produce its first ack was
    // handed everything sent in that second: measured on a Switch at 51, 110,
    // 170, 230 and then 240 frames outstanding before the first ack landed,
    // whereupon the round trip read 1063ms and the picture was four seconds
    // behind the game. The window is the window from the first frame; the
    // stall recovery below is what stops a companion that never acks at all
    // from being starved, and it does that job without this having to leave
    // the gate open indefinitely to begin with.
    if (frame_sent_at_.size() >= frames_in_flight_allowed(
                                     static_cast<uint64_t>(round_trip_high_ms_ + 0.5),
                                     produce_period_ms_,
                                     static_cast<uint64_t>(return_delay_high_ms_ + 0.5))) {
        // Still worth waiting for, unless the oldest of these has been
        // outstanding long enough that the ack which would release it is
        // never coming -- see kAckStallMs's own comment for why that is a
        // real, reproduced failure and not just a hypothetical one.
        if (!give_up_on_stalled_frames()) {
            return;
        }
    }

    bool sent_anything = false;
    // The newest serial whose bytes actually left, which is what the ack gate
    // and the round-trip bookkeeping below are about -- not the newest one
    // handed to the encoder.
    uint64_t emitted_serial = 0;
    const struct {
        ScreenId screen;
        const ScreenPixels& pixels;
        std::vector<uint8_t>& acked_baseline;
        std::deque<std::pair<uint64_t, std::vector<uint8_t>>>& snapshots;
    } screens[] = {{ScreenId::Top, top, previous_top_, tile_snapshots_top_},
                   {ScreenId::Bottom, bottom, previous_bottom_, tile_snapshots_bottom_}};

    for (const auto& entry : screens) {
        const ScreenPixels& source = entry.pixels;
        if (source.pixels == nullptr || source.width <= 0 || source.height <= 0 ||
            !companion_wants(entry.screen)) {
            continue;
        }
        FrameMessage frame;
        frame.screen = entry.screen;
        frame.serial = serial;
        frame.width = static_cast<uint16_t>(source.width);
        frame.height = static_cast<uint16_t>(source.height);
        // A tile diff is computed from a copy of the confirmed baseline, not
        // the baseline itself -- see previous_top_/bottom_'s own comment.
        // encode_one_screen() (its tiles branch, by way of
        // encode_screen_tiles()) mutates whatever it is given into the new
        // frame's full state, which is exactly what gets kept as this send's
        // own snapshot below; a video frame leaves the copy untouched, so
        // there is nothing to keep for it.
        std::vector<uint8_t> attempt = entry.acked_baseline;
        const uint64_t encode_started_ms = monotonic_millis();
        const Encoded encoded = encode_one_screen(entry.screen, source, attempt, frame);
        const uint64_t encode_ended_ms = monotonic_millis();
        if (encoded == Encoded::Nothing) {
            continue;
        }
        if (encoded == Encoded::Queued) {
            // On its way from the encoder thread, which sends it itself. It
            // counts as sent from now for the ack gate: the ack that clears it
            // is for this serial, and the encode is part of the round trip.
            sent_anything = true;
            emitted_serial = std::max(emitted_serial, serial);
            continue;
        }
        std::unique_lock<std::mutex> media_lock(media_mutex_);
        stats_encode_total_ms_ +=
            encode_ended_ms > encode_started_ms ? encode_ended_ms - encode_started_ms : 0;
        ++stats_encodes_;
        const std::size_t queued_bytes = send_frame_fragments(frame);
        media_lock.unlock();
        if (queued_bytes > 0) {
            // media_bytes_sent_ is credited by drain_pending_media_sends() at
            // actual-send time, not here at queue time -- see its own
            // comment on why sending is paced rather than immediate.
            sent_anything = true;
            // frame.serial rather than serial: video comes out of the encoder
            // a frame behind what went in (see try_encode_video), so what was
            // actually just sent is not always the moment being published.
            emitted_serial = std::max(emitted_serial, frame.serial);
            if (frame.encoding == FrameEncoding::Rgb565DeflateTiles) {
                entry.snapshots.emplace_back(frame.serial, std::move(attempt));
            }
        }
    }

    if (sent_anything) {
        frame_sent_at_.emplace_back(emitted_serial, monotonic_millis());
        // A companion that stops acking must not make this grow for ever.
        while (frame_sent_at_.size() > 240) {
            frame_sent_at_.pop_front();
        }
        ++stats_sent_;
        sent_serial_ = emitted_serial;
    }
    // Always, even when the encoder held this frame back: it says what has
    // been offered, which is what decides whether the next call has anything
    // new to do at all. sent_serial_ above says what has actually gone out,
    // which is a different question and the one the ack gate asks.
    submitted_serial_ = serial;
    resend_screens_ = false;

    // Out now, not at the next poll(). This is called after the game has run,
    // and the next poll() is at the top of the next pass of the app's loop --
    // after it has drawn and waited out its own display's refresh. Every frame
    // used to sit here that whole refresh, about 17ms at 60Hz, before a byte
    // of it left: latency on every picture, for nothing, since the pacer's
    // budget only exists to spread a keyframe out and an ordinary frame fits
    // in it anyway. What a keyframe does not fit in is still spread across
    // the polls that follow, exactly as before.
    if (sent_anything) {
        std::lock_guard<std::mutex> lock(media_mutex_);
        drain_pending_media_sends(monotonic_millis());
    }
}

}  // namespace ds::net
