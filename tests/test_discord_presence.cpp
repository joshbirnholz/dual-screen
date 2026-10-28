// Telling Discord what is being played.
//
// Against a fake Discord: a socket where the real one keeps its own, which
// reads what is sent to it the way Discord does -- handshake, then commands --
// and answers the handshake with READY. Nothing here can show that Discord
// draws the result, only that what it is sent is what its documentation asks
// for; that part is for a real Discord.

#include "test_support.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "util/discord_presence.hpp"

using namespace ds;

namespace {

struct Frame {
    uint32_t opcode = 0;
    nlohmann::json body;
};

bool read_exactly(int fd, char* into, size_t length, int timeout_ms) {
    size_t got = 0;
    while (got < length) {
        pollfd waiting{fd, POLLIN, 0};
        if (::poll(&waiting, 1, timeout_ms) <= 0) return false;
        const ssize_t now = ::recv(fd, into + got, length - got, 0);
        if (now <= 0) return false;
        got += static_cast<size_t>(now);
    }
    return true;
}

bool read_frame(int fd, Frame* frame, int timeout_ms = 5000) {
    char header[8];
    if (!read_exactly(fd, header, 8, timeout_ms)) return false;
    uint32_t length = 0;
    frame->opcode = 0;
    for (int i = 0; i < 4; ++i) {
        frame->opcode |= static_cast<uint32_t>(static_cast<unsigned char>(header[i])) << (8 * i);
        length |= static_cast<uint32_t>(static_cast<unsigned char>(header[4 + i])) << (8 * i);
    }
    std::string json(length, '\0');
    if (!read_exactly(fd, &json[0], length, timeout_ms)) return false;
    frame->body = nlohmann::json::parse(json, nullptr, false);
    return true;
}

void send_frame(int fd, uint32_t opcode, const nlohmann::json& body) {
    const std::string bytes = discord_frame(opcode, body.dump());
    CHECK(::send(fd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size()));
}

int listen_at(const std::string& path) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    ::unlink(path.c_str());
    CHECK(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(::listen(fd, 4) == 0);
    return fd;
}

int accept_within(int listener, int timeout_ms) {
    pollfd waiting{listener, POLLIN, 0};
    if (::poll(&waiting, 1, timeout_ms) <= 0) return -1;
    return ::accept(listener, nullptr, nullptr);
}

void test_a_frame_is_opcode_then_length_little_endian() {
    const std::string frame = discord_frame(1, "{}");
    CHECK(frame.size() == 10);
    CHECK(frame[0] == 1 && frame[1] == 0 && frame[2] == 0 && frame[3] == 0);
    CHECK(frame[4] == 2 && frame[5] == 0 && frame[6] == 0 && frame[7] == 0);
    CHECK(frame.substr(8) == "{}");
}

void test_the_activity_names_the_game_and_clearing_sends_none() {
    DiscordActivity activity;
    activity.details = "Pok\xc3\xa9mon Platinum";
    activity.state = "Nintendo DS";
    activity.started = 1700000000;
    const nlohmann::json command = nlohmann::json::parse(discord_activity_command(activity, 42, "7"));
    CHECK(command["cmd"] == "SET_ACTIVITY");
    CHECK(command["nonce"] == "7");
    CHECK(command["args"]["pid"] == 42);
    const nlohmann::json& shown = command["args"]["activity"];
    CHECK(shown["details"] == "Pok\xc3\xa9mon Platinum");
    CHECK(shown["state"] == "Nintendo DS");
    CHECK(shown["timestamps"]["start"] == 1700000000);
    CHECK(shown["status_display_type"] == 2);
    CHECK(shown["assets"]["large_image"].get<std::string>().rfind("https://", 0) == 0);

    // Not a game's name, but a file's: still sent, never thrown on.
    activity.details = "bad \xff name";
    CHECK(nlohmann::json::parse(discord_activity_command(activity, 42, "8"), nullptr, false).is_object());

    const nlohmann::json cleared = nlohmann::json::parse(discord_activity_command(DiscordActivity(), 42, "9"));
    CHECK(cleared["cmd"] == "SET_ACTIVITY");
    CHECK(!cleared["args"].contains("activity"));
}

void test_the_flatpak_discord_is_looked_for_too() {
    ::setenv("XDG_RUNTIME_DIR", "/run/user/1000/", 1);
    const std::vector<std::string> paths = discord_socket_paths();
    CHECK(!paths.empty() && paths.front() == "/run/user/1000/discord-ipc-0");
    bool flatpak = false;
    for (const std::string& path : paths) {
        flatpak = flatpak || path == "/run/user/1000/app/com.discordapp.Discord/discord-ipc-0";
    }
    CHECK(flatpak);
}

void test_discord_is_told_and_told_again_after_it_restarts() {
    char root[] = "/tmp/ds-discord-XXXXXX";
    CHECK(::mkdtemp(root) != nullptr);
    ::setenv("XDG_RUNTIME_DIR", root, 1);
    const std::string socket_path = std::string(root) + "/discord-ipc-0";
    int listener = listen_at(socket_path);

    DiscordActivity playing;
    playing.details = "Mario Kart DS";
    playing.state = "Nintendo DS";
    playing.started = 1700000000;

    int client = -1;
    std::thread closer;
    {
        DiscordPresence presence("1234567890");
        presence.show(playing);

        client = accept_within(listener, 5000);
        CHECK(client >= 0);
        Frame frame;
        CHECK(read_frame(client, &frame));
        CHECK(frame.opcode == 0);
        CHECK(frame.body["v"] == 1);
        CHECK(frame.body["client_id"] == "1234567890");
        send_frame(client, 1, {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", {{"v", 1}}}});

        CHECK(read_frame(client, &frame));
        CHECK(frame.opcode == 1);
        CHECK(frame.body["args"]["activity"]["details"] == "Mario Kart DS");
        send_frame(client, 1, {{"cmd", "SET_ACTIVITY"}, {"evt", nullptr}, {"nonce", frame.body["nonce"]}});

        // Discord quitting and coming back: told again, without being asked.
        ::close(client);
        client = accept_within(listener, 15000);
        CHECK(client >= 0);
        CHECK(read_frame(client, &frame));
        CHECK(frame.opcode == 0);
        send_frame(client, 1, {{"cmd", "DISPATCH"}, {"evt", "READY"}});
        CHECK(read_frame(client, &frame));
        CHECK(frame.body["args"]["activity"]["details"] == "Mario Kart DS");

        // A new game replaces it.
        playing.details = "New Super Mario Bros.";
        presence.show(playing);
        CHECK(read_frame(client, &frame));
        CHECK(frame.body["args"]["activity"]["details"] == "New Super Mario Bros.");
        for (int wait = 0; wait < 100 && !presence.up_to_date(); ++wait) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(presence.up_to_date());

        // Shown once, however often the same thing is asked for.
        presence.show(playing);
        presence.show(playing);
        CHECK(!read_frame(client, &frame, 300));

        // ...and on the way out, cleared: read while it is being destroyed
        // at the end of this scope.
        closer = std::thread([client] {
            Frame last;
            CHECK(read_frame(client, &last));
            CHECK(last.body["cmd"] == "SET_ACTIVITY");
            CHECK(!last.body["args"].contains("activity"));
            ::close(client);
        });
    }
    closer.join();

    ::close(listener);
    ::unlink(socket_path.c_str());
    ::rmdir(root);
}

void test_no_discord_is_nothing_to_wait_for() {
    char root[] = "/tmp/ds-discord-XXXXXX";
    CHECK(::mkdtemp(root) != nullptr);
    ::setenv("XDG_RUNTIME_DIR", root, 1);
    ::setenv("TMPDIR", root, 1);
    const auto before = std::chrono::steady_clock::now();
    {
        DiscordPresence presence("1234567890");
        DiscordActivity playing;
        playing.details = "Mario Kart DS";
        presence.show(playing);
        CHECK(!presence.up_to_date());
    }
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
    ::rmdir(root);
}

}  // namespace

int main() {
    test_a_frame_is_opcode_then_length_little_endian();
    test_the_activity_names_the_game_and_clearing_sends_none();
    test_the_flatpak_discord_is_looked_for_too();
    test_discord_is_told_and_told_again_after_it_restarts();
    test_no_discord_is_nothing_to_wait_for();
    return test::finish("discord presence");
}
