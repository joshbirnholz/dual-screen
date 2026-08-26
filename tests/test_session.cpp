// Pairs a real HostSession with a real CompanionSession over loopback: the
// handshake, the screens the layout says are needed, and input travelling back.
// No GL context, so the companion's pixel buffers are inspected rather than its
// textures.

#include "test_support.hpp"

#include <algorithm>
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
    host.set_session_state(shared);
    const bool drawn = spin(host, companion, [&] {
        return companion.host_menu_open() && companion.host_menu_focus() == 4;
    });
    CHECK_MSG(drawn, "the host's open menu never reached the companion");
    CHECK_EQ(static_cast<int>(companion.host_menu_confirm()), 2);
    CHECK_MSG(companion.host_max_render_scale() == 3 && companion.host_cursor_choosable() &&
                  companion.host_right_stick_choosable(),
              "the companion was not told which entries that menu has, so its row could differ");

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
                   host.has_companion();
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

    // Three at once, as a moment of network jitter delivers them.
    publish(2);
    publish(3);
    publish(4);
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
    CHECK_MSG(companion.buffered_frames() == 2,
              std::to_string(companion.buffered_frames()) + " frames were held rather than 2");

    // A pass before the next frame is due changes nothing on screen.
    companion.poll(start_ms + 4);
    CHECK_MSG(companion.newest_frame_serial_for_test() == 2,
              "a pass before the frame was due showed it early");

    // One frame period later it is due, and exactly one more is shown.
    companion.poll(start_ms + 40);
    CHECK_MSG(companion.newest_frame_serial_for_test() == 3,
              "the pass after a frame period showed serial " +
                  std::to_string(companion.newest_frame_serial_for_test()) + " rather than 3");
    companion.poll(start_ms + 80);
    CHECK_MSG(companion.newest_frame_serial_for_test() == 4,
              "the pass after that did not reach serial 4");

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
                   host.has_companion();
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
    test_a_received_screen_is_readable_as_pixels();
    test_a_screen_above_the_old_native_only_size_still_arrives();
    test_non_split_layout_sends_both_screens();
    test_companion_gets_the_screen_it_asked_for();
    test_audio_reaches_the_companion();
    test_companion_can_request_a_layout();
    test_the_menu_is_one_menu_across_both_devices();
    test_shared_settings_stay_in_sync();
    test_companion_menu_pauses_the_host();
    test_reconnecting_after_a_menu_disconnect_does_not_leave_the_host_paused();
    test_quit_reaches_the_host_even_though_the_companion_disconnects_right_behind_it();
    test_menu_gets_the_companion_both_screens();
    test_joining_gives_up_on_a_host_that_never_answers();
    test_a_real_host_is_not_timed_out();
    test_frames_that_arrive_together_are_shown_on_a_clock();
    test_a_companion_that_falls_behind_stops_being_sent_frames();
    test_host_can_disconnect_the_companion();
    return test::finish("session");
}
