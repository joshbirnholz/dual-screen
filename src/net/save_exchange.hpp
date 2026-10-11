#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "core/save_store.hpp"

namespace ds::net {

// Games' saves, from one device that plays games to another -- Data
// Management's Get Save and Send Save.
//
// Every device that plays games answers while the app is open: a Steam Deck,
// a PC, a Switch with melonDS. A device that only shows another's screens --
// an Apple TV, a phone, a Switch without a core -- has no saves and never
// answers, so it is never offered as somewhere to send one.
//
// Nothing is paired: any Dualscreen on the network is trusted, as a companion
// joining a game is. Nothing moves unless the player asks for it on the
// device doing the asking.
//
// A search is a broadcast question on kSaveQueryPort, answered by each device
// with who it is. Everything after that is one request and one answer on a TCP
// connection to kSaveExchangePort: a frame of JSON, and for a save, the save.

constexpr uint16_t kSaveQueryPort = 47823;
constexpr uint16_t kSaveExchangePort = 47824;
// Far above any DS save (1 MB at most); room for a 3DS or Wii U one later.
constexpr uint32_t kMaxSaveBytes = 64u * 1024u * 1024u;

// Kinds of device, for its icon.
inline constexpr const char* kDeviceKindSteamDeck = "steam-deck";
inline constexpr const char* kDeviceKindSwitch = "switch";
inline constexpr const char* kDeviceKindSwitchLite = "switch-lite";
inline constexpr const char* kDeviceKindPc = "pc";
// A phone, a handheld or a TV box running Android. A device from before this
// kind existed draws one as a PC.
inline constexpr const char* kDeviceKindAndroid = "android";

// This one's.
std::string this_device_kind();

// Another device that keeps saves, as it answered a search.
struct SaveDevice {
    std::string instance;  // this run of the app there, to tell two apart
    std::string name;
    std::string kind;
    std::vector<std::string> systems;  // which systems' saves it keeps: "ds", ...
    std::string address;
    uint16_t port = kSaveExchangePort;

    bool keeps(const std::string& system) const;
};

// What another device has for one game.
struct RemoteSaveStatus {
    SaveVersionState versions;
    int64_t saved_at = 0;
    std::string made_on;
    uint64_t size = 0;
    // It is playing that game right now, so its save is open and nothing
    // there will be replaced or read until it stops.
    bool playing_now = false;
    bool has_resume_point = false;
};

class SaveExchangeService {
public:
    struct Config {
        uint16_t query_port = kSaveQueryPort;
        uint16_t exchange_port = kSaveExchangePort;
        std::string kind;
        std::vector<std::string> systems{kSaveSystemDs};
    };

    SaveExchangeService(SaveStore& store, Config config);
    ~SaveExchangeService();

    // Starts answering, on a thread of its own.
    bool start(std::string* error);
    void stop();
    bool running() const { return thread_.joinable(); }

    // Names this run of the app, so a search can leave it out.
    const std::string& instance() const { return instance_; }
    // Goes up each time another device sends a save here, so a screen showing
    // saves knows to look again.
    uint64_t saves_received() const { return saves_received_.load(); }

private:
    void run();

    SaveStore& store_;
    Config config_;
    std::string instance_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> saves_received_{0};
};

struct SaveSearch {
    uint16_t query_port = kSaveQueryPort;
    int timeout_ms = 1500;
    // Leave out this run of the app.
    std::string own_instance;
    // Asked directly as well as by broadcast: for a test, which has no
    // network to broadcast on.
    std::vector<std::string> addresses;
};

// Every device that answered within the time, by name. Blocks for the whole
// of it.
std::vector<SaveDevice> search_save_devices(const SaveSearch& search);

// Each blocks until the other device answers, or for a few seconds when it
// does not.
bool ask_save_status(const SaveDevice& device, const std::string& system,
                     const std::string& game_id, RemoteSaveStatus& out, std::string* error);
// One game's save on another device, as a list of them gives it.
struct RemoteSave {
    std::string game_id;
    std::string system;
    std::string title;
    RemoteSaveStatus status;
};
// Every game with a save on `device`, for Sync All.
bool list_remote_saves(const SaveDevice& device, std::vector<RemoteSave>& out, std::string* error);
bool fetch_save(const SaveDevice& device, const std::string& game_id, SaveInfo& info,
                std::vector<uint8_t>& bytes, std::string* error);
bool send_save(const SaveDevice& device, const SaveInfo& info, const std::vector<uint8_t>& bytes,
               const std::string& from_device, std::string* error);

}  // namespace ds::net
