// Pairs a real HostSession with a real CompanionSession over loopback: the
// handshake, the screens the layout says are needed, and input travelling back.
// No GL context, so the companion's pixel buffers are inspected rather than its
// textures.

#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <utility>
#include <vector>

#include "net/companion_session.hpp"
#include "net/host_session.hpp"
#include "video/video_codec.hpp"

using namespace ds;
using namespace ds::net;

namespace {

// Runs both sides until `done` or the deadline, the way the frame loop would.
template <typename Predicate>
bool spin(HostSession& host, CompanionSession& companion, Predicate done, int milliseconds = 3000) {
    const uint64_t deadline = monotonic_millis() + static_cast<uint64_t>(milliseconds);
    while (monotonic_millis() < deadline) {
        const uint64_t now = monotonic_millis();
        host.poll(now);
        companion.poll(now);
        if (done()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return done();
}

// Both sides have to agree the handshake finished. Waiting only for the host
// to see a companion returns before the companion has read the welcome, so
// anything the welcome carries is still unset.
bool paired(HostSession& host, CompanionSession& companion) {
    return spin(host, companion, [&] {
        return host.has_companion() && host.companion_media_ready_for_test() &&
               companion.status() == CompanionSession::Status::Connected;
    });
}

// The host's shared state, which is what both devices follow. Spelled out here
// because most tests only care about one field of it.
void set_shared(HostSession& host,
                const std::string& layout_id,
                bool bilinear_filtering = false,
                AudioTarget audio_target = AudioTarget::Both,
                Handedness handedness = Handedness::Right,
                int render_scale = 1,
                bool screen_gap = true,
                RightStickMode right_stick_mode = RightStickMode::CStick) {
    HostSession::SharedState shared;
    shared.layout_id = layout_id;
    shared.per_device_layout = (layout_id == "upper_on_tv");
    shared.bilinear_filtering = bilinear_filtering;
    shared.audio_target = audio_target;
    shared.handedness = handedness;
    shared.render_scale = render_scale;
    shared.screen_gap = screen_gap;
    shared.right_stick_mode = right_stick_mode;
    host.set_session_state(shared);
}

// Publishing takes each screen's own pixels and size, so a system whose screens
// differ can say so. These tests use a DS's, which are the same.
HostSession::ScreenPixels screen_pixels(const std::vector<uint32_t>& pixels) {
    HostSession::ScreenPixels entry;
    entry.pixels = pixels.data();
    entry.width = kDsScreenWidth;
    entry.height = kDsScreenHeight;
    entry.stride_pixels = kDsScreenWidth;
    return entry;
}

std::vector<uint32_t> flat_screen(uint32_t colour) {
    return std::vector<uint32_t>(static_cast<std::size_t>(kDsScreenWidth) * kDsScreenHeight, colour);
}

// A connect the operating system refuses has to be tried again, and the
// trying has to actually happen.
//
// It did not. poll() returned early whenever there was no socket, and a
// refused attempt closes its socket -- so the retry below it was code that
// could never run. The join said "trying again" once and then sat on
// Connecting for ever: it never retried, and it never even reached its own
// deadline to give up. From the outside that is a connect that hangs until
// you cancel and start over, which is exactly what it did on a Mac, every
// first try, for as long as the retry has existed.
void test_a_refused_connect_is_really_tried_again() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());

    // Exactly the state a refusal leaves behind: still joining, no socket, an
    // attempt owed. Reached through a hook because no connect() outside macOS
    // produces it -- see owe_an_attempt_for_test().
    companion.owe_an_attempt_for_test();
    CHECK(companion.status() == CompanionSession::Status::Connecting);
    CHECK(companion.waiting_on_local_network());

    // And the session gets there anyway, without anybody starting over.
    const bool connected = spin(host, companion, [&] {
        return companion.status() == CompanionSession::Status::Connected && host.has_companion();
    });
    CHECK_MSG(connected, std::string("the owed attempt was never made: ") +
                             companion.status_detail());
    CHECK(!companion.waiting_on_local_network());
}

void test_host_and_companion_pair_up() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);

    // Find the host by discovery rather than by hardcoding an address: that is
    // the path the player actually takes.
    bool found = spin(host, companion, [&] { return !companion.hosts().empty(); });
    CHECK_MSG(found, "the companion never discovered the host");
    if (!found) return;

    DiscoveredHost target = companion.hosts().front();
    CHECK_STR_EQ(target.game_title, "Test Game");
    // Both screens are on offer until one is taken: the host adapts to whichever
    // the companion asks for.
    CHECK(target.offers(ScreenId::Bottom));
    CHECK(target.offers(ScreenId::Top));

    companion.connect_to(target);
    const bool connected = spin(host, companion, [&] {
        return companion.status() == CompanionSession::Status::Connected && host.has_companion() &&
               host.companion_media_ready_for_test();
    });
    CHECK_MSG(connected, std::string("handshake did not complete: ") + companion.status_detail());
    if (!connected) return;

    CHECK_STR_EQ(companion.game_title(), "Test Game");
    CHECK(companion.granted_screen() == ScreenId::Bottom);
    CHECK_STR_EQ(host.companion_name(), "Test Deck");
    CHECK_STR_EQ(companion.host_layout_id(), "upper_on_tv");

    // --- screens -----------------------------------------------------------
    // Under the split layout the companion needs only its own screen, so the
    // top must never arrive.
    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    const std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    host.publish_screens(screen_pixels(top), screen_pixels(bottom), 1);

    const bool got_bottom =
        spin(host, companion, [&] { return companion.has_screen(ScreenId::Bottom); });
    CHECK_MSG(got_bottom, "the companion's own screen never arrived");
    CHECK_MSG(!companion.has_screen(ScreenId::Top),
              "the top screen was sent even though the split layout does not need it");
    if (got_bottom) {
        CHECK_EQ(companion.received_serial(ScreenId::Bottom), static_cast<uint64_t>(1));
        // Close, not equal: RGB565 is lossy.
        const uint32_t pixel = companion.sample_pixel(ScreenId::Bottom);
        CHECK_MSG(std::abs(static_cast<int>((pixel >> 16) & 0xff) - 0x80) <= 8 &&
                      std::abs(static_cast<int>(pixel & 0xff) - 0x20) <= 8,
                  "the wrong screen's pixels arrived");
    }

    // --- input travels back ------------------------------------------------
    DsInputState input;
    input.set(DsButton::A, true);
    input.set(DsButton::Down, true);
    input.touching = true;
    input.touch_x = 200;
    input.touch_y = 100;
    companion.send_input(input);

    const bool got_input = spin(host, companion, [&] { return host.companion_input_fresh(); });
    CHECK_MSG(got_input, "the companion's input never reached the host");
    if (got_input) {
        CHECK(host.companion_input().pressed(DsButton::A));
        CHECK(host.companion_input().pressed(DsButton::Down));
        CHECK(!host.companion_input().pressed(DsButton::B));
        CHECK_EQ(host.companion_input().touch_x, 200);
        CHECK_EQ(host.companion_input().touch_y, 100);
        CHECK_EQ(host.companion_input().touching, true);
    }

    // --- leaving -----------------------------------------------------------
    companion.disconnect();
    const bool host_noticed = spin(host, companion, [&] { return !host.has_companion(); });
    CHECK_MSG(host_noticed, "the host did not notice the companion leaving");

    host.stop();
}

// A screen streamed above the console's own resolution -- what a 3DS at its
// maximum render scale, read back for a companion with a big enough window,
// actually produces -- must still arrive rather than being read as corrupt.
// The companion's own sanity check on incoming frame dimensions was written
// when every screen was always sent at native resolution and never revisited
// once that stopped being true, so a real 3DS top screen at 3x (1200x720)
// tripped it and the companion dropped the connection reporting "impossible
// dimensions" on a perfectly good frame.
// The whole received screen, not just its corner: a client that draws through
// something other than the GL compositor -- the Switch one does -- uploads
// these pixels to a texture of its own, so the buffer has to be readable in
// full, at the size the frame actually arrived at, with a serial that moves
// when a new frame lands.
void test_a_received_screen_is_readable_as_pixels() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Switch", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }

    // Nothing has arrived yet, so there is nothing to draw and the caller has
    // to be able to tell -- a client that trusted a pointer here would upload
    // whatever the buffer happened to contain.
    CHECK(!companion.screen_pixels(ScreenId::Bottom).valid());

    // A gradient rather than a flat fill: a buffer handed back at the wrong
    // stride, or only partly filled, looks exactly like a flat one.
    std::vector<uint32_t> bottom(static_cast<std::size_t>(kDsScreenWidth) * kDsScreenHeight);
    for (int y = 0; y < kDsScreenHeight; ++y) {
        for (int x = 0; x < kDsScreenWidth; ++x) {
            const uint32_t red = static_cast<uint32_t>(x) & 0xf8u;
            const uint32_t green = static_cast<uint32_t>(y) & 0xf8u;
            bottom[static_cast<std::size_t>(y) * kDsScreenWidth + x] = (red << 16) | (green << 8);
        }
    }
    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    host.publish_screens(screen_pixels(top), screen_pixels(bottom), 7);

    if (!spin(host, companion, [&] { return companion.has_screen(ScreenId::Bottom); })) {
        CHECK_MSG(false, "the companion's own screen never arrived");
        return;
    }

    const CompanionSession::ScreenPixels frame = companion.screen_pixels(ScreenId::Bottom);
    CHECK(frame.valid());
    CHECK_EQ(frame.width, kDsScreenWidth);
    CHECK_EQ(frame.height, kDsScreenHeight);
    CHECK_EQ(frame.serial, static_cast<uint64_t>(7));

    // Every pixel, at the far corner as well as the near one. RGB565 is lossy,
    // so this asks for close rather than equal.
    int worst = 0;
    for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
            const uint32_t got = frame.pixels[static_cast<std::size_t>(y) * frame.width + x];
            const uint32_t want = bottom[static_cast<std::size_t>(y) * kDsScreenWidth + x];
            for (int shift : {16, 8, 0}) {
                const int difference = std::abs(static_cast<int>((got >> shift) & 0xff) -
                                                static_cast<int>((want >> shift) & 0xff));
                worst = std::max(worst, difference);
            }
        }
    }
    CHECK_MSG(worst <= 8, "the decoded screen was off by " + std::to_string(worst));

    // A new frame has to be distinguishable from the one already drawn, which
    // is the whole point of the serial travelling with the pixels.
    host.publish_screens(screen_pixels(top), screen_pixels(flat_screen(0x00112233u)), 8);
    const bool moved = spin(host, companion, [&] {
        return companion.screen_pixels(ScreenId::Bottom).serial == 8;
    });
    CHECK_MSG(moved, "a second frame never replaced the first");

    companion.disconnect();
    host.stop();
}

void test_a_screen_above_the_old_native_only_size_still_arrives() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test Deck";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Bottom;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    // The companion holds the top screen, the same role the bug report's PC
    // was in: connected to a Deck hosting, showing the top screen.
    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test PC", ScreenId::Top, &error), error);
    const bool found = spin(host, companion, [&] { return !companion.hosts().empty(); });
    CHECK_MSG(found, "the companion never discovered the host");
    if (!found) return;

    companion.connect_to(companion.hosts().front());
    const bool connected = spin(host, companion, [&] {
        return companion.status() == CompanionSession::Status::Connected && host.has_companion() &&
               host.companion_media_ready_for_test();
    });
    CHECK_MSG(connected, std::string("handshake did not complete: ") + companion.status_detail());
    if (!connected) return;

    // The exact size named in the guard's own comment: a 3DS top screen read
    // back at its maximum render scale.
    constexpr int kWidth = 1200;
    constexpr int kHeight = 720;
    const std::vector<uint32_t> pixels(static_cast<std::size_t>(kWidth) * kHeight, 0x00204080u);
    HostSession::ScreenPixels top;
    top.pixels = pixels.data();
    top.width = kWidth;
    top.height = kHeight;
    top.stride_pixels = kWidth;
    host.publish_screens(top, HostSession::ScreenPixels(), 1);

    const bool got_it = spin(host, companion, [&] { return companion.has_screen(ScreenId::Top); });
    CHECK_MSG(got_it, std::string("a legitimate 1200x720 screen never arrived (") +
                          companion.status_detail() + ")");
    CHECK_MSG(companion.status() == CompanionSession::Status::Connected,
              "the companion dropped a connection over a screen bigger than the old cap");

    companion.disconnect();
    host.stop();
}

// A companion asking for a screen while a non-split layout is active must
// receive both screens, because it draws the whole layout itself.
void test_non_split_layout_sends_both_screens() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC 2";
    config.game_title = "Both Screens";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        return;  // covered by the first test's skip
    }
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Deck 2", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }

    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00112233u);
    const std::vector<uint32_t> bottom = flat_screen(0x00445566u);
    // A distinct serial per publish, as the app does when pixels change.
    for (uint64_t serial = 1; serial <= 3; ++serial) {
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        spin(host, companion, [] { return false; }, 60);
    }

    // Both screens have to have arrived; the companion is drawing the whole
    // vertical layout, not just its own half.
    CHECK_STR_EQ(companion.host_layout_id(), "vertical");
    CHECK_MSG(companion.has_screen(ScreenId::Top), "the top screen never arrived");
    CHECK_MSG(companion.has_screen(ScreenId::Bottom), "the bottom screen never arrived");
    CHECK_MSG(std::abs(static_cast<int>((companion.sample_pixel(ScreenId::Top) >> 16) & 0xff) -
                       0x11) <= 8,
              "the top screen carried the wrong pixels");
    CHECK_MSG(std::abs(static_cast<int>((companion.sample_pixel(ScreenId::Bottom) >> 16) & 0xff) -
                       0x44) <= 8,
              "the bottom screen carried the wrong pixels");

    companion.disconnect();
    host.stop();
}

// The reported bug: "connect as top screen" and "connect as bottom screen"
// behaved identically because the host decoded the companion's request and then
// ignored it, always granting the opposite of its own fixed screen.
void test_companion_gets_the_screen_it_asked_for() {
    for (const ScreenId wanted : {ScreenId::Top, ScreenId::Bottom}) {
        HostSession host;
        HostSession::Config config;
        config.device_name = "Screen Choice Host";
        config.game_title = "Choice";
        config.host_screen = ScreenId::Top;  // deliberately the same either way
        config.audio_sample_rate = 32000;

        std::string error;
        if (!host.start(config, &error)) {
            return;  // covered by the first test's skip
        }
        set_shared(host, "upper_on_tv", false, AudioTarget::Both);

        CompanionSession companion;
        if (!companion.begin_search("Chooser", wanted, &error)) {
            host.stop();
            return;
        }
        if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
            host.stop();
            return;
        }

        // Both screens must be on offer before anyone has taken one, or the
        // picker would grey out the very choice being made.
        const DiscoveredHost found = companion.hosts().front();
        CHECK_MSG(found.offers(ScreenId::Top) && found.offers(ScreenId::Bottom),
                  "an unpaired host did not offer both screens");

        companion.connect_to(found);
        const bool connected = spin(host, companion, [&] {
            return companion.status() == CompanionSession::Status::Connected &&
                   host.has_companion() && host.companion_media_ready_for_test();
        });
        CHECK_MSG(connected, "handshake did not complete");
        if (!connected) {
            host.stop();
            return;
        }

        const char* asked = to_string(wanted);
        CHECK_MSG(companion.granted_screen() == wanted,
                  std::string("asked for the ") + asked + " screen but was granted the " +
                      to_string(companion.granted_screen()));
        CHECK_MSG(host.companion_screen() == wanted,
                  std::string("the host thinks the companion has the ") +
                      to_string(host.companion_screen()) + " screen");
        // ...and the host takes the other one, whatever it started as.
        CHECK_MSG(host.host_screen() != wanted, "both devices claimed the same screen");

        // The frames that arrive must be the ones asked for.
        const std::vector<uint32_t> top = flat_screen(0x00204080u);
        const std::vector<uint32_t> bottom = flat_screen(0x00801020u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), 1);
        const bool arrived = spin(host, companion, [&] { return companion.has_screen(wanted); });
        CHECK_MSG(arrived, std::string("the ") + asked + " screen never arrived");
        const ScreenId other = (wanted == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;
        CHECK_MSG(!companion.has_screen(other),
                  "the screen the companion did not ask for was sent anyway");

        companion.disconnect();
        host.stop();
    }
}

// Sound is mirrored: what the host plays, the companion plays too.
void test_audio_reaches_the_companion() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Audio Host";
    config.game_title = "Noisy";
    config.audio_sample_rate = 32823;

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    std::size_t frames_heard = 0;
    uint32_t heard_rate = 0;
    int16_t first_sample = 0;
    companion.set_audio_callback(
        [&](const int16_t* samples, std::size_t frames, uint32_t rate) {
            if (frames_heard == 0 && frames > 0) {
                first_sample = samples[0];
            }
            frames_heard += frames;
            heard_rate = rate;
        });

    if (!companion.begin_search("Listener", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    // The rate is part of the handshake, so the companion can open its output
    // to match rather than guessing.
    CHECK_EQ(companion.audio_sample_rate(), static_cast<uint32_t>(32823));

    std::vector<int16_t> tone(512 * 2);
    for (std::size_t index = 0; index < tone.size(); ++index) {
        tone[index] = static_cast<int16_t>((index % 64) * 300 - 9600);
    }
    host.publish_audio(AudioStreamId::Shared, tone.data(), 512, 32823);

    const bool heard = spin(host, companion, [&] { return frames_heard > 0; });
    CHECK_MSG(heard, "no audio reached the companion");
    if (heard) {
        CHECK_EQ(frames_heard, static_cast<std::size_t>(512));
        CHECK_EQ(heard_rate, static_cast<uint32_t>(32823));
        CHECK_EQ(first_sample, tone[0]);
    }

    // Turning it off stops it, without disturbing anything else.
    frames_heard = 0;
    host.set_audio_enabled(false);
    host.publish_audio(AudioStreamId::Shared, tone.data(), 512, 32823);
    spin(host, companion, [] { return false; }, 200);
    CHECK_MSG(frames_heard == 0, "audio was still sent after being switched off");

    companion.disconnect();
    host.stop();
}

// A companion can ask for a layout; the host owns the decision.
void test_companion_can_request_a_layout() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Layout Host";
    config.game_title = "Layouts";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Asker", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    // Nothing pending before anything is asked for.
    RequestMessage requested;
    CHECK(!host.take_request(requested));

    companion.request_layout("vertical");
    const bool asked = spin(host, companion, [&] { return host.take_request(requested); });
    CHECK_MSG(asked, "the layout request never reached the host");
    if (asked) {
        CHECK_STR_EQ(requested.layout_id, "vertical");
        // Taken once only, so a request is not applied repeatedly.
        RequestMessage again;
        CHECK(!host.take_request(again));
    }

    // And the host's answer comes back, so both devices agree.
    set_shared(host, "vertical", false, AudioTarget::Both);
    const bool agreed =
        spin(host, companion, [&] { return companion.host_layout_id() == "vertical"; });
    CHECK_MSG(agreed, "the host's layout change never reached the companion");

    companion.disconnect();
    host.stop();
}

// There is one menu per session, not one per device: the host holds it and the
// companion draws what it is told, so the two screens cannot show a menu in two
// different states while the player works it from either pad.
void test_the_menu_is_one_menu_across_both_devices() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Menu Host";
    config.game_title = "One Menu";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "vertical");

    CompanionSession companion;
    if (!companion.begin_search("Watcher", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    CHECK_MSG(!companion.host_menu_open(), "a companion thought a menu was up before one was");

    // What the host has, the companion draws -- down to which entry the cursor
    // is on and whether a confirmation is up over it.
    HostSession::SharedState shared;
    shared.layout_id = "vertical";
    shared.menu_open = true;
    shared.menu_focus = 4;
    shared.menu_confirm = 2;  // MenuConfirm::Quit
    shared.max_render_scale = 3;
    shared.cursor_choosable = true;
    shared.right_stick_choosable = true;
    shared.left_stick_choosable = true;
    shared.left_stick_press = LeftStickPress::SwapLayout;
    host.set_session_state(shared);
    const bool drawn = spin(host, companion, [&] {
        return companion.host_menu_open() && companion.host_menu_focus() == 4;
    });
    CHECK_MSG(drawn, "the host's open menu never reached the companion");
    CHECK_EQ(static_cast<int>(companion.host_menu_confirm()), 2);
    CHECK_MSG(companion.host_max_render_scale() == 3 && companion.host_cursor_choosable() &&
                  companion.host_right_stick_choosable(),
              "the companion was not told which entries that menu has, so its row could differ");
    CHECK_MSG(companion.host_left_stick_choosable() &&
                  companion.host_left_stick_press() == LeftStickPress::SwapLayout,
              "the companion was not told the menu offers the left stick's click, so its Options "
              "list would be a row short");

    // And a press made over there is answered here, not there.
    RequestMessage pending;
    CHECK(!host.take_request(pending));
    companion.request_menu_press(2, MenuAction::Confirm);

    bool saw_focus = false;
    bool saw_action = false;
    spin(host, companion, [&] {
        HostSession::MenuPress got;
        while (host.take_menu_press(got)) {
            if (got.has_focus && got.focus == 2) saw_focus = true;
            if (got.has_action && got.action == MenuAction::Confirm) saw_action = true;
        }
        return saw_focus && saw_action;
    });
    CHECK_MSG(saw_focus, "the companion's tap never reached the menu it was aimed at");
    CHECK_MSG(saw_action, "the companion's press never reached the menu it was aimed at");

    // Two presses made in quick succession are two moves. Sent back to back so
    // they land in one poll, which is exactly when a single held slot would
    // keep the last and quietly lose the first.
    companion.request_menu_action(MenuAction::Down);
    companion.request_menu_action(MenuAction::Down);
    int downs = 0;
    spin(host, companion, [&] {
        HostSession::MenuPress got;
        while (host.take_menu_press(got)) {
            if (got.has_action && got.action == MenuAction::Down) ++downs;
        }
        return downs >= 2;
    });
    CHECK_MSG(downs == 2, "two presses arrived as " + std::to_string(downs) +
                              ": a cursor moved once where the player moved it twice");

    // Closing is the host's to announce too, so the menu comes down on both.
    shared.menu_open = false;
    host.set_session_state(shared);
    CHECK_MSG(spin(host, companion, [&] { return !companion.host_menu_open(); }),
              "the companion kept a menu up after the host put it away");

    companion.disconnect();
    host.stop();
}

// The settings both devices share are the host's to hold. A companion asks and
// then adopts what comes back, which is what makes "always in sync" true rather
// than merely likely: there is no second copy to disagree with.
void test_shared_settings_stay_in_sync() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Sync Host";
    config.game_title = "Syncing";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Sync Deck", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    // Smoothing: asked for on the companion, decided on the host, and only then
    // true on both. Nothing is applied locally on the way through.
    companion.request_filtering(true);
    RequestMessage request;
    const bool asked = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_filtering;
    });
    CHECK_MSG(asked, "the smoothing request never reached the host");
    CHECK_EQ(request.bilinear_filtering, true);
    // Asking about smoothing must not read as asking for a layout as well.
    CHECK(request.layout_id.empty());
    CHECK_EQ(request.wants_audio_target, false);

    set_shared(host, "upper_on_tv", true, AudioTarget::Both);
    CHECK_MSG(spin(host, companion, [&] { return companion.host_filtering(); }),
              "the host's smoothing never reached the companion");

    // The sound target travels the same way.
    companion.request_audio_target(AudioTarget::TopScreen);
    const bool asked_audio = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_audio_target;
    });
    CHECK_MSG(asked_audio, "the sound request never reached the host");
    CHECK(request.audio_target == AudioTarget::TopScreen);

    set_shared(host, "upper_on_tv", true, AudioTarget::TopScreen);
    CHECK_MSG(spin(host, companion,
                   [&] { return companion.host_audio_target() == AudioTarget::TopScreen; }),
              "the host's sound target never reached the companion");

    // And which hand holds the stylus, which mirrors what both devices draw.
    companion.request_handedness(Handedness::Left);
    const bool asked_hand = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_handedness;
    });
    CHECK_MSG(asked_hand, "the handedness request never reached the host");
    CHECK(request.handedness == Handedness::Left);

    set_shared(host, "upper_on_tv", true, AudioTarget::TopScreen, Handedness::Left);
    CHECK_MSG(spin(host, companion,
                   [&] { return companion.host_handedness() == Handedness::Left; }),
              "the host's handedness never reached the companion");

    // And the render scale, which only the host can actually act on -- it is
    // the one running a core. The companion still asks for it from the same
    // menu, because there is only one menu.
    companion.request_render_scale(2);
    const bool asked_scale = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_render_scale;
    });
    CHECK_MSG(asked_scale, "the render scale request never reached the host");
    CHECK_EQ(static_cast<int>(request.render_scale), 2);

    set_shared(host, "upper_on_tv", true, AudioTarget::TopScreen, Handedness::Left, 2);
    CHECK_MSG(spin(host, companion, [&] { return companion.host_render_scale() == 2; }),
              "the host's render scale never reached the companion");

    // And the screen gap choice, the same shape as everything above: the
    // companion asks, the host decides and broadcasts.
    companion.request_screen_gap(false);
    const bool asked_gap = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_screen_gap;
    });
    CHECK_MSG(asked_gap, "the screen gap request never reached the host");
    CHECK_EQ(request.screen_gap, false);

    set_shared(host, "upper_on_tv", true, AudioTarget::TopScreen, Handedness::Left, 2, false);
    CHECK_MSG(spin(host, companion, [&] { return companion.host_screen_gap() == false; }),
              "the host's screen gap choice never reached the companion");

    // And what the right stick is read as. Both devices' pads drive the game,
    // so both have to be reading their own stick the same way -- which is why
    // this belongs to the host like the rest of it rather than being decided
    // wherever the menu happened to be opened.
    companion.request_right_stick_mode(RightStickMode::DPad);
    const bool asked_stick = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_right_stick_mode;
    });
    CHECK_MSG(asked_stick, "the right stick request never reached the host");
    CHECK(request.right_stick_mode == RightStickMode::DPad);

    // The left stick's click, the same way: asked of the host, not decided here.
    companion.request_left_stick_press(LeftStickPress::SwapLayout);
    const bool asked_left = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_left_stick_press;
    });
    CHECK_MSG(asked_left, "the left stick request never reached the host");
    CHECK(request.left_stick_press == LeftStickPress::SwapLayout);

    set_shared(host, "upper_on_tv", true, AudioTarget::TopScreen, Handedness::Left, 2, false,
               RightStickMode::DPad);
    CHECK_MSG(spin(host, companion,
                   [&] { return companion.host_right_stick_mode() == RightStickMode::DPad; }),
              "the host's right stick choice never reached the companion");

    // Reset is one-shot: there is no value to broadcast back, just the request
    // itself reaching the host.
    companion.request_reset();
    const bool asked_reset = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_reset;
    });
    CHECK_MSG(asked_reset, "the reset request never reached the host");

    // Quit is the same shape: one-shot, nothing to broadcast back. The app
    // layer is what actually shuts the host down on seeing it.
    companion.request_quit();
    const bool asked_quit = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_quit;
    });
    CHECK_MSG(asked_quit, "the quit request never reached the host");

    // The companion's window size travels the other way from everything
    // above: the host keeps it privately, to decide how much detail a screen
    // it sends is worth, rather than adopting and broadcasting it like a
    // shared setting. Unknown until told, which is what a companion that has
    // not reported yet looks like from the host's side.
    int reported_width = 0;
    int reported_height = 0;
    CHECK(!host.companion_display_size(reported_width, reported_height));
    companion.request_display_size(1920, 1080);
    CHECK_MSG(spin(host, companion,
                   [&] { return host.companion_display_size(reported_width, reported_height); }),
              "the companion's display size never reached the host");
    CHECK_EQ(reported_width, 1920);
    CHECK_EQ(reported_height, 1080);

    companion.disconnect();
    host.stop();
}

// A tap quicker than a frame -- press and release both landing between two of
// the host's frames -- still reaches the game as a tap. Tapping a Goomba as it
// runs across the screen is exactly that quick, and a game that only ever
// sees the release sees nothing at all.
void test_a_tap_quicker_than_a_frame_still_reaches_the_game() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Tap Host";
    config.game_title = "Tapping";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Tap Deck", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    const std::vector<ReportedPad> no_pads;
    DsInputState tap;
    tap.touching = true;
    tap.touch_x = 40;
    tap.touch_y = 50;
    companion.send_input(tap, &no_pads);
    CHECK_MSG(spin(host, companion, [&] { return host.companion_input().touching; }),
              "the companion's touch never reached the host");

    // Let go before the game has run a frame: the host keeps the touch until
    // one has.
    companion.send_input(DsInputState(), &no_pads);
    spin(host, companion, [] { return false; }, 150);
    CHECK_MSG(host.companion_input().touching,
              "a tap was let go before the game had a frame to see it in");
    host.note_frame_ran(1);
    CHECK_MSG(!host.companion_input().touching,
              "the release was held back after the game had seen the touch");

    // A touch the game has already seen is let go at once.
    companion.send_input(tap, &no_pads);
    CHECK(spin(host, companion, [&] { return host.companion_input().touching; }));
    host.note_frame_ran(2);
    companion.send_input(DsInputState(), &no_pads);
    CHECK_MSG(spin(host, companion, [&] { return !host.companion_input().touching; }),
              "a release was held back although the game had already seen the touch");

    companion.disconnect();
    host.stop();
}

// A menu on the companion pauses the host. Otherwise one player reads a menu
// while the other's game carries on without them.
void test_companion_menu_pauses_the_host() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Pause Host";
    config.game_title = "Pausing";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Pause Deck", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    HostSession::MenuPress unused;
    CHECK(!host.take_menu_open_request());
    CHECK(!host.take_menu_press(unused));

    // Something held down as the menu opens.
    DsInputState held;
    held.buttons = 1u << static_cast<unsigned>(DsButton::A);
    held.touching = true;
    held.touch_x = 100;
    held.touch_y = 100;
    companion.send_input(held);
    CHECK_MSG(spin(host, companion, [&] { return host.companion_input().buttons != 0; }),
              "the companion's input never reached the host");

    companion.set_menu_open(true);
    CHECK_MSG(spin(host, companion, [&] { return host.take_menu_open_request(); }),
              "the companion's ask for the menu never reached the host");
    // Only changes go over the wire, so a button held as the menu opened would
    // stay held for as long as the menu was up if the host did not let go of it.
    CHECK_EQ(host.companion_input().buttons, 0u);
    CHECK_EQ(host.companion_input().touching, false);

    // Taken once. Every request the companion sends carries this flag, so a
    // level read here would reopen a menu the host had just closed -- and the
    // host is the one that holds it.
    CHECK_MSG(!host.take_menu_open_request(), "one ask for the menu was read as two");
    companion.request_filtering(true);
    spin(host, companion, [&] {
        RequestMessage seen;
        return host.take_request(seen) && seen.wants_filtering;
    });
    CHECK_MSG(!host.take_menu_open_request(),
              "a request about something else reopened the menu on its way past");

    // Closed and asked for again: a second ask is a second opening.
    companion.set_menu_open(false);
    spin(host, companion, [&] {
        RequestMessage seen;
        host.take_request(seen);
        return false;
    });
    companion.set_menu_open(true);
    CHECK_MSG(spin(host, companion, [&] { return host.take_menu_open_request(); }),
              "a companion that had closed the menu could not open it again");

    companion.disconnect();
    CHECK_MSG(spin(host, companion, [&] { return !host.has_companion(); }),
              "the host never noticed the companion leaving");
    CHECK_MSG(!host.take_menu_open_request(),
              "a device that left was still asking for a menu");

    host.stop();
}

// Disconnecting is only reachable from the menu -- the Disconnect button is
// on it -- so a device that just left had its own menu_open_ set at that
// exact moment. If that survived into the next session, the very first
// request the reconnected companion sends would tell the host its menu was
// still open, pausing the host permanently with no way for the companion to
// clear it: the menu that would send menu_open = false again is the one this
// device no longer thinks is open.
void test_reconnecting_after_a_menu_disconnect_does_not_leave_the_host_paused() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Reconnect Host";
    config.game_title = "Reconnecting";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Reconnect Deck", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    const DiscoveredHost discovered = companion.hosts().front();
    companion.connect_to(discovered);
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    // Leaves exactly the way the in-menu Disconnect button does: with the
    // menu open.
    companion.set_menu_open(true);
    CHECK(spin(host, companion, [&] { return host.take_menu_open_request(); }));
    companion.disconnect();
    CHECK(spin(host, companion, [&] { return !host.has_companion(); }));

    companion.connect_to(discovered);
    CHECK_MSG(paired(host, companion), "the companion could not reconnect");

    // Any request carries the menu state, so an unrelated one is enough to
    // show what the new session's first message actually said -- without
    // this, host.companion_menu_open() reading false here would only prove
    // drop_companion() reset the *host's* copy, which was never in question.
    companion.request_filtering(true);
    RequestMessage received;
    const bool got_request = spin(host, companion, [&] { return host.take_request(received); });
    CHECK_MSG(got_request, "the reconnected companion's request never reached the host");
    CHECK_MSG(!received.menu_open,
              "a stale menu_open_ from the last session leaked into the new one");

    host.stop();
}

// The Quit button on the companion sends its quit request immediately
// followed by its own Bye -- both usually land in the same poll(). That has
// to still reach the app: drop_companion() (triggered by the Bye) clears the
// generic request_/have_request_ pair, which would otherwise silently lose a
// quit request that arrived in the same batch but had not been read yet.
void test_quit_reaches_the_host_even_though_the_companion_disconnects_right_behind_it() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Quit Host";
    config.game_title = "Quitting";

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv", false, AudioTarget::Both);

    CompanionSession companion;
    if (!companion.begin_search("Quit Deck", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    // Exactly what the Quit button does: ask, then leave right after.
    companion.request_quit();
    companion.disconnect();

    CHECK_MSG(spin(host, companion, [&] { return host.take_companion_quit_request(); }),
              "the quit request was lost behind the companion's own disconnect");

    host.stop();
}

// A companion showing one screen still needs both the moment its menu opens: the
// preview tiles draw every layout, including the one it is not being sent. The
// catch is that a menu also pauses the game, so no new frame is coming to carry
// the change across -- the host has to re-send what it already has.
void test_menu_gets_the_companion_both_screens() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Screens Host";
    config.game_title = "Screens";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "upper_on_tv");

    CompanionSession companion;
    if (!companion.begin_search("Screens Deck", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00112233u);
    const std::vector<uint32_t> bottom = flat_screen(0x00445566u);
    auto publish = [&](uint64_t serial) {
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
    };

    // One screen each: the companion holds the bottom one and is sent only that.
    publish(1);
    CHECK_MSG(spin(host, companion, [&] { return companion.has_screen(ScreenId::Bottom); }),
              "the companion never got the screen it owns");
    CHECK_MSG(!companion.has_screen(ScreenId::Top),
              "the companion was sent a screen it does not show");

    // Now its menu goes up. The game is paused from here on, so the serial never
    // moves again -- and the top screen still has to arrive.
    companion.set_menu_open(true);
    CHECK(spin(host, companion, [&] { return host.take_menu_open_request(); }));
    const bool arrived = spin(host, companion, [&] {
        publish(1);  // the same frame, over and over, exactly as a paused host does
        return companion.has_screen(ScreenId::Top);
    });
    CHECK_MSG(arrived, "the other screen never reached the companion's menu");
    if (arrived) {
        // Close, not equal: RGB565 is lossy. What matters is that it is the top
        // screen's pixels rather than a copy of the bottom's.
        const uint32_t pixel = companion.sample_pixel(ScreenId::Top);
        CHECK_MSG(std::abs(static_cast<int>((pixel >> 16) & 0xff) - 0x11) <= 8 &&
                      std::abs(static_cast<int>(pixel & 0xff) - 0x33) <= 8,
                  "the wrong screen arrived for the menu");
    }

    companion.disconnect();
    host.stop();
}

// Either end can end the session.
// Joining has to either happen or stop happening. A host that accepts the
// connection and then never says hello -- one that has wedged, or one reached
// at an address that goes nowhere useful -- used to leave the companion
// sitting in "connecting" forever, with nothing on screen changing and every
// button appearing to do nothing. That is what makes joining look like it
// needs several tries.
void test_joining_gives_up_on_a_host_that_never_answers() {
    // A listener that accepts and then says nothing at all. Not a HostSession:
    // the point is a peer that completes the TCP handshake and no more.
    TcpListener silent;
    std::string error;
    if (!silent.start(0, &error)) {
        std::fprintf(stderr, "skipping: cannot listen (%s)\n", error.c_str());
        return;
    }

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);

    DiscoveredHost host;
    host.address = "127.0.0.1";
    host.session_port = silent.port();
    host.device_name = "Wedged PC";
    companion.connect_to(host);
    CHECK(companion.status() == CompanionSession::Status::Connecting);

    // Driven on a clock of this test's own, the way the frame loop drives it.
    // Well inside the deadline it is still trying, which is what stops a slow
    // handshake being called a failure.
    TcpStream accepted;
    uint64_t now = 1'000'000;
    for (int i = 0; i < 40; ++i) {
        silent.accept(accepted);
        now += 50;
        companion.poll(now);
    }
    CHECK_MSG(companion.status() == CompanionSession::Status::Connecting,
              std::string("gave up too early: ") + to_string(companion.status()));

    // Past it, it stops rather than waiting on the operating system.
    now += kJoinTimeoutMs;
    companion.poll(now);
    CHECK_MSG(companion.status() == CompanionSession::Status::Failed,
              std::string("still ") + to_string(companion.status()) + " after " +
                  std::to_string(kJoinTimeoutMs) + "ms");
    CHECK(!companion.status_detail().empty());

    companion.disconnect();
    silent.stop();
}

// And a host that does answer is not caught by that deadline: the timer is
// only ever running while a connection is still arriving.
void test_a_real_host_is_not_timed_out() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    CHECK_MSG(paired(host, companion), "the handshake did not complete");

    // Long past the deadline, and still connected: nothing re-arms it.
    const uint64_t later = monotonic_millis() + kJoinTimeoutMs * 4;
    host.poll(later);
    companion.poll(later);
    CHECK(companion.status() == CompanionSession::Status::Connected);

    companion.disconnect();
    host.stop();
}

// A companion that falls behind must stop being sent frames it cannot show.
//
// This is what regressed when frames became differences from the last one
// rather than whole pictures. The backlog limit was a byte count standing in
// for "two or three frames", and a DS screen with little moving on it now
// compresses to well under a hundred bytes -- measured on a real session, a
// 57-byte median frame against a 98304-byte limit, which is over a thousand
// frames of backlog before a single one is dropped. That is not a limit, it
// is half a minute of delay that nothing is ever dropped to relieve.
//
// The companion here acks a few frames and then goes quiet, which is what a
// device whose link cannot carry the stream looks like from the host: frames
// keep being handed to the socket, and nothing comes back to say any of them
// arrived.
// Frames that arrive together are shown one at a time, on a clock -- and the
// waiting ones are still reported to the host as arrived.
//
// The host produces on its own clock and a companion shows on its own, so a
// frame or two lands inside one display interval and none inside the next.
// Showing everything that has arrived turns that into a frame skipped and then
// a frame repeated, which is what a stream running at a healthy sixty frames a
// second still looks choppy at.
//
// The pace is a clock rather than one per pass because a pass is not a frame:
// a 90Hz panel showing 60fps content passes half again as often as there is
// anything new to show.
//
// And what is waiting has still *arrived*. The ack is the host's flow control
// -- it answers "did this get there" -- so counting only what has been shown
// made the host read this device's own smoothing as a device falling behind,
// and drop frames against it.
void test_frames_that_arrive_together_are_shown_on_a_clock() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!spin(host, companion, [&] {
            return companion.status() == CompanionSession::Status::Connected &&
                   host.has_companion() && host.companion_media_ready_for_test();
        })) {
        CHECK_MSG(false, "handshake did not complete");
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    const auto publish = [&](uint64_t serial) {
        bottom[serial % bottom.size()] = static_cast<uint32_t>(serial * 2654435761u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        host.poll(monotonic_millis());
    };

    publish(1);
    if (!spin(host, companion, [&] { return companion.newest_frame_serial_for_test() == 1; })) {
        CHECK_MSG(false, "the first frame never arrived");
        return;
    }

    // Two at once, as a moment of network jitter delivers them -- matching
    // kMaxQueuedFrames (see host_session.cpp), the most the host will now let
    // itself get ahead of an unacknowledged frame. A third here would not
    // test this pacing any further; it would just be the host's own backlog
    // limit dropping it before it was ever sent, which is its own behaviour
    // (and its own coverage) rather than this one's.
    publish(2);
    publish(3);
    // Give the socket a moment, without letting the companion read them yet.
    // Long enough that the pacing clock is unambiguously stale and restarts
    // from the first pass below, so what follows does not depend on how long
    // the lines above happened to take.
    for (int wait = 0; wait < 60; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        host.poll(monotonic_millis());
    }

    // A synthetic clock from here, so what is being tested is the pacing and
    // not how long these lines took to run.
    const uint64_t start_ms = monotonic_millis();
    companion.poll(start_ms);
    CHECK_MSG(companion.newest_frame_serial_for_test() == 2,
              "the first pass showed serial " +
                  std::to_string(companion.newest_frame_serial_for_test()) +
                  " rather than advancing one frame from 1");
    // The rest are held, not thrown away: each is a difference from the last.
    CHECK_MSG(companion.buffered_frames() == 1,
              std::to_string(companion.buffered_frames()) + " frames were held rather than 1");

    // A pass before the next frame is due changes nothing on screen.
    companion.poll(start_ms + 4);
    CHECK_MSG(companion.newest_frame_serial_for_test() == 2,
              "a pass before the frame was due showed it early");

    // One frame period later it is due, and the one held frame is shown.
    companion.poll(start_ms + 40);
    CHECK_MSG(companion.newest_frame_serial_for_test() == 3,
              "the pass after a frame period showed serial " +
                  std::to_string(companion.newest_frame_serial_for_test()) + " rather than 3");

    // Throughout, the host must have been told all four arrived -- the two
    // that were waiting had got there, whatever was on screen.
    if (!spin(host, companion, [&] { return host.companion_frames_behind_for_test() == 0; })) {
        CHECK_MSG(false, "the host still counts the companion " +
                             std::to_string(host.companion_frames_behind_for_test()) +
                             " frames behind, which is its own buffer being read as a backlog");
    }
}

void test_a_companion_that_falls_behind_stops_being_sent_frames() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!spin(host, companion, [&] {
            return companion.status() == CompanionSession::Status::Connected &&
                   host.has_companion() && host.companion_media_ready_for_test();
        })) {
        CHECK_MSG(false, "handshake did not complete");
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    // Different every frame, so none is skipped for having nothing to say.
    const auto publish = [&](uint64_t serial) {
        bottom[serial % bottom.size()] = static_cast<uint32_t>(serial * 2654435761u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        host.poll(monotonic_millis());
    };

    // Keeping up: the host must be free to send, and the acks must come back.
    for (uint64_t serial = 1; serial <= 12; ++serial) {
        publish(serial);
        companion.poll(monotonic_millis());
    }
    CHECK_MSG(companion.newest_frame_serial_for_test() > 0,
              "the companion never decoded a frame while it was keeping up");

    // Now it stops answering.
    constexpr uint64_t kSilentFrames = 400;
    std::size_t queued = 0;
    for (uint64_t serial = 13; serial < 13 + kSilentFrames; ++serial) {
        const uint64_t before = host.queued_bytes_for_test();
        publish(serial);
        if (host.queued_bytes_for_test() != before) {
            ++queued;
        }
    }

    CHECK_MSG(queued <= 16, "the host queued " + std::to_string(queued) + " of " +
                                std::to_string(kSilentFrames) +
                                " frames for a companion that had stopped answering");
}

// Serials advance at whatever rate the game produces, whoever ends up sending
// them. A source running faster than the link carries therefore leaves gaps
// between the serials that do get sent -- and the backlog gate, which is
// written in frames, used to be applied to that gap. On Bayonetta 2 (60
// produced a second, 20 sent) one frame in flight already put three serials
// between sent and acked, so a limit of two refused to send with less than one
// frame outstanding. Wind Waker sends every frame it makes, so the two counts
// agreed and nothing looked wrong.
void test_a_source_faster_than_the_link_is_still_allowed_frames_in_flight() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!spin(host, companion, [&] {
            return companion.status() == CompanionSession::Status::Connected &&
                   host.has_companion() && host.companion_media_ready_for_test();
        })) {
        CHECK_MSG(false, "handshake did not complete");
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    const auto publish = [&](uint64_t serial) {
        bottom[serial % bottom.size()] = static_cast<uint32_t>(serial * 2654435761u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        host.poll(monotonic_millis());
    };

    // Acking normally first, so the gate is armed at all.
    for (uint64_t serial = 1; serial <= 6; ++serial) {
        publish(serial);
        companion.poll(monotonic_millis());
    }

    // Now the companion goes quiet and the source jumps serials the way a
    // 60fps game does when only every third frame is being sent. The gate
    // must still let kMaxQueuedFrames whole frames out, not stop after one
    // because the serial numbers moved further than the frame count did.
    uint64_t sent_while_silent = 0;
    uint64_t previous = host.queued_bytes_for_test();
    for (uint64_t step = 1; step <= 6; ++step) {
        publish(100 + step * 3);
        const uint64_t now = host.queued_bytes_for_test();
        if (now != previous) {
            ++sent_while_silent;
            previous = now;
        }
    }
    CHECK_MSG(sent_while_silent >= 2,
              "a source whose serials outrun its frames was throttled to " +
                  std::to_string(sent_while_silent) +
                  " frame(s) in flight, below the limit the gate is written for");
}

void test_a_companion_that_stops_acking_entirely_is_recovered_from() {
    // A companion that has merely fallen behind (the test above) still acks
    // eventually, so the backlog gate reopens on its own. This is the harder
    // case, reproduced live on real hardware: every frame the host currently
    // has outstanding is lost together (a real packet-loss burst was enough,
    // since kMaxQueuedFrames only ever allows a couple), so no ack for any
    // of them is ever coming -- nothing here retries a lost UDP datagram.
    // Without a way out, the host would wait for that ack forever: sent_serial_
    // stuck at acked_serial_ + kMaxQueuedFrames, publish_screens() refusing
    // every frame after, looking to both ends like the picture had simply
    // stopped -- audio included, since a stalled video path was mistaken
    // upstream for a stalled session. See kAckStallMs's own comment.
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!spin(host, companion, [&] {
            return companion.status() == CompanionSession::Status::Connected &&
                   host.has_companion() && host.companion_media_ready_for_test();
        })) {
        CHECK_MSG(false, "handshake did not complete");
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    const auto publish = [&](uint64_t serial) {
        bottom[serial % bottom.size()] = static_cast<uint32_t>(serial * 2654435761u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        host.poll(monotonic_millis());
    };

    // Keeping up first, the same as the test above, so this is a companion
    // that was working and then went silent -- not one that never answered
    // at all.
    for (uint64_t serial = 1; serial <= 6; ++serial) {
        publish(serial);
        companion.poll(monotonic_millis());
    }
    CHECK_MSG(companion.newest_frame_serial_for_test() > 0,
              "the companion never decoded a frame while it was keeping up");

    // Now every frame from here is lost together: fill the backlog gate and
    // never let the companion see any of them. Filled by publishing until the
    // gate actually shuts rather than by counting to kMaxQueuedFrames, so this
    // keeps testing the stall itself however wide the window is set.
    uint64_t serial = 7;
    uint64_t queued_before_wait = host.queued_bytes_for_test();
    bool gate_shut = false;
    for (int attempt = 0; attempt < 64 && !gate_shut; ++attempt, ++serial) {
        publish(serial);
        const uint64_t now_queued = host.queued_bytes_for_test();
        gate_shut = (now_queued == queued_before_wait);
        queued_before_wait = now_queued;
    }
    CHECK_MSG(gate_shut, "the backlog gate never shut, so there is no stall to recover from");
    CHECK_MSG(host.companion_frames_behind_for_test() > 0,
              "the host was not actually backlogged before the stall -- this test needs it to be");

    // Past ack_stall_ms() with no ack having arrived in between -- which for
    // a link that has been keeping up, as this one was, is a few of its round
    // trips, lifted to the 400ms floor on loopback. Real time, since the
    // stall check reads the clock directly rather than through poll()'s own
    // now_ms parameter, the same way video_retry_at_ms_ elsewhere in this
    // file already does.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    const uint64_t queued_before_recovery = host.queued_bytes_for_test();
    const uint64_t recovery_serial = serial + 1;
    publish(recovery_serial);
    CHECK_MSG(host.queued_bytes_for_test() != queued_before_recovery,
              "the host never recovered from a companion that stopped acking entirely");

    // And the companion, once it starts listening again, gets a full,
    // correct picture rather than a diff computed against a baseline it was
    // never actually confirmed to have.
    if (!spin(host, companion,
              [&] { return companion.newest_frame_serial_for_test() == recovery_serial; })) {
        CHECK_MSG(false, "the companion never caught back up after the host recovered");
    }
}

// A frame lost just as the game paused -- the menu opening does both at once
// -- is resent while it stays paused, not only once the game moves again.
// Nothing new is published while paused, and noticing a lost frame used to
// happen only on the way to sending a new one: measured, a companion's menu
// sat on a half-arrived picture for the two and a half minutes the menu was
// up, and the host said "no ack for 150579ms" on closing it.
void test_a_frame_lost_as_the_game_paused_is_sent_again_while_paused() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    const auto publish = [&](uint64_t serial) {
        bottom[serial % bottom.size()] = static_cast<uint32_t>(serial * 2654435761u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        host.poll(monotonic_millis());
    };
    for (uint64_t serial = 1; serial <= 6; ++serial) {
        publish(serial);
        spin(host, companion, [&] { return companion.newest_frame_serial_for_test() == serial; },
             500);
    }

    // The last frame before the pause, which the companion never hears of.
    publish(7);
    // Paused: the same picture, over and over. Nothing to send yet.
    const uint64_t queued = host.queued_bytes_for_test();
    publish(7);
    CHECK_EQ(host.queued_bytes_for_test(), queued);

    // Still paused, and past the point an ack was coming: sent again.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    publish(7);
    CHECK_MSG(host.queued_bytes_for_test() != queued,
              "a frame lost as the game paused was not sent again until it moved");

    host.stop();
}

// Frames lost in a wifi blip are noticed as soon as the companion says it
// never got them, not only once a timeout runs out. The companion keeps
// repeating its newest ack while nothing new arrives; one that still does not
// cover frames sent well before it means they are gone. Measured on a Switch
// Lite: every blip longer than the host's few-frame window froze the picture
// for most of half a second after the link was back, waiting on the timeout.
void test_frames_lost_in_a_blip_are_noticed_before_the_timeout() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) return;
    set_shared(host, "vertical", false, AudioTarget::Both);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    const auto publish = [&](uint64_t serial) {
        bottom[serial % bottom.size()] = static_cast<uint32_t>(serial * 2654435761u);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        host.poll(monotonic_millis());
        companion.poll(monotonic_millis());
    };
    uint64_t serial = 1;
    for (; serial <= 6; ++serial) {
        publish(serial);
        spin(host, companion, [&] { return companion.newest_frame_serial_for_test() == serial; },
             500);
    }

    // The blip: nothing reaches the companion, which goes on answering.
    companion.drop_frames_for_test(true);
    const uint64_t started = monotonic_millis();
    uint64_t queued = host.queued_bytes_for_test();
    bool shut = false;
    for (int attempt = 0; attempt < 64 && !shut; ++attempt, ++serial) {
        publish(serial);
        shut = host.queued_bytes_for_test() == queued;
        queued = host.queued_bytes_for_test();
    }
    CHECK_MSG(shut, "the window never filled, so there is nothing to recover from");
    companion.drop_frames_for_test(false);

    // Back again. Frames flow within a few repeats of the ack, well inside
    // the timeout that is the only other way out.
    bool flowing = false;
    while (!flowing && monotonic_millis() - started < 350) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        publish(serial++);
        flowing = host.queued_bytes_for_test() != queued;
    }
    CHECK_MSG(flowing, "frames lost in a blip waited out the whole timeout");
    const uint64_t recovered = serial - 1;
    CHECK_MSG(spin(host, companion,
                   [&] { return companion.newest_frame_serial_for_test() == recovered; }, 1000),
              "the companion never got a whole picture again after the blip");

    host.stop();
}

// The floor the ladder can step down to, which has to mean the same thing on
// a phone and on a television.
//
// Measured on an Apple TV: it walked down to the old floor, sat there spending
// 940ms of every second turning frames into pictures, and had nowhere further
// to go -- because half of a 3840-wide panel is 1920, which is the whole size
// of the picture being sent and exactly what it could not decode.
// A picture is late when it is late by more than this device could have
// noticed, not when it is late by half a frame.
//
// Measured on an Apple TV: a 30fps stream, nothing dropped, a 40ms round trip
// -- and the hold swinging between 33ms and 140ms for the whole session. Half
// a period there is 16ms and that device's loop comes round every 14 to 16, so
// a frame that arrived exactly on time was seen a tick later and counted as a
// stall. Cover bought, latency added, cover decayed, stall counted again: a
// sawtooth a hundred milliseconds deep with no stall underneath it.
void test_a_device_does_not_count_its_own_loop_as_the_link_stalling() {
    using namespace ds::net;

    // 30fps, and a loop that comes round about as often as the old deadline.
    // The lateness that counts has to be beyond what that loop can see.
    const uint64_t period = 33;
    CHECK(lateness_that_counts_ms(period, 15) > 15);
    CHECK(lateness_that_counts_ms(period, 15) == period / 2 + 15);

    // A loop quick enough to see the deadline is still held to it: nothing is
    // given away on a device that can keep up.
    CHECK(lateness_that_counts_ms(period, 1) == period / 2 + 1);
    CHECK(lateness_that_counts_ms(period, 0) == period / 2);

    // At 60fps the period halves and so does the deadline, but the loop's own
    // granularity is whatever it is and still has to be allowed for.
    CHECK(lateness_that_counts_ms(16, 15) > 15);

    // And it never falls below the old deadline, which would make a device
    // fussier than it was rather than less fussy.
    for (const uint64_t gap : {uint64_t{0}, uint64_t{1}, uint64_t{8}, uint64_t{15}}) {
        CHECK(lateness_that_counts_ms(period, gap) >= period / 2);
    }
}

void test_the_smallest_picture_is_a_size_rather_than_a_fraction() {
    using namespace ds::net;

    // A 4K television. The old floor was 1920 across; the new one is a small
    // picture rather than a large fraction.
    const double tv = lowest_display_scale(3840);
    CHECK_MSG(3840 * tv <= 700.0,
              "a 4K panel could not ask for anything smaller than " +
                  std::to_string(static_cast<int>(3840 * tv)) + " across");
    CHECK(tv < kMinDisplayScale);

    // Nothing loses the headroom it had. A phone and a 720p panel can still
    // halve, which is what they could always do.
    CHECK(lowest_display_scale(1920) <= kMinDisplayScale);
    CHECK(lowest_display_scale(1280) <= kMinDisplayScale);
    // And a panel narrower than two of the smallest pictures is still bound by
    // the fraction, not raised by the pixels.
    CHECK(lowest_display_scale(800) == kMinDisplayScale);
    CHECK(lowest_display_scale(320) == kMinDisplayScale);
    // A size nobody has yet is not a reason to divide by zero.
    CHECK(lowest_display_scale(0) == kMinDisplayScale);

    // The floor is a floor: stepping down from it repeatedly stays on it.
    double scale = 1.0;
    for (int step = 0; step < 40; ++step) {
        scale = std::max(tv, scale * kDisplayScaleStep);
    }
    CHECK(scale == tv);
}

// Whether stepping down again is worth it, which the ladder never used to ask.
//
// Its premise is that the work is proportional to the pixel count. On an Apple
// TV it is not: measured across four rungs, 3.8 times fewer pixels bought 1.17
// times less work, because the cost there is per frame -- a synchronous
// hardware decode and a lock on the decoder's buffer, neither of which cares
// how big the picture is. Without this the ladder walks to its floor,
// degrading the picture the whole way, and arrives no quicker.
void test_a_step_that_bought_nothing_is_not_taken_twice() {
    using namespace ds::net;

    // The first step has nothing to compare against, so it is always tried.
    CHECK(shrinking_is_worth_it(0.0, 27.0));

    // A device where the picture size does decide the cost: a third off the
    // pixels, a third off the time. Keep going.
    CHECK(shrinking_is_worth_it(30.0, 20.0));

    // The Apple TV, one rung at a time. Its log reported only per-second
    // totals, which is why the line now carries the per-frame cost too --
    // these are those totals standing in for a frame count that did not
    // change much between neighbouring windows.
    CHECK(!shrinking_is_worth_it(938.0, 930.0));  // 51% -> 41%: 0.9% quicker
    CHECK(!shrinking_is_worth_it(930.0, 896.0));  // 41% -> 33%: 3.7% quicker
    // The judgement is made on the first step, so the ladder stops there
    // rather than walking to the floor as it did.

    // But only when the step made the picture smaller. On a 4K panel the
    // first rungs are 3072 and 2456 wide and a Wii U game draws 1920, so the
    // host sends the same 1920 either way: the step asked for less and
    // changed nothing. Judging that as "size does not matter" froze an Apple
    // TV at the one size it could not manage -- 19 frames a second and a
    // 350ms round trip, where letting the ladder run had reached 30 frames a
    // second and 34ms. The width the pictures actually arrive at is what the
    // comparison has to be made against, and that is checked in
    // adapt_to_what_this_device_can_decode() rather than here.

    // A step that made things worse is not a reason to take another.
    CHECK(!shrinking_is_worth_it(20.0, 25.0));
    // Exactly a tenth better counts, a hair under does not.
    CHECK(shrinking_is_worth_it(10.0, 9.0));
    CHECK(!shrinking_is_worth_it(10.0, 9.5));
}

// Only a step that really made the picture smaller can say whether size
// matters.
//
// Nintendo Land draws 1280 wide. On a 3840 panel the rungs ask for 3072, 2457,
// 1958 and 1574, all above it, so the host sent 1280 each time -- and then
// 1267, one percent narrower. Judged as a real step, the cost barely moved
// (27ms to 26ms), "size does not matter" was concluded, and a 60fps game was
// left at 26ms a frame: a second and a half of work per second, unplayable.
// Wind Waker's rungs were real (1920, 1568, 1248, 992) and reached 12ms.
void test_a_sliver_narrower_is_not_a_step() {
    using namespace ds::net;

    // The step that froze Nintendo Land.
    CHECK(!picture_shrank_enough_to_judge(1280, 1267));
    // The step that would have come next, and would have told the truth.
    CHECK(picture_shrank_enough_to_judge(1267, 998));

    // Wind Waker's rungs, every one of them a real step.
    CHECK(picture_shrank_enough_to_judge(1920, 1568));
    CHECK(picture_shrank_enough_to_judge(1568, 1248));
    CHECK(picture_shrank_enough_to_judge(1248, 992));

    // A rung of the ladder's own size always clears the bar, so nothing that
    // is genuinely a step is ever passed over.
    for (const int width : {640, 1000, 1280, 1920, 3840}) {
        const int stepped = static_cast<int>(width * kDisplayScaleStep) & ~1;
        CHECK(picture_shrank_enough_to_judge(width, stepped));
    }

    // Nothing to compare is not a shrink, and neither is growing.
    CHECK(!picture_shrank_enough_to_judge(0, 1280));
    CHECK(!picture_shrank_enough_to_judge(1280, 0));
    CHECK(!picture_shrank_enough_to_judge(1280, 1280));
    CHECK(!picture_shrank_enough_to_judge(992, 1248));
}

// The ladder steps from the size that is arriving, not the size it asked for.
//
// An Apple TV's panel is 3840 wide and Wind Waker draws 1920. Stepping from
// the ask, the first three rungs (3072, 2457, 1966) were all above what the
// game draws, so the host sent 1920 through all three -- three seconds of
// pictures at 58ms apiece before the first rung that did anything.
void test_the_ladder_steps_from_what_arrives() {
    using namespace ds::net;

    // Wind Waker on the 4K panel: half of it, whatever was asked for.
    CHECK(scale_the_picture_arrived_at(1920, 1080, 3840, 2160) == 0.5);
    // So the first step is one the host has to take.
    const int first_rung = static_cast<int>(3840 * 0.5 * kDisplayScaleStep) & ~1;
    CHECK_MSG(first_rung < 1920, "the first step from what arrives is still above what arrives");
    // Where stepping from the ask started, for contrast.
    CHECK(static_cast<int>(3840 * 1.0 * kDisplayScaleStep) > 1920);

    // Nintendo Land draws 1280: a third of the panel.
    CHECK(std::abs(scale_the_picture_arrived_at(1280, 720, 3840, 2160) - 1.0 / 3.0) < 1e-9);

    // A 4:3 picture fitted to a 16:9 box fills it top to bottom, and that is
    // the side that says how much of the box it is using. Taking the width
    // would read a picture already at full size as a quarter short of it,
    // and step a whole extra rung down for nothing.
    CHECK(scale_the_picture_arrived_at(2880, 2160, 3840, 2160) == 1.0);

    // A picture the box is binding is at the ask, less the host's rounding
    // to 16 -- which the ladder must not mistake for a smaller picture.
    CHECK(scale_the_picture_arrived_at(992, 558, 998, 560) > 0.99);

    // Nothing to go on is not a size.
    CHECK(scale_the_picture_arrived_at(0, 1080, 3840, 2160) == 0.0);
    CHECK(scale_the_picture_arrived_at(1920, 1080, 0, 2160) == 0.0);
}

// A step is judged on its own pictures, and only once they are arriving.
//
// The window after a step used to hold a mixture: a few pictures still at the
// old size, then the new ones. 29ms a picture at 998 wide was really three at
// 1574 and the rest at 13ms, and the ladder stepped down again on the strength
// of it -- to a size well below the one that worked.
void test_a_step_is_judged_on_the_size_it_asked_for() {
    using namespace ds::net;

    // Down and up are both the size a step asked for.
    CHECK(picture_size_changed_enough(1920, 1536));
    CHECK(picture_size_changed_enough(992, 1248));
    // Rounding is neither.
    CHECK(!picture_size_changed_enough(998, 992));
    CHECK(!picture_size_changed_enough(992, 998));
    CHECK(!picture_size_changed_enough(1280, 1280));
    // And nothing to compare is not a change.
    CHECK(!picture_size_changed_enough(0, 1280));

    // Long enough for a host to rebuild its encoder and send a keyframe, and
    // short enough that a step the host never took is not waited on for ever.
    CHECK(kAwaitNewSizeMs >= 1000);
    CHECK(kAwaitNewSizeMs <= 5000);
}

// Cover is bought for the link, not for the game.
//
// A picture the game drew late arrives as promptly as any other, just later,
// and no buffer on this end could have shown it any sooner: it did not exist.
// Buying cover for it only adds the same delay to every picture after it --
// which is what Wind Waker did, running the cover to its ceiling for eight
// seconds at a stretch.
void test_a_picture_the_host_sent_late_buys_no_cover() {
    using namespace ds::net;

    const uint64_t period = 33;
    // Held up on the way: the link's, and worth covering.
    CHECK(lateness_was_the_links(40.0, period));
    CHECK(lateness_was_the_links(17.0, period));
    // Got here as quickly as ever: late because it was sent late.
    CHECK(!lateness_was_the_links(0.0, period));
    CHECK(!lateness_was_the_links(5.0, period));
    CHECK(!lateness_was_the_links(15.0, period));
    // Nothing to measure it by: counted as it always was.
    CHECK(lateness_was_the_links(-1.0, period));

    // How far behind the quietest arrival a transit is, which is where that
    // number comes from. The offset between the two clocks is in every
    // transit and cancels.
    VideoJitter jitter;
    CHECK(jitter.above_quietest(500.0) < 0.0);
    for (const double transit : {1000.0, 1004.0, 1002.0, 1010.0}) {
        jitter.add(transit);
    }
    CHECK(jitter.above_quietest(1000.0) == 0.0);
    CHECK(jitter.above_quietest(1040.0) == 40.0);
    // Quicker than any in the window is not negative -- negative is "unknown".
    CHECK(jitter.above_quietest(990.0) == 0.0);

    // And the log says which end the late pictures were at, or nothing.
    CHECK(late_pictures_summary(0, 0).empty());
    const std::string both = late_pictures_summary(2, 5);
    CHECK_MSG(both.find("2 held up on the way") != std::string::npos, both);
    CHECK_MSG(both.find("5 sent late by the host") != std::string::npos, both);
}

// A second in which the host held pictures back is not room to spare.
//
// Measured on a Switch: the host, finding it behind, sent 21 of every 60
// pictures. 21 pictures at 9ms is under a fifth of a second of work, so the
// ladder read it as room and asked for 854x480 again -- which it had just
// stepped down from, and which made the stall it was in last longer. Four
// times in one session.
void test_a_stream_the_host_held_back_is_not_room_to_spare() {
    using namespace ds::net;

    // That second: 60 serials, 21 pictures.
    CHECK(host_held_pictures_back(60, 21));
    // A whole stream, and one with the odd picture dropped in passing.
    CHECK(!host_held_pictures_back(60, 60));
    CHECK(!host_held_pictures_back(60, 58));
    // A tenth missing is where it starts to count.
    CHECK(host_held_pictures_back(60, 53));
    // Nothing moved: a paused game or a still screen, not a held-back one.
    CHECK(!host_held_pictures_back(0, 0));
    // A 30fps game is its own full rate, and is not mistaken for half of 60.
    CHECK(!host_held_pictures_back(30, 30));
}

void test_host_can_disconnect_the_companion() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Ender";
    config.game_title = "Ending";

    std::string error;
    if (!host.start(config, &error)) return;

    CompanionSession companion;
    if (!companion.begin_search("Ended", ScreenId::Bottom, &error)) {
        host.stop();
        return;
    }
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        host.stop();
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        host.stop();
        return;
    }

    host.disconnect_companion("you ended the session");
    CHECK(!host.has_companion());
    // The companion is told rather than left waiting for a timeout.
    const bool noticed = spin(host, companion, [&] {
        return companion.status() != CompanionSession::Status::Connected;
    });
    CHECK_MSG(noticed, "the companion was not told the session ended");

    host.stop();
}

// Changing the codec while a session is live has to take effect on that
// session, not on the next one. The whole reason the setting exists is to see
// what the other answer looks like on your own machines, and a setting you have
// to reconnect to feel is a setting you cannot compare with.
//
// The hard direction is video back to tiles: a tiled frame is a difference from
// the last tiled frame, and the frames in between went a different way
// entirely. Both ends have to still be describing the same picture afterwards.
void test_the_codec_can_change_without_reconnecting() {
    if (!video::have_ffmpeg()) {
        test::skip("this build has no video codec");
        return;
    }

    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }
    CHECK_MSG(host.companion_accepts_video(),
              "the far end cannot decode video, so this run says nothing about switching to it");

    // Each frame is a different flat colour, so what arrived is checkable
    // without caring how it got here -- and so that switching codecs cannot be
    // hidden by the picture happening not to change.
    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    uint64_t serial = 1;
    const auto send_and_wait = [&](uint32_t colour) {
        const std::vector<uint32_t> bottom = flat_screen(colour);
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
        const bool arrived = spin(host, companion, [&] {
            return companion.screen_pixels(ScreenId::Bottom).serial == serial;
        });
        ++serial;
        return arrived;
    };
    // How far off the arrived colour is. Both codecs are lossy, so this asks
    // for close rather than equal -- but a frame that never updated is not
    // close, which is the failure being looked for.
    const auto difference_from = [&](uint32_t colour) {
        const CompanionSession::ScreenPixels frame = companion.screen_pixels(ScreenId::Bottom);
        if (!frame.valid()) return 255;
        int worst = 0;
        for (int index = 0; index < frame.width * frame.height; ++index) {
            for (int shift : {16, 8, 0}) {
                const int got = static_cast<int>((frame.pixels[index] >> shift) & 0xffu);
                const int want = static_cast<int>((colour >> shift) & 0xffu);
                worst = std::max(worst, std::abs(got - want));
            }
        }
        return worst;
    };

    // Tiles first, which is where a DS session starts.
    CHECK_MSG(send_and_wait(0x00801020u), "the first frame never arrived");
    CHECK_MSG(!companion.screen_is_video_for_test(ScreenId::Bottom),
              "the session started on video, so switching to it proves nothing");
    CHECK_MSG(difference_from(0x00801020u) <= 12, "the first frame arrived wrong");

    // Turned on mid-session.
    host.set_video_wanted(true);
    CHECK_MSG(send_and_wait(0x0020c040u), "no frame arrived after asking for video");
    CHECK_MSG(companion.screen_is_video_for_test(ScreenId::Bottom),
              "asking for video mid-session did nothing, so it only takes effect on reconnecting");
    CHECK_MSG(difference_from(0x0020c040u) <= 12, "the first video frame arrived wrong");

    // And off again. This is the one that can go quietly wrong: the tile codec
    // sends only what changed since the last tiled frame, and several video
    // frames have been shown since that one.
    host.set_video_wanted(false);
    CHECK_MSG(send_and_wait(0x00f0a010u), "no frame arrived after turning video off");
    CHECK_MSG(!companion.screen_is_video_for_test(ScreenId::Bottom),
              "turning video off mid-session did nothing");
    CHECK_MSG(difference_from(0x00f0a010u) <= 12,
              "the picture did not survive going back to tiles: what arrived was a difference "
              "from a frame the far end is no longer showing");

    companion.disconnect();
    host.stop();
}

// A screen streamed as H.264 is never sent any larger than the companion's
// own reported display -- see compute_capped_encode_size() in
// host_session.cpp. This is the session-level half of that: the encoder-level
// scaling itself is test_video_codec.cpp's job, this is confirming
// HostSession actually asks for it once it knows the companion's display is
// smaller than the screen being sent.
void test_video_is_capped_to_the_companions_display_size() {
    if (!video::have_ffmpeg()) {
        test::skip("this build has no video codec");
        return;
    }

    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");
    host.set_video_wanted(true);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }
    CHECK_MSG(host.companion_accepts_video(), "the far end cannot decode video");

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    const std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    uint64_t serial = 1;
    host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
    CHECK_MSG(spin(host, companion,
                   [&] { return companion.screen_pixels(ScreenId::Bottom).valid(); }),
              "no video frame arrived at all");
    const CompanionSession::ScreenPixels native = companion.screen_pixels(ScreenId::Bottom);
    CHECK_EQ(native.width, kDsScreenWidth);
    CHECK_EQ(native.height, kDsScreenHeight);

    // Smaller than the DS screen in both dimensions. 120x90 is deliberately
    // the same 4:3 shape the DS screen already is: both request_display_size
    // arguments below stay clean fractions of it (120/256 == 90/192 exactly
    // in binary floating point), so the width and height bounds land evenly
    // rather than one of them tripping over a rounding edge the aspect-ratio
    // check below would then have to be loose enough to absorb.
    constexpr int kDisplayWidth = 120;
    constexpr int kDisplayHeight = 90;
    companion.request_display_size(kDisplayWidth, kDisplayHeight);
    int reported_width = 0;
    int reported_height = 0;
    CHECK_MSG(spin(host, companion,
                   [&] { return host.companion_display_size(reported_width, reported_height); }),
              "the companion's display size never reached the host");

    ++serial;
    host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
    const bool shrank = spin(host, companion, [&] {
        const CompanionSession::ScreenPixels frame = companion.screen_pixels(ScreenId::Bottom);
        return frame.valid() && frame.width != native.width;
    });
    CHECK_MSG(shrank, "the stream was not capped down once the companion's display was smaller "
                      "than the screen being sent");
    const CompanionSession::ScreenPixels capped = companion.screen_pixels(ScreenId::Bottom);
    CHECK_MSG(capped.width <= kDisplayWidth && capped.height <= kDisplayHeight,
              "the capped frame (" + std::to_string(capped.width) + "x" +
                  std::to_string(capped.height) + ") still does not fit the companion's own " +
                  std::to_string(kDisplayWidth) + "x" + std::to_string(kDisplayHeight) +
                  " display");
    CHECK_MSG(capped.width < native.width && capped.height < native.height,
              "the cap did not actually shrink both dimensions of the screen being sent");
    // A multiple of 16, not just even -- see compute_capped_encode_size()'s
    // own comment on why a capped size lands there rather than only on an
    // even number the way an uncapped one only needs to.
    CHECK_MSG((capped.width % 16) == 0 && (capped.height % 16) == 0,
              "a capped H.264 frame (" + std::to_string(capped.width) + "x" +
                  std::to_string(capped.height) + ") was not macroblock-aligned");
    // Loose on purpose: rounding each dimension down to its own nearest
    // multiple of 16 does not distribute the same fraction of loss to both,
    // so a little aspect drift is expected at a target this small -- the
    // point of this check is that the cap scaled the picture down rather
    // than stretching it to fill the display's own shape regardless of the
    // source's, which a factor of two or more off would still catch.
    const double source_aspect =
        static_cast<double>(native.width) / static_cast<double>(native.height);
    const double capped_aspect =
        static_cast<double>(capped.width) / static_cast<double>(capped.height);
    CHECK_MSG(capped_aspect > source_aspect * 0.75 && capped_aspect < source_aspect * 1.25,
              "the capped frame's aspect ratio drifted from the source's");

    companion.disconnect();
    host.stop();
}

// A link that is delivering evenly must not hold anything back speculatively:
// every frame held is a frame of latency, and the old fixed depth of three was
// this app's biggest single source of it.
void test_a_steady_link_holds_the_smallest_buffer_it_can() {
    CHECK_EQ(video_buffer_target_frames(0.0, 33.0), kMinVideoBufferFrames);
    // Sub-frame unevenness is what a clock already absorbs; it should not buy
    // a whole extra frame of picture.
    CHECK_EQ(video_buffer_target_frames(0.4, 33.0), kMinVideoBufferFrames + 1);
    // Degenerate inputs fall back to the floor rather than to something wild.
    CHECK_EQ(video_buffer_target_frames(5.0, 0.0), kMinVideoBufferFrames);
    CHECK_EQ(video_buffer_target_frames(-5.0, 33.0), kMinVideoBufferFrames);
}

void test_an_uneven_link_buys_cover_and_gives_it_back() {
    // Cover grows with the unevenness actually seen...
    const std::size_t calm = video_buffer_target_frames(1.0, 33.0);
    const std::size_t rough = video_buffer_target_frames(50.0, 33.0);
    CHECK(rough > calm);
    // ...but never past the ceiling, however bad it gets.
    CHECK_EQ(video_buffer_target_frames(10000.0, 33.0), kMaxVideoBufferFrames);
    CHECK(video_buffer_target_frames(50.0, 33.0) <= kMaxVideoBufferFrames);

    // And the decay the session applies to its jitter estimate has to actually
    // bring the buffer back down, or a single hiccup ratchets the latency up
    // for the rest of the session -- which is exactly the bug this replaced.
    double jitter = 90.0;
    CHECK_EQ(video_buffer_target_frames(jitter, 33.0), kMaxVideoBufferFrames);
    for (int frame = 0; frame < 300; ++frame) {
        jitter *= 0.99;  // kJitterDecayPerFrame
    }
    CHECK_EQ(video_buffer_target_frames(jitter, 33.0), kMinVideoBufferFrames + 1);
}

// The bitrate is a budget for the wire, not a quality target for a frame.
//
// It used to be bits per pixel per frame times an assumed 60, which on a 30fps
// game came out at 43.5 Mbit/s for 1080p -- and the ask is a ceiling, so a
// ceiling twice as high as the content wanted bound on nothing at all. The
// encoder was honouring it the whole time (asked 22.9 it made 23.0, asked 22.7
// it made 23.5); it was simply never asked for less than it already wanted.
void test_the_bitrate_is_a_budget_for_the_link() {
    // 1080p lands about where this stream was already running, which is what
    // it looked right at. Cutting it to half that was tried for a session: it
    // cost picture and moved the tail of late arrivals by 0.8ms.
    const int at_1080p = bitrate_kbps_for_encode_size(1920, 1080, 30.0);
    CHECK(at_1080p > 20000 && at_1080p < 28000);

    // And it does not depend on the frame rate, because a link budget is the
    // same budget whether thirty pictures are drawn into it or sixty. That is
    // the assumption that made the old number wrong, so there is now none to
    // get wrong.
    CHECK_EQ(bitrate_kbps_for_encode_size(1920, 1080, 60.0), at_1080p);
    CHECK_EQ(bitrate_kbps_for_encode_size(1920, 1080, 0.0), at_1080p);

    // A smaller picture asks for less, but not proportionally less: the same
    // blockiness costs more bits per pixel as the picture shrinks, so the
    // floor holds the small end up rather than the slope starving it.
    const int gamepad = bitrate_kbps_for_encode_size(854, 480, 30.0);
    CHECK(gamepad < at_1080p);
    CHECK(gamepad >= 4000);

    // And nothing is asked for a rate no link here could carry.
    CHECK(bitrate_kbps_for_encode_size(7680, 4320, 60.0) <= 40000);
}

// What the pacer drains at has to follow what the encoder really produces,
// because that is not what it was asked to produce.
//
// Measured on a Steam Deck's h264_vaapi: asked for 43.5 Mbit/s it made 20.1,
// asked for 22.9 it made 23.0, asked for 34.6 it made 21.5, asked for 22.7 it
// made 23.5. The request is a suggestion there. Pacing from it halved the
// budget while the traffic stayed put, and the pacing queue -- pure latency
// this end adds -- went from 14.6ms average to 34.9ms with spikes to 159.
void test_the_pacer_follows_what_is_really_produced() {
    // 23 Mbit/s of real traffic is 2875 bytes a millisecond.
    constexpr double produced = 2875.0;

    // Asked for half of what it makes, the pacer still keeps up with what it
    // makes -- which is the case that regressed.
    const double paced_low = media_pacing_bytes_per_ms(22922, produced);
    CHECK(paced_low > produced * 2.0);

    // Asked for twice what it makes, the pacer does not run away to match the
    // request either: nothing is sent that was not produced, so a budget built
    // on the bigger number is only ever unused.
    CHECK(std::abs(media_pacing_bytes_per_ms(43458, produced) - paced_low) < 1.0);

    // Before anything has been produced to measure, the declared rate stands
    // in -- a stream has to start somewhere.
    CHECK(media_pacing_bytes_per_ms(22922, 0.0) > 0.0);
    CHECK(media_pacing_bytes_per_ms(43458, 0.0) >
          media_pacing_bytes_per_ms(22922, 0.0));

    // And a screen that compresses to almost nothing -- a DS with a static
    // picture -- still gets a floor to send through, so one small frame does
    // not take a second to leave.
    CHECK(media_pacing_bytes_per_ms(0, 1.0) > 100.0);
}

// The window is a bandwidth-delay product, and the point of computing it
// rather than picking it is that the delay is not a constant.
// A frame is taken for lost after a few of this link's own round trips, not
// after a fixed three seconds -- which, after a one-second wifi dropout, was
// two more seconds of frozen picture than the dropout itself.
void test_a_lost_frame_is_given_up_on_in_round_trips() {
    // Nothing measured yet: the full, cautious wait.
    CHECK_EQ(ack_stall_ms(0.0, false), uint64_t{3000});
    CHECK_EQ(ack_stall_ms(45.0, false), uint64_t{3000});
    // A 45ms link: well under a second, but never below the floor that keeps
    // a device's slow moment from reading as a loss.
    CHECK(ack_stall_ms(45.0, true) < uint64_t{1000});
    CHECK(ack_stall_ms(45.0, true) >= uint64_t{400});
    CHECK_EQ(ack_stall_ms(10.0, true), uint64_t{400});
    // A slow link waits longer, and never longer than before.
    CHECK(ack_stall_ms(200.0, true) > ack_stall_ms(45.0, true));
    CHECK_EQ(ack_stall_ms(5000.0, true), uint64_t{3000});
}

void test_the_in_flight_window_covers_the_delay_this_link_shows() {
    constexpr double at_60fps = 16.7;

    // Where it came in: a 40ms round trip at 60fps. Four, which is the number
    // that was hardcoded -- so a good link behaves exactly as it did.
    CHECK_EQ(frames_in_flight_allowed(40, at_60fps), std::size_t{4});

    // And the case that was costing frames. A Steam Deck on wifi averages
    // 40ms with a tail past 110ms; at four frames every excursion past
    // 4 * 16.7 = 66ms shuts the gate, which is where 60 produced and 15 sent
    // came from. The window has to be bigger than the product, not equal to
    // the average of it.
    CHECK(frames_in_flight_allowed(110, at_60fps) > frames_in_flight_allowed(40, at_60fps));
    CHECK(frames_in_flight_allowed(110, at_60fps) >= std::size_t{8});

    // A round trip this far out is congestion, and matching it would add to
    // it. Bounded, and bounded again by the frame count itself.
    CHECK_EQ(frames_in_flight_allowed(1000, at_60fps), frames_in_flight_allowed(120, at_60fps));
    CHECK(frames_in_flight_allowed(100000, 1.0) <= std::size_t{8});

    // A game producing more slowly needs fewer frames to cover the same
    // delay, which is why the rate is measured rather than assumed. Thirty a
    // second over the same 40ms link:
    CHECK(frames_in_flight_allowed(40, 33.3) < frames_in_flight_allowed(40, at_60fps));

    // Never below a floor: one frame in flight would mean every frame waiting
    // on the previous one's ack, which is the frame-rate ceiling this whole
    // thing exists to avoid.
    CHECK_EQ(frames_in_flight_allowed(0, at_60fps), std::size_t{2});
    CHECK_EQ(frames_in_flight_allowed(40, 0.0), std::size_t{2});
    CHECK_EQ(frames_in_flight_allowed(40, -1.0), std::size_t{2});
}

// The link the Steam Deck actually provides: mostly even, with one bad moment
// a second. Sizing the buffer from the worst of those pins it at the ceiling
// for the whole session; sizing it from what the link does nearly all the time
// costs a hitch on the bad moments and nothing the rest of the time.
void test_one_bad_moment_a_second_does_not_buy_a_whole_session_of_buffer() {
    constexpr double period = 20.0;
    // Transit times, so every one of them carries the clocks' offset. It is
    // deliberately large and deliberately negative here: nothing downstream
    // may depend on the two machines agreeing about the time, or on which of
    // them is ahead.
    constexpr double kClockOffset = -5000.0;

    VideoJitter quantile;
    double worst_with_decay = 0.0;
    for (int frame = 0; frame < 600; ++frame) {
        // A twelfth of a second is about one arrival in fifty at 50fps, which
        // is what a round trip averaging 34ms with a 190ms worst looks like.
        const double queued = (frame % 50 == 0) ? 120.0 : 4.0;
        quantile.add(kClockOffset + queued);
        worst_with_decay = std::max(queued, worst_with_decay * 0.99);
    }

    // What this used to do: never less than the worst of the last couple of
    // seconds, so never less than the ceiling.
    CHECK_EQ(video_buffer_target_frames(worst_with_decay, period), kMaxVideoBufferFrames);
    // What it does now: nothing at all. One arrival in fifty is two frames in
    // a hundred, which is inside the five this is willing to show a frame
    // late, so the bad moment is not bought cover for -- it is allowed to
    // hitch. That is the trade, stated plainly: the other ninety-eight frames
    // in the hundred are not delayed for it.
    CHECK(quantile.estimate() < 1.0);
    CHECK_EQ(video_buffer_target_frames(quantile.estimate(), period), kMinVideoBufferFrames);

    // A link that is uneven all the time still buys the cover it needs -- the
    // quantile is not a cap, it is a measurement, and it follows the link up
    // as readily as down. Alternating, because a constant transit is not an
    // uneven link at all however large it is: a link that always takes 70ms
    // is just a slow one, and needs no buffer.
    VideoJitter rough;
    for (int frame = 0; frame < 600; ++frame) {
        rough.add(kClockOffset + ((frame % 2 == 0) ? 0.0 : 70.0));
    }
    CHECK_EQ(video_buffer_target_frames(rough.estimate(), period), kMaxVideoBufferFrames);

    // And it gives all of it back once the link settles, within the window
    // rather than over a whole session.
    for (int frame = 0; frame < 200; ++frame) {
        rough.add(kClockOffset + 2.0);
    }
    CHECK_EQ(video_buffer_target_frames(rough.estimate(), period), kMinVideoBufferFrames);
}

// The point of the whole change: a source that renders unevenly is not an
// uneven link, and must not be paid for as one.
void test_an_emulator_rendering_unevenly_does_not_buy_buffer() {
    constexpr double period = 33.0;
    constexpr double kClockOffset = 1234.0;

    // Citra on a Steam Deck, swinging between 18 and 58 frames a second: the
    // gaps between arrivals are wild, but every frame crossed a quiet link in
    // the same 12ms, because the link was never the problem.
    VideoJitter link;
    for (int frame = 0; frame < 600; ++frame) {
        link.add(kClockOffset + 12.0);
    }
    CHECK(link.estimate() < 1.0);
    CHECK_EQ(video_buffer_target_frames(link.estimate(), period), kMinVideoBufferFrames);

    // Measured the old way -- from the spacing of arrivals against a smoothed
    // idea of the source's rate -- the same session reads as tens of
    // milliseconds of unevenness, and buys buffer that cannot help: those
    // frames were late when they were made.
    double smoothed_period = period;
    double worst_deviation = 0.0;
    for (int frame = 0; frame < 600; ++frame) {
        const double gap = (frame % 2 == 0) ? 17.0 : 55.0;  // 58fps, then 18fps
        worst_deviation = std::max(worst_deviation, std::abs(gap - smoothed_period));
        smoothed_period = smoothed_period * 0.95 + gap * 0.05;
    }
    CHECK(worst_deviation > 20.0);
    CHECK(video_buffer_target_frames(worst_deviation, period) > kMinVideoBufferFrames);
}

// A picture shown on time is not a stall, however empty the queue behind it.
//
// The first version of this counted "the clock came due and nothing was
// waiting", which at the least latency this can run at is the *normal* state:
// the clock comes due, the frame arrives a moment later, it is shown on time,
// and nothing is ever queued. That read as a stall on every frame -- thirty a
// second on a stream keeping up perfectly -- and drove the cover straight to
// its ceiling, which is the delay the whole mechanism exists not to buy.
void test_an_empty_queue_is_not_a_stall_if_the_picture_was_on_time() {
    constexpr uint64_t period = 33;
    // What the loop does with what a healthy stream really produces: nothing
    // late, so nothing bought, however often the queue was empty.
    CHECK_EQ(hold_extra_ms(0, period, 0, 1), static_cast<uint64_t>(0));
    // And what it did with the stream that was being misread: a stall counted
    // on every frame took it to its ceiling and left it there.
    uint64_t cover = 0;
    for (int second = 0; second < 20; ++second) {
        cover = hold_extra_ms(cover, period, 30, 0);
    }
    CHECK_EQ(cover, period * 2);
}

// The trade the quantile cannot settle: what to do about the tail.
//
// Measured, Steam Deck to Mac over wifi: half the frames land at the quietest
// arrival, three quarters within 19ms of it, and one in twenty 76ms behind.
// Cover the body and that one frame in twenty finds nothing waiting, so the
// picture stalls about once a second. Cover the tail and every frame waits
// 76ms for it. There is no quantile right for both, and the same numbers mean
// something else on different wifi.
//
// So the cover is not chosen, it is earned: a second in which the picture ran
// dry buys some, a long spell without one gives it back.
void test_cover_is_earned_by_running_dry_and_given_back_when_calm() {
    constexpr uint64_t period = 33;

    // A link that never stalls never buys anything.
    CHECK_EQ(hold_extra_ms(0, period, 0, 0), static_cast<uint64_t>(0));
    CHECK_EQ(hold_extra_ms(0, period, 0, 100), static_cast<uint64_t>(0));

    // One stall in a second is enough -- a player notices one a second, so
    // this does not wait to be convinced.
    CHECK(hold_extra_ms(0, period, 1, 0) > 0);

    // In quarter-frame steps, not whole ones. A link needing 43ms of cover
    // used to be handed 66, and every frame waited the 23ms of rounding.
    CHECK_EQ(hold_extra_ms(0, period, 1, 0), period / 4);
    uint64_t cover = 0;
    for (int second = 0; second < 6; ++second) {
        cover = hold_extra_ms(cover, period, 1, 0);
    }
    CHECK(cover >= 40 && cover <= 50);

    // And it stops. Past two periods on top of what the measurement can ask
    // for, a link that still runs dry will not be fixed by waiting longer.
    CHECK_EQ(hold_extra_ms(period * 2, period, 5, 0), period * 2);

    // Given back only after a calm spell, and a step at a time, so a link that
    // needed the cover does not lose all of it on one quiet stretch.
    CHECK_EQ(hold_extra_ms(period, period, 0, 2), period);
    CHECK_EQ(hold_extra_ms(period, period, 0, 3), period - period / 4);
    CHECK_EQ(hold_extra_ms(0, period, 0, 3), static_cast<uint64_t>(0));

    // What it is worth on the link that prompted it: the body asks for what
    // it measured, which does not reach a frame 76ms late; the earned cover
    // does, and without rounding up to a whole extra frame to get there.
    CHECK(video_hold_allowance_ms(19.0, period, 0) < 76u);
    CHECK(video_hold_allowance_ms(19.0, period, 57) >= 76u);
    CHECK(video_hold_allowance_ms(19.0, period, 57) < video_hold_allowance_ms(19.0, period, 66));
}

// Cover that can only be bought is not cover, it is a ratchet.
//
// Measured on an Apple TV: a rough patch bought 73ms, the stream went back to
// 30 frames a second with nothing dropped and a 35ms round trip, and the cover
// sat at 73ms for the rest of the session. The give-back needed ten
// *consecutive* calm seconds, and one bad second every dozen or so put the
// count back to zero before it ever got there. Ten is not a wait, it is a
// streak, and a real link does not supply streaks.
void test_cover_comes_back_on_a_link_that_twitches_now_and_then() {
    constexpr uint64_t period = 33;

    // Earn it the way that Apple TV did.
    uint64_t cover = 0;
    for (int second = 0; second < 12; ++second) {
        cover = hold_extra_ms(cover, period, 2, 0);
    }
    CHECK(cover >= 60);
    const uint64_t earned = cover;

    // Now a link that is calm most of the time and twitches every eighth
    // second. Under the old streak this never gave a penny back.
    unsigned calm = 0;
    for (int second = 0; second < 80; ++second) {
        const unsigned starved = (second % 8 == 7) ? 1u : 0u;
        cover = hold_extra_ms(cover, period, starved, calm);
        calm = (starved == 0) ? calm + 1 : 0;
    }
    CHECK_MSG(cover < earned,
              "cover never came back on a link that was calm seven seconds in eight");
    // Down to a single step, which is the right answer rather than zero: a
    // link that stalls once every eight seconds does want a little cover, and
    // one step is what one stall a cycle earns. What it must not do is keep
    // the 66ms it bought during the rough patch.
    CHECK_MSG(cover <= period / 4,
              "cover was still " + std::to_string(cover) + "ms after eighty mostly-calm seconds");

    // And a link that really is bad still keeps what it bought: the twitch is
    // what must not ratchet, not a genuine stall.
    uint64_t bad = 0;
    for (int second = 0; second < 40; ++second) {
        bad = hold_extra_ms(bad, period, 3, 0);
    }
    CHECK_EQ(bad, period * 2);
}

// Cover is only worth buying for lateness the link caused.
//
// The case this was written for, measured on a Mac drawing a DS game: 120
// frames a second arriving, so a period of 8ms and a "late" deadline of 4ms,
// against a loop whose quickest pass all session was 9ms. The loop could not
// resolve the deadline even at its best, so every miss was its own -- and the
// stream bought 34ms of cover against a link whose worst one-in-twenty arrival
// was 11ms.
void test_a_clock_finer_than_the_loop_buys_no_cover() {
    // The measured case: 8ms period, 9ms quickest pass. Nothing to be bought.
    CHECK(!lateness_is_worth_covering(8, 9));
    // And it is the deadline that decides it, not the period: at a 16ms period
    // the deadline is 8ms, which a 9ms loop still cannot resolve.
    CHECK(!lateness_is_worth_covering(16, 9));
    // A 30fps stream on the same device has 16ms of deadline against a 9ms
    // pass, so a late frame there really is the link's and is worth covering.
    CHECK(lateness_is_worth_covering(33, 9));
    CHECK(lateness_is_worth_covering(33, 16));
    // Exactly on the line counts, since the loop can just resolve it.
    CHECK(lateness_is_worth_covering(20, 10));
    CHECK(!lateness_is_worth_covering(20, 11));
    // Nothing measured yet is not a reason to stop counting -- a session that
    // has only just started has no quickest pass to compare against.
    CHECK(lateness_is_worth_covering(8, 0));
    CHECK(lateness_is_worth_covering(33, 0));
}

// The setting that decides how much of that trade a player wants made for
// them. Everything above is the Balanced end, which is what the app did before
// the setting existed; these are the other two.
void test_the_hold_setting_moves_both_ends_of_the_trade() {
    using ds::video::VideoHold;
    constexpr uint64_t period = 33;

    // Balanced is exactly what it was, so the setting's default changed
    // nothing about a stream nobody has touched.
    CHECK_EQ(video_hold_allowance_ms(19.0, period, 44, VideoHold::Balanced),
             video_hold_allowance_ms(19.0, period, 44));
    CHECK_EQ(hold_extra_ms(0, period, 1, 0, VideoHold::Balanced), hold_extra_ms(0, period, 1, 0));

    // Responsive never buys cover, however badly the picture runs dry.
    CHECK_EQ(hold_extra_ms(0, period, 30, 0, VideoHold::Responsive), static_cast<uint64_t>(0));
    uint64_t cover = 0;
    for (int second = 0; second < 20; ++second) {
        cover = hold_extra_ms(cover, period, 5, 0, VideoHold::Responsive);
    }
    CHECK_EQ(cover, static_cast<uint64_t>(0));

    // And it holds no more than one frame period whatever the spread says,
    // so the clock still paces but nothing is ever waited on beyond that.
    // Cover left over from a previous setting is not applied either.
    CHECK_EQ(video_hold_allowance_ms(0.0, period, 0, VideoHold::Responsive),
             video_hold_floor_ms(period));
    CHECK_EQ(video_hold_allowance_ms(500.0, period, 0, VideoHold::Responsive), period);
    CHECK_EQ(video_hold_allowance_ms(19.0, period, 44, VideoHold::Responsive), 19u);

    // Smooth may buy twice what Balanced may, and no more.
    CHECK_EQ(hold_extra_ms(period * 2, period, 5, 0, VideoHold::Smooth), period * 2 + period / 4);
    CHECK_EQ(hold_extra_ms(period * 4, period, 5, 0, VideoHold::Smooth), period * 4);
    CHECK(hold_extra_ms(period * 2, period, 5, 0, VideoHold::Smooth) >
          hold_extra_ms(period * 2, period, 5, 0, VideoHold::Balanced));

    // Which is the whole point of the setting: on the same link, the three
    // settings really do hold a frame for three different lengths of time,
    // in the order their names promise.
    const uint64_t responsive = video_hold_allowance_ms(19.0, period, 132, VideoHold::Responsive);
    const uint64_t balanced = video_hold_allowance_ms(19.0, period, 132, VideoHold::Balanced);
    const uint64_t smooth = video_hold_allowance_ms(19.0, period, 132, VideoHold::Smooth);
    CHECK(responsive < balanced && balanced < smooth);

    // The keys survive a round trip, so a chosen setting comes back after a
    // restart rather than quietly reverting to the default.
    for (VideoHold hold : {VideoHold::Responsive, VideoHold::Balanced, VideoHold::Smooth}) {
        CHECK(ds::video::video_hold_from_key(ds::video::video_hold_key(hold),
                                             VideoHold::Balanced) == hold);
    }
    CHECK(ds::video::video_hold_from_key("nonsense", VideoHold::Smooth) == VideoHold::Smooth);
    // And the cycle reaches all three and comes back round.
    VideoHold walked = VideoHold::Responsive;
    walked = ds::video::next_video_hold(walked);
    CHECK(walked == VideoHold::Balanced);
    walked = ds::video::next_video_hold(walked);
    CHECK(walked == VideoHold::Smooth);
    CHECK(ds::video::next_video_hold(walked) == VideoHold::Responsive);
}

// What the buffer is sized from is a trade, and this is where it is made.
//
// A link that is mostly prompt with a bad tail -- a Steam Deck to a Mac over
// wifi, measured: quiet most of the time, one arrival in twenty about 75ms
// behind the quietest -- can be covered at the tail or at the body. Covering
// the tail makes all twenty of those frames 75ms late to save the one from
// being shown a display interval behind. Covering the body leaves the tail to
// be late and hands the other nineteen back their time.
void test_the_buffer_pays_for_the_body_and_not_the_tail() {
    constexpr double kClockOffset = 777.0;
    constexpr double period = 33.0;  // a 30fps stream, as Wind Waker HD runs

    VideoJitter link;
    for (int frame = 0; frame < 128; ++frame) {
        // Nineteen in twenty cross in 12ms; the twentieth takes 87.
        link.add(kClockOffset + ((frame % 20 == 0) ? 87.0 : 12.0));
    }

    const VideoTransitShape shape = link.shape();
    // The shape says plainly what one number cannot: the body is tight and
    // only the tail is late. This is the line that now goes in the log.
    CHECK(shape.median_ms < 1.0);
    CHECK(shape.three_quarters_ms < 1.0);
    CHECK(shape.worst_twentieth_ms > 70.0);

    // So the buffer holds the least it can, and the one late frame in twenty
    // is shown when it arrives rather than every frame waiting for it.
    CHECK_EQ(video_buffer_target_frames(link.estimate(), period), kMinVideoBufferFrames);
    // Where the old choice of quantile would have bought three more frames of
    // it -- 99ms at this rate, paid on every frame there is.
    CHECK_EQ(video_buffer_target_frames(shape.worst_twentieth_ms, period),
             kMinVideoBufferFrames + 3);

    // A link that is genuinely late rather than occasionally late still buys
    // what it needs: this is cover for unevenness, not a decision to stop
    // covering.
    VideoJitter uneven;
    for (int frame = 0; frame < 128; ++frame) {
        uneven.add(kClockOffset + ((frame % 2 == 0) ? 5.0 : 70.0));
    }
    CHECK(uneven.estimate() > 60.0);
    CHECK(video_buffer_target_frames(uneven.estimate(), period) > kMinVideoBufferFrames);
}

// The same mistake at the other end of the wire: a stall on the device showing
// the picture reads as a link that held everything, and buys buffer that
// cannot help -- the queue drains at exactly the standstill the picture does.
//
// Measured on a Mac companion: its loop rate fell to 14-19 passes a second at
// times, and the jitter estimate sat at 570-1130ms for seconds afterwards on a
// link whose round trip averaged 20-30ms, pinning the buffer at its ceiling
// and holding every frame about 70ms.
void test_this_devices_own_stall_is_not_the_links_fault() {
    // The baseline is this device's own cadence, not the stream's. A Mac
    // companion comes round about every 14ms while showing a 33fps stream, so
    // measuring against the stream's period tolerated a 100ms pass -- seven
    // times this machine's normal one -- and read everything that piled up
    // during it as the link. That is the shape the far end then bought 66ms
    // of cover against.
    constexpr uint64_t typical = 14;  // a Mac companion, measured
    CHECK(poll_gap_measures_the_link(0, typical));
    CHECK(poll_gap_measures_the_link(typical, typical));
    CHECK(poll_gap_measures_the_link(typical * 2, typical));

    // Six times a normal pass is this device having been somewhere else.
    CHECK(!poll_gap_measures_the_link(75, typical));
    CHECK(!poll_gap_measures_the_link(600, typical));
    // And it would have sailed through the old rule, which is the hole: the
    // stream in question ran at 30fps, so three of its periods is 99ms.
    constexpr uint64_t thirty_fps_period = 33;
    CHECK(75u <= thirty_fps_period * 3);

    // A loop running very fast does not reject its own ordinary variation.
    CHECK(poll_gap_measures_the_link(4, 1));

    // Nothing to measure against before a cadence is known.
    CHECK(poll_gap_measures_the_link(600, 0));

    // And what it is worth: a window of quiet transits keeps the smallest
    // buffer there is, where letting one stall's worth in would pin it at the
    // ceiling for the rest of the window.
    constexpr double kClockOffset = 4321.0;
    VideoJitter without_the_stall;
    VideoJitter with_the_stall;
    for (int frame = 0; frame < 300; ++frame) {
        without_the_stall.add(kClockOffset + 12.0);
        with_the_stall.add(kClockOffset + 12.0);
    }
    // One pass that came 600ms late hands its whole backlog over at once --
    // at 60fps that is about forty frames, which is a third of the window
    // they land in, so they are a large enough share of it to move any
    // quantile the buffer could reasonably be sized from.
    for (int frame = 0; frame < 40; ++frame) {
        with_the_stall.add(kClockOffset + 12.0 + 600.0);
    }
    CHECK_EQ(video_buffer_target_frames(without_the_stall.estimate(), 16.0),
             kMinVideoBufferFrames);
    CHECK_EQ(video_buffer_target_frames(with_the_stall.estimate(), 16.0),
             kMaxVideoBufferFrames);
}

// Measured on hardware: the pacing clock shows one frame per period, which
// conserves the queue's depth exactly, so a backlog built during one hiccup
// became permanent latency -- a session climbed from showing frames on arrival
// to sitting a fixed 66ms behind and stayed there for its whole length. The
// allowance is what forces the clock to give that back.
void test_a_backlog_is_given_back_rather_than_carried_for_ever() {
    constexpr uint64_t period = 33;  // a 30fps stream, as these actually run

    // A calm link holds what its spread asks for and nothing more -- not the
    // whole frame this used to insist on, which on a calm link was the whole
    // of the delay the pacer added: frames settled wherever in the period the
    // clock's phase put them, up to a frame behind, and nothing pulled them
    // earlier. Only a floor of a few milliseconds is kept, so the clock is not
    // chasing noise. Never the two frames that were being carried either --
    // that second frame was bought by a hiccup long past and never given back.
    CHECK_EQ(video_hold_allowance_ms(0.0, period, 0), video_hold_floor_ms(period));
    CHECK_EQ(video_hold_allowance_ms(15.0, period, 0), 15u);
    CHECK_EQ(video_hold_allowance_ms(22.0, period, 0), 22u);

    // A link that really is uneven buys cover for as long as it stays uneven.
    CHECK_EQ(video_hold_allowance_ms(50.0, period, 0), 50u);
    CHECK(video_hold_allowance_ms(50.0, period, 0) > video_hold_allowance_ms(15.0, period, 0));

    // However wild the estimate gets, it can never talk its way back into
    // hoarding more than a couple of frames.
    CHECK_EQ(video_hold_allowance_ms(5000.0, period, 0), period * 2);
    CHECK(video_hold_allowance_ms(1e9, period, 0) == period * 2);
}

// A backlog has to be given back by running the clock slightly fast, never by
// showing an extra frame. Both frames of a two-frame pass land before anything
// is drawn, so the first is not shown at all -- and dropping frames several
// times a second is far more obvious than the delay it buys back. That was
// tried on hardware and the judder was the thing people noticed.
void test_a_backlog_is_drained_by_pacing_not_by_skipping_frames() {
    constexpr uint64_t period = 33;

    // Settled: exactly the frames shown, at exactly the period.
    CHECK_EQ(video_pace_step_ms(period, 1, false), period);
    CHECK_EQ(video_pace_step_ms(period, 2, false), period * 2);

    // Draining: the same frames, a little sooner. Never zero, and never so
    // aggressive that the picture visibly speeds up.
    const uint64_t drained = video_pace_step_ms(period, 1, true);
    CHECK(drained < period);
    CHECK(drained > period * 3 / 4);
    CHECK(video_pace_step_ms(1, 1, true) >= 1u);

    // Giving back a whole frame of delay should take on the order of half a
    // second -- slow enough to be invisible, quick enough to matter.
    const uint64_t gained = period - drained;
    CHECK(gained > 0);
    const uint64_t frames_to_recover = period / gained;
    CHECK(frames_to_recover >= 5);
    CHECK(frames_to_recover <= 30);
}

// The clock steps in fractions of a millisecond. In whole ones a 60fps
// stream's 16.67ms period rounded to 17, the clock ran two percent slow, and
// the queue grew by twenty milliseconds a second until a drain caught it --
// a sawtooth between one frame and four behind.
void test_the_pacing_clock_keeps_the_streams_own_rate() {
    const double period = 1000.0 / 60.0;
    double due = 0.0;
    for (int frame = 0; frame < 600; ++frame) {
        due += video_pace_step_exact_ms(period, 1, false);
    }
    // Ten seconds of frames is ten seconds, to well under a millisecond.
    CHECK(std::abs((due) - (10000.0)) < 0.01);
    CHECK(std::abs((video_pace_step_exact_ms(period, 2, false)) - (period * 2.0)) < 1e-9);
    // Draining is the same eight percent the whole-millisecond step gives.
    CHECK(std::abs((video_pace_step_exact_ms(period, 1, true)) - (period * 0.92)) < 1e-9);
    CHECK(std::abs((video_pace_step_exact_ms(period, 0, false)) - (period)) < 1e-9);

    // The least a frame is held for: a few milliseconds, never a frame.
    CHECK_EQ(video_hold_floor_ms(16), 2u);
    CHECK_EQ(video_hold_floor_ms(33), 4u);
    CHECK_EQ(video_hold_floor_ms(0), 2u);
    CHECK(video_hold_floor_ms(33) < 33u);
}

// A frame's worth of backlog, once it is there, stays there on a companion
// drawing at the game's own rate: one frame in and one out for ever. Letting
// one go unseen is the only way back, and how promptly is the setting's call.
void test_a_standing_backlog_is_let_go_as_promptly_as_the_setting_says() {
    using ds::video::VideoHold;
    constexpr uint64_t period = 17;
    constexpr uint64_t never = UINT64_MAX;

    // Responsive: the moment it is seen, however recently the last one was.
    CHECK(shed_a_frame_to_catch_up(0, never, period, VideoHold::Responsive));
    CHECK(shed_a_frame_to_catch_up(0, 0, period, VideoHold::Responsive));

    // Balanced: two frames of it first, and no more than four a second.
    CHECK(!shed_a_frame_to_catch_up(0, never, period, VideoHold::Balanced));
    CHECK(!shed_a_frame_to_catch_up(period, never, period, VideoHold::Balanced));
    CHECK(shed_a_frame_to_catch_up(period * 2, never, period, VideoHold::Balanced));
    CHECK(!shed_a_frame_to_catch_up(period * 2, 100, period, VideoHold::Balanced));
    CHECK(shed_a_frame_to_catch_up(period * 2, 250, period, VideoHold::Balanced));

    // Smooth: longer again, and half as often.
    CHECK(!shed_a_frame_to_catch_up(period * 2, never, period, VideoHold::Smooth));
    CHECK(shed_a_frame_to_catch_up(period * 6, never, period, VideoHold::Smooth));
    CHECK(!shed_a_frame_to_catch_up(period * 6, 250, period, VideoHold::Smooth));
    CHECK(shed_a_frame_to_catch_up(period * 6, 500, period, VideoHold::Smooth));
}

// The hardware encoder hands back the packet for an earlier submission than
// the one that just went in, which is what lets the send loop stop waiting on
// the GPU. None of that is reachable through the software encoder these tests
// run on -- it answers on the same call -- so the mapping that keeps a
// picture's serial honest across that delay is checked directly.
void test_a_pipelined_encoder_still_sends_each_picture_under_its_own_serial() {
    std::deque<std::pair<int64_t, uint64_t>> pending;
    uint64_t serial = 0;

    // A software encoder: in and out on the same call.
    pending.clear();
    pending.emplace_back(0, 100);
    CHECK(resolve_encoded_serial(pending, 0, serial));
    CHECK_EQ(serial, 100u);
    CHECK(pending.empty());

    // A pipelining one: three frames in, and the packet that arrives is the
    // first of them. It must be sent as serial 100, not as the 102 that went
    // in alongside it -- that is the whole bug this guards.
    pending.clear();
    pending.emplace_back(0, 100);
    pending.emplace_back(1, 101);
    pending.emplace_back(2, 102);
    CHECK(resolve_encoded_serial(pending, 0, serial));
    CHECK_EQ(serial, 100u);
    CHECK(resolve_encoded_serial(pending, 1, serial));
    CHECK_EQ(serial, 101u);
    CHECK_EQ(pending.size(), std::size_t{1});

    // A frame the encoder swallowed (a reset loses what it was holding) must
    // not strand everything behind it waiting for a packet that never comes.
    pending.clear();
    pending.emplace_back(7, 200);
    pending.emplace_back(8, 201);
    pending.emplace_back(9, 202);
    CHECK(resolve_encoded_serial(pending, 9, serial));
    CHECK_EQ(serial, 202u);
    CHECK(pending.empty());

    // A packet newer than the oldest frame outstanding means that oldest one
    // is never coming, so it goes rather than stranding the queue behind it.
    pending.clear();
    pending.emplace_back(5, 300);
    CHECK(!resolve_encoded_serial(pending, 6, serial));
    CHECK(pending.empty());

    // A packet older than anything still outstanding says nothing about any
    // moment in hand, so it is refused rather than sent under a guess -- and
    // must not disturb the frames that are still waiting.
    pending.clear();
    pending.emplace_back(5, 300);
    CHECK(!resolve_encoded_serial(pending, 4, serial));
    CHECK_EQ(pending.size(), std::size_t{1});

    // Nothing outstanding at all.
    pending.clear();
    CHECK(!resolve_encoded_serial(pending, 0, serial));
}

// The whole point of the encoder's long keyframe interval: a device that
// loses a frame does not wait out that interval, it asks -- and keeps asking
// until a picture that stands on its own actually arrives, since both the
// request and the answer ride UDP and can be lost like anything else.
void test_a_lost_frame_is_repaired_by_asking_rather_than_by_waiting() {
    if (!video::have_ffmpeg()) {
        test::skip("this build has no video codec");
        return;
    }

    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");
    host.set_video_wanted(true);

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }
    CHECK_MSG(host.companion_accepts_video(), "the far end cannot decode video");

    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    const std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    uint64_t serial = 1;
    host.publish_screens(screen_pixels(top), screen_pixels(bottom), serial);
    CHECK_MSG(spin(host, companion,
                   [&] { return companion.screen_pixels(ScreenId::Bottom).valid(); }),
              "no video frame arrived at all");

    // Past the stream's own opening keyframe, so what arrives next is only a
    // keyframe because it was asked for. Kept well under the encoder's
    // periodic interval, which is the thing this is meant to make unnecessary.
    for (int frame = 0; frame < 20; ++frame) {
        host.publish_screens(screen_pixels(top), screen_pixels(bottom), ++serial);
        spin(host, companion, [] { return false; }, 20);
    }
    const unsigned asked_before = host.keyframes_asked_for_test();

    // Everything already on its way shown first, so what the screen holds
    // from here is only what arrives after the loss.
    spin(host, companion, [&] { return companion.buffered_frames() == 0; }, 1000);
    const uint64_t shown_at_loss = companion.newest_frame_serial_for_test();

    companion.lose_a_frame_for_test(ScreenId::Bottom);
    CHECK_MSG(spin(host, companion,
                   [&] { return host.keyframes_asked_for_test() > asked_before; }, 2000),
              "the companion never told the host it had lost a frame");

    // And the answer actually lands: the companion stops waiting only when a
    // picture that stands on its own arrives, not when it has finished asking.
    // The picture has to keep changing for the host to have anything to send.
    //
    // And nothing is shown until it does. A picture built on the one that was
    // lost is exactly the corruption this exists to keep off the screen, so
    // the last good picture stays up instead.
    bool shown_while_waiting = false;
    CHECK_MSG(spin(host, companion,
                   [&] {
                       host.publish_screens(screen_pixels(top), screen_pixels(bottom), ++serial);
                       const bool waiting = companion.waiting_for_keyframe_for_test(ScreenId::Bottom);
                       if (waiting && companion.newest_frame_serial_for_test() != shown_at_loss) {
                           shown_while_waiting = true;
                       }
                       return !waiting;
                   },
                   4000),
              "the keyframe the companion asked for never arrived");
    CHECK_MSG(!shown_while_waiting,
              "a picture built on the lost frame was shown while waiting for the keyframe");
    // The picture moves on again from the keyframe.
    CHECK_MSG(spin(host, companion,
                   [&] {
                       host.publish_screens(screen_pixels(top), screen_pixels(bottom), ++serial);
                       return companion.newest_frame_serial_for_test() > shown_at_loss;
                   },
                   2000),
              "the picture never moved on after the keyframe arrived");
}

// A Wii U game reading the GamePad's gyroscope needs the companion's, since
// the companion is the thing being held. Two halves to this: the host has to
// ask (a console with no use for motion must not leave a handheld's gyro
// running), and what the companion sends has to arrive as the numbers it
// sent -- these are raw sensor readings and nothing in between is entitled
// to an opinion about them.
void test_a_console_that_reads_motion_asks_for_it_and_gets_it() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.system = SystemId::WiiU;
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "tv_only");

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }

    CHECK_MSG(spin(host, companion, [&] { return companion.host_wants_motion(); }),
              "a Wii U host never told the companion it reads how the pad is held, so the "
              "companion would leave its gyroscope switched off");

    DsInputState input;
    input.motion.present = true;
    input.motion.gyro_x = -0.0125f;
    input.motion.gyro_y = 1.75f;
    input.motion.gyro_z = 0.5f;
    input.motion.accel_x = 0.03f;
    input.motion.accel_y = -9.79f;
    input.motion.accel_z = 0.42f;
    input.motion.sampled_us = 4000000000u;
    companion.send_input(input);

    CHECK_MSG(spin(host, companion, [&] { return host.companion_input().motion.present; }),
              "how the companion is being held never reached the host");
    CHECK(host.companion_input().motion == input.motion);
    // Named separately so a failure says which number went wrong. The small
    // one matters most: a gyroscope barely moving is most of what one
    // reports, and anything that rounded this would flatten aiming entirely.
    CHECK_EQ(host.companion_input().motion.gyro_x, -0.0125f);
    CHECK_EQ(host.companion_input().motion.accel_y, -9.79f);
    CHECK_EQ(host.companion_input().motion.sampled_us, 4000000000u);
}

// And the other way round: a console with no use for motion never asks, so
// nothing on the companion turns a sensor on for it.
void test_a_console_that_ignores_motion_never_asks_for_it() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.system = SystemId::Ds;
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }
    // Given time to have said it, so this is "it never did" rather than "it
    // had not yet".
    spin(host, companion, [] { return false; }, 200);
    CHECK_MSG(!companion.host_wants_motion(),
              "a DS host asked the companion for motion, which would leave a handheld's "
              "gyroscope running for a console that has none");

    // A companion that reads its sensors anyway -- something else on that
    // machine wanted them -- still must not fill the host's pad with
    // readings nothing there will look at.
    DsInputState input;
    input.motion.present = true;
    input.motion.gyro_y = 1.75f;
    companion.send_input(input);
    spin(host, companion, [&] { return host.companion_input_fresh(); }, 500);
    CHECK_MSG(!host.companion_input().motion.present,
              "motion was sent to a host that never asked for it");
}

// A device that cannot turn arriving frames into drawn ones fast enough asks
// for fewer pixels rather than falling further and further behind. Measured on
// a Switch: 30ms per frame against a 16ms frame period, so it decoded 23
// frames a second, drew 11, and held every one of them 200ms.
void test_a_device_that_cannot_keep_up_asks_for_a_smaller_picture() {
    HostSession host;
    HostSession::Config config;
    config.device_name = "Test PC";
    config.game_title = "Test Game";
    config.rom_sha1 = "cafebabe";
    config.host_screen = ScreenId::Top;

    std::string error;
    if (!host.start(config, &error)) {
        std::fprintf(stderr, "skipping: cannot host (%s)\n", error.c_str());
        return;
    }
    set_shared(host, "upper_on_tv");

    CompanionSession companion;
    CHECK_MSG(companion.begin_search("Test Deck", ScreenId::Bottom, &error), error);
    if (!spin(host, companion, [&] { return !companion.hosts().empty(); })) {
        CHECK_MSG(false, "the companion never discovered the host");
        return;
    }
    companion.connect_to(companion.hosts().front());
    if (!paired(host, companion)) {
        CHECK_MSG(false, "handshake did not complete: " + companion.status_detail());
        return;
    }

    companion.request_display_size(1280, 720);
    int width = 0;
    int height = 0;
    CHECK_MSG(spin(host, companion,
                   [&] { return host.companion_display_size(width, height); }),
              "the companion's display size never reached the host");
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 720);
    CHECK(companion.display_scale_for_test() == 1.0);

    // Two windows' worth of a device spending almost all of every second on
    // this, which is the case the whole thing exists for.
    const double before = companion.display_scale_for_test();
    CHECK_MSG(spin(host, companion,
                   [&] {
                       companion.spend_this_long_decoding_for_test(20);
                       return companion.display_scale_for_test() < before;
                   },
                   8000),
              "a device spending its whole budget on decoding never asked for less");
    CHECK_MSG(spin(host, companion,
                   [&] {
                       host.companion_display_size(width, height);
                       return width < 1280;
                   },
                   4000),
              "the smaller size it settled on never reached the host");
    // Still the same shape, so the picture is not stretched on the way down.
    CHECK_MSG(std::abs(static_cast<double>(width) / height - 1280.0 / 720.0) < 0.02,
              "asking for less changed the shape of the picture, not just its size");
    // And never below the floor, however hopeless it looks.
    spin(host, companion,
         [&] {
             companion.spend_this_long_decoding_for_test(20);
             return false;
         },
         12000);
    CHECK_MSG(companion.display_scale_for_test() >= 0.5,
              "a device that cannot keep up at any size shrank the picture away to nothing");
}

}  // namespace

int main() {
    test_host_and_companion_pair_up();
    test_a_refused_connect_is_really_tried_again();
    test_a_received_screen_is_readable_as_pixels();
    test_a_screen_above_the_old_native_only_size_still_arrives();
    test_non_split_layout_sends_both_screens();
    test_companion_gets_the_screen_it_asked_for();
    test_audio_reaches_the_companion();
    test_companion_can_request_a_layout();
    test_the_menu_is_one_menu_across_both_devices();
    test_shared_settings_stay_in_sync();
    test_companion_menu_pauses_the_host();
    test_a_tap_quicker_than_a_frame_still_reaches_the_game();
    test_reconnecting_after_a_menu_disconnect_does_not_leave_the_host_paused();
    test_quit_reaches_the_host_even_though_the_companion_disconnects_right_behind_it();
    test_menu_gets_the_companion_both_screens();
    test_joining_gives_up_on_a_host_that_never_answers();
    test_a_real_host_is_not_timed_out();
    test_frames_that_arrive_together_are_shown_on_a_clock();
    test_a_companion_that_falls_behind_stops_being_sent_frames();
    test_a_source_faster_than_the_link_is_still_allowed_frames_in_flight();
    test_a_companion_that_stops_acking_entirely_is_recovered_from();
    test_a_frame_lost_as_the_game_paused_is_sent_again_while_paused();
    test_frames_lost_in_a_blip_are_noticed_before_the_timeout();
    test_a_steady_link_holds_the_smallest_buffer_it_can();
    test_an_uneven_link_buys_cover_and_gives_it_back();
    test_the_bitrate_is_a_budget_for_the_link();
    test_the_pacer_follows_what_is_really_produced();
    test_the_in_flight_window_covers_the_delay_this_link_shows();
    test_a_lost_frame_is_given_up_on_in_round_trips();
    test_one_bad_moment_a_second_does_not_buy_a_whole_session_of_buffer();
    test_an_emulator_rendering_unevenly_does_not_buy_buffer();
    test_this_devices_own_stall_is_not_the_links_fault();
    test_the_buffer_pays_for_the_body_and_not_the_tail();
    test_cover_is_earned_by_running_dry_and_given_back_when_calm();
    test_cover_comes_back_on_a_link_that_twitches_now_and_then();
    test_the_hold_setting_moves_both_ends_of_the_trade();
    test_a_clock_finer_than_the_loop_buys_no_cover();
    test_an_empty_queue_is_not_a_stall_if_the_picture_was_on_time();
    test_a_backlog_is_given_back_rather_than_carried_for_ever();
    test_a_backlog_is_drained_by_pacing_not_by_skipping_frames();
    test_the_pacing_clock_keeps_the_streams_own_rate();
    test_a_standing_backlog_is_let_go_as_promptly_as_the_setting_says();
    test_a_pipelined_encoder_still_sends_each_picture_under_its_own_serial();
    test_a_lost_frame_is_repaired_by_asking_rather_than_by_waiting();
    test_a_console_that_reads_motion_asks_for_it_and_gets_it();
    test_a_console_that_ignores_motion_never_asks_for_it();
    test_a_device_that_cannot_keep_up_asks_for_a_smaller_picture();
    test_a_device_does_not_count_its_own_loop_as_the_link_stalling();
    test_the_smallest_picture_is_a_size_rather_than_a_fraction();
    test_a_step_that_bought_nothing_is_not_taken_twice();
    test_a_sliver_narrower_is_not_a_step();
    test_the_ladder_steps_from_what_arrives();
    test_a_step_is_judged_on_the_size_it_asked_for();
    test_a_picture_the_host_sent_late_buys_no_cover();
    test_a_stream_the_host_held_back_is_not_room_to_spare();
    test_host_can_disconnect_the_companion();
    test_the_codec_can_change_without_reconnecting();
    test_video_is_capped_to_the_companions_display_size();
    return test::finish("session");
}
