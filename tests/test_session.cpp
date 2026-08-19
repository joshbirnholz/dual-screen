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
    // The host offers the screen it is not using itself.
    CHECK(target.offers(ScreenId::Bottom));
    CHECK(!target.offers(ScreenId::Top));

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
    if (!spin(host, companion, [&] { return host.has_companion(); })) {
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

}  // namespace

int main() {
    test_host_and_companion_pair_up();
    test_non_split_layout_sends_both_screens();
    return test::finish("session");
}
