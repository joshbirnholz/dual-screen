#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core/screen_source.hpp"
#include "input/ds_input.hpp"
#include "net/discovery.hpp"
#include "net/protocol.hpp"
#include "net/transport.hpp"
#include "video/video_decoder.hpp"

namespace ds::net {

// Bounds on how much picture is ever held back to smooth out an uneven link.
//
// The floor is one frame rather than none: a frame still waits for its slot on
// the clock, so motion keeps the host's cadence instead of this device's, but
// nothing beyond that is held speculatively. The ceiling is what a genuinely
// bad link is allowed to cost before the answer becomes "show it late" rather
// than "hold more".
inline constexpr std::size_t kMinVideoBufferFrames = 1;
inline constexpr std::size_t kMaxVideoBufferFrames = 4;

// How recently a frame must have arrived for this device to count the picture
// running dry against the link rather than against a game that is paused. A
// host with its menu open sends nothing at all, and half a second is far
// longer than any frame this is meant to notice being late by.
inline constexpr uint64_t kStreamIsLiveMs = 500;

// How long to wait before asking the host again for a picture that stands on
// its own. Comfortably longer than a round trip on the LAN this targets (tens
// of milliseconds), so a link that is merely slow does not collect several
// keyframes for one loss, and short enough that a lost request or a lost
// keyframe costs a fraction of a second rather than a visible freeze.
inline constexpr uint64_t kKeyframeRequestIntervalMs = 120;

// How long to wait at most, once repeated asking has plainly not worked. The
// interval doubles from the one above towards this on every unanswered
// request, and drops back the moment a keyframe lands.
//
// Because asking is not free. A keyframe is tens of times the size of an
// ordinary frame, and a link that is losing fragments is usually losing them
// *because* something arrived faster than it could be taken in -- so asking
// again straight away sends another burst into the very condition that lost
// the last one. Measured on a Switch: 7-8 keyframes a second, every second,
// none of them arriving whole, for an entire session. Backing off turns a
// storm that cannot recover into a handful of attempts that can.
inline constexpr uint64_t kMaxKeyframeRequestIntervalMs = 1000;

// How long a stretch to judge "can this device keep up?" over, and the two
// thresholds it is judged against -- as milliseconds spent decoding per second
// of wall clock.
//
// Turning arriving frames into drawn ones is the whole cost, not just the
// decode call: on hardware that decodes into GPU memory it is a copy back, a
// YUV-to-RGB conversion on the CPU and a texture upload, all of them
// proportional to the pixel count. Above kBusyDecodeMsPerSecond that work is
// crowding out drawing itself, which is what turns a 60fps stream into a
// stuttering, half-second-late one. Below kEasyDecodeMsPerSecond there is
// room to ask for the full picture again.
//
// The gap between the two is deliberately wide: a device sitting near one
// threshold must not spend the session stepping up and down across it, since
// every step is a resolution change and a fresh keyframe.
// Short, because the cost of reacting late is paid immediately and visibly: a
// device that cannot keep up builds a backlog it then has to hold, and at
// two-second windows a Switch spent its first six seconds at 600-1150ms behind
// before the ladder had stepped down far enough. Stepping down one rung too
// far is a much smaller mistake than that, and stepping back up is what the
// slower, more careful half of this is for.
inline constexpr uint64_t kAdaptWindowMs = 1000;
inline constexpr uint64_t kBusyDecodeMsPerSecond = 550;
inline constexpr uint64_t kEasyDecodeMsPerSecond = 250;
// How far down it may go, and in what steps.
//
// The floor is a picture size, not a fraction, because a fraction means
// nothing without knowing what it is a fraction of. Half of a phone's panel is
// about 960 across; half of an Apple TV's 3840 is 1920 -- the full size of the
// Wii U picture being sent, and precisely what that device could not decode.
// It stepped down to the floor, sat there spending 940ms of every second, and
// had nowhere left to go: the one thing the ladder exists to do, it could not
// do, because the bottom rung was above the problem.
//
// So: never below kMinDisplayWidth across, or half the panel, whichever is the
// lower bar. Every device can therefore go at least as low as it could before,
// and a big panel can go a great deal lower. 640 across is a small picture on a
// television, and a small picture that arrives is worth more than a large one
// that stutters -- which is the same argument the fraction was making, in the
// units that decide it.
inline constexpr double kMinDisplayScale = 0.5;
inline constexpr int kMinDisplayWidth = 640;
inline constexpr double kDisplayScaleStep = 0.8;

// The lowest scale allowed on a panel this wide. Pure, so the arithmetic above
// can be checked against the sizes real devices actually have.
double lowest_display_scale(int panel_width);

// Whether asking for a smaller picture is worth doing again, given what the
// last step down actually bought.
//
// The ladder assumes the work is proportional to the pixel count -- it says so
// at kBusyDecodeMsPerSecond, and on every device this was built against it was
// true. On an Apple TV it is not: measured across four rungs, 3.8 times fewer
// pixels bought 1.17 times less work. The cost there is per frame and nearly
// fixed -- a synchronous hardware decode and a lock on the decoder's own
// buffer, neither of which cares how big the picture is.
//
// A ladder that cannot tell the difference walks all the way to its floor,
// degrading the picture the whole way, and arrives no faster than it started.
// So: one step down is always worth trying, and a second only if the first
// one moved the number.
//
// `before_ms` and `after_ms` are milliseconds per frame, either side of one
// step. A tenth is the bar -- well under what proportionality would predict
// for a step that removes a third of the pixels, and well above measurement
// noise.
bool shrinking_is_worth_it(double before_ms, double after_ms);

// Whether a step actually made the picture enough smaller to learn anything
// from.
//
// "Smaller at all" is not enough, and assuming it was cost an Apple TV a whole
// game. The ladder asks for a fraction of the panel and the host sends what the
// game draws, capped by the ask -- so on a 3840-wide panel a Wii U game drawing
// 1280 sees the first four rungs (3072, 2457, 1958, 1574) change nothing, and
// the fifth (1267) shrink it by one percent. A picture one percent narrower
// cannot be expected to cost ten percent less, so judging that step said "size
// does not matter" and froze the stream at 26ms a frame. On a 60fps game that
// is a second and a half of work per second, which is exactly as playable as
// it sounds.
//
// A tenth narrower is the bar. The ladder's own step is a fifth, so a real rung
// clears it easily and rounding to even numbers does not.
bool picture_shrank_enough_to_judge(int before_width, int after_width);

// What fraction of a `box_width` x `box_height` display a picture that
// arrived at `width` x `height` actually fills: the larger of the two ratios,
// since the host fits the picture inside the box without stretching it, so the
// side that binds is the one that tells. Zero when any of the four is not a
// size.
//
// Where the ladder steps from. It used to step from what it had asked for, and
// the host never sends more than the game draws -- so on an Apple TV's
// 3840-wide panel, a Wii U game drawing 1920 took three rungs (3072, 2457,
// 1966) to reach a single pixel of difference, each of them a second of
// frames at 58ms apiece. Stepping from what arrives makes the first rung the
// first one that does anything.
double scale_the_picture_arrived_at(int width, int height, int box_width, int box_height);

// Whether a picture changed size enough, either way, to be the one a step
// asked for rather than rounding. The same tenth as
// picture_shrank_enough_to_judge(), in both directions.
bool picture_size_changed_enough(int before_width, int after_width);

// Whether the host held pictures back during a window: its serials moved on
// by `serials_advanced` while only `arrived` pictures got here. A host that has
// stopped sending to a device that has fallen behind skips the serials it
// drops, so the gap is exactly the pictures that never came.
//
// A second like that is not a second with room to spare, however little it
// cost: it was cheap because most of the pictures were never sent. Read as
// room, it walked a Switch back up to a size it had just been shown it could
// not manage -- in the middle of the very stall that had made it cheap, which
// made the stall last longer.
//
// A tenth missing is the bar. The odd picture dropped in passing is not the
// host holding a stream back.
bool host_held_pictures_back(uint64_t serials_advanced, uint64_t arrived);

// How long a step waits for pictures of its new size before it is judged
// anyway. Long enough for the host to rebuild its encoder and send a
// keyframe; a step that has not landed by then is one the host did not take.
inline constexpr uint64_t kAwaitNewSizeMs = 3000;
// How long before a size that was too expensive is worth trying once more.
// Long, because every attempt that fails costs two resolution changes and two
// keyframes, and a quiet stretch is not evidence that a busy one will fit.
inline constexpr uint64_t kRetryLargerAfterMs = 60000;

// How many decodes in a row for one screen may fail before the session is
// given up on. A hole in the stream is recovered from by asking for a picture
// that stands on its own, so a handful of failures in a row is an ordinary
// bad moment on a wireless link, not a broken session. A run this long means
// the asking is not working at all -- several seconds of a frozen picture --
// at which point saying so beats pretending.
inline constexpr unsigned kMaxConsecutiveDecodeFailures = 240;

// How many frames to keep in hand given how unevenly they have been arriving.
//
// A fixed depth has to be sized for the worst link it will ever see, and then
// every good link pays that cost for ever: frames sit finished and undisplayed
// purely because a number said so. Worse, a depth that is only ever a floor
// ratchets -- one hiccup fills it and nothing ever drains it again, so latency
// climbs over a session and never comes back down. Sized from the jitter
// actually measured instead, a calm link settles to the floor and a rough one
// buys exactly the cover it needs, for exactly as long as it needs it.
// How uneven the *link* has been lately, in milliseconds, from how long each
// frame took to get here.
//
// Fed transit times -- the far end's send stamp subtracted from local arrival.
// Those are not a delay anyone could act on: the two clocks have no
// relationship, so the number includes an unknown constant offset. But the
// offset is the same every time, so it vanishes the moment one transit is
// compared with another, and what is left is queueing on the link. The
// smallest transit seen recently is as close to "no queueing at all" as this
// link gets, and everything above it is delay the link added.
//
// Measuring it this way rather than from the spacing of arrivals is the
// difference between a buffer that helps and one that only costs. Arrivals
// alone cannot tell a link that stalled from an emulator that rendered
// slowly, and a buffer does nothing for the second: those frames were already
// late when they were made, and holding the ones around them just adds delay
// to a stutter that is still there. Citra on a Steam Deck swings between 18
// and 58 frames a second, and measured from arrivals that read as 55 to 78ms
// of link jitter -- on a link whose round trip averaged 34ms and was dropping
// almost nothing.
//
// A high quantile of the recent ones rather than the worst of them. The worst
// is a latency ratchet: one arrival 150ms late buys four frames of buffer that
// every frame after it waits behind, and the next bad moment re-arms it before
// the first has been given back. Buffering for the worst case does not even
// prevent the worst case -- a hiccup larger than what is held still hitches.
// What a buffer can absorb is the unevenness that happens all the time, and
// that is what a quantile measures. The few arrivals past it are shown a frame
// late, which is the trade Moonlight and Sunshine make by default and the one
// this app was asked for.
// The transit `coverage` of a window of them falls below, measured up from the
// quietest one in it. Exposed so the shape reported in the log and the number
// the buffer is sized from are the same arithmetic rather than two that agree
// by inspection.
double transit_quantile_above_quietest(const std::vector<double>& recent, double coverage);

// The shape of a window of transit times, each measured up from the quietest
// one in it. Reported once a second so that what the buffer is being sized
// against is a thing anyone can read, rather than a single number nobody can
// argue with -- see VideoJitter::kCoverage, which is a judgement call about
// where on this curve to sit and should be made from the curve.
struct VideoTransitShape {
    double median_ms = 0.0;
    double three_quarters_ms = 0.0;
    double worst_twentieth_ms = 0.0;
};

class VideoJitter {
public:
    void reset();
    // `transit_ms` may be negative, and routinely is: it carries the clocks'
    // offset, which can be of either sign. Only differences are taken.
    void add(double transit_ms);
    double estimate() const { return estimate_; }
    VideoTransitShape shape() const;
    // How much longer than the quietest transit in the window `transit_ms`
    // is. Negative when the window is empty and there is nothing to compare.
    double above_quietest(double transit_ms) const;

private:
    // How much of the spread the buffer is sized to absorb.
    //
    // This is the latency-against-judder trade, and it is the whole of it.
    // Covering a quantile means every frame waits for it: at the 95th, a link
    // whose slowest one-in-twenty arrives 74ms after its quietest -- measured,
    // on a Steam Deck to a Mac over wifi -- makes all twenty of them 74ms
    // late, to save the one from being shown a frame behind. That is a bad
    // bargain for anything aimed with a gyro, where the delay sits in the
    // control loop twice.
    //
    // At three quarters, three frames in four are covered as before and the
    // fourth is shown when it gets here, one display interval behind the
    // cadence. A repeated frame is a much cheaper thing to spend than tens of
    // milliseconds on every frame there is.
    //
    // Not lower than this without evidence: with no cover at all the picture
    // advances at whatever unevenness the link has, which is the judder this
    // buffer was built to remove and which the same player has already asked
    // twice to be rid of. Read the shape line in the log before moving it.
    static constexpr double kCoverage = 0.75;

    // About two and a half seconds of a 50fps stream. Long enough to hold a
    // representative sample, short enough that a link which has settled stops
    // paying for a spell when it had not -- and short enough that the clocks
    // drifting apart, which happens over minutes, never shows up inside it.
    static constexpr std::size_t kWindow = 128;
    std::vector<double> recent_;
    std::size_t next_ = 0;
    double estimate_ = 0.0;
};

// Whether anything measured during a poll that came this long after the last
// one says anything about the link, given how long a pass normally takes on
// this device.
//
// The baseline has to be this device's own cadence, not the stream's frame
// period, and getting that wrong left the hole this closes. A Mac companion
// runs its loop about every 14ms; the stream it was showing had a 33ms period,
// so a threshold of three periods tolerated a 100ms pass -- six or seven times
// this machine's normal one -- and counted everything that piled up during it
// as the link being slow. A spread of exactly that shape was what the far end
// then held 66ms of cover against: quiet for three quarters of frames and 75ms
// behind for one in twenty.
//
// A pass that ran late reads every datagram that piled up while this device
// was away at the moment it finally looks, so all of them appear to have been
// held on the way. They were not -- they were held here. Counting that as link
// jitter buys buffer, and buffer cannot cover a stall on the device doing the
// showing: the queue drains at exactly the same standstill the picture does.
// Worse, one such outlier stays in the window for a hundred and twenty-eight
// frames, so a stall of half a second keeps the buffer at its ceiling, and the
// latency with it, for seconds afterwards.
//
// The same distinction the host already makes when it declines to count a
// frame it never sent -- see FrameFragmentHeader::sent_at_ms. This is its
// mirror image at the receiving end.
//
// Generous on purpose: an ordinary pass runs about one period, and only a gap
// several times that is a stall rather than a missed refresh.
bool poll_gap_measures_the_link(uint64_t poll_gap_ms, uint64_t typical_gap_ms);

std::size_t video_buffer_target_frames(double jitter_ms, double period_ms);

// How long a finished frame may sit waiting before the pacing clock has to
// give ground and let it through, in milliseconds.
//
// This is what stops the clock hoarding, and it is the number that actually
// decides how deep the queue settles -- the buffer is not a target anything
// enforces, it is what this allows. Showing one frame per period keeps the
// cadence even but conserves the queue's depth exactly, so any backlog becomes
// permanent latency. Letting a frame through once it has waited longer than
// the link's own measured unevenness drains that backlog instead, so the queue
// settles at the depth the link needs rather than at whatever a past hiccup
// left behind. Never below one period, or there would be nothing left for the
// clock to pace with and frames would simply be shown on arrival, jitter and
// all; never above two from the measurement alone, so no jitter estimate can
// talk this back into hoarding.
//
// `extra_periods` is cover this link has shown it needs beyond that, which is
// the one thing a quantile cannot know -- see hold_extra_periods().
uint64_t video_hold_allowance_ms(double jitter_ms, uint64_t period_ms, uint64_t extra_ms,
                                 video::VideoHold hold = video::VideoHold::Balanced);

// How much cover to hold beyond what the measured spread asks for, given how
// often the picture has actually had nothing to show.
//
// Where the quantile the spread is read at stops being arguable and starts
// being a guess about a tail. Measured, Steam Deck to Mac over wifi: three
// quarters of frames land within 16ms of the quietest and one in twenty lands
// 72ms behind it. Cover the body and that one frame in twenty finds nothing
// waiting -- the picture stalls, about once a second, which is what a player
// calls judder. Cover the tail and every frame waits 72ms for it. Neither is
// a number to pick in advance, because the same numbers mean different things
// on different wifi.
//
// So this watches instead of guessing. A second in which the picture ran dry
// buys a period of cover; a spell without one gives it back. The link settles
// it, and nobody is ever asked to tune it.
uint64_t hold_extra_ms(uint64_t current_ms, uint64_t period_ms,
                       unsigned starved_last_second, unsigned calm_seconds,
                       video::VideoHold hold = video::VideoHold::Balanced);

// The two ceilings VideoHold sets: how far the measured spread alone may push
// the hold, and how much more a link that keeps running dry may earn on top.
//
// Responsive pins both at the floor -- one period, nothing earned -- which is
// pacing without smoothing. The other two differ only in how much a stalling
// link is allowed to buy, because the measured part was never the problem:
// it is the earned cover that reached 52ms in practice.
// How late a picture has to be before it counts as the link stalling rather
// than as this device having looked a moment later. See the definition.
uint64_t lateness_that_counts_ms(uint64_t period_ms, uint64_t quickest_poll_gap_ms);

uint64_t video_hold_measured_ceiling_ms(video::VideoHold hold, uint64_t period_ms);
uint64_t video_hold_extra_ceiling_ms(video::VideoHold hold, uint64_t period_ms);

// Whether a frame shown late, at this cadence on this device, says anything
// about the link -- and so whether it is worth buying cover for.
//
// A frame counts as late when it is shown more than half a period after it was
// due. That is a sound test only while this device's own loop comes round
// often enough to hit such a deadline: if the quickest pass it manages is
// longer than half a period, it can miss by simply not running, and no amount
// of cover fixes a clock finer-grained than the loop reading it.
//
// Measured, a Mac drawing a DS game: 120 frames a second arriving, so a period
// of 8ms and a deadline of 4ms, against a loop whose *quickest* pass was 9ms.
// Every miss there was the loop's. It bought 34ms of cover against a link
// whose worst one-in-twenty arrival was 11ms, and the picture waited all of it
// for nothing.
//
// A quickest gap of zero means nothing has been measured yet, which is not a
// reason to stop counting.
bool lateness_is_worth_covering(uint64_t period_ms, uint64_t quickest_poll_gap_ms);

// Whether a picture shown late was held up on its way here, rather than sent
// late in the first place -- and so whether holding more back would have
// covered it.
//
// `link_delay_ms` is how much longer than the quietest transit in the window
// this picture took to arrive (negative when unknown, which is taken as the
// link's: that is what this counted before it could tell). Half a period is
// the bar: the lateness that counts is half a period past due, and a picture
// the link delayed by less than that was mostly late for some other reason.
//
// Buffering covers a link. It does nothing for a game that drew a frame
// slowly, and cannot: the picture is shown the moment it exists either way,
// and the only thing cover adds is the same delay to every picture after it.
// Wind Waker on a Steam Deck ran the cover to its ceiling -- 66ms on top of
// the rest -- for eight seconds at a stretch, twice a minute, and a player
// felt every one of them in the controls.
bool lateness_was_the_links(double link_delay_ms, uint64_t period_ms);

// The tail of the once-a-second latency line: how many pictures were shown
// late, and which end each was late at. Empty when none were.
std::string late_pictures_summary(unsigned held_up_on_the_way, unsigned sent_late);

// How far to advance the pacing clock after showing `shown` frames.
//
// Normally exactly one period per frame, which is what keeps the cadence even.
// While a backlog is being given back the clock runs a few percent fast
// instead, so the excess drains a fraction of a frame at a time.
//
// It has to be the rate that bends rather than the frame count. Draining by
// showing an extra frame empties the queue far faster, but an extra frame
// shown in one pass is an extra frame never seen, and dropping frames several
// times a second is far more obvious than the delay it buys back. Bending the
// rate shows every frame and takes about half a second to give back a frame's
// worth of delay, which nobody can see.
uint64_t video_pace_step_ms(uint64_t period_ms, std::size_t shown, bool draining);

// The companion half: finds hosts, connects to one, and presents the screens it
// receives as a ScreenSource -- the same seam the libretro core plugs into, so
// the compositor and layout system never learn where the pixels came from.
class CompanionSession final : public ScreenSource {
public:
    enum class Status {
        Idle,
        Searching,
        Connecting,
        Connected,
        Failed,
    };

    CompanionSession();
    ~CompanionSession() override;

    // Starts browsing for hosts. Safe to call repeatedly.
    bool begin_search(const std::string& device_name, ScreenId wanted_screen, std::string* error);
    // Connects to one of the hosts found by the search.
    // By value: callers pass an entry out of hosts(), and a session that ever
    // clears that list while connecting would leave the argument dangling.
    void connect_to(DiscoveredHost host);
    void disconnect();

    void poll(uint64_t now_ms);

    Status status() const { return status_; }
    const std::string& status_detail() const { return status_detail_; }
    const std::vector<DiscoveredHost>& hosts() const { return browser_.hosts(); }
    const std::string& game_title() const { return game_title_; }
    // Whether joining is being held up by this machine refusing this app the
    // local network, rather than by the host. Worth saying on screen: the
    // waiting looks identical either way, and only one of them is worth
    // waiting through.
    bool waiting_on_local_network() const { return waiting_on_local_network_; }

    // Puts this session into the state a refused connect leaves behind --
    // Connecting, no socket, an attempt owed -- so that the retry can be
    // exercised on a machine whose connect() never produces that refusal.
    //
    // Worth a hook of its own: the state is unreachable anywhere but macOS,
    // and the bug it covers was a poll() that returned before ever looking at
    // the owed attempt, so the retry was code that could not run and no test
    // on any other platform could have noticed.
    void owe_an_attempt_for_test();
    // Which console the host is playing, so this device draws it the same way.
    SystemId system() const { return system_; }
    ScreenId granted_screen() const { return granted_screen_; }
    // The session as the host sees it. This device follows all of it rather
    // than deciding anything locally, which is what keeps the two in step.
    const std::string& host_layout_id() const { return host_layout_id_; }
    bool host_paused() const { return host_paused_; }
    bool host_filtering() const { return host_filtering_; }
    AudioTarget host_audio_target() const { return host_audio_target_; }
    Handedness host_handedness() const { return host_handedness_; }
    RightStickMode host_right_stick_mode() const { return host_right_stick_mode_; }
    int host_render_scale() const { return host_render_scale_; }
    bool host_screen_gap() const { return host_screen_gap_; }

    // The one menu, as the host has it. This device draws from these rather
    // than running a menu of its own, so both screens show the same thing
    // whichever pad is working it.
    bool host_menu_open() const { return host_menu_open_; }
    // The host's own settings screen. Not mirrored -- this device is told only
    // that it is up, so it can say where to look and keep sending presses.
    bool host_settings_open() const { return host_settings_open_; }
    int host_menu_focus() const { return host_menu_focus_; }
    uint8_t host_menu_confirm() const { return host_menu_confirm_; }
    int host_max_render_scale() const { return host_max_render_scale_; }
    bool host_cursor_choosable() const { return host_cursor_choosable_; }
    bool host_right_stick_choosable() const { return host_right_stick_choosable_; }
    bool host_cursor_shown() const { return host_cursor_shown_; }
    bool host_save_points_available() const { return host_save_points_available_; }

    // Sends this device's input to the host. Only what changed goes out.
    // `pads`, when given, is every pad on this device on its own, in this
    // device's order -- see InputMessage::pads. Without it the host treats
    // this device as one pad, the way it treats a companion built before pads
    // were reported.
    void send_input(const DsInputState& state, const std::vector<ReportedPad>* pads = nullptr);

    // Asks the host to change something. The host owns every one of these and
    // broadcasts the result back, so nothing is assumed to have taken effect
    // here -- which is exactly why the two devices cannot end up disagreeing.
    void request_layout(const std::string& layout_id);
    void request_filtering(bool bilinear);
    void request_audio_target(AudioTarget target);
    void request_handedness(Handedness handedness);
    void request_right_stick_mode(RightStickMode mode);
    void request_render_scale(int scale);
    // Whether the host reads how this device is being held -- true only on a
    // console that has a use for it, see SystemProfile::uses_motion. What the
    // client switches its sensors on and off from: a gyroscope left running
    // costs battery, so a session with a console that ignores motion never
    // starts one. False until a host has said, which is the right answer
    // before one has.
    bool host_wants_motion() const { return host_wants_motion_; }

    void request_screen_gap(bool include_gap);

    // Stands in for a frame that arrived in pieces and never completed, which
    // is the one thing a test on a loopback link cannot make happen for real
    // -- nothing is ever actually lost there. Everything downstream of this
    // point (asking, repeating, and stopping once a keyframe lands) is the
    // real path. See ask_for_keyframes().
    void lose_a_frame_for_test(ScreenId screen) {
        ((screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_) = true;
    }
    // Whether that screen is still waiting on a picture that stands on its
    // own. Goes back to false when one actually arrives.
    // Stands in for a device slow enough that turning frames into pictures
    // costs it more than it has -- which a loopback test with a fast decoder
    // cannot produce for real. Everything downstream of the measurement (the
    // decision, the step, the request that goes out) is the real path.
    void spend_this_long_decoding_for_test(uint64_t milliseconds) {
        adapt_decode_ms_ += milliseconds;
    }
    // The fraction of its display size this device is currently asking for.
    double display_scale_for_test() const { return display_scale_; }

    bool waiting_for_keyframe_for_test(ScreenId screen) const {
        return (screen == ScreenId::Top) ? keyframe_wanted_top_ : keyframe_wanted_bottom_;
    }
    // A press made on this device, for the host's menu to answer.
    void request_menu_action(MenuAction action);
    // Where a finger landed here, already resolved to an entry: the two
    // devices' menus are laid out for different screens, so the host cannot
    // work out what a tap over there was on.
    void request_menu_focus(int focus);
    // A tap: one press, at the entry the finger landed on. Sent together so
    // the two cannot be separated on the way over.
    void request_menu_press(int focus, MenuAction action);
    // One-shot: asks the host to restart the game. There is nothing to adopt
    // back, since there is no value beyond the moment it happens.
    void request_reset();
    // One-shot: asks the host to shut down entirely, the same as choosing Quit
    // there. Does not itself end this device's session -- the caller still
    // calls disconnect() to leave, the same as it would for Disconnect.
    void request_quit();
    // Not one of the choices above -- the host does not adopt or broadcast
    // this, it just uses it privately to decide how much detail a screen sent
    // here is worth being sent at. Call it once after connecting and again
    // whenever this device's own window resizes.
    void request_display_size(int width, int height);

    // Tells the host whether a menu is up on this device, so it pauses too.
    // Sent whenever it changes, and repeated with every other request.
    void set_menu_open(bool open);

    // Called with audio received from the host, ready to play. Set before
    // connecting.
    using AudioCallback =
        std::function<void(const int16_t* interleaved_stereo, std::size_t frames, uint32_t rate)>;
    void set_audio_callback(AudioCallback callback) { audio_callback_ = std::move(callback); }
    // The rate the host said it produces audio at, known from the handshake.
    uint32_t audio_sample_rate() const { return audio_sample_rate_; }

    // Called whenever the host says the Wii U GamePad's motor turned on or
    // off, so this device can rumble whatever controller it has in its
    // place. Set before connecting, same as the audio callback above.
    // `pad` is which of this device's pads, by the id it was reported under
    // (see ReportedPad), or RumbleMessage::kWholeDevice for all of them.
    // `ping_ms` non-zero is a single buzz that ends on its own; see
    // RumbleMessage::ping_ms.
    using RumbleCallback = std::function<void(bool active, uint32_t pad, uint16_t ping_ms)>;
    void set_rumble_callback(RumbleCallback callback) { rumble_callback_ = std::move(callback); }

    // How far the pacing clock may hold a frame back to even out the link --
    // see VideoHold. Safe to set at any time, including mid-session: it
    // changes the two ceilings, and cover already earned above the new one is
    // simply not applied.
    void set_video_hold(video::VideoHold hold) { video_hold_ = hold; }
    video::VideoHold video_hold() const { return video_hold_; }

    // The last frame received for a screen, as XRGB8888 pixels on the CPU,
    // with the serial that identifies it so a caller can tell a new frame from
    // one it has already drawn. Valid until the next poll(). Empty until
    // something has arrived.
    //
    // screens() hands the same pixels over as GL textures, which is what the
    // desktop compositor draws. A device that does not have that renderer --
    // the Switch client, which draws through SDL's own -- uploads these to a
    // texture of its own instead, rather than there being a second copy of
    // the receiving and decoding to keep in step with this one.
    struct ScreenPixels {
        const uint32_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;

        bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
    };
    ScreenPixels screen_pixels(ScreenId screen) const;

    // The same frame in the form it arrived in: RGB565, two bytes a pixel.
    // A renderer that can upload that directly should use this instead --
    // expanding costs a whole pass over the frame and hands the GPU twice the
    // bytes, both of which are worth avoiding on a small device.
    struct ScreenBytes {
        const uint8_t* pixels = nullptr;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;

        bool valid() const { return pixels != nullptr && width > 0 && height > 0; }
    };
    ScreenBytes screen_rgb565(ScreenId screen) const;

    // Whether a screen has ever arrived, and the serial of the last one. The
    // split layout only sends one screen, so "did the other one arrive?" is a
    // real question worth being able to answer.
    bool has_screen(ScreenId screen) const;
    uint64_t received_serial(ScreenId screen) const;
    // Top-left pixel of the last frame for a screen, for tests that have no GL
    // context and so cannot look at the texture.
    uint32_t sample_pixel(ScreenId screen) const;

    // --- ScreenSource ---
    bool advance_frame(std::string* error) override;
    const ScreenTextures& screens() override;
    // Drops the GL textures the streamed screens were uploaded into, so the
    // next screens() uploads them again. For a device whose GL context can be
    // taken away and given back -- Android, when the app is backgrounded --
    // where the texture names survive as numbers that no longer mean anything.
    void forget_screen_textures();

    // How many whole frames have arrived and not yet been shown.
    std::size_t buffered_frames() const;
    // The newest frame actually decoded here, which is what the host is told.
    uint64_t newest_frame_serial_for_test() const {
        return std::max(top_.serial, bottom_.serial);
    }
    // Which way the newest frame for this screen arrived. The two are meant to
    // be interchangeable, which is exactly why a test has to be able to tell
    // them apart: a run that quietly fell back to tiles looks identical in the
    // pixels.
    bool screen_is_video_for_test(ScreenId screen) const {
        return (screen == ScreenId::Top ? top_ : bottom_).video;
    }
    SystemScreens screens_geometry() const override;
    bool wants_shutdown() const override { return false; }

private:
    void handle_message(MessageType type, const std::vector<uint8_t>& payload);
    // Frame and Audio, the host->companion half of the media channel -- see
    // kMediaPort. A malformed or stray datagram is simply ignored rather than
    // ending the session: UDP has no stream to desynchronise, so one bad
    // packet is just one bad packet.
    void handle_media_datagram(MessageType type, const uint8_t* payload, std::size_t payload_size);
    // Accumulates one screen's fragments (see FrameFragmentHeader) until
    // they are all in, then hands the whole thing to pending_frames_ the same
    // way a TCP-carried Frame message used to arrive complete already. A
    // fragment for a newer serial than whatever is in progress discards the
    // old, incomplete one -- the sender has moved on, so waiting for stray
    // fragments of a frame that will never complete would only add latency.
    struct FrameReassembly {
        bool active = false;
        uint64_t serial = 0;
        ScreenId screen = ScreenId::Top;
        FrameEncoding encoding = FrameEncoding::Rgb565Deflate;
        uint16_t width = 0;
        uint16_t height = 0;
        uint16_t fragment_count = 0;
        // What the host stamped on this picture. See VideoJitter.
        uint32_t sent_at_ms = 0;
        std::size_t received_count = 0;
        std::vector<std::vector<uint8_t>> fragments;
        // Separate from fragments[i].empty(): a fragment can legitimately
        // carry zero payload bytes, which must not read as "not received yet"
        // and stall a reassembly that is otherwise already complete.
        std::vector<bool> received;
    };
    void handle_frame_fragment(const uint8_t* payload, std::size_t payload_size);
    // Notices frames that left the host and never got here, from the gap they
    // leave in the run of sent_index values for that screen. Returns how many
    // went missing, which is zero for the ordinary case of the next one in the
    // run, and for anything arriving out of order or twice.
    unsigned note_frames_that_never_arrived(const FrameFragmentHeader& header);

    // Sends a KeyframeRequest for whichever screens are waiting on one, at
    // most one per screen per kKeyframeRequestIntervalMs. Called every poll()
    // while anything is waiting.
    void ask_for_keyframes(uint64_t now_ms);

    FrameReassembly reassembly_top_;
    FrameReassembly reassembly_bottom_;
    // The last sent_index seen on each screen, and whether one has been seen
    // at all yet. The far end of FrameFragmentHeader::sent_index: a jump of
    // more than one is frames that were sent and never arrived.
    //
    // This is what makes whole-frame loss visible. Reassembly can only see a
    // frame that arrived in pieces and never finished; a frame that fits in
    // one datagram -- which at a kilobyte a frame is nearly every DS frame --
    // used to vanish without leaving anything behind to notice, and the
    // decoder would go on predicting from a reference it never had until the
    // next scheduled keyframe came round.
    uint16_t last_sent_index_top_ = 0;
    uint16_t last_sent_index_bottom_ = 0;
    bool sent_index_known_top_ = false;
    bool sent_index_known_bottom_ = false;
    // Whether this screen is waiting on a picture that stands on its own,
    // because a frame arrived in pieces and the rest of it never did. Until
    // one does, everything decoded for this screen is built on a reference
    // frame this end never saw whole. Cleared only by a keyframe actually
    // arriving, not by having asked: the request rides UDP like everything
    // else here and can be lost, so "keep asking until the answer lands" is
    // the only thing that reliably ends it. See ask_for_keyframe().
    bool host_wants_motion_ = false;
    bool keyframe_wanted_top_ = false;
    bool keyframe_wanted_bottom_ = false;
    // When each was last asked for, so a repeat goes out at a sensible rate
    // rather than once per poll() -- see kKeyframeRequestIntervalMs.
    uint64_t keyframe_asked_at_top_ = 0;
    uint64_t keyframe_asked_at_bottom_ = 0;
    // How long to wait before asking again, per screen. Starts at
    // kKeyframeRequestIntervalMs, doubles while asking goes unanswered, and
    // is reset by a keyframe actually arriving -- see
    // kMaxKeyframeRequestIntervalMs.
    uint64_t keyframe_wait_top_ms_ = kKeyframeRequestIntervalMs;
    uint64_t keyframe_wait_bottom_ms_ = kKeyframeRequestIntervalMs;
    // How many frames arrived in pieces that never completed, since the last
    // report. The one measurement that says whether the link is actually
    // losing anything, which is what the host's long keyframe interval is
    // betting on.
    unsigned stats_frames_lost_ = 0;
    // Pictures passed over while a keyframe was awaited -- see
    // handle_frame_fragment().
    unsigned stats_frames_skipped_ = 0;
    // Decodes that have failed in a row for each screen, cleared by the first
    // picture that comes out -- see kMaxConsecutiveDecodeFailures.
    unsigned decode_failures_top_ = 0;
    unsigned decode_failures_bottom_ = 0;
    // Pictures in a row the renderer could not draw from the decoder's own
    // memory -- see screens(). At kNativeTextureFailuresToGiveUp the decoder
    // is told to copy them out instead, for the rest of the run.
    unsigned native_texture_failures_ = 0;
    static constexpr unsigned kNativeTextureFailuresToGiveUp = 3;
    void send_request(RequestMessage request);
    void fail(const std::string& reason);
    // One attempt at reaching joining_. Leaves the session Connecting either
    // way: a failure the operating system may answer differently in a moment
    // schedules another attempt rather than ending the join.
    void begin_connect();
    // Reaches out to each newly found host so the operating system makes up
    // its mind about the local network before anybody picks one.
    void knock_on_the_local_network();
    // Puts that attempt down and owes another one. See connect_is_undecided().
    void owe_another_attempt(const std::string& reason);

    // Frames that have arrived but not yet been applied.
    //
    // The host produces on its own clock and this device shows on its own, and
    // the two are never quite the same: a frame or two arrives inside one
    // display interval and none inside the next. Applying everything that has
    // arrived, as this used to, turns that into a shown frame skipped and then
    // a shown frame repeated -- which is what a stream at a healthy sixty
    // frames a second still looks choppy at.
    //
    // Kept whole rather than collapsed because a tiled frame is a difference
    // from the one before it: they have to be applied in order, and none can
    // be thrown away without corrupting the picture.
    std::deque<FrameMessage> pending_frames_;
    // Defined further down, with the rest of what a screen holds.
    struct ScreenBuffer;
    void apply_frame(const FrameMessage& frame);
    // False when nothing came out: a failure, already reported, or a stream
    // still waiting for its first keyframe.
    bool apply_video_frame(const FrameMessage& frame, ScreenBuffer& target);
    bool decode_next_frame();

    HostBrowser browser_;
    TcpStream stream_;
    std::vector<uint8_t> inbox_;
    // Video, audio, input and frame-acks ride here instead of the TCP
    // connection above -- see kMediaPort. An ephemeral local port is enough:
    // this device only ever sends to and receives from the one host, whose
    // fixed, well-known port it already sends to directly.
    UdpSocket media_socket_;
    // From WelcomeMessage, echoed on every datagram this device sends there
    // -- see HostSession::session_token_'s own comment for why.
    uint64_t session_token_ = 0;
    // The host's own address, learned from the TCP connection (same machine,
    // just a different port for the media channel).
    std::string host_address_;

    Status status_ = Status::Idle;
    std::string status_detail_;
    std::string device_name_;
    ScreenId wanted_screen_ = ScreenId::Bottom;
    ScreenId granted_screen_ = ScreenId::Bottom;
    std::string game_title_;
    SystemId system_ = SystemId::Ds;
    std::string host_layout_id_;
    bool host_paused_ = false;
    bool host_filtering_ = false;
    AudioTarget host_audio_target_ = AudioTarget::Both;
    Handedness host_handedness_ = Handedness::Right;
    RightStickMode host_right_stick_mode_ = RightStickMode::CStick;
    int host_render_scale_ = 1;
    bool host_screen_gap_ = true;
    bool host_menu_open_ = false;
    bool host_settings_open_ = false;
    int host_menu_focus_ = 0;
    uint8_t host_menu_confirm_ = 0;
    int host_max_render_scale_ = 1;
    bool host_cursor_choosable_ = false;
    bool host_right_stick_choosable_ = false;
    bool host_cursor_shown_ = true;
    bool host_save_points_available_ = false;
    bool menu_open_ = false;
    bool said_hello_ = false;
    // The host being joined, kept for as long as the join takes: an attempt
    // that has to be made again needs somewhere to make it to.
    DiscoveredHost joining_;
    // Whether an attempt is owed, and when it is due. The time is zero until
    // the first poll() after the failure, for the same reason the deadline
    // below is: the clock belongs to the caller.
    bool retry_connect_ = false;
    // How many times this join has tried, so the log can say whether the
    // retrying is what got there.
    unsigned connect_attempts_ = 0;
    // Whether the attempts are being refused by this machine's own
    // local-network permission rather than by anything on the network. The
    // join is given far longer in that case -- see kJoinTimeoutMs -- because
    // what it may be waiting for is a person answering a prompt.
    bool waiting_on_local_network_ = false;
    // Hosts already reached out to during this search -- see
    // knock_on_the_local_network().
    std::vector<std::string> knocked_;
    uint64_t retry_connect_at_ms_ = 0;
    // Why the last attempt did not get anywhere. Kept so that a join which
    // runs out of time after several of these says what actually went wrong
    // rather than blaming a host that may well have been answering.
    std::string connect_failure_;
    // When the last poll() ran, so this one can tell whether it is late --
    // see poll_gap_measures_the_link() -- and whether the transits it is about
    // to measure describe the link or this device. Zero before the first one.
    // Cover this link has earned beyond what its measured spread asks for,
    // in milliseconds -- see hold_extra_ms(). Zero on a link that never runs
    // the picture dry.
    uint64_t extra_hold_ms_ = 0;
    // Which of the two ends of that trade this player asked for.
    video::VideoHold video_hold_ = video::VideoHold::Balanced;
    // Pictures shown more than half a period after they were due, which is
    // the only running-dry a player can see -- see where this is counted for
    // why "the queue was empty" is not the same thing at all.
    unsigned stats_starved_ = 0;
    // Pictures shown just as late, that got here as promptly as any: the host
    // sent them late. The game drew slowly, or the host did. No cover this end
    // could hold would have shown them sooner -- a picture cannot be buffered
    // before it exists -- so these buy none. Counted so the log can say which
    // end the stalls are at.
    unsigned stats_late_from_host_ = 0;
    unsigned calm_seconds_ = 0;
    uint64_t last_poll_ms_ = 0;
    // The quickest pass this device has managed lately, which is this loop
    // with nothing in its way -- the same notion of a baseline the transit
    // times use, for the same reason. Anything several times it is this
    // machine being busy, not the network.
    static constexpr std::size_t kPollGapWindow = 64;
    std::vector<uint64_t> recent_poll_gaps_;
    std::size_t next_poll_gap_ = 0;
    uint64_t typical_poll_gap_ms_ = 0;
    bool this_pass_measures_the_link_ = false;
    // When to give up on a connection that has not finished arriving. Zero
    // until the first poll() after connect_to(), so the whole timer runs on
    // the caller's clock rather than half on one and half on another.
    uint64_t connect_deadline_ms_ = 0;
    // The newest frame serial already reported to the host, and the newest
    // that has arrived. Reported on arrival rather than on being shown: the
    // ack answers "did this get here", which is the host's flow control, and
    // frames waiting their turn below have got here. Answering it with what
    // has been shown instead made the host read this device's own smoothing
    // as a device falling behind, and throttle against it.
    uint64_t acked_serial_ = 0;
    uint64_t received_serial_ = 0;

    // When the next frame is due on screen, and how far apart frames are
    // arriving. Paced by a clock rather than by one per pass, because a pass
    // is not a frame: a 90Hz panel showing 60fps content passes half again as
    // often as there is anything new to show, and pinning the two together
    // leaves nothing buffered and each frame held for whichever number of
    // refreshes its arrival happened to land on.
    uint64_t next_due_ms_ = 0;
    uint64_t last_arrival_ms_ = 0;
    double frame_period_ms_ = 16.7;
    uint64_t frame_period() const;
    // How unevenly frames have been arriving lately, in milliseconds, and the
    // buffer depth that calls for. Every frame held back is a frame of latency
    // paid for smoothness, so this buys only as much as the link is actually
    // being uneven by, and gives it back as soon as the link settles -- see
    // video_buffer_target_frames().
    VideoJitter jitter_;
    std::size_t target_buffer_frames() const;

    // When each frame finished reassembling, so that the wait between having
    // every byte of a picture and actually showing it can be measured rather
    // than reasoned about. That wait is the one part of the end-to-end delay
    // this end chooses rather than inherits, which makes it the first thing
    // worth knowing the real size of -- see report_stream().
    //
    // With how far behind the quietest transit in the window each one got
    // here, which is how much of its lateness the link is to blame for --
    // see lateness_was_the_links(). Negative when there was nothing to
    // measure it against.
    struct Arrival {
        uint64_t serial = 0;
        uint64_t at_ms = 0;
        double link_delay_ms = -1.0;
    };
    std::deque<Arrival> complete_at_;
    // The same, for the picture decode_next_frame() most recently showed.
    double shown_link_delay_ms_ = -1.0;
    uint64_t stats_wait_total_ms_ = 0;
    uint64_t stats_wait_worst_ms_ = 0;
    unsigned stats_waits_ = 0;
    uint64_t stats_decode_total_ms_ = 0;
    unsigned stats_decodes_ = 0;

    // --- keeping up ---------------------------------------------------------
    //
    // What the client asked for -- its panel -- kept separately from what is
    // actually asked of the host, because on a slow device those are not the
    // same thing.
    //
    // Turning an arriving frame into something drawable is not just the
    // decode: on a device whose decoder hands frames back in the GPU's own
    // memory, they have to be copied back out, converted from YUV to RGB on
    // the CPU, and uploaded as a texture. All three cost time proportional to
    // the pixel count, and none of them is what the hardware decoder is good
    // at. Measured on a Switch at 1280x720: 30ms per frame, against a 16ms
    // frame period -- so it decoded 23 frames a second, drew 11, and held
    // every one of them 200ms while it caught up.
    //
    // Fewer pixels is the lever that actually moves that, and this end is the
    // only one that can know it is needed. Asking the host for a smaller
    // picture costs nothing on the wire and nothing on the host, which was
    // already scaling to whatever size it was told.
    int wanted_display_width_ = 0;
    int wanted_display_height_ = 0;
    // A fraction of the above, in each direction. 1.0 until this device shows
    // it cannot keep up.
    double display_scale_ = 1.0;
    // Decode time accumulated since the last time that decision was made, and
    // when that was.
    uint64_t adapt_decode_ms_ = 0;
    // The share of the above that was not decoding at all, but copying the
    // finished picture out of the decoder's memory. See DecodedFrame::
    // readback_micros: on an Apple device those are different costs with
    // different answers, and the log has to be able to say which one is
    // eating the second.
    uint64_t adapt_readback_micros_ = 0;
    // The share of the readback that was the lock -- see DecodedFrame::
    // lock_micros.
    uint64_t adapt_lock_micros_ = 0;
    // How many pictures that time was spent on, so the window can be read per
    // frame as well as per second. Per second saturates -- a device that is
    // the bottleneck spends all of it whatever the picture size, because the
    // smaller the frames the more of them get through -- and a number that is
    // always the same cannot say whether anything helped. Per frame can.
    uint64_t adapt_frames_ = 0;
    // The cost per frame when this device last asked for a smaller picture,
    // and whether shrinking it turned out to matter at all. See
    // shrinking_is_worth_it().
    double adapt_cost_before_step_ms_ = 0.0;
    // And how wide the pictures actually were at the time.
    //
    // Asking for a smaller picture is not the same as getting one. The host
    // sends what the game draws, capped by what was asked for -- so on a 4K
    // panel the first rungs are 3072 and 2456 wide, both of them larger than
    // the 1920 a Wii U game draws, and a step between them changes nothing at
    // all. Judging "did shrinking help" across a step like that compares a
    // picture with itself, finds no improvement, and concludes that size does
    // not matter -- which is exactly what happened, and it froze a device at
    // the one size it could not manage.
    int adapt_width_before_step_ = 0;
    // The width of the last picture decoded in this window, which is what the
    // request above has to be judged against.
    int adapt_last_width_ = 0;
    int adapt_last_height_ = 0;
    // The largest share of the display any picture in this window filled --
    // see scale_the_picture_arrived_at(). The rung the next step goes from.
    double adapt_arrived_scale_ = 0.0;
    // The width a step was asked from, while its pictures have not arrived
    // yet, and when it was asked. Nothing is judged in between.
    //
    // A window that straddles a step judges a mixture: some pictures at the
    // old size, some at the new, a cost per picture that belongs to neither,
    // and a verdict on the new size made mostly from the old one. That walked
    // an Apple TV two rungs below where it could keep up -- 29ms a picture at
    // 998 wide was three at 1574 and the rest at 998, which by then cost 13
    // -- and it only found its way back up half a minute later. So the
    // window starts again once the first picture of the new size is in,
    // after that picture, which also carries the cost of a new decoder and a
    // keyframe that no later picture pays.
    // The newest serial when this window began, and how many pictures have
    // arrived since -- see host_held_pictures_back().
    uint64_t adapt_serial_at_start_ = 0;
    uint64_t adapt_arrivals_ = 0;
    int adapt_awaiting_from_width_ = 0;
    uint64_t adapt_awaiting_since_ms_ = 0;
    bool adapt_restart_window_ = false;
    bool adapt_size_decides_cost_ = true;
    bool said_size_does_not_decide_ = false;
    bool said_held_back_is_not_room_ = false;
    uint64_t adapt_at_ms_ = 0;
    // Consecutive windows with room to spare, so stepping back up needs a
    // sustained case rather than one quiet moment.
    unsigned adapt_easy_windows_ = 0;
    // The smallest scale that has actually been measured as more than this
    // device can keep up with, and when. Stepping back up stops just below it
    // rather than walking into it again.
    //
    // Without this the ladder oscillates, and it did: 51% -> 64% -> 51% ->
    // 64% -> 51% in one session. The cost of a step up is not predictable
    // from the cost at the smaller size -- the bitrate rises with the
    // resolution and the scene keeps changing -- so the only reliable thing
    // to go on is that this size was tried and did not work. After
    // kRetryLargerAfterMs it is worth one more try, since what a device can
    // keep up with depends on what the game is doing.
    double too_expensive_scale_ = 2.0;
    uint64_t too_expensive_at_ms_ = 0;

    // Looks at what decoding has been costing and, if this device cannot keep
    // up with it, asks the host for fewer pixels -- or for more again once it
    // comfortably can. Called from poll().
    void adapt_to_what_this_device_can_decode(uint64_t now_ms);
    // Sends whatever wanted_display_width_/height_ and display_scale_
    // currently come to.
    void send_display_size();

    // The other half of the host's once-a-second summary: what actually
    // arrived here. The two together say whether frames are being lost on the
    // wire or never sent in the first place.
    void report_stream(uint64_t now_ms);
    uint64_t stats_at_ms_ = 0;
    unsigned stats_decoded_ = 0;

public:
    // Counted by whoever is drawing, once per presented frame. Decoded and
    // drawn are different questions: a device can be handed every frame and
    // still show a fraction of them, and only the second is what the player
    // sees. Told apart here because the difference is the whole diagnosis.
    void note_frame_drawn() { ++stats_drawn_; }

private:
    unsigned stats_drawn_ = 0;

    // Latest pixels per screen, uploaded lazily so a frame that arrives twice
    // between draws is only sent to the GPU once.
    struct ScreenBuffer {
        // Which of the two below is the frame and which is derived from it. A
        // tiled payload patches rgb565 and is expanded on demand; a video frame
        // decodes straight to pixels and never has a 5-6-5 form at all.
        bool video = false;
        // Which form a video frame came back in -- see DecodedLayout. Xrgb8888
        // means `pixels` holds it; the two planar layouts mean `plane` does,
        // and the conversion to colour happens in the shader while it is
        // drawn rather than here.
        video::DecodedLayout layout = video::DecodedLayout::Xrgb8888;
        // The planes of a planar frame, each with its own row length in bytes.
        std::vector<uint8_t> plane[3];
        int plane_stride[3] = {0, 0, 0};
        // Or, instead of `plane`, the decoder's own picture, left where it is
        // -- see DecodedFrame::native. What an iPhone or an Apple TV gets.
        std::shared_ptr<void> native;
        // The frame as it travels and as tiles are patched onto it. Canonical:
        // a tiled payload is a difference from this, so it has to survive from
        // one frame to the next.
        std::vector<uint8_t> rgb565;
        int width = 0;
        int height = 0;
        uint64_t serial = 0;
        uint64_t uploaded_serial = 0;
        // Expanded only when somebody asks for XRGB8888, and only once per
        // frame that does. A client that can upload 5-6-5 straight to a
        // texture never asks, and so never pays for the pass or the doubled
        // bytes.
        mutable std::vector<uint32_t> pixels;
        mutable uint64_t expanded_serial = 0;
        mutable bool ever_expanded = false;
        Texture texture;

        const std::vector<uint32_t>& expanded() const;
        // Whether anything has arrived for this screen at all, whichever form
        // it arrived in.
        bool has_frame() const {
            if (!video) return !rgb565.empty();
            if (layout == video::DecodedLayout::Xrgb8888) return !pixels.empty();
            return !plane[0].empty() || native != nullptr;
        }
    };
    ScreenBuffer top_;
    ScreenBuffer bottom_;
    // One per screen, matching the encoders at the other end: two streams, two
    // sizes, and each frame only means anything after the ones before it.
    video::VideoDecoder video_top_;
    video::VideoDecoder video_bottom_;
    ScreenTextures textures_;

    std::vector<ReportedPad> last_sent_pads_;
    bool last_sent_reported_pads_ = false;
    DsInputState last_sent_input_;
    bool ever_sent_input_ = false;
    AudioCallback audio_callback_;
    uint32_t audio_sample_rate_ = 0;
    RumbleCallback rumble_callback_;
    // Per AudioStreamId (3 entries -- Shared/TopDevice/BottomDevice), for
    // detecting a genuinely missing audio packet rather than the ordinary
    // jitter UDP never promised against -- see AudioMessage::sequence and
    // handle_media_datagram()'s Audio case.
    bool audio_sequence_known_[3] = {false, false, false};
    uint32_t audio_next_sequence_[3] = {0, 0, 0};
};

const char* to_string(CompanionSession::Status status);

}  // namespace ds::net
