// Starting a game on a PC from a connected device, over a real socket on
// loopback: the device finds the PC, is turned away until it has been paired,
// pairs with the code showing and no other, and then sees the PC's games and
// starts one -- only ever one the PC listed.

#include "test_support.hpp"

#include <chrono>
#include <ctime>
#include <thread>
#include <string>
#include <vector>

#include <stdlib.h>

#include "net/launch_service.hpp"

using namespace ds;
using namespace ds::net;

namespace {

// Below 32768, as the save exchange test's are, and apart from them.
constexpr uint16_t kQueryPort = 29943;
constexpr uint16_t kPort = 29944;

std::string make_sandbox() {
    char pattern[] = "/tmp/dualscreen-launch-XXXXXX";
    const char* directory = mkdtemp(pattern);
    return directory != nullptr ? std::string(directory) : std::string();
}

int64_t now() { return static_cast<int64_t>(std::time(nullptr)); }

void test_a_device_pairs_then_starts_a_game() {
    const std::string sandbox = make_sandbox();
    CHECK(!sandbox.empty());
    const std::string record_path = sandbox + "/remote_start.json";

    std::vector<std::string> started;
    LaunchService::Config config;
    config.query_port = kQueryPort;
    config.port = kPort;
    config.name = "Living Room PC";
    config.kind = "pc";
    config.record_path = record_path;
    config.macs = {"aa:bb:cc:dd:ee:ff"};
    config.games = [] {
        LaunchableGame kirby;
        kirby.id = "steam:123";
        kirby.title = "Kirby Mass Attack";
        kirby.system = "ds";
        kirby.steam_run_id = 123;
        return std::vector<LaunchableGame>{kirby};
    };
    config.launch = [&started](const LaunchableGame& game, std::string*) {
        started.push_back(game.title);
        return true;
    };
    LaunchService service(config);
    std::string error;
    CHECK_MSG(service.start(&error), error);

    LaunchSearch search;
    search.query_port = kQueryPort;
    search.addresses = {"127.0.0.1"};
    search.timeout_ms = 800;
    const std::vector<LaunchHost> hosts = search_launch_hosts(search);
    CHECK_EQ(hosts.size(), 1u);
    if (hosts.empty()) {
        service.stop();
        return;
    }
    LaunchHost host = hosts.front();
    CHECK_STR_EQ(host.name, "Living Room PC");
    CHECK_EQ(host.port, kPort);
    CHECK(host.macs.size() == 1u && host.macs[0] == "aa:bb:cc:dd:ee:ff");
    // The same PC answers as the same PC every time.
    CHECK_STR_EQ(host.id, RemoteStartRecord(record_path).host_id());

    // Not paired: turned away, and told so.
    std::vector<LaunchableGame> games;
    bool unpaired = false;
    CHECK(!list_host_games(host, "", games, unpaired, &error));
    CHECK(unpaired);

    // No code showing, or the wrong one: no.
    std::string token;
    CHECK(!pair_with_host(host, "123456", "Switch", token, &error));
    RemoteStartRecord record(record_path);
    const std::string code = record.show_code(now());
    CHECK_EQ(code.size(), 6u);
    const std::string wrong = code == "000000" ? "000001" : "000000";
    CHECK(!pair_with_host(host, wrong, "Switch", token, &error));
    // The one showing: yes, once.
    CHECK_MSG(pair_with_host(host, code, "Switch", token, &error), error);
    CHECK(!token.empty());
    std::string again;
    CHECK(!pair_with_host(host, code, "Phone", again, &error));
    CHECK(record.paired_devices() == std::vector<std::string>{"Switch"});

    CHECK_MSG(list_host_games(host, token, games, unpaired, &error), error);
    CHECK(!unpaired);
    CHECK_EQ(games.size(), 1u);
    CHECK(!games.empty() && games[0].title == "Kirby Mass Attack" && games[0].system == "ds");

    CHECK_MSG(start_host_game(host, token, "steam:123", unpaired, &error), error);
    CHECK(started == std::vector<std::string>{"Kirby Mass Attack"});
    // Nothing it did not list.
    CHECK(!start_host_game(host, token, "file:/usr/bin/xterm", unpaired, &error));
    CHECK_EQ(started.size(), 1u);
    CHECK_EQ(service.launches(), 1u);

    // Forgotten: turned away again.
    record.forget_devices();
    CHECK(!start_host_game(host, token, "steam:123", unpaired, &error));
    CHECK(unpaired);

    service.stop();
}

void test_a_code_runs_out() {
    const std::string sandbox = make_sandbox();
    RemoteStartRecord record(sandbox + "/remote_start.json");
    const std::string code = record.show_code(1000);
    std::string token;
    CHECK(!record.pair(code, "Switch", 1000 + kPairingCodeSeconds + 1, token));
    const std::string fresh = record.show_code(2000);
    record.hide_code();
    CHECK(!record.pair(fresh, "Switch", 2001, token));
    const std::string last = record.show_code(3000);
    CHECK(record.pair(last, "Switch", 3001, token));
    CHECK(record.paired(token));
    CHECK(!record.paired(""));
}

// The device's end: a token kept by the computer's id, found again, and
// forgotten with an empty one.
void test_a_device_keeps_its_tokens() {
    const std::string path = make_sandbox() + "/remote_start_tokens.json";
    CHECK(remote_start_token(path, "pc1").empty());
    CHECK(keep_remote_start_token(path, "pc1", "abc"));
    CHECK(keep_remote_start_token(path, "pc2", "def"));
    CHECK_STR_EQ(remote_start_token(path, "pc1"), "abc");
    CHECK_STR_EQ(remote_start_token(path, "pc2"), "def");
    CHECK(keep_remote_start_token(path, "pc1", ""));
    CHECK(remote_start_token(path, "pc1").empty());
    CHECK_STR_EQ(remote_start_token(path, "pc2"), "def");
}

// The finder searches on its own thread: what it has found is there to read
// without waiting, and gone once it stops.
void test_the_finder_finds_a_computer() {
    LaunchService::Config config;
    config.query_port = kQueryPort;
    config.port = kPort;
    config.name = "Den PC";
    config.record_path = make_sandbox() + "/remote_start.json";
    LaunchService service(config);
    std::string error;
    CHECK_MSG(service.start(&error), error);
    LaunchSearch search;
    search.query_port = kQueryPort;
    search.addresses = {"127.0.0.1"};
    search.timeout_ms = 300;
    LaunchHostFinder finder;
    finder.start(search, 100);
    std::vector<LaunchHost> found;
    for (int tries = 0; tries < 40 && found.empty(); ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        found = finder.hosts();
    }
    CHECK_EQ(found.size(), 1u);
    CHECK(!found.empty() && found[0].name == "Den PC");
    finder.stop();
    CHECK(finder.hosts().empty());
    service.stop();
}

void test_the_wake_packet() {
    const std::vector<uint8_t> packet = wake_on_lan_packet("01:23:45:67:89:AB");
    CHECK_EQ(packet.size(), 102u);
    if (packet.size() == 102u) {
        CHECK_EQ(packet[0], 0xFFu);
        CHECK_EQ(packet[5], 0xFFu);
        CHECK_EQ(packet[6], 0x01u);
        CHECK_EQ(packet[11], 0xABu);
        CHECK_EQ(packet[101], 0xABu);
    }
    CHECK(wake_on_lan_packet("not a mac").empty());
}

}  // namespace

int main() {
    test_a_device_pairs_then_starts_a_game();
    test_a_code_runs_out();
    test_a_device_keeps_its_tokens();
    test_the_finder_finds_a_computer();
    test_the_wake_packet();
    return test::finish("launch-service");
}
