#pragma once

#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ds {

// What a player's friends see under their name in Discord while a game is
// going: its name, and what it is being played as.
struct DiscordActivity {
    // The game's name, as the console calls it. Empty means nothing is being
    // played, and the status is cleared.
    std::string details;
    // The line under it: the console.
    std::string state;
    // When the game started, for Discord's "elapsed" clock.
    std::time_t started = 0;
    // The game's own icon, as a web address -- see discord_game_icon_url().
    // Shown only once it is known to be there, with this app's icon small
    // in its corner; this app's icon alone otherwise.
    std::string image;

    bool operator==(const DiscordActivity& other) const {
        return details == other.details && state == other.state && started == other.started &&
               image == other.image;
    }
    bool operator!=(const DiscordActivity& other) const { return !(*this == other); }
};

// Discord's Rich Presence, spoken directly to the Discord app on this machine
// over its local socket -- the same thing Discord's own SDK does, without the
// SDK. There is nothing to sign in to: Discord only shows it if it is running
// and the player has activity sharing on, and nothing here waits on either.
//
// All of the talking happens on a thread of its own, so a Discord that is
// slow, missing, or restarted mid-game never costs a frame: show() only
// stores what to say. While Discord is not there it is looked for again every
// few seconds, and told the current activity as soon as it turns up.
//
// A no-op on a Switch, an Android device or an iPhone, which have no Discord
// app to talk to.
class DiscordPresence {
public:
    // `application_id` is the Discord application whose name heads the status
    // ("Playing Dualscreen"). Empty turns the whole thing off.
    // `image_exists` says whether an activity's image is really there, from
    // the talking thread; by default curl asks the server.
    using ImageCheck = std::function<bool(const std::string& url)>;
    explicit DiscordPresence(std::string application_id, ImageCheck image_exists = ImageCheck());
    ~DiscordPresence();

    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    // Cheap enough to call every frame: it does nothing unless the activity
    // has changed.
    void show(const DiscordActivity& activity);

    // Whether Discord is connected and has been told the latest activity.
    // For the tests.
    bool up_to_date() const;

private:
    void run();

    std::string application_id_;
    ImageCheck image_exists_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    DiscordActivity wanted_;
    bool told_ = false;
    bool stopping_ = false;
    std::thread thread_;
};

// The pieces of the protocol, here so the tests can check them.
//
// A frame is an opcode and a length, both 32-bit little-endian, and then that
// many bytes of JSON.
std::string discord_frame(uint32_t opcode, const std::string& json);
// Where a game's own icon is kept for Discord to show: the icons branch of
// this app's repository, filled once from Nintendo's own icon servers (see
// tools/build_icons.py there) and DS-Scene's DS icons. `key` is
// "3ds/<title ID>", "wiiu/<title ID>" or "ds/<ds_icon_key()>".
std::string discord_game_icon_url(const std::string& key);

// The SET_ACTIVITY command for `activity`, or the one that clears it when its
// details are empty. Its image is used as given: the caller has checked it.
std::string discord_activity_command(const DiscordActivity& activity, long process_id, const std::string& nonce);
// Where Discord's socket can be, most likely first: discord-ipc-0 to -9 under
// $XDG_RUNTIME_DIR, $TMPDIR and /tmp, including where the Flatpak and Snap
// builds and Vesktop put theirs.
std::vector<std::string> discord_socket_paths();

}  // namespace ds
