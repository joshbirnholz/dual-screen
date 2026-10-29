#include "net/launch_service.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

#include "net/transport.hpp"
#include "util/log.hpp"

namespace ds::net {
namespace {

using json = nlohmann::json;

// "DSLQ", "DSLR" and "DSLV", as save exchange's are DSS-.
constexpr char kQueryMagic[4] = {'D', 'S', 'L', 'Q'};
constexpr char kReplyMagic[4] = {'D', 'S', 'L', 'R'};
constexpr char kFrameMagic[4] = {'D', 'S', 'L', 'V'};
constexpr int kLaunchVersion = 1;
constexpr uint32_t kMaxFrameJson = 1024u * 1024u;
constexpr std::size_t kFrameHeaderBytes = 8;
constexpr uint64_t kServerRequestTimeoutMs = 10000;
constexpr uint64_t kRequestTimeoutMs = 6000;

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

uint32_t u32_at(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

std::vector<uint8_t> make_frame(const json& message) {
    const std::string text = message.dump();
    std::vector<uint8_t> out(kFrameMagic, kFrameMagic + 4);
    for (int shift = 0; shift < 32; shift += 8) out.push_back(static_cast<uint8_t>(text.size() >> shift));
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

enum class FrameState { Incomplete, Complete, Bad };

FrameState take_frame(std::vector<uint8_t>& inbox, json& message) {
    if (inbox.size() < kFrameHeaderBytes) return FrameState::Incomplete;
    if (std::memcmp(inbox.data(), kFrameMagic, 4) != 0) return FrameState::Bad;
    const uint32_t size = u32_at(inbox.data() + 4);
    if (size > kMaxFrameJson) return FrameState::Bad;
    if (inbox.size() < kFrameHeaderBytes + size) return FrameState::Incomplete;
    try {
        message = json::parse(inbox.begin() + kFrameHeaderBytes,
                              inbox.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + size));
    } catch (const json::exception&) {
        return FrameState::Bad;
    }
    if (!message.is_object()) return FrameState::Bad;
    inbox.erase(inbox.begin(), inbox.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes + size));
    return FrameState::Complete;
}

std::string random_hex(int bytes) {
    std::random_device device;
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (int index = 0; index < bytes; ++index) {
        const unsigned value = device() & 0xFFu;
        out.push_back(digits[value >> 4]);
        out.push_back(digits[value & 0x0Fu]);
    }
    return out;
}

json load_record(const std::string& path) {
    std::ifstream in(path);
    if (!in) return json::object();
    try {
        json record = json::parse(in);
        return record.is_object() ? record : json::object();
    } catch (const json::exception&) {
        return json::object();
    }
}

bool store_record(const std::string& path, const json& record) {
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
    // Written aside and moved over, so the service never reads half of it.
    const std::string temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        out << record.dump(2);
        if (!out) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) return false;
    // Tokens that start games here: this user's to read, and nobody else's.
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace, error);
    return true;
}

json describe_game(const LaunchableGame& game) {
    return {{"id", game.id}, {"title", game.title}, {"system", game.system}};
}

// One request, one answer.
bool exchange(const LaunchHost& host, const json& request, json& reply, std::string* error) {
    TcpStream stream;
    if (!stream.start_connect(host.address, host.port, error)) return false;
    stream.send(make_frame(request));
    std::vector<uint8_t> inbox;
    const uint64_t deadline = monotonic_millis() + kRequestTimeoutMs;
    while (true) {
        std::string pump_error;
        const bool open = stream.pump(inbox, &pump_error);
        switch (take_frame(inbox, reply)) {
            case FrameState::Complete: return true;
            case FrameState::Bad:
                if (error != nullptr) *error = host.name + " answered with something unreadable";
                return false;
            case FrameState::Incomplete: break;
        }
        if (!open) {
            if (error != nullptr) *error = host.name + " closed the connection: " + pump_error;
            return false;
        }
        if (monotonic_millis() >= deadline) {
            if (error != nullptr) *error = host.name + " did not answer in time";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// Whether a reply said yes; its reason when it did not.
bool answered_yes(const LaunchHost& host, const json& reply, bool& unpaired, std::string* error) {
    unpaired = get_or<bool>(reply, "unpaired", false);
    if (get_or<bool>(reply, "ok", false)) return true;
    if (error != nullptr) *error = get_or<std::string>(reply, "error", host.name + " said no");
    return false;
}

}  // namespace

// --- the record ---------------------------------------------------------------

std::string RemoteStartRecord::host_id() {
    json record = load_record(path_);
    std::string id = get_or<std::string>(record, "host_id", "");
    if (id.empty()) {
        id = random_hex(8);
        record["host_id"] = id;
        store_record(path_, record);
    }
    return id;
}

std::string RemoteStartRecord::show_code(int64_t now) {
    std::random_device device;
    std::uniform_int_distribution<int> digits(0, 999999);
    char code[8];
    std::snprintf(code, sizeof(code), "%06d", digits(device));
    json record = load_record(path_);
    record["code"] = code;
    record["code_until"] = now + kPairingCodeSeconds;
    store_record(path_, record);
    return code;
}

void RemoteStartRecord::hide_code() {
    json record = load_record(path_);
    if (!record.contains("code")) return;
    record.erase("code");
    record.erase("code_until");
    store_record(path_, record);
}

bool RemoteStartRecord::pair(const std::string& code, const std::string& device, int64_t now,
                             std::string& token) {
    json record = load_record(path_);
    const std::string showing = get_or<std::string>(record, "code", "");
    const int64_t until = get_or<int64_t>(record, "code_until", 0);
    if (showing.empty() || code != showing || now > until) return false;
    // Once: a code overheard is no good after it has been used.
    record.erase("code");
    record.erase("code_until");
    token = random_hex(16);
    if (!record.contains("devices") || !record["devices"].is_array()) record["devices"] = json::array();
    record["devices"].push_back({{"name", device}, {"token", token}});
    return store_record(path_, record);
}

bool RemoteStartRecord::paired(const std::string& token) {
    if (token.empty()) return false;
    const json record = load_record(path_);
    const auto devices = record.find("devices");
    if (devices == record.end() || !devices->is_array()) return false;
    return std::any_of(devices->begin(), devices->end(), [&](const json& entry) {
        return entry.is_object() && get_or<std::string>(entry, "token", "") == token;
    });
}

std::vector<std::string> RemoteStartRecord::paired_devices() {
    std::vector<std::string> names;
    const json record = load_record(path_);
    const auto devices = record.find("devices");
    if (devices == record.end() || !devices->is_array()) return names;
    for (const json& entry : *devices) {
        if (entry.is_object()) names.push_back(get_or<std::string>(entry, "name", ""));
    }
    return names;
}

void RemoteStartRecord::forget_devices() {
    json record = load_record(path_);
    record.erase("devices");
    store_record(path_, record);
}

// --- the PC end ---------------------------------------------------------------

LaunchService::LaunchService(Config config) : config_(std::move(config)) {
    if (config_.macs.empty()) config_.macs = this_machine_macs();
}

LaunchService::~LaunchService() { stop(); }

bool LaunchService::start(std::string* error) {
    stop();
    stopping_ = false;
    auto query = std::make_shared<UdpSocket>();
    auto listener = std::make_shared<TcpListener>();
    if (!query->start(config_.query_port, false, error)) return false;
    if (!listener->start(config_.port, error)) return false;
    RemoteStartRecord record(config_.record_path);
    const std::string host_id = record.host_id();
    thread_ = std::thread([this, query, listener, host_id] {
        struct Connection {
            TcpStream stream;
            std::vector<uint8_t> inbox;
            uint64_t deadline = 0;
            bool answered = false;
        };
        std::vector<Connection> connections;
        RemoteStartRecord record(config_.record_path);
        while (!stopping_) {
            query->wait_readable(connections.empty() ? 50 : 2);
            std::vector<uint8_t> datagram;
            std::string from;
            uint16_t from_port = 0;
            while (query->receive(datagram, from, from_port)) {
                if (datagram.size() < 4 || std::memcmp(datagram.data(), kQueryMagic, 4) != 0) continue;
                const json reply = {{"v", kLaunchVersion}, {"id", host_id},      {"name", config_.name},
                                    {"kind", config_.kind}, {"port", config_.port}, {"macs", config_.macs}};
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
                if (!connection.stream.pump(connection.inbox, nullptr)) {
                    connection.stream.close();
                    continue;
                }
                if (connection.answered) {
                    if (connection.stream.pending_bytes() == 0) connection.stream.close();
                    continue;
                }
                json request;
                const FrameState state = take_frame(connection.inbox, request);
                if (state == FrameState::Bad) {
                    connection.stream.close();
                    continue;
                }
                if (state == FrameState::Incomplete) {
                    if (monotonic_millis() >= connection.deadline) connection.stream.close();
                    continue;
                }
                json reply = {{"ok", false}};
                const std::string op = get_or<std::string>(request, "op", "");
                const std::string token = get_or<std::string>(request, "token", "");
                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                if (op == "pair") {
                    const std::string device = get_or<std::string>(request, "device", connection.stream.peer_address());
                    std::string new_token;
                    if (record.pair(get_or<std::string>(request, "code", ""), device, now, new_token)) {
                        reply = {{"ok", true}, {"token", new_token}};
                        log_info(device + " can start games here now");
                    } else {
                        reply["error"] = "That code is not the one showing on " + config_.name + ".";
                    }
                } else if (!record.paired(token)) {
                    reply["unpaired"] = true;
                    reply["error"] = "This device is not paired with " + config_.name + ".";
                } else if (op == "list") {
                    json games = json::array();
                    if (config_.games) {
                        for (const LaunchableGame& game : config_.games()) games.push_back(describe_game(game));
                    }
                    reply = {{"ok", true}, {"games", games}};
                } else if (op == "launch") {
                    // Only a game it would list: an id names one of those and
                    // nothing else, so nothing outside them can be started.
                    const std::string id = get_or<std::string>(request, "game", "");
                    std::vector<LaunchableGame> games;
                    if (config_.games) games = config_.games();
                    const auto found = std::find_if(games.begin(), games.end(),
                                                    [&](const LaunchableGame& game) { return game.id == id; });
                    std::string error;
                    if (found == games.end()) {
                        reply["error"] = "That game is not on " + config_.name + " any more.";
                    } else if (config_.launch && config_.launch(*found, &error)) {
                        reply = {{"ok", true}};
                        ++launches_;
                        log_info("starting " + found->title + " for a connected device");
                    } else {
                        reply["error"] = error.empty() ? found->title + " could not be started." : error;
                    }
                } else {
                    reply["error"] = "unknown request \"" + op + "\"";
                }
                connection.stream.send(make_frame(reply));
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
    log_info("other devices can start games here, on ports " + std::to_string(config_.query_port) + " and " +
             std::to_string(config_.port));
    return true;
}

void LaunchService::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    thread_.join();
}

// --- the device end -------------------------------------------------------------

std::vector<LaunchHost> search_launch_hosts(const LaunchSearch& search) {
    std::vector<LaunchHost> found;
    UdpSocket socket;
    std::string error;
    if (!socket.start(0, true, &error)) {
        log_warn("could not look for computers to start games on: " + error);
        return found;
    }
    const json asked = {{"v", kLaunchVersion}};
    std::vector<uint8_t> query(kQueryMagic, kQueryMagic + 4);
    const std::string text = asked.dump();
    query.insert(query.end(), text.begin(), text.end());

    const uint64_t started = monotonic_millis();
    const uint64_t deadline = started + static_cast<uint64_t>(std::max(search.timeout_ms, 1));
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
            LaunchHost host;
            host.id = get_or<std::string>(reply, "id", "");
            if (host.id.empty()) continue;
            if (std::any_of(found.begin(), found.end(), [&](const LaunchHost& other) { return other.id == host.id; })) {
                continue;
            }
            host.name = get_or<std::string>(reply, "name", from);
            host.kind = get_or<std::string>(reply, "kind", "pc");
            host.port = get_or<uint16_t>(reply, "port", kLaunchPort);
            host.macs = get_or<std::vector<std::string>>(reply, "macs", {});
            host.address = from;
            found.push_back(host);
        }
    }
    std::sort(found.begin(), found.end(), [](const LaunchHost& a, const LaunchHost& b) { return a.name < b.name; });
    return found;
}

std::string remote_start_token(const std::string& path, const std::string& host_id) {
    const json tokens = load_record(path);
    return get_or<std::string>(tokens, host_id.c_str(), "");
}

bool keep_remote_start_token(const std::string& path, const std::string& host_id, const std::string& token) {
    json tokens = load_record(path);
    if (token.empty()) {
        tokens.erase(host_id);
    } else {
        tokens[host_id] = token;
    }
    return store_record(path, tokens);
}

void LaunchHostFinder::start(LaunchSearch search, int every_ms) {
    stop();
    stopping_ = false;
    thread_ = std::thread([this, search, every_ms] {
        while (!stopping_) {
            std::vector<LaunchHost> found = search_launch_hosts(search);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                hosts_ = std::move(found);
            }
            // In short steps, so stop() never waits long.
            for (int waited = 0; waited < every_ms && !stopping_; waited += 50) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
    });
}

void LaunchHostFinder::stop() {
    if (!thread_.joinable()) return;
    stopping_ = true;
    thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    hosts_.clear();
}

std::vector<LaunchHost> LaunchHostFinder::hosts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return hosts_;
}

bool pair_with_host(const LaunchHost& host, const std::string& code, const std::string& device_name,
                    std::string& token, std::string* error) {
    json reply;
    if (!exchange(host, {{"op", "pair"}, {"code", code}, {"device", device_name}}, reply, error)) return false;
    bool unpaired = false;
    if (!answered_yes(host, reply, unpaired, error)) return false;
    token = get_or<std::string>(reply, "token", "");
    if (token.empty()) {
        if (error != nullptr) *error = host.name + " did not say how to ask again";
        return false;
    }
    return true;
}

bool list_host_games(const LaunchHost& host, const std::string& token, std::vector<LaunchableGame>& out,
                     bool& unpaired, std::string* error) {
    out.clear();
    unpaired = false;
    json reply;
    if (!exchange(host, {{"op", "list"}, {"token", token}}, reply, error)) return false;
    if (!answered_yes(host, reply, unpaired, error)) return false;
    const auto games = reply.find("games");
    if (games == reply.end() || !games->is_array()) return true;
    for (const json& entry : *games) {
        if (!entry.is_object()) continue;
        LaunchableGame game;
        game.id = get_or<std::string>(entry, "id", "");
        game.title = get_or<std::string>(entry, "title", "");
        game.system = get_or<std::string>(entry, "system", "");
        if (!game.id.empty()) out.push_back(game);
    }
    return true;
}

bool start_host_game(const LaunchHost& host, const std::string& token, const std::string& game_id,
                     bool& unpaired, std::string* error) {
    unpaired = false;
    json reply;
    if (!exchange(host, {{"op", "launch"}, {"token", token}, {"game", game_id}}, reply, error)) return false;
    return answered_yes(host, reply, unpaired, error);
}

// --- waking -------------------------------------------------------------------

std::vector<std::string> this_machine_macs() {
    std::vector<std::string> macs;
#if defined(__linux__) && !defined(__ANDROID__)
    std::set<std::string> seen;
    for (const LocalNetwork& network : local_networks()) {
        std::ifstream in("/sys/class/net/" + network.interface + "/address");
        std::string mac;
        if (in && std::getline(in, mac) && mac.size() == 17 && mac != "00:00:00:00:00:00" &&
            seen.insert(mac).second) {
            macs.push_back(mac);
        }
    }
#endif
    return macs;
}

std::vector<uint8_t> wake_on_lan_packet(const std::string& mac) {
    std::vector<uint8_t> address;
    std::string digits;
    for (const char c : mac) {
        if (std::isxdigit(static_cast<unsigned char>(c)) != 0) digits.push_back(c);
    }
    if (digits.size() != 12) return {};
    for (std::size_t at = 0; at < 12; at += 2) {
        address.push_back(static_cast<uint8_t>(std::stoi(digits.substr(at, 2), nullptr, 16)));
    }
    std::vector<uint8_t> packet(6, 0xFF);
    for (int copy = 0; copy < 16; ++copy) packet.insert(packet.end(), address.begin(), address.end());
    return packet;
}

bool wake_on_lan(const std::string& mac, std::string* error) {
    const std::vector<uint8_t> packet = wake_on_lan_packet(mac);
    if (packet.empty()) {
        if (error != nullptr) *error = "\"" + mac + "\" is not a hardware address";
        return false;
    }
    UdpSocket socket;
    if (!socket.start(0, true, error)) return false;
    // Both ports a network card listens on, a few times over: a lost
    // datagram is simply lost.
    bool sent = false;
    for (int round = 0; round < 3; ++round) {
        sent = socket.broadcast(9, packet.data(), packet.size()) || sent;
        sent = socket.broadcast(7, packet.data(), packet.size()) || sent;
    }
    if (!sent && error != nullptr) *error = "nothing could be sent on this network";
    return sent;
}

}  // namespace ds::net
