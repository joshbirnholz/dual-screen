#include "app.hpp"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <utility>

#include "audio/audio_output.hpp"
#include "input/motion.hpp"
#include "input/motion_compare.hpp"
#include "config/game_config.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "config/sha1.hpp"
#include "core/external_screen_source.hpp"
#include "core/gl_call_trace.hpp"
#include "core/libretro_screen_source.hpp"
#include "core/game_title.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "input/pad_routing.hpp"
#include "input/retro_keymap.hpp"
#include "layout/layout_registry.hpp"
#include "render/bezel_art.hpp"
#include "render/button_glyphs.hpp"
#include "render/compositor.hpp"
#include "render/menu_backdrop.hpp"
#include "ui/menu_actions.hpp"
#include "render/gl_loader.hpp"
#include "render/gl_window.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/layout_picker.hpp"
#include "ui/menu_state.hpp"
#include "net/companion_session.hpp"
#include "net/host_session.hpp"
#include "ui/connecting_screen.hpp"
#include "ui/controllers_screen.hpp"
#include "ui/game_browser.hpp"
#include "ui/launcher_screen.hpp"
#include "ui/message_screen.hpp"
#include "ui/settings_screen.hpp"
#include "util/child_process.hpp"
#include "util/build_stamp.hpp"
#include "util/log.hpp"
#include "util/shutdown_watchdog.hpp"

namespace ds {
namespace {

// Anything longer than this between frames means the machine was asleep (or the
// process was stopped), not that we ran slow. The frame clock is restarted and
// the stale audio queue thrown away rather than played out.
constexpr double kSuspendGapSeconds = 1.0;
// How long leaving may take before it is abandoned -- see ShutdownWatchdog.
// Well past the three seconds an emulator is given to close its own windows
// before it is ended, so a slow but working exit is never cut short.
constexpr std::chrono::milliseconds kShutdownDeadline{10000};

// How often the cartridge save is checked for changes and written out. A
// handheld gets put to sleep and powered off without warning, so waiting for a
// clean exit to write the save is not good enough.
constexpr double kSaveFlushIntervalSeconds = 5.0;

std::string next_argument(int argc, char** argv, int& index, const char* flag, std::string& error) {
    if (index + 1 >= argc) {
        error = std::string("missing value after ") + flag;
        return std::string();
    }
    return std::string(argv[++index]);
}

// Picks the core for a system: an explicit --core, then settings.json, then
// $DUALSCREEN_CORE, then the usual install locations. The first two and the
// environment name one core rather than one per system, so they only apply to
// the system actually being asked for when they are set.
// {socket} and {game} in an argument for the program that plays an external
// console -- see Settings::wii_u_arguments. Anywhere in the argument rather than
// only as the whole of it, since a fork may well want --socket=<path>.
std::string fill_placeholders(const std::string& argument, const std::string& socket_path,
                              const std::string& rom_path) {
    std::string filled = argument;
    for (const auto& pair : {std::pair<const char*, const std::string*>{"{socket}", &socket_path},
                             std::pair<const char*, const std::string*>{"{game}", &rom_path}}) {
        const std::string token = pair.first;
        for (std::size_t at = filled.find(token); at != std::string::npos;
             at = filled.find(token, at + pair.second->size())) {
            filled.replace(at, token.size(), *pair.second);
        }
    }
    return filled;
}

std::string resolve_core_path(const AppOptions& options,
                              const Settings& settings,
                              SystemId system) {
    if (!options.core_path.empty()) {
        return expand_user_path(options.core_path);
    }
    if (!settings.core_path_for(system).empty()) {
        return expand_user_path(settings.core_path_for(system));
    }
    const char* from_environment = std::getenv(core_path_environment_variable(system));
    if (from_environment != nullptr && from_environment[0] != '\0') {
        return expand_user_path(from_environment);
    }
    for (const std::string& candidate : core_search_paths(system)) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return std::string();
}

// One entry per console for the Settings screen, so a machine set up for one
// and not another says so before a game is picked rather than after.
std::vector<SettingsScreen::CoreStatus> resolve_core_statuses(const AppOptions& options,
                                                              const Settings& settings) {
    std::vector<SettingsScreen::CoreStatus> statuses;
    for (const SystemProfile& profile : system_profiles()) {
        // Where this console is played from, whichever kind of thing that is.
        // What the player needs to know is the same either way: is it there.
        switch (profile.frame_origin) {
            case FrameOrigin::LibretroCore:
                statuses.push_back(
                    {profile.name, resolve_core_path(options, settings, profile.id)});
                break;
            case FrameOrigin::External:
                statuses.push_back({profile.name, settings.external_program_for(profile.id)});
                break;
        }
    }
    return statuses;
}

}  // namespace

std::string usage_text() {
    return
        "dualscreen - a dual-screen shell for DS and 3DS emulation\n"
        "\n"
        "Usage:\n"
        "  dualscreen --game /path/to/rom.nds [options]\n"
        "\n"
        "Options:\n"
        "  --game <path>        ROM to boot straight into\n"
        "  --core <path>        libretro core (.so) to load, instead of the one\n"
        "                       found for this game's console\n"
        "  --layout <id>        Override the layout for this run\n"
        "  --system-dir <path>  Folder holding your BIOS/firmware files\n"
        "  --windowed           Run in a window instead of fullscreen\n"
        "  --connect <screen>   Join the first game found on the network as its\n"
        "                       top or bottom screen, skipping the menu\n"
        "  --external-socket <path>\n"
        "                       Where a console played by another program hands\n"
        "                       its screens over, instead of the usual place\n"
        "  --video-codec <how>  How to send screens to a second device for this\n"
        "                       run: tiles or h264. Overrides whichever console\n"
        "                       is played this run's own streaming format setting\n"
        "  --list-layouts       Print the available layout ids and exit\n"
        "  --verbose            Log core and frontend debug output\n"
        "  --trace-core-gl      Log every OpenGL call a GPU-rendering core makes,\n"
        "                       and which the driver rejected. Slow; for when such\n"
        "                       a core draws nothing and does not say why\n"
        "  --help               Show this text\n"
        "\n"
        "Run with no --game to pick one from a file browser instead.\n"
        "\n"
        "Layouts are remembered per game, keyed by the ROM's SHA-1, in\n"
        "~/.config/dualscreen/games/. The layout menu opens on F1 or a\n"
        "two-finger tap; bind F1 in Steam Input to reach it from a pad.\n";
}

AppOptions parse_command_line(int argc, char** argv) {
    AppOptions options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            options.show_help = true;
        } else if (argument == "--game") {
            options.game_path = next_argument(argc, argv, index, "--game", options.parse_error);
        } else if (argument == "--core") {
            options.core_path = next_argument(argc, argv, index, "--core", options.parse_error);
        } else if (argument == "--layout") {
            options.layout_id = next_argument(argc, argv, index, "--layout", options.parse_error);
        } else if (argument == "--system-dir") {
            options.system_dir = next_argument(argc, argv, index, "--system-dir", options.parse_error);
        } else if (argument == "--windowed") {
            options.windowed = true;
        } else if (argument == "--trace-core-gl") {
            options.trace_core_gl = true;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--list-layouts") {
            options.list_layouts = true;
        } else if (argument == "--exit-after-frames") {
            const std::string value =
                next_argument(argc, argv, index, "--exit-after-frames", options.parse_error);
            options.exit_after_frames = std::atoi(value.c_str());
        } else if (argument == "--video-codec") {
            options.video_codec_override =
                next_argument(argc, argv, index, "--video-codec", options.parse_error);
            if (options.video_codec_override != "tiles" &&
                options.video_codec_override != "h264") {
                options.parse_error = "--video-codec wants 'tiles' or 'h264'";
            }
        } else if (argument == "--external-socket") {
            options.external_socket =
                next_argument(argc, argv, index, "--external-socket", options.parse_error);
        } else if (argument == "--connect") {
            options.auto_connect_screen =
                next_argument(argc, argv, index, "--connect", options.parse_error);
            if (options.auto_connect_screen != "top" && options.auto_connect_screen != "bottom") {
                options.parse_error = "--connect wants 'top' or 'bottom'";
            }
        } else if (argument == "--start-screen") {
            options.start_screen =
                next_argument(argc, argv, index, "--start-screen", options.parse_error);
        } else if (argument == "--simulate-touch") {
            const std::string value =
                next_argument(argc, argv, index, "--simulate-touch", options.parse_error);
            const std::size_t comma = value.find(',');
            if (comma == std::string::npos) {
                options.parse_error = "--simulate-touch wants x,y as fractions of the window";
            } else {
                options.simulate_touch_x = std::strtof(value.substr(0, comma).c_str(), nullptr);
                options.simulate_touch_y = std::strtof(value.substr(comma + 1).c_str(), nullptr);
            }
        } else if (argument == "--latency-probe") {
            options.latency_probe = true;
        } else if (argument == "--simulate-key") {
            options.simulate_key = next_argument(argc, argv, index, "--simulate-key", options.parse_error);
        } else if (argument == "--exit-after-seconds") {
            const std::string value =
                next_argument(argc, argv, index, "--exit-after-seconds", options.parse_error);
            options.exit_after_seconds = std::strtod(value.c_str(), nullptr);
        } else if (argument == "--screenshot-at-seconds") {
            const std::string value =
                next_argument(argc, argv, index, "--screenshot-at-seconds", options.parse_error);
            options.screenshot_at_seconds = std::strtod(value.c_str(), nullptr);
        } else if (argument == "--screenshot") {
            options.screenshot_path =
                next_argument(argc, argv, index, "--screenshot", options.parse_error);
        } else if (!argument.empty() && argument[0] != '-' && options.game_path.empty()) {
            // Bare path, so `dualscreen rom.nds` works too.
            options.game_path = argument;
        } else {
            options.parse_error = "unrecognised argument: " + argument;
        }
        if (!options.parse_error.empty()) {
            break;
        }
    }
    return options;
}

// ---------------------------------------------------------------------------

struct App::Impl {
    enum class State {
        Playing,
        Menu,
        Settings,
        Controllers,
        Message,
        Browser,
        Launcher,
        HostPicker,
        Companion
    };

    AppOptions options;
    AppPaths paths;
    Settings settings;
    LayoutRegistry registry;
    GameConfig game_config;
    bool have_game_config = false;

    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;
    int output_width = 1280;
    int output_height = 800;

    QuadRenderer quads;
    TextRenderer text;
    Compositor compositor;
    MenuBackdrop menu_backdrop;
    // Whether the game was running last frame, so a pause can be noticed once
    // rather than acted on every frame it lasts.
    bool game_was_running = false;
    ButtonGlyphSheet button_glyphs;
    LocalOutputRouter router;
    // Lives as long as the GL context does, and no longer: created just after
    // the window, destroyed just before it.
    BezelArt bezel_art;
    InputManager input;
    // Only ever fed while this device and a companion both report motion, so
    // it costs nothing on the ordinary path. See MotionComparison for what it
    // is for.
    MotionComparison motion_comparison;
    AudioOutput audio;

    std::unique_ptr<LibretroCoreScreenSource> source;
    // The other kind of console: one played by a program beside this app rather
    // than by a core inside it. Only one of the two is ever set -- which, the
    // system's FrameOrigin decides.
    std::unique_ptr<ExternalScreenSource> external_source;
    // And the program that is playing it, when this app was the one to start
    // it. Stopping it is as much the point as starting it: in Game Mode there
    // is nothing left to close a window the shell walked away from.
    ChildProcess external_program;
    std::unique_ptr<LayoutPicker> picker;
    std::unique_ptr<SettingsScreen> settings_screen;
    // Opened from Settings, and drawn over it -- see ControllersScreen.
    ControllersScreen controllers_screen;
    // The game running here, which its save points are named after.
    std::string game_path;
    std::unique_ptr<GameBrowser> browser;
    std::unique_ptr<LauncherScreen> launcher;
    std::unique_ptr<HostPickerScreen> host_picker;
    MessageScreen message;

    // Two-device mode. Exactly one of these is live at a time: a device either
    // runs the game and serves a screen, or joins someone else's game.
    std::unique_ptr<net::HostSession> host_session;
    std::unique_ptr<net::CompanionSession> companion_session;
    ScreenId local_screen = ScreenId::Top;
    // Which single screen this device is when it is on its own and the
    // console's two are separate pictures -- see SoloScreens. Seeded from
    // Settings::wii_u_screen_alone, where the reasoning lives, and reseeded
    // whenever that setting changes; F3 flips it for the rest of a session
    // without writing anything down.
    ScreenId solo_screen = ScreenId::Top;
    // Whether F3 has flipped it for this session. Kept apart from the setting
    // rather than written over solo_screen, because on Auto that is worked
    // out again whenever the display changes, and a flip has to survive it.
    bool solo_screen_swapped = false;
    bool said_solo_screen = false;
    // Works solo_screen out again from the setting, the display, and any flip.
    // When a game starts, and whenever the setting or F3 changes it -- not
    // continuously. A Steam Deck's display does not change size under a
    // running app, docked or not, until the app is started again, so the
    // answer at the start of a game is the answer for the whole of it.
    void update_solo_screen();
    // Set when a game starts; the loop does the check and clears it, which it
    // can only do once there is a window to ask about -- a game started from
    // the command line starts before there is one.
    bool solo_screen_unchecked = true;
    // Where such a console hands its screens over, for the waiting screen to
    // name while nothing has.
    std::string external_socket_path;
    // Where the settings screen was opened from, so closing it goes back
    // there. It used to be reachable only before a game was running, when the
    // answer could not be anything but the main menu.
    State settings_came_from = State::Launcher;
    // Whether this app started the program playing an external console, so
    // that one which exits without ever connecting is noticed rather than
    // waited for indefinitely. Cleared once that has been reported.
    bool external_program_wanted = false;
    // Whether the program playing an external console has connected yet. Only
    // once it has does the link know what it is sending, which is when the
    // frame clock and the sound device can be set to match it.
    bool external_game_arrived = false;
    // The window size last told to the host, on a companion. Compared every
    // frame against the real one so a resize is reported without a dedicated
    // resize hook, and so the very first report (0 against whatever the
    // window actually is) goes out the moment there is a host to tell.
    int companion_reported_width = 0;
    int companion_reported_height = 0;
    // Whether the companion was connected as of last frame, purely to catch
    // the edge into a fresh connection: a new host has no memory of a size we
    // told a previous one, so that edge has to force a resend even when the
    // window happens not to have changed since.
    bool companion_was_connected = false;
    // The screen last asked of an external producer (a Wii U's Cemu) via
    // ExternalScreenSource::request_screen() -- absent means nothing has been
    // asked for. Tracked so the host poll loop only calls request_screen()
    // when what is wanted actually changes, the same as send_input() dedupes
    // on its own side of the same link.
    std::optional<ScreenId> external_requested_screen;
    // The size cap last asked for alongside it, tracked for the same reason
    // and on the same terms: zero means no cap was asked for.
    int external_requested_width = 0;
    int external_requested_height = 0;
    // And the screen the producer was last told to show in its own window,
    // which is a different question from the one above -- see
    // ExternalFrameLink::request_screen(). Optional only to mean "nothing has
    // been said yet", so that the first poll always says something: this end
    // always has an opinion about it, and with nobody else connected that
    // opinion is the whole point (Settings::wii_u_screen_alone).
    std::optional<ScreenId> external_shown_screen;
    // Whether anything has yet been pressed on a console played by another
    // program -- see where it is set for what its absence from a log means.
    bool said_first_pad = false;
    bool said_first_motion = false;
    // Whose controls the console's pad was last said to take -- see
    // console_pad_owner(). Said again whenever it changes.
    bool said_pad_owner = false;
    PadOwner last_pad_owner = PadOwner::ThisDevice;
    // On a Wii U, which pad is the GamePad and which is each player -- see
    // pad_routing.hpp. `pad_order` is the line as the Controllers screen left
    // it this session; nothing is written down, because a pad is only known
    // for as long as it stays connected.
    PadOrder pad_order;
    // Every pad as of the last frame, and where each went, in the same order.
    std::vector<KnownPad> known_pads;
    std::vector<PadSlot> pad_slots;
    // The last assignment written to the log, so it is said once per change.
    std::string said_pad_slots;
    // Whether the game has each motor on: the GamePad's, then Player 1 to 8.
    bool slot_rumble[1 + kPadPlayers] = {};
    // The pads whose motors this app has turned on, by pad_identity(), so that one
    // leaving a rumbling slot -- or the slot going quiet -- is turned off.
    std::map<std::string, KnownPad> rumbling_pads;
    // An older companion buzzing as a whole so the player can tell which it
    // is, and until when -- see identify_pad(). A newer one ends its own.
    std::optional<KnownPad> identifying_pad;
    uint32_t identify_until_ms = 0;
    // Where the Controllers screen was opened from, which is where closing it
    // goes back to.
    State controllers_came_from = State::Playing;
    // Whether the Wii U list shown in place of "Playing on Cemu" has had the
    // window's focus, as last logged -- see LayoutPickerState::embedded.
    bool embedded_menu_focused = false;
    // Whether the external producer was connected as of last frame. A
    // companion can connect, and so decide what is wanted, before Cemu ever
    // does -- request_screen() sent in that gap has nowhere to go and is
    // silently dropped (the control socket has no peer yet), so the rising
    // edge here is a second reason to resend, on top of the value itself
    // changing.
    bool external_link_was_running = false;

    State state = State::Playing;
    bool message_is_fatal = false;
    bool running = true;
    std::string active_layout_id;
    bool bilinear_filtering = false;
    // Which device makes the sound. Only meaningful once two are connected.
    // DS/3DS only -- a Wii U's TV and GamePad audio are not a player choice
    // at all, see the External source's own audio callback.
    AudioTarget audio_target = AudioTarget::Both;
    // Which layouts are flipped for a left hand, by id -- each chosen on its
    // own. Shared with the other device, since it changes what both of them
    // are drawing.
    std::vector<std::string> left_handed_layouts;
    Handedness handedness_of(const std::string& layout_id) const {
        return std::find(left_handed_layouts.begin(), left_handed_layouts.end(), layout_id) !=
                       left_handed_layouts.end()
                   ? Handedness::Left
                   : Handedness::Right;
    }
    // The hand the layout in use is drawn for.
    Handedness current_handedness() const { return handedness_of(active_layout().id); }
    // How many pixels the core renders per console pixel. Only the host runs a
    // core, but the menu is one menu wherever it is opened from, so this is
    // shared like everything else on it.
    int render_scale = 1;
    // Whether the current layout shows the gap between its screens. Shared
    // like the settings above, though it is only ever asked about while the
    // active layout offers the choice -- see LayoutComposition::gap_toggleable.
    bool screen_gap = true;
    // What the right stick is read as, on a console that has one of its own.
    RightStickMode right_stick_mode = RightStickMode::CStick;
    std::string game_title;
    std::string resolved_core_path;
    // Which console the loaded game belongs to, and its screens. On a companion
    // both come from the host; on a device running a game, from the ROM.
    SystemId system = SystemId::Ds;

    // The screens whatever is being drawn actually has. Every layout, touch and
    // preview goes through this rather than assuming a DS's.
    SystemScreens screens_geometry() const;

    uint64_t last_counter = 0;
    double frame_period = 1.0 / 60.0;
    double frame_accumulator = 0.0;
    double save_flush_accumulator = 0.0;
    bool awaiting_auto_connect = false;
    // The host being joined, kept so the connecting screen can name it before
    // the handshake has told this device anything.
    net::DiscoveredHost joining_host;
    bool screenshot_taken = false;
    uint32_t companion_audio_rate = 0;
    int frames_rendered = 0;

    bool create_window(GlContextNeed need, std::string* error);
    void destroy_window();
    // Rebuilds the window and everything that lives in its GL context when the
    // system about to be played needs a different one. A no-op when the context
    // already in hand will do.
    bool ensure_gl_context(GlContextNeed need, std::string* error);
    // Rebuilds what lives in the GL context after the window is replaced.
    bool rebuild_renderer(std::string* error);
    // What the current context was built for.
    GlContextNeed gl_context_need = GlContextNeed::Any;
    void refresh_output_size();
    const Layout& active_layout() const;
    // What this device actually draws: the chosen layout, resolved for this
    // device's role when it is a per-device layout.
    Layout device_layout() const;
    // The same, but for the companion rather than this device -- what decides
    // how big a picture a screen streamed to it is actually worth being. Zero
    // in both when there is no companion or it has not reported its window
    // size yet, which read_screen() already treats as "use the console's own
    // resolution".
    void companion_target_size(ScreenId screen, int& out_width, int& out_height) const;
    // The screen source currently feeding the compositor, whichever role we are
    // in. Null before anything is running.
    ScreenSource* active_source();
    // Whatever is producing this device's own game, whichever kind it is. Null
    // on a companion, which is showing somebody else's. Everything that only
    // wants to know "is a game running here" asks this rather than naming one
    // of the two kinds and quietly meaning both.
    ScreenSource* local_game();
    const ScreenSource* local_game() const;
    // What the game here runs at, whichever kind it is. An external one does
    // not know until the program playing it says so, which is what
    // adopt_external_timing() is for.
    // Whether this session should ask for video rather than changed tiles.
    // Three answers folded into one: what this run was told, what the player
    // settled on, and failing both, what the console being played wants.
    bool wants_video() const;
    double local_frames_per_second() const;
    double local_audio_sample_rate() const;
    void adopt_external_timing();
    void start_companion(ScreenId wanted_screen);
    void open_launcher();
    // The session this device joined has ended, however it ended. Called from
    // one place for every screen it might have been on, because the game is not
    // the only one: a host that quits while this device is in the menu leaves
    // it holding a menu about a session that no longer exists.
    void end_companion_session();
    // Puts up this device's settings, remembering what to go back to. Reached
    // from the main menu and, now, from a game's own menu -- and on a host,
    // possibly because the other device asked.
    void open_settings();
    // What to do about whatever that screen just decided -- called for a press
    // made here and for one the companion sent, the same way the menu is.
    void apply_settings_result(const SettingsScreenResult& result);
    // Plays what arrives from the host, opening this device's audio output the
    // first time something does -- the rate is not known until then.
    void attach_companion_callbacks();
    // Pushes the current choice down to whichever source is drawing, and
    // remembers it. Cheap enough to call whenever it changes.
    void apply_filtering();
    // Everything that follows from which console is being played: the picture
    // the Standard layout draws, and whether the left stick is also a d-pad.
    // Called whenever the console changes, which on a companion means whenever
    // the host says so.
    void apply_system();
    // True while this device is showing another device's game.
    bool is_companion() const;
    // Whether this device is the one being held as the GamePad, which is the
    // one showing the GamePad's screen -- the bottom one. With a companion
    // connected that is whichever screen the companion did not take; alone it
    // is what Settings::wii_u_screen_alone says this device is being.
    bool gamepad_is_here() const;
    // Every controller on both devices, in order: this device's, then the
    // companion's. A companion too old to list its pads is one pad.
    std::vector<KnownPad> gather_pads(bool companion_here) const;
    // Whether a companion is joined, and whether this device then counts as
    // holding the GamePad -- see route_wii_u_pads().
    bool companion_joined() const { return host_session && host_session->has_companion(); }
    bool holds_gamepad() const { return !companion_joined() || gamepad_is_here(); }
    // The Controllers screen: put up over whatever this device is showing,
    // handed presses, and put away again back to where it was opened from.
    void open_controllers();
    void close_controllers();
    void update_controllers_screen(const std::vector<UiAction>& actions,
                                   const std::vector<PointerEvent>& pointers);
    // Save points, made and loaded from the quick menu. The slots are read
    // off the disk here and handed to the menu -- this device's, and the
    // companion's copy of it -- with one line about the last save.
    bool save_points_available() const;
    void refresh_save_points(const std::string& status = std::string(), bool failed = false);
    void make_save_point(int slot);
    void load_save_point(int slot);
    // The companion's end: whatever the host last said is in the slots.
    uint32_t save_slots_shown = 0;
    void show_host_save_slots();
    // Buzzes one pad for a moment -- see identify_pad().
    void identify_pad(const KnownPad& pad);
    void tick_identify();
    // Where the pads stand this frame, as the Controllers screen has them.
    std::vector<PadSlot> current_pad_slots(const std::vector<KnownPad>& pads) const {
        return slots_of(pads, line_up_pads(pads, pad_order, holds_gamepad()));
    }
    // Whether the Wii U list is shown in place of "Playing on Cemu" -- see
    // LayoutPickerState::embedded.
    bool shows_embedded_menu() const;
    // Assigns the pads, hands each player's to the game, and returns the
    // GamePad's -- with the touch screen and the motion of whichever device is
    // showing the GamePad's screen.
    DsInputState route_wii_u_pads(const DsInputState& here, const DsInputState& remote,
                                  bool companion_here);
    // Turns each pad's motor on or off to match its slot's.
    void apply_pad_rumble();
    // Stops every motor this app turned on for a slot, as when the game ends.
    void stop_pad_rumble();
    // True while two devices are in a session, whichever end this is.
    bool paired() const;
    // Opens the layout menu with the state it should be showing. `embedded`
    // shows it in place of "Playing on Cemu" instead -- see
    // LayoutPickerState::embedded.
    void open_picker(bool embedded = false);
    // What to do about whatever the menu just decided. Called for a press made
    // here and for one forwarded from the other device alike: there is one
    // menu, so there is one place that answers it.
    // `pressed_here` is false for a press the companion made and sent over.
    // The one menu is worked from either device, so almost nothing cares which
    // pad it came from -- leaving the session is the exception.
    void apply_menu_result(const LayoutPickerResult& result, bool pressed_here = true);
    // Opens or closes the one menu on this device, which for a host is also
    // what the companion is told to draw.
    void set_menu_open(bool open);
    // The menu as the host has it, for a device that is only showing it.
    LayoutPickerState menu_state_from_host() const;
    // The two settings both devices share. On a companion these ask the host
    // and change nothing locally; the host owns them and broadcasts the result,
    // which is what keeps the two devices from ever disagreeing.
    void request_layout(const std::string& layout_id);
    void request_filtering(bool bilinear);
    void request_audio_target(AudioTarget target);
    void request_handedness(Handedness hand);
    void request_render_scale(int scale);
    void request_screen_gap(bool include_gap);
    void request_right_stick_mode(RightStickMode mode);
    // Restarts the running game. On a companion this asks the host, since only
    // the host runs a core to restart; there is nothing to adopt back, since
    // there is no value beyond the moment it happens.
    void request_reset();
    // Applies and persists a host-side change to any of them.
    void adopt_filtering(bool bilinear);
    void adopt_render_scale(int scale);
    // The next scale in the cycle, wrapping at the system's maximum.
    int next_render_scale() const;
    void adopt_audio_target(AudioTarget target);
    // Sets the hand of the layout in use, and only that layout.
    void adopt_handedness(Handedness hand);
    // The whole list, as the host has it, on a companion.
    void adopt_left_handed_layouts(const std::vector<std::string>& layouts);
    // An older settings.json's one hand for every layout, turned into the
    // list: every layout with a hand, flipped. Once.
    void adopt_legacy_handedness();
    void adopt_screen_gap(bool include_gap);
    void adopt_right_stick_mode(RightStickMode mode);
    // What clicking the left stick does. The host's setting, and while a
    // companion is connected the one both devices follow: the quick menu
    // that offers it is one menu, so what it says has to come from one place.
    LeftStickPress left_stick_press_in_effect() const;
    void request_left_stick_press(LeftStickPress press);
    void adopt_left_stick_press(LeftStickPress press);
    // Puts whatever is in effect into this device's L3 and the menu.
    void apply_left_stick_press();
    // A DS game's Rumble Pak: whether one is in, as the host has it, and the
    // same ask-or-apply pair as everything else the menu shares.
    bool rumble_pak_in_effect() const;
    void request_rumble_pak(bool inserted);
    void adopt_rumble_pak(bool inserted);
    // Whether this device should be making sound right now.
    bool sound_plays_here() const;
    // Loads a ROM and starts emulating it. On failure it puts a readable
    // message on screen and returns false; `fatal_on_failure` decides whether
    // dismissing that message quits or goes back to the browser.
    bool start_game(const std::string& rom_path, bool fatal_on_failure);
    // The libretro half of the above: the core, its callbacks and its
    // settings, for the game at `rom_path` on `system`. `details` gets lines
    // worth showing the player if it fails.
    bool open_core(const std::string& rom_path, std::string* error,
                   std::vector<std::string>* details);
    // The context the game about to be opened needs: its console's, or a 3.3
    // core profile for a DS rendered on the GPU. See renders_on_gpu().
    GlContextNeed gl_context_for_game() const;
    // Closes the core and opens it again on the same game, keeping its place
    // through a save state -- for a setting the core only takes as a game is
    // opened, such as a DS going between its software and OpenGL renderers.
    bool reopen_core_in_place();
    void open_browser();
    void enter_message(MessageKind kind,
                       const std::string& title,
                       const std::string& body,
                       const std::vector<std::string>& details,
                       bool fatal);
    void persist_game_config();
    void handle_ui(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers);
    // How long the last frame took, so screens that need a clock have one.
    double last_delta_seconds = 0.0;
    CursorVisibility cursor;
    // What SDL was last told, so it is only told when the answer changes.
    bool cursor_shown = false;
    void render();
    bool write_screenshot(const std::string& path);
};

App::App() : impl_(new Impl()) {}
App::~App() = default;

bool App::Impl::create_window(GlContextNeed need, std::string* error) {
    gl_context_need = need;

    GlWindowRequest request;
    request.title = "Dualscreen";
    request.width = 1280;
    request.height = 800;
    request.windowed = options.windowed;
    request.need = need;
    request.vsync = settings.vsync;

    GlWindow created;
    if (!create_gl_window(request, created, error)) {
        return false;
    }
    window = created.window;
    gl_context = created.context;

    std::string art_error;
    if (bezel_art.load(&art_error)) {
        apply_system();
    } else {
        // Not fatal: the Standard layout still puts the screens where they
        // belong, it just has nothing drawn behind them.
        log_warn("cannot draw the console artwork: " + art_error);
    }
    refresh_output_size();
    return true;
}

void App::Impl::destroy_window() {
    // Before the context that owns it -- everything here, and the menu's own
    // pictures, which can be on screen when the window is rebuilt under them.
    menu_backdrop.forget();
    if (picker) {
        picker->release_textures();
    }
    bezel_art.destroy();
    // And the button prompts, which rebuild_renderer() loads again: loaded
    // into textures left over from the old context instead, they came back
    // as blank white boxes and an OpenGL error.
    button_glyphs.shutdown();
    compositor.set_bezel_art(nullptr);
    if (picker) {
        picker->set_bezel_art(nullptr);
    }
    text.shutdown();
    quads.shutdown();
    if (gl_context != nullptr) {
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
    }
    if (window != nullptr) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
}

bool App::Impl::ensure_gl_context(GlContextNeed need, std::string* error) {
    // The question is whether the context in hand can do the job, not whether
    // it is the one that would have been asked for. A context stronger than a
    // system needs is fine, so going from a 3DS game back to a DS one does not
    // rebuild anything.
    const bool satisfied =
        window != nullptr && (need != GlContextNeed::OpenGl33Core || gl::at_least(3, 3));
    if (satisfied) {
        return true;
    }
    log_info("rebuilding the window for a different OpenGL context");

    // Everything living in the old context goes with it, and comes back
    // against the new one. Nothing else holds GL objects at this point: the
    // screen textures belong to the source, which is opened after this.
    destroy_window();
    if (create_window(need, error)) {
        return rebuild_renderer(error);
    }

    // The window is gone and the one that was wanted could not be built. Put
    // *some* window back before returning the failure, or there is nothing left
    // to show the player the error on -- which on a Deck in Game Mode means the
    // app simply vanishes.
    const std::string original_error = (error != nullptr) ? *error : std::string();
    std::string fallback_error;
    if (create_window(GlContextNeed::Any, &fallback_error) && rebuild_renderer(&fallback_error)) {
        log_warn("carrying on with the context this driver will give: " + original_error);
    } else {
        log_error("could not rebuild any window: " + fallback_error);
    }
    if (error != nullptr) *error = original_error;
    return false;
}

bool App::Impl::rebuild_renderer(std::string* error) {
    if (!quads.initialize(error) || !text.initialize(error) || !button_glyphs.initialize(error)) {
        return false;
    }
    log_info("renderer rebuilt against the new context: font atlas " +
             std::to_string(text.atlas_width()) + "x" + std::to_string(text.atlas_height()) +
             (gl::is_core_profile() ? ", core profile" : "") +
             (gl::has_vertex_arrays() ? ", vertex arrays available" : ", no vertex arrays"));
    gl::log_error_once("rebuilding the renderer");
    apply_system();
    refresh_output_size();
    return true;
}

void App::Impl::apply_system() {
    const SystemProfile& profile = system_profile(system);
    const Texture* art = bezel_art.texture(system);
    compositor.set_bezel_art(art);
    if (picker) {
        picker->set_bezel_art(art);
    }
    // A console with its own stick reads the stick as that stick. One without
    // lets it stand in for the d-pad, because a player will reach for it.
    input.set_stick_is_dpad(!profile.has_analog_stick);
    input.set_game_owns_controls(profile.game_uses_every_control);
    apply_left_stick_press();
    // Whether this device's own sensors are read at all.
    //
    // Only when it is the one running the game: a companion is told by the
    // host instead (see host_wants_motion below), because the host is the one
    // that knows whether anything at the far end reads motion. This was
    // missing entirely, which nothing noticed while only a Wii U used motion
    // -- there the companion supplies it and Cemu reads its own pads. A 3DS
    // runs its core here, so with nothing switching these on, a game played on
    // one device had no motion to read at all.
    if (!is_companion()) {
        input.set_motion_enabled(profile.uses_motion);
    }
}

void App::Impl::refresh_output_size() {
    if (window == nullptr) return;
    int width = 0;
    int height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width > 0 && height > 0) {
        output_width = width;
        output_height = height;
    }
}

const Layout& App::Impl::active_layout() const { return registry.resolve(active_layout_id); }

Layout App::Impl::device_layout() const {
    // Handedness first: it mirrors the arrangement, and everything downstream --
    // what is drawn, and where a touch lands -- has to see the same one. The
    // gap choice does not interact with either, so its order does not matter.
    Layout chosen =
        resolve_for_handedness(resolve_gap_choice(active_layout(), screen_gap),
                               current_handedness());
    // A companion holding the touch screen is a thing being touched, so it
    // fills its panel however many pixels the panel has -- the same as a
    // display small enough to do that already. A host does not, whatever it is
    // showing: it is the one on the desk or the television.
    chosen.fill_display = is_companion() && local_screen == ScreenId::Bottom;
    if (!chosen.per_device) {
        return chosen;
    }
    // A per-device layout with nobody on the other end would show half a game,
    // so it falls back to the whole thing until a companion is actually there.
    const bool paired = (host_session && host_session->has_companion()) ||
                        (companion_session &&
                         companion_session->status() == net::CompanionSession::Status::Connected);
    if (!paired) {
        if (system_profile(system).solo_screens == SoloScreens::Both) {
            Layout both = chosen;
            both.per_device = false;
            return both;
        }
        // One screen, whichever the player is watching. Resolved exactly as a
        // paired device resolves its own, so what is drawn and where a touch
        // lands come from the same place either way.
        return resolve_for_device(chosen, solo_screen);
    }
    return resolve_for_device(chosen, local_screen);
}

void App::Impl::companion_target_size(ScreenId screen, int& out_width, int& out_height) const {
    out_width = 0;
    out_height = 0;
    if (!host_session) {
        return;
    }
    int companion_width = 0;
    int companion_height = 0;
    if (!host_session->companion_display_size(companion_width, companion_height)) {
        return;
    }

    // The companion's own version of device_layout(): the same layout, but
    // resolved for whichever screen the companion holds rather than this
    // device's. A companion is by definition paired the moment there is a
    // reported size to act on, so there is no unpaired fallback to consider.
    Layout companion_layout = resolve_for_handedness(
        resolve_gap_choice(active_layout(), screen_gap), current_handedness());
    if (companion_layout.per_device) {
        companion_layout = resolve_for_device(companion_layout, host_session->companion_screen());
    }

    for (const PlacedScreen& placed : place_screens(companion_layout, static_cast<float>(companion_width),
                                                     static_cast<float>(companion_height),
                                                     screens_geometry())) {
        if (placed.screen != screen || placed.rect.w <= 0.0f || placed.rect.h <= 0.0f) {
            continue;
        }
        // rect is the box this screen fills in the companion's own window,
        // which is what its resolution should match -- except a rotated
        // placement measures that box turned, while the texture sent over the
        // wire is always in the screen's own unrotated axes (rotation is
        // applied by whichever device draws it, not baked into the picture).
        int width = static_cast<int>(std::ceil(placed.rect.w));
        int height = static_cast<int>(std::ceil(placed.rect.h));
        if (rotation_is_quarter_turn(placed.rotation)) {
            std::swap(width, height);
        }
        out_width = width;
        out_height = height;
        return;
    }
}

ScreenSource* App::Impl::local_game() {
    if (source) return source.get();
    if (external_source) return external_source.get();
    return nullptr;
}

const ScreenSource* App::Impl::local_game() const {
    if (source) return source.get();
    if (external_source) return external_source.get();
    return nullptr;
}

bool App::Impl::wants_video() const {
    const video::ScreenCodec chosen =
        options.video_codec_override.empty()
            ? settings.screen_codec_for(system)
            : video::screen_codec_from_key(options.video_codec_override,
                                           settings.screen_codec_for(system));
    return chosen == video::ScreenCodec::H264;
}

double App::Impl::local_frames_per_second() const {
    if (source) return source->frames_per_second();
    if (external_source) return external_source->frames_per_second();
    return 60.0;
}

double App::Impl::local_audio_sample_rate() const {
    if (source) return source->audio_sample_rate();
    if (external_source) return external_source->audio_sample_rate();
    return 48000.0;
}

void App::Impl::adopt_external_timing() {
    frame_period = 1.0 / std::max(1.0, local_frames_per_second());
    frame_accumulator = 0.0;
    // Reopened rather than left as it was: the rate the link reported before
    // anything connected was a default, and playing 48kHz audio through a
    // device opened for some other rate is heard rather than merely wrong.
    audio.close();
    std::string audio_error;
    if (!audio.open(local_audio_sample_rate(), &audio_error)) {
        log_warn(audio_error + "; continuing without sound");
    }
    log_info("the game runs at " + std::to_string(static_cast<int>(local_frames_per_second() + 0.5)) +
             " fps, " + std::to_string(static_cast<int>(local_audio_sample_rate())) + " Hz");
}

ScreenSource* App::Impl::active_source() {
    if (ScreenSource* here = local_game()) return here;
    if (companion_session) return companion_session.get();
    return nullptr;
}

SystemScreens App::Impl::screens_geometry() const {
    if (const ScreenSource* here = local_game()) {
        return here->screens_geometry();
    }
    if (companion_session) {
        return companion_session->screens_geometry();
    }
    return system_profile(system).screens;
}

void App::Impl::update_solo_screen() {
    int display_width = 0;
    int display_height = 0;
    if (window != nullptr) {
        const int display = SDL_GetWindowDisplayIndex(window);
        SDL_DisplayMode mode{};
        if (display >= 0 && SDL_GetCurrentDisplayMode(display, &mode) == 0) {
            display_width = mode.w;
            display_height = mode.h;
        }
    }
    // Auto with no display to ask yet has no answer to give, and saying
    // "television" on the way to the real one would be a line in the log that
    // was never true. Asked again on the next pass.
    if (settings.wii_u_screen_alone == WiiUScreenAlone::Auto &&
        (display_width <= 0 || display_height <= 0)) {
        return;
    }
    solo_screen_unchecked = false;
    ScreenId screen =
        resolve_wii_u_screen_alone(settings.wii_u_screen_alone, display_width, display_height);
    if (solo_screen_swapped) {
        screen = (screen == ScreenId::Top) ? ScreenId::Bottom : ScreenId::Top;
    }
    // Said on every change and on the first answer, with the reason, since
    // "why is my docked Deck showing the GamePad" has exactly one place to be
    // answered from.
    if (said_solo_screen && screen == solo_screen) {
        return;
    }
    said_solo_screen = true;
    solo_screen = screen;
    std::string why;
    if (solo_screen_swapped) {
        why = "flipped for this session";
    } else if (settings.wii_u_screen_alone == WiiUScreenAlone::Auto) {
        why = "Auto, on a " + std::to_string(display_width) + "x" +
              std::to_string(display_height) + " display, which " +
              (display_is_a_steam_decks_own(display_width, display_height)
                   ? "is a Steam Deck's own panel"
                   : "is not a Steam Deck's own panel");
    } else {
        why = "as set";
    }
    log_info(std::string("alone, this device is a Wii U's ") +
             (screen == ScreenId::Bottom ? "GamePad" : "television") + " (" + why + ")");
}

bool App::Impl::is_companion() const {
    return companion_session &&
           companion_session->status() == net::CompanionSession::Status::Connected;
}

bool App::Impl::gamepad_is_here() const {
    // Only a Wii U has a GamePad distinct from a television; everywhere else
    // this device is holding the whole console and the motor is its own.
    if (system_profile(system).solo_screens == SoloScreens::Both) {
        return true;
    }
    const ScreenId here = (host_session && host_session->has_companion())
                              ? host_session->host_screen()
                              : solo_screen;
    return here == ScreenId::Bottom;
}

std::vector<KnownPad> App::Impl::gather_pads(bool companion_here) const {
    std::vector<KnownPad> pads;
    int position = 0;
    for (const PadReading& reading : input.pad_readings()) {
        KnownPad pad;
        pad.on_this_device = true;
        pad.id = static_cast<uint32_t>(reading.id);
        pad.position = position++;
        pad.name = reading.name;
        pad.slot = reading.slot;
        pad.held = reading.held;
        pads.push_back(pad);
    }
    if (!companion_here) {
        return pads;
    }
    if (!host_session->companion_reports_pads()) {
        // Everything it has, as one pad, which is all an older companion can
        // say -- and a motor for it means every motor it has.
        KnownPad pad;
        pad.on_this_device = false;
        pad.id = net::RumbleMessage::kWholeDevice;
        pad.held = host_session->companion_input();
        pad.held.touching = false;
        pads.push_back(pad);
        return pads;
    }
    position = 0;
    for (const net::ReportedPad& reported : host_session->companion_pads()) {
        // Keys are Cemu's to map; see pad_routing.hpp.
        if (reported.id == net::ReportedPad::kKeyboard) continue;
        KnownPad pad;
        pad.on_this_device = false;
        pad.id = reported.id;
        pad.position = position++;
        pad.name = reported.name;
        pad.slot = reported.slot;
        pad.held = reported.held;
        pads.push_back(pad);
    }
    return pads;
}

DsInputState App::Impl::route_wii_u_pads(const DsInputState& here, const DsInputState& remote,
                                         bool companion_here) {
    // Nothing connected counts as holding the GamePad even when this device
    // is being the television: most Wii U games cannot be started without
    // one, and the Controllers screen can move it anywhere else.
    const bool gamepad_here = holds_gamepad();
    known_pads = gather_pads(companion_here);
    pad_slots = current_pad_slots(known_pads);

    std::string said;
    for (std::size_t index = 0; index < known_pads.size(); ++index) {
        const KnownPad& known = known_pads[index];
        if (!said.empty()) said += ", ";
        said += pad_slot_name(pad_slots[index]) + " " + pad_identity(known) + " (" +
                pad_label(known) + (known.name.empty() ? "" : ", " + known.name) + ")";
    }
    if (said != said_pad_slots) {
        said_pad_slots = said;
        log_info("controllers: " + (said.empty() ? std::string("none") : said));
    }

    const RoutedPads routed = route_pads(known_pads, pad_slots);
    if (external_source) {
        for (int player = 0; player < kPadPlayers; ++player) {
            external_source->submit_player_input(player, routed.player_fed[player],
                                                 routed.players[player]);
        }
    }

    // The GamePad's buttons are its pads'; its screen and how it is held are
    // the device showing that screen -- unless the pads in its slot are all on
    // the other device, in which case how the GamePad is held is theirs.
    const DsInputState& screen_device = gamepad_here ? here : remote;
    DsInputState gamepad = routed.gamepad;
    gamepad.touching = screen_device.touching;
    gamepad.touch_x = screen_device.touch_x;
    gamepad.touch_y = screen_device.touch_y;
    bool screen_device_holds_it = false;
    const std::vector<std::size_t> holding = pads_in_slot(pad_slots, PadSlot::gamepad());
    for (const std::size_t index : holding) {
        if (known_pads[index].on_this_device == gamepad_here) screen_device_holds_it = true;
    }
    if ((holding.empty() || screen_device_holds_it) && screen_device.motion.present) {
        gamepad.motion = screen_device.motion;
    }

    apply_pad_rumble();
    return gamepad;
}

void App::Impl::apply_pad_rumble() {
    std::map<std::string, KnownPad> wanted;
    for (std::size_t index = 0; index < known_pads.size() && index < pad_slots.size(); ++index) {
        const KnownPad& pad = known_pads[index];
        const PadSlot slot = pad_slots[index];
        if (slot.kind == PadSlot::Kind::NotUsed) continue;
        const int motor = slot.kind == PadSlot::Kind::GamePad ? 0 : 1 + slot.player;
        if (slot_rumble[motor]) wanted.emplace(pad_identity(pad), pad);
    }
    const auto set = [this](const KnownPad& pad, bool active) {
        if (pad.on_this_device) {
            input.set_pad_rumble(static_cast<SDL_JoystickID>(pad.id), active);
        } else if (host_session && pad.id == net::RumbleMessage::kWholeDevice) {
            host_session->publish_rumble(active);
        } else if (host_session) {
            host_session->publish_pad_rumble(pad.id, active);
        }
    };
    for (const auto& [key, pad] : rumbling_pads) {
        const auto still = wanted.find(key);
        if (still == wanted.end() || still->second.id != pad.id) set(pad, false);
    }
    for (const auto& [key, pad] : wanted) {
        const auto was = rumbling_pads.find(key);
        if (was == rumbling_pads.end() || was->second.id != pad.id) set(pad, true);
    }
    rumbling_pads = std::move(wanted);
}

void App::Impl::stop_pad_rumble() {
    for (bool& motor : slot_rumble) motor = false;
    apply_pad_rumble();
}

void App::Impl::open_controllers() {
    controllers_came_from = state;
    if (picker) {
        // The same as Settings: one menu on screen at a time.
        picker->close();
    }
    controllers_screen.open();
    state = State::Controllers;
    input.set_game_input_enabled(false);
    log_info("the controllers screen is up");
}

void App::Impl::close_controllers() {
    controllers_screen.close();
    log_info("the controllers screen is down");
    if (controllers_came_from == State::Menu && local_game()) {
        set_menu_open(true);
    } else if (local_game()) {
        state = State::Playing;
        input.set_game_input_enabled(true);
    } else {
        open_launcher();
    }
}

void App::Impl::update_controllers_screen(const std::vector<UiAction>& actions,
                                          const std::vector<PointerEvent>& pointers) {
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    const ControllersScreenResult result = controllers_screen.update(
        actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height),
        pads, holds_gamepad(), pad_order);
    if (result.order_changed) {
        log_info(pad_order.chosen ? "controllers: the order was changed"
                                  : "controllers: back to each device's own order");
    }
    if (result.identify >= 0 && static_cast<std::size_t>(result.identify) < pads.size()) {
        identify_pad(pads[static_cast<std::size_t>(result.identify)]);
    }
    if (result.close_requested) {
        close_controllers();
    }
}

bool App::Impl::save_points_available() const {
    return source && !game_path.empty() && source->supports_save_states();
}

void App::Impl::refresh_save_points(const std::string& status, bool failed) {
    adopt_old_save_point_names(paths.saves_dir, game_path);
    net::SaveSlotsMessage slots;
    slots.slots.resize(kSavePointSlots);
    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        read_save_point_info(save_point_path(paths.saves_dir, game_path, slot),
                             slots.slots[static_cast<std::size_t>(slot)]);
    }
    slots.status = status;
    slots.status_failed = failed;
    if (picker) {
        picker->set_save_slots(slots.slots);
        picker->set_save_status(status, failed);
    }
    if (host_session) host_session->set_save_slots(slots);
}

void App::Impl::show_host_save_slots() {
    if (!is_companion() || !picker) return;
    const uint32_t revision = companion_session->host_save_slots_revision();
    if (revision == save_slots_shown) return;
    save_slots_shown = revision;
    const net::SaveSlotsMessage& slots = companion_session->host_save_slots();
    picker->set_save_slots(slots.slots);
    picker->set_save_status(slots.status, slots.status_failed);
}

void App::Impl::make_save_point(int slot) {
    if (!save_points_available()) return;
    const std::string path = save_point_path(paths.saves_dir, game_path, slot);
    const std::string name = "save state " + std::to_string(slot + 1);
    std::vector<uint8_t> state_bytes;
    std::string error;
    // The picture first: it is of the frame the game is paused on, which is
    // exactly the moment being saved.
    const SavePointThumbnail thumbnail = capture_save_point_thumbnail(*source);
    if (source->save_state(state_bytes, &error) &&
        write_save_point(path, state_bytes, thumbnail, static_cast<int64_t>(std::time(nullptr)),
                         &error)) {
        log_info(name + " saved (" + std::to_string(state_bytes.size()) + " bytes) to " + path);
        refresh_save_points("Saved to " + name + ".", false);
    } else {
        log_warn(name + " could not be saved: " + error);
        refresh_save_points(error, true);
    }
}

void App::Impl::load_save_point(int slot) {
    if (!save_points_available()) return;
    const std::string path = save_point_path(paths.saves_dir, game_path, slot);
    const std::string name = "save state " + std::to_string(slot + 1);
    std::vector<uint8_t> state_bytes;
    std::string error;
    if (read_save_point_state(path, state_bytes, &error) &&
        source->load_state(state_bytes, &error)) {
        log_info(name + " loaded from " + path);
        // Straight back into the game, the way NSO does it: loading one is
        // choosing to play from there.
        set_menu_open(false);
        return;
    }
    log_warn(name + " could not be loaded: " + error);
    refresh_save_points(error, true);
}

void App::Impl::identify_pad(const KnownPad& pad) {
    // A quick buzz to say which one it is, and that is all. The device that
    // owns the pad ends it by itself -- so it is the same short length even
    // when the message getting there waited behind a streaming screen.
    constexpr uint16_t kPingMs = 300;
    log_info("controllers: rumbling " + pad_identity(pad) + " so it can be found");
    if (pad.on_this_device) {
        input.ping_pad(static_cast<SDL_JoystickID>(pad.id), kPingMs);
    } else if (host_session && pad.id != net::RumbleMessage::kWholeDevice) {
        host_session->publish_pad_ping(pad.id, kPingMs);
    } else if (host_session && !identifying_pad) {
        // A companion too old to end a ping itself: on, then off from here.
        identifying_pad = pad;
        identify_until_ms = SDL_GetTicks() + kPingMs;
        host_session->publish_rumble(true);
    }
}

void App::Impl::tick_identify() {
    if (!identifying_pad || static_cast<int32_t>(SDL_GetTicks() - identify_until_ms) < 0) {
        return;
    }
    const bool game_has_it = rumbling_pads.count(pad_identity(*identifying_pad)) != 0;
    identifying_pad.reset();
    // Left alone if the game has its motor on meanwhile: that is not ours to
    // stop.
    if (host_session && !game_has_it) {
        host_session->publish_rumble(false);
    }
}

bool App::Impl::shows_embedded_menu() const {
    return state == State::Playing && system == SystemId::WiiU && !is_companion() &&
           external_source && external_source->running() && picker;
}

bool App::Impl::paired() const {
    return (host_session && host_session->has_companion()) || is_companion();
}

LayoutPickerState App::Impl::menu_state_from_host() const {
    return ds::menu_state_from_host(*companion_session);
}

void App::Impl::set_menu_open(bool open) {
    if (open) {
        if (state == State::Menu) return;
        open_picker();
        state = State::Menu;
        log_info("the quick menu is up");
        input.set_game_input_enabled(false);
        if (is_companion()) companion_session->set_menu_open(true);
        return;
    }
    if (state != State::Menu) return;
    log_info("the quick menu is down");
    picker->close();
    if (is_companion()) {
        companion_session->set_menu_open(false);
        state = State::Companion;
    } else {
        state = State::Playing;
    }
    input.set_game_input_enabled(true);
}

void App::Impl::apply_menu_result(const LayoutPickerResult& result, bool pressed_here) {
            // Made here whichever device's pad chose them: the saves are on
            // this machine, beside the game.
            if (!is_companion()) {
                if (result.save_slot >= 0) make_save_point(result.save_slot);
                if (result.load_slot >= 0) load_save_point(result.load_slot);
            }
            if (result.selection_changed) {
                // A companion does not own the choice: request_layout() asks,
                // and this device adopts whatever the host broadcasts back.
                request_layout(picker->selected_layout_id());
            }
            if (result.filtering_toggled) {
                request_filtering(!bilinear_filtering);
            }
            if (result.render_scale_changed) {
                request_render_scale(next_render_scale());
            }
            if (result.audio_target_changed) {
                request_audio_target(next_audio_target(audio_target));
            }
            if (result.handedness_changed) {
                request_handedness(other_hand(current_handedness()));
            }
            if (result.screen_gap_toggled) {
                request_screen_gap(!screen_gap);
            }
            if (result.right_stick_mode_changed) {
                request_right_stick_mode(next_right_stick_mode(right_stick_mode));
            }
            if (result.left_stick_press_changed) {
                request_left_stick_press(next_left_stick_press(left_stick_press_in_effect()));
            }
            if (result.rumble_pak_toggled) {
                request_rumble_pak(!rumble_pak_in_effect());
            }
            if (result.cursor_mode_changed) {
                // This device's own pointer, so it is decided here and not
                // asked of the host -- the other device has its own.
                settings.cursor_mode = next_cursor_mode(settings.cursor_mode);
                picker->set_cursor_mode(settings.cursor_mode);
                settings.save(paths);
                log_info(std::string("pointer: ") + to_string(settings.cursor_mode));
            }
            if (result.reset_requested) {
                request_reset();
            }
            if (result.disconnect_requested) {
                if (host_session) {
                    host_session->disconnect_companion(pressed_here ? "you ended the session"
                                                                    : "you left the session");
                    // Only when somebody here asked for it. A companion leaving
                    // is not this player asking for their game back: they are
                    // still sitting in the menu, and it has just changed under
                    // them -- one screen where there were two, and the entries
                    // that only meant something with a second device gone.
                    if (pressed_here) {
                        set_menu_open(false);
                    }
                } else if (companion_session) {
                    companion_session->disconnect();
                    open_launcher();
                }
            } else if (result.quit_requested) {
                if (is_companion()) {
                    // Quit here means the whole session ends, not just this
                    // device's part of it: ask the host to shut down too, the
                    // same as if Quit had been pressed there, then leave the
                    // way Disconnect would.
                    companion_session->request_quit();
                    companion_session->disconnect();
                    open_launcher();
                } else {
                    running = false;
                }
            } else if (result.controllers_requested) {
                // The host's own screen, the same as Settings below: only it
                // can see every controller on both devices.
                open_controllers();
            } else if (result.settings_requested) {
                // The settings belong to the device running the game, whichever
                // pad asked: half of what they show is where this machine's
                // BIOS files and emulators are, which is nothing the other
                // device could act on. It is told to look over here instead.
                open_settings();
            } else if (result.close_requested || result.reset_requested) {
                // Through the one place that opens and closes it, so that
                // putting the menu away always sounds like it.
                set_menu_open(false);
            }
}

void App::Impl::open_picker(bool embedded) {
    LayoutPickerState picker_state;
    picker_state.embedded = embedded;
    if (embedded) {
        picker_state.subtitle = "Playing on " + system_profile(system).core_description +
                                ". Switch back to its window to play.";
    }
    picker_state.layout_id = active_layout_id;
    picker_state.session_active = paired();
    picker_state.bilinear_filtering = bilinear_filtering;
    picker_state.audio_target = audio_target;
    picker_state.cursor_mode = settings.cursor_mode;
    picker_state.left_handed_layouts = left_handed_layouts;
    picker_state.render_scale = render_scale;
    // From the console being played, on both devices: a companion is shown the
    // same menu as the host, and `system` follows the host while it is
    // connected. Outside a game there is nothing to scale.
    picker_state.max_render_scale =
        is_companion() ? companion_session->host_max_render_scale()
                       : (local_game() ? system_profile(system).max_render_scale : 1);
    picker_state.screen_gap = screen_gap;
    picker_state.right_stick_mode = right_stick_mode;
    // Only a console whose right stick can mean more than one thing has a
    // question worth offering -- a 3DS. A DS has no right stick to speak of,
    // and a Wii U's is always just itself.
    picker_state.right_stick_choosable =
        is_companion() ? companion_session->host_right_stick_choosable()
                       : (local_game() && system_profile(system).offers_right_stick_mode);
    // What L3 does, where there is a microphone for it to be -- see
    // LeftStickPress. The host's, and told to the companion like the rest.
    picker_state.left_stick_press = left_stick_press_in_effect();
    picker_state.left_stick_choosable =
        is_companion() ? companion_session->host_left_stick_choosable()
                       : (local_game() && system_profile(system).has_microphone);
    // A Rumble Pak, where the core running this game can put one in.
    picker_state.rumble_pak = rumble_pak_in_effect();
    picker_state.rumble_choosable = is_companion() ? companion_session->host_rumble_choosable()
                                                   : (source && source->offers_rumble_pak());
    // Which entries the menu has has to match on both devices, or the same
    // cursor position would mean different things on the two screens.
    picker_state.cursor_choosable =
        is_companion() ? companion_session->host_cursor_choosable() : true;
    // A property of the console, not of the session, so both devices reach the
    // same answer from `system` without anything to ask the host for.
    picker_state.save_points_available = is_companion()
                                             ? companion_session->host_save_points_available()
                                             : save_points_available();
    picker_state.reset_available = system_profile(system).offers_reset;
    picker_state.smoothing_available = system_profile(system).offers_smoothing;
    picker->set_screens(screens_geometry(), system);
    // The tiles are judged against the same display, and filled on the same
    // terms, as the render behind them -- see device_layout().
    picker->set_display_size(0.0f, 0.0f, is_companion() && local_screen == ScreenId::Bottom);
    picker->open(picker_state);
    if (is_companion()) {
        save_slots_shown = companion_session->host_save_slots_revision() - 1;
        show_host_save_slots();
    } else if (picker_state.save_points_available) {
        refresh_save_points();
    }
}

int App::Impl::next_render_scale() const {
    const int maximum = std::max(1, system_profile(system).max_render_scale);
    return render_scale >= maximum ? 1 : render_scale + 1;
}

void App::Impl::request_render_scale(int scale) {
    if (is_companion()) {
        companion_session->request_render_scale(scale);
        return;  // the host decides; it will tell us
    }
    adopt_render_scale(scale);
}

void App::Impl::adopt_render_scale(int scale) {
    const int wanted = std::clamp(scale, 1, std::max(1, system_profile(system).max_render_scale));
    if (wanted == render_scale) {
        return;
    }
    const int was = render_scale;
    render_scale = wanted;
    if (source && LibretroCoreScreenSource::renders_on_gpu(system, was) !=
                      LibretroCoreScreenSource::renders_on_gpu(system, wanted)) {
        // Across the line between a DS's software and OpenGL renderers, which
        // the core only picks between as a game is opened. Written down first,
        // so the game opens again at the scale it is going to.
        if (have_game_config) {
            persist_game_config();
        }
        reopen_core_in_place();
    } else if (source) {
        source->set_render_scale(render_scale);
        // Whatever the core actually accepted, which is what the menu shows.
        render_scale = source->render_scale();
    }
    if (picker) {
        picker->set_render_scale(render_scale);
    }
    // A companion is following the host and has no game of its own to remember
    // this against, so it changes what it displays and nothing on disk.
    if (is_companion()) {
        log_info("following the host: render scale " + std::to_string(render_scale) + "x");
        return;
    }
    if (have_game_config) {
        persist_game_config();
    } else {
        settings.default_render_scale = render_scale;
        settings.save(paths);
    }
}

void App::Impl::request_layout(const std::string& layout_id) {
    if (layout_id.empty() || layout_id == active_layout_id) {
        return;
    }
    if (is_companion()) {
        companion_session->request_layout(layout_id);
        return;  // the host decides; it will tell us
    }
    active_layout_id = layout_id;
    persist_game_config();
    if (picker) {
        picker->set_selected_layout(active_layout_id);
    }
    log_info("layout: " + active_layout().display_name);
}

LeftStickPress App::Impl::left_stick_press_in_effect() const {
    return is_companion() ? companion_session->host_left_stick_press() : settings.left_stick_press;
}

void App::Impl::request_left_stick_press(LeftStickPress press) {
    if (is_companion()) {
        companion_session->request_left_stick_press(press);
        return;  // the host decides; it will tell us
    }
    adopt_left_stick_press(press);
}

void App::Impl::adopt_left_stick_press(LeftStickPress press) {
    if (press == settings.left_stick_press) {
        return;
    }
    settings.left_stick_press = press;
    settings.save(paths);
    apply_left_stick_press();
    log_info(std::string("left stick click: ") + to_string(press));
}

void App::Impl::apply_left_stick_press() {
    const LeftStickPress press = left_stick_press_in_effect();
    input.set_microphone_button(
        left_stick_is_microphone(system_profile(system).has_microphone, press));
    if (picker) {
        picker->set_left_stick_press(press);
    }
}

bool App::Impl::rumble_pak_in_effect() const {
    if (is_companion()) return companion_session->host_rumble_pak();
    return source && source->rumble_pak();
}

void App::Impl::request_rumble_pak(bool inserted) {
    if (is_companion()) {
        companion_session->request_rumble_pak(inserted);
        return;  // the host decides; it will tell us
    }
    adopt_rumble_pak(inserted);
}

void App::Impl::adopt_rumble_pak(bool inserted) {
    if (!source || !source->offers_rumble_pak() || inserted == source->rumble_pak()) {
        return;
    }
    source->set_rumble_pak(inserted);
    if (!inserted) {
        stop_pad_rumble();
    }
    if (picker) {
        picker->set_rumble_pak(inserted);
    }
    if (have_game_config) {
        game_config.rumble_pak = inserted;
        persist_game_config();
    }
}

void App::Impl::request_right_stick_mode(RightStickMode mode) {
    if (is_companion()) {
        companion_session->request_right_stick_mode(mode);
        return;  // the host decides; it will tell us
    }
    adopt_right_stick_mode(mode);
}

void App::Impl::adopt_right_stick_mode(RightStickMode mode) {
    if (mode == right_stick_mode) {
        return;
    }
    right_stick_mode = mode;
    // Both devices' pads drive the game, so both read their own right stick
    // the same way -- which is why this is applied here as well as on the
    // device that owns the setting.
    input.set_right_stick_mode(right_stick_mode);
    if (picker) {
        picker->set_right_stick_mode(right_stick_mode);
    }
    if (is_companion()) {
        log_info(std::string("following the host: right stick ") + to_string(right_stick_mode));
        return;
    }
    if (have_game_config) {
        persist_game_config();
    } else {
        settings.default_right_stick_mode = right_stick_mode;
        settings.save(paths);
    }
    log_info(std::string("right stick: ") + to_string(right_stick_mode));
}

void App::Impl::request_screen_gap(bool include_gap) {
    if (is_companion()) {
        companion_session->request_screen_gap(include_gap);
        return;  // the host decides; it will tell us
    }
    adopt_screen_gap(include_gap);
}

void App::Impl::adopt_screen_gap(bool include_gap) {
    if (include_gap == screen_gap) {
        return;
    }
    screen_gap = include_gap;
    if (picker) {
        picker->set_screen_gap(screen_gap);
    }
    if (is_companion()) {
        log_info(std::string("following the host: screen gap ") + (screen_gap ? "on" : "off"));
        return;
    }
    if (have_game_config) {
        persist_game_config();
    } else {
        settings.default_screen_gap = screen_gap;
        settings.save(paths);
    }
    log_info(std::string("screen gap ") + (screen_gap ? "on" : "off"));
}

void App::Impl::request_reset() {
    if (is_companion()) {
        companion_session->request_reset();
        return;  // only the host has a core to reset
    }
    if (source) {
        source->reset();
    } else if (external_source) {
        external_source->reset();
    }
}

void App::Impl::request_filtering(bool bilinear) {
    if (is_companion()) {
        companion_session->request_filtering(bilinear);
        return;  // the host decides; it will tell us
    }
    adopt_filtering(bilinear);
}

void App::Impl::request_handedness(Handedness hand) {
    if (is_companion()) {
        companion_session->request_handedness(hand);
        return;
    }
    adopt_handedness(hand);
}

void App::Impl::adopt_handedness(Handedness hand) {
    const std::string id = active_layout().id;
    if (hand == handedness_of(id)) {
        return;
    }
    if (hand == Handedness::Left) {
        left_handed_layouts.push_back(id);
    } else {
        left_handed_layouts.erase(
            std::remove(left_handed_layouts.begin(), left_handed_layouts.end(), id),
            left_handed_layouts.end());
    }
    if (picker) {
        picker->set_left_handed_layouts(left_handed_layouts);
    }
    // Remembered layout by layout, and not per game: which way round you like
    // a layout does not change from one game to the next.
    if (!is_companion()) {
        settings.left_handed_layouts = left_handed_layouts;
        settings.save(paths);
    }
    log_info(id + ": " + to_string(hand));
}

void App::Impl::adopt_left_handed_layouts(const std::vector<std::string>& layouts) {
    if (layouts == left_handed_layouts) {
        return;
    }
    left_handed_layouts = layouts;
    if (picker) {
        picker->set_left_handed_layouts(left_handed_layouts);
    }
    log_info(std::string("following the host: ") + to_string(current_handedness()) + " for " +
             active_layout().id);
}

void App::Impl::adopt_legacy_handedness() {
    if (!settings.legacy_left_handed) {
        return;
    }
    settings.legacy_left_handed = false;
    settings.left_handed_layouts.clear();
    for (std::size_t index = 0; index < registry.size(); ++index) {
        if (layout_has_handedness(registry.at(index))) {
            settings.left_handed_layouts.push_back(registry.at(index).id);
        }
    }
    settings.save(paths);
    log_info("the one left hand in settings.json is now every layout that has a hand");
}

void App::Impl::request_audio_target(AudioTarget target) {
    if (is_companion()) {
        companion_session->request_audio_target(target);
        return;
    }
    adopt_audio_target(target);
}

void App::Impl::adopt_filtering(bool bilinear) {
    if (bilinear == bilinear_filtering) {
        return;
    }
    bilinear_filtering = bilinear;
    apply_filtering();
    if (have_game_config) {
        persist_game_config();
    } else {
        settings.default_bilinear_filtering = bilinear_filtering;
        settings.save(paths);
    }
    log_info(std::string("smoothing ") + (bilinear_filtering ? "on" : "off"));
}

void App::Impl::adopt_audio_target(AudioTarget target) {
    if (target == audio_target) {
        return;
    }
    audio_target = target;
    if (picker) {
        picker->set_audio_target(audio_target);
    }
    // A device arrangement rather than a property of the game, so it is
    // remembered globally rather than per ROM.
    if (!is_companion()) {
        settings.audio_target = audio_target;
        settings.save(paths);
    }
    log_info(std::string(is_companion() ? "following the host: sound " : "sound: ") +
             to_string(audio_target));
}

bool App::Impl::sound_plays_here() const {
    // A DS or 3DS has one soundtrack, so alone there is nobody else to hand
    // it to and it always plays here. A Wii U's TV and GamePad audio are
    // routed by a fixed rule instead, not a player choice -- see the
    // External source's own audio callback -- so this is never asked on that
    // system's behalf.
    if (!paired()) {
        return true;  // nobody else to hand it to
    }
    return audio_reaches(audio_target, local_screen);
}

void App::Impl::apply_filtering() {
    // Only ever a request. Whether a given screen is actually smoothed is
    // decided where it is drawn, since a screen that came out at its own size
    // has nothing to gain from it.
    compositor.set_smoothing(bilinear_filtering);
    if (picker) {
        picker->set_filtering(bilinear_filtering);
        picker->set_smoothing(bilinear_filtering);
    }
}

void App::Impl::attach_companion_callbacks() {
    if (!companion_session) {
        return;
    }
    companion_session->set_audio_callback(
        [this](const int16_t* samples, std::size_t frames, uint32_t rate) {
            // Anything that arrives is meant for this device: the host stops
            // sending when the sound is not supposed to come out here.
            if (rate == 0) {
                return;
            }
            if (!audio.is_open() || companion_audio_rate != rate) {
                std::string error;
                if (!audio.open(static_cast<double>(rate), &error)) {
                    log_warn("cannot play the streamed sound: " + error);
                    companion_audio_rate = rate;  // do not retry every packet
                    return;
                }
                companion_audio_rate = rate;
                log_info("playing streamed audio at " + std::to_string(rate) + " Hz");
            }
            audio.push(samples, frames);
        });
    companion_session->set_rumble_callback([this](bool active, uint32_t pad, uint16_t ping_ms) {
        if (ping_ms > 0) {
            // Which pad is this? A short buzz that ends here, on time.
            input.ping_pad(static_cast<SDL_JoystickID>(pad), ping_ms);
        } else if (pad == net::RumbleMessage::kWholeDevice) {
            input.set_rumble(active);
        } else {
            input.set_pad_rumble(static_cast<SDL_JoystickID>(pad), active);
        }
    });
    companion_session->set_video_hold(settings.video_hold);
}

void App::Impl::open_settings() {
    settings_came_from = state;
    if (picker) {
        // The menu it was opened from goes away rather than sitting underneath:
        // two menus at once would leave the player's presses going somewhere
        // they cannot see.
        picker->close();
    }
    settings_screen->open(settings, resolve_core_statuses(options, settings));
    state = State::Settings;
    input.set_game_input_enabled(false);
    log_info("the settings screen is up");
}

void App::Impl::apply_settings_result(const SettingsScreenResult& result) {
    if (result.settings_changed) {
        settings = settings_screen->settings();
        if (!settings.save(paths)) {
            log_warn("could not save settings.json");
        }
        SDL_GL_SetSwapInterval(settings.vsync ? 1 : 0);
        // The prompts change under the cursor that changed them, which is the
        // only way to see what the setting does.
        button_glyphs.set_style(settings.pad_glyphs);
        input.set_confirm_button(settings.confirm_button);
        set_confirm_button(settings.confirm_button);
        // L3's job is one of the things that can have just changed, and the
        // player who changed it is about to press it.
        input.set_microphone_button(left_stick_is_microphone(
            system_profile(system).has_microphone, settings.left_stick_press));
        // Takes effect at once rather than at the next launch: this is the
        // kind of thing somebody changes to see what it does, and the poll
        // loop below picks the new value up on its next pass.
        solo_screen_swapped = false;
        update_solo_screen();
        // Likewise immediate, and the one setting somebody is most likely to
        // change while watching the thing it changes.
        if (companion_session) {
            companion_session->set_video_hold(settings.video_hold);
        }
        if (source && system == SystemId::Ds) {
            source->set_run_ahead(settings.run_ahead_ds);
        }
    }
    if (result.close_requested) {
        if (settings_came_from == State::Menu && local_game()) {
            // Back to the menu it was opened from, rebuilt from what the
            // session looks like now rather than from what it looked like then
            // -- a companion may have arrived or left while this screen was up.
            set_menu_open(true);
        } else if (settings_came_from == State::Playing && local_game()) {
            // Opened from the list a Wii U host shows while Cemu plays: back
            // to it, with the game never having stopped.
            state = State::Playing;
            input.set_game_input_enabled(true);
        } else {
            open_launcher();
        }
    }
}

void App::Impl::end_companion_session() {
    log_info("the session this device joined has ended; leaving what it was showing");
    // Whatever was on screen was about the session, so none of it survives it.
    // The menu especially: left open it would start answering its own presses,
    // since there is no longer a host to forward them to -- which is how
    // choosing Quit from here used to leave a confirmation up that then quit
    // this device instead of the game.
    if (picker) {
        picker->close();
    }
    if (companion_session->status() == net::CompanionSession::Status::Failed) {
        enter_message(MessageKind::Error, "Lost the connection",
                      companion_session->status_detail(), {}, false);
        return;
    }
    // An ordinary end -- the other device stopped, or this one asked to leave.
    // Nothing went wrong, so there is nothing to read.
    open_launcher();
}

void App::Impl::open_launcher() {
    if (!launcher) {
        launcher.reset(new LauncherScreen());
    }
    launcher->open();
    state = State::Launcher;
    input.set_game_input_enabled(false);
}

void App::Impl::start_companion(ScreenId wanted_screen) {
    if (!companion_session) {
        companion_session.reset(new net::CompanionSession());
    }
    attach_companion_callbacks();
    std::string error;
    if (!companion_session->begin_search(settings.device_name, wanted_screen, &error)) {
        enter_message(MessageKind::Error, "Cannot look for other devices", error,
                      {"Another copy of dualscreen may already be searching on this device."},
                      false);
        return;
    }
    local_screen = wanted_screen;
    // Where this device starts. Once a host is on the other end, both of these
    // come from it instead -- see the companion branch of the frame loop.
    bilinear_filtering = settings.default_bilinear_filtering;
    render_scale = settings.default_render_scale;
    audio_target = settings.audio_target;
    left_handed_layouts = settings.left_handed_layouts;
    screen_gap = settings.default_screen_gap;
    right_stick_mode = settings.default_right_stick_mode;
    input.set_right_stick_mode(right_stick_mode);
    apply_filtering();
    if (!host_picker) {
        host_picker.reset(new HostPickerScreen());
    }
    host_picker->open(wanted_screen);
    state = State::HostPicker;
    input.set_game_input_enabled(false);
}

void App::Impl::enter_message(MessageKind kind,
                              const std::string& title,
                              const std::string& body,
                              const std::vector<std::string>& details,
                              bool fatal) {
    message.show(kind, title, body, details);
    message_is_fatal = fatal;
    state = State::Message;
    input.set_game_input_enabled(false);
}

void App::Impl::open_browser() {
    if (!browser) {
        browser.reset(new GameBrowser());
    }
    browser->open(settings.last_browse_dir);
    state = State::Browser;
    input.set_game_input_enabled(false);
}

bool App::Impl::open_core(const std::string& rom_path, std::string* error,
                          std::vector<std::string>* details) {
    const SystemProfile& profile = system_profile(system);
    LibretroSourceConfig source_config;
    source_config.system = system;
    source_config.core_path = resolved_core_path;
    source_config.rom_path = rom_path;
    source_config.system_directory = settings.system_dir;
    source_config.save_directory = paths.saves_dir;
    // Named after the ROM rather than its hash, so saves are interchangeable
    // with RetroArch's and recognisable in a file listing.
    source_config.save_path = path_join(paths.saves_dir, path_stem(rom_path) + ".srm");
    source_config.render_scale = render_scale;
    source_config.rumble_pak = have_game_config && game_config.rumble_pak;

    source.reset(new LibretroCoreScreenSource());
    source->set_audio_callback([this](const int16_t* samples, std::size_t frames) {
        if (sound_plays_here()) {
            audio.push(samples, frames);
        }
        // Mirrored to the companion as it is produced, so both devices hear the
        // same thing at the same time.
        if (host_session) {
            host_session->publish_audio(net::AudioStreamId::Shared, samples, frames,
                                        static_cast<uint32_t>(source->audio_sample_rate()));
        }
    });
    // A DS game's Rumble Pak. Both devices' pads drive a DS, so both feel it:
    // this one's here, the other's through the session.
    source->set_rumble_callback([this](bool active) {
        input.set_rumble(active);
        if (host_session) {
            host_session->publish_rumble(active);
        }
    });

    if (!source->open(source_config, error)) {
        if (details != nullptr) {
            if (resolved_core_path.empty()) {
                details->push_back("Looked for " + profile.core_filenames.front() +
                                   " in the RetroArch core folders and /usr/lib/libretro.");
                details->push_back("Point at it directly with --core, or set \"" +
                                   std::string(core_path_settings_key(system)) + "\" in " +
                                   paths.settings_file + ".");
            } else {
                details->push_back("Core: " + resolved_core_path);
            }
            details->push_back("BIOS folder: " + settings.system_dir);
        }
        source.reset();
        return false;
    }

    // A scale from an older config, or one this system does not offer, is
    // clamped by the source; the menu should show what the core was given.
    render_scale = source->render_scale();
    if (system == SystemId::Ds) {
        source->set_run_ahead(settings.run_ahead_ds);
    }
    return true;
}

GlContextNeed App::Impl::gl_context_for_game() const {
    const SystemProfile& profile = system_profile(system);
    if (profile.frame_origin == FrameOrigin::LibretroCore &&
        LibretroCoreScreenSource::renders_on_gpu(
            system, std::clamp(render_scale, 1, std::max(1, profile.max_render_scale)))) {
        return GlContextNeed::OpenGl33Core;
    }
    return profile.gl_context;
}

bool App::Impl::reopen_core_in_place() {
    if (!source) {
        return false;
    }
    // Where the game is, carried across as a save state: the core is closed
    // and opened again, and nothing else is -- the session, the companion, the
    // audio device and the window all stay, bar the window's GL context when
    // the new renderer needs a different one.
    std::vector<uint8_t> state;
    std::string state_error;
    const bool kept = source->save_state(state, &state_error);
    if (!kept) {
        log_warn("could not keep the game's place across reopening it: " + state_error);
    }
    stop_pad_rumble();
    source->close();
    source.reset();

    std::string error;
    if (!ensure_gl_context(gl_context_for_game(), &error)) {
        // Back down to what the old context can do, rather than no game.
        log_warn("could not get the display the new renderer needs: " + error);
        render_scale = 1;
        persist_game_config();
    }
    std::vector<std::string> details;
    if (!open_core(game_path, &error, &details)) {
        enter_message(MessageKind::Error, "Could not start the game again", error, details, false);
        return false;
    }
    if (kept && !source->load_state(state, &state_error)) {
        log_warn("the game started again from the beginning: " + state_error);
    }
    // One frame, so there is a picture: a game paused under the menu that
    // asked for this would otherwise show nothing behind it, and send the
    // other device nothing, until it was resumed.
    std::string frame_error;
    source->advance_frame(&frame_error);
    frame_accumulator = 0.0;
    last_counter = SDL_GetPerformanceCounter();
    log_info("reopened the core at " + std::to_string(render_scale) + "x");
    return true;
}

bool App::Impl::start_game(const std::string& rom_path, bool fatal_on_failure) {
    // Tear down anything already running, so picking a second game from the
    // browser is the same code path as booting the first.
    audio.close();
    if (source) {
        source->close();
        source.reset();
    }
    external_program.stop();
    external_source.reset();
    // A motor the last game left on has nobody left to turn it off.
    stop_pad_rumble();
    have_game_config = false;
    game_config = GameConfig();

    if (!file_exists(rom_path)) {
        enter_message(MessageKind::Error, "ROM not found", "There is no file at " + rom_path,
                      {"Check the path in this game's Steam shortcut."}, fatal_on_failure);
        return false;
    }

    // What the console itself would call this, rather than what the file is
    // called. See game_title.hpp: every one of these formats carries the real
    // name, and the file name is the fallback rather than the answer.
    game_title = game_title_for_file(rom_path);
    if (game_title.empty()) {
        game_title = path_stem(rom_path);
        // Which is a guess, and says so. A Wii U keeps its name in
        // meta/meta.xml beside the game rather than inside it, so a format
        // that bundles the lot into one file has nothing here to read; an
        // encrypted 3DS card image keeps its name behind a key this app does
        // not have. Worth a line either way, because "the name is wrong" and
        // "there was no name to find" look identical from the outside, and
        // telling them apart from a log is the whole point of saying anything.
        log_info("nothing in " + path_basename(rom_path) +
                 " says what this game is called, so it is showing as \"" + game_title +
                 "\", after the file");
    } else if (game_title != path_stem(rom_path)) {
        log_info("this game calls itself " + game_title);
    }
    active_layout_id = kNewGameLayoutId;
    bilinear_filtering = settings.default_bilinear_filtering;
    audio_target = settings.audio_target;
    left_handed_layouts = settings.left_handed_layouts;
    screen_gap = settings.default_screen_gap;
    right_stick_mode = settings.default_right_stick_mode;

    const std::string rom_sha1 = Sha1::hash_file(rom_path);
    render_scale = settings.default_render_scale;
    // Whether this game has been played before, which decides where a DS
    // game's render scale starts -- see below.
    const bool remembered =
        !rom_sha1.empty() && file_exists(GameConfig::file_path(paths, rom_sha1));
    if (rom_sha1.empty()) {
        log_warn("could not hash the ROM; this session's layout will not be remembered");
    } else {
        game_config = GameConfig::load_or_default(paths, settings, rom_sha1, game_title);
        have_game_config = true;
        active_layout_id = game_config.layout_id;
        bilinear_filtering = game_config.bilinear_filtering;
        render_scale = game_config.render_scale;
        screen_gap = game_config.screen_gap;
        right_stick_mode = game_config.right_stick_mode;
    }
    input.set_right_stick_mode(right_stick_mode);
    if (!options.layout_id.empty()) {
        active_layout_id = options.layout_id;
    }
    // An unknown id from an old config or a typo'd --layout falls back rather
    // than leaving the player with a blank screen.
    active_layout_id = registry.resolve(active_layout_id).id;

    // The extension says which console this is for, which decides the core, the
    // screen sizes and everything downstream of them.
    if (!system_for_rom(rom_path, system)) {
        system = SystemId::Ds;
    }
    const SystemProfile& profile = system_profile(system);
    resolved_core_path = resolve_core_path(options, settings, system);
    apply_system();
    // A DS game new to this app starts at 1x, on melonDS's software renderer,
    // whatever scale 3DS games have been left at: upscaling a DS is a choice
    // made game by game in the quick menu, not a default carried over from
    // another console.
    if (system == SystemId::Ds && !remembered) {
        render_scale = 1;
    }

    // Resolved once more now the console is known. A layout remembered from a
    // different one, or the global default, may have nowhere to put these
    // screens -- and a layout that cannot place them draws nothing at all,
    // which looks exactly like a game that failed to start.
    if (!layout_fits_system(registry.resolve(active_layout_id), system, profile.screens)) {
        const std::string instead = profile.default_layout_id.empty()
                                        ? LayoutRegistry::fallback_id()
                                        : profile.default_layout_id;
        log_info("the " + registry.resolve(active_layout_id).display_name +
                 " layout has no shape for a " + profile.name + "; using " +
                 registry.resolve(instead).display_name + " instead");
        active_layout_id = registry.resolve(instead).id;
    }

    // Before the core is loaded, for the same reason the context is: the core
    // resolves its GL functions once, while the game is being opened, and keeps
    // whatever it was handed.
    if (options.trace_core_gl) {
        gl_trace::force_enable();
    }

    // Before the core is loaded, not after: a core that renders on the GPU asks
    // for its context while the game is being opened and refuses to open it at
    // all if the answer is no.
    std::string context_error;
    if (!ensure_gl_context(gl_context_for_game(), &context_error)) {
        enter_message(MessageKind::Error, "Cannot set up the display for " + profile.name,
                      context_error,
                      {profile.core_description + " renders on the GPU and needs OpenGL 3.3."},
                      fatal_on_failure);
        return false;
    }

    switch (profile.frame_origin) {
        case FrameOrigin::LibretroCore: {
            game_path = rom_path;
            std::string source_error;
            std::vector<std::string> details;
            if (!open_core(rom_path, &source_error, &details)) {
                enter_message(MessageKind::Error, "Could not start the game", source_error, details,
                              fatal_on_failure);
                return false;
            }
            break;
        }
        case FrameOrigin::External: {
            // Nothing is loaded here. The game is played by another program,
            // which is started separately and connects to this socket when it
            // is ready -- so opening a game means opening the way in and then
            // waiting, and the black screen until it arrives is correct.
            external_source.reset(new ExternalScreenSource(system));
            external_source->set_audio_callback(
                [this](ScreenId screen, const int16_t* samples, std::size_t frames) {
                    // Not a player choice on either end, and no target to
                    // track here at all: Cemu already made this call on its
                    // own side (see DualscreenSession::ShouldCapture in
                    // ax_out.cpp) before this callback ever fires -- a screen
                    // it is showing locally is never sent over the link in
                    // the first place, only played there directly, so
                    // whichever screen's audio does arrive here is, by
                    // construction, audio meant for whatever is connected,
                    // never for this machine. That is also what makes this
                    // already handle a companion holding either screen: a
                    // companion connecting as the top screen leaves Cemu
                    // showing (and playing) the GamePad locally and sending
                    // only the television's audio here, the mirror image of
                    // the usual case, with nothing here needing to know
                    // which way round it is.
                    if (host_session) {
                        host_session->publish_audio(screen == ScreenId::Top
                                                        ? net::AudioStreamId::TopDevice
                                                        : net::AudioStreamId::BottomDevice,
                                                    samples, frames,
                                                    static_cast<uint32_t>(
                                                        external_source->audio_sample_rate()));
                    }
                });
            external_source->set_rumble_callback([this](bool active) {
                // The motor belongs to whoever is holding the GamePad, and
                // that is not always the other device.
                //
                // This used to send it onward unconditionally, on the
                // assumption that a machine running the game is a machine on a
                // desk with a television attached. A Steam Deck hosting for a
                // Mac is the other way round: the Deck is the thing in two
                // hands and the Mac is the television, so every rumble the
                // game asked for was crossing the network to a laptop with no
                // motor while the pad actually being held stayed still.
                //
                // Which device that is, is a thing this app already knows --
                // it is whichever one is showing the GamePad's screen.
                //
                // On a console whose players each hold their own controller,
                // that is whichever pads are the GamePad -- see
                // route_wii_u_pads().
                if (system_profile(system).pad_belongs_to_its_screen) {
                    slot_rumble[0] = active;
                    apply_pad_rumble();
                } else if (gamepad_is_here()) {
                    input.set_rumble(active);
                } else if (host_session) {
                    host_session->publish_rumble(active);
                }
            });
            external_source->set_player_rumble_callback([this](int player, bool active) {
                if (player < 0 || player >= kPadPlayers) return;
                slot_rumble[1 + player] = active;
                apply_pad_rumble();
            });

            const std::string socket_path = options.external_socket.empty()
                                                ? ExternalFrameLink::default_socket_path()
                                                : options.external_socket;
            external_socket_path = socket_path;
            std::string link_error;
            if (!external_source->open(socket_path, &link_error)) {
                external_source.reset();
                enter_message(MessageKind::Error,
                              "Could not open the way in for " + profile.core_description,
                              link_error, {"Socket: " + socket_path}, fatal_on_failure);
                return false;
            }
            render_scale = 1;  // whatever is playing the game owns its resolution
            external_game_arrived = false;
            solo_screen_unchecked = true;
            log_info(profile.core_description + " can hand its screens over at " + socket_path);

            // Started here rather than by the player: in Game Mode there is no
            // desktop to start it from, which is the whole reason this app
            // exists. Not fatal when it fails -- the socket is open either way
            // and a player on a desktop can still start one by hand.
            const std::string& program = settings.external_program_for(system);
            if (!program.empty()) {
                std::vector<std::string> arguments;
                for (const std::string& argument : settings.external_arguments_for(system)) {
                    arguments.push_back(fill_placeholders(argument, socket_path, rom_path));
                }
                std::string start_error;
                if (external_program.start(program, arguments, &start_error)) {
                    external_program_wanted = true;
                } else {
                    log_warn("could not start " + program + ": " + start_error);
                }
            }
            break;
        }
    }

    // Until an external game has connected these are the link's own defaults
    // rather than what it is really sending; adopt_external_timing() replaces
    // them the moment it says.
    frame_period = 1.0 / std::max(1.0, local_frames_per_second());
    std::string audio_error;
    if (!audio.open(local_audio_sample_rate(), &audio_error)) {
        log_warn(audio_error + "; continuing without sound");
    }

    state = State::Playing;
    input.set_game_input_enabled(true);
    apply_filtering();
    frame_accumulator = 0.0;
    last_counter = SDL_GetPerformanceCounter();

    // Offer this game to the network. A failure here is not fatal: the game is
    // perfectly playable on one device.
    host_session.reset(new net::HostSession());
    net::HostSession::Config host_config;
    host_config.device_name = settings.device_name;
    host_config.game_title = game_title;
    host_config.rom_sha1 = game_config.rom_sha1;
    host_config.host_screen = local_screen;
    host_config.system = system;
    host_config.audio_sample_rate = static_cast<uint32_t>(local_audio_sample_rate());
    std::string host_error;
    if (!host_session->start(host_config, &host_error)) {
        log_warn("not hosting for other devices: " + host_error);
        host_session.reset();
    }

    if (source && !source->warnings().empty()) {
        enter_message(MessageKind::Info, "The core did not accept every setting",
                      "The game will still run, but the screen split may not look right until this "
                      "is resolved.",
                      source->warnings(), false);
    }
    return true;
}

void App::Impl::persist_game_config() {
    if (!have_game_config) {
        return;
    }
    game_config.layout_id = active_layout_id;
    game_config.bilinear_filtering = bilinear_filtering;
    game_config.render_scale = render_scale;
    game_config.screen_gap = screen_gap;
    game_config.right_stick_mode = right_stick_mode;
    game_config.touch_last_played();
    if (!game_config.save(paths)) {
        log_warn("could not save this game's layout");
    }
}

void App::Impl::handle_ui(const std::vector<UiAction>& actions,
                          const std::vector<PointerEvent>& pointers) {
    // Filtering is deliberately handled before anything else looks at the
    // actions: R3 means the same thing in a game and in every menu, so it does
    // not belong to any one screen.
    for (const UiAction action : actions) {
        if (action == UiAction::ToggleFilter) {
            request_filtering(!bilinear_filtering);
            break;
        }
    }

    // L3 flips between a layout and the one it pairs with -- the two focus
    // layouts -- without stopping the game to open a menu. Only with a game on
    // screen, on either device: anywhere else there is no layout being played
    // to swap, and in the menu itself every layout is already a tile away.
    if (state == State::Playing || state == State::Companion) {
        for (const UiAction action : actions) {
            if (action == UiAction::SwapLayout) {
                // On a console whose devices each show a screen of their own,
                // there is no pair of layouts to flip between -- what "the
                // other one" means is the other screen. With a companion
                // connected it means nothing at all: both screens are already
                // being shown, one on each device.
                if (system_profile(system).solo_screens == SoloScreens::One) {
                    if (!paired()) {
                        solo_screen_swapped = !solo_screen_swapped;
                        update_solo_screen();
                    }
                } else {
                    request_layout(active_layout().swap_layout_id);
                }
                break;
            }
        }
    }

    switch (state) {
        case State::Message: {
            if (message.update(actions, pointers, last_delta_seconds)) {
                message.clear();
                if (!message_is_fatal && is_companion()) {
                    state = State::Companion;
                    input.set_game_input_enabled(true);
                } else if (!message_is_fatal && local_game()) {
                    state = State::Playing;
                    input.set_game_input_enabled(true);
                } else if (!message_is_fatal || launcher) {
                    // Nothing here to go back to: this device was showing
                    // somebody else's game and that session is over. The main
                    // menu is where it came from and where it can start again.
                    if (companion_session) {
                        companion_session->disconnect();
                    }
                    open_launcher();
                } else if (launcher) {
                    // Getting here through the launcher means there is
                    // somewhere to go back to, so a failure is not the end of
                    // the session.
                    open_launcher();
                } else if (browser) {
                    open_browser();
                } else {
                    running = false;
                }
            }
            break;
        }

        case State::Launcher: {
            const LauncherResult result =
                launcher->update(actions, pointers, static_cast<float>(output_width),
                                 static_cast<float>(output_height));
            switch (result.choice) {
                case LauncherChoice::LoadGame:
                    open_browser();
                    break;
                case LauncherChoice::ConnectAsTopScreen:
                    start_companion(ScreenId::Top);
                    break;
                case LauncherChoice::ConnectAsBottomScreen:
                    start_companion(ScreenId::Bottom);
                    break;
                case LauncherChoice::Settings:
                    settings_screen->open(settings, resolve_core_statuses(options, settings));
                    state = State::Settings;
                    break;
                case LauncherChoice::Quit:
                    running = false;
                    break;
                case LauncherChoice::None:
                    break;
            }
            break;
        }

        case State::HostPicker: {
            const HostPickerResult result = host_picker->update(
                companion_session->hosts(), actions, pointers, static_cast<float>(output_width),
                static_cast<float>(output_height));
            if (result.cancelled) {
                companion_session->disconnect();
                open_launcher();
            } else if (result.chosen) {
                joining_host = result.host;
                companion_session->connect_to(result.host);
                state = State::Companion;
            }
            break;
        }

        case State::Companion: {
            // Still joining: the connecting screen is up, its only offer is to
            // stop waiting, and nothing is connected for a button to drive.
            if (companion_session->status() == net::CompanionSession::Status::Connecting) {
                for (const UiAction action : actions) {
                    if (action == UiAction::Cancel || action == UiAction::ToggleMenu) {
                        companion_session->disconnect();
                        open_launcher();
                        break;
                    }
                }
                break;
            }
            // The host's settings are up. This device is not drawing them --
            // see MenuAction::Settings -- but the player is still holding a pad
            // and it is still their session, so the presses go over.
            if (companion_session->host_settings_open()) {
                input.set_game_input_enabled(false);
                for (const UiAction action : actions) {
                    net::MenuAction wire = net::MenuAction::Confirm;
                    if (menu_action_for(action, wire)) {
                        companion_session->request_menu_action(wire);
                    }
                }
                break;
            }
            // Connected: the companion's own touchscreen and buttons drive the
            // host's game from here on.
            input.set_game_input_enabled(true);
            for (const UiAction action : actions) {
                if (action == UiAction::ToggleMenu || action == UiAction::OpenMenu) {
                    // Asked for rather than opened. The host holds the one
                    // menu; this device puts it up when told, which is what
                    // keeps the two screens showing the same thing.
                    companion_session->set_menu_open(true);
                    break;
                }
            }
            break;
        }

        case State::Browser: {
            const GameBrowserResult result =
                browser->update(input, actions, pointers, static_cast<float>(output_width),
                                static_cast<float>(output_height));
            if (result.directory_changed) {
                settings.last_browse_dir = browser->current_directory();
                if (!settings.save(paths)) {
                    log_warn("could not remember the browsed folder");
                }
            }
            if (result.back_requested) {
                open_launcher();
            } else if (result.quit_requested) {
                running = false;
            } else if (result.game_chosen) {
                settings.last_browse_dir = browser->current_directory();
                settings.save(paths);
                start_game(result.chosen_path, false);
            }
            break;
        }

        case State::Menu: {
            // One menu, held by whichever device runs the game. A companion
            // does not answer its own presses: it forwards them and redraws
            // whatever the host says the menu now looks like, so both screens
            // show the same thing and either pad can work it.
            if (is_companion()) {
                for (const UiAction action : actions) {
                    net::MenuAction wire = net::MenuAction::Confirm;
                    if (menu_action_for(action, wire)) {
                        companion_session->request_menu_action(wire);
                    }
                }
                for (const PointerEvent& pointer : pointers) {
                    if (!pointer.released) continue;
                    // A finger on one of the control prompts is that button
                    // being pressed, forwarded like any other press.
                    const ButtonGlyph on_guide = picker->guide_button_at(pointer.x, pointer.y);
                    if (on_guide != ButtonGlyph::Count &&
                        on_guide == picker->guide_button_at(pointer.press_x, pointer.press_y)) {
                        net::MenuAction wire = net::MenuAction::Confirm;
                        bool forward = true;
                        switch (on_guide) {
                            case ButtonGlyph::A: wire = net::MenuAction::Confirm; break;
                            case ButtonGlyph::B: wire = net::MenuAction::Cancel; break;
                            case ButtonGlyph::X: wire = net::MenuAction::Quit; break;
                            case ButtonGlyph::Y: wire = net::MenuAction::Reset; break;
                            case ButtonGlyph::Plus: wire = net::MenuAction::Disconnect; break;
                            // Smoothing is a shared setting rather than a menu
                            // press, asked for the way its button asks for it.
                            case ButtonGlyph::RightStick:
                                request_filtering(!bilinear_filtering);
                                forward = false;
                                break;
                            default: forward = false; break;
                        }
                        if (forward) companion_session->request_menu_action(wire);
                        continue;
                    }
                    const auto entry_under = [&](float x, float y) {
                        return picker->entry_at(x, y, static_cast<float>(output_width),
                                                static_cast<float>(output_height));
                    };
                    const int entry = entry_under(pointer.x, pointer.y);
                    // Only where the finger came down on the same entry it
                    // left on.
                    if (entry >= 0 && entry == entry_under(pointer.press_x, pointer.press_y)) {
                        companion_session->request_menu_press(entry, net::MenuAction::Confirm);
                    }
                }
                break;
            }
            const LayoutPickerResult result =
                picker->update(actions, pointers, static_cast<float>(output_width),
                               static_cast<float>(output_height));
            apply_menu_result(result);
            break;
        }

        case State::Controllers: {
            update_controllers_screen(actions, pointers);
            break;
        }


        case State::Settings: {
            apply_settings_result(
                settings_screen->update(actions, pointers, static_cast<float>(output_width),
                                        static_cast<float>(output_height)));
            break;
        }

        case State::Playing: {
            if (shows_embedded_menu()) {
                // The Wii U list is already on screen in place of the menu,
                // so there is nothing for the menu keys to open. It answers
                // only while this window has the focus: the pads keep
                // reporting while Cemu's window is in front, and a press
                // meant for the game must not also pick Quit here.
                if (!picker->embedded()) {
                    open_picker(/*embedded=*/true);
                }
                const bool focused =
                    window != nullptr &&
                    (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
                if (focused != embedded_menu_focused) {
                    embedded_menu_focused = focused;
                    log_info(focused ? "this window has the focus; the list beside the game answers"
                                     : "this window lost the focus; the list beside the game is "
                                       "left alone");
                }
                if (focused) {
                    apply_menu_result(picker->update(actions, pointers,
                                                     static_cast<float>(output_width),
                                                     static_cast<float>(output_height)));
                }
                break;
            }
            if (picker && picker->embedded()) {
                // The game beside it ended or has not arrived.
                picker->close();
            }
            for (const UiAction action : actions) {
                if (action == UiAction::ToggleMenu || action == UiAction::OpenMenu) {
                    set_menu_open(true);
                    if (source) {
                        source->flush_save_ram(false);
                    }
                    break;
                }
            }
            break;
        }
    }

}

// Swaps, and under DUALSCREEN_TEST_VSYNC_HZ then waits for the next tick of
// a clock at that rate, the way a display's vertical refresh holds a real
// swap. A virtual display's swap returns at once, which would let a loop spin
// free and hide exactly the refresh-locked waits that latency is made of on
// real hardware. For measuring, and nothing else.
void swap_and_pace(SDL_Window* window) {
    SDL_GL_SwapWindow(window);
    static const double hz = [] {
        const char* value = std::getenv("DUALSCREEN_TEST_VSYNC_HZ");
        return value != nullptr ? std::atof(value) : 0.0;
    }();
    if (hz <= 0.0) return;
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    const auto period = std::chrono::duration<double>(1.0 / hz);
    const double elapsed = std::chrono::duration<double>(clock::now() - start).count();
    const double ticks = std::floor(elapsed / period.count()) + 1.0;
    std::this_thread::sleep_until(start + std::chrono::duration_cast<clock::duration>(period * ticks));
}

void App::Impl::render() {
    const float width = static_cast<float>(output_width);
    const float height = static_cast<float>(output_height);
    // A GL error is sticky and otherwise invisible: the picture is simply wrong
    // and nothing says why. Reported once, since a failing draw fails every
    // frame and one line is the useful amount.
    gl::log_error_once("something in the last frame was rejected by OpenGL");

    if (state == State::Browser) {
        browser->render(quads, text, button_glyphs, width, height);
        swap_and_pace(window);
        ++frames_rendered;
        return;
    }
    if (state == State::Launcher) {
        launcher->render(quads, text, button_glyphs, width, height,
                         companion_session ? static_cast<int>(companion_session->hosts().size())
                                           : 0);
        swap_and_pace(window);
        ++frames_rendered;
        return;
    }
    if (state == State::HostPicker) {
        host_picker->render(quads, text, button_glyphs, width, height,
                            companion_session->hosts(), companion_session->status_detail());
        swap_and_pace(window);
        ++frames_rendered;
        return;
    }
    if (companion_session &&
        companion_session->status() == net::CompanionSession::Status::Connecting) {
        // Only when it is this machine holding things up, and said in the
        // words the player has to act on -- a system prompt they may not be
        // able to see behind a fullscreen window.
        const std::string note =
            companion_session->waiting_on_local_network()
                ? std::string("Waiting for this Mac to allow the local network. If it asked, "
                              "say yes; otherwise turn it on in System Settings, Privacy & "
                              "Security, Local Network.")
                : std::string();
        render_connecting_screen(quads, text, button_glyphs, width, height,
                                 joining_host.device_name, joining_host.game_title, note);
        swap_and_pace(window);
        ++frames_rendered;
        return;
    }

    // A companion showing a console played by a program beside the host: until
    // that program starts there is nothing for the host to send, and this
    // device would draw the same blank window the host used to. It works this
    // out rather than being told -- it knows which console it joined, and that
    // nothing has arrived -- which keeps the wire out of it.
    // The host has its settings up. They are about that machine -- where its
    // BIOS files and its emulators are -- so there is nothing useful to mirror
    // here, and a frozen game would look like a session that had stopped.
    if (state == State::Companion && companion_session &&
        companion_session->host_settings_open()) {
        // Settings or Controllers: both are the host's own screens, and a
        // name for either here could be the wrong one.
        render_waiting_screen(quads, text, button_glyphs, width, height, "On the other screen",
                              {"A menu is open there.", "This device's buttons still work it."},
                              {});
        swap_and_pace(window);
        ++frames_rendered;
        companion_session->note_frame_drawn();
        return;
    }

    if (state == State::Companion && companion_session &&
        system_profile(system).frame_origin == FrameOrigin::External) {
        const ScreenTextures& arrived = companion_session->screens();
        if (arrived.top == nullptr && arrived.bottom == nullptr) {
            const SystemProfile& profile = system_profile(system);
            render_waiting_screen(quads, text, button_glyphs, width, height,
                                  "Waiting for " + profile.core_description,
                                  {joining_host.game_title,
                                   joining_host.device_name.empty()
                                       ? std::string()
                                       : "to start on " + joining_host.device_name,
                                   "F1 or Tab for the menu"},
                                  {});
            swap_and_pace(window);
            ++frames_rendered;
            companion_session->note_frame_drawn();
            return;
        }
    }

    if (state == State::Playing && external_source && !external_source->running()) {
        // The game is played by a program started separately, and it has not
        // arrived. A black window here would be the same black window a broken
        // one gives, so it says what it is waiting for and where.
        const SystemProfile& profile = system_profile(system);
        // Two quite different situations that look the same from here: one
        // where this app started the program and is waiting for it to come up,
        // and one where nobody has started it and the player has to.
        const bool ours = external_program_wanted;
        std::vector<std::string> lines{game_title};
        if (!ours) {
            lines.push_back("Start it and point it at " + external_socket_path);
        }
        lines.push_back("F1 or Tab for the menu");
        render_waiting_screen(quads, text, button_glyphs, width, height,
                              (ours ? "Starting " : "Waiting for ") + profile.core_description,
                              lines, {});
        swap_and_pace(window);
        ++frames_rendered;
        return;
    }

    const ScreenTextures* frames = nullptr;
    if (system == SystemId::WiiU && !is_companion() && external_source &&
        external_source->running()) {
        // Never composited locally any more -- Cemu's own window is the only
        // place either of a Wii U's screens appears now, and a connected
        // companion gets whichever one it asked for over the network. See
        // the request_screen() block in the host poll loop.
        //
        // What shows instead is the list the quick menu would have, so it is
        // one step from here rather than two -- see LayoutPickerState::embedded.
        if (state == State::Playing && picker && picker->embedded()) {
            quads.begin_frame(output_width, output_height);
            quads.clear(ui_theme().background);
            picker->render(quads, text, button_glyphs, width, height, game_title, nullptr);
        } else {
            const SystemProfile& profile = system_profile(system);
            render_waiting_screen(quads, text, button_glyphs, width, height,
                                  "Playing on " + profile.core_description, {game_title}, {});
        }
    } else if (ScreenSource* drawing = active_source()) {
        const ScreenTextures& textures = drawing->screens();
        compositor.render(quads, device_layout(), textures, screens_geometry(), width, height,
                          router);
        frames = &textures;
    } else {
        quads.begin_frame(output_width, output_height);
        quads.clear(compositor.style().background);
    }

    // Settings gets the same blurred backdrop as the menu it was opened from,
    // when that is where it was opened from: the game is still paused behind
    // it either way, and a Settings reached from the Launcher has no game
    // frame to blur at all.
    const bool show_menu_backdrop =
        state == State::Menu || (state == State::Settings && settings_came_from == State::Menu) ||
        (state == State::Controllers && controllers_came_from == State::Menu);
    if (show_menu_backdrop) {
        // Taken here, between the game and the menu, so what gets blurred is
        // exactly the frame underneath it -- and taken again when that frame
        // stops being the one that was copied, which is what picking a layout
        // does while the menu is still up.
        if (!menu_backdrop.matches(compositor.composition_key())) {
            menu_backdrop.capture(output_width, output_height, compositor.composition_key());
        }
        menu_backdrop.draw(quads, width, height);
    } else {
        menu_backdrop.forget();
    }
    if (state == State::Menu) {
        // The previews are drawn from the frame the game is paused on, so each
        // tile shows what picking it will actually look like.
        picker->render(quads, text, button_glyphs, width, height, game_title, frames);
    } else if (state == State::Controllers) {
        controllers_screen.render(quads, text, button_glyphs, width, height,
                                  gather_pads(companion_joined()), holds_gamepad(), pad_order,
                                  settings.device_name,
                                  companion_joined() ? host_session->companion_name()
                                                     : std::string("other device"));
    } else if (state == State::Settings) {
        settings_screen->render(quads, text, button_glyphs, width, height);
    } else if (state == State::Message) {
        message.render(quads, text, button_glyphs, width, height);
    }

    swap_and_pace(window);
    ++frames_rendered;
    // Only the game path gets here, which is the one whose rate says whether a
    // companion is showing what it is being sent.
    if (companion_session) {
        companion_session->note_frame_drawn();
    }
}

bool App::Impl::write_screenshot(const std::string& path) {
    if (gl::ReadPixels == nullptr || output_width <= 0 || output_height <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<std::size_t>(output_width) * output_height * 4);
    gl::ReadPixels(0, 0, output_width, output_height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // Plain PPM: no image library, and every tool can read it.
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file << "P6\n" << output_width << " " << output_height << "\n255\n";
    for (int row = output_height - 1; row >= 0; --row) {  // GL origin is bottom-left
        for (int column = 0; column < output_width; ++column) {
            const std::size_t offset =
                (static_cast<std::size_t>(row) * output_width + column) * 4;
            file.put(static_cast<char>(pixels[offset + 0]));
            file.put(static_cast<char>(pixels[offset + 1]));
            file.put(static_cast<char>(pixels[offset + 2]));
        }
    }
    return file.good();
}

// ---------------------------------------------------------------------------

int App::run(const AppOptions& options) {
    Impl& impl = *impl_;
    impl.options = options;

    if (options.show_help) {
        std::printf("%s", usage_text().c_str());
        return 0;
    }
    if (!options.parse_error.empty()) {
        std::fprintf(stderr, "dualscreen: %s\n\n%s", options.parse_error.c_str(), usage_text().c_str());
        return 2;
    }
    if (options.verbose) {
        set_min_log_level(LogLevel::Debug);
    }

    impl.paths = AppPaths::resolve();

    // Opened before anything that can fail. In Game Mode there is no terminal,
    // so without a log on disk an early failure is indistinguishable from the
    // app never starting at all.
    ensure_directory(path_dirname(impl.paths.log_file));
    if (set_log_file(impl.paths.log_file)) {
        log_info("dualscreen starting; log at " + impl.paths.log_file);
    }
    // Immediately after, because everything below is read in the light of it:
    // a log from a binary older than the change being looked for says nothing
    // about the change. See build_stamp.hpp.
    log_build_stamp();
    // The environment is what actually differs between Game Mode and desktop,
    // so it is the first thing worth knowing when only one of them works.
    for (const char* name : {"XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY", "SDL_VIDEODRIVER",
                             "XDG_RUNTIME_DIR", "GAMESCOPE_WAYLAND_DISPLAY", "STEAM_COMPAT_DATA_PATH",
                             "GTK_IM_MODULE", "QT_IM_MODULE", "XMODIFIERS"}) {
        const char* value = std::getenv(name);
        log_info(std::string("env ") + name + " = " + (value != nullptr ? value : "(unset)"));
    }

    impl.settings = Settings::load(impl.paths);
    impl.adopt_legacy_handedness();
    impl.button_glyphs.set_style(impl.settings.pad_glyphs);
    impl.input.set_confirm_button(impl.settings.confirm_button);
    set_confirm_button(impl.settings.confirm_button);
    if (!options.system_dir.empty()) {
        impl.settings.system_dir = expand_user_path(options.system_dir);
    }


    if (options.list_layouts) {
        for (const Layout& layout : impl.registry.layouts()) {
            std::printf("%-20s %s\n", layout.id.c_str(), layout.display_name.c_str());
        }
        return 0;
    }

    ensure_directory(impl.paths.config_dir);
    ensure_directory(impl.paths.games_dir);
    ensure_directory(impl.paths.saves_dir);
    ensure_directory(impl.settings.system_dir);

    apply_controller_hints();

    // Video and events are the only subsystems worth failing over. The game
    // controller subsystem is initialised separately because it can fail on its
    // own -- device permissions differ inside Game Mode's sandbox -- and losing
    // controllers is a reason to warn, never a reason to refuse to start.
    // And never an on-screen keyboard, whatever asks for one: nothing here is
    // typed into, and on a Steam Deck SDL raises Steam's the moment text input
    // starts. See create_gl_window(), which also turns text input off.
    // By its string, which is what SDL reads: the name for it arrived in SDL
    // 2.28's headers, and Debian 12 -- what the SteamOS build is made in --
    // has 2.26. The SDL it runs against on the Deck is newer and honours it.
    SDL_SetHint("SDL_ENABLE_SCREEN_KEYBOARD", "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        const std::string reason = SDL_GetError();
        log_error("could not start SDL video: " + reason);
        log_error("SDL knows about these video drivers:" + [] {
            std::string names;
            for (int index = 0; index < SDL_GetNumVideoDrivers(); ++index) {
                names += std::string(" ") + SDL_GetVideoDriver(index);
            }
            return names;
        }());
        std::fprintf(stderr, "dualscreen: could not start SDL: %s\n", reason.c_str());
        return 1;
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        log_warn(std::string("no game controller support this session: ") + SDL_GetError());
    }
    {
        const char* driver = SDL_GetCurrentVideoDriver();
        log_info(std::string("SDL video driver: ") + (driver != nullptr ? driver : "none"));
        // Which SDL, because what reaches a controller -- rumble especially,
        // through Steam's virtual pads -- differs between versions, and the
        // one linked is the system's, not the one this was built against.
        SDL_version linked;
        SDL_GetVersion(&linked);
        log_info("SDL " + std::to_string(linked.major) + "." + std::to_string(linked.minor) + "." +
                 std::to_string(linked.patch) + " (built against " +
                 std::to_string(SDL_MAJOR_VERSION) + "." + std::to_string(SDL_MINOR_VERSION) +
                 "." + std::to_string(SDL_PATCHLEVEL) + ")");
        // Said out loud because a touchscreen the system keeps for itself looks
        // exactly like one the app is ignoring: the two-finger way into the menu
        // needs to be told how many fingers are down, and a screen handed over
        // as a mouse can only ever report one.
        log_info("touch devices SDL can see: " + std::to_string(SDL_GetNumTouchDevices()));
        // Said here rather than left to only show up as "cannot decode video"
        // once a companion connects -- that line is about the far end, and
        // this host equally needs a codec built in before it can ever
        // encode a Wii U's GamePad screen as video instead of tiles.
        log_info(std::string("video encode: ") +
                 (video::can_decode() ? "available" : "not built into this binary"));
    }
    // Touch on a desktop should not also arrive as synthetic mouse events, or
    // every tap would register twice.
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // A pad SDL has no entry for, or a wrong entry, is fixable by dropping a
    // gamecontrollerdb.txt next to settings.json - no rebuild needed.
    const std::string controller_db = path_join(impl.paths.config_dir, "gamecontrollerdb.txt");
    if (file_exists(controller_db)) {
        const int added = SDL_GameControllerAddMappingsFromFile(controller_db.c_str());
        if (added < 0) {
            log_warn("could not read " + controller_db + ": " + SDL_GetError());
        } else {
            log_info("loaded " + std::to_string(added) + " controller mappings from " + controller_db);
        }
    }
    log_info("controls are Nintendo positional: east is A, south is B, in menus and in game");
    log_info(std::string("pointer: ") + to_string(impl.settings.cursor_mode));

    // When the game is already known, build the window for its console's core
    // straight away: a 3DS core needs a context an ES 2.0 one cannot stand in
    // for, and starting with the right one avoids rebuilding it a moment later.
    GlContextNeed initial_context = GlContextNeed::Any;
    // Picking a game from the browser starts on whatever context the launcher
    // got and rebuilds the window for the game's console. Booting straight into
    // a game skips that, so this forces the rebuild for tests that need to
    // cover the path a player actually takes.
    const char* force_rebuild = std::getenv("DUALSCREEN_FORCE_CONTEXT_REBUILD");
    const bool skip_initial_context =
        force_rebuild != nullptr && force_rebuild[0] != '\0' && force_rebuild[0] != '0';
    if (!skip_initial_context && !options.game_path.empty()) {
        SystemId booting = SystemId::Ds;
        if (system_for_rom(options.game_path, booting)) {
            initial_context = system_profile(booting).gl_context;
        }
    }

    std::string error;
    if (initial_context != GlContextNeed::Any && !impl.create_window(initial_context, &error)) {
        // Start anyway, on whatever context there is. The game will fail to
        // load and say why on a screen the player can actually read, which
        // beats exiting to a terminal nobody is looking at.
        log_warn("could not build the context this game's console needs: " + error);
        impl.gl_context_need = GlContextNeed::Any;
    }
    if (impl.window == nullptr && !impl.create_window(GlContextNeed::Any, &error)) {
        log_error(error);
        log_error("no usable OpenGL context was available, so there is nothing to draw into");
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        SDL_Quit();
        close_log_file();
        return 1;
    }
    if (!impl.quads.initialize(&error) || !impl.text.initialize(&error) ||
        !impl.button_glyphs.initialize(&error)) {
        log_error("cannot set up the renderer: " + error);
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        impl.destroy_window();
        SDL_Quit();
        close_log_file();
        return 1;
    }

    impl.input.initialize(impl.settings);
    impl.picker.reset(new LayoutPicker(impl.registry));
    impl.apply_system();
    impl.settings_screen.reset(new SettingsScreen(impl.paths));
    impl.active_layout_id = kNewGameLayoutId;
    // A starting guess for the settings screen. Loading a game replaces it with
    // the core for whichever console that game is for.
    impl.resolved_core_path = resolve_core_path(options, impl.settings, SystemId::Ds);

    // --- decide what to boot -------------------------------------------------
    if (!options.auto_connect_screen.empty() && options.game_path.empty()) {
        const ScreenId wanted =
            options.auto_connect_screen == "top" ? ScreenId::Top : ScreenId::Bottom;
        impl.companion_session.reset(new net::CompanionSession());
        std::string search_error;
        if (!impl.companion_session->begin_search(impl.settings.device_name, wanted,
                                                  &search_error)) {
            impl.enter_message(MessageKind::Error, "Cannot look for other devices", search_error,
                               {}, true);
        } else {
            // The same setup start_companion() does for the menu route. This
            // path skipped it, which left --connect with no streamed sound at
            // all and nothing for the host's settings to override.
            impl.attach_companion_callbacks();
            impl.local_screen = wanted;
            impl.bilinear_filtering = impl.settings.default_bilinear_filtering;
            impl.audio_target = impl.settings.audio_target;
            impl.left_handed_layouts = impl.settings.left_handed_layouts;
            impl.screen_gap = impl.settings.default_screen_gap;
            impl.apply_filtering();
            impl.state = Impl::State::Companion;
            impl.awaiting_auto_connect = true;
            impl.input.set_game_input_enabled(true);
        }
    } else if (options.game_path.empty()) {
        // No ROM named: offer to play here or to join a game already running on
        // another device. Failing to start a game chosen this way returns to the
        // launcher rather than ending the session.
        impl.companion_session.reset(new net::CompanionSession());
        std::string search_error;
        if (!impl.companion_session->begin_search(impl.settings.device_name, ScreenId::Bottom,
                                                  &search_error)) {
            // Only discovery is lost; playing locally still works.
            log_warn("cannot look for other devices: " + search_error);
            impl.companion_session.reset();
        }
        impl.open_launcher();
    } else {
        impl.start_game(expand_user_path(options.game_path), true);
    }

    // Test hook: bring a menu up without synthesising input events. The layout
    // menu waits for a frame to exist first -- its previews are drawn from the
    // game, and a menu opened before the core has run shows none of it.
    bool open_layout_menu_when_playing = false;
    // The same for the Save Points screen, whose pictures are of the game,
    // and for the Options list with the cursor on its first row.
    bool open_save_points_when_playing = false;
    bool open_options_when_playing = false;
    if (options.start_screen == "layout" && impl.state == Impl::State::Playing) {
        open_layout_menu_when_playing = true;
    } else if (options.start_screen == "save-points" && impl.state == Impl::State::Playing) {
        open_save_points_when_playing = true;
    } else if (options.start_screen == "options" && impl.state == Impl::State::Playing) {
        open_options_when_playing = true;
    } else if (options.start_screen == "settings" && impl.state == Impl::State::Playing) {
        impl.settings_screen->open(impl.settings, resolve_core_statuses(options, impl.settings));
        impl.state = Impl::State::Settings;
        impl.input.set_game_input_enabled(false);
    } else if (options.start_screen == "browser") {
        impl.open_browser();
    } else if (options.start_screen == "launcher") {
        impl.open_launcher();
    } else if (options.start_screen == "hosts") {
        impl.start_companion(ScreenId::Bottom);
    }

    // --- main loop -----------------------------------------------------------
    const uint64_t counter_frequency = SDL_GetPerformanceFrequency();
    impl.last_counter = SDL_GetPerformanceCounter();
    const uint64_t started_counter = impl.last_counter;
    bool simulate_key_sent = false;

    while (impl.running) {
        const uint64_t now = SDL_GetPerformanceCounter();
        double elapsed = static_cast<double>(now - impl.last_counter) / static_cast<double>(counter_frequency);
        impl.last_counter = now;

        if (elapsed > kSuspendGapSeconds) {
            // Woken from suspend (or a very long stall): start the clock over
            // instead of trying to catch up thousands of frames, and drop the
            // audio that was queued before the machine went to sleep.
            log_info("resumed after a " + std::to_string(elapsed) + "s gap; resetting the frame clock");
            elapsed = impl.frame_period;
            impl.frame_accumulator = 0.0;
            impl.audio.flush();
            impl.refresh_output_size();
            if (impl.source) {
                impl.source->flush_save_ram(false);
            }
        }

        impl.last_delta_seconds = elapsed;
        if (impl.solo_screen_unchecked) {
            impl.update_solo_screen();
        }
        // Read from the setting every frame rather than pushed in when it
        // changes. An "apply this setting" step is a step that can be forgotten,
        // and forgetting it made every pointer mode behave as the default.
        impl.cursor.set_mode(impl.settings.cursor_mode);
        impl.cursor.update(elapsed);
        impl.input.begin_frame();
        impl.tick_identify();

        // Test hook: hold a touch at a fixed spot by pushing the same events a
        // real device would, so everything downstream is the production path.
        if (options.simulate_touch_x >= 0.0f && options.simulate_touch_y >= 0.0f) {
            SDL_Event touch {};
            touch.type = SDL_MOUSEBUTTONDOWN;
            touch.button.button = SDL_BUTTON_LEFT;
            touch.button.state = SDL_PRESSED;
            touch.button.x = static_cast<Sint32>(options.simulate_touch_x * impl.output_width);
            touch.button.y = static_cast<Sint32>(options.simulate_touch_y * impl.output_height);
            impl.input.handle_event(touch);
        }

        // Test hook: tap, let go, tap somewhere else, about eight times a
        // second -- see AppOptions::latency_probe.
        if (options.latency_probe && impl.frames_rendered >= 2) {
            static uint64_t next_ms = 0;
            static bool down = false;
            static bool left = false;
            const uint64_t now = SDL_GetTicks64();
            if (now >= next_ms) {
                next_ms = now + 60;
                down = !down;
                if (down) left = !left;
                SDL_Event touch {};
                touch.type = down ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
                touch.button.button = SDL_BUTTON_LEFT;
                touch.button.state = down ? SDL_PRESSED : SDL_RELEASED;
                touch.button.x = static_cast<Sint32>((left ? 0.3f : 0.7f) * impl.output_width);
                touch.button.y = static_cast<Sint32>(0.5f * impl.output_height);
                impl.input.handle_event(touch);
            }
        }

        // Test hook: a single tap of one key, so the path from a physical
        // keypress to a core's own keyboard callback can be exercised
        // headless. Sent once the core has had a frame to register one, and
        // once whatever this run was asked to reach is actually reached: a key
        // meant for a menu that has not opened yet, or for a session that has
        // not been joined yet, lands somewhere else entirely.
        const bool still_arriving =
            open_layout_menu_when_playing || open_save_points_when_playing ||
            open_options_when_playing ||
            (!options.auto_connect_screen.empty() && !impl.is_companion());
        if (!options.simulate_key.empty() && !simulate_key_sent && impl.frames_rendered >= 2 &&
            !still_arriving) {
            const SDL_Keycode key = SDL_GetKeyFromName(options.simulate_key.c_str());
            if (key != SDLK_UNKNOWN) {
                SDL_Event down {};
                down.type = SDL_KEYDOWN;
                down.key.state = SDL_PRESSED;
                down.key.keysym.sym = key;
                SDL_PushEvent(&down);
                SDL_Event up = down;
                up.type = SDL_KEYUP;
                up.key.state = SDL_RELEASED;
                SDL_PushEvent(&up);
            } else {
                log_warn("--simulate-key does not name a key SDL knows: " + options.simulate_key);
            }
            simulate_key_sent = true;
        }

        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            impl.input.handle_event(event);
            // Forwarded to the core's own on-screen keyboard, if it has one --
            // a 3DS's swkbd, chiefly. This is independent of everything
            // input_manager just did with the same event (a hotkey, a DS
            // button): those still fire, since the two are meant to overlap
            // rather than fight over the key -- a keyboard callback only ever
            // matters while the core's own keyboard is actually up waiting
            // for text, and does nothing the rest of the time.
            if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) && impl.source) {
                const SDL_Keycode key = event.key.keysym.sym;
                const uint16_t modifiers = retro_key_modifiers();
                const bool down = event.type == SDL_KEYDOWN;
                impl.source->send_key_event(down, retro_keycode_for_sdl(key),
                                            down ? retro_character_for_sdl(key, modifiers) : 0,
                                            modifiers);
            }
            // A real mouse brings the pointer back; a finger does not. SDL
            // reports touches as mouse events from SDL_TOUCH_MOUSEID, and a tap
            // on the touchscreen should not put an arrow on the screen.
            if ((event.type == SDL_MOUSEMOTION && event.motion.which != SDL_TOUCH_MOUSEID) ||
                (event.type == SDL_MOUSEBUTTONDOWN && event.button.which != SDL_TOUCH_MOUSEID) ||
                (event.type == SDL_MOUSEWHEEL && event.wheel.which != SDL_TOUCH_MOUSEID)) {
                impl.cursor.notice_mouse_activity();
            }
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    impl.audio.set_paused(true);
                    // Losing focus often precedes being suspended.
                    if (impl.source) {
                        impl.source->flush_save_ram(false);
                    }
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    impl.audio.flush();
                    impl.audio.set_paused(false);
                }
            }
        }
        if (impl.cursor.visible() != impl.cursor_shown) {
            impl.cursor_shown = impl.cursor.visible();
            SDL_ShowCursor(impl.cursor_shown ? SDL_ENABLE : SDL_DISABLE);
        }

        if (impl.input.window_size_changed()) {
            impl.refresh_output_size();
        }

        // Touches are mapped against what this device is actually showing, not
        // against the shared layout. On "one screen per device" the two differ
        // completely: the device holding the bottom screen fills its window
        // with it, while the shared layout still describes a stacked pair, so
        // mapping against the shared one put every touch in the wrong place.
        const Layout touch_layout = impl.device_layout();
        impl.input.end_frame(touch_layout, static_cast<float>(impl.output_width),
                             static_cast<float>(impl.output_height), impl.screens_geometry());

        if (impl.input.quit_requested()) {
            impl.running = false;
        }
        // Both sessions are polled before the UI so a state change this frame
        // is acted on this frame.
        const uint64_t network_now = net::monotonic_millis();
        if (impl.host_session) {
            // Sound only goes over the wire when the other device is one of the
            // ones meant to be making it. Only the Shared stream a DS or 3DS
            // sends is gated here -- a Wii U's TV and GamePad streams check
            // their own targets inside publish_audio itself, since the two
            // can disagree about whether the companion should hear them.
            impl.host_session->set_video_wanted(impl.wants_video());
            impl.host_session->set_audio_enabled(
                audio_reaches(impl.audio_target, impl.host_session->companion_screen()));
            // A menu on either device pauses the game, so the two are always
            // looking at the same frame.
            const bool paused_here = impl.state != Impl::State::Playing;
            net::HostSession::SharedState shared;
            shared.layout_id = impl.active_layout_id;
            shared.per_device_layout = impl.active_layout().per_device;
            shared.paused = paused_here;
            shared.bilinear_filtering = impl.bilinear_filtering;
            shared.audio_target = impl.audio_target;
            shared.handedness = impl.current_handedness();
            shared.left_handed_layouts = impl.left_handed_layouts;
            shared.render_scale = impl.render_scale;
            shared.screen_gap = impl.screen_gap;
            shared.right_stick_mode = impl.right_stick_mode;
            // The menu itself, so the companion draws this one rather than
            // keeping its own. Sent even while closed: that is how the other
            // device learns to put it away again.
            // A companion arriving or leaving changes what the menu offers --
            // Disconnect, and a sound target worth choosing -- so the row is
            // rebuilt rather than left as it was when the menu opened.
            if ((impl.state == Impl::State::Menu || impl.state == Impl::State::Playing) &&
                impl.picker && impl.picker->is_open()) {
                impl.picker->set_session_active(impl.paired());
            }
            shared.menu_open = impl.state == Impl::State::Menu;
            // The Controllers screen is the host's own too, and the other
            // device is told to look over here for it the same way.
            shared.settings_open = impl.state == Impl::State::Settings ||
                                   impl.state == Impl::State::Controllers;
            shared.menu_focus = impl.picker ? impl.picker->focus() : 0;
            shared.menu_confirm =
                impl.picker ? static_cast<uint8_t>(impl.picker->confirmation()) : 0;
            shared.max_render_scale =
                impl.local_game() ? system_profile(impl.system).max_render_scale : 1;
            shared.cursor_choosable = true;  // this end is a machine with a pointer
            shared.cursor_shown = impl.settings.cursor_mode == CursorMode::Shown;
            shared.right_stick_choosable =
                impl.local_game() && system_profile(impl.system).offers_right_stick_mode;
            // The same test open_picker() makes, so the host's menu and the
            // one it tells the companion to build have the same rows.
            shared.left_stick_choosable =
                impl.local_game() && system_profile(impl.system).has_microphone;
            shared.left_stick_press = impl.settings.left_stick_press;
            shared.rumble_choosable = impl.source && impl.source->offers_rumble_pak();
            shared.rumble_pak = impl.source && impl.source->rumble_pak();
            shared.save_points_available = impl.save_points_available();
            impl.host_session->set_session_state(shared);
            impl.host_session->poll(network_now);

            const bool host_now_has_companion = impl.host_session->has_companion();
            // For a Wii U, a companion connecting or changing which screen it
            // wants is what decides which screen exists here at all: Cemu
            // keeps whichever one nobody asked for entirely to itself (its
            // own window shows it, its own audio plays it), so this end only
            // ever has the one screen the companion is actually watching --
            // and never composites it into anything of its own: Cemu's own
            // window is the only place the complementary screen appears now,
            // so there is nothing here for this end to show either, paired
            // or not. No companion means nothing is asked for, which is what
            // makes not running this app alongside Cemu at all a perfectly
            // normal way to just play the game.
            if (impl.system == SystemId::WiiU && impl.external_source) {
                const std::optional<ScreenId> wanted =
                    host_now_has_companion
                        ? std::optional<ScreenId>(impl.host_session->companion_screen())
                        : std::nullopt;
                const bool external_now_running = impl.external_source->running();
                // A companion can decide what is wanted before Cemu has even
                // connected; a request sent into that gap has no peer to
                // reach yet and is silently dropped, so a fresh connection
                // is resent regardless of whether the value itself changed.
                const bool freshly_connected =
                    external_now_running && !impl.external_link_was_running;
                // The size that screen is any use at: the companion's own
                // display. Cemu may be rendering far above it -- a 4K
                // graphics pack against a 1280x800 handheld is sixteen times
                // the pixels -- and every one of those pixels would otherwise
                // be copied through shared memory and scaled down on a CPU
                // here, per frame, only to be thrown away. Cemu already has
                // the frame on a GPU and can reduce it for nothing. Zero
                // until the companion has said (or when none is connected),
                // which asks for whatever size Cemu has, exactly as before.
                int wanted_width = 0;
                int wanted_height = 0;
                if (host_now_has_companion) {
                    impl.host_session->companion_display_size(wanted_width, wanted_height);
                }
                // And which screen Cemu should put in its own window, which is
                // a separate question and one this end has always had an
                // answer to -- it just used to be left implicit, for Cemu to
                // infer as "the one not being captured". That inference has no
                // answer at all when nothing is being captured, which is a
                // device playing on its own: Cemu would show whatever it
                // happened to be showing, and a handheld with no companion
                // would sit there displaying a television it is not.
                //
                // With a companion it is still the other screen. Alone it is
                // whichever this device is being -- see
                // Settings::wii_u_screen_alone.
                const ScreenId shown =
                    wanted.has_value()
                        ? (*wanted == ScreenId::Top ? ScreenId::Bottom : ScreenId::Top)
                        : impl.solo_screen;
                if (freshly_connected || wanted != impl.external_requested_screen ||
                    wanted_width != impl.external_requested_width ||
                    wanted_height != impl.external_requested_height ||
                    impl.external_shown_screen != std::optional<ScreenId>(shown)) {
                    impl.external_source->request_screen(wanted, wanted_width, wanted_height,
                                                         shown);
                    impl.external_requested_screen = wanted;
                    impl.external_requested_width = wanted_width;
                    impl.external_requested_height = wanted_height;
                    impl.external_shown_screen = shown;
                    log_info(wanted.has_value()
                                 ? "a companion wants the " + std::string(to_string(*wanted)) +
                                       " screen at up to " + std::to_string(wanted_width) + "x" +
                                       std::to_string(wanted_height) + "; asking Cemu for it and "
                                       "for the " + to_string(shown) + " screen in its own window"
                                 : "nobody else is connected, so this device is the " +
                                       std::string(to_string(shown)) +
                                       " screen; asking Cemu to show it and to send nothing");
                }
                impl.external_link_was_running = external_now_running;
                // Told to the companion as SharedState.layout_id below, so it
                // knows which single screen the video it is about to receive
                // actually is -- this end never draws it itself (see above),
                // so the only reader of this value is the far end.
                //
                // Alone there is nobody to tell, and the id is this device's
                // own record of which screen it is being -- kept in step for
                // the same reason, so that a companion connecting later does
                // not find a stale one waiting for it.
                const ScreenId here = wanted.value_or(impl.solo_screen);
                const std::string& single_id =
                    (here == ScreenId::Top) ? "tv_only" : "gamepad_only";
                const std::string& resolved_id = impl.registry.resolve(single_id).id;
                if (impl.active_layout_id != resolved_id) {
                    impl.active_layout_id = resolved_id;
                    impl.persist_game_config();
                }
            }

            // The companion can ask for any of the shared settings; the host
            // owns them all, so a layout id it does not recognise is refused
            // rather than adopted.
            net::RequestMessage request;
            if (impl.host_session->take_request(request)) {
                if (!request.layout_id.empty()) {
                    const Layout& resolved = impl.registry.resolve(request.layout_id);
                    if (resolved.id == request.layout_id && resolved.id != impl.active_layout_id) {
                        impl.active_layout_id = resolved.id;
                        impl.persist_game_config();
                        if (impl.picker && impl.picker->is_open()) {
                            impl.picker->set_selected_layout(impl.active_layout_id);
                        }
                        log_info("companion asked for the " + resolved.display_name + " layout");
                    }
                }
                if (request.wants_filtering) {
                    impl.adopt_filtering(request.bilinear_filtering);
                }
                if (request.wants_audio_target) {
                    impl.adopt_audio_target(request.audio_target);
                }
                if (request.wants_handedness) {
                    impl.adopt_handedness(request.handedness);
                }
                if (request.wants_render_scale) {
                    impl.adopt_render_scale(request.render_scale);
                }
                if (request.wants_screen_gap) {
                    impl.adopt_screen_gap(request.screen_gap);
                }
                if (request.wants_right_stick_mode) {
                    impl.adopt_right_stick_mode(request.right_stick_mode);
                }
                if (request.wants_left_stick_press) {
                    impl.adopt_left_stick_press(request.left_stick_press);
                }
                if (request.wants_rumble_pak) {
                    impl.adopt_rumble_pak(request.rumble_pak);
                }
                if (request.wants_reset) {
                    impl.request_reset();
                }
                // The companion asking for the menu, and then working it. Both
                // land on the one picker this device holds, so a press over
                // there and a press here are the same press.


            }
            // Checked separately from the request above, rather than as
            // request.wants_quit there: the companion sends its Bye right
            // behind a quit request, and both are normally handled in this
            // same poll() -- which would clear the pending request before
            // this code next ran, if quit rode along with it instead of its
            // own latch that survives that disconnect.
            // Asked for on the press that asked, not on every request that
            // still happens to carry the flag.
            if (impl.host_session->take_menu_open_request()) {
                impl.set_menu_open(true);
            }
            // Every press the companion made in the menu, in the order it made
            // them: two of them are two moves, not one.
            net::HostSession::MenuPress press;
            while (impl.host_session->take_menu_press(press)) {
                // A press from the other pad answers whichever of the two
                // screens this device is actually showing, and is dropped when
                // it is showing neither: a press aimed at a menu that has since
                // closed is not a press at the game.
                const bool for_menu = impl.state == Impl::State::Menu && impl.picker;
                const bool for_settings = impl.state == Impl::State::Settings ||
                                          impl.state == Impl::State::Controllers;
                if (!for_menu && !for_settings) continue;
                if (press.has_focus && for_menu) {
                    impl.picker->set_focus(press.focus);
                }
                if (!press.has_action) continue;
                const UiAction action = ui_action_for(press.action);
                if (impl.state == Impl::State::Controllers) {
                    impl.update_controllers_screen({action}, {});
                    continue;
                }
                if (for_settings) {
                    // Not a screen the other device draws, but still one it can
                    // work -- see MenuAction::Settings. The presses come over
                    // and get the same answers a press made here would.
                    impl.apply_settings_result(impl.settings_screen->update(
                        {action}, {}, static_cast<float>(impl.output_width),
                        static_cast<float>(impl.output_height)));
                    continue;
                }
                const LayoutPickerResult result =
                    impl.picker->update({action}, {}, static_cast<float>(impl.output_width),
                                        static_cast<float>(impl.output_height));
                impl.apply_menu_result(result, /*pressed_here=*/false);
            }
            if (impl.host_session->take_companion_quit_request()) {
                // The same as choosing Quit here: the whole app shuts down.
                // The companion that asked is leaving on its own right after,
                // not waiting to be told to.
                impl.running = false;
            }
            // Streaming format is an explicit per-console choice with no
            // fallback -- see Settings::screen_codec_for() -- so a format
            // that does not actually work is shown rather than quietly
            // replaced by the other one. Non-fatal: the local game, and
            // whatever this device shows of it, is untouched either way.
            std::string video_error;
            if (impl.host_session->take_video_error(video_error)) {
                impl.enter_message(MessageKind::Error, "Video streaming isn't working",
                                   video_error, {}, false);
            }
            // Whichever screen the companion took, the host shows the other.
            // Moot for a Wii U -- this app never composites either of its
            // screens locally any more, see the request_screen() block above
            // -- but harmless to leave computed the same way for every
            // system rather than special-cased for one that does not read it.
            impl.local_screen = impl.host_session->host_screen();
        }
        if (impl.companion_session) {
            // What was just touched goes to the host before this pass reads
            // and decodes what the host sent, not after: the host is waiting
            // on it and the decoding is not, so every millisecond spent here
            // first is a millisecond later that the tap reaches the game.
            // Every pad on its own too, so the host can make each one any
            // controller its game has. See InputMessage::pads.
            if (impl.state == Impl::State::Companion) {
                const std::vector<net::ReportedPad> pads =
                    net::reported_pads(impl.input.pad_readings());
                impl.companion_session->send_input(impl.input.ds_state(), &pads);
            }
            impl.companion_session->poll(network_now);
            // Told to the host so it knows how much detail a screen sent here
            // is actually worth -- checked every frame rather than only on a
            // resize event, which also covers the first report once a
            // connection exists to send it to.
            const bool companion_now_connected = impl.is_companion();
            if (companion_now_connected && !impl.companion_was_connected) {
                // Freshly connected: a new host has no memory of what we told
                // whatever we were connected to before, even if it happens to
                // be the same number.
                impl.companion_reported_width = 0;
                impl.companion_reported_height = 0;
            }
            // Leaving is not something that only happens while the game is on
            // screen. Checked here rather than in any one screen's own handling
            // so that every screen the session could have left behind is
            // covered by the same line.
            if (!companion_now_connected && impl.companion_was_connected &&
                (impl.state == Impl::State::Companion || impl.state == Impl::State::Menu)) {
                impl.end_companion_session();
            }
            impl.companion_was_connected = companion_now_connected;
            if (companion_now_connected && (impl.output_width != impl.companion_reported_width ||
                                            impl.output_height != impl.companion_reported_height)) {
                impl.companion_session->request_display_size(impl.output_width, impl.output_height);
                impl.companion_reported_width = impl.output_width;
                impl.companion_reported_height = impl.output_height;
            }
            // Outside the is_companion() block below on purpose: this has to
            // keep being told when the session ends, not only while one is up
            // -- and host_wants_motion() goes false the moment it does.
            //
            // But it is not the host's answer alone. This line used to hand
            // host_wants_motion() straight through, and a host has a companion
            // session of its own -- that is how it browses, and a Steam Deck
            // duly finds itself in its own beacon -- so an idle session's flat
            // no was switching off the sensors of the machine running the
            // game, one frame after apply_system() switched them on. See
            // motion_wanted_here(), which is where the three cases now live
            // and where they are tested.
            impl.input.set_motion_enabled(
                motion_wanted_here(impl.is_companion(),
                                   impl.companion_session->host_wants_motion(),
                                   impl.local_game() != nullptr,
                                   system_profile(impl.system).uses_motion));
            // Alongside the session's own stream report, on the same clock, so
            // the two lines can be read together.
            impl.audio.report(network_now);
            // A companion follows the host on everything the two devices share
            // -- layout, smoothing, where the sound comes out. It decides none
            // of it locally, which is why they cannot drift apart. This applies
            // with the menu open too, so the menu shows the host's real state.
            if (impl.is_companion()) {
                const std::string& host_layout = impl.companion_session->host_layout_id();
                if (!host_layout.empty() && host_layout != impl.active_layout_id) {
                    impl.active_layout_id = impl.registry.resolve(host_layout).id;
                    if (impl.picker && impl.picker->is_open()) {
                        impl.picker->set_selected_layout(impl.active_layout_id);
                    }
                }
                // The menu is one of those shared things now: the host holds
                // it, so this device opens and closes when told and redraws
                // whatever the host says it currently looks like.
                const bool host_menu = impl.companion_session->host_menu_open();
                if (host_menu != (impl.state == Impl::State::Menu) &&
                    (impl.state == Impl::State::Menu || impl.state == Impl::State::Companion)) {
                    impl.set_menu_open(host_menu);
                }
                if (impl.state == Impl::State::Menu && impl.picker) {
                    impl.picker->show(
                        impl.menu_state_from_host(), impl.companion_session->host_menu_focus(),
                        static_cast<MenuConfirm>(impl.companion_session->host_menu_confirm()));
                    impl.show_host_save_slots();
                }
                if (impl.companion_session->host_filtering() != impl.bilinear_filtering) {
                    impl.bilinear_filtering = impl.companion_session->host_filtering();
                    impl.apply_filtering();
                    log_info(std::string("following the host: smoothing ") +
                             (impl.bilinear_filtering ? "on" : "off"));
                }
                impl.adopt_audio_target(impl.companion_session->host_audio_target());
                impl.adopt_left_handed_layouts(impl.companion_session->host_left_handed_layouts());
                impl.adopt_screen_gap(impl.companion_session->host_screen_gap());
                impl.adopt_right_stick_mode(impl.companion_session->host_right_stick_mode());
                // L3 does what the host has it doing, on both devices.
                impl.apply_left_stick_press();
                // Whether this machine's own gyro is being read at all. Told
                // rather than worked out from the console, because it is the
                // host that knows whether there is anything at the far end
                // that reads motion -- and cheap to repeat, since
                // set_motion_enabled() with what is already set does nothing.
                if (impl.system != impl.companion_session->system()) {
                    impl.system = impl.companion_session->system();
                    impl.apply_system();
                    log_info(std::string("following the host: ") +
                             system_profile(impl.system).name);
                }
                // After the console, which is what decides whether there is a
                // scale to follow at all.
                impl.adopt_render_scale(impl.companion_session->host_render_scale());
                impl.local_screen = impl.companion_session->granted_screen();
            }
            // --connect joins the first host that offers the screen we asked
            // for, as soon as one appears.
            if (impl.awaiting_auto_connect &&
                impl.companion_session->status() == net::CompanionSession::Status::Searching) {
                for (const net::DiscoveredHost& host : impl.companion_session->hosts()) {
                    if (host.offers(impl.local_screen)) {
                        impl.joining_host = host;
                        impl.companion_session->connect_to(host);
                        impl.awaiting_auto_connect = false;
                        break;
                    }
                }
            }
        }

        // A rendered frame is not an emulated one: without vsync the loop spins
        // far faster than the core runs, so this waits for the core's own
        // output rather than for a swap.
        if (open_layout_menu_when_playing && impl.state == Impl::State::Playing &&
            impl.source && impl.source->core_frame_for_test().valid()) {
            open_layout_menu_when_playing = false;
            impl.set_menu_open(true);
        }
        if (open_save_points_when_playing && impl.state == Impl::State::Playing &&
            impl.source && impl.source->core_frame_for_test().valid()) {
            open_save_points_when_playing = false;
            impl.set_menu_open(true);
            impl.picker->set_focus(LayoutPicker::focus_of(MenuSection::CreateSavePoint));
        }
        if (open_options_when_playing && impl.state == Impl::State::Playing &&
            impl.source && impl.source->core_frame_for_test().valid()) {
            open_options_when_playing = false;
            impl.set_menu_open(true);
            impl.picker->set_focus(LayoutPicker::focus_of(MenuSection::Options, 0));
        }

        impl.handle_ui(impl.input.ui_actions(), impl.input.pointer_events());

        // Emulation only advances while the game is actually in front; opening
        // the layout menu pauses it, which is what makes comparing layouts easy.
        // A menu on the *other* device counts just as much: otherwise one player
        // reads a menu while the game carries on without them.
        // Opening the menu pauses the game, wherever it was opened from: the
        // host holds the one menu, so its own state already says so.
        const bool game_running = impl.state == Impl::State::Playing;
        // Whichever kind of game this device is playing, if it is playing one.
        // Everything below that only needs a frame out of it asks through here;
        // what is still spelled `impl.source` is genuinely a core's own -- save
        // RAM, an input sink -- and a console played elsewhere has none of it.
        ScreenSource* const playing = impl.local_game();
        if (impl.external_source && impl.external_source->running() &&
            !impl.external_game_arrived) {
            impl.external_game_arrived = true;
            impl.adopt_external_timing();
        }
        // A program this app started that stopped without ever connecting.
        // Otherwise the shell waits for ever on something that is not coming,
        // and the reason -- a bad path, a flag the fork does not know -- is in
        // a terminal nobody in Game Mode can see.
        if (impl.external_program_wanted && !impl.external_game_arrived &&
            !impl.external_program.running()) {
            impl.external_program_wanted = false;
            const SystemProfile& profile = system_profile(impl.system);
            impl.enter_message(MessageKind::Error, profile.core_description + " did not start",
                               "It stopped without handing over a game.",
                               {"Command: " + impl.settings.external_program_for(impl.system),
                                "Socket: " + impl.external_socket_path,
                                "Check \"wii_u_program\" and \"wii_u_arguments\" in " +
                                    impl.paths.settings_file + "."},
                               true);
        }
        if (playing != nullptr && game_running) {
            // Whose controls the console's pad takes. On a DS or a 3DS, both
            // devices', merged, so either player can press anything -- the
            // InputSink seam doing exactly what it was left in place for. On a
            // Wii U, each pad on either device is either the GamePad or one of
            // eight players, and Cemu is handed each of those separately:
            // merging every pad into the GamePad made every player press the
            // GamePad's buttons too. See pad_belongs_to_its_screen and
            // pad_routing.hpp.
            const bool companion_here =
                impl.host_session && impl.host_session->has_companion();
            const DsInputState remote =
                companion_here ? impl.host_session->companion_input() : DsInputState();
            DsInputState combined;
            if (system_profile(impl.system).pad_belongs_to_its_screen && impl.external_source) {
                combined = impl.route_wii_u_pads(impl.input.ds_state(), remote, companion_here);
            } else {
                const PadOwner owner = console_pad_owner(
                    system_profile(impl.system).pad_belongs_to_its_screen, companion_here,
                    companion_here && impl.host_session->companion_screen() == ScreenId::Bottom);
                if (!impl.said_pad_owner || owner != impl.last_pad_owner) {
                    impl.said_pad_owner = true;
                    impl.last_pad_owner = owner;
                    log_info(std::string("the console's pad takes its controls from ") +
                             pad_owner_name(owner) +
                             (owner == PadOwner::Companion
                                  ? ", which is showing its screen; this device's controllers "
                                    "are left to the game's other players"
                                  : ""));
                }
                combined = input_for_the_console_pad(impl.input.ds_state(), remote, owner);
            }
            // While both devices report motion, hold the two up against each
            // other. This is the host, so both readings are in hand at the
            // same instant with nothing to line up afterwards -- which is what
            // makes a device that is already right usable as an instrument for
            // one that is not. See MotionComparison.
            if (companion_here && impl.input.motion().present && remote.motion.present) {
                impl.motion_comparison.add(impl.input.motion(), remote.motion);
                impl.motion_comparison.report(SDL_GetTicks());
            }
            if (impl.source) {
                impl.source->input_sink().submit(combined);
            } else if (impl.external_source) {
                // Said once, the first time anything is actually pressed.
                //
                // "The controls do not work" has two halves and they are not
                // distinguishable from the outside: either nothing reaches
                // this app, or it reaches this app and stops somewhere in
                // Cemu. This line separates them. Its absence, with a game
                // running and buttons being pressed, means this app is
                // reading a pad that never moves -- which is what SDL hands
                // back while another program's window is in front, and why
                // apply_controller_hints() exists.
                if (!impl.said_first_pad && (combined.buttons != 0 || combined.touching)) {
                    impl.said_first_pad = true;
                    log_info("the first pad press reached the game beside this app; controls are "
                             "getting through");
                }
                // And the same for motion, separately, because the two travel
                // together and fail apart: buttons working while aiming does
                // not is exactly the case this distinguishes. If this line is
                // in the log, how the device is being held crossed the link
                // and anything still wrong is on the far side of it.
                if (!impl.said_first_motion && combined.motion.present) {
                    impl.said_first_motion = true;
                    log_info("how this device is being held is reaching the game beside it too");
                }
                impl.external_source->submit_input(combined);
            }

            impl.frame_accumulator += elapsed;
            // Cap catch-up so a hitch cannot spiral into a long fast-forward.
            const int max_frames_per_iteration = 4;
            int advanced = 0;
            while (impl.frame_accumulator >= impl.frame_period && advanced < max_frames_per_iteration) {
                std::string frame_error;
                if (!playing->advance_frame(&frame_error)) {
                    // A game that has finished is not a game that failed: a
                    // console played by another program ends its session by
                    // that program closing, which is an ordinary way to stop
                    // and is handled by wants_shutdown below.
                    if (!playing->wants_shutdown()) {
                        impl.enter_message(MessageKind::Error, "The core stopped running",
                                           frame_error, {}, true);
                    }
                    break;
                }
                impl.frame_accumulator -= impl.frame_period;
                ++advanced;
            }
            if (impl.frame_accumulator > impl.frame_period * max_frames_per_iteration) {
                impl.frame_accumulator = 0.0;
            }
            // What the companion's input has now reached, and a quick tap's
            // release let go of -- see HostSession::note_frame_ran().
            if (advanced > 0 && impl.host_session && impl.host_session->has_companion()) {
                impl.host_session->note_frame_ran(playing->frame_serial());
            }
            if (impl.source) {
                impl.save_flush_accumulator += elapsed;
                if (impl.save_flush_accumulator >= kSaveFlushIntervalSeconds) {
                    impl.save_flush_accumulator = 0.0;
                    impl.source->flush_save_ram(false);
                }
            }

            if (playing->wants_shutdown()) {
                impl.running = false;
            }
        }

        // Hand the current screens to the companion, if any. The serial only
        // advances when the pixels actually changed, so an unchanged frame costs
        // nothing on the wire -- which is why this can sit outside the emulation
        // step. It has to: a paused game still owes the other device a picture
        // when what it needs changes, and while paused no new frame is coming.
        if (playing != nullptr && impl.host_session && impl.host_session->has_companion()) {
            // Asked for per screen rather than taken from the frame directly:
            // a core that renders on the GPU has no CPU copy of its picture,
            // and reaching into one is how a 3DS session used to send an empty
            // top screen and then read off the end of nothing for the bottom.
            // Only the screens the companion actually needs are asked for,
            // because on that path each one is a copy back off the card.
            const uint64_t serial = playing->frame_serial();
            ScreenReadback top;
            ScreenReadback bottom;
            // Sent at the console's own resolution regardless of the render
            // scale when carried as changed tiles: a general-purpose CPU codec
            // was never going to keep pace with several times the pixels every
            // frame, and a choppy stream at a sharper resolution is a worse
            // trade than a smooth one at a softer one. H.264 has no such
            // trouble -- that is the whole reason it exists on this link --
            // so a screen sent that way is asked for at everything the core
            // actually rendered instead. read_screen() itself still clamps to
            // that: this is a ceiling, not a demand, for a core that is not
            // rendering above native to begin with.
            const bool full_resolution = impl.wants_video();
            const SystemScreens& native = system_profile(impl.system).screens;
            if (impl.host_session->companion_wants(ScreenId::Top)) {
                if (full_resolution) {
                    playing->read_screen(ScreenId::Top, top, native.top.width * impl.render_scale,
                                         native.top.height * impl.render_scale);
                } else {
                    playing->read_screen(ScreenId::Top, top);
                }
            }
            if (impl.host_session->companion_wants(ScreenId::Bottom)) {
                if (full_resolution) {
                    playing->read_screen(ScreenId::Bottom, bottom,
                                         native.bottom.width * impl.render_scale,
                                         native.bottom.height * impl.render_scale);
                } else {
                    playing->read_screen(ScreenId::Bottom, bottom);
                }
            }
            if (top.valid() || bottom.valid()) {
                const auto wire_order = [](ChannelOrder order) {
                    return order == ChannelOrder::Rgba ? net::PixelChannelOrder::Rgba
                                                       : net::PixelChannelOrder::XrgbBytes;
                };
                net::HostSession::ScreenPixels top_pixels;
                top_pixels.pixels = top.pixels;
                top_pixels.width = top.width;
                top_pixels.height = top.height;
                top_pixels.stride_pixels = top.stride_pixels;
                top_pixels.channel_order = wire_order(top.channel_order);
                net::HostSession::ScreenPixels bottom_pixels;
                bottom_pixels.pixels = bottom.pixels;
                bottom_pixels.width = bottom.width;
                bottom_pixels.height = bottom.height;
                bottom_pixels.stride_pixels = bottom.stride_pixels;
                bottom_pixels.channel_order = wire_order(bottom.channel_order);
                impl.host_session->publish_screens(top_pixels, bottom_pixels, serial);
            }
        }

        if (playing != nullptr && !game_running) {
            // Paused: hold the sticks and buttons off the game entirely. Not
            // only so nothing moves while the menu is up -- a button held as
            // the menu opened would otherwise stay held for as long as it is.
            if (impl.source) {
                impl.source->input_sink().submit(DsInputState());
            } else if (impl.external_source) {
                impl.external_source->submit_input(DsInputState());
                // The players too, still plugged in -- a pause is not a pad
                // being taken away.
                const RoutedPads routed = route_pads(impl.known_pads, impl.pad_slots);
                for (int player = 0; player < kPadPlayers; ++player) {
                    impl.external_source->submit_player_input(player, routed.player_fed[player],
                                                              DsInputState());
                }
            }
        }
        if (playing != nullptr && !game_running && impl.game_was_running) {
            // The tail of a game that has just stopped, which would otherwise
            // keep playing over a menu the player has already opened. Dropped
            // on the way in only: there is nothing stale to drop on the way out.
            impl.audio.flush();
        }
        impl.game_was_running = game_running;

        impl.render();

        if (!impl.settings.vsync) {
            // Without vsync, hand the CPU back rather than spinning.
            SDL_Delay(1);
        }
        if (options.exit_after_frames > 0 && impl.frames_rendered >= options.exit_after_frames) {
            impl.running = false;
        }
        if (options.screenshot_at_seconds > 0.0 && !impl.screenshot_taken &&
            !options.screenshot_path.empty()) {
            const double alive =
                static_cast<double>(SDL_GetPerformanceCounter() - started_counter) /
                static_cast<double>(counter_frequency);
            if (alive >= options.screenshot_at_seconds) {
                impl.render();  // so the buffer read back is the one just drawn
                impl.screenshot_taken = impl.write_screenshot(options.screenshot_path);
            }
        }
        if (options.exit_after_seconds > 0.0) {
            const double alive =
                static_cast<double>(SDL_GetPerformanceCounter() - started_counter) /
                static_cast<double>(counter_frequency);
            if (alive >= options.exit_after_seconds) {
                impl.running = false;
            }
        }
    }

    if (!options.screenshot_path.empty() && !impl.screenshot_taken) {
        // Re-render once so the buffer being read is the one just drawn.
        impl.render();
        if (impl.write_screenshot(options.screenshot_path)) {
            log_info("wrote " + options.screenshot_path);
        } else {
            log_warn("could not write " + options.screenshot_path);
        }
    }

    // What is worth keeping goes first, before any step below that waits on
    // something else: this game's layout, and a core's save -- which is
    // written when the core closes. See ShutdownWatchdog for why the order
    // matters.
    impl.persist_game_config();
    if (impl.source) {
        impl.source->close();
        impl.source.reset();
    }

    // Everything after this is a call into something that may not answer.
    // Each step is named in the log as it starts, and if they have not all
    // finished within kShutdownDeadline the process ends without them.
    ShutdownWatchdog watchdog(kShutdownDeadline);
    if (impl.host_session) {
        watchdog.at("saying goodbye to the other device");
        impl.host_session->stop();
        impl.host_session.reset();
    }
    if (impl.companion_session) {
        watchdog.at("leaving the other device's session");
        impl.companion_session->disconnect();
        impl.companion_session.reset();
    }
    watchdog.at("closing the audio device");
    impl.audio.close();
    // Before the window goes away rather than with everything else at the end:
    // the emulator has a window of its own, and the tidy order to close two is
    // the one the player would have chosen.
    if (impl.external_program.running()) {
        watchdog.at("stopping the emulator this app started");
    }
    impl.external_program.stop();
    impl.external_source.reset();
    watchdog.at("letting go of the controllers");
    impl.input.shutdown();
    watchdog.at("closing the window");
    impl.destroy_window();
    SDL_Quit();
    watchdog.finish();
    log_info("dualscreen exiting normally");
    close_log_file();
    return 0;
}

}  // namespace ds
