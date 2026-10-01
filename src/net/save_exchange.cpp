#include "net/save_exchange.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>

#include <nlohmann/json.hpp>

#include "net/transport.hpp"
#include "util/log.hpp"

#ifdef __SWITCH__
#include "input/switch_console.hpp"
#endif

namespace ds::net {
namespace {

using json = nlohmann::json;

// "DSSQ", "DSSR" and "DSSV", in the order they are sent in.
constexpr char kQueryMagic[4] = {'D', 'S', 'S', 'Q'};
constexpr char kReplyMagic[4] = {'D', 'S', 'S', 'R'};
constexpr char kFrameMagic[4] = {'D', 'S', 'S', 'V'};
constexpr int kExchangeVersion = 1;
constexpr uint32_t kMaxFrameJson = 4u * 1024u * 1024u;
constexpr std::size_t kFrameHeaderBytes = 12;
// A request that has not arrived whole by then is not going to.
constexpr uint64_t kServerRequestTimeoutMs = 20000;
constexpr uint64_t kStatusTimeoutMs = 4000;
constexpr uint64_t kTransferTimeoutMs = 20000;

template <typename T>
T get_or(const json& object, const char* key, const T& fallback) {
    try {
        const auto found = object.find(key);
        if (found == object.end() || found->is_null()) return fallback;
        return found->get<T>();
    } catch (const json::exception&) {
        return fallback;
    }
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(value >> shift));
}

uint32_t u32_at(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

std::vector<uint8_t> make_frame(const json& message, const std::vector<uint8_t>& blob) {
    const std::string text = message.dump();
    std::vector<uint8_t> out;
    out.reserve(kFrameHeaderBytes + text.size() + blob.size());
    out.insert(out.end(), kFrameMagic, kFrameMagic + 4);
    put_u32(out, static_cast<uint32_t>(text.size()));
    put_u32(out, static_cast<uint32_t>(blob.size()));
    out.insert(out.end(), text.begin(), text.end());
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

enum class FrameState { Incomplete, Complete, Bad };

// Takes one whole frame off the front of `inbox`, if one is there.
FrameState take_frame(std::vector<uint8_t>& inbox, json& message, std::vector<uint8_t>& blob) {
    if (inbox.size() < kFrameHeaderBytes) return FrameState::Incomplete;
    if (std::memcmp(inbox.data(), kFrameMagic, 4) != 0) return FrameState::Bad;
    const uint32_t text_size = u32_at(inbox.data() + 4);
    const uint32_t blob_size = u32_at(inbox.data() + 8);
    if (text_size > kMaxFrameJson || blob_size > kMaxSaveBytes) return FrameState::Bad;
    const std::size_t total = kFrameHeaderBytes + text_size + blob_size;
    if (inbox.size() < total) return FrameState::Incomplete;
    try {
        message = json::parse(inbox.begin() + kFrameHeaderBytes,
                              inbox.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + text_size));
    } catch (const json::exception&) {
        return FrameState::Bad;
    }
    if (!message.is_object()) return FrameState::Bad;
    blob.assign(inbox.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + text_size),
                inbox.begin() + static_cast<std::ptrdiff_t>(total));
    inbox.erase(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(total));
    return FrameState::Complete;
}

std::string to_hex(const std::vector<uint8_t>& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const uint8_t byte : bytes) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

std::vector<uint8_t> from_hex(const std::string& text) {
    const auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> out;
    if (text.size() % 2 != 0) return out;
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) {
        const int high = nibble(text[index]);
        const int low = nibble(text[index + 1]);
        if (high < 0 || low < 0) return {};
        out.push_back(static_cast<uint8_t>(high << 4 | low));
    }
    return out;
}

// A save's description as it travels: everything but where the ROM is here,
// which means nothing anywhere else.
json describe_save(const SaveInfo& info) {
    json history = json::array();
    for (const SaveVersion& version : info.history) {
        history.push_back({{"id", version.id},
                           {"hash", version.hash},
                           {"made_on", version.made_on},
                           {"made_at", version.made_at}});
    }
    return {{"stem", info.stem},       {"system", info.system},
            {"game_id", info.game_id}, {"title", info.title},
            {"publisher", info.publisher}, {"icon", to_hex(info.icon)},
            {"history", history}};
}

SaveInfo read_save_description(const json& about) {
    SaveInfo info;
    info.stem = get_or<std::string>(about, "stem", "");
    info.system = get_or<std::string>(about, "system", kSaveSystemDs);
    info.game_id = get_or<std::string>(about, "game_id", "");
    info.title = get_or<std::string>(about, "title", "");
    info.publisher = get_or<std::string>(about, "publisher", "");
    info.icon = from_hex(get_or<std::string>(about, "icon", ""));
    const auto history = about.find("history");
    if (history != about.end() && history->is_array()) {
        for (const json& entry : *history) {
            if (!entry.is_object()) continue;
            SaveVersion version;
            version.id = get_or<std::string>(entry, "id", "");
            version.hash = get_or<std::string>(entry, "hash", "");
            version.made_on = get_or<std::string>(entry, "made_on", "");
            version.made_at = get_or<int64_t>(entry, "made_at", 0);
            if (!version.id.empty()) info.history.push_back(version);
        }
    }
    info.has_save = true;
    // A name with a path in it would put the save somewhere else.
    if (info.stem.find('/') != std::string::npos || info.stem.find('\\') != std::string::npos ||
        info.stem == "." || info.stem == "..") {
        info.stem.clear();
    }
    return info;
}

// One request, one answer: connects, sends, waits for the whole reply.
bool exchange(const SaveDevice& device, const json& request, const std::vector<uint8_t>& blob,
              uint64_t timeout_ms, json& reply, std::vector<uint8_t>& reply_blob,
              std::string* error) {
    TcpStream stream;
    if (!stream.start_connect(device.address, device.port, error)) return false;
    stream.send(make_frame(request, blob));
    std::vector<uint8_t> inbox;
    const uint64_t deadline = monotonic_millis() + timeout_ms;
    while (true) {
        std::string pump_error;
        const bool open = stream.pump(inbox, &pump_error);
        switch (take_frame(inbox, reply, reply_blob)) {
            case FrameState::Complete:
                if (!get_or<bool>(reply, "ok", false)) {
                    if (error != nullptr) {
                        *error = get_or<std::string>(reply, "error", device.name + " refused");
                    }
                    return false;
                }
                return true;
            case FrameState::Bad:
                if (error != nullptr) *error = device.name + " answered with something unreadable";
                return false;
            case FrameState::Incomplete:
                break;
        }
        if (!open) {
            if (error != nullptr) *error = device.name + " closed the connection: " + pump_error;
            return false;
        }
        if (monotonic_millis() >= deadline) {
            if (error != nullptr) *error = device.name + " did not answer in time";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

std::string this_device_kind() {
#ifdef __SWITCH__
    return switch_is_lite() ? kDeviceKindSwitchLite : kDeviceKindSwitch;
#else
    // Valve's own boards: Jupiter and Galileo, the LCD and OLED Decks.
    std::ifstream vendor("/sys/devices/virtual/dmi/id/board_vendor");
    std::string name;
    if (vendor && std::getline(vendor, name) && name.rfind("Valve", 0) == 0) {
        return kDeviceKindSteamDeck;
    }
    return kDeviceKindPc;
#endif
}

bool SaveDevice::keeps(const std::string& system) const {
    return std::find(systems.begin(), systems.end(), system) != systems.end();
}

SaveExchangeService::SaveExchangeService(SaveStore& store, Config config)
    : store_(store), config_(std::move(config)), instance_(new_save_version_id()) {
    if (config_.kind.empty()) config_.kind = this_device_kind();
}

SaveExchangeService::~SaveExchangeService() { stop(); }

bool SaveExchangeService::start(std::string* error) {
    stop();
    stopping_ = false;
    // Bound here rather than on the thread, so a port already taken is said
    // to whoever started this.
    auto query = std::make_shared<UdpSocket>();
    auto listener = std::make_shared<TcpListener>();
    if (!query->start(config_.query_port, false, error)) return false;
    if (!listener->start(config_.exchange_port, error)) return false;
    thread_ = std::thread([this, query, listener] {
        // Each time: sockets live on the thread that uses them.
        struct Connection {
            TcpStream stream;
            std::vector<uint8_t> inbox;
            uint64_t deadline = 0;
            bool answered = false;
        };
        std::vector<Connection> connections;
        // Saves copied in by hand are matched to their ROMs now, so that a
        // device asking about one finds it by its game.
        store_.list();
        while (!stopping_) {
            query->wait_readable(connections.empty() ? 50 : 2);
            std::vector<uint8_t> datagram;
            std::string from;
            uint16_t from_port = 0;
            while (query->receive(datagram, from, from_port)) {
                if (datagram.size() < 4 || std::memcmp(datagram.data(), kQueryMagic, 4) != 0) continue;
                json asked;
                try {
                    asked = json::parse(datagram.begin() + 4, datagram.end());
                } catch (const json::exception&) {
                    continue;
                }
                if (get_or<std::string>(asked, "instance", "") == instance_) continue;
                const json reply = {{"v", kExchangeVersion},
                                    {"instance", instance_},
                                    {"name", store_.device_name()},
                                    {"kind", config_.kind},
                                    {"systems", config_.systems},
                                    {"port", config_.exchange_port}};
                std::vector<uint8_t> out(kReplyMagic, kReplyMagic + 4);
                const std::string text = reply.dump();
                out.insert(out.end(), text.begin(), text.end());
                query->send_to(from, from_port, out.data(), out.size());
            }

            Connection incoming;
            while (listener->accept(incoming.stream)) {
                incoming.deadline = monotonic_millis() + kServerRequestTimeoutMs;
                connections.push_back(std::move(incoming));
                incoming = Connection();
            }
            for (Connection& connection : connections) {
                std::string pump_error;
                if (!connection.stream.pump(connection.inbox, &pump_error)) {
                    connection.stream.close();
                    continue;
                }
                if (connection.answered) {
                    if (connection.stream.pending_bytes() == 0) connection.stream.close();
                    continue;
                }
                json request;
                std::vector<uint8_t> blob;
                const FrameState state = take_frame(connection.inbox, request, blob);
                if (state == FrameState::Bad) {
                    connection.stream.close();
                    continue;
                }
                if (state == FrameState::Incomplete) {
                    if (monotonic_millis() >= connection.deadline) connection.stream.close();
                    continue;
                }
                json reply = {{"ok", false}};
                std::vector<uint8_t> reply_blob;
                const std::string op = get_or<std::string>(request, "op", "");
                const std::string game_id = get_or<std::string>(request, "game_id", "");
                const std::string peer = connection.stream.peer_address();
                // How one game's save stands here.
                const auto status_of = [this](const std::string& id) {
                    SaveInfo info;
                    const bool known = store_.find(id, info);
                    const SaveVersionState versions = known ? version_state(info) : SaveVersionState();
                    return json{{"has_save", versions.has_save},
                                {"current_id", versions.current_id},
                                {"current_hash", versions.current_hash},
                                {"history_ids", versions.history_ids},
                                {"saved_at", known ? info.saved_at : 0},
                                {"made_on", known && info.current() != nullptr ? info.current()->made_on
                                                                               : std::string()},
                                {"size", known ? info.size : 0},
                                {"playing_now", !id.empty() && store_.game_in_play() == id},
                                {"has_resume_point", known && store_.has_resume_point(id)}};
                };
                if (op == "status") {
                    reply = status_of(game_id);
                    reply["ok"] = true;
                } else if (op == "list") {
                    // Every game with a save here, for Sync All: which game,
                    // what it is called, and how its save stands.
                    json saves = json::array();
                    for (const SaveInfo& info : store_.list()) {
                        if (!info.has_save || info.game_id.empty()) continue;
                        if (!config_.systems.empty() &&
                            std::find(config_.systems.begin(), config_.systems.end(), info.system) ==
                                config_.systems.end()) {
                            continue;
                        }
                        saves.push_back({{"game_id", info.game_id},
                                         {"system", info.system},
                                         {"title", info.display_title()},
                                         {"status", status_of(info.game_id)}});
                    }
                    reply = {{"ok", true}, {"saves", saves}};
                } else if (op == "get") {
                    std::string error;
                    SaveInfo info;
                    if (store_.game_in_play() == game_id) {
                        reply["error"] = store_.device_name() + " is playing this game";
                    } else if (store_.read_for_sending(game_id, info, reply_blob, &error)) {
                        reply = {{"ok", true}, {"save", describe_save(info)}};
                        log_info("sent the save for " + info.display_title() + " to " + peer);
                    } else {
                        reply["error"] = error;
                    }
                } else if (op == "put") {
                    const auto about = request.find("save");
                    const std::string from_device = get_or<std::string>(request, "from", peer);
                    std::string error;
                    if (about == request.end() || !about->is_object()) {
                        reply["error"] = "no save came with the request";
                    } else {
                        const SaveInfo info = read_save_description(*about);
                        if (!config_.systems.empty() &&
                            std::find(config_.systems.begin(), config_.systems.end(), info.system) ==
                                config_.systems.end()) {
                            reply["error"] = store_.device_name() + " does not keep " +
                                             save_system_name(info.system) + " saves";
                        } else if (store_.accept(info, blob, from_device, &error)) {
                            reply = {{"ok", true}};
                            ++saves_received_;
                            log_info("took " + from_device + "'s save for " + info.display_title());
                        } else {
                            reply["error"] = error;
                        }
                    }
                } else {
                    reply["error"] = "unknown request \"" + op + "\"";
                }
                connection.stream.send(make_frame(reply, reply_blob));
                connection.answered = true;
                connection.deadline = monotonic_millis() + kServerRequestTimeoutMs;
                connection.stream.pump(connection.inbox, nullptr);
            }
            connections.erase(std::remove_if(connections.begin(), connections.end(),
                                             [](const Connection& connection) {
                                                 return !connection.stream.open() ||
                                                        (connection.answered &&
                                                         monotonic_millis() >= connection.deadline);
                                             }),
                              connections.end());
        }
    });
    log_info("answering for saves on ports " + std::to_string(config_.query_port) + " and " +
             std::to_string(config_.exchange_port) + " as " + store_.device_name() + " (" +
             config_.kind + ")");
    return true;
}

void SaveExchangeService::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    thread_.join();
}

std::vector<SaveDevice> search_save_devices(const SaveSearch& search) {
    std::vector<SaveDevice> found;
    UdpSocket socket;
    std::string error;
    if (!socket.start(0, true, &error)) {
        log_warn("could not search for devices: " + error);
        return found;
    }
    const json asked = {{"v", kExchangeVersion}, {"instance", search.own_instance}};
    std::vector<uint8_t> query(kQueryMagic, kQueryMagic + 4);
    const std::string text = asked.dump();
    query.insert(query.end(), text.begin(), text.end());

    const uint64_t started = monotonic_millis();
    const uint64_t deadline = started + static_cast<uint64_t>(std::max(search.timeout_ms, 1));
    // Asked three times over the first part of the wait: a broadcast lost on
    // wifi is simply lost.
    int asked_times = 0;
    while (true) {
        const uint64_t now = monotonic_millis();
        if (asked_times < 3 && now >= started + static_cast<uint64_t>(asked_times) * 300) {
            socket.broadcast(search.query_port, query.data(), query.size());
            for (const std::string& address : search.addresses) {
                socket.send_to(address, search.query_port, query.data(), query.size());
            }
            ++asked_times;
        }
        if (now >= deadline) break;
        socket.wait_readable(static_cast<int>(std::min<uint64_t>(deadline - now, 50)));
        std::vector<uint8_t> datagram;
        std::string from;
        while (socket.receive(datagram, from)) {
            if (datagram.size() < 4 || std::memcmp(datagram.data(), kReplyMagic, 4) != 0) continue;
            json reply;
            try {
                reply = json::parse(datagram.begin() + 4, datagram.end());
            } catch (const json::exception&) {
                continue;
            }
            SaveDevice device;
            device.instance = get_or<std::string>(reply, "instance", "");
            if (device.instance.empty() || device.instance == search.own_instance) continue;
            const bool seen = std::any_of(found.begin(), found.end(), [&](const SaveDevice& other) {
                return other.instance == device.instance;
            });
            if (seen) continue;
            device.name = get_or<std::string>(reply, "name", from);
            device.kind = get_or<std::string>(reply, "kind", kDeviceKindPc);
            device.systems = get_or<std::vector<std::string>>(reply, "systems", {});
            device.port = get_or<uint16_t>(reply, "port", kSaveExchangePort);
            device.address = from;
            found.push_back(device);
        }
    }
    std::sort(found.begin(), found.end(), [](const SaveDevice& a, const SaveDevice& b) {
        return a.name < b.name;
    });
    return found;
}

namespace {

RemoteSaveStatus read_status(const json& reply) {
    RemoteSaveStatus out;
    out.versions.has_save = get_or<bool>(reply, "has_save", false);
    out.versions.current_id = get_or<std::string>(reply, "current_id", "");
    out.versions.current_hash = get_or<std::string>(reply, "current_hash", "");
    out.versions.history_ids = get_or<std::vector<std::string>>(reply, "history_ids", {});
    out.saved_at = get_or<int64_t>(reply, "saved_at", 0);
    out.made_on = get_or<std::string>(reply, "made_on", "");
    out.size = get_or<uint64_t>(reply, "size", 0);
    out.playing_now = get_or<bool>(reply, "playing_now", false);
    out.has_resume_point = get_or<bool>(reply, "has_resume_point", false);
    return out;
}

}  // namespace

bool ask_save_status(const SaveDevice& device, const std::string& system,
                     const std::string& game_id, RemoteSaveStatus& out, std::string* error) {
    out = RemoteSaveStatus();
    json reply;
    std::vector<uint8_t> blob;
    const json request = {{"op", "status"}, {"system", system}, {"game_id", game_id}};
    if (!exchange(device, request, {}, kStatusTimeoutMs, reply, blob, error)) return false;
    out = read_status(reply);
    return true;
}

bool list_remote_saves(const SaveDevice& device, std::vector<RemoteSave>& out, std::string* error) {
    out.clear();
    json reply;
    std::vector<uint8_t> blob;
    if (!exchange(device, json{{"op", "list"}}, {}, kTransferTimeoutMs, reply, blob, error)) {
        return false;
    }
    const auto saves = reply.find("saves");
    if (saves == reply.end() || !saves->is_array()) return true;
    for (const json& entry : *saves) {
        if (!entry.is_object()) continue;
        RemoteSave save;
        save.game_id = get_or<std::string>(entry, "game_id", "");
        save.system = get_or<std::string>(entry, "system", kSaveSystemDs);
        save.title = get_or<std::string>(entry, "title", "");
        const auto status = entry.find("status");
        if (status != entry.end() && status->is_object()) save.status = read_status(*status);
        if (!save.game_id.empty()) out.push_back(save);
    }
    return true;
}

bool fetch_save(const SaveDevice& device, const std::string& game_id, SaveInfo& info,
                std::vector<uint8_t>& bytes, std::string* error) {
    json reply;
    const json request = {{"op", "get"}, {"game_id", game_id}};
    if (!exchange(device, request, {}, kTransferTimeoutMs, reply, bytes, error)) return false;
    const auto about = reply.find("save");
    if (about == reply.end() || !about->is_object()) {
        if (error != nullptr) *error = device.name + " sent a save without saying what it is";
        return false;
    }
    info = read_save_description(*about);
    if (info.game_id != game_id) {
        if (error != nullptr) *error = device.name + " sent the save for another game";
        return false;
    }
    return true;
}

bool send_save(const SaveDevice& device, const SaveInfo& info, const std::vector<uint8_t>& bytes,
               const std::string& from_device, std::string* error) {
    json reply;
    std::vector<uint8_t> reply_blob;
    const json request = {{"op", "put"}, {"from", from_device}, {"save", describe_save(info)}};
    return exchange(device, request, bytes, kTransferTimeoutMs, reply, reply_blob, error);
}

}  // namespace ds::net
