#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "launch/game_catalog.hpp"

namespace ds::net {

// Starting a game on a PC from a connected device: a Switch or a tablet asks
// the PC what it has and which to start, and then joins the session that
// starts, as it joins any.
//
// The PC end is this app run with --service, started at login and kept
// running with no window: it answers a search the way a running host's beacon
// does, lists the games -- the Dualscreen Steam shortcuts, so a game starts
// through Steam with its Steam Input layout, or the ROM folders when there
// are none -- and starts the one asked for.
//
// Unlike joining a game already on screen, this starts something on a
// machine nobody may be sitting at, so only a device given a token may: one
// that has joined a game on that PC while Remote Start was on there, which
// the host hands over as it is joined -- see RemoteStartGrantMessage.
//
// A search is a broadcast question on kLaunchQueryPort, answered by each PC
// with who it is. Everything after that is one request and one answer on a
// TCP connection to kLaunchPort, a frame of JSON each way.

constexpr uint16_t kLaunchQueryPort = 47825;
constexpr uint16_t kLaunchPort = 47826;

// A PC that can start games, as it answered a search.
struct LaunchHost {
    // The same every time that PC answers, so a device can keep its token.
    std::string id;
    std::string name;
    std::string kind;
    std::string address;
    uint16_t port = kLaunchPort;
    // Every network card's hardware address, for waking it -- see
    // wake_on_lan().
    std::vector<std::string> macs;
    // Not found by the last search, but remembered from before -- see
    // remembered_computers(): asleep, perhaps, and woken to be asked.
    bool asleep = false;
};

// The PC end's own record: who it is, and the devices given a token. Kept as
// JSON in one file, which the app writes the tokens into and the service
// reads, so the two need nothing else between them.
class RemoteStartRecord {
public:
    explicit RemoteStartRecord(std::string path) : path_(std::move(path)) {}

    // Who this PC is, made up the first time and kept.
    std::string host_id();
    // The token for `device` -- a companion joining a game here, which is
    // all the pairing there is -- made the first time and the same after.
    // Empty when it could not be kept.
    std::string grant(const std::string& device);
    bool paired(const std::string& token);
    // Which devices are paired, by name, and forgetting them all.
    std::vector<std::string> paired_devices();
    void forget_devices();

private:
    std::string path_;
};

class LaunchService {
public:
    struct Config {
        uint16_t query_port = kLaunchQueryPort;
        uint16_t port = kLaunchPort;
        std::string name;
        std::string kind;
        std::string record_path;
        // What it offers, asked afresh for each list: a shortcut added since
        // is there without a restart.
        std::function<std::vector<LaunchableGame>()> games;
        // Starts one. False with why, when it could not.
        std::function<bool(const LaunchableGame&, std::string*)> launch;
        // Whether a game is running here already, asked before starting
        // one: its title, and whether it is `wanted` itself. False for none.
        std::function<bool(const LaunchableGame& wanted, std::string& title, bool& same)> running;
        // Closes the one running, waiting until it has gone.
        std::function<bool()> close_running;
        // One's icon, RGBA: false when it has none to give.
        std::function<bool(const LaunchableGame&, int&, int&, std::vector<uint8_t>&)> icon;
        // The hardware addresses to give out; empty means this machine's.
        std::vector<std::string> macs;
    };

    explicit LaunchService(Config config);
    ~LaunchService();

    bool start(std::string* error);
    void stop();
    bool running() const { return thread_.joinable(); }
    // How many games it has started, for a test.
    uint64_t launches() const { return launches_.load(); }

private:
    Config config_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> launches_{0};
};

struct LaunchSearch {
    uint16_t query_port = kLaunchQueryPort;
    int timeout_ms = 1500;
    // Asked directly as well as by broadcast, for a test.
    std::vector<std::string> addresses;
};

// Every PC that answered within the time, by name. Blocks for all of it.
std::vector<LaunchHost> search_launch_hosts(const LaunchSearch& search);

// Searches again and again on a thread of its own while it runs, so a screen
// listing the computers that can start a game never waits on the network --
// the companion's host picker, which shows them under the games running.
class LaunchHostFinder {
public:
    LaunchHostFinder() = default;
    ~LaunchHostFinder() { stop(); }
    LaunchHostFinder(const LaunchHostFinder&) = delete;
    LaunchHostFinder& operator=(const LaunchHostFinder&) = delete;

    void start(LaunchSearch search = {}, int every_ms = 3000);
    // At once: the thread, which may be part way through a search, is let
    // go, and what it finds after is dropped.
    void stop();
    bool running() const { return shared_ != nullptr; }
    // Whether a search has finished since start().
    bool searched() const;
    // What the last search found.
    std::vector<LaunchHost> hosts() const;

private:
    struct Shared {
        std::atomic<bool> stopping{false};
        std::atomic<bool> searched{false};
        std::mutex mutex;
        std::vector<LaunchHost> hosts;
    };
    std::shared_ptr<Shared> shared_;
};

// The tokens this device was given by the computers it paired with, by each
// computer's id, kept in a file of their own at `path` -- the companion
// keeps no settings.json of its own. Empty for a computer it has none from.
std::string remote_start_token(const std::string& path, const std::string& host_id);
// Kept, or with an empty `token` forgotten.
bool keep_remote_start_token(const std::string& path, const std::string& host_id, const std::string& token);
// The computers this device has paired with, as each last answered --
// name, address and hardware addresses -- kept in the same file, so one
// asleep can still be listed and woken.
void remember_computer(const std::string& path, const LaunchHost& host);
// A computer joined that let this device start games there -- the token as
// well as how to wake it, kept in `path`.
bool keep_remote_start_grant(const std::string& path, const LaunchHost& host, const std::string& token);
std::vector<LaunchHost> remembered_computers(const std::string& path);

// The games most recently played on every computer found that this device
// is paired with, newest first, with their icons: the first screen's rows of
// them. Gathered on a thread of its own, again every few seconds, from the
// computers last handed to set_hosts() and the tokens in `tokens_path`.
//
// Its thread is let go rather than waited for when it stops -- it may be
// waiting on a computer slow to answer -- and shares nothing with this
// object but what both hold.
class RecentGamesFeed {
public:
    struct Game {
        LaunchHost host;
        LaunchableGame game;
        int icon_width = 0;
        int icon_height = 0;
        std::vector<uint8_t> icon_rgba;
    };

    RecentGamesFeed() = default;
    ~RecentGamesFeed() { stop(); }
    RecentGamesFeed(const RecentGamesFeed&) = delete;
    RecentGamesFeed& operator=(const RecentGamesFeed&) = delete;

    // At most `most` games, only ones played at all. Until a computer
    // paired with answers, the ones last seen -- see kept_recent_games() --
    // with their computer taken to be asleep, to be woken when one is
    // chosen.
    void start(std::string tokens_path, std::size_t most);
    void stop();
    bool running() const { return shared_ != nullptr; }
    void set_hosts(const std::vector<LaunchHost>& hosts);
    std::vector<Game> games() const;
    // Changes whenever games() does, so a screen knows to look again.
    uint64_t version() const;

private:
    struct Shared {
        std::atomic<bool> stopping{false};
        std::atomic<uint64_t> version{0};
        std::mutex mutex;
        std::vector<LaunchHost> hosts;
        std::vector<Game> games;
    };
    std::shared_ptr<Shared> shared_;
};

// The recent games last seen, icons and all, kept beside the tokens -- in
// remote_start_recent.json -- so the first screen still has them while the
// computer they are on is asleep. RecentGamesFeed keeps and reads them.
std::string recent_games_path(const std::string& tokens_path);
bool keep_recent_games(const std::string& path, const std::vector<RecentGamesFeed::Game>& games);
std::vector<RecentGamesFeed::Game> kept_recent_games(const std::string& path);

// The other end's errors are the player's to read; `unpaired` is set when the
// token was not one the PC knows -- its devices forgotten since.
bool list_host_games(const LaunchHost& host, const std::string& token, std::vector<LaunchableGame>& out,
                     bool& unpaired, std::string* error);
// How a start was answered beyond yes or no.
struct StartAnswer {
    bool unpaired = false;
    // That very game is running there already: nothing was started, and it
    // is to be joined as it is.
    bool already = false;
    // Another game is running there, by its title: nothing was started.
    // Asked again with `replace`, it is closed first.
    std::string running;
};
bool start_host_game(const LaunchHost& host, const std::string& token, const std::string& game_id, bool replace,
                     StartAnswer& answer, std::string* error);
// A listed game's icon, RGBA. False when it has none -- or the computer is
// one from before icons were asked for, which says it does not know the
// question.
bool host_game_icon(const LaunchHost& host, const std::string& token, const std::string& game_id, int& width,
                    int& height, std::vector<uint8_t>& rgba, std::string* error);

// Base64, for an icon's bytes inside the JSON. Empty out on bad input.
std::string base64_encode(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> base64_decode(const std::string& text);

// The hardware addresses of this machine's network cards, "aa:bb:cc:dd:ee:ff".
std::vector<std::string> this_machine_macs();
// A magic packet for `mac`, broadcast on the usual ports: a PC asleep with
// Wake-on-LAN on wakes up.
bool wake_on_lan(const std::string& mac, std::string* error);
// Wakes a computer remembered from before, the way Moonlight wakes a PC: a
// magic packet for each of its network cards, to every local network's
// broadcast address, to 255.255.255.255, and straight to the address it
// last had, on ports 7 and 9 -- whichever of those reaches it.
bool wake_computer(const LaunchHost& host, std::string* error);
// Wakes it, then asks after it until it answers, sending the packets again
// every few seconds, for up to `timeout_ms`; `awake` is how it answered.
// Stops early when `cancelled` is set.
bool wake_and_wait(const LaunchHost& host, int timeout_ms, const std::atomic<bool>& cancelled, LaunchHost& awake,
                   std::string* error, uint16_t query_port = kLaunchQueryPort);
// The packet itself, for a test.
std::vector<uint8_t> wake_on_lan_packet(const std::string& mac);

}  // namespace ds::net
