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
    host.set_layout("split_devices", false);

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
    CHECK_STR_EQ(companion.host_layout_id(), "split_devices");

    // --- screens -----------------------------------------------------------
    // Under the split layout the companion needs only its own screen, so the
    // top must never arrive.
    const std::vector<uint32_t> top = flat_screen(0x00204080u);
    const std::vector<uint32_t> bottom = flat_screen(0x00801020u);
    host.publish_screens(top.data(), bottom.data(), kDsScreenWidth, kDsScreenHeight,
                         kDsScreenWidth, 1);

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
    host.set_layout("vertical", false);

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
        host.publish_screens(top.data(), bottom.data(), kDsScreenWidth, kDsScreenHeight,
                             kDsScreenWidth, serial);
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
        host.set_layout("split_devices", false);

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
        host.publish_screens(top.data(), bottom.data(), kDsScreenWidth, kDsScreenHeight,
                             kDsScreenWidth, 1);
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
    host.set_layout("split_devices", false);

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
    host.set_layout("split_devices", false);

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
    std::string requested;
    CHECK(!host.take_requested_layout(requested));

    companion.request_layout("vertical");
    const bool asked = spin(host, companion, [&] { return host.take_requested_layout(requested); });
    CHECK_MSG(asked, "the layout request never reached the host");
    if (asked) {
        CHECK_STR_EQ(requested, "vertical");
        // Taken once only, so a request is not applied repeatedly.
        std::string again;
        CHECK(!host.take_requested_layout(again));
    }

    // And the host's answer comes back, so both devices agree.
    host.set_layout("vertical", false);
    const bool agreed =
        spin(host, companion, [&] { return companion.host_layout_id() == "vertical"; });
    CHECK_MSG(agreed, "the host's layout change never reached the companion");

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
    test_non_split_layout_sends_both_screens();
    test_companion_gets_the_screen_it_asked_for();
    test_audio_reaches_the_companion();
    test_companion_can_request_a_layout();
    test_host_can_disconnect_the_companion();
    return test::finish("session");
}
