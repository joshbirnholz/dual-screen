// Starting a game on a PC from a connected device, over a real socket on
// loopback: the device finds the PC, is turned away until it has been paired,
// pairs with the code showing and no other, and then sees the PC's games and
// starts one -- only ever one the PC listed.

#include "test_support.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#include <string>
#include <vector>

#include <stdlib.h>

#include "launch/game_catalog.hpp"
#include "net/launch_service.hpp"

using namespace ds;
using namespace ds::net;

namespace {

// Below 32768, as the save exchange test's are, and apart from them.
constexpr uint16_t kQueryPort = 29943;
constexpr uint16_t kPort = 29944;
constexpr uint16_t kWakeQueryPort = 29947;
constexpr uint16_t kWakePort = 29948;

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
    config.icon = [](const LaunchableGame&, int& width, int& height, std::vector<uint8_t>& rgba) {
        width = 2;
        height = 3;
        rgba.assign(2 * 3 * 4, 0);
        for (std::size_t at = 0; at < rgba.size(); ++at) rgba[at] = static_cast<uint8_t>(at * 7);
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

    // Its icon, as the grid draws it.
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    CHECK_MSG(host_game_icon(host, token, "steam:123", width, height, rgba, &error), error);
    CHECK(width == 2 && height == 3 && rgba.size() == 24u && rgba[23] == static_cast<uint8_t>(23 * 7));
    CHECK(!host_game_icon(host, token, "steam:999", width, height, rgba, &error));

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
    // A grid unless the list was chosen.
    CHECK(remote_start_grid(path));
    CHECK(keep_remote_start_grid(path, false));
    CHECK(!remote_start_grid(path));
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

void test_base64() {
    for (std::size_t size = 0; size < 7; ++size) {
        std::vector<uint8_t> bytes;
        for (std::size_t at = 0; at < size; ++at) bytes.push_back(static_cast<uint8_t>(250 - at * 31));
        CHECK(base64_decode(base64_encode(bytes)) == bytes);
    }
    CHECK_STR_EQ(base64_encode({'M', 'a'}), "TWE=");
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

// When each game was last played is kept by its path, newest kept; a game
// played that the list does not have is added to it, while it is still there.
void test_games_played_are_remembered() {
    const std::string sandbox = make_sandbox();
    const std::string record = sandbox + "/played_games.json";
    const std::string kirby = sandbox + "/kirby.nds";
    const std::string mario = sandbox + "/mario.nds";
    for (const std::string& path : {kirby, mario}) {
        FILE* file = fopen(path.c_str(), "wb");
        CHECK(file != nullptr);
        if (file != nullptr) fclose(file);
    }
    CHECK(played_games(record).empty());
    remember_played(record, kirby, 100);
    remember_played(record, mario, 200);
    remember_played(record, kirby, 300);
    CHECK_EQ(played_games(record).size(), 2u);

    LaunchableGame listed;
    listed.id = "file:" + kirby;
    listed.title = "Kirby";
    listed.system = "ds";
    listed.path = kirby;
    std::vector<LaunchableGame> games{listed};
    add_played(games, record);
    CHECK_EQ(games.size(), 2u);
    for (const LaunchableGame& game : games) {
        if (game.path == kirby) CHECK_EQ(game.last_played, 300);
        else CHECK_EQ(game.last_played, 200);
    }
    // One gone since is not offered.
    remove(mario.c_str());
    games = {listed};
    add_played(games, record);
    CHECK_EQ(games.size(), 1u);
}

// A computer that answered is remembered, by id, with how to wake it, and
// waking one that is awake finds it straight away.
void test_a_computer_is_remembered_and_woken() {
    const std::string tokens = make_sandbox() + "/remote_start_tokens.json";
    CHECK(remembered_computers(tokens).empty());
    LaunchService::Config config;
    config.query_port = kWakeQueryPort;
    config.port = kWakePort;
    config.name = "Study PC";
    config.macs = {"aa:bb:cc:dd:ee:01"};
    config.record_path = make_sandbox() + "/remote_start.json";
    LaunchService service(config);
    std::string error;
    CHECK_MSG(service.start(&error), error);
    LaunchSearch search;
    search.query_port = kWakeQueryPort;
    search.addresses = {"127.0.0.1"};
    search.timeout_ms = 500;
    const std::vector<LaunchHost> hosts = search_launch_hosts(search);
    CHECK_EQ(hosts.size(), 1u);
    if (hosts.empty()) {
        service.stop();
        return;
    }
    remember_computer(tokens, hosts[0]);
    CHECK(keep_remote_start_token(tokens, hosts[0].id, "abc"));
    remember_computer(tokens, hosts[0]);
    const std::vector<LaunchHost> remembered = remembered_computers(tokens);
    CHECK_EQ(remembered.size(), 1u);
    CHECK(!remembered.empty() && remembered[0].name == "Study PC" && remembered[0].macs == config.macs);
    CHECK_STR_EQ(remote_start_token(tokens, hosts[0].id), "abc");

    LaunchHost asleep = remembered.empty() ? hosts[0] : remembered[0];
    asleep.address = "127.0.0.1";
    asleep.asleep = true;
    std::atomic<bool> cancelled{false};
    LaunchHost awake;
    CHECK_MSG(wake_and_wait(asleep, 5000, cancelled, awake, &error, kWakeQueryPort), error);
    CHECK_STR_EQ(awake.id, hosts[0].id);
    // Put away while waiting: stops, saying nothing.
    service.stop();
    cancelled = true;
    error.clear();
    CHECK(!wake_and_wait(asleep, 5000, cancelled, awake, &error, kWakeQueryPort));
    CHECK(error.empty());
    // Never woken in time: says so.
    cancelled = false;
    CHECK(!wake_and_wait(asleep, 1500, cancelled, awake, &error, kWakeQueryPort));
    CHECK(!error.empty());
}

// The first screen's recent games: only ones played, newest first, at most
// as many as asked for, from each computer paired with.
void test_the_recent_games_feed() {
    const std::string sandbox = make_sandbox();
    LaunchService::Config config;
    config.query_port = kWakeQueryPort;
    config.port = kWakePort;
    config.name = "Den PC";
    config.record_path = sandbox + "/remote_start.json";
    config.games = [] {
        std::vector<LaunchableGame> games;
        const char* titles[] = {"Never", "Older", "Newest", "Middle"};
        const int64_t played[] = {0, 100, 300, 200};
        for (int at = 0; at < 4; ++at) {
            LaunchableGame game;
            game.id = "steam:" + std::to_string(at + 1);
            game.title = titles[at];
            game.system = "ds";
            game.steam_run_id = static_cast<uint64_t>(at + 1);
            game.last_played = played[at];
            games.push_back(game);
        }
        return games;
    };
    LaunchService service(config);
    std::string error;
    CHECK_MSG(service.start(&error), error);
    LaunchSearch search;
    search.query_port = kWakeQueryPort;
    search.addresses = {"127.0.0.1"};
    search.timeout_ms = 500;
    const std::vector<LaunchHost> hosts = search_launch_hosts(search);
    CHECK_EQ(hosts.size(), 1u);
    if (hosts.empty()) {
        service.stop();
        return;
    }
    RemoteStartRecord record(config.record_path);
    std::string token;
    CHECK_MSG(pair_with_host(hosts[0], record.show_code(now()), "Switch", token, &error), error);
    const std::string tokens = sandbox + "/remote_start_tokens.json";
    CHECK(keep_remote_start_token(tokens, hosts[0].id, token));

    RecentGamesFeed feed;
    feed.start(tokens, 2);
    feed.set_hosts(hosts);
    std::vector<RecentGamesFeed::Game> games;
    for (int tries = 0; tries < 50 && games.size() < 2; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        games = feed.games();
    }
    CHECK_EQ(games.size(), 2u);
    if (games.size() == 2u) {
        CHECK_STR_EQ(games[0].game.title, "Newest");
        CHECK_STR_EQ(games[1].game.title, "Middle");
        CHECK_STR_EQ(games[0].host.id, hosts[0].id);
    }
    feed.stop();
    CHECK(!feed.running());
    service.stop();
}

}  // namespace

int main() {
    test_a_device_pairs_then_starts_a_game();
    test_a_code_runs_out();
    test_a_device_keeps_its_tokens();
    test_the_finder_finds_a_computer();
    test_base64();
    test_the_wake_packet();
    test_games_played_are_remembered();
    test_a_computer_is_remembered_and_woken();
    test_the_recent_games_feed();
    return test::finish("launch-service");
}
