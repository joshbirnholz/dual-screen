// Pairs a real HostSession with a real CompanionSession over loopback: the
// handshake, the screens the layout says are needed, and input travelling back.
// No GL context, so the companion's pixel buffers are inspected rather than its
// textures.

#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include "net/companion_session.hpp"
#include "net/host_session.hpp"

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
        return host.has_companion() &&
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
                bool vertical_gap = true) {
    HostSession::SharedState shared;
    shared.layout_id = layout_id;
    shared.per_device_layout = (layout_id == "upper_on_tv");
    shared.bilinear_filtering = bilinear_filtering;
    shared.audio_target = audio_target;
    shared.handedness = handedness;
    shared.render_scale = render_scale;
    shared.vertical_gap = vertical_gap;
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
        return companion.status() == CompanionSession::Status::Connected && host.has_companion();
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
        return companion.status() == CompanionSession::Status::Connected && host.has_companion();
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
                   host.has_companion();
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
    host.publish_audio(tone.data(), 512, 32823);

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
    host.publish_audio(tone.data(), 512, 32823);
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

    // And Vertical Screens' gap, the same shape as everything above: the
    // companion asks, the host decides and broadcasts.
    companion.request_vertical_gap(false);
    const bool asked_gap = spin(host, companion, [&] {
        return host.take_request(request) && request.wants_vertical_gap;
    });
    CHECK_MSG(asked_gap, "the vertical gap request never reached the host");
    CHECK_EQ(request.vertical_gap, false);

    set_shared(host, "upper_on_tv", true, AudioTarget::TopScreen, Handedness::Left, 2, false);
    CHECK_MSG(spin(host, companion, [&] { return companion.host_vertical_gap() == false; }),
              "the host's vertical gap choice never reached the companion");

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

    CHECK(!host.companion_menu_open());

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
    CHECK_MSG(spin(host, companion, [&] { return host.companion_menu_open(); }),
              "the companion's menu never reached the host");
    // Only changes go over the wire, so a button held as the menu opened would
    // stay held for as long as the menu was up if the host did not let go of it.
    CHECK_EQ(host.companion_input().buttons, 0u);
    CHECK_EQ(host.companion_input().touching, false);

    companion.set_menu_open(false);
    CHECK_MSG(spin(host, companion, [&] { return !host.companion_menu_open(); }),
              "the companion's menu never closed on the host");

    // And a device that leaves is not still holding a menu open.
    companion.set_menu_open(true);
    CHECK(spin(host, companion, [&] { return host.companion_menu_open(); }));
    companion.disconnect();
    CHECK_MSG(spin(host, companion, [&] { return !host.has_companion(); }),
              "the host never noticed the companion leaving");
    CHECK(!host.companion_menu_open());

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
    CHECK(spin(host, companion, [&] { return host.companion_menu_open(); }));
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
    CHECK(spin(host, companion, [&] { return host.companion_menu_open(); }));
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

}  // namespace

int main() {
    test_host_and_companion_pair_up();
    test_a_screen_above_the_old_native_only_size_still_arrives();
    test_non_split_layout_sends_both_screens();
    test_companion_gets_the_screen_it_asked_for();
    test_audio_reaches_the_companion();
    test_companion_can_request_a_layout();
    test_shared_settings_stay_in_sync();
    test_companion_menu_pauses_the_host();
    test_reconnecting_after_a_menu_disconnect_does_not_leave_the_host_paused();
    test_quit_reaches_the_host_even_though_the_companion_disconnects_right_behind_it();
    test_menu_gets_the_companion_both_screens();
    test_host_can_disconnect_the_companion();
    return test::finish("session");
}
