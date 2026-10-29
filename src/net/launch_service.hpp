#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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
// machine nobody may be sitting at, so a device is paired with it once: the
// PC's Settings show a code while Pair a Device is open, the device sends it
// back, and is given a token it shows every time after.
//
// A search is a broadcast question on kLaunchQueryPort, answered by each PC
// with who it is. Everything after that is one request and one answer on a
// TCP connection to kLaunchPort, a frame of JSON each way.

constexpr uint16_t kLaunchQueryPort = 47825;
constexpr uint16_t kLaunchPort = 47826;
// How long a code shown for pairing stays good for.
constexpr int64_t kPairingCodeSeconds = 300;

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
};

// The PC end's own record: who it is, the code being shown, and the devices
// paired with it. Kept as JSON in one file, which the app writes the code
// into and the service reads, so the two need nothing else between them.
class RemoteStartRecord {
public:
    explicit RemoteStartRecord(std::string path) : path_(std::move(path)) {}

    // Who this PC is, made up the first time and kept.
    std::string host_id();
    // A new six-digit code, good for kPairingCodeSeconds from `now`.
    std::string show_code(int64_t now);
    // Taken down: the page showing it closed.
    void hide_code();
    // The token for `device`, if `code` is the one showing and still good;
    // the code is used up either way it matches.
    bool pair(const std::string& code, const std::string& device, int64_t now, std::string& token);
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
    void stop();
    bool running() const { return thread_.joinable(); }
    // What the last search found.
    std::vector<LaunchHost> hosts() const;

private:
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    mutable std::mutex mutex_;
    std::vector<LaunchHost> hosts_;
};

// The tokens this device was given by the computers it paired with, by each
// computer's id, kept in a file of their own at `path` -- the companion
// keeps no settings.json of its own. Empty for a computer it has none from.
std::string remote_start_token(const std::string& path, const std::string& host_id);
// Kept, or with an empty `token` forgotten.
bool keep_remote_start_token(const std::string& path, const std::string& host_id, const std::string& token);
// Whether the games are shown as a grid of icons rather than a list, kept
// in the same file -- a grid unless the list was chosen.
bool remote_start_grid(const std::string& path);
bool keep_remote_start_grid(const std::string& path, bool grid);

// The other end's errors are the player's to read; `unpaired` is set when the
// token was not one the PC knows, so the device can ask for a code instead.
bool pair_with_host(const LaunchHost& host, const std::string& code, const std::string& device_name,
                    std::string& token, std::string* error);
bool list_host_games(const LaunchHost& host, const std::string& token, std::vector<LaunchableGame>& out,
                     bool& unpaired, std::string* error);
bool start_host_game(const LaunchHost& host, const std::string& token, const std::string& game_id,
                     bool& unpaired, std::string* error);
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
// The packet itself, for a test.
std::vector<uint8_t> wake_on_lan_packet(const std::string& mac);

}  // namespace ds::net
