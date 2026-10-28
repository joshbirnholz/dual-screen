#include "util/discord_presence.hpp"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "util/log.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__SWITCH__) || defined(__ANDROID__) || defined(_WIN32) || \
    (defined(__APPLE__) && TARGET_OS_IPHONE)
#define DS_DISCORD_PRESENCE 0
#else
#define DS_DISCORD_PRESENCE 1
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ds {

namespace {

// Discord shows a picture only from its own uploads or from the web, never one
// sent to it, so the game's own icon cannot be the picture: it is this app's,
// from the repository.
constexpr const char* kLargeImage =
    "https://raw.githubusercontent.com/joshbirnholz/dual-screen/claude/emulator-display-streaming-xnfjj0/"
    "assets/app_icon.png";

enum Opcode : uint32_t { Handshake = 0, Frame = 1, Close = 2 };

// A reply's "evt", which Discord sends as null on a plain answer.
bool reply_is(const std::string& json, const char* event) {
    const nlohmann::json reply = nlohmann::json::parse(json, nullptr, false);
    if (!reply.is_object()) return false;
    const auto found = reply.find("evt");
    return found != reply.end() && found->is_string() && *found == event;
}

}  // namespace

std::string discord_frame(uint32_t opcode, const std::string& json) {
    std::string frame(8, '\0');
    const uint32_t length = static_cast<uint32_t>(json.size());
    for (int i = 0; i < 4; ++i) {
        frame[i] = static_cast<char>((opcode >> (8 * i)) & 0xff);
        frame[4 + i] = static_cast<char>((length >> (8 * i)) & 0xff);
    }
    return frame + json;
}

std::string discord_activity_command(const DiscordActivity& activity, long process_id, const std::string& nonce) {
    nlohmann::json args = {{"pid", process_id}};
    if (!activity.details.empty()) {
        nlohmann::json shown = {
            {"type", 0},
            // The member list says "Playing <the game>" rather than "Playing
            // Dualscreen".
            {"status_display_type", 2},
            {"details", activity.details},
            {"assets", {{"large_image", kLargeImage}, {"large_text", "Dualscreen"}}},
        };
        if (!activity.state.empty()) shown["state"] = activity.state;
        if (activity.started != 0) shown["timestamps"] = {{"start", static_cast<int64_t>(activity.started)}};
        args["activity"] = shown;
    }
    const nlohmann::json command = {{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", nonce}};
    // Replaced rather than refused: a game's name is whatever its file says.
    return command.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::vector<std::string> discord_socket_paths() {
    std::vector<std::string> bases;
    for (const char* name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && *value != '\0') bases.emplace_back(value);
    }
    bases.emplace_back("/tmp");
    std::vector<std::string> paths;
    for (std::string base : bases) {
        while (base.size() > 1 && base.back() == '/') base.pop_back();
        for (const char* within :
             {"", "/app/com.discordapp.Discord", "/snap.discord", "/.flatpak/dev.vencord.Vesktop/xdg-run"}) {
            for (int number = 0; number < 10; ++number) {
                std::string path = base + within + "/discord-ipc-" + std::to_string(number);
                bool seen = false;
                for (const std::string& earlier : paths) seen = seen || earlier == path;
                if (!seen) paths.push_back(std::move(path));
            }
        }
    }
    return paths;
}

DiscordPresence::DiscordPresence(std::string application_id) : application_id_(std::move(application_id)) {
#if DS_DISCORD_PRESENCE
    if (!application_id_.empty()) thread_ = std::thread([this] { run(); });
#endif
}

DiscordPresence::~DiscordPresence() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void DiscordPresence::show(const DiscordActivity& activity) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activity == wanted_) return;
        wanted_ = activity;
        told_ = false;
    }
    wake_.notify_all();
}

bool DiscordPresence::up_to_date() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return told_;
}

#if !DS_DISCORD_PRESENCE

void DiscordPresence::run() {}

#else

namespace {

bool send_all(int fd, const std::string& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t now = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
#else
        const ssize_t now = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
#endif
        if (now < 0 && errno == EINTR) continue;
        if (now < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd waiting{fd, POLLOUT, 0};
            if (::poll(&waiting, 1, 1000) <= 0) return false;
            continue;
        }
        if (now <= 0) return false;
        sent += static_cast<size_t>(now);
    }
    return true;
}

// Discord's replies arrive whole or not at all in practice, but a frame split
// across reads is still put back together. Returns false once the socket is
// closed or broken, or Discord closes it on purpose.
class Reader {
public:
    // Reads what is waiting, waiting up to `timeout_ms` for the first of it.
    // `on_frame` is called with each whole frame's opcode and JSON.
    template <typename OnFrame>
    bool pump(int fd, int timeout_ms, OnFrame on_frame) {
        pollfd waiting{fd, POLLIN, 0};
        const int ready = ::poll(&waiting, 1, timeout_ms);
        if (ready < 0) return errno == EINTR;
        if (ready == 0) return true;
        char chunk[4096];
        const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
        if (got < 0) return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
        if (got == 0) return false;
        buffer_.append(chunk, static_cast<size_t>(got));
        while (buffer_.size() >= 8) {
            uint32_t opcode = 0;
            uint32_t length = 0;
            for (int i = 0; i < 4; ++i) {
                opcode |= static_cast<uint32_t>(static_cast<unsigned char>(buffer_[i])) << (8 * i);
                length |= static_cast<uint32_t>(static_cast<unsigned char>(buffer_[4 + i])) << (8 * i);
            }
            if (length > 1u << 20) return false;
            if (buffer_.size() < 8 + static_cast<size_t>(length)) break;
            const std::string json = buffer_.substr(8, length);
            buffer_.erase(0, 8 + static_cast<size_t>(length));
            if (opcode == Close) {
                log_debug("Discord closed its connection: " + json);
                return false;
            }
            on_frame(opcode, json);
        }
        return true;
    }

private:
    std::string buffer_;
};

int connect_to_discord(std::string* where) {
    for (const std::string& path : discord_socket_paths()) {
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path)) continue;
        if (::access(path.c_str(), F_OK) != 0) continue;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
#ifdef SO_NOSIGPIPE
        const int on = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            *where = path;
            return fd;
        }
        ::close(fd);
    }
    return -1;
}

}  // namespace

void DiscordPresence::run() {
    using Clock = std::chrono::steady_clock;
    int fd = -1;
    Reader reader;
    Clock::time_point next_look = Clock::now();
    long nonce = 0;
    bool said_connected = false;

    const auto hang_up = [&] {
        if (fd >= 0) ::close(fd);
        fd = -1;
        reader = Reader();
        next_look = Clock::now() + std::chrono::seconds(5);
        std::lock_guard<std::mutex> lock(mutex_);
        told_ = false;
    };

    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (stopping_) break;
        }

        if (fd < 0 && Clock::now() >= next_look) {
            std::string where;
            fd = connect_to_discord(&where);
            bool ready = false;
            if (fd >= 0) {
                const nlohmann::json hello = {{"v", 1}, {"client_id", application_id_}};
                if (send_all(fd, discord_frame(Handshake, hello.dump()))) {
                    // READY, or a close saying why not -- a wrong application
                    // id, say -- within a couple of seconds.
                    const Clock::time_point give_up = Clock::now() + std::chrono::seconds(3);
                    while (!ready && Clock::now() < give_up) {
                        const bool open = reader.pump(fd, 250, [&](uint32_t, const std::string& json) {
                            ready = ready || reply_is(json, "READY");
                        });
                        if (!open) break;
                    }
                }
            }
            if (ready) {
                if (!said_connected) log_info("Discord will show what is being played");
                said_connected = true;
                log_debug("Discord connected at " + where);
            } else {
                hang_up();
                // Not there, or not wanting to talk: a Discord that has just
                // been opened takes a while. Looked for again in a few seconds.
                next_look = Clock::now() + std::chrono::seconds(where.empty() ? 5 : 15);
            }
        }

        if (fd >= 0) {
            DiscordActivity activity;
            bool tell = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                tell = !told_;
                activity = wanted_;
            }
            if (tell) {
                if (send_all(fd, discord_frame(Frame, discord_activity_command(activity, static_cast<long>(::getpid()),
                                                                                std::to_string(++nonce))))) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // Unless it changed again while being sent.
                    told_ = activity == wanted_;
                } else {
                    hang_up();
                }
            }
            // Replies are read and dropped so they never fill the socket; a
            // refused activity is only worth a debug line.
            if (fd >= 0 && !reader.pump(fd, 0, [](uint32_t, const std::string& json) {
                    if (reply_is(json, "ERROR")) {
                        log_debug("Discord refused the activity: " + json);
                    }
                })) {
                hang_up();
            }
        }

        std::unique_lock<std::mutex> lock(mutex_);
        if (stopping_) break;
        // Woken early by a new activity; otherwise often enough to notice
        // Discord going away or arriving.
        wake_.wait_for(lock, std::chrono::milliseconds(fd >= 0 ? 1000 : 500),
                       [&] { return stopping_ || (fd >= 0 && !told_); });
    }

    if (fd >= 0) {
        // Cleared on the way out rather than left to Discord noticing the
        // process has gone, which it does, but slowly.
        send_all(fd, discord_frame(Frame, discord_activity_command(DiscordActivity(),
                                                                    static_cast<long>(::getpid()), "bye")));
        ::close(fd);
    }
}

#endif

}  // namespace ds
