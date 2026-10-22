#include "app.hpp"

#include <SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <tuple>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <ctime>
#include <fstream>
#include <map>
#include <optional>
#include <utility>

#include "audio/audio_output.hpp"
#include "audio/ui_sounds.hpp"
#include "input/motion.hpp"
#include "input/motion_compare.hpp"
#include "config/game_config.hpp"
#include "config/melonds_tuning.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "config/sha1.hpp"
#include "core/amiibo_dump.hpp"
#include "core/external_screen_source.hpp"
#include "core/gl_call_trace.hpp"
#include "core/libretro_core.hpp"
#include "core/libretro_screen_source.hpp"
#include "core/console_title_info.hpp"
#include "core/game_title.hpp"
#include "core/render_scale.hpp"
#include "core/save_store.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "input/pad_routing.hpp"
#include "input/retro_keymap.hpp"
#include "input/stylus_cursor.hpp"
#include "layout/layout_registry.hpp"
#include "render/bezel_art.hpp"
#include "render/button_glyphs.hpp"
#include "render/compositor.hpp"
#include "render/frame_delay.hpp"
#include "render/menu_backdrop.hpp"
#include "render/menu_overlay.hpp"
#include "ui/accent.hpp"
#include "ui/system_keyboard.hpp"
#include "ui/menu_actions.hpp"
#include "render/gl_loader.hpp"
#include "render/gl_window.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/layout_picker.hpp"
#include "ui/menu_state.hpp"
#include "ui/notice_popup.hpp"
#include "ui/corner_notice.hpp"
#include "ui/game_dialog.hpp"
#include "ui/mirrored_game_dialog.hpp"
#include "ui/menu_cursor_sync.hpp"
#include "ui/menu_scroll_sync.hpp"
#include "ui/stylus_cursor_view.hpp"
#include "ui/menu_entrance.hpp"
#include "net/companion_session.hpp"
#include "net/host_session.hpp"
#include "net/save_exchange.hpp"
#include "ui/connecting_screen.hpp"
#include "ui/controllers_screen.hpp"
#include "ui/button_mapping_screen.hpp"
#include "ui/graphic_packs_screen.hpp"
#include "ui/wii_u_player_screen.hpp"
#include "ui/data_management_screen.hpp"
#include "ui/forwarder_screen.hpp"
#include "ui/game_browser.hpp"
#include "ui/launcher_screen.hpp"
#include "ui/list_touch.hpp"
#include "ui/message_screen.hpp"
#include "ui/settings_screen.hpp"
#include "ui/update_prompt.hpp"
#include "update/desktop_updater.hpp"
#include "update/updater.hpp"
#include "util/child_process.hpp"
#include "util/build_stamp.hpp"
#include "util/log.hpp"
#include "util/shutdown_watchdog.hpp"

namespace ds {
namespace {

// Forwarder Creation on a machine that cannot make forwarders, for looking at
// it: --start-screen forwarder-preview. No keyboard, no network and no
// install, each of which says so.
class PreviewForwarderPlatform : public ForwarderPlatform {
public:
    bool edit_text(const std::string&, const std::string&, const std::string&, std::size_t,
                   std::string&) override {
        return false;
    }
    HttpResponse http_get(const std::string&, const std::string&) override {
        HttpResponse response;
        response.error = "the preview has no network";
        return response;
    }
    bool install(const ForwarderRequest&, std::string* error) override {
        if (error != nullptr) *error = "forwarders are installed on a Switch";
        return false;
    }
};

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
// How often a game played on this device says how fast it is really running.
constexpr double kSpeedReportSeconds = 10.0;

// How often, while a game with Resume from Save State on is being played, its
// resume point is brought up to date even when nothing else has asked. Quitting,
// the quick menu, losing the focus and every in-game save each write one too;
// this is for what gives no warning at all -- a battery running out, a crash --
// and says how far back that can land.
constexpr double kResumePointIntervalSeconds = 30.0;

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
    // A build with its core linked in has that one core and no way to load
    // another: its own console gets it, and every other console has none.
    if (const std::optional<SystemId> built_in = builtin_core_system()) {
        return *built_in == system ? std::string(kBuiltInCorePath) : std::string();
    }
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

// Whether this device can play a console's games itself. A Switch and an
// Android device play DS games only -- melonDS is built in, or in the APK --
// so nothing about another console's emulator is offered there.
bool device_plays(SystemId system) {
#if defined(__SWITCH__) || defined(__ANDROID__)
    return system == SystemId::Ds;
#else
    (void)system;
    return true;
#endif
}

// One entry per console this device plays, for the Settings screen, so a
// machine set up for one and not another says so before a game is picked
// rather than after.
std::vector<SettingsScreen::CoreStatus> resolve_core_statuses(const AppOptions& options,
                                                              const Settings& settings) {
    std::vector<SettingsScreen::CoreStatus> statuses;
    for (const SystemProfile& profile : system_profiles()) {
        if (!device_plays(profile.id)) continue;
        // Where this console is played from, whichever kind of thing that is.
        // What the player needs to know is the same either way: is it there.
        switch (profile.frame_origin) {
            case FrameOrigin::LibretroCore: {
                const std::string path = resolve_core_path(options, settings, profile.id);
                // A core linked in is the Switch's own melonDS, whichever.
                statuses.push_back({profile.name, path, profile.id,
                                    path == kBuiltInCorePath ? std::string("melonDS") : profile.core_description});
                break;
            }
            case FrameOrigin::External:
                statuses.push_back(
                    {profile.name, settings.external_program_for(profile.id), profile.id, profile.core_description});
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
        "  --video-codec <how>  Send screens to a second device only as tiles or\n"
        "                       only as h264 this run, instead of choosing per\n"
        "                       screen by its size and how busy it is\n"
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
        Companion,
        // The game has just been started from its resume point and is held
        // there, paused, under a popup saying so, until OK is pressed -- on
        // either device. See resume_game().
        ResumeNotice,
        // Settings > Data Management.
        DataManagement,
        // A ROM's forwarder, from the file picker.
        Forwarder,
        // A Wii U game's graphics packs, from its quick menu.
        GraphicPacks,
        // An amiibo to scan, from the same menu.
        Amiibo,
        // A DS or 3DS game's Button Mapping, from its menu's Controls.
        ButtonMapping,
        // A Wii U game's player, or its GamePad, from its menu's Controls.
        WiiUPlayer,
        // A Wii U game's own dialog -- an error, its keyboard -- shown over
        // it in place of Cemu's, the game going on underneath. See
        // update_game_dialogs().
        GameDialog
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
    // The quick menu as Cemu draws it over a Wii U game -- see
    // wants_menu_overlay() -- and whether Cemu is showing one now.
    MenuOverlay menu_overlay;
    // A Wii U game paused for the quick menu, as the console pauses for its
    // HOME Menu -- see set_menu_open(). Cemu answers when the game has
    // stopped, so the menu waits for that before it opens.
    bool wii_u_paused = false;
    bool menu_waiting_for_pause = false;
    // Both screens as they were when the game paused, which Cemu sends then:
    // the paused game behind the menu, blurred, as a DS or 3DS game is --
    // whichever of the two Cemu's window is showing, so Swap Screens swaps
    // the picture too. Indexed Top 0, Bottom 1; each ready once a picture
    // newer than its `wii_u_still_after` has arrived.
    uint64_t wii_u_still_after[2] = {0, 0};
    bool wii_u_still_ready[2] = {false, false};
    std::chrono::steady_clock::time_point wii_u_paused_at;
    std::chrono::steady_clock::time_point menu_pause_asked_at;
    // The pointer over the menu drawn in Cemu's window -- see
    // ExternalControl::OverlayPointer: down or not, where it is, and where it
    // went down, in this window's pixels.
    bool overlay_pointer_down = false;
    float overlay_pointer_x = 0.0f;
    float overlay_pointer_y = 0.0f;
    float overlay_press_x = 0.0f;
    float overlay_press_y = 0.0f;
    // A few words in the top right corner, until `corner_notice_until`.
    std::string corner_notice;
    std::chrono::steady_clock::time_point corner_notice_until;
    // The game's icon for the quick menu's header: this device's game's, read
    // from its save store as Data Management shows it, or on a companion the
    // one the host sent. `game_icon_for` says whose it is.
    Texture game_icon;
    std::string game_icon_for;
    // The file the game being played was started from -- a DS or 3DS ROM, or
    // a Wii U game's -- which the icon is read out of.
    std::string game_icon_rom;
    int game_icon_width = 0;
    int game_icon_height = 0;
    std::vector<uint8_t> game_icon_rgba;
    uint64_t game_icon_tried_us = 0;
    // Loads it when it is not loaded yet for what is being played.
    void refresh_game_icon();
    bool menu_overlay_shown = false;
    // Whether what is up in Cemu's window is the menu, which takes its input,
    // or only a notice, which leaves it to the game.
    bool menu_overlay_takes_input = true;
    bool menu_overlay_logged = false;
    // The menu's coming or going last handed to Cemu to animate, by when it
    // began -- see render_menu_overlay().
    MenuEntrance::Clock::time_point overlay_animation_sent{};
    // A companion's pass started as late before the display's refresh as it
    // can be -- see FrameDelay. Never the host's: the host's pass runs the
    // game, whose timing is the game's.
    FrameDelay frame_delay;
    bool frame_delay_said = false;
    uint64_t pass_began_us = 0;
    // Where a game-playing pass's time went, for the one line said when a pass
    // runs long -- see report_slow_pass(). Microseconds this pass.
    struct PassTimes {
        uint64_t network_us = 0;
        uint64_t core_us = 0;
        int frames = 0;
        uint64_t stream_us = 0;
        uint64_t draw_us = 0;
    } pass_times;
    uint64_t slow_pass_said_us = 0;
    int slow_passes_unsaid = 0;
    void report_slow_pass(uint64_t pass_us);
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
    // What it was started with, so Reset can start it again: see
    // restart_external_game().
    std::string external_program_path;
    std::vector<std::string> external_program_arguments;
    std::unique_ptr<LayoutPicker> picker;
    std::unique_ptr<SettingsScreen> settings_screen;
    // Updating Dualscreen, and the Cemu it keeps, from the nightly builds --
    // on a PC or a Steam Deck. Null everywhere else: a Switch's and an
    // Android device's are their companion menu's. See update/updater.hpp.
    std::unique_ptr<update::Updater> updater;
    UpdatePrompt update_prompt;
    void start_updater(const AppOptions& options);
    void poll_updates();
    // Where the core Dualscreen keeps for `system` is, or would be.
    std::string managed_core_path(SystemId system) const;
    std::unique_ptr<DataManagementScreen> data_screen;
    // What the popup over a paused game says: that it was resumed, or that
    // Data Management restarted it on another save.
    std::string game_notice = kResumedNotice;
    // Opened from Settings, and drawn over it -- see ControllersScreen.
    ControllersScreen controllers_screen;
    GraphicPacksScreen graphic_packs_screen;
    ButtonMappingScreen button_mapping_screen;
    WiiUPlayerScreen wii_u_player_screen;
    // Detect Controller's: each controller's buttons as of the last frame, by
    // pad_identity(), so only a press made since counts.
    std::map<std::string, uint32_t> detect_pressing;
    // Cemu's own options, as it last said: shown in a Wii U menu's Options,
    // and the ones it keeps for every game alike set to this game's choice
    // once each session -- see ExternalOption::default_choice.
    std::vector<ExternalOption> cemu_options;
    bool cemu_options_asked = false;
    bool cemu_options_applied = false;
    // The amiibo in the amiibo folder, listed in the menu's Scan amiibo.
    std::vector<std::string> amiibo_paths;
    GameBrowser amiibo_browser;
    // An amiibo chosen in the menu, held to the GamePad once the game is
    // going again: a Wii U game only sees a figure for the moment it is
    // there, which a paused one would miss.
    std::string pending_amiibo;
    // Its name, for the notice that says how the scan went.
    std::string pending_amiibo_name;
    std::chrono::steady_clock::time_point pending_amiibo_at;
    // From the amiibo being chosen until Cemu says how its scan went: the
    // corner says "Scanning..." meanwhile, over the game -- see
    // shown_corner_notice(). Given up on a while after it is sent, should
    // Cemu never answer.
    bool amiibo_scanning = false;
    std::chrono::steady_clock::time_point amiibo_sent_at;
    // The host's, drawn here while this device is its companion -- see
    // render_mirrored_controllers().
    ControllersScreen mirrored_controllers;
    // And its Graphics Packs page, the same way.
    GraphicPacksScreen mirrored_graphic_packs;
    // And the game's Button Mapping.
    ButtonMappingScreen mirrored_button_mapping;
    WiiUPlayerScreen mirrored_wii_u_player;
    bool wii_u_player_view_sent = false;
    net::WiiUPlayerViewMessage wii_u_player_view_said;
    bool button_mapping_view_sent = false;
    net::ButtonMappingViewMessage button_mapping_view_said;
    // What the companion was last told of the Graphics Packs page -- see
    // GraphicPacksScreen::packs_revision().
    bool graphic_packs_view_sent = false;
    std::array<int64_t, 7> graphic_packs_view_key{};
    // The game running here, which its save points are named after.
    std::string game_path;
    std::unique_ptr<GameBrowser> browser;
    std::unique_ptr<ForwarderScreen> forwarder_screen;
    std::unique_ptr<ForwarderPlatform> preview_forwarders;
    ForwarderPlatform* forwarder_platform() const {
        return options.forwarder_platform != nullptr ? options.forwarder_platform
                                                     : preview_forwarders.get();
    }
    void open_forwarder(const std::string& rom_path);
    std::unique_ptr<LauncherScreen> launcher;
    std::unique_ptr<HostPickerScreen> host_picker;
    MessageScreen message;

    // Two-device mode. Exactly one of these is live at a time: a device either
    // runs the game and serves a screen, or joins someone else's game.
    std::unique_ptr<net::HostSession> host_session;
    std::unique_ptr<net::CompanionSession> companion_session;
    ScreenId local_screen = ScreenId::Top;
    // Which screen Separate Screens shows on this device alone, and which it
    // holds with a second device connected -- see GameConfig::separate_screen.
    ScreenId separate_screen = ScreenId::Top;
    // Swaps it: here, or between the two devices, or -- on a companion --
    // by asking the host to.
    void swap_separate_screens();
    // The screen this device shows in a layout that gives each device one.
    ScreenId own_separate_screen() const;
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
    // Swap Screens, from the menu or F3: the other screen from the setting's,
    // for this game, until swapped back -- see GameConfig::screens_swapped.
    void toggle_solo_screen_swap();
    // Set when a game starts; the loop does the check and clears it, which it
    // can only do once there is a window to ask about -- a game started from
    // the command line starts before there is one.
    bool solo_screen_unchecked = true;
    // Where such a console hands its screens over, for the waiting screen to
    // name while nothing has.
    std::string external_socket_path;
    // The game that program is playing, for its save -- see
    // note_external_game_closed().
    std::string external_save_rom;
    // Where the settings screen was opened from, so closing it goes back
    // there. It used to be reachable only before a game was running, when the
    // answer could not be anything but the main menu.
    State settings_came_from = State::Launcher;
    // Data Management was opened from its row in Settings, and goes back
    // there when it is left.
    bool data_from_settings = false;
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
    // A menu or page over the game: every motor the game has on is stopped
    // until the game has the pads back, when they start again as it left
    // them -- see apply_pad_rumble().
    bool rumble_quiet = false;
    // The pads whose motors this app has turned on, by pad_identity(), so that one
    // leaving a rumbling slot -- or the slot going quiet -- is turned off.
    std::map<std::string, KnownPad> rumbling_pads;
    // An older companion buzzing as a whole so the player can tell which it
    // is, and until when -- see identify_pad(). A newer one ends its own.
    std::optional<KnownPad> identifying_pad;
    uint32_t identify_until_ms = 0;
    // The controller in hand in Controls, by pad_identity().
    std::string moving_pad;
    // Where the Controllers screen was opened from, which is where closing it
    // goes back to.
    State controllers_came_from = State::Playing;
    // The same for the pages that are Cemu's -- Graphics Packs, Options, an
    // amiibo: the quick menu over the game, or the list in this window while
    // the game plays in Cemu's.
    State cemu_page_came_from = State::Playing;
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
    // Quit was chosen from the companion's side of the menu, so the companion
    // is told to close its app too when this one says goodbye -- see
    // HostSession::stop().
    bool quit_from_companion = false;
    // The player quit -- Quit in the quick menu, from either device -- or the
    // system closed the app, rather than leaving for a menu. See
    // App::quit_chosen().
    bool quit_chosen = false;
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
    // How many pixels the core renders per console pixel, as chosen -- or
    // kAutoRenderScale. Only the host runs a core, but the menu is one menu
    // wherever it is opened from, so this is shared like everything else on it.
    int render_scale = 1;
    // What the core is drawing at: the choice, or Auto's -- see
    // update_auto_render_scale().
    int effective_render_scale = 1;
    // Whether the current layout shows the gap between its screens. Shared
    // like the settings above, though it is only ever asked about while the
    // active layout offers the choice -- see LayoutComposition::gap_toggleable.
    bool screen_gap = true;
    // What the right stick is read as, on a console that has one of its own.
    RightStickMode right_stick_mode = RightStickMode::CStick;
    // What each of the pad's buttons does in this game -- see ButtonMapping.
    // The host's, kept with the game, and followed by a companion's pad.
    ButtonMapping button_mapping;
    // The stylus on the right stick, while it is one: moved by both devices'
    // sticks, as the game's touch, and drawn by whichever shows the touch
    // screen. Kept here, the host, and told to a companion.
    StylusCursor stylus_cursor;
    uint32_t stylus_ticks = 0;
    // When a Touch Point's spot was last steered by the pad -- see
    // steer_touch_spot().
    uint32_t spot_steer_ticks = 0;
    std::string game_title;
    std::string resolved_core_path;
    // Which console the loaded game belongs to, and its screens. On a companion
    // both come from the host; on a device running a game, from the ROM.
    SystemId system = SystemId::Ds;

    // The screens whatever is being drawn actually has. Every layout, touch and
    // preview goes through this rather than assuming a DS's.
    SystemScreens screens_geometry() const;

    void report_speed();
    // Starts the next window afresh -- after a report, and whenever the game
    // stops being played (a menu, a message), so paused time is not counted
    // as a slow game.
    void reset_speed_window() {
        speed_window_start = 0;
        speed_seconds = 0.0;
        speed_core_seconds = 0.0;
        speed_frames = 0;
        speed_passes = 0;
        speed_slowest_frame = 0.0;
        speed_long_frames = 0;
        speed_catch_ups = 0;
        speed_frame_gaps = 0;
        speed_queue_waits = 0;
        speed_swap_waits = 0;
        speed_draw_us = 0;
        speed_slowest_draw_us = 0;
        speed_draws = 0;
    }

    uint64_t last_counter = 0;
    double frame_period = 1.0 / 60.0;
    double frame_accumulator = 0.0;
    // How fast a game played here is really running, said every
    // kSpeedReportSeconds: frames the core ran against the frames that time
    // was worth, and how much of it went to the core rather than to this app.
    // A device that cannot keep up shows it here first, and which side of that
    // line the time went is the whole question when it does.
    uint64_t speed_window_start = 0;  // a performance counter; 0 between windows
    double speed_seconds = 0.0;
    // Whether the core has made a frame since this window last drew one. See
    // draw_this_pass in the main loop.
    bool core_frame_undrawn = true;
    double speed_core_seconds = 0.0;
    int speed_frames = 0;
    int speed_passes = 0;
    // What an average over ten seconds hides: a second of slow frames, or one
    // frame long enough to be seen as a hitch, barely moves it. The slowest
    // frame the core ran, how many took longer than a frame lasts, and how
    // many passes had to run more than one frame to catch up -- each of those
    // a picture the player never saw.
    double speed_slowest_frame = 0.0;
    int speed_long_frames = 0;
    int speed_catch_ups = 0;
    // Frames that reached the window further apart than a refresh and a half:
    // a picture held on screen twice as long as it should be, which is what a
    // hitch looks like whatever its cause. The core's frames only.
    int speed_frame_gaps = 0;
    uint64_t last_frame_presented_us = 0;
    // Locked to the display (see display_locked in the main loop): refreshes
    // given to a frame already waiting, and frames that found the queue full.
    int speed_queue_waits = 0;
    int speed_swap_waits = 0;
    // Drawing and presenting each of the core's frames: total and slowest.
    uint64_t speed_draw_us = 0;
    uint64_t speed_slowest_draw_us = 0;
    int speed_draws = 0;
    // The display's refresh rate, where the system says; 0 where it does not.
    double display_refresh_hz = 0.0;
    double display_refresh_said = -1.0;
    SDL_Window* display_refresh_window = nullptr;
    // Reads display_refresh_hz from the system, once for each window.
    void note_display_refresh();
    // Whether this pass's frames were run one to a refresh, and, on a Switch,
    // when the next pass is due and whether the last frame found two waiting
    // to be shown -- see the end of the main loop.
    bool display_locked_now = false;
    uint64_t locked_next_pass_us = 0;
    bool locked_queue_full = false;
    // The display's real refresh period, measured from frames that waited for
    // it; 0 until then.
    uint64_t locked_period_us = 0;
    uint64_t locked_learn_start_us = 0;
    int locked_learn_count = 0;
    // Vsync held off: the display fell behind (see the end of the main loop),
    // and until vsync_held_off_until_us this loop runs as with vsync off --
    // the game's own clock, never waiting for the display -- so that a
    // display that takes frames late or not at all slows only the picture,
    // not the game. Each time it happens again soon after, it holds off for
    // twice as long, up to two minutes.
    bool vsync_held_off = false;
    uint64_t vsync_held_off_until_us = 0;
    uint64_t vsync_hold_off_us = 10'000'000;
    uint64_t locked_since_us = 0;
    // Until when, after waking from sleep, a display that keeps frames
    // waiting is the display coming back rather than falling behind.
    uint64_t resume_settle_until_us = 0;
    uint64_t locked_waits_window_us = 0;
    int locked_waits_in_window = 0;
    // Vsync as it is right now: the setting, unless held off.
    bool vsync_in_effect() const { return settings.vsync && !vsync_held_off; }
    double save_flush_accumulator = 0.0;
    // Played time since the resume point was last written -- see
    // kResumePointIntervalSeconds.
    double resume_point_accumulator = 0.0;
    // Writes resume points off the game's thread. Declared after the
    // settings and paths it writes with, so it finishes with them still here.
    BackgroundSavePointWriter resume_writer;
    // The games' own saves, for Data Management: which game each is, and
    // every version it has been. Told when a game starts and stops.
    std::unique_ptr<SaveStore> save_store;
    // Answers other devices asking about those saves, or sending one.
    std::unique_ptr<net::SaveExchangeService> save_service;
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
    // Which pad's symbols the prompts are drawn in, and which button means
    // yes: the player's settings, except on a Switch -- see the definition.
    void apply_button_style();
    // The panel in front of the player, where that is not the window: see
    // Compositor::set_display_size(). Zero in both everywhere else.
    void panel_size(float& width, float& height) const;
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
    // device_layout() as the connected device resolves it, for its own screen.
    Layout companion_layout() const;
    // Which way the touch screen is turned where it is drawn, so the stylus
    // cursor's stick moves it the way the player sees it.
    ScreenRotation touch_screen_rotation() const;
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
    // How screens go to a companion: chosen by the session, screen by screen,
    // unless --video-codec pinned one -- see HostSession::CodecChoice.
    net::HostSession::CodecChoice codec_choice() const;
    double local_frames_per_second() const;
    double local_audio_sample_rate() const;
    void adopt_external_timing();
    // Whether a game played by another program is setting this loop's pace
    // -- see the top of the main loop.
    bool paced_by_external_game() const {
        return !is_companion() && external_source && external_source->running();
    }
    // Vsync on or off for this window: the setting, except while an external
    // game paces the loop, when waiting on this window's refresh as well
    // would be two clocks. Applied only when it changes.
    int swap_interval_applied = -1;
    int swap_interval_said = -1;
    void update_swap_pacing();
    // Once a second: how many frames the game made and how many came and went
    // before this app took them.
    uint64_t external_frames_said_ms = 0;
    uint64_t last_render_us = 0;
    void report_external_frames();
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
    // `whole_list`: every game's saves, even during one -- what Settings
    // opens. Otherwise, during a game, its own page alone.
    void open_data_management(bool whole_list = false);
    void apply_data_management_result(const DataManagementResult& result);
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
    // The name of the game being played: on a companion, the host's.
    const std::string& shown_game_title() const;
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
    // Whether the Pointer setting has anything to show or hide: a mouse
    // pointer on this device or on the one that joined it. A Switch playing
    // alone, or with another Switch, has neither, and is not offered it.
    bool pointer_choosable() const {
        return device_has_pointer() || (host_session && host_session->companion_has_pointer());
    }
    bool holds_gamepad() const { return !companion_joined() || gamepad_is_here(); }
    // The Controllers screen: put up over whatever this device is showing,
    // handed presses, and put away again back to where it was opened from.
    void open_controllers();
    void close_controllers();
    // The Wii U game's graphics packs, which Cemu keeps and can change while
    // the game runs -- see GraphicPacksScreen.
    bool graphic_packs_available() const;
    void open_graphic_packs();
    void update_graphic_packs_screen(const std::vector<UiAction>& actions,
                                     const std::vector<PointerEvent>& pointers);
    // What a press on the page asked for: a pack changed, or the page left.
    void apply_graphic_packs_result(const GraphicPacksScreenResult& result);
    // The game's Button Mapping, kept with it on the device running it --
    // see ButtonMappingScreen.
    bool button_mapping_available() const;
    void open_button_mapping();
    void update_button_mapping_screen(const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers);
    // What the page shows, for the game as it is now.
    void show_button_mapping(ButtonMappingScreen& screen) const;
    // A Wii U game's player's page -- see WiiUPlayerScreen. `screen` is
    // handed the controllers and the player's types; a companion's, the
    // host's, through show_host_wii_u_player(). `adding`: Add Controller's,
    // the player after the last, yet to be.
    void open_wii_u_player(PadSlot slot, bool adding = false, const std::string& pad = std::string());
    void update_wii_u_player_screen(const std::vector<UiAction>& actions,
                                    const std::vector<PointerEvent>& pointers);
    void show_wii_u_player(WiiUPlayerScreen& screen) const;
    // Detect Controller watching for a press: true once one was found.
    bool detect_controller();
    // The page on screen now that taps reach as a MenuPage, or none -- and
    // that page updated with these presses and no pointer, to hear the taps
    // the other device sent it. See MenuPage.
    MenuPage* page_shown();
    void update_shown_page(const std::vector<UiAction>& actions);
    // Cemu's own options -- see cemu_options -- asked for once Cemu is up,
    // set to this game's, and stepped from the menu.
    void update_cemu_options();
    std::vector<std::pair<std::string, std::string>> cemu_option_rows() const;
    void step_cemu_option(int index, int steps = 1);
    // One of Cemu's options made `choice`, kept for the game as a step is.
    void set_cemu_option(int index, int choice);
    // Pad `identity` made a new player after the last, as Cemu's type
    // `type` for it.
    void add_pad_as_player(const std::string& identity, int type);
    std::vector<std::vector<std::string>> cemu_option_choices() const;
    // Where among cemu_options the players' controllers begin, which the
    // menu lists under Controls rather than Game Options; -1 for none.
    int cemu_controls_from() const;
    // Cemu's option for player `player`'s controller type (0 for Player 1),
    // by its index in cemu_options; -1 where it has none.
    int cemu_player_type_option(int player) const;
    // A Wii U game's Controls as the menu lists them -- see
    // LayoutPickerState::wii_u_controllers.
    std::vector<ControllerRowView> wii_u_controller_rows() const;
    // The pad of one, by pad_identity(), as an index into `pads`; -1 for none.
    static int pad_index(const std::vector<KnownPad>& pads, const std::string& identity);
    // Moves pad `identity` in the line: a place up or down, into a slot, or
    // out of play. Logged as a player would say it.
    void move_pad(const std::string& identity, int direction);
    void put_pad_in_slot(const std::string& identity, PadSlot slot);
    void take_pad_out_of_play(const std::string& identity);
    // Controls' answers, from either device's pad -- see LayoutPickerResult.
    void apply_controls_result(const LayoutPickerResult& result);
    // Settings' Wii U TV audio, told to a Cemu that is up, when it is not
    // what it was. Cemu reads it as a game starts, so a game already
    // running hears it once it is reset.
    void apply_wii_u_tv_audio(int previous);
    // An amiibo scanned -- see pending_amiibo -- from the list of those in
    // the amiibo folder, or from the browser.
    bool amiibo_available() const;
    // Whether the menu offers Scan amiibo: where Cemu can scan one, only
    // while the game was looking for one as it was last going -- or always,
    // from a Cemu that does not say.
    bool amiibo_in_menu() const;
    void scan_amiibo_folder();
    void choose_amiibo(const std::string& path);
    void open_amiibo_browser();
    void update_amiibo_browser(const std::vector<UiAction>& actions,
                               const std::vector<PointerEvent>& pointers);
    void send_pending_amiibo();
    // The game's own dialogs, which Cemu hands over to be shown here: one
    // put up waits until nothing else is over the game, and is gone once the
    // game puts it away.
    GameDialogScreen game_dialog_screen;
    std::optional<ExternalGameDialog> pending_game_dialog;
    void update_game_dialogs();
    void leave_game_dialog();
    // What is up for the companion to show too: every frame, sent on change.
    void publish_game_dialog();
    // The pages the other device draws as this one shows them -- the amiibo
    // browser's rows, and where every one of them is scrolled.
    void publish_mirrored_pages();
    // Which of them is up here, as the other device knows it; None for a
    // page that is this device's alone.
    net::MirroredPage mirrored_page() const;
    // One page on both devices, scrolled the same: see MenuScrollSync. Built
    // knowing which end this is: see App::Impl's constructor.
    std::unique_ptr<MenuScrollSync> menu_scroll_sync;
    // And the companion's end of it, for the host's pages drawn here.
    MenuScrollSync companion_scroll_sync{/*host=*/false};
    // And its cursor -- see MenuCursorSync.
    MenuCursorSync host_cursor_sync{/*host=*/true};
    MenuCursorSync companion_cursor_sync{/*host=*/false};
    void sync_companion_scroll();
    // The host's amiibo browser, drawn here while this device is a companion.
    GameBrowser mirrored_amiibo;
    // Whether the game's keyboard is typed on the companion: it is the
    // GamePad, the device in the player's hands.
    bool companion_types_game_dialog() const {
        return host_session && host_session->has_companion() &&
               host_session->companion_screen() == ScreenId::Bottom;
    }
    // The companion's copy of the host's game dialog, while this device is one.
    MirroredGameDialog mirrored_game_dialog;
    void show_steam_keyboard(bool typing);
    // Steam's keyboard was asked for by its URL rather than raised by SDL,
    // so it is put away the same way.
    bool steam_keyboard_opened = false;
    // When the game was last seen going again with an amiibo waiting -- see
    // send_pending_amiibo().
    std::optional<std::chrono::steady_clock::time_point> amiibo_game_going_since;
    // What the game was looking for when it was last going -- see
    // kExternalFeatureAmiiboReading. Not what it says while paused under the
    // menu: a game lets go of the GamePad's reader then, as it does for the
    // HOME Menu, and takes it again once it is going.
    std::optional<ExternalAmiiboReading> amiibo_reading_going;
    // Why an amiibo would not scan in the game as it was last going, or
    // nothing when it would or there is no telling.
    std::optional<std::string> amiibo_unwanted_reason() const;
    // A popup in the quick menu, on both devices, put away with OK.
    void show_menu_notice(const std::string& message);
    // Any of those pages, which open over the game from its menu and go
    // back to where they came from -- Button Mapping too, though it is not
    // Cemu's.
    bool on_cemu_page() const {
        return state == State::GraphicPacks || state == State::Amiibo || state == State::ButtonMapping ||
               state == State::WiiUPlayer;
    }
    void enter_cemu_page(State page);
    void close_cemu_page();
    void update_controllers_screen(const std::vector<UiAction>& actions,
                                   const std::vector<PointerEvent>& pointers);
    // Save points, made and loaded from the quick menu. The slots are read
    // off the disk here and handed to the menu -- this device's, and the
    // companion's copy of it -- with one line about the last save.
    bool save_points_available() const;
    // A DS game played here, whose save Data Management can sync: Save Sync
    // on the quick menu.
    bool save_sync_available() const;
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
    // A Wii U's line kept as it stands, every frame, so that a controller
    // leaving leaves its place empty rather than the rest moving up -- and
    // one arriving takes the first empty place -- as on a Wii U.
    void keep_pad_order();
    // A new line, with which players a controller arriving may become kept
    // from the old -- see PadOrder::joinable.
    PadOrder keeping_joinable(PadOrder order) const {
        order.joinable = pad_order.joinable;
        return order;
    }
    // Whether the Wii U list is shown in place of "Playing on Cemu" -- see
    // LayoutPickerState::embedded.
    bool shows_embedded_menu() const;
    // Whether this app's menu, or a page opened from it, is up over a Wii U
    // game whose program can draw it in its own window -- the window the
    // player is looking at, where this one is behind it. From either device:
    // a companion opening the menu opens it here too.
    bool menu_over_wii_u_game() const;
    // That, or a corner notice over a Wii U game: anything Cemu's window has
    // to draw for this app.
    bool wants_menu_overlay() const;
    bool corner_notice_up() const;
    void show_corner_notice(const std::string& message);
    // What the corner says now, if anything: a notice for its moment, or
    // while an amiibo is on its way to the game with the menu closed,
    // "Scanning...".
    std::string shown_corner_notice() const;
    // Whether opening the menu pauses the game first -- see set_menu_open().
    bool pauses_wii_u_for_menu() const;
    // Hears Cemu out on the pause, opens a menu that was waiting for it, and
    // asks for the game to be paused or to go on as the pages over it say.
    void update_wii_u_pause();
    // The paused game's blurred picture in menu_backdrop, made when it is
    // first wanted. False while the game is not paused, or its picture has
    // not arrived, when the game is shown live and darkened instead.
    bool wii_u_backdrop_ready();
    // The screen Cemu's window is showing, whose picture that is.
    ScreenId wii_u_screen_shown() const { return external_shown_screen.value_or(ScreenId::Top); }
    // Draws that, on its own, and hands it over when it has changed; or takes
    // it down once it is not wanted.
    void render_menu_overlay();
    // The menu's own button, pressed on a page opened from the menu during a
    // game: straight back to the game, every page and the menu put away at
    // once, the way it closes the menu itself. False when that is not where
    // it is pressed, for the page to answer as usual.
    bool menu_key_back_to_game(const std::vector<UiAction>& actions);
    // The menu and the pages opened from it, over whatever has been drawn
    // already -- shared by the window and the overlay.
    void render_pages(float width, float height, bool over_game, const ScreenTextures* frames);
    // Which of a Wii U's screens this device is showing, for its menu's
    // icon; none for any other console.
    std::optional<ScreenId> wii_u_screen_here() const;
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
    // LayoutPickerState::embedded. `from_game` when it comes up from the
    // game rather than back from a page of its own -- see
    // LayoutPickerState::amiibo_wanted.
    void open_picker(bool embedded = false, bool from_game = true);
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
    // Opens it, with no waiting.
    void open_quick_menu();
    // The quick menu coming up from the game, and going back to it: its
    // backdrop, then itself, and the other way round. See MenuEntrance.
    MenuEntrance menu_entrance;
    // Closed back to the game and still going away: the picker is kept open
    // to be drawn until it has -- see quick_menu_leaving().
    bool menu_closing = false;
    bool quick_menu_leaving() const {
        return menu_closing && menu_entrance.leaving() && picker && picker->is_drawn() && state != State::Menu;
    }
    // Gone at once: closed from a page of it, where it was never on screen
    // to go away.
    void skip_menu_leave() {
        menu_entrance.finish();
        menu_closing = false;
        if (state != State::Menu && picker && !picker->embedded()) picker->close();
    }
    // Once it has gone, put away.
    void settle_menu_leave() {
        if (!menu_closing || menu_entrance.leaving()) return;
        menu_closing = false;
        if (state != State::Menu && picker && !picker->embedded()) picker->close();
    }
    // On a companion, whether the host had a page of its menu up last frame:
    // the menu coming back from one is not coming up from the game.
    bool companion_saw_host_page = false;
    // Draws `draw_backdrop` and then `draw_menu` as the quick menu's entrance
    // has them -- faded and grown in while it is coming up, and as they are
    // otherwise, or on any other page.
    template <typename Backdrop, typename Menu>
    void draw_entering(float width, float height, Backdrop&& draw_backdrop, Menu&& draw_menu) {
        const bool entering = state == State::Menu || quick_menu_leaving();
        if (entering) quads.set_opacity(menu_entrance.backdrop());
        draw_backdrop();
        quads.set_opacity(1.0f);
        if (entering) {
            quads.set_opacity(menu_entrance.contents());
            quads.set_zoom(menu_entrance.scale(), width * 0.5f, height * 0.5f);
        }
        draw_menu();
        quads.set_opacity(1.0f);
        quads.set_zoom(1.0f, 0.0f, 0.0f);
    }
    // The menu as the host has it, for a device that is only showing it.
    LayoutPickerState menu_state_from_host() const;
    // The two settings both devices share. On a companion these ask the host
    // and change nothing locally; the host owns them and broadcasts the result,
    // which is what keeps the two devices from ever disagreeing.
    void request_layout(const std::string& layout_id);
    // L3: the layout this one pairs with, drawn for the same hand -- see
    // layout_swap_from().
    void swap_layout();
    // Next Screen Layout: the next layout the quick menu offers -- see
    // next_menu_layout().
    void next_layout();
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
    // Reset for a game played by another program: see the definition.
    bool restart_external_game();
    // Applies and persists a host-side change to any of them.
    void adopt_filtering(bool bilinear);
    void adopt_render_scale(int scale);
    // The largest scale the game's core offers; 1 for nothing to choose.
    int max_render_scale() const;
    // What Auto comes to now, for the screens as this device and the one
    // connected to it draw them, up to `maximum`.
    int auto_render_scale_now(int maximum) const;
    // On Auto, the core kept at what it comes to as the window, the layout and
    // the devices change.
    void update_auto_render_scale();
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
    // The stylus on the right stick: moved by the stick in `held`, which it
    // takes out, and touching for R3 -- see RightStickMode::TouchCursor.
    void apply_stylus_cursor(DsInputState& held);
    // The right stick as the Touch Analog Stick: the touch screen touched
    // where it is pushed to, while it is.
    void apply_touch_analog_stick(DsInputState& held, const ScreenSize& touch);
    // The pad moving the spot on Touch Point's page, as smoothly as the
    // Touch Cursor moves in a game. True if it moved.
    bool steer_touch_spot(ButtonMappingScreen& screen);
    void publish_stylus_cursor();
    // Drawn over the touch screen where this device shows it.
    void draw_stylus_cursor(float width, float height);
    // This game's Button Mapping, taken on: by this device's pad, and kept
    // with the game where it runs here. A companion's comes from the host.
    void adopt_button_mapping(const ButtonMapping& mapping);
    // Resume from Save State: whether it is on for this game, as the host has
    // it, and the same ask-or-apply pair as everything else the menu shares.
    // Off means the resume point is deleted as well as no longer written, so
    // one left from before can never be resumed later by surprise.
    bool resume_in_effect() const;
    void request_resume(bool on);
    void adopt_resume(bool on);
    // Brings this game's resume point up to date, if it has Resume from Save
    // State on: the state is taken here, on the game's thread, and written
    // on resume_writer's. `why` goes in the log.
    void write_resume_point(const char* why);
    // Writes the cartridge save if it has changed, and the resume point with
    // it when it has -- the two have to agree, since loading a state puts the
    // in-game save back to how it was then. With `why`, the resume point is
    // written either way.
    void flush_saves(const char* why = nullptr);
    // At the start of a game with it on: loads the resume point, if there is
    // one, and runs one frame from it so the picture paused behind the popup
    // is the moment resumed. False, and the game starts from the beginning,
    // when there is nothing to resume or it will not load.
    bool resume_game();
    // OK on the popup, from either device: the game goes on from there.
    void dismiss_resume_notice();
    // A DS game's Rumble Pak: whether one is in, as the host has it, and the
    // same ask-or-apply pair as everything else the menu shares.
    bool rumble_pak_in_effect() const;
    void request_rumble_pak(bool inserted);
    void adopt_rumble_pak(bool inserted);
    bool custom_textures_in_effect() const;
    void request_custom_textures(bool on);
    void adopt_custom_textures(bool on);
    // A DS game's LCD filter -- see GameConfig::lcd_filter. The host's, and
    // on a companion what the host says.
    bool lcd_filter = false;
    bool lcd_filter_in_effect() const;
    void request_lcd_filter(bool on);
    void adopt_lcd_filter(bool on);
    // The compositor told whether to draw it, before each frame it draws.
    void apply_lcd_grid();
    // Whether this device should be making sound right now.
    bool sound_plays_here() const;
    // Loads a ROM and starts emulating it. On failure it puts a readable
    // message on screen and returns false; `fatal_on_failure` decides whether
    // dismissing that message quits or goes back to the browser.
    bool start_game(const std::string& rom_path, bool fatal_on_failure);
    // The game that was playing has closed, and its save may have changed.
    void note_game_closed();
    // The same for a game another program plays: Cemu writes its saves
    // while it runs, and they are a new version once it stops.
    void note_external_game_closed();
    // Where Data Management looks for the ROM of a save it has never seen
    // played: the folders the file picker would show.
    void update_save_rom_dirs();
    // The systems whose saves this device keeps and trades.
    std::vector<std::string> save_systems() const;
    // The libretro half of the above: the core, its callbacks and its
    // settings, for the game at `rom_path` on `system`. `details` gets lines
    // worth showing the player if it fails.
    bool open_core(const std::string& rom_path, std::string* error,
                   std::vector<std::string>* details);
    void open_browser();
    void enter_message(MessageKind kind,
                       const std::string& title,
                       const std::string& body,
                       const std::vector<std::string>& details,
                       bool fatal);
    void persist_game_config();
    void handle_ui(const std::vector<UiAction>& actions, const std::vector<PointerEvent>& pointers);
    // The menus' own sounds: which one a press has earned, decided from what
    // is on screen before and after it, and a device of their own to play
    // them on -- see UiSoundJudge and UiSoundPlayer.
    UiSoundPlayer ui_sounds;
    UiSoundJudge ui_sound_judge;
    UiSoundView sound_view() const;
    // Save states made since the app started: each one is heard -- see
    // UiSoundView::saves.
    uint32_t save_states_made = 0;
    // What the judge decided, played -- and, with a companion mirroring the
    // menu, sent for it to play too. A companion does the reverse: while it
    // is in a session the host's sounds are the ones it plays.
    void play_ui_sound(UiSound sound, const UiSoundView& view);
    bool host_decides_sounds() const {
        return is_companion() && (state == State::Companion || state == State::Menu);
    }
    // How long the last frame took, so screens that need a clock have one.
    double last_delta_seconds = 0.0;
    CursorVisibility cursor;
    // What SDL was last told, so it is only told when the answer changes.
    bool cursor_shown = false;
    void render();
    // swap_and_pace(), measured for frame_delay.
    void swap();
    bool write_screenshot(const std::string& path);
};

App::App() : impl_(new Impl()) {}
App::~App() = default;

bool App::quit_chosen() const { return impl_->quit_chosen; }

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
    controllers_screen.release_textures();
    wii_u_player_screen.release_textures();
    mirrored_wii_u_player.release_textures();
    mirrored_controllers.release_textures();
    if (data_screen) data_screen->release_textures();
    // The game's icon, which is kept while it is the same game: kept across
    // a new context, its name came to mean another texture, drawn garbled.
    game_icon = Texture();
    game_icon_for.clear();
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
    input.set_microphone_button(profile.has_microphone);
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

void App::Impl::apply_button_style() {
#ifdef __SWITCH__
    // A Switch's own, the way its own menus have them, with no setting for
    // either: A (east) says yes, and the buttons are drawn as the letters on
    // them -- except with one Joy-Con held sideways, whose letters are the
    // wrong way round for how it is held, so the system draws them by
    // position instead and so does this. Asked every pass, since which
    // controller is in the player's hands can change at any press.
    const PadGlyphStyle style =
        input.single_joycon_in_use() ? PadGlyphStyle::Positional : PadGlyphStyle::Nintendo;
    if (style != button_glyphs.style()) {
        button_glyphs.set_style(style);
        log_info(style == PadGlyphStyle::Positional
                     ? "button prompts: by position, for a single Joy-Con"
                     : "button prompts: the Switch's letters");
    }
    input.set_confirm_button(ConfirmButton::East);
    set_confirm_button(ConfirmButton::East);
#else
    button_glyphs.set_style(settings.pad_glyphs);
    input.set_confirm_button(settings.confirm_button);
    set_confirm_button(settings.confirm_button);
#endif
}

void App::Impl::panel_size(float& width, float& height) const {
    width = 0.0f;
    height = 0.0f;
#ifdef __SWITCH__
    // A Switch playing here hands SDL a 1920x1080 window whichever way it is
    // held, and undocked scales it onto a 1280x720 panel -- so the window
    // alone said "a television" in handheld, and Standard kept its margins
    // rather than cropping the console to the screens. The same answer the
    // Switch companion gets from the console (companion/source/main.cpp).
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    width = handheld ? 1280.0f : 1920.0f;
    height = handheld ? 720.0f : 1080.0f;
#endif
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
        // Alone, Separate Screens is the one screen the game is being played
        // on -- swapped with L3 or from the menu, and kept per game.
        if (system_profile(system).solo_screens == SoloScreens::Both) {
            return resolve_for_device(chosen, separate_screen);
        }
        // One screen, whichever the player is watching. Resolved exactly as a
        // paired device resolves its own, so what is drawn and where a touch
        // lands come from the same place either way.
        return resolve_for_device(chosen, solo_screen);
    }
    return resolve_for_device(chosen, local_screen);
}

Layout App::Impl::companion_layout() const {
    // The companion's own version of device_layout(): the same layout, but
    // resolved for whichever screen the companion holds rather than this
    // device's. A companion is by definition paired the moment there is a
    // reported size to act on, so there is no unpaired fallback to consider.
    Layout layout = resolve_for_handedness(resolve_gap_choice(active_layout(), screen_gap), current_handedness());
    if (layout.per_device && host_session) {
        layout = resolve_for_device(layout, host_session->companion_screen());
    }
    return layout;
}

ScreenRotation App::Impl::touch_screen_rotation() const {
    // Where the touch screen is drawn: this device's window first, then the
    // connected device's, whichever is the one holding it.
    for (const PlacedScreen& placed : place_screens(device_layout(), static_cast<float>(output_width),
                                                     static_cast<float>(output_height), screens_geometry())) {
        if (placed.screen == ScreenId::Bottom && placed.rect.w > 0.0f && placed.rect.h > 0.0f) {
            return placed.rotation;
        }
    }
    int companion_width = 0;
    int companion_height = 0;
    if (host_session && host_session->has_companion() && host_session->companion_wants(ScreenId::Bottom) &&
        host_session->companion_display_size(companion_width, companion_height)) {
        for (const PlacedScreen& placed : place_screens(companion_layout(), static_cast<float>(companion_width),
                                                         static_cast<float>(companion_height),
                                                         screens_geometry())) {
            if (placed.screen == ScreenId::Bottom && placed.rect.w > 0.0f && placed.rect.h > 0.0f) {
                return placed.rotation;
            }
        }
    }
    return ScreenRotation::None;
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

    for (const PlacedScreen& placed : place_screens(companion_layout(), static_cast<float>(companion_width),
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

net::HostSession::CodecChoice App::Impl::codec_choice() const {
    if (options.video_codec_override == "tiles") return net::HostSession::CodecChoice::Tiles;
    if (options.video_codec_override == "h264") return net::HostSession::CodecChoice::H264;
    return net::HostSession::CodecChoice::Automatic;
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

void App::Impl::update_swap_pacing() {
    const int wanted = paced_by_external_game() ? 0 : (vsync_in_effect() ? 1 : 0);
    if (wanted == swap_interval_applied || window == nullptr) return;
    SDL_GL_SetSwapInterval(wanted);
    // Said when it changes, not each time it is applied again: every change
    // in Settings reapplies it, and the log had a line per button press.
    if (wanted != swap_interval_said && (swap_interval_applied != -1 || wanted == 0)) {
        swap_interval_said = wanted;
        log_info(wanted == 0 && paced_by_external_game()
                     ? "the game beside this app now sets the pace; this window no longer waits "
                       "for its own refresh"
                     : std::string("this window waits for its refresh: ") +
                           (wanted ? "yes" : "no"));
    }
    swap_interval_applied = wanted;
}

void App::Impl::note_display_refresh() {
    if (window == nullptr || window == display_refresh_window) return;
    display_refresh_window = window;
    // A new window may be on another display: measured again.
    locked_period_us = 0;
    locked_learn_count = 0;
    SDL_DisplayMode mode{};
    const int display = SDL_GetWindowDisplayIndex(window);
    display_refresh_hz =
        SDL_GetCurrentDisplayMode(display < 0 ? 0 : display, &mode) == 0 && mode.refresh_rate > 0
            ? mode.refresh_rate
            : 0.0;
    if (display_refresh_hz != display_refresh_said) {
        display_refresh_said = display_refresh_hz;
        log_info(display_refresh_hz > 0.0
                     ? "the display refreshes " + std::to_string(static_cast<int>(display_refresh_hz)) +
                           " times a second"
                     : std::string("the display does not say how often it refreshes"));
    }
}

void App::Impl::report_external_frames() {
    const uint64_t now = SDL_GetTicks64();
    if (external_frames_said_ms == 0) {
        external_frames_said_ms = now;
        return;
    }
    if (now - external_frames_said_ms < 1000) return;
    external_frames_said_ms = now;
    std::string line = "from the game this second:";
    bool any = false;
    for (const ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
        const ExternalFrameLink::FrameCounts counts = external_source->take_frame_counts(screen);
        if (counts.announced == 0) continue;
        line += std::string(any ? ";" : "") + " " + to_string(screen) + " screen " +
                std::to_string(counts.announced) + " made, " + std::to_string(counts.missed) +
                " replaced before they were taken";
        // How old the frames were -- from the game recording the copy of its
        // screen to the announcement, and to this app taking it. A game that
        // does not say when it captured them leaves this out.
        if (counts.timed_announced > 0 && counts.timed_taken > 0) {
            const auto ms = [](uint64_t us) {
                char text[16];
                std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(us) / 1000.0);
                return std::string(text);
            };
            line += " (" + ms(counts.announced_age_us / counts.timed_announced) +
                    "ms old when announced, " + ms(counts.taken_age_us / counts.timed_taken) +
                    "ms when taken, at most " + ms(counts.taken_age_max_us) + "ms)";
        }
        any = true;
    }
    if (any) log_info(line);
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
        why = "swapped for this game";
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

void App::Impl::toggle_solo_screen_swap() {
    solo_screen_swapped = !solo_screen_swapped;
    if (have_game_config) {
        game_config.screens_swapped = solo_screen_swapped;
        persist_game_config();
    }
    if (picker) picker->set_screens_swapped(solo_screen_swapped);
    update_solo_screen();
}

void App::Impl::refresh_game_icon() {
    std::string key;
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    if (is_companion()) {
        key = "host:" + companion_session->game_title() + ":" +
              std::to_string(companion_session->game_icon_rgba().size());
        if (key == game_icon_for) return;
        width = companion_session->game_icon_width();
        height = companion_session->game_icon_height();
        rgba = companion_session->game_icon_rgba();
    } else {
        key = "here:" + game_icon_rom;
        if (key == game_icon_for || game_icon_rom.empty()) return;
        // Read out of the game itself, so a game that has never been saved
        // has its icon too -- and from the save store when the file says
        // nothing it can read (a Wii U disc image, say).
        bool found = false;
        SystemId rom_system = SystemId::Ds;
        if (system_for_rom(game_icon_rom, rom_system)) {
            if (rom_system == SystemId::Ds) {
                DsRomInfo info;
                found = read_ds_rom_info(game_icon_rom, info, nullptr) && !info.icon.empty() &&
                        save_icon_rgba(kSaveSystemDs, info.icon, width, height, rgba);
            } else {
                ConsoleTitleInfo info;
                const bool read =
                    rom_system == SystemId::ThreeDs
                        ? read_3ds_title_info(game_icon_rom, info, nullptr)
                    : path_extension_lower(game_icon_rom) == "wua"
                        ? read_wii_u_archive_title_info(game_icon_rom, "", 0, info, nullptr)
                        : read_wii_u_title_info(game_icon_rom, info, nullptr);
                found = read && !info.icon.empty() && decode_rgba_icon(info.icon, width, height, rgba);
            }
        }
        if (!found && save_store) {
            // At most every two seconds while it is not found: the store may
            // only learn the game once it has been played a moment.
            const uint64_t now_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count());
            if (game_icon_tried_us != 0 && now_us - game_icon_tried_us < 2000000) return;
            game_icon_tried_us = now_us;
            SaveInfo info;
            found = save_store->find_for_rom(game_icon_rom, info) && !info.icon.empty() &&
                    save_icon_rgba(info.system, info.icon, width, height, rgba);
        }
        if (!found) {
            game_icon = Texture();
            return;
        }
    }
    game_icon_for = key;
    game_icon = Texture();
    game_icon_width = width;
    game_icon_height = height;
    game_icon_rgba = rgba;
    if (width > 0 && height > 0 &&
        rgba.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) {
        game_icon.upload_rgba8888(reinterpret_cast<const uint32_t*>(rgba.data()), width, height,
                                  width);
        game_icon.set_linear_filtering(true);
    }
}

std::optional<ScreenId> App::Impl::wii_u_screen_here() const {
    if (system != SystemId::WiiU) return std::nullopt;
    if (is_companion()) return companion_session->granted_screen();
    if (host_session && host_session->has_companion()) return host_session->host_screen();
    return solo_screen;
}

bool App::Impl::is_companion() const {
    return companion_session &&
           companion_session->status() == net::CompanionSession::Status::Connected;
}

const std::string& App::Impl::shown_game_title() const {
    return is_companion() ? companion_session->game_title() : game_title;
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
        pad.pressing = reading.pressing;
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
        pad.pressing = reported.pressing;
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
    // A player with a controller is never a Classic Controller alone -- see
    // offered_type() -- but one plugged into a Wii Remote, kept as that.
    // Nor None: a controller only becomes a player with a type -- see
    // keep_pad_order().
    for (int player = 0; player < kPadPlayers; ++player) {
        if (!routed.player_fed[player]) continue;
        const int option = cemu_player_type_option(player);
        if (option < 0) continue;
        const ExternalOption& type = cemu_options[static_cast<std::size_t>(option)];
        if (type.active < 0 || type.active >= static_cast<int>(type.choices.size())) continue;
        const std::string& now = type.choices[static_cast<std::size_t>(type.active)];
        if (now != "Classic Controller") continue;
        const int instead = offered_instead(now, type.choices);
        if (instead >= 0) set_cemu_option(option, instead);
    }

    // The GamePad's buttons are its pads'; its screen is the device showing
    // that screen, and how it is held the device whose controller it is --
    // only that one's, never the other's: a Steam Deck's gyro is not the
    // GamePad's while a pad on another device is. A controller through Steam
    // Input has no motion of its own to give, so it is the device's: its own
    // sensors, or a DSU server it reads. With controllers from both, the one
    // showing it; with none, none.
    const DsInputState& screen_device = gamepad_here ? here : remote;
    const DsInputState& other_device = gamepad_here ? remote : here;
    DsInputState gamepad = routed.gamepad;
    gamepad.touching = screen_device.touching;
    gamepad.touch_x = screen_device.touch_x;
    gamepad.touch_y = screen_device.touch_y;
    bool screen_device_holds_it = false;
    bool other_device_holds_it = false;
    for (const std::size_t index : pads_in_slot(pad_slots, PadSlot::gamepad())) {
        (known_pads[index].on_this_device == gamepad_here ? screen_device_holds_it : other_device_holds_it) = true;
    }
    const DsInputState* holder = screen_device_holds_it  ? &screen_device
                                 : other_device_holds_it ? &other_device
                                                         : nullptr;
    if (holder != nullptr && holder->motion.present) gamepad.motion = holder->motion;

    apply_pad_rumble();
    return gamepad;
}

void App::Impl::apply_pad_rumble() {
    std::map<std::string, KnownPad> wanted;
    for (std::size_t index = 0; index < known_pads.size() && index < pad_slots.size(); ++index) {
        const KnownPad& pad = known_pads[index];
        const PadSlot slot = pad_slots[index];
        if (slot.kind == PadSlot::Kind::NotUsed || rumble_quiet) continue;
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

bool App::Impl::graphic_packs_available() const {
    return system == SystemId::WiiU && !is_companion() && external_source &&
           external_source->running() && external_source->graphic_packs_supported();
}

void App::Impl::enter_cemu_page(State page) {
    cemu_page_came_from = state;
    // One menu on screen at a time, as for Controllers.
    if (picker) picker->close();
    state = page;
    input.set_game_input_enabled(false);
}

bool App::Impl::button_mapping_available() const {
    return !is_companion() && local_game() && system != SystemId::WiiU;
}

void App::Impl::show_button_mapping(ButtonMappingScreen& screen) const {
    screen.set_mapping(button_mapping, system, pad_console_for(system, right_stick_mode));
}

void App::Impl::open_button_mapping() {
    enter_cemu_page(State::ButtonMapping);
    show_button_mapping(button_mapping_screen);
    button_mapping_screen.open();
    log_info("the button mapping page is up");
}

void App::Impl::update_button_mapping_screen(const std::vector<UiAction>& actions,
                                             const std::vector<PointerEvent>& pointers) {
    // The game went while the page was up: nothing left to map.
    if (!button_mapping_available()) {
        close_cemu_page();
        return;
    }
    show_button_mapping(button_mapping_screen);
    steer_touch_spot(button_mapping_screen);
    const ButtonMappingScreenResult result = button_mapping_screen.update(
        actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height));
    if (result.changed) {
        const bool back_to_standard =
            result.changed->is_standard(pad_console_for(system, right_stick_mode));
        adopt_button_mapping(*result.changed);
        log_info(back_to_standard ? "button mapping: every button is back to standard"
                                  : "button mapping: a button was given another job");
    }
    if (result.right_stick) request_right_stick_mode(*result.right_stick);
    if (result.close_everything) {
        close_cemu_page();
        if (state == State::Menu) set_menu_open(false);
    } else if (result.close_requested) {
        close_cemu_page();
    }
}

void App::Impl::show_wii_u_player(WiiUPlayerScreen& screen) const {
    // Cemu's types for the player, with the one it has now.
    std::vector<std::string> types;
    int current = -1;
    if (screen.slot().kind == PadSlot::Kind::Player) {
        const int option = cemu_player_type_option(screen.slot().player);
        if (option >= 0) {
            types = cemu_options[static_cast<std::size_t>(option)].choices;
            current = cemu_options[static_cast<std::size_t>(option)].active;
        }
    }
    screen.set_controllers(wii_u_controller_rows(), std::move(types), current);
}

void App::Impl::open_wii_u_player(PadSlot slot, bool adding, const std::string& pad) {
    enter_cemu_page(State::WiiUPlayer);
    if (adding) {
        wii_u_player_screen.open_adding(slot, pad);
    } else {
        wii_u_player_screen.open(slot);
    }
    show_wii_u_player(wii_u_player_screen);
    log_info(adding ? "adding a controller as " + pad_slot_name(slot) : "the page for " + pad_slot_name(slot) + " is up");
}

bool App::Impl::detect_controller() {
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    std::map<std::string, uint32_t> now;
    std::string found;
    for (const KnownPad& pad : pads) {
        const std::string identity = pad_identity(pad);
        now[identity] = pad.pressing;
        const auto was = detect_pressing.find(identity);
        // A button down that was not before -- on a controller that was here
        // a frame ago, so one plugged in with a button held does not count.
        if (found.empty() && was != detect_pressing.end() && (pad.pressing & ~was->second) != 0) found = identity;
    }
    detect_pressing = std::move(now);
    if (found.empty()) return false;
    // A new player's is only chosen, until it is added.
    if (!wii_u_player_screen.adding()) put_pad_in_slot(found, wii_u_player_screen.slot());
    wii_u_player_screen.detected(found);
    detect_pressing.clear();
    return true;
}

void App::Impl::update_wii_u_player_screen(const std::vector<UiAction>& actions,
                                           const std::vector<PointerEvent>& pointers) {
    // The game went while the page was up: no player left to change.
    if (system != SystemId::WiiU || !local_game()) {
        close_cemu_page();
        return;
    }
    show_wii_u_player(wii_u_player_screen);
    // A press found on a controller is that controller chosen, and nothing
    // else: the button pressed was for this, not for the page.
    if (wii_u_player_screen.detecting()) {
        wii_u_player_screen.tick(static_cast<float>(last_delta_seconds));
        if (detect_controller()) return;
        if (!wii_u_player_screen.detecting()) {
            detect_pressing.clear();
            log_info("controllers: no controller was pressed in time");
        }
    }
    const WiiUPlayerScreenResult result = wii_u_player_screen.update(
        actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height));
    const PadSlot slot = wii_u_player_screen.slot();
    if (result.added && result.controller_chosen) {
        add_pad_as_player(*result.controller_chosen, result.type_chosen);
    } else if (result.type_chosen >= 0 && slot.kind == PadSlot::Kind::Player) {
        set_cemu_option(cemu_player_type_option(slot.player), result.type_chosen);
    }
    if (result.controller_chosen && !result.added) {
        if (result.controller_chosen->empty()) {
            if (const ControllerRowView* own = wii_u_player_screen.own_row()) take_pad_out_of_play(own->pad);
        } else {
            put_pad_in_slot(*result.controller_chosen, slot);
        }
    }
    if (result.removed) {
        if (const ControllerRowView* own = wii_u_player_screen.own_row()) take_pad_out_of_play(own->pad);
    }
    if (result.detect_started) {
        // Where every controller's buttons are as it starts.
        detect_pressing.clear();
        for (const KnownPad& pad : gather_pads(companion_joined())) detect_pressing[pad_identity(pad)] = pad.pressing;
        log_info("controllers: waiting for a press on the controller " + pad_slot_name(slot) + " will use");
    }
    if (result.close_everything) {
        close_cemu_page();
        if (state == State::Menu) set_menu_open(false);
    } else if (result.close_requested) {
        close_cemu_page();
    }
}

void App::Impl::open_graphic_packs() {
    enter_cemu_page(State::GraphicPacks);
    graphic_packs_screen.open();
    external_source->request_graphic_packs();
    log_info("the graphics packs page is up");
}

void App::Impl::update_cemu_options() {
    if (!external_source || !external_source->running() || !external_source->options_supported()) {
        cemu_options.clear();
        cemu_options_asked = false;
        cemu_options_applied = false;
        return;
    }
    if (!cemu_options_asked) {
        external_source->request_options();
        cemu_options_asked = true;
    }
    std::vector<ExternalOption> options;
    if (!external_source->take_options(options)) return;
    // Settings' own, the same for every game: made what Settings says, and
    // kept out of this game's Options.
    for (auto it = options.begin(); it != options.end();) {
        if (it->id != kWiiUTvAudioOption) {
            ++it;
            continue;
        }
        const int wanted = std::clamp(settings.wii_u_tv_audio, 0, 2);
        if (it->active != wanted && wanted < static_cast<int>(it->choices.size())) {
            external_source->change_option(it->id, wanted);
            log_info(std::string("Cemu: TV audio set to ") + kWiiUTvAudioChoices[wanted]);
        }
        it = options.erase(it);
    }
    cemu_options = std::move(options);
    // Each player's controller last, together, for Controls -- see
    // cemu_controls_from().
    std::stable_partition(cemu_options.begin(), cemu_options.end(),
                          [](const ExternalOption& option) { return !is_cemu_controls_option(option.id); });
    if (!cemu_options_applied && have_game_config) {
        // This game's choice for each one Cemu keeps for every game alike, or
        // Cemu's own default where it has none, so one game's never carries
        // into the next.
        cemu_options_applied = true;
        for (ExternalOption& option : cemu_options) {
            if (option.default_choice < 0) continue;
            const auto kept = game_config.wii_u_options.find(option.id);
            int wanted = kept != game_config.wii_u_options.end() ? kept->second : option.default_choice;
            if (wanted < 0 || wanted >= static_cast<int>(option.choices.size())) {
                wanted = option.default_choice;
            }
            if (wanted == option.active) continue;
            external_source->change_option(option.id, wanted);
            option.active = wanted;
            log_info("Cemu: " + option.label + " set to this game's, " +
                     option.choices[static_cast<std::size_t>(wanted)]);
        }
    }
    if (picker && picker->is_open() && system == SystemId::WiiU && !is_companion()) {
        picker->set_producer_options(cemu_option_rows(), cemu_option_choices(), cemu_controls_from());
    }
}

std::vector<std::pair<std::string, std::string>> App::Impl::cemu_option_rows() const {
    std::vector<std::pair<std::string, std::string>> rows;
    for (const ExternalOption& option : cemu_options) {
        const int at = option.active >= 0 ? option.active : 0;
        rows.emplace_back(option.label, option.choices.empty()
                                            ? std::string()
                                            : option.choices[static_cast<std::size_t>(at)]);
    }
    return rows;
}

int App::Impl::cemu_controls_from() const {
    for (std::size_t index = 0; index < cemu_options.size(); ++index) {
        if (is_cemu_controls_option(cemu_options[index].id)) return static_cast<int>(index);
    }
    return -1;
}

int App::Impl::cemu_player_type_option(int player) const {
    const std::string id = "player" + std::to_string(player + 1) + "_type";
    for (std::size_t index = 0; index < cemu_options.size(); ++index) {
        if (cemu_options[index].id == id) return static_cast<int>(index);
    }
    return -1;
}

std::vector<ControllerRowView> App::Impl::wii_u_controller_rows() const {
    std::vector<ControllerRowView> rows;
    if (system != SystemId::WiiU || is_companion()) return rows;
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    const PadLineup lineup = line_up_pads(pads, pad_order, holds_gamepad());
    const std::string other = companion_joined() ? host_session->companion_name() : std::string("Other Device");
    for (const LineupRow& line : lineup_rows(lineup)) {
        ControllerRowView row;
        row.slot = line.slot;
        if (line.pad >= 0 && static_cast<std::size_t>(line.pad) < pads.size()) {
            const KnownPad& pad = pads[static_cast<std::size_t>(line.pad)];
            row.pad = pad_identity(pad);
            row.name = pad.name.empty() ? pad_label(pad) : pad.name;
            row.device = pad.on_this_device ? settings.device_name : other;
            row.place = row.device + (pad.slot >= 0 ? "  P" + std::to_string(pad.slot + 1) : std::string());
        }
        // What the game has there: the GamePad, or the type Cemu has the
        // player as.
        if (line.slot.kind == PadSlot::Kind::GamePad) {
            row.type = "Wii U GamePad";
        } else if (line.slot.kind == PadSlot::Kind::Player) {
            const int option = cemu_player_type_option(line.slot.player);
            if (option >= 0) {
                const ExternalOption& type = cemu_options[static_cast<std::size_t>(option)];
                if (type.active >= 0 && type.active < static_cast<int>(type.choices.size())) {
                    row.type = wii_u_type_name(type.choices[static_cast<std::size_t>(type.active)]);
                }
            }
        }
        rows.push_back(std::move(row));
    }
    // Where each comes in Controller's list, which is the same however the
    // line is: this device's first, then the other's, each by the player
    // its own system has it as.
    std::vector<std::pair<std::tuple<int, int, std::string>, std::size_t>> order;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const int pad = pad_index(pads, rows[index].pad);
        if (pad < 0) continue;
        const KnownPad& known = pads[static_cast<std::size_t>(pad)];
        order.push_back({{known.on_this_device ? 0 : 1, known.slot >= 0 ? known.slot : kPadPlayers + pad,
                          rows[index].pad},
                         index});
    }
    std::sort(order.begin(), order.end());
    for (std::size_t at = 0; at < order.size(); ++at) rows[order[at].second].listed = static_cast<int>(at);
    return rows;
}

int App::Impl::pad_index(const std::vector<KnownPad>& pads, const std::string& identity) {
    for (std::size_t index = 0; index < pads.size(); ++index) {
        if (pad_identity(pads[index]) == identity) return static_cast<int>(index);
    }
    return -1;
}

void App::Impl::move_pad(const std::string& identity, int direction) {
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    PadLineup lineup = line_up_pads(pads, pad_order, holds_gamepad());
    const int pad = pad_index(pads, identity);
    if (pad < 0 || !move_among_players(lineup, pad, direction)) return;
    pad_order = keeping_joinable(order_of(pads, lineup, holds_gamepad()));
    log_info("controllers: the order was changed");
}

void App::Impl::put_pad_in_slot(const std::string& identity, PadSlot slot) {
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    PadLineup lineup = line_up_pads(pads, pad_order, holds_gamepad());
    const int pad = pad_index(pads, identity);
    if (pad < 0 || !put_in_slot(lineup, pad, slot)) return;
    pad_order = keeping_joinable(order_of(pads, lineup, holds_gamepad()));
    log_info("controllers: " + pad_label(pads[static_cast<std::size_t>(pad)]) + " is now " + pad_slot_name(slot));
}

void App::Impl::take_pad_out_of_play(const std::string& identity) {
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    PadLineup lineup = line_up_pads(pads, pad_order, holds_gamepad());
    const int pad = pad_index(pads, identity);
    if (pad < 0 || !take_out_of_play(lineup, pad)) return;
    pad_order = keeping_joinable(order_of(pads, lineup, holds_gamepad()));
    log_info("controllers: " + pad_label(pads[static_cast<std::size_t>(pad)]) + " is no longer used");
}

void App::Impl::apply_controls_result(const LayoutPickerResult& result) {
    // The line is the host's: a companion's press reaches the host's menu
    // and lands here there.
    if (is_companion() || !picker) return;
    const std::vector<ControllerRowView>& rows = picker->state().wii_u_controllers;
    const auto row_at = [&](int index) -> const ControllerRowView* {
        return index >= 0 && index < static_cast<int>(rows.size()) ? &rows[static_cast<std::size_t>(index)] : nullptr;
    };
    if (const ControllerRowView* row = row_at(result.controller_identified)) {
        const std::vector<KnownPad> pads = gather_pads(companion_joined());
        const int pad = pad_index(pads, row->pad);
        if (pad >= 0) identify_pad(pads[static_cast<std::size_t>(pad)]);
    }
    if (const ControllerRowView* row = row_at(result.controller_picked_up)) moving_pad = row->pad;
    if (result.controller_moved != 0 && !moving_pad.empty()) move_pad(moving_pad, result.controller_moved);
    if (!picker->moving_controller()) moving_pad.clear();
    picker->set_wii_u_controllers(wii_u_controller_rows());
}

std::vector<std::vector<std::string>> App::Impl::cemu_option_choices() const {
    std::vector<std::vector<std::string>> choices;
    for (const ExternalOption& option : cemu_options) choices.push_back(option.choices);
    return choices;
}

void App::Impl::step_cemu_option(int index, int steps) {
    if (index < 0 || index >= static_cast<int>(cemu_options.size()) || !external_source) return;
    ExternalOption& option = cemu_options[static_cast<std::size_t>(index)];
    const int count = static_cast<int>(option.choices.size());
    if (count < 2) return;
    // On as many as its popup chose, round from the last to the first, as
    // every value in the menu's Options goes.
    option.active = ((option.active >= 0 ? option.active : 0) + std::max(1, steps)) % count;
    external_source->change_option(option.id, option.active);
    const std::string& value = option.choices[static_cast<std::size_t>(option.active)];
    log_info("Cemu: " + option.label + ", " + value);
    // A player's type is this game's whatever Cemu says its default is: it
    // is what lets a controller arriving be that player -- see
    // keep_pad_order().
    if ((option.default_choice >= 0 || is_cemu_controls_option(option.id)) && have_game_config) {
        game_config.wii_u_options[option.id] = option.active;
        persist_game_config();
    }
    if (option.restart_needed) show_corner_notice(option.label + " will change when the game is reset.");
    if (picker) picker->set_producer_options(cemu_option_rows(), cemu_option_choices(), cemu_controls_from());
}

void App::Impl::set_cemu_option(int index, int choice) {
    if (index < 0 || index >= static_cast<int>(cemu_options.size())) return;
    const ExternalOption& option = cemu_options[static_cast<std::size_t>(index)];
    const int count = static_cast<int>(option.choices.size());
    const int at = option.active >= 0 ? option.active : 0;
    if (count < 2 || choice < 0 || choice >= count || choice == option.active) return;
    step_cemu_option(index, ((choice - at) % count + count) % count);
}

void App::Impl::keep_pad_order() {
    // A controller arriving becomes a player only where this game has had
    // one, as it had it -- never None. Elsewhere it waits in Not used to be
    // added, its type chosen then. Until Cemu has said what types it has,
    // nobody joins, and nothing is kept: the line is worked out again once
    // it has. A Cemu with no types to say lets anyone join, as before.
    const bool told = !external_source->options_supported() || !cemu_options.empty();
    std::vector<bool> joinable(kPadPlayers, false);
    bool any_type = false;
    for (int player = 0; player < kPadPlayers && told; ++player) {
        const int option = cemu_player_type_option(player);
        if (option < 0) continue;
        any_type = true;
        const ExternalOption& type = cemu_options[static_cast<std::size_t>(option)];
        const auto kept = game_config.wii_u_options.find(type.id);
        joinable[static_cast<std::size_t>(player)] =
            have_game_config && kept != game_config.wii_u_options.end() && kept->second >= 0 &&
            kept->second < static_cast<int>(type.choices.size()) &&
            (offered_type(type.choices[static_cast<std::size_t>(kept->second)]) ||
             type.choices[static_cast<std::size_t>(kept->second)] == "Classic Controller");
    }
    if (told && !any_type) joinable.clear();
    pad_order.joinable = joinable;
    if (!told) return;
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    pad_order = keeping_joinable(order_of(pads, line_up_pads(pads, pad_order, holds_gamepad()), holds_gamepad()));
}

void App::Impl::add_pad_as_player(const std::string& identity, int type) {
    const std::vector<KnownPad> pads = gather_pads(companion_joined());
    PadLineup lineup = line_up_pads(pads, pad_order, holds_gamepad());
    const int pad = pad_index(pads, identity);
    const int player = first_free_player(lineup);
    if (pad < 0 || !add_player(lineup, pad)) return;
    pad_order = keeping_joinable(order_of(pads, lineup, holds_gamepad()));
    // Its type first, so the game is not shown it as something else.
    set_cemu_option(cemu_player_type_option(player), type);
    log_info("controllers: " + pad_label(pads[static_cast<std::size_t>(pad)]) + " was added as " +
             pad_slot_name(PadSlot::player_number(player)));
}

void App::Impl::apply_wii_u_tv_audio(int previous) {
    if (settings.wii_u_tv_audio == previous) return;
    if (!external_source || !external_source->running() || !external_source->options_supported()) return;
    const int wanted = std::clamp(settings.wii_u_tv_audio, 0, 2);
    external_source->change_option(kWiiUTvAudioOption, wanted);
    log_info(std::string("Cemu: TV audio set to ") + kWiiUTvAudioChoices[wanted]);
    if (system == SystemId::WiiU && local_game()) show_corner_notice("TV audio will change when the game is reset.");
}

bool App::Impl::amiibo_available() const {
    return system == SystemId::WiiU && !is_companion() && external_source &&
           external_source->running() && external_source->amiibo_supported();
}

bool App::Impl::amiibo_in_menu() const {
    if (!amiibo_available()) return false;
    if (!external_source->amiibo_reading_supported()) return true;
    return amiibo_reading_going == ExternalAmiiboReading::Amiibo;
}

namespace {

// Where amiibo dumps are usually kept, for a first look before one has ever
// been chosen.
std::string first_amiibo_folder() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) return std::string();
    for (const char* candidate :
         {"amiibo", "Amiibo", "Documents/amiibo", "Documents/Amiibo", "Emulation/amiibo"}) {
        const std::string path = path_join(home, candidate);
        if (directory_exists(path)) return path;
    }
    return std::string();
}

}  // namespace

void App::Impl::scan_amiibo_folder() {
    amiibo_paths.clear();
    std::string folder = settings.amiibo_dir.empty() ? first_amiibo_folder() : settings.amiibo_dir;
    if (folder.empty()) return;
    // A folder of them, sorted into folders by series as collections often
    // are -- so a level or two down as well, and no further.
    std::vector<std::pair<std::string, int>> pending{{folder, 0}};
    while (!pending.empty() && amiibo_paths.size() < 1000) {
        const auto [directory, depth] = pending.back();
        pending.pop_back();
        std::vector<DirectoryEntry> entries;
        if (!list_directory(directory, entries, nullptr)) continue;
        for (const DirectoryEntry& entry : entries) {
            if (entry.is_directory) {
                if (depth < 2) pending.emplace_back(entry.path, depth + 1);
            } else if (is_amiibo_file(entry.name) && amiibo_paths.size() < 1000) {
                amiibo_paths.push_back(entry.path);
            }
        }
    }
    std::sort(amiibo_paths.begin(), amiibo_paths.end(), [](const std::string& a, const std::string& b) {
        std::string left = path_stem(a);
        std::string right = path_stem(b);
        for (char& c : left) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : right) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return left < right;
    });
}

void App::Impl::show_menu_notice(const std::string& message) {
    // The quick menu's own popup, put away with OK, on both devices: the
    // words travel as the save status does -- see SaveSlotsMessage.
    if (picker) {
        picker->set_save_status(message, true);
        picker->show_notice();
    }
    if (host_session) {
        net::SaveSlotsMessage slots;
        slots.slots.resize(kSavePointSlots);
        slots.status = message;
        slots.status_failed = true;
        host_session->set_save_slots(slots);
    }
}

std::optional<std::string> App::Impl::amiibo_unwanted_reason() const {
    if (!amiibo_reading_going) return std::nullopt;
    switch (*amiibo_reading_going) {
        case ExternalAmiiboReading::Nothing: return std::string("The game isn't reading amiibo right now.");
        case ExternalAmiiboReading::OwnFigures:
            return std::string("This game doesn't read amiibo, only its own figures.");
        case ExternalAmiiboReading::Amiibo: break;
    }
    return std::nullopt;
}

void App::Impl::choose_amiibo(const std::string& path) {
    // A file that is not one is said to be so in the menu, which stays up --
    // not found out after the game has gone on without it.
    std::vector<uint8_t> bytes;
    std::string error;
    const bool readable = read_file(path, bytes, &error);
    if (!readable || !is_amiibo_dump(bytes)) {
        log_warn("amiibo: " + path_basename(path) +
                 (readable ? " is not an amiibo" : " could not be read: " + error));
        if (state == State::Amiibo) close_cemu_page();
        show_menu_notice(readable ? path_stem(path) + " isn't an amiibo."
                                  : path_stem(path) + "'s file couldn't be opened.");
        return;
    }
    // And a game not looking for one is said to be so too, before it is
    // waited on -- the menu still up, so another can be chosen or it closed.
    if (const std::optional<std::string> reason = amiibo_unwanted_reason()) {
        log_warn("amiibo: " + path_basename(path) + " was not scanned: " + *reason);
        if (state == State::Amiibo) close_cemu_page();
        show_menu_notice(*reason);
        return;
    }
    // Held to the GamePad once the game is going again, so the menu goes
    // first -- all of it, as loading a save state does.
    pending_amiibo = path;
    pending_amiibo_at = std::chrono::steady_clock::now();
    amiibo_scanning = true;
    log_info("amiibo chosen: " + path_basename(path) + ", to scan once the game is going again");
    if (state == State::Amiibo) amiibo_browser.close();
    state = State::Menu;
    set_menu_open(false);
    skip_menu_leave();
}

void App::Impl::open_amiibo_browser() {
    enter_cemu_page(State::Amiibo);
    amiibo_browser.set_amiibo(true);
    const char* home = std::getenv("HOME");
    std::string folder = settings.amiibo_dir.empty() ? first_amiibo_folder() : settings.amiibo_dir;
    if (folder.empty()) folder = home != nullptr ? home : "/";
    amiibo_browser.open(folder);
    log_info("the amiibo browser is up, in " + amiibo_browser.current_directory());
}

void App::Impl::update_amiibo_browser(const std::vector<UiAction>& actions,
                                      const std::vector<PointerEvent>& pointers) {
    if (!amiibo_available()) {
        close_cemu_page();
        return;
    }
    const GameBrowserResult result = amiibo_browser.update(
        input, actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height));
    if (result.game_chosen) {
        // Its folder is the one the menu lists from now on.
        settings.amiibo_dir = path_dirname(result.chosen_path);
        if (!settings.save(paths)) log_warn("could not keep the amiibo folder");
        choose_amiibo(result.chosen_path);
        return;
    }
    if (result.back_requested) close_cemu_page();
}

// Steam's on-screen keyboard, up while a name is typed on a Steam Deck. SDL
// raises it itself when text input starts and it knows it is on a Deck --
// which it reads from the SteamDeck variable Steam sets -- so it is let to
// for the name; where it did not, Steam is asked for it by its URL, as SDL
// would have.
void App::Impl::show_steam_keyboard(bool typing) {
    static const bool on_deck = std::getenv("SteamDeck") != nullptr || std::getenv("SteamOS") != nullptr ||
                                net::this_device_kind() == net::kDeviceKindSteamDeck;
    if (!on_deck) return;
    if (typing) {
        SDL_SetHint("SDL_ENABLE_SCREEN_KEYBOARD", "1");
        input.set_text_entry(true);
        if (window != nullptr && SDL_IsScreenKeyboardShown(window)) {
            log_info("Steam's keyboard is up for typing");
            return;
        }
        if (SDL_OpenURL("steam://open/keyboard?Mode=0") == 0) {
            steam_keyboard_opened = true;
            log_info("Asked Steam for its keyboard for typing");
        } else {
            log_info(std::string("Steam's keyboard could not be opened: ") + SDL_GetError());
        }
        return;
    }
    if (steam_keyboard_opened) SDL_OpenURL("steam://close/keyboard");
    steam_keyboard_opened = false;
    input.set_text_entry(false);
    SDL_SetHint("SDL_ENABLE_SCREEN_KEYBOARD", "0");
}

void App::Impl::send_pending_amiibo() {
    if (!external_source || !external_source->running()) {
        pending_amiibo.clear();
        amiibo_scanning = false;
        return;
    }
    // Four seconds after the menu is down and the game going again: a game
    // coming back from its HOME Menu shows a loading icon of its own for a
    // while -- Breath of the Wild does, for about that long -- and does not
    // see a figure held to the GamePad before it is back. Or after twelve
    // seconds regardless, rather than never, should the game not say it is.
    const auto now = std::chrono::steady_clock::now();
    const bool going = state == State::Playing && !wii_u_paused && !menu_waiting_for_pause;
    if (!external_source->amiibo_reading_supported()) {
        amiibo_reading_going.reset();
    } else if (!wii_u_paused && !menu_waiting_for_pause) {
        amiibo_reading_going = external_source->amiibo_reading();
    }
    if (pending_amiibo.empty() || !going) {
        amiibo_game_going_since.reset();
    } else if (!amiibo_game_going_since) {
        amiibo_game_going_since = now;
    }
    const bool settled = amiibo_game_going_since && now - *amiibo_game_going_since >= std::chrono::seconds(4);
    if (!pending_amiibo.empty() && (settled || now - pending_amiibo_at > std::chrono::seconds(12))) {
        amiibo_game_going_since.reset();
        // Asked again, now it is time: a game can stop looking in the
        // seconds the menu took to go.
        if (const std::optional<std::string> reason = amiibo_unwanted_reason()) {
            log_warn("amiibo: " + path_basename(pending_amiibo) + " was not scanned: " + *reason);
            show_corner_notice(*reason);
            pending_amiibo.clear();
            amiibo_scanning = false;
            return;
        }
        external_source->scan_amiibo(pending_amiibo);
        amiibo_sent_at = now;
        log_info("amiibo: holding " + path_basename(pending_amiibo) + " to the GamePad");
        pending_amiibo_name = path_stem(pending_amiibo);
        pending_amiibo.clear();
    }
    // Sent, and no word back from Cemu: not "Scanning..." for ever.
    if (amiibo_scanning && pending_amiibo.empty() && now - amiibo_sent_at > std::chrono::seconds(5)) {
        amiibo_scanning = false;
    }
    ExternalAmiiboResult result = ExternalAmiiboResult::Scanned;
    while (external_source->take_amiibo_result(result)) {
        amiibo_scanning = false;
        switch (result) {
            // The game says so itself.
            case ExternalAmiiboResult::Scanned: log_info("amiibo: scanned"); break;
            case ExternalAmiiboResult::CannotOpen:
                log_warn("amiibo: Cemu could not open the file");
                show_corner_notice("That amiibo's file couldn't be opened.");
                break;
            case ExternalAmiiboResult::NotAnAmiibo:
                log_warn("amiibo: Cemu says the file is not an amiibo");
                show_corner_notice("That file isn't an amiibo.");
                break;
            case ExternalAmiiboResult::NotReading:
                log_warn("amiibo: the game isn't reading amiibo right now");
                show_corner_notice("The game isn't reading amiibo right now.");
                break;
            case ExternalAmiiboResult::NotItsKind:
                log_warn("amiibo: the game reads NFC figures, but not amiibo");
                show_corner_notice("This game doesn't read amiibo, only its own figures.");
                break;
            case ExternalAmiiboResult::NoGame:
                log_warn("amiibo: no game was running to scan it in");
                break;
        }
    }
}

void App::Impl::close_cemu_page() {
    if (state == State::GraphicPacks) {
        graphic_packs_screen.close();
        log_info("the graphics packs page is down");
    } else if (state == State::Amiibo) {
        amiibo_browser.close();
        log_info("the amiibo browser is down");
    } else if (state == State::ButtonMapping) {
        button_mapping_screen.close();
        log_info("the button mapping page is down");
    } else if (state == State::WiiUPlayer) {
        wii_u_player_screen.close();
        detect_pressing.clear();
        log_info("the player's page is down");
    }
    if (cemu_page_came_from == State::Menu && local_game()) {
        set_menu_open(true);
    } else if (local_game()) {
        state = State::Playing;
        input.set_game_input_enabled(true);
    } else {
        open_launcher();
    }
}

void App::Impl::update_graphic_packs_screen(const std::vector<UiAction>& actions,
                                            const std::vector<PointerEvent>& pointers) {
    // The game went while the page was up: nothing left to change.
    if (!graphic_packs_available()) {
        close_cemu_page();
        return;
    }
    std::vector<ExternalGraphicPack> packs;
    if (external_source->take_graphic_packs(packs)) {
        graphic_packs_screen.set_packs(std::move(packs));
    }
    apply_graphic_packs_result(graphic_packs_screen.update(
        actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height)));
}

MenuPage* App::Impl::page_shown() {
    switch (state) {
        case State::Menu: return picker.get();
        case State::GraphicPacks: return &graphic_packs_screen;
        case State::Controllers: return &controllers_screen;
        case State::Amiibo: return &amiibo_browser;
        case State::ButtonMapping: return &button_mapping_screen;
        case State::WiiUPlayer: return &wii_u_player_screen;
        default: return nullptr;
    }
}

net::MirroredPage App::Impl::mirrored_page() const {
    // Every state named, and no default: one added later is a warning here
    // until it says whether the other device shows it.
    switch (state) {
        case State::Menu: return picker ? net::MirroredPage::QuickMenu : net::MirroredPage::None;
        case State::GraphicPacks: return net::MirroredPage::GraphicPacks;
        case State::Controllers: return net::MirroredPage::Controllers;
        case State::Amiibo: return net::MirroredPage::AmiiboBrowser;
        case State::ButtonMapping: return net::MirroredPage::ButtonMapping;
        case State::WiiUPlayer: return net::MirroredPage::WiiUPlayer;
        // Settings and its pages are this device's own; the rest are not
        // over a game the two share, or have nothing that scrolls.
        case State::Settings:
        case State::DataManagement:
        case State::Playing:
        case State::Message:
        case State::Browser:
        case State::Launcher:
        case State::HostPicker:
        case State::Companion:
        case State::ResumeNotice:
        case State::Forwarder:
        case State::GameDialog: return net::MirroredPage::None;
    }
    return net::MirroredPage::None;
}

void App::Impl::sync_companion_scroll() {
    if (!companion_session || !is_companion()) return;
    MenuPage* page = nullptr;
    net::MirroredPage shown = net::MirroredPage::None;
    if (state == State::Menu && picker) {
        page = picker.get();
        shown = net::MirroredPage::QuickMenu;
    } else if (state == State::Companion && companion_session->host_settings_open()) {
        if (companion_session->host_controllers_view().open) {
            page = &mirrored_controllers;
            shown = net::MirroredPage::Controllers;
        } else if (companion_session->host_amiibo_browser_view().open) {
            page = &mirrored_amiibo;
            shown = net::MirroredPage::AmiiboBrowser;
        } else if (companion_session->host_graphic_packs_view().open) {
            page = &mirrored_graphic_packs;
            shown = net::MirroredPage::GraphicPacks;
        } else if (companion_session->host_button_mapping_view().open) {
            page = &mirrored_button_mapping;
            shown = net::MirroredPage::ButtonMapping;
        } else if (companion_session->host_wii_u_player_view().open) {
            page = &mirrored_wii_u_player;
            shown = net::MirroredPage::WiiUPlayer;
        }
    }
    if (const std::optional<net::MenuScrollMessage> said =
            companion_scroll_sync.sync(page, shown, &companion_session->host_menu_scroll(),
                                       static_cast<float>(output_width), static_cast<float>(output_height))) {
        companion_session->send_menu_scroll(*said);
    }
    net::MenuCursorMessage host_cursor;
    const bool cursor_arrived = companion_session->take_host_menu_cursor(host_cursor);
    if (const std::optional<net::MenuCursorMessage> said =
            companion_cursor_sync.sync(page, shown, cursor_arrived ? &host_cursor : nullptr,
                                       static_cast<float>(output_width), static_cast<float>(output_height))) {
        companion_session->send_menu_cursor(*said);
    }
}

void App::Impl::publish_mirrored_pages() {
    if (!host_session) return;
    net::AmiiboBrowserViewMessage amiibo;
    if (state == State::Amiibo && amiibo_browser.is_open()) {
        amiibo.open = true;
        amiibo.directory = amiibo_browser.current_directory();
        for (const GameBrowserEntry& entry : amiibo_browser.entries()) {
            amiibo.entries.push_back(net::AmiiboBrowserViewEntry{entry.label, entry.is_directory, entry.is_parent});
        }
        amiibo.focus = amiibo_browser.focus();
    }
    host_session->set_amiibo_browser_view(amiibo);
    if (!menu_scroll_sync) menu_scroll_sync = std::make_unique<MenuScrollSync>(/*host=*/true);
    net::MenuScrollMessage theirs;
    const bool fresh = host_session->take_menu_scroll(theirs);
    if (const std::optional<net::MenuScrollMessage> said =
            menu_scroll_sync->sync(page_shown(), mirrored_page(), fresh ? &theirs : nullptr,
                                   static_cast<float>(output_width), static_cast<float>(output_height))) {
        host_session->set_menu_scroll(*said);
    }
    net::MenuCursorMessage companion_cursor;
    const bool cursor_arrived = host_session->take_menu_cursor(companion_cursor);
    if (const std::optional<net::MenuCursorMessage> said =
            host_cursor_sync.sync(page_shown(), mirrored_page(), cursor_arrived ? &companion_cursor : nullptr,
                                  static_cast<float>(output_width), static_cast<float>(output_height))) {
        host_session->set_menu_cursor(*said);
    }
}

void App::Impl::update_shown_page(const std::vector<UiAction>& actions) {
    switch (state) {
        case State::Menu:
            if (picker) {
                apply_menu_result(picker->update(actions, {}, static_cast<float>(output_width),
                                                 static_cast<float>(output_height)),
                                  /*pressed_here=*/false);
            }
            break;
        case State::GraphicPacks: update_graphic_packs_screen(actions, {}); break;
        case State::Controllers: update_controllers_screen(actions, {}); break;
        case State::Amiibo: update_amiibo_browser(actions, {}); break;
        case State::ButtonMapping: update_button_mapping_screen(actions, {}); break;
        case State::WiiUPlayer: update_wii_u_player_screen(actions, {}); break;
        default: break;
    }
}

void App::Impl::apply_graphic_packs_result(const GraphicPacksScreenResult& result) {
    if (result.change) {
        const ExternalGraphicPackChange& change = *result.change;
        external_source->change_graphic_pack(change);
        std::string name = change.id;
        for (const ExternalGraphicPack& pack : graphic_packs_screen.packs()) {
            if (pack.id == change.id) name = pack.name;
        }
        if (change.kind == ExternalGraphicPackChange::Kind::Enabled) {
            log_info("graphics packs: " + name + (change.enabled ? " turned on" : " turned off"));
        } else {
            log_info("graphics packs: " + name + ", " +
                     (change.category.empty() ? std::string() : change.category + " ") + change.preset);
        }
    }
    if (result.close_requested) close_cemu_page();
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

bool App::Impl::save_sync_available() const {
    // A 3DS game's too: Azahar keeps its save in a folder it reads when the
    // game starts, the same as a DS game's .srm. Not a Wii U game's -- Cemu
    // is another program, and is not restarted from here.
    return save_store && local_game() && source && !game_path.empty() &&
           (system == SystemId::Ds || system == SystemId::ThreeDs);
}

bool App::Impl::save_points_available() const {
    return source && !game_path.empty() && system_profile(system).offers_save_states &&
           source->supports_save_states();
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
        // Said with a chime, as Nintendo's own classics say it, and the
        // slot's new picture: nothing to put away.
        refresh_save_points();
        ++save_states_made;
        return;
    }
    log_warn(name + " could not be saved: " + error);
    refresh_save_points(error, true);
    // Said in a popup, put away with OK, rather than a line in the corner
    // that was easy to miss.
    if (picker) picker->show_notice();
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
    if (picker) picker->show_notice();
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

bool App::Impl::menu_key_back_to_game(const std::vector<UiAction>& actions) {
    if (!local_game() || is_companion()) return false;
    if (state != State::Settings && state != State::Controllers && state != State::DataManagement &&
        !on_cemu_page()) {
        return false;
    }
    if (std::find(actions.begin(), actions.end(), UiAction::ToggleMenu) == actions.end()) {
        return false;
    }
    if (state == State::Settings) settings_screen->close();
    if (state == State::Controllers) controllers_screen.close();
    if (state == State::GraphicPacks) graphic_packs_screen.close();
    if (state == State::Amiibo) amiibo_browser.close();
    if (state == State::ButtonMapping) button_mapping_screen.close();
    if (state == State::WiiUPlayer) wii_u_player_screen.close();
    if (state == State::DataManagement && data_screen) data_screen->close();
    data_from_settings = false;
    log_info("the menu button: every page put away, back to the game");
    // Closed the way the menu itself is, so that everything closing it does
    // -- the game running again, the other device told -- happens here too.
    state = State::Menu;
    set_menu_open(false);
    skip_menu_leave();
    return true;
}

bool App::Impl::menu_over_wii_u_game() const {
    const bool page_up = state == State::Menu || state == State::Message || state == State::GameDialog ||
                         ((state == State::Settings || state == State::DataManagement) &&
                          settings_came_from == State::Menu) ||
                         (state == State::Controllers && controllers_came_from == State::Menu) ||
                         (on_cemu_page() && cemu_page_came_from == State::Menu);
    return page_up && system == SystemId::WiiU && !is_companion() && external_source &&
           external_source->running() && external_source->overlay_supported() &&
           MenuOverlay::supported();
}

bool App::Impl::wants_menu_overlay() const {
    if (menu_over_wii_u_game()) return true;
    // Going away over the game, it is drawn there until it has.
    if (quick_menu_leaving() && system == SystemId::WiiU && !is_companion() && external_source &&
        external_source->running() && external_source->overlay_supported() && MenuOverlay::supported()) {
        return true;
    }
    return !shown_corner_notice().empty() && system == SystemId::WiiU && !is_companion() && external_source &&
           external_source->running() && external_source->overlay_supported() &&
           MenuOverlay::supported();
}

bool App::Impl::corner_notice_up() const {
    return !corner_notice.empty() && std::chrono::steady_clock::now() < corner_notice_until;
}

std::string App::Impl::shown_corner_notice() const {
    if (corner_notice_up()) return corner_notice;
    if (amiibo_scanning && state == State::Playing) return "Scanning\u2026";
    return std::string();
}

void App::Impl::show_corner_notice(const std::string& message) {
    corner_notice = message;
    corner_notice_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
}

bool App::Impl::pauses_wii_u_for_menu() const {
    return settings.pause_wii_u_in_menu && system == SystemId::WiiU && !is_companion() &&
           external_source && external_source->running() && external_source->pause_supported();
}

void App::Impl::update_wii_u_pause() {
    if (!external_source || !external_source->running()) {
        wii_u_paused = false;
        menu_waiting_for_pause = false;
        pending_game_dialog.reset();
        leave_game_dialog();
        update_cemu_options();
        return;
    }
    bool refused = false;
    bool not_listening = false;
    ExternalPauseState said = ExternalPauseState::Running;
    while (external_source->take_pause_state(said)) {
        switch (said) {
            case ExternalPauseState::Paused:
                wii_u_paused = true;
                for (const ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
                    const int index = screen == ScreenId::Top ? 0 : 1;
                    wii_u_still_after[index] = external_source->screen_serial(screen);
                    wii_u_still_ready[index] = false;
                }
                wii_u_paused_at = std::chrono::steady_clock::now();
                menu_backdrop.forget();
                log_info("the game paused");
                break;
            case ExternalPauseState::Running:
                if (wii_u_paused) log_info("the game is going again");
                wii_u_paused = false;
                wii_u_still_ready[0] = wii_u_still_ready[1] = false;
                menu_backdrop.forget();
                break;
            case ExternalPauseState::Refused:
                wii_u_paused = false;
                refused = true;
                break;
            case ExternalPauseState::NotListening:
                wii_u_paused = false;
                not_listening = true;
                break;
        }
    }
    // A screen already being sent here is already its paused picture.
    for (const ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
        const int index = screen == ScreenId::Top ? 0 : 1;
        if (wii_u_paused && !wii_u_still_ready[index] &&
            (external_source->screen_serial(screen) != wii_u_still_after[index] ||
             external_requested_screen == screen)) {
            wii_u_still_ready[index] = true;
            log_info(std::string("the paused game's ") +
                     (screen == ScreenId::Top ? "television" : "GamePad") +
                     " picture arrived, to blur behind the menu");
        }
    }
    if (menu_waiting_for_pause) {
        if (wii_u_paused) {
            // And for its picture, a moment behind: a menu opened before it
            // had only the darkening a game going on gets, nearly black, and
            // then the blur -- a flash, every time the picture was late.
            // Never for long, though: a picture that does not come leaves
            // the darkening, as before.
            if (wii_u_still_ready[wii_u_screen_shown() == ScreenId::Top ? 0 : 1] ||
                std::chrono::steady_clock::now() - wii_u_paused_at >
                    std::chrono::milliseconds(500)) {
                menu_waiting_for_pause = false;
                open_quick_menu();
            }
        } else if (refused) {
            // As the console does while a game has its HOME Menu turned off,
            // which games do while saving: no menu, and a word to say so.
            menu_waiting_for_pause = false;
            log_info("the game can't be paused right now, so the quick menu stays closed");
            show_corner_notice(kMenuUnavailableNotice);
        } else if (not_listening) {
            // A game that never listens for the HOME Menu cannot be asked,
            // and would otherwise never have a menu at all -- no Quit, no
            // Settings -- so it gets one over it as it plays, at once.
            menu_waiting_for_pause = false;
            log_info("the game doesn't listen for the HOME Menu, so the quick menu opens over it "
                     "as it plays");
            open_quick_menu();
        } else if (std::chrono::steady_clock::now() - menu_pause_asked_at >
                   std::chrono::seconds(2)) {
            // One that listens but does not answer -- stuck loading, say --
            // the same, once it has had its chance. The request stands while
            // the menu is up, so a game that answers late still pauses under
            // it.
            menu_waiting_for_pause = false;
            log_info("the game didn't pause in time, so the quick menu opens over it as it plays");
            open_quick_menu();
        }
    }
    const bool page_over_game =
        state == State::Menu || state == State::Settings || state == State::Controllers ||
        state == State::DataManagement || state == State::Message || on_cemu_page();
    // Still paused while the menu goes away, as a Switch's HOME Menu does:
    // a game going on under it came back mid-way and swapped the blur
    // behind it for the plain darkening -- a flash as the menu closed.
    external_source->request_pause(pauses_wii_u_for_menu() &&
                                   (menu_waiting_for_pause || page_over_game || quick_menu_leaving()));
    send_pending_amiibo();
    update_game_dialogs();
    update_cemu_options();
}

void App::Impl::update_game_dialogs() {
    if (!external_source->game_dialogs_supported()) return;
    external_source->want_game_dialogs(true);
    ExternalGameDialog dialog;
    while (external_source->take_game_dialog(dialog)) {
        if (dialog.kind != ExternalGameDialogKind::None) {
            if (state == State::GameDialog && game_dialog_screen.serial() == dialog.serial) {
                game_dialog_screen.refresh(dialog);
            } else {
                pending_game_dialog = dialog;
            }
            continue;
        }
        // Put away by the game.
        if (pending_game_dialog && pending_game_dialog->serial == dialog.serial) pending_game_dialog.reset();
        if (state == State::GameDialog && game_dialog_screen.serial() == dialog.serial) leave_game_dialog();
    }
    // Answered on the companion, or typed there as it goes.
    if (host_session) {
        net::GameDialogAnswerMessage answer;
        while (host_session->take_game_dialog_answer(answer)) {
            if (state != State::GameDialog || answer.serial != game_dialog_screen.serial()) continue;
            if (!answer.final) {
                ExternalGameDialog typed = game_dialog_screen.current();
                typed.text = answer.text;
                game_dialog_screen.refresh(typed);
                continue;
            }
            ExternalGameDialogAnswer given;
            given.serial = answer.serial;
            given.button = answer.button;
            given.text = answer.text;
            external_source->answer_game_dialog(given);
            log_info("the game's dialog was answered on " + host_session->companion_name());
            leave_game_dialog();
        }
    }
    // Shown once nothing else is over the game: a menu up first stays up.
    if (pending_game_dialog && state == State::Playing) {
        game_dialog_screen.show(*pending_game_dialog);
        log_info(pending_game_dialog->kind == ExternalGameDialogKind::Keyboard
                     ? "the game wants something typed"
                     : "the game has a message: " + pending_game_dialog->title + " " + pending_game_dialog->text);
        pending_game_dialog.reset();
        state = State::GameDialog;
        input.set_game_input_enabled(false);
    }
}

void App::Impl::leave_game_dialog() {
    game_dialog_screen.close();
    if (state != State::GameDialog) return;
    state = State::Playing;
    input.set_game_input_enabled(true);
}

void App::Impl::publish_game_dialog() {
    if (!host_session) return;
    net::GameDialogMessage message;
    if (state == State::GameDialog && game_dialog_screen.is_open()) {
        const ExternalGameDialog dialog = game_dialog_screen.current();
        message.kind = static_cast<uint8_t>(dialog.kind);
        message.serial = dialog.serial;
        message.title = dialog.title;
        message.text = dialog.text;
        message.buttons = dialog.buttons;
        message.max_length = dialog.max_length;
        message.typed_here = companion_types_game_dialog();
    }
    host_session->set_game_dialog(message);
}

bool App::Impl::wii_u_backdrop_ready() {
    const ScreenId shown = wii_u_screen_shown();
    const int index = shown == ScreenId::Top ? 0 : 1;
    if (!wii_u_paused || !wii_u_still_ready[index] || !external_source) return false;
    // Keyed to the screen as well as its picture, so swapping the screens
    // makes it again from the other one.
    const std::size_t key =
        static_cast<std::size_t>(external_source->screen_serial(shown)) * 2u + index + 1u;
    if (!menu_backdrop.matches(key)) {
        ScreenReadback still;
        if (!external_source->read_screen(shown, still)) return false;
        menu_backdrop.capture_image(still.pixels, still.width, still.height, still.stride_pixels,
                                    still.channel_order == ChannelOrder::Rgba, output_width,
                                    output_height, key);
    }
    return menu_backdrop.matches(key);
}

void App::Impl::render_menu_overlay() {
    if (!wants_menu_overlay()) {
        if (menu_overlay_shown) {
            if (external_source) external_source->hide_overlay();
            menu_overlay_shown = false;
            menu_overlay.forget();
            log_info("nothing of this app's is over the game any more");
        }
        return;
    }
    int width = 0;
    int height = 0;
    MenuOverlay::fit(output_width, output_height, ExternalOverlayArea::kMaxWidth,
                     ExternalOverlayArea::kMaxHeight, width, height);
    const float w = static_cast<float>(output_width);
    const float h = static_cast<float>(output_height);
    const bool menu_over = menu_over_wii_u_game() || quick_menu_leaving();
    const auto draw_backdrop = [&] {
        if (wii_u_backdrop_ready()) {
            // Paused: the game's picture blurred, as a DS or 3DS game's
            // is, over all of Cemu's window, and a page opened from the
            // menu 60% darker again.
            menu_backdrop.draw(quads, w, h);
            if (state == State::Settings || state == State::DataManagement ||
                state == State::Controllers || on_cemu_page()) {
                quads.draw_solid(PixelRect{0.0f, 0.0f, w, h}, Color{0.0f, 0.0f, 0.0f, 0.6f});
            }
        } else {
            // The game goes on underneath, Cemu's to draw: darkened, as
            // it is on every page in this window -- see kLiveGameDim.
            quads.draw_solid(PixelRect{0.0f, 0.0f, w, h}, Color{0.0f, 0.0f, 0.0f, kLiveGameDim});
        }
    };
    const auto draw_menu = [&] { render_pages(w, h, /*over_game=*/true, nullptr); };
    // Laid out at this window's own size and drawn into the smaller target:
    // pages keep their scroll in this window's pixels, and a copy laid out
    // at another size scrolled too far for it -- the cursor off its list, and
    // empty space under Settings.
    const auto begin_picture = [&] {
        if (!menu_overlay.begin(width, height)) return false;
        quads.begin_frame(output_width, output_height,
                          static_cast<float>(width) / static_cast<float>(std::max(1, output_width)));
        quads.clear(Color{0.0f, 0.0f, 0.0f, 0.0f});
        // Alpha accumulates as coverage rather than being blended like a
        // colour, so what is left is how much of the game each pixel hides.
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        return true;
    };
    const auto cannot_draw = [&] {
        if (!menu_overlay_logged) {
            log_warn("could not draw the menu for Cemu's window; it is only in this one");
            menu_overlay_logged = true;
        }
    };

    // The menu coming or going, where Cemu animates it itself: what is
    // behind the menu and the menu, each once, and then nothing until it
    // has come or gone. A picture a frame could not be shown there any
    // faster than Cemu makes textures of them, which is seldom.
    const bool coming_or_going = menu_entrance.animating() || menu_entrance.leaving();
    if (menu_over && coming_or_going && external_source->overlay_animation_supported()) {
        if (menu_entrance.started() != overlay_animation_sent) {
            overlay_animation_sent = menu_entrance.started();
            if (!begin_picture()) return cannot_draw();
            draw_backdrop();
            menu_overlay.finish();
            const std::vector<uint8_t> behind = menu_overlay.pixels();
            if (!begin_picture()) return cannot_draw();
            draw_menu();
            menu_overlay.finish();
            gl::reset_render_state();
            const bool takes_input = menu_over_wii_u_game();
            if (external_source->publish_overlay_animation(
                    quick_menu_leaving() ? ExternalOverlayAnimation::Mode::Leaving
                                         : ExternalOverlayAnimation::Mode::Entering,
                    behind.data(), menu_overlay.pixels().data(), menu_overlay.width(), menu_overlay.height(),
                    takes_input)) {
                if (!menu_overlay_shown) log_info("the menu is coming up over the game in Cemu's window");
                menu_overlay_shown = true;
                menu_overlay_takes_input = takes_input;
            }
            // The still picture after it is sent whatever it looks like:
            // Cemu shows the two until it comes.
            menu_overlay.forget();
        }
        return;
    }

    if (!begin_picture()) return cannot_draw();
    if (menu_over) draw_entering(w, h, draw_backdrop, draw_menu);
    const std::string notice = shown_corner_notice();
    if (!notice.empty()) render_corner_notice(quads, text, w, h, notice);
    const bool changed = menu_overlay.finish();
    gl::reset_render_state();
    // A notice alone, the game running under it and refused the pause, keeps
    // the pads and keys going to the game, as the Switch's own does.
    const bool takes_input = menu_over_wii_u_game();
    if (changed || !menu_overlay_shown || takes_input != menu_overlay_takes_input) {
        if (external_source->publish_overlay(menu_overlay.pixels().data(), menu_overlay.width(),
                                             menu_overlay.height(), takes_input)) {
            if (!menu_overlay_shown) {
                log_info(takes_input ? "the menu is up over the game in Cemu's window"
                                     : "a notice is up over the game in Cemu's window");
            }
            menu_overlay_shown = true;
            menu_overlay_takes_input = takes_input;
        }
    }
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
        // The popup is answered first: it is what the game is waiting on.
        if (state == State::ResumeNotice) return;
        // A Wii U game is asked to pause first, as for the console's HOME
        // Menu, and the menu opens once it has -- see update_wii_u_pause().
        if (pauses_wii_u_for_menu() && !wii_u_paused) {
            if (!menu_waiting_for_pause) {
                menu_waiting_for_pause = true;
                menu_pause_asked_at = std::chrono::steady_clock::now();
                log_info("asking the game to pause for the quick menu");
                update_wii_u_pause();
            }
            return;
        }
        open_quick_menu();
        return;
    }
    if (state != State::Menu) return;
    log_info("the quick menu is down");
    // Back to the game, it goes away over it, and is put away once it has --
    // see settle_menu_leave(). On a companion following the host into a page
    // of the menu, it is simply gone: the page is on the host.
    if (is_companion() && companion_session && companion_session->host_settings_open()) {
        picker->close();
        menu_entrance.finish();
    } else {
        menu_entrance.leave();
        menu_closing = true;
        // A close pressed in the menu has already closed it there: it is
        // still drawn, as it was, until it has gone -- and in the meantime
        // nothing else (the list beside a Wii U game) is opened in its place.
        if (picker) picker->linger();
    }
    if (is_companion()) {
        companion_session->set_menu_open(false);
        state = State::Companion;
    } else {
        state = State::Playing;
    }
    input.set_game_input_enabled(true);
}

void App::Impl::open_quick_menu() {
    if (state == State::Menu || state == State::ResumeNotice) return;
    // Coming up from the game -- or back from one of its pages, when it is
    // simply there again.
    const bool from_game =
        state == State::Playing || (state == State::Companion && !companion_saw_host_page);
    // A notice that the menu could not open is over once it has.
    corner_notice.clear();
    open_picker(/*embedded=*/false, from_game);
    state = State::Menu;
    menu_closing = false;
    if (from_game) {
        menu_entrance.start();
    } else {
        menu_entrance.finish();
    }
    log_info("the quick menu is up");
    // A menu is often the last thing opened before a game is put down,
    // whichever device opened it.
    if (!is_companion()) flush_saves("the quick menu opened");
    input.set_game_input_enabled(false);
    if (is_companion()) companion_session->set_menu_open(true);
}

void App::Impl::apply_menu_result(const LayoutPickerResult& result, bool pressed_here) {
            apply_controls_result(result);
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
            // Each changed by however many steps its popup chose -- see
            // LayoutPickerResult::option_steps -- in the order it steps.
            const int steps = std::max(1, result.option_steps);
            if (result.render_scale_changed) {
                // Auto, then each scale: as the menu lists them.
                const int choices = max_render_scale() + 1;
                request_render_scale((std::clamp(render_scale, 0, choices - 1) + steps) % choices);
            }
            if (result.audio_target_changed) {
                AudioTarget target = audio_target;
                for (int step = 0; step < steps; ++step) target = next_audio_target(target);
                request_audio_target(target);
            }
            if (result.handedness_changed) {
                request_handedness(other_hand(current_handedness()));
            }
            if (result.screens_swapped) {
                swap_separate_screens();
            }
            if (result.screen_gap_toggled) {
                request_screen_gap(!screen_gap);
            }
            if (result.right_stick_mode_changed) {
                RightStickMode mode = right_stick_mode;
                const bool has_stick = system_profile(system).has_analog_stick;
                for (int step = 0; step < steps; ++step) mode = next_right_stick_mode(mode, has_stick);
                request_right_stick_mode(mode);
            }
            // One of Cemu's, which only the device running it can change; a
            // companion's press reaches the host's menu and lands here there.
            if (result.producer_option >= 0 && !is_companion()) {
                step_cemu_option(result.producer_option, steps);
            }
            if (result.lcd_filter_toggled) {
                request_lcd_filter(!lcd_filter_in_effect());
            }
            if (result.rumble_pak_toggled) {
                request_rumble_pak(!rumble_pak_in_effect());
            }
            if (result.custom_textures_toggled) {
                request_custom_textures(!custom_textures_in_effect());
            }
            if (result.resume_toggled) {
                request_resume(!resume_in_effect());
            }
            if (result.screens_swap_toggled) {
                // Alone, this game's screen; with a second device, the two
                // trading theirs -- as Separate Screens does on a DS.
                if (paired()) {
                    swap_separate_screens();
                } else {
                    toggle_solo_screen_swap();
                }
            }
            if (result.cursor_mode_changed) {
                // This device's own pointer, so it is decided here and not
                // asked of the host -- the other device has its own.
                settings.cursor_mode = next_cursor_mode(settings.cursor_mode);
                picker->set_cursor_mode(settings.cursor_mode);
                settings.save(paths);
                log_info(settings.cursor_mode == CursorMode::Hidden ? "the cursor will be hidden in games"
                                                                    : "the cursor will show in games");
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
                    // And this app with it: quitting from here means quitting,
                    // not going back to the main menu.
                    running = false;
                    quit_chosen = true;
                } else {
                    running = false;
                    quit_chosen = true;
                    quit_from_companion = !pressed_here;
                }
            } else if (result.controllers_requested) {
                // The host's own screen, the same as Settings below: only it
                // can see every controller on both devices.
                open_controllers();
            } else if (result.graphic_packs_requested) {
                // Cemu's, on this machine, like Settings: the other device is
                // told to look over here.
                if (graphic_packs_available()) open_graphic_packs();
            } else if (result.button_mapping_requested) {
                // The game's, kept on the device running it, the same way.
                if (button_mapping_available()) open_button_mapping();
            } else if (result.controller_opened >= 0) {
                // A player's page, on the device that can see every
                // controller, the same way.
                const std::vector<ControllerRowView>& rows = picker->state().wii_u_controllers;
                if (!is_companion() && local_game() &&
                    result.controller_opened < static_cast<int>(rows.size())) {
                    open_wii_u_player(rows[static_cast<std::size_t>(result.controller_opened)].slot);
                }
            } else if (result.controller_adding >= 0) {
                // A controller not used: the first player with no
                // controller, to add it as, the same way.
                const std::vector<ControllerRowView>& rows = picker->state().wii_u_controllers;
                if (!is_companion() && local_game() &&
                    result.controller_adding < static_cast<int>(rows.size())) {
                    const std::vector<KnownPad> pads = gather_pads(companion_joined());
                    const int player = first_free_player(line_up_pads(pads, pad_order, holds_gamepad()));
                    if (player < kWiiUPlayers) {
                        open_wii_u_player(PadSlot::player_number(player), true,
                                          rows[static_cast<std::size_t>(result.controller_adding)].pad);
                    }
                }
            } else if (result.amiibo_chosen >= 0) {
                // Chosen from the list, from either device: the host's files.
                if (!is_companion() && amiibo_available() &&
                    result.amiibo_chosen < static_cast<int>(amiibo_paths.size())) {
                    choose_amiibo(amiibo_paths[static_cast<std::size_t>(result.amiibo_chosen)]);
                }
            } else if (result.amiibo_folder_requested) {
                if (amiibo_available()) open_amiibo_browser();
            } else if (result.save_sync_requested) {
                // The game's own page, on the device whose save it is --
                // like Settings, whichever pad asked.
                if (!is_companion() && save_sync_available()) open_data_management();
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

void App::Impl::open_picker(bool embedded, bool from_game) {
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
    picker_state.screens_swappable = system_profile(system).solo_screens == SoloScreens::One;
    picker_state.screens_swapped = solo_screen_swapped;
    picker_state.screens_swap_value_shown = !paired();
    picker_state.graphic_packs_available = is_companion()
                                               ? companion_session->host_graphic_packs_available()
                                               : graphic_packs_available();
    picker_state.amiibo_available =
        is_companion() ? companion_session->host_amiibo_available()
                       // In place of the game, the menu is up while it plays
                       // on, so it is always offered there.
                       : (embedded ? amiibo_available() : amiibo_in_menu());
    // Asked for by the game itself, rather than only offered.
    picker_state.amiibo_wanted = from_game && !embedded && picker_state.amiibo_available && !is_companion() &&
                                 external_source && external_source->amiibo_reading_supported();
    if (is_companion()) {
        picker_state.producer_options = companion_session->host_producer_options();
        picker_state.producer_choices = companion_session->host_producer_choices();
        picker_state.producer_controls_from = companion_session->host_producer_controls_from();
        picker_state.wii_u_controllers = companion_session->host_wii_u_controllers();
        picker_state.amiibo = companion_session->host_amiibo();
        picker_state.amiibo_folder = companion_session->host_amiibo_folder();
    } else if (system == SystemId::WiiU) {
        picker_state.producer_options = cemu_option_rows();
        picker_state.producer_choices = cemu_option_choices();
        picker_state.producer_controls_from = cemu_controls_from();
        picker_state.wii_u_controllers = wii_u_controller_rows();
        // Looked for afresh each time the menu opens, so a file added since
        // is there.
        if (picker_state.amiibo_available) scan_amiibo_folder();
        for (const std::string& path : amiibo_paths) picker_state.amiibo.push_back(path_stem(path));
        picker_state.amiibo_folder =
            settings.amiibo_dir.empty() ? first_amiibo_folder() : settings.amiibo_dir;
    }
    picker_state.left_handed_layouts = left_handed_layouts;
    picker_state.render_scale = render_scale;
    // From the console being played, on both devices: a companion is shown the
    // same menu as the host, and `system` follows the host while it is
    // connected. Outside a game there is nothing to scale.
    picker_state.max_render_scale =
        is_companion() ? companion_session->host_max_render_scale() : max_render_scale();
    picker_state.screen_gap = screen_gap;
    picker_state.right_stick_mode = right_stick_mode;
    // Only a console whose right stick can mean more than one thing has a
    // question worth offering -- a 3DS. A DS has no right stick to speak of,
    // and a Wii U's is always just itself.
    picker_state.right_stick_choosable =
        is_companion() ? companion_session->host_right_stick_choosable()
                       : (local_game() && system_profile(system).offers_right_stick_mode);
    picker_state.right_stick_modes = right_stick_modes(system_profile(system).has_analog_stick);
    // Every DS and 3DS game has one; a Wii U's buttons are Cemu's, player by
    // player.
    picker_state.button_mapping_available = system != SystemId::WiiU && (is_companion() || local_game());
    // A Rumble Pak, where the core running this game can put one in.
    picker_state.rumble_pak = rumble_pak_in_effect();
    picker_state.lcd_filter = lcd_filter_in_effect();
    picker_state.lcd_filter_choosable =
        is_companion() ? companion_session->host_lcd_filter_choosable() : system == SystemId::Ds;
    picker_state.rumble_choosable = is_companion() ? companion_session->host_rumble_choosable()
                                                   : (source && source->offers_rumble_pak());
    // A 3DS game's texture pack, where the core running it can load one.
    picker_state.custom_textures = custom_textures_in_effect();
    picker_state.custom_textures_choosable = is_companion()
                                                 ? companion_session->host_custom_textures_choosable()
                                                 : (source && source->offers_custom_textures());
    // Resume from Save State, wherever save states are.
    picker_state.resume_from_save_state = resume_in_effect();
    picker_state.resume_choosable =
        is_companion() ? companion_session->host_resume_choosable() : save_points_available();
    // Which entries the menu has has to match on both devices, or the same
    // cursor position would mean different things on the two screens.
    picker_state.cursor_choosable =
        is_companion() ? companion_session->host_cursor_choosable() : pointer_choosable();
    // A property of the console, not of the session, so both devices reach the
    // same answer from `system` without anything to ask the host for.
    picker_state.save_points_available = is_companion()
                                             ? companion_session->host_save_points_available()
                                             : save_points_available();
    picker_state.save_sync_available =
        is_companion() ? companion_session->host_save_sync_available() : save_sync_available();
    picker_state.reset_available = system_profile(system).offers_reset;
    picker_state.smoothing_available = system_profile(system).offers_smoothing;
    picker->set_screens(screens_geometry(), system);
    // The tiles are judged against the same display, and filled on the same
    // terms, as the render behind them -- see device_layout().
    float panel_width = 0.0f;
    float panel_height = 0.0f;
    panel_size(panel_width, panel_height);
    picker->set_display_size(panel_width, panel_height,
                             is_companion() && local_screen == ScreenId::Bottom);
    picker->open(picker_state);
    if (is_companion()) {
        save_slots_shown = companion_session->host_save_slots_revision() - 1;
        show_host_save_slots();
    } else if (picker_state.save_points_available) {
        refresh_save_points();
    }
}

int App::Impl::max_render_scale() const {
    if (is_companion() && companion_session) return std::max(1, companion_session->host_max_render_scale());
    if (source) return source->max_render_scale();
    return local_game() ? std::max(1, system_profile(system).max_render_scale) : 1;
}

int App::Impl::auto_render_scale_now(int maximum) const {
    std::vector<DrawnScreen> drawn;
    const auto add = [&](ScreenId screen, int width, int height) {
        if (width > 0 && height > 0) drawn.push_back(DrawnScreen{screen, width, height});
    };
    // This device's window, as the layout draws each screen in it.
    for (const PlacedScreen& placed : place_screens(device_layout(), static_cast<float>(output_width),
                                                     static_cast<float>(output_height), screens_geometry())) {
        int width = static_cast<int>(std::ceil(placed.rect.w));
        int height = static_cast<int>(std::ceil(placed.rect.h));
        if (rotation_is_quarter_turn(placed.rotation)) std::swap(width, height);
        add(placed.screen, width, height);
    }
    // And the connected device's, for whichever screen it shows.
    for (const ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
        int width = 0;
        int height = 0;
        if (host_session && host_session->companion_wants(screen)) companion_target_size(screen, width, height);
        add(screen, width, height);
    }
    return auto_render_scale(system_profile(system).screens, drawn, maximum);
}

void App::Impl::update_auto_render_scale() {
    if (!source || is_companion() || render_scale != kAutoRenderScale) return;
    const int wanted = auto_render_scale_now(max_render_scale());
    if (wanted == effective_render_scale) return;
    source->set_render_scale(wanted);
    effective_render_scale = source->render_scale();
    log_info("render scale: Auto chose " + std::to_string(effective_render_scale) + "x for the screens as they are drawn");
}

void App::Impl::request_render_scale(int scale) {
    if (is_companion()) {
        companion_session->request_render_scale(scale);
        return;  // the host decides; it will tell us
    }
    adopt_render_scale(scale);
}

void App::Impl::adopt_render_scale(int scale) {
    const int wanted = scale == kAutoRenderScale ? kAutoRenderScale : std::clamp(scale, 1, max_render_scale());
    if (wanted == render_scale) {
        return;
    }
    render_scale = wanted;
    if (source) {
        if (render_scale == kAutoRenderScale) {
            effective_render_scale = 0;  // chosen afresh just below
            update_auto_render_scale();
        } else {
            source->set_render_scale(render_scale);
            // Whatever the core actually accepted, which is what the menu shows.
            render_scale = source->render_scale();
            effective_render_scale = render_scale;
        }
    }
    if (picker) {
        picker->set_render_scale(render_scale);
    }
    // A companion is following the host and has no game of its own to remember
    // this against, so it changes what it displays and nothing on disk.
    if (is_companion()) {
        log_info("following the host: render scale " +
                 (render_scale == kAutoRenderScale ? std::string("Auto") : std::to_string(render_scale) + "x"));
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

ScreenId App::Impl::own_separate_screen() const {
    if (is_companion()) return local_screen;
    if (host_session && host_session->has_companion()) return host_session->host_screen();
    return separate_screen;
}

void App::Impl::swap_separate_screens() {
    const auto other = [](ScreenId screen) { return screen == ScreenId::Top ? ScreenId::Bottom : ScreenId::Top; };
    if (is_companion()) {
        companion_session->request_screen_swap();
        return;  // the host decides; it will tell us
    }
    if (host_session && host_session->has_companion()) {
        // The two devices trade: the companion learns its new screen from the
        // session state, and each draws the screen it now holds.
        separate_screen = other(host_session->host_screen());
        host_session->set_host_screen(separate_screen);
        local_screen = separate_screen;
    } else {
        separate_screen = other(separate_screen);
    }
    persist_game_config();
    log_info(std::string("Separate Screens: this device shows the ") + to_string(separate_screen) + " screen");
}

void App::Impl::next_layout() {
    const Layout& from = active_layout();
    const LayoutSwap next = next_menu_layout(registry, system, screens_geometry(), from);
    if (next.layout_id.empty()) return;
    const Handedness hand = handedness_of(from.id);
    if (is_companion()) {
        if (next.keeps_hand) {
            companion_session->request_layout(next.layout_id, hand);
        } else {
            companion_session->request_layout(next.layout_id);
        }
        return;  // the host decides; it will tell us
    }
    request_layout(next.layout_id);
    if (next.keeps_hand) adopt_handedness(hand);
}

void App::Impl::swap_layout() {
    const Layout& from = active_layout();
    const LayoutSwap swap = layout_swap_from(registry, from);
    if (swap.layout_id.empty()) {
        return;
    }
    const Handedness hand = handedness_of(from.id);
    if (is_companion()) {
        if (swap.keeps_hand) {
            companion_session->request_layout(swap.layout_id, hand);
        } else {
            companion_session->request_layout(swap.layout_id);
        }
        return;  // the host decides; it will tell us
    }
    request_layout(swap.layout_id);
    if (swap.keeps_hand) {
        adopt_handedness(hand);  // onto the layout just swapped to
    }
}

void App::Impl::adopt_button_mapping(const ButtonMapping& mapping) {
    if (mapping == button_mapping) {
        return;
    }
    button_mapping = mapping;
    input.set_button_mapping(button_mapping);
    if (is_companion()) {
        log_info("following the host's button mapping");
        return;
    }
    if (have_game_config) {
        game_config.button_mapping = button_mapping;
        if (!game_config.save(paths)) log_warn("could not save this game's button mapping");
    }
}

bool App::Impl::resume_in_effect() const {
    if (is_companion()) return companion_session->host_resume_from_save_state();
    return have_game_config && game_config.resume_from_save_state;
}

void App::Impl::request_resume(bool on) {
    if (is_companion()) {
        companion_session->request_resume_from_save_state(on);
        return;  // the host decides; it will tell us
    }
    adopt_resume(on);
}

void App::Impl::adopt_resume(bool on) {
    if (!have_game_config || !save_points_available() || on == game_config.resume_from_save_state) {
        return;
    }
    game_config.resume_from_save_state = on;
    persist_game_config();
    if (picker) picker->set_resume_from_save_state(on);
    resume_point_accumulator = 0.0;
    if (on) {
        log_info("resume from save state is on: this game will start where it is left");
        return;
    }
    // Nothing still on its way to the disk, and nothing left there.
    resume_writer.cancel();
    const std::string path = resume_point_path(paths.saves_dir, game_path);
    if (file_exists(path)) std::remove(path.c_str());
    log_info("resume from save state is off: this game will start from the beginning");
}

void App::Impl::write_resume_point(const char* why) {
    if (is_companion() || !source || !have_game_config || !game_config.resume_from_save_state ||
        !save_points_available()) {
        return;
    }
    // Still exactly where it was resumed, which is what is on the disk.
    if (state == State::ResumeNotice) return;
    resume_point_accumulator = 0.0;
    const auto began = std::chrono::steady_clock::now();
    std::vector<uint8_t> state_bytes;
    std::string error;
    if (!source->save_state(state_bytes, &error)) {
        log_warn(std::string("resume point not written (") + why + "): " + error);
        return;
    }
    const auto taken_us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - began)
                              .count();
    log_debug(std::string("resume point: ") + std::to_string(state_bytes.size()) +
              " bytes taken in " + std::to_string(taken_us / 1000) + "." +
              std::to_string((taken_us / 100) % 10) + "ms (" + why + ")");
    resume_writer.write(resume_point_path(paths.saves_dir, game_path), std::move(state_bytes),
                        static_cast<int64_t>(std::time(nullptr)));
    std::string failure;
    if (resume_writer.take_failures(&failure) > 0) {
        log_warn("an earlier resume point could not be written: " + failure);
    }
}

void App::Impl::flush_saves(const char* why) {
    if (!source) return;
    if (source->flush_save_ram(false)) {
        write_resume_point("the game saved");
    } else if (why != nullptr) {
        write_resume_point(why);
    }
}

bool App::Impl::resume_game() {
    if (!source || !have_game_config || !game_config.resume_from_save_state ||
        !save_points_available()) {
        return false;
    }
    const std::string path = resume_point_path(paths.saves_dir, game_path);
    if (!file_exists(path)) {
        log_info("resume from save state is on, but there is nothing to resume yet; starting "
                 "from the beginning");
        return false;
    }
    SavePointInfo info;
    read_save_point_info(path, info);
    std::vector<uint8_t> state_bytes;
    std::string error;
    if (!read_save_point_state(path, state_bytes, &error)) {
        log_warn("could not resume from " + path + ": " + error + "; starting from the beginning");
        return false;
    }
    // Not straight after loading the game: the core has to have run first.
    // melonDS DS finishes setting itself up on its first frame -- its renderer
    // and its screen layout are built then -- and a state loaded before that
    // lands in a console that is only half there: sprites drawn over nothing,
    // and a picture the wrong shape, which the next frame then fails on. A
    // state loaded from the quick menu never met this, because by then the
    // game has been running for a while. Nothing of these frames is seen or
    // heard: the sound is not open yet, and the popup goes up over the frame
    // run after the state is in.
    constexpr int kFramesBeforeResuming = 3;
    constexpr int kMostFramesToWaitForAPicture = 60;
    int ran = 0;
    for (int frame = 0; frame < kMostFramesToWaitForAPicture; ++frame) {
        std::string boot_error;
        if (!source->advance_frame(&boot_error)) {
            log_warn("could not resume: the game did not start (" + boot_error +
                     "); starting from the beginning");
            return false;
        }
        ++ran;
        if (ran >= kFramesBeforeResuming && source->core_frame_for_test().valid()) break;
    }
    if (!source->load_state(state_bytes, &error)) {
        log_warn("could not resume from " + path + ": " + error + "; starting from the beginning");
        source->reset();
        return false;
    }
    // A few frames from there, so what is paused behind the popup is the
    // moment resumed, whole -- a single frame can still show some of what the
    // console was drawing before the state went in. Four: the deko3d melonDS
    // draws a frame's 3D during the frame before and hands over each picture
    // a frame late, so after two the picture behind the popup was a race's
    // 2D alone over black, and only the third has all of it. A core that
    // cannot run from the state it was handed is started again from the
    // beginning, rather than left to fail in front of the player a moment
    // later.
    constexpr int kFramesAfterResuming = 4;
    for (int frame = 0; frame < kFramesAfterResuming; ++frame) {
        std::string frame_error;
        if (!source->advance_frame(&frame_error)) {
            log_warn("resumed from " + path + ", but the game would not run from there (" +
                     frame_error + "); starting from the beginning");
            source->reset();
            return false;
        }
    }
    log_info("resumed from " + path + " after " + std::to_string(ran) + " frames of start-up" +
             (info.exists ? " (saved " + format_save_time(info.saved_at) + ")" : std::string()));
    return true;
}

void App::Impl::dismiss_resume_notice() {
    if (state != State::ResumeNotice) return;
    game_notice = kResumedNotice;
    state = State::Playing;
    input.set_game_input_enabled(true);
    frame_accumulator = 0.0;
    log_info("the resumed game is running");
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

bool App::Impl::custom_textures_in_effect() const {
    if (is_companion()) return companion_session->host_custom_textures();
    return source && source->custom_textures();
}

void App::Impl::request_custom_textures(bool on) {
    if (is_companion()) {
        companion_session->request_custom_textures(on);
        return;  // the host decides; it will tell us
    }
    adopt_custom_textures(on);
}

void App::Impl::adopt_custom_textures(bool on) {
    if (!source || !source->offers_custom_textures() || on == source->custom_textures()) {
        return;
    }
    source->set_custom_textures(on);
    if (picker) {
        picker->set_custom_textures(on);
    }
    if (have_game_config) {
        game_config.custom_textures = on;
        persist_game_config();
    }
}

bool App::Impl::lcd_filter_in_effect() const {
    if (system != SystemId::Ds) return false;
    return is_companion() ? companion_session->host_lcd_filter() : lcd_filter;
}

void App::Impl::request_lcd_filter(bool on) {
    if (is_companion()) {
        companion_session->request_lcd_filter(on);
        return;  // the host decides; it will tell us
    }
    adopt_lcd_filter(on);
}

void App::Impl::adopt_lcd_filter(bool on) {
    if (system != SystemId::Ds || on == lcd_filter) return;
    lcd_filter = on;
    if (picker) picker->set_lcd_filter(on);
    if (have_game_config) {
        game_config.lcd_filter = on;
        persist_game_config();
    }
    log_info(std::string("LCD filter ") + (on ? "on" : "off"));
}

void App::Impl::apply_lcd_grid() {
    if (lcd_filter_in_effect()) {
        compositor.set_lcd_grid(256, 192);
    } else {
        compositor.set_lcd_grid(0, 0);
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

bool App::Impl::steer_touch_spot(ButtonMappingScreen& screen) {
    const uint32_t now = SDL_GetTicks();
    const float seconds = spot_steer_ticks == 0 ? 0.0f : static_cast<float>(now - spot_steer_ticks) / 1000.0f;
    spot_steer_ticks = now;
    float push_x = 0.0f;
    float push_y = 0.0f;
    input.menu_push(push_x, push_y);
    return screen.steer(push_x, push_y, seconds);
}

void App::Impl::apply_stylus_cursor(DsInputState& held) {
    const uint32_t now = SDL_GetTicks();
    const float seconds = stylus_ticks == 0 ? 0.0f : static_cast<float>(now - stylus_ticks) / 1000.0f;
    stylus_ticks = now;
    const ScreenSize touch = system_profile(system).screens.bottom;
    if (right_stick_mode == RightStickMode::TouchAnalog) {
        stylus_cursor.hide();
        apply_touch_analog_stick(held, touch);
        return;
    }
    if (right_stick_mode != RightStickMode::TouchCursor) {
        stylus_cursor.hide();
        return;
    }
    // Down on the stick is down the screen as it is drawn, sideways or not.
    int16_t push_x = held.c_stick_x;
    int16_t push_y = held.c_stick_y;
    unturn_stick_push(touch_screen_rotation(), push_x, push_y);
    stylus_cursor.update(push_x, push_y, held.pressed(DsButton::RightStick),
                         settings.stick_deadzone, seconds, touch.width, touch.height);
    held.c_stick_x = 0;
    held.c_stick_y = 0;
    held.set(DsButton::RightStick, false);
    // A finger on the glass is the touch while it is there.
    if (stylus_cursor.pressed() && !held.touching) {
        held.touching = true;
        held.touch_x = stylus_cursor.x();
        held.touch_y = stylus_cursor.y();
    }
}

void App::Impl::apply_touch_analog_stick(DsInputState& held, const ScreenSize& touch) {
    // Down on the stick is down the screen as it is drawn, sideways or not.
    int16_t push_x = held.c_stick_x;
    int16_t push_y = held.c_stick_y;
    unturn_stick_push(touch_screen_rotation(), push_x, push_y);
    held.c_stick_x = 0;
    held.c_stick_y = 0;
    // A finger on the glass is the touch while it is there; the stick only
    // touches while it is pushed.
    if (held.touching) return;
    TouchSpot at;
    if (!touch_analog_stick_spot(push_x, push_y, settings.stick_deadzone, button_mapping.stick_centre,
                                 button_mapping.stick_radius, static_cast<float>(touch.width) / touch.height, at)) {
        return;
    }
    held.touching = true;
    held.touch_x = std::clamp(static_cast<int>(at.u * touch.width), 0, touch.width - 1);
    held.touch_y = std::clamp(static_cast<int>(at.v * touch.height), 0, touch.height - 1);
}

void App::Impl::publish_stylus_cursor() {
    if (!host_session) return;
    net::StylusCursorMessage message;
    message.u = stylus_cursor.u();
    message.v = stylus_cursor.v();
    message.opacity = stylus_cursor.opacity();
    message.pressed = stylus_cursor.pressed();
    host_session->set_stylus_cursor(message);
}

void App::Impl::draw_stylus_cursor(float width, float height) {
    net::StylusCursorMessage cursor;
    if (state == State::Companion && companion_session) {
        cursor = companion_session->host_stylus_cursor();
    } else if (state == State::Playing) {
        cursor.u = stylus_cursor.u();
        cursor.v = stylus_cursor.v();
        cursor.opacity = stylus_cursor.opacity();
        cursor.pressed = stylus_cursor.pressed();
    } else {
        return;
    }
    float panel_width = 0.0f;
    float panel_height = 0.0f;
    panel_size(panel_width, panel_height);
    ds::draw_stylus_cursor(quads, device_layout(), width, height, screens_geometry(), panel_width,
                           panel_height, cursor);
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
        if (!restart_external_game()) external_source->reset();
    }
    // What the other device shows is from before the reset, so it goes too.
    if (host_session) {
        host_session->clear_companion_screens();
    }
}

bool App::Impl::restart_external_game() {
    // Cemu cannot restart a game in place -- its own way of stopping one is
    // unfinished, and crashes after a few uses -- so the reset is the whole
    // program: closed, and started again on the same game, the way a player
    // would do it by hand. Only one this app started: one started by hand is
    // not this app's to close.
    if (!external_source || external_program_path.empty()) {
        log_warn("Reset: " + system_profile(system).core_description +
                 " was not started by this app, so it is not this app's to restart");
        return false;
    }
    log_info("Reset: closing " + system_profile(system).core_description +
             " and starting it again on the same game");
    // The link first, so its going is not taken for the game ending.
    external_source->close();
    external_program.stop();
    std::string error;
    if (!external_source->open(external_socket_path, &error)) {
        log_warn("Reset: could not open the way back in: " + error);
        return false;
    }
    external_game_arrived = false;
    external_link_was_running = false;
    menu_overlay_shown = false;
    menu_overlay.forget();
    std::string start_error;
    if (external_program.start(external_program_path, external_program_arguments, &start_error)) {
        external_program_wanted = true;
    } else {
        log_warn("Reset: could not start " + external_program_path + " again: " + start_error);
    }
    return true;
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

void App::Impl::open_data_management(bool whole_list) {
    if (!save_store) return;
    if (!data_screen) {
        data_screen.reset(new DataManagementScreen(
            *save_store, save_service ? save_service->instance() : new_save_version_id()));
        data_screen->set_systems(save_systems());
        if (save_service) {
            net::SaveExchangeService* service = save_service.get();
            data_screen->set_change_source([service] { return service->saves_received(); });
        }
    }
    // During a DS or 3DS game, its own page and nothing else -- unless asked
    // for the whole list, which still knows which game is being played.
    std::string game_id;
    SaveInfo info;
    if (save_sync_available() && save_store->find_for_rom(game_path, info)) {
        game_id = info.game_id;
    }
    data_screen->open(game_id, whole_list);
    data_from_settings = false;
    state = State::DataManagement;
    input.set_game_input_enabled(false);
}

void App::Impl::apply_data_management_result(const DataManagementResult& result) {
    if (result.restart_with) {
        // The game has its save open, so it is closed first -- without a
        // resume point, which would hold the save being replaced -- the save
        // changed, and the game started again from it.
        data_screen->close();
        const std::string rom = game_path;
        log_info("Data Management is restarting " + game_title + " on a changed save");
        resume_writer.finish();
        audio.close();
        if (source) {
            source->close();
            source.reset();
            note_game_closed();
        }
        std::string error;
        const bool changed = result.restart_with(&error);
        if (!changed) log_warn("the save was not changed: " + error);
        if (start_game(rom, false)) {
            game_notice = changed ? result.restart_notice : "The save could not be changed: " + error;
            state = State::ResumeNotice;
            input.set_game_input_enabled(false);
        }
        return;
    }
    if (result.close_requested) {
        // Back to where it was opened from: Settings, from its row there; the
        // quick menu during a game; the main menu otherwise.
        if (data_from_settings) {
            data_from_settings = false;
            settings_screen->return_from_data_management();
            state = State::Settings;
        } else if (local_game() && source) {
            set_menu_open(true);
        } else {
            open_launcher();
        }
    }
}

namespace {

// Where the cores Dualscreen downloads go: its own cores folder, the first
// place core_search_paths() looks.
std::string managed_cores_dir(const AppPaths& paths) { return path_join(paths.config_dir, "cores"); }

update::Component core_component_for(SystemId system) {
    return system == SystemId::ThreeDs ? update::Component::ThreeDsCore : update::Component::DsCore;
}

}  // namespace

std::string App::Impl::managed_core_path(SystemId system) const {
    if (!updater) return std::string();
    for (const update::ComponentSpec& component : updater->components()) {
        if (component.id == core_component_for(system)) {
            return path_join(managed_cores_dir(paths), update::core_library_name(component.asset));
        }
    }
    return std::string();
}

void App::Impl::start_updater(const AppOptions& options) {
    if (!update::desktop_updates_supported() || options.leave_instead_of_launcher) return;
    // Cemu, which Dualscreen downloads when asked on the Emulators page and
    // keeps up to date while it is the one Wii U games are played with: when
    // no other Wii U program is named, one it downloaded is the one used.
    const std::string cemu = update::managed_cemu_path(paths.data_dir);
    if (settings.wii_u_program.empty() && file_exists(cemu)) settings.wii_u_program = cemu;
    const bool cemu_in_use = settings.wii_u_program == cemu;
    std::vector<update::ComponentSpec> components =
        update::components_for(update::kSteamOsAsset, build_number(), true,
                               update::managed_cemu_build(paths.data_dir), cemu_in_use);
    // The DS and 3DS cores, from libretro's buildbot as RetroArch gets them:
    // kept up to date while the one in Dualscreen's cores folder is the one
    // played with.
    const std::string cores = managed_cores_dir(paths);
    bool core_kept = false;
    for (const SystemId system : {SystemId::Ds, SystemId::ThreeDs}) {
        update::ComponentSpec core = update::core_component(core_component_for(system), cores, false);
        core.automatic = core.installed != update::kNotInstalled &&
                         resolve_core_path(options, settings, system) ==
                             path_join(cores, update::core_library_name(core.asset));
        core_kept = core_kept || core.automatic;
        components.push_back(core);
    }
    updater = std::make_unique<update::Updater>(update::make_desktop_backend(paths.data_dir, cores),
                                                std::move(components));
    update_prompt.set_updater(updater.get());
    update_prompt.set_declined_file(path_join(path_join(paths.data_dir, "update"), "declined"));
    // The device connected, asked about its own update as this one checks.
    update_prompt.set_peer_asker([this](net::UpdateRequest request) {
        if (host_session) host_session->request_companion_update(request);
    });
    settings_screen->set_offers_updates(true);
    // At start-up, quietly, and only on a build from CI: a build made by hand
    // is not offered one unless it asks, so there is nothing to look for --
    // and nothing for a test run to go out to the network for. A build made
    // by hand still keeps its emulators up to date, when it keeps any.
    const bool test_run = options.exit_after_frames > 0 || options.exit_after_seconds > 0.0;
    const bool cemu_kept = cemu_in_use && update::managed_cemu_build(paths.data_dir) > 0;
    if ((build_number() > 0 || cemu_kept || core_kept) && !test_run) update_prompt.check_at_start();
}

void App::Impl::poll_updates() {
    if (!updater) return;
    // A companion joined here is offered its own update beside this one's;
    // joined to a host, this device is asked by it about its own.
    update_prompt.set_peer_name(companion_joined() ? host_session->companion_name() : std::string());
    if (companion_joined()) {
        net::UpdateAnswerMessage answer;
        if (host_session->take_companion_update_answer(answer)) update_prompt.peer_answered(answer);
    }
    if (is_companion()) {
        net::UpdateRequest request;
        while (companion_session->take_update_request(request)) update_prompt.host_requests(request);
    }
    const UpdatePromptResult result = update_prompt.poll(state == State::Launcher);
    if (result.answer_host && is_companion()) companion_session->answer_update(*result.answer_host);
    const std::string cemu = update::managed_cemu_path(paths.data_dir);
    bool changed = false;
    for (const update::Component installed : result.installed) {
        // Asked for on the Emulators page, or updated because it was already
        // the one in use: either way it is the one its console is played
        // with now, and kept up to date from here on.
        if (installed == update::Component::Cemu) {
            if (settings.wii_u_program != cemu) {
                log_info("Wii U games will be played with the Cemu at " + cemu);
                settings.wii_u_program = cemu;
                changed = true;
            }
        } else if (installed != update::Component::Dualscreen) {
            const SystemId system =
                installed == update::Component::ThreeDsCore ? SystemId::ThreeDs : SystemId::Ds;
            const std::string core = managed_core_path(system);
            if (resolve_core_path(options, settings, system) != core) {
                log_info(system_profile(system).name + " games will be played with the core at " + core);
                (system == SystemId::ThreeDs ? settings.core_path_3ds : settings.core_path) = core;
                changed = true;
            }
        } else {
            continue;
        }
        updater->set_automatic(installed, true);
    }
    if (changed && !settings.save(paths)) log_warn("could not save settings.json");
    if (!result.installed.empty()) {
        settings_screen->refresh_settings(settings);
        settings_screen->set_cores(resolve_core_statuses(options, settings));
    }
    // The Emulators page's rows: Dualscreen's own copy of each emulator,
    // named as what it would take over from when another is in use.
    if (device_plays(SystemId::WiiU)) {
        settings_screen->set_download_row(
            SystemId::WiiU,
            settings.wii_u_program.empty() || settings.wii_u_program == cemu
                ? "Dualscreen's Cemu"
                : "Use Dualscreen's own Cemu",
            update_prompt.component_value(update::Component::Cemu));
    }
    for (const update::ComponentSpec& component : updater->components()) {
        if (component.source != update::Source::Buildbot) continue;
        const SystemId system =
            component.id == update::Component::ThreeDsCore ? SystemId::ThreeDs : SystemId::Ds;
        if (!device_plays(system)) continue;
        const bool ours = resolve_core_path(options, settings, system) == managed_core_path(system);
        settings_screen->set_download_row(
            system, (ours || component.installed == update::kNotInstalled ? "Dualscreen's " : "Use Dualscreen's own ") +
                        component.name,
            update_prompt.component_value(component.id));
    }
    if (result.restart) {
        log_info("leaving to start the new build");
        update::request_restart();
        running = false;
    }
    settings_screen->set_updates_value(update_prompt.row_value());
}

void App::Impl::apply_settings_result(const SettingsScreenResult& result) {
    if (result.open_updates) {
        // A restart starts the program again as it was started: into the
        // game a shortcut named, when it is that one being played.
        // A companion's restart leaves the host's game playing.
        const bool over_game = settings_screen->over_game() && !is_companion();
        // And whether closing it loses anything, as Quit's warning says.
        update_prompt.set_game_playing(
            over_game,
            over_game && !options.game_path.empty() && expand_user_path(options.game_path) == game_path,
            over_game && save_points_available() && resume_in_effect());
        update_prompt.choose();
        return;
    }
    if (result.open_download) {
        update_prompt.choose(*result.open_download == SystemId::WiiU      ? update::Component::Cemu
                             : *result.open_download == SystemId::ThreeDs ? update::Component::ThreeDsCore
                                                                         : update::Component::DsCore);
        return;
    }
    if (result.open_data_management) {
        // Every game's saves, from Settings, whether or not one is playing.
        open_data_management(true);
        data_from_settings = state == State::DataManagement;
        return;
    }
    if (result.settings_changed) {
        const int previous_tv_audio = settings.wii_u_tv_audio;
        settings = settings_screen->settings();
        apply_favorite_color_accent(settings.ds_favorite_color);
        ui_sounds.set_enabled(settings.ui_sounds);
        apply_wii_u_tv_audio(previous_tv_audio);
        if (!settings.save(paths)) {
            log_warn("could not save settings.json");
        }
        swap_interval_applied = -1;  // reapplied next pass: see update_swap_pacing()
        update_swap_pacing();
        // The prompts change under the cursor that changed them, which is the
        // only way to see what the setting does.
        apply_button_style();
        // Takes effect at once rather than at the next launch: this is the
        // kind of thing somebody changes to see what it does, and the poll
        // loop below picks the new value up on its next pass.
        // A game's own swap stays: it is the other screen from whatever this
        // now says.
        solo_screen_swapped = have_game_config && game_config.screens_swapped;
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
    // The menu's blurred still was taken from the session's picture.
    menu_backdrop.forget();
    // The player quit the game from this device: the host has shut down, and
    // this app goes with it rather than back to the main menu.
    if (companion_session->host_quit_for_this_device()) {
        log_info("the game was quit from this device, so this app is quitting too");
        running = false;
        return;
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
    if (options.leave_instead_of_launcher) {
        // The main menu this would go back to is the caller's -- see
        // AppOptions::leave_instead_of_launcher.
        log_info("leaving for the main menu of the program that started this");
        running = false;
        return;
    }
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
    lcd_filter = false;
    right_stick_mode = settings.default_right_stick_mode;
    input.set_right_stick_mode(right_stick_mode);
    button_mapping = ButtonMapping();
    input.set_button_mapping(button_mapping);
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
    // Said in the log too: a message on screen is otherwise the one thing a
    // log sent afterwards cannot show.
    std::string said = "showing a message: " + title;
    for (const std::string& detail : details) said += " | " + detail;
    log_info(said);
    state = State::Message;
    input.set_game_input_enabled(false);
}

void App::Impl::open_browser() {
    if (!browser) {
        browser.reset(new GameBrowser());
    }
    browser->set_offers_forwarders(forwarder_platform() != nullptr);
    browser->open(settings.last_browse_dir);
    state = State::Browser;
    input.set_game_input_enabled(false);
}

void App::Impl::open_forwarder(const std::string& rom_path) {
    ForwarderPlatform* platform = forwarder_platform();
    if (platform == nullptr) return;
    if (!forwarder_screen) forwarder_screen.reset(new ForwarderScreen(*platform));
    // What the ROM says it is; the file's own name when it says nothing.
    DsRomInfo info;
    std::string error;
    if (!read_ds_rom_info(rom_path, info, &error)) {
        log_info("forwarder: " + rom_path + " is not a DS ROM that names itself (" + error + ")");
        info = DsRomInfo();
    }
    forwarder_screen->open(rom_path, info, settings.steamgriddb_api_key);
    state = State::Forwarder;
    input.set_game_input_enabled(false);
    log_info("Forwarder Creation is up for " + rom_path);
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
    // Auto as far as the console goes: the core says how far it goes once it
    // is loaded, and the source holds it to that.
    effective_render_scale = render_scale == kAutoRenderScale
                                 ? auto_render_scale_now(system_profile(system).max_render_scale)
                                 : render_scale;
    source_config.render_scale = effective_render_scale;
    source_config.rumble_pak = have_game_config && game_config.rumble_pak;
    source_config.custom_textures = have_game_config && game_config.custom_textures;
    source_config.nickname = settings.ds_nickname;
    source_config.favorite_color = settings.ds_favorite_color;
    source_config.birth_month = settings.ds_birth_month;
    source_config.birth_day = settings.ds_birth_day;

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
            if (resolved_core_path.empty() && builtin_core_system()) {
                details->push_back("This device plays " +
                                   system_profile(*builtin_core_system()).name +
                                   " games only: its emulator is part of the app, and there is "
                                   "no other to load.");
            } else if (resolved_core_path.empty()) {
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

    // A scale from an older config, or one this core does not offer, is
    // clamped by the source; the menu should show what the core was given.
    effective_render_scale = source->render_scale();
    if (render_scale != kAutoRenderScale) render_scale = effective_render_scale;
    if (system == SystemId::Ds) {
        source->set_run_ahead(settings.run_ahead_ds);
    }
    return true;
}

void App::Impl::note_game_closed() {
    if (save_store && !game_path.empty()) save_store->note_game_stopped(game_path);
}

void App::Impl::note_external_game_closed() {
    if (save_store && !external_save_rom.empty()) save_store->note_game_stopped(external_save_rom);
    external_save_rom.clear();
}

std::vector<std::string> App::Impl::save_systems() const {
    std::vector<std::string> systems{kSaveSystemDs};
#if !defined(__SWITCH__) && !defined(__ANDROID__)
    // A 3DS game's save goes in Azahar's folder under this app's saves, which
    // is always here to put it in; a Wii U game's in Cemu's, if there is one.
    systems.push_back(kSaveSystem3ds);
    if (save_store && !save_store->folder_roots().wii_u_mlc.empty()) systems.push_back(kSaveSystemWiiU);
#endif
    return systems;
}

void App::Impl::update_save_rom_dirs() {
    if (!save_store) return;
    std::vector<std::string> directories;
    if (!settings.last_browse_dir.empty()) directories.push_back(settings.last_browse_dir);
    for (const std::string& directory : default_browse_dirs()) directories.push_back(directory);
    save_store->set_rom_dirs(std::move(directories));
    // And where the 3DS and Wii U saves are: Azahar's under the saves folder
    // as the store already knows, Cemu's wherever Cemu keeps them.
    FolderSaveRoots roots = save_store->folder_roots();
    const char* home = std::getenv("HOME");
    roots.wii_u_mlc = find_cemu_mlc(settings.wii_u_mlc_path, settings.wii_u_program, home != nullptr ? home : "");
    if (roots.wii_u_mlc != save_store->folder_roots().wii_u_mlc) {
        log_info(roots.wii_u_mlc.empty() ? std::string("Cemu's mlc01 folder was not found; no Wii U saves")
                                         : "Wii U saves are in " + roots.wii_u_mlc);
    }
    // Cemu's game list, for the names and icons of games it plays from
    // somewhere other than mlc01.
    roots.wii_u_title_lists =
        roots.wii_u_mlc.empty()
            ? std::vector<std::string>()
            : find_cemu_title_lists(settings.wii_u_program, home != nullptr ? home : "", roots.wii_u_mlc);
    if (roots.wii_u_title_lists != save_store->folder_roots().wii_u_title_lists && !roots.wii_u_title_lists.empty()) {
        log_info("Wii U games are named from Cemu's game list in " + roots.wii_u_title_lists.front());
    }
    save_store->set_folder_roots(std::move(roots));
}

bool App::Impl::start_game(const std::string& rom_path, bool fatal_on_failure) {
    // The game being left is left where it was, for when it is next started.
    write_resume_point("another game is starting");
    resume_writer.finish();
    // Tear down anything already running, so picking a second game from the
    // browser is the same code path as booting the first.
    audio.close();
    if (source) {
        source->close();
        source.reset();
        note_game_closed();
    }
    external_program.stop();
    external_source.reset();
    note_external_game_closed();
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
    lcd_filter = false;
    right_stick_mode = settings.default_right_stick_mode;

    // A Wii U game is a folder, or one file of gigabytes: kept by where it is
    // rather than by reading it all. Everything else, by what it is.
    const std::string path_key = "path:" + rom_path;
    SystemId rom_system = SystemId::Ds;
    const bool wii_u_game = system_for_rom(rom_path, rom_system) && rom_system == SystemId::WiiU;
    const std::string rom_sha1 = wii_u_game
                                     ? Sha1::hash_bytes(path_key.data(), path_key.size())
                                     : Sha1::hash_file(rom_path);
    render_scale = settings.default_render_scale;
    if (rom_sha1.empty()) {
        log_warn("could not hash the ROM; this session's layout will not be remembered");
    } else {
        game_config = GameConfig::load_or_default(paths, settings, rom_sha1, game_title);
        have_game_config = true;
        active_layout_id = game_config.layout_id;
        separate_screen = game_config.separate_screen;
        // A layout the menu no longer offers -- Standard, for now -- is not
        // one to be left on with no way back to it.
        if (registry.resolve(active_layout_id).hidden) active_layout_id = kNewGameLayoutId;
        bilinear_filtering = game_config.bilinear_filtering;
        render_scale = game_config.render_scale;
        screen_gap = game_config.screen_gap;
        lcd_filter = game_config.lcd_filter;
        right_stick_mode = game_config.right_stick_mode;
    }
    // A Wii U's buttons are Cemu's to map, player by player.
    button_mapping = have_game_config && !wii_u_game ? game_config.button_mapping : ButtonMapping();
    input.set_button_mapping(button_mapping);
    // One this console has: a DS's right stick is a stylus or nothing, so a
    // 3DS's C-Stick, the default everywhere else, is the stylus there.
    right_stick_mode = right_stick_mode_for(right_stick_mode, system_profile(rom_system).has_analog_stick);
    stylus_cursor = StylusCursor();
    solo_screen_swapped = have_game_config && game_config.screens_swapped;
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
    // The game's icon, read now -- out of the file itself -- so the menu has
    // it from the first time it opens, saved or not.
    game_icon_rom = rom_path;
    game_icon_tried_us = 0;
    refresh_game_icon();
    const SystemProfile& profile = system_profile(system);
    resolved_core_path = resolve_core_path(options, settings, system);
    apply_system();

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
    if (!ensure_gl_context(profile.gl_context, &context_error)) {
        enter_message(MessageKind::Error, "Cannot set up the display for " + profile.name,
                      context_error,
                      {profile.core_description + " renders on the GPU and needs OpenGL 3.3."},
                      fatal_on_failure);
        return false;
    }

    switch (profile.frame_origin) {
        case FrameOrigin::LibretroCore: {
            game_path = rom_path;
            // Before the core opens it: a save kept under another name for
            // this game is brought under this ROM's, where the core looks.
            if (save_store) {
                save_store->set_device_name(settings.device_name);
                save_store->note_game_started(rom_path);
            }
            std::string source_error;
            std::vector<std::string> details;
            if (!open_core(rom_path, &source_error, &details)) {
                if (save_store) save_store->note_game_stopped(rom_path);
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
            effective_render_scale = 1;
            external_game_arrived = false;
            solo_screen_unchecked = true;
            log_info(profile.core_description + " can hand its screens over at " + socket_path);

            // Started here rather than by the player: in Game Mode there is no
            // desktop to start it from, which is the whole reason this app
            // exists. Not fatal when it fails -- the socket is open either way
            // and a player on a desktop can still start one by hand.
            if (save_store) {
                save_store->set_device_name(settings.device_name);
                save_store->note_game_started(rom_path);
                external_save_rom = rom_path;
            }
            const std::string& program = settings.external_program_for(system);
            if (!program.empty()) {
                std::vector<std::string> arguments;
                for (const std::string& argument : settings.external_arguments_for(system)) {
                    arguments.push_back(fill_placeholders(argument, socket_path, rom_path));
                }
                std::string start_error;
                external_program_path = program;
                external_program_arguments = arguments;
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
    // Before the sound opens, so the one frame run from the resume point is
    // not heard.
    const bool resumed = resume_game();
    resume_point_accumulator = 0.0;
    std::string audio_error;
    if (!audio.open(local_audio_sample_rate(), &audio_error)) {
        log_warn(audio_error + "; continuing without sound");
    }

    state = resumed ? State::ResumeNotice : State::Playing;
    input.set_game_input_enabled(!resumed);
    apply_filtering();
    frame_accumulator = 0.0;
    reset_speed_window();
    last_counter = SDL_GetPerformanceCounter();

    // Offer this game to the network. A failure here is not fatal: the game is
    // perfectly playable on one device.
    host_session.reset(new net::HostSession());
#ifdef __SWITCH__
    // Every send here is a call into the system, and a busy picture is
    // thirty of them: done on this thread, a Mario Kart race streamed to a
    // Deck ran three frames a pass and sent one picture in three. See
    // HostSession::set_media_off_thread().
    host_session->set_media_off_thread(true);
#endif
    net::HostSession::Config host_config;
    host_config.device_name = settings.device_name;
    host_config.game_title = game_title;
    refresh_game_icon();
    if (game_icon_for == "here:" + game_icon_rom) {
        host_config.game_icon_width = game_icon_width;
        host_config.game_icon_height = game_icon_height;
        host_config.game_icon_rgba = game_icon_rgba;
    }
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

namespace {
uint64_t steady_micros() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
}  // namespace

void App::Impl::report_slow_pass(uint64_t pass_us) {
    // Long enough that the next pass has two frames to run, which is a
    // picture never shown -- and, past four, time the game loses outright.
    const uint64_t frame_us = static_cast<uint64_t>(frame_period * 1e6);
    if (pass_us <= frame_us + frame_us / 2) return;
    // One line a second at most, counting the rest: a stall that lasts a
    // second is sixty of these, and one of them says what it was.
    const uint64_t now_us = steady_micros();
    if (slow_pass_said_us != 0 && now_us - slow_pass_said_us < 1'000'000) {
        ++slow_passes_unsaid;
        return;
    }
    slow_pass_said_us = now_us;
    const PassTimes& t = pass_times;
    const uint64_t known = t.network_us + t.core_us + t.stream_us + t.draw_us;
    const auto ms = [](uint64_t us) {
        char text[16];
        std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(us) / 1000.0);
        return std::string(text);
    };
    log_info("a slow pass: " + ms(pass_us) + "ms -- " + ms(t.network_us) + "ms network, " +
             ms(t.core_us) + "ms emulating " + std::to_string(t.frames) + " frame" +
             (t.frames == 1 ? "" : "s") + ", " + ms(t.stream_us) +
             "ms handing the picture to the other device, " + ms(t.draw_us) +
             "ms drawing and presenting, " + ms(pass_us > known ? pass_us - known : 0) +
             "ms everything else (input, menus, waiting)" +
             (slow_passes_unsaid > 0
                  ? "; " + std::to_string(slow_passes_unsaid) + " more since the last one said"
                  : std::string()));
    slow_passes_unsaid = 0;
}

void App::Impl::report_speed() {
    const double wanted = speed_seconds / frame_period;
    const double percent = wanted > 0.0 ? 100.0 * speed_frames / wanted : 0.0;
    const double core_ms = speed_frames > 0 ? 1000.0 * speed_core_seconds / speed_frames : 0.0;
    const double rest_ms =
        speed_passes > 0 ? 1000.0 * (speed_seconds - speed_core_seconds) / speed_passes : 0.0;
    char line[512];
    std::snprintf(line, sizeof(line),
                  "speed: %d frames in %.1fs, %.0f%% of full speed; the core took %.1f ms a frame "
                  "(a frame is %.1f ms), and each of %d passes spent %.1f ms more drawing, "
                  "reading input and waiting; slowest frame %.1f ms, %d longer than a frame, "
                  "%d passes catching up, %d frames shown late, %d refreshes left to a waiting "
                  "frame, %d frames waited for a buffer; drawing a frame took %.1f ms, at "
                  "most %.1f",
                  speed_frames, speed_seconds, percent, core_ms, 1000.0 * frame_period, speed_passes,
                  rest_ms, 1000.0 * speed_slowest_frame, speed_long_frames, speed_catch_ups,
                  speed_frame_gaps, speed_queue_waits, speed_swap_waits,
                  speed_draws > 0 ? speed_draw_us / 1000.0 / speed_draws : 0.0,
                  speed_slowest_draw_us / 1000.0);
    log_info(line);
    reset_speed_window();
}

void App::Impl::persist_game_config() {
    if (!have_game_config) {
        return;
    }
    game_config.layout_id = active_layout_id;
    game_config.bilinear_filtering = bilinear_filtering;
    game_config.render_scale = render_scale;
    game_config.screen_gap = screen_gap;
    game_config.lcd_filter = lcd_filter;
    game_config.separate_screen = separate_screen;
    game_config.right_stick_mode = right_stick_mode;
    game_config.button_mapping = button_mapping;
    game_config.touch_last_played();
    if (!game_config.save(paths)) {
        log_warn("could not save this game's layout");
    }
}

UiSoundView App::Impl::sound_view() const {
    UiSoundView view;
    view.screen = static_cast<int>(state);
    view.saves = save_states_made;
    // A companion's game is the host's, and on screen here once connected.
    const bool game_here = local_game() != nullptr || is_companion();
    view.in_game = state == State::Playing || (state == State::Companion && is_companion());
    view.menu = game_here && (state == State::Menu || state == State::Settings ||
                              state == State::Controllers || state == State::DataManagement ||
                              on_cemu_page());
    switch (state) {
        case State::Menu:
            if (picker) picker->describe_for_sound(view);
            break;
        case State::Settings:
            if (settings_screen) settings_screen->describe_for_sound(view);
            break;
        case State::Controllers: controllers_screen.describe_for_sound(view); break;
        case State::GraphicPacks: graphic_packs_screen.describe_for_sound(view); break;
        case State::Amiibo: amiibo_browser.describe_for_sound(view); break;
        case State::ButtonMapping: button_mapping_screen.describe_for_sound(view); break;
        case State::WiiUPlayer: wii_u_player_screen.describe_for_sound(view); break;
        case State::Message: message.describe_for_sound(view); break;
        case State::GameDialog: view.focus = static_cast<uint64_t>(game_dialog_screen.focus()); break;
        case State::Browser:
            if (browser) browser->describe_for_sound(view);
            break;
        case State::Launcher:
            if (launcher) launcher->describe_for_sound(view);
            break;
        case State::HostPicker:
            if (host_picker) host_picker->describe_for_sound(view);
            break;
        case State::DataManagement:
            if (data_screen) data_screen->describe_for_sound(view);
            break;
        case State::Forwarder:
            if (forwarder_screen) forwarder_screen->describe_for_sound(view);
            break;
        case State::Playing:
        case State::Companion:
        case State::ResumeNotice: break;
    }
    update_prompt.describe_for_sound(view);
    return view;
}

void App::Impl::play_ui_sound(UiSound sound, const UiSoundView& view) {
    if (host_decides_sounds()) return;
    ui_sounds.play(sound);
    // Anything over the game is the session's, whichever device is showing
    // it: the menu is drawn on both, and its sounds are heard on both.
    if (host_session && host_session->has_companion() && (view.menu || view.in_game)) {
        host_session->publish_menu_sound(static_cast<net::MenuSound>(ui_sound_to_wire(sound)));
    }
}

void App::Impl::handle_ui(const std::vector<UiAction>& actions,
                          const std::vector<PointerEvent>& pointers) {
    poll_updates();
    // An update being offered, downloaded or reported is answered before
    // anything under it sees a press.
    if (update_prompt.open()) {
        update_prompt.update(actions, pointers, static_cast<float>(output_width),
                             static_cast<float>(output_height));
        return;
    }
    if (menu_key_back_to_game(actions)) return;
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
                    if (!paired()) toggle_solo_screen_swap();
                } else if (active_layout().swaps_screens) {
                    // Separate Screens: the other screen, rather than another
                    // layout.
                    swap_separate_screens();
                } else {
                    swap_layout();
                }
                break;
            }
            if (action == UiAction::NextLayout) {
                next_layout();
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
                    settings_came_from = State::Launcher;
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
                menu_backdrop.forget();
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
                // Taps on the prompts of a Controller Order drawn here too,
                // which press what they name, over there.
                std::vector<UiAction> forwarded = actions;
                if (companion_session->host_controllers_view().open) {
                    std::vector<PointerEvent> rest = pointers;
                    take_guide_taps(mirrored_controllers.guide_hits(), forwarded, rest);
                    // Scrolled here, as this device's own view of the list;
                    // a tap goes over as the row it landed on.
                    for (const int tap : MenuPage::taps_on(mirrored_controllers, rest,
                                                           static_cast<float>(output_width),
                                                           static_cast<float>(output_height))) {
                        if (tap >= 0) companion_session->request_menu_focus(tap);
                    }
                } else if (companion_session->host_amiibo_browser_view().open) {
                    std::vector<PointerEvent> rest = pointers;
                    take_guide_taps(mirrored_amiibo.guide_hits(), forwarded, rest);
                    // Its own view of the list here, scrolled under a finger;
                    // a tap goes over as the row it landed on.
                    for (const int tap : MenuPage::taps_on(mirrored_amiibo, rest, static_cast<float>(output_width),
                                                           static_cast<float>(output_height))) {
                        if (tap >= 0) companion_session->request_menu_focus(tap);
                    }
                } else if (companion_session->host_graphic_packs_view().open) {
                    std::vector<PointerEvent> rest = pointers;
                    take_guide_taps(mirrored_graphic_packs.guide_hits(), forwarded, rest);
                    // Scrolled under a finger here, as this device's own view
                    // of the page; a tap goes over as what it landed on.
                    for (const int tap : MenuPage::taps_on(mirrored_graphic_packs, rest,
                                                           static_cast<float>(output_width),
                                                           static_cast<float>(output_height))) {
                        if (tap == MenuPage::kTapOffPopup) {
                            companion_session->request_menu_action(net::MenuAction::Cancel);
                        } else {
                            companion_session->request_menu_focus(tap);
                        }
                    }
                } else if (companion_session->host_wii_u_player_view().open) {
                    std::vector<PointerEvent> rest = pointers;
                    take_guide_taps(mirrored_wii_u_player.guide_hits(), forwarded, rest);
                    for (const int tap : MenuPage::taps_on(mirrored_wii_u_player, rest,
                                                           static_cast<float>(output_width),
                                                           static_cast<float>(output_height))) {
                        if (tap == MenuPage::kTapOffPopup) {
                            companion_session->request_menu_action(net::MenuAction::Cancel);
                        } else {
                            companion_session->request_menu_focus(tap);
                        }
                    }
                } else if (companion_session->host_button_mapping_view().open) {
                    std::vector<PointerEvent> rest = pointers;
                    take_guide_taps(mirrored_button_mapping.guide_hits(), forwarded, rest);
                    for (const int tap : MenuPage::taps_on(mirrored_button_mapping, rest,
                                                           static_cast<float>(output_width),
                                                           static_cast<float>(output_height))) {
                        if (tap == MenuPage::kTapOffPopup) {
                            companion_session->request_menu_action(net::MenuAction::Cancel);
                        } else if (tap >= ButtonMappingScreen::kSpotEntryBase) {
                            // Too wide to go as an entry: where it lifted, said as a spot.
                            const TouchSpot spot = ButtonMappingScreen::entry_spot(tap);
                            companion_session->request_touch_spot(spot.u, spot.v);
                        } else {
                            companion_session->request_menu_focus(tap);
                        }
                    }
                    // A finger or the pad moving the Touch Point here moves the
                    // host's as it goes -- the pad smoothly, so its directions
                    // are not sent as steps too.
                    const net::ButtonMappingViewMessage& host_view = companion_session->host_button_mapping_view();
                    const bool steered = steer_touch_spot(mirrored_button_mapping);
                    if (host_view.placing) strip_directions(forwarded);
                    if (mirrored_button_mapping.drag_spot(static_cast<float>(output_width),
                                                          static_cast<float>(output_height)) ||
                        steered) {
                        const TouchSpot spot = mirrored_button_mapping.spot();
                        if (std::fabs(spot.u - host_view.spot_u) > 0.001f ||
                            std::fabs(spot.v - host_view.spot_v) > 0.001f) {
                            companion_session->request_touch_spot(spot.u, spot.v);
                        }
                    }
                }
                for (const UiAction action : forwarded) {
                    net::MenuAction wire = net::MenuAction::Confirm;
                    if (menu_action_for(action, wire)) {
                        companion_session->request_menu_action(wire);
                    }
                }
                break;
            }
            // The host has just resumed its game and is waiting under a popup
            // that says so; this device shows the same one, and its OK is the
            // host's OK. Nothing reaches the game until then.
            if (companion_session->host_resume_notice()) {
                input.set_game_input_enabled(false);
                if (notice_popup_answered(actions, pointers, static_cast<float>(output_width),
                                          static_cast<float>(output_height), kResumedNotice)) {
                    companion_session->request_menu_action(net::MenuAction::Confirm);
                }
                break;
            }
            // The game's own dialog, up over it on the host: the same popup
            // here, answered here as it would be there -- and the keyboard
            // typed here when this device is the GamePad.
            if (mirrored_game_dialog.follow(companion_session->host_game_dialog())) {
                input.set_game_input_enabled(false);
                mirrored_game_dialog.type(input.typed_text(), input.backspaces());
                for (const net::GameDialogAnswerMessage& said :
                     mirrored_game_dialog.update(actions, pointers, static_cast<float>(output_width),
                                                 static_cast<float>(output_height))) {
                    companion_session->answer_game_dialog(said);
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
                    companion_session->ask_for_menu();
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
                update_save_rom_dirs();
                if (!settings.save(paths)) {
                    log_warn("could not remember the browsed folder");
                }
            }
            if (result.forwarder_requested) {
                open_forwarder(result.chosen_path);
            } else if (result.back_requested) {
                open_launcher();
            } else if (result.game_chosen) {
                settings.last_browse_dir = browser->current_directory();
                update_save_rom_dirs();
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
                std::vector<PointerEvent> rest;
                for (const PointerEvent& pointer : pointers) {
                    if (!pointer.released) {
                        rest.push_back(pointer);
                        continue;
                    }
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
                            case ButtonGlyph::Plus: wire = net::MenuAction::Settings; break;
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
                    rest.push_back(pointer);
                }
                // Everything else a finger did on the menu drawn here: scrolled here,
                // as this device's own view of it, and each tap sent over as what it
                // landed on, heard there as a tap made there would be -- see MenuPage.
                for (const int tap : MenuPage::taps_on(*picker, rest, static_cast<float>(output_width),
                                                       static_cast<float>(output_height))) {
                    if (tap == MenuPage::kTapOffPopup) {
                        companion_session->request_menu_action(net::MenuAction::Cancel);
                    } else {
                        companion_session->request_menu_focus(tap);
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

        case State::GraphicPacks: {
            update_graphic_packs_screen(actions, pointers);
            break;
        }

        case State::Amiibo: {
            update_amiibo_browser(actions, pointers);
            break;
        }

        case State::ButtonMapping: {
            update_button_mapping_screen(actions, pointers);
            break;
        }

        case State::WiiUPlayer: {
            update_wii_u_player_screen(actions, pointers);
            break;
        }

        case State::GameDialog: {
            game_dialog_screen.type(input.typed_text(), input.backspaces());
            const GameDialogResult result = game_dialog_screen.update(
                actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height));
            if (result.answered) {
                if (external_source) external_source->answer_game_dialog(result.answer);
                log_info("the game's dialog was answered");
                leave_game_dialog();
            }
            break;
        }

        case State::ResumeNotice: {
            if (notice_popup_answered(actions, pointers, static_cast<float>(output_width),
                                      static_cast<float>(output_height), game_notice)) {
                dismiss_resume_notice();
            }
            break;
        }


        case State::Settings: {
            // A name being typed, from the keyboard -- see wants_text().
            settings_screen->type(input.typed_text(), input.backspaces());
            apply_settings_result(
                settings_screen->update(actions, pointers, static_cast<float>(output_width),
                                        static_cast<float>(output_height)));
            break;
        }

        case State::Forwarder: {
            const ForwarderScreenResult result = forwarder_screen->update(
                actions, pointers, static_cast<float>(output_width), static_cast<float>(output_height));
            if (result.api_key_changed) {
                settings.steamgriddb_api_key = result.api_key;
                if (!settings.save(paths)) log_warn("could not keep the SteamGridDB API key");
            }
            if (result.close_requested) {
                // Back to the file it was made for, the cursor still on it.
                state = State::Browser;
                if (!browser->is_open()) browser->open(browser->current_directory());
            }
            break;
        }

        case State::DataManagement: {
            apply_data_management_result(
                data_screen->update(actions, pointers, static_cast<float>(output_width),
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
                if (!picker->embedded() && !quick_menu_leaving()) {
                    open_picker(/*embedded=*/true);
                }
                // Where Cemu can draw the menu over the game, the menu keys
                // open it there, wherever the focus is -- a pad's menu button,
                // or Tab or F1 in Cemu's window, which arrive as the same
                // press. See wants_menu_overlay().
                if (external_source->overlay_supported() && MenuOverlay::supported() &&
                    std::any_of(actions.begin(), actions.end(), [](UiAction action) {
                        return action == UiAction::ToggleMenu || action == UiAction::OpenMenu;
                    })) {
                    picker->close();
                    set_menu_open(true);
                    break;
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
            if (picker && picker->embedded() && !quick_menu_leaving()) {
                // The game beside it ended or has not arrived.
                picker->close();
            }
            for (const UiAction action : actions) {
                if (action == UiAction::ToggleMenu || action == UiAction::OpenMenu) {
                    set_menu_open(true);
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

void App::Impl::swap() {
    const uint64_t began = steady_micros();
    if (pass_began_us != 0 && began > pass_began_us) {
        frame_delay.worked(began - pass_began_us);
    }
    swap_and_pace(window);
    const uint64_t done = steady_micros();
    frame_delay.presented(done, done - began);
}

void App::Impl::render_pages(float width, float height, bool over_game,
                             const ScreenTextures* frames) {
    if (state == State::Menu || quick_menu_leaving()) {
        // The previews are drawn from the frame the game is paused on, so each
        // tile shows what picking it will actually look like.
        picker->set_preview_screen(own_separate_screen());
        // And judged as the render behind them is this frame: a companion
        // fills its display with the bottom screen and not the top, so a swap
        // changes which -- see device_layout().
        {
            float panel_width = 0.0f;
            float panel_height = 0.0f;
            panel_size(panel_width, panel_height);
            picker->set_display_size(panel_width, panel_height,
                                     is_companion() && local_screen == ScreenId::Bottom);
        }
        picker->set_device_screen(wii_u_screen_here());
        // Controls as they are this frame: controllers come, go and move.
        if (!is_companion() && system == SystemId::WiiU) picker->set_wii_u_controllers(wii_u_controller_rows());
        refresh_game_icon();
        picker->set_game_icon(game_icon.valid() ? &game_icon : nullptr);
        picker->render(quads, text, button_glyphs, width, height, shown_game_title(), frames);
    } else if (state == State::GraphicPacks) {
        graphic_packs_screen.set_over_game(over_game);
        graphic_packs_screen.render(quads, text, button_glyphs, width, height);
    } else if (state == State::Amiibo) {
        amiibo_browser.set_over_game(over_game);
        amiibo_browser.render(quads, text, button_glyphs, width, height);
    } else if (state == State::ButtonMapping) {
        button_mapping_screen.set_over_game(over_game);
        // The game's touch screen as it was paused, to place a Touch Point on.
        ScreenSource* const drawing = active_source();
        button_mapping_screen.set_touch_screen(drawing != nullptr ? drawing->screens().bottom : nullptr);
        button_mapping_screen.render(quads, text, button_glyphs, width, height);
    } else if (state == State::WiiUPlayer) {
        wii_u_player_screen.set_over_game(over_game);
        wii_u_player_screen.render(quads, text, button_glyphs, width, height);
    } else if (state == State::Controllers) {
        controllers_screen.set_over_game(over_game);
        controllers_screen.render(quads, text, button_glyphs, width, height,
                                  gather_pads(companion_joined()), holds_gamepad(), pad_order,
                                  settings.device_name,
                                  companion_joined() ? host_session->companion_name()
                                                     : std::string("other device"));
    } else if (state == State::Settings) {
        settings_screen->set_over_game(over_game);
        settings_screen->render(quads, text, button_glyphs, width, height);
        update_prompt.render(quads, text, button_glyphs, width, height);
    } else if (state == State::DataManagement) {
        data_screen->set_over_game(over_game);
        data_screen->render(quads, text, button_glyphs, width, height);
    } else if (state == State::Message) {
        message.set_over_game(over_game);
        message.render(quads, text, button_glyphs, width, height);
    } else if (state == State::GameDialog) {
        game_dialog_screen.render(quads, text, width, height);
    }
}

void App::Impl::render() {
    const float width = static_cast<float>(output_width);
    const float height = static_cast<float>(output_height);
    // The quick menu put away once it has finished going.
    settle_menu_leave();
    // A GL error is sticky and otherwise invisible: the picture is simply wrong
    // and nothing says why. Reported once, since a failing draw fails every
    // frame and one line is the useful amount.
    gl::log_error_once("something in the last frame was rejected by OpenGL");
    // First, so this window's own pass is the last to lay out the pages: what
    // a tap here lands on is where they are drawn here.
    render_menu_overlay();
    gl::bind_window_framebuffer();

    if (state == State::Forwarder) {
        quads.begin_frame(output_width, output_height);
        forwarder_screen->render(quads, text, button_glyphs, width, height);
        swap();
        ++frames_rendered;
        return;
    }
    if (state == State::Browser) {
        browser->render(quads, text, button_glyphs, width, height);
        swap();
        ++frames_rendered;
        return;
    }
    if (state == State::Launcher) {
        launcher->render(quads, text, button_glyphs, width, height,
                         companion_session ? static_cast<int>(companion_session->hosts().size())
                                           : 0);
        update_prompt.render(quads, text, button_glyphs, width, height);
        swap();
        ++frames_rendered;
        return;
    }
    if (state == State::HostPicker) {
        host_picker->render(quads, text, button_glyphs, width, height,
                            companion_session->hosts(), companion_session->status_detail());
        swap();
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
        swap();
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
        // Its Controller Order screen and Graphics Packs page are drawn here
        // too, the way the quick menu is; anything else of the host's own --
        // Settings, Data Management -- is about that machine, and is looked
        // at there.
        const net::ControllersViewMessage& view = companion_session->host_controllers_view();
        const net::GraphicPacksViewMessage& packs = companion_session->host_graphic_packs_view();
        const net::AmiiboBrowserViewMessage& amiibo = companion_session->host_amiibo_browser_view();
        const net::ButtonMappingViewMessage& mapping = companion_session->host_button_mapping_view();
        const net::WiiUPlayerViewMessage& player = companion_session->host_wii_u_player_view();
        const auto draw_page = [&](bool over_game) {
            if (view.open) {
                render_mirrored_controllers(mirrored_controllers, quads, text, button_glyphs, width,
                                            height, view, over_game);
            } else if (amiibo.open) {
                if (!over_game) {
                    quads.begin_frame(output_width, output_height);
                    quads.clear(ui_theme().background);
                }
                std::vector<GameBrowserEntry> entries;
                for (const net::AmiiboBrowserViewEntry& entry : amiibo.entries) {
                    entries.push_back(GameBrowserEntry{entry.label, entry.is_directory, entry.is_parent});
                }
                mirrored_amiibo.set_amiibo(true);
                mirrored_amiibo.show_mirrored(amiibo.directory, entries, amiibo.focus);
                mirrored_amiibo.set_over_game(over_game);
                mirrored_amiibo.render(quads, text, button_glyphs, width, height);
            } else if (mapping.open) {
                if (!over_game) {
                    quads.begin_frame(output_width, output_height);
                    quads.clear(ui_theme().background);
                }
                // The host's mapping, which this device's pad follows too.
                show_button_mapping(mirrored_button_mapping);
                ScreenSource* const drawing = active_source();
                mirrored_button_mapping.set_touch_screen(drawing != nullptr ? drawing->screens().bottom : nullptr);
                mirrored_button_mapping.show_mirrored(mapping, width, height);
                mirrored_button_mapping.set_over_game(over_game);
                mirrored_button_mapping.render(quads, text, button_glyphs, width, height);
            } else if (player.open) {
                if (!over_game) {
                    quads.begin_frame(output_width, output_height);
                    quads.clear(ui_theme().background);
                }
                show_host_wii_u_player(mirrored_wii_u_player, *companion_session, width, height);
                mirrored_wii_u_player.set_over_game(over_game);
                mirrored_wii_u_player.render(quads, text, button_glyphs, width, height);
            } else {
                render_mirrored_graphic_packs(mirrored_graphic_packs, quads, text, button_glyphs,
                                              width, height, packs, over_game);
            }
        };
        if (view.open || packs.open || amiibo.open || mapping.open || player.open) {
            // Over the game, darkened as the host's own page is: live for a
            // Wii U game, which goes on, and the blurred still otherwise.
            if (ScreenSource* drawing = active_source()) {
                float panel_width = 0.0f;
                float panel_height = 0.0f;
                panel_size(panel_width, panel_height);
                compositor.set_display_size(panel_width, panel_height);
                apply_lcd_grid();
                compositor.render(quads, device_layout(), drawing->screens(), screens_geometry(),
                                  width, height, router);
                if (system == SystemId::WiiU && !companion_session->host_paused()) {
                    menu_backdrop.forget();
                    quads.draw_solid(PixelRect{0.0f, 0.0f, width, height},
                                     Color{0.0f, 0.0f, 0.0f, kLiveGameDim});
                } else {
                    if (!menu_backdrop.matches(compositor.composition_key())) {
                        menu_backdrop.capture(output_width, output_height,
                                              compositor.composition_key());
                    }
                    menu_backdrop.draw(quads, width, height);
                    quads.draw_solid(PixelRect{0.0f, 0.0f, width, height},
                                     Color{0.0f, 0.0f, 0.0f, 0.6f});
                }
                draw_page(/*over_game=*/true);
            } else {
                draw_page(/*over_game=*/false);
            }
        } else {
            render_centred_message(quads, text, width, height, "Look at the other screen.");
        }
        // An update the host asked for here, going.
        update_prompt.render(quads, text, button_glyphs, width, height);
        swap();
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
            swap();
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
        swap();
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
            picker->set_preview_screen(own_separate_screen());
            picker->set_device_screen(wii_u_screen_here());
            if (!is_companion() && system == SystemId::WiiU) picker->set_wii_u_controllers(wii_u_controller_rows());
            refresh_game_icon();
            picker->set_game_icon(game_icon.valid() ? &game_icon : nullptr);
            picker->render(quads, text, button_glyphs, width, height, shown_game_title(), nullptr);
        } else if (state != State::Playing || quick_menu_leaving()) {
            // Under the menu, or a page opened from it, coming or going:
            // nothing, the game being in Cemu's window rather than this one.
            // "Playing on Cemu" would show through the menu's darkness.
            quads.begin_frame(output_width, output_height);
            quads.clear(ui_theme().background);
        } else {
            const SystemProfile& profile = system_profile(system);
            render_waiting_screen(quads, text, button_glyphs, width, height,
                                  "Playing on " + profile.core_description, {game_title}, {});
        }
    } else if (ScreenSource* drawing = active_source()) {
        const ScreenTextures& textures = drawing->screens();
        float panel_width = 0.0f;
        float panel_height = 0.0f;
        panel_size(panel_width, panel_height);
        compositor.set_display_size(panel_width, panel_height);
        apply_lcd_grid();
        compositor.render(quads, device_layout(), textures, screens_geometry(), width, height,
                          router);
        draw_stylus_cursor(width, height);
        frames = &textures;
    } else {
        quads.begin_frame(output_width, output_height);
        quads.clear(compositor.style().background);
    }

    // Every page over a game has the same background as the quick menu: the
    // game, paused, blurred and darkened. A page with no game behind it has
    // the app's own background -- and a page opened from the menu of a game
    // that has since ended is one of those: what was there is gone, and a
    // picture of it would say otherwise.
    const bool game_behind =
        local_game() != nullptr || companion_session != nullptr ||
        (system == SystemId::WiiU && external_source && external_source->running());
    const bool show_menu_backdrop =
        game_behind &&
        (state == State::Menu || state == State::Message ||
         ((state == State::Settings || state == State::DataManagement) &&
          settings_came_from == State::Menu) ||
         (state == State::Controllers && controllers_came_from == State::Menu) ||
         (on_cemu_page() && cemu_page_came_from == State::Menu));
    draw_entering(width, height, [&] {
        if (show_menu_backdrop && system == SystemId::WiiU && wii_u_backdrop_ready()) {
            // Paused, and its picture here: as a DS or 3DS game is.
            menu_backdrop.draw(quads, width, height);
            if (state == State::Settings || state == State::DataManagement || state == State::Controllers ||
                on_cemu_page()) {
                quads.draw_solid(PixelRect{0.0f, 0.0f, width, height}, Color{0.0f, 0.0f, 0.0f, 0.6f});
            }
        } else if (show_menu_backdrop && system == SystemId::WiiU &&
                   !(is_companion() && companion_session && companion_session->host_paused())) {
            // A Wii U game going on under the menu -- one that did not pause, or
            // whose picture has yet to arrive -- is shown live rather than
            // blurred, under the same darkness on every page: the menu's first
            // page as much as the ones opened from it. On the other device, a
            // paused one has stopped sending pictures, and is blurred below like
            // any other console's.
            if (!wii_u_paused) menu_backdrop.forget();
            quads.draw_solid(PixelRect{0.0f, 0.0f, width, height}, Color{0.0f, 0.0f, 0.0f, kLiveGameDim});
        } else if (show_menu_backdrop) {
            // Taken here, between the game and the menu, so what gets blurred is
            // exactly the frame underneath it -- and taken again when that frame
            // stops being the one that was copied, which is what picking a layout
            // does while the menu is still up.
            if (!menu_backdrop.matches(compositor.composition_key())) {
                menu_backdrop.capture(output_width, output_height, compositor.composition_key());
            }
            menu_backdrop.draw(quads, width, height);
            // A page opened from the menu -- Settings, Data Management,
            // Controllers -- sits a step further back from the game: the same
            // picture, 60% darker again.
            if (state == State::Settings || state == State::DataManagement || state == State::Controllers ||
                on_cemu_page()) {
                quads.draw_solid(PixelRect{0.0f, 0.0f, width, height}, Color{0.0f, 0.0f, 0.0f, 0.6f});
            }
        } else if (quick_menu_leaving()) {
            // Going away over the game, which is going again under it: a Wii
            // U's darkened, as it is under a menu over it as it plays; the
            // blurred picture otherwise, as it was taken.
            if (system == SystemId::WiiU) {
                quads.draw_solid(PixelRect{0.0f, 0.0f, width, height}, Color{0.0f, 0.0f, 0.0f, kLiveGameDim});
            } else {
                menu_backdrop.draw(quads, width, height);
            }
        } else {
            menu_backdrop.forget();
        }
    }, [&] { render_pages(width, height, show_menu_backdrop || quick_menu_leaving(), frames); });
    if (const std::string notice = shown_corner_notice(); !notice.empty()) {
        render_corner_notice(quads, text, width, height, notice);
    } else if (state == State::Companion && companion_session &&
               !companion_session->host_corner_notice().empty()) {
        // The host's, shown here as well while it shows it.
        render_corner_notice(quads, text, width, height, companion_session->host_corner_notice());
    } else if (state == State::Companion && companion_session &&
               companion_session->host_menu_unavailable_notice()) {
        render_corner_notice(quads, text, width, height, kMenuUnavailableNotice);
    }
    if (state == State::ResumeNotice ||
               (state == State::Companion && companion_session &&
                companion_session->host_resume_notice())) {
        render_notice_popup(quads, text, button_glyphs, width, height,
                            state == State::ResumeNotice ? game_notice : std::string(kResumedNotice));
    }
    if (state == State::Companion && mirrored_game_dialog.is_open()) {
        mirrored_game_dialog.render(quads, text, width, height);
    }
    // An update the host asked this device for, going; Settings draws its
    // own.
    if (is_companion() && state != State::Settings) {
        update_prompt.render(quads, text, button_glyphs, width, height);
    }

    swap();
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
    if (!options.keep_log_file) {
        ensure_directory(path_dirname(impl.paths.log_file));
        if (set_log_file(impl.paths.log_file)) {
            log_info("dualscreen starting; log at " + impl.paths.log_file);
        }
    } else {
        log_info("dualscreen starting, in the log that was already open");
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
    apply_favorite_color_accent(impl.settings.ds_favorite_color);
    impl.adopt_legacy_handedness();
    impl.apply_button_style();
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
    impl.save_store.reset(new SaveStore(impl.paths.saves_dir, impl.settings.device_name));
    impl.update_save_rom_dirs();
    net::SaveExchangeService::Config save_config;
    save_config.systems = impl.save_systems();
    impl.save_service.reset(new net::SaveExchangeService(*impl.save_store, save_config));
    {
        std::string error;
        if (!impl.save_service->start(&error)) {
            // Another copy of the app is answering on this machine already.
            log_warn("not answering other devices about saves: " + error);
            impl.save_service.reset();
        }
    }

    apply_controller_hints();

    // Video and events are the only subsystems worth failing over. The game
    // controller subsystem is initialised separately because it can fail on its
    // own -- device permissions differ inside Game Mode's sandbox -- and losing
    // controllers is a reason to warn, never a reason to refuse to start.
    // And no on-screen keyboard but for a name being typed: on a Steam Deck
    // SDL raises Steam's the moment text input starts, so it is let to only
    // then -- see show_steam_keyboard(). See create_gl_window(), which also
    // turns text input off.
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
                 (video::can_encode() ? "available" : "not in this build") + "; decode: " +
                 (video::can_decode() ? "available" : "not in this build"));
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
    log_info(impl.settings.cursor_mode == CursorMode::Hidden ? "the cursor is hidden in games"
                                                             : "the cursor shows in games");

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
        if (!options.keep_log_file) close_log_file();
        return 1;
    }
    if (!impl.quads.initialize(&error) || !impl.text.initialize(&error) ||
        !impl.button_glyphs.initialize(&error)) {
        log_error("cannot set up the renderer: " + error);
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        impl.destroy_window();
        SDL_Quit();
        if (!options.keep_log_file) close_log_file();
        return 1;
    }

    impl.input.initialize(impl.settings);
    impl.picker.reset(new LayoutPicker(impl.registry));
    impl.picker->set_repeat_source(
        [&input = impl.input](UiAction direction) { return input.direction_repeating(direction); });
    impl.apply_system();
    impl.settings_screen.reset(new SettingsScreen(impl.paths));
    if (system_keyboard_available()) impl.settings_screen->set_text_editor(&edit_with_system_keyboard);
    impl.settings_screen->set_offers_data_management(impl.save_store != nullptr);
    // Settings > melonDS: how the Switch's melonDS runs, in its melonds.ini.
    // Elsewhere only when DUALSCREEN_MELONDS_INI names a file to show it
    // with, for working on the page away from a Switch.
#ifdef __SWITCH__
#ifdef DS_DEKO_CORE
    impl.settings_screen->set_melonds_page(true, kMelonDsTuningPath);
#else
    impl.settings_screen->set_melonds_page(false, kMelonDsTuningPath);
#endif
#else
    if (const char* ini = std::getenv("DUALSCREEN_MELONDS_INI"); ini != nullptr && *ini != '\0') {
        impl.settings_screen->set_melonds_page(true, ini);
    }
#endif
    impl.start_updater(options);
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
    } else if (options.game_path.empty() && options.leave_instead_of_launcher) {
        // Somebody else's main menu sent the player here to pick a game, so
        // that is what they get -- see AppOptions::leave_instead_of_launcher.
        impl.open_browser();
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
    } else if (options.start_screen == "forwarder-preview") {
        impl.preview_forwarders.reset(new PreviewForwarderPlatform());
        impl.open_browser();
    } else if (options.start_screen == "settings") {
        impl.open_settings();
    } else if (options.start_screen == "emulators") {
        impl.open_settings();
        impl.poll_updates();
        impl.settings_screen->open_emulators_page();
    } else if (options.start_screen == "updates") {
        // Settings, with its Updates row chosen: a real check, for seeing
        // what the popups that follow look like.
        impl.open_settings();
        impl.update_prompt.choose();
    } else if (options.start_screen == "controllers") {
        impl.open_controllers();
    } else if (options.start_screen == "data") {
        impl.settings_came_from = Impl::State::Launcher;
        impl.open_data_management();
    }

    {
        std::string sound_error;
        if (impl.ui_sounds.open(&sound_error)) {
            log_info(std::string("menu sounds ready") + (impl.settings.ui_sounds ? "" : ", but turned off in Settings"));
        } else {
            log_info("no menu sounds: " + sound_error);
        }
        impl.ui_sounds.set_enabled(impl.settings.ui_sounds);
    }

    // --- main loop -----------------------------------------------------------
    const uint64_t counter_frequency = SDL_GetPerformanceFrequency();
    impl.last_counter = SDL_GetPerformanceCounter();
    const uint64_t started_counter = impl.last_counter;
    // --simulate-key's keys, which may be several separated by commas -- a
    // menu walked through in steps -- each tapped a few frames after the last.
    std::vector<std::string> simulated_keys;
    {
        std::string rest = options.simulate_key;
        while (!rest.empty()) {
            const std::size_t comma = rest.find(',');
            simulated_keys.push_back(rest.substr(0, comma));
            rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
        }
    }
    std::size_t simulated_next = 0;
    int simulated_at_frame = 0;

    while (impl.running) {
#ifdef __SWITCH__
        // The console can take the app away -- the HOME menu's close, a
        // system update -- and this is how it says so.
        if (!appletMainLoop()) {
            log_info("the console asked this app to close");
            impl.running = false;
            impl.quit_chosen = true;
            break;
        }
#endif
        // A companion showing a stream waits out most of the refresh first,
        // so reading input and the network, decoding and drawing all happen
        // just before the refresh the result is shown at. See FrameDelay.
        if (impl.state == Impl::State::Companion) {
            const uint64_t wait = impl.frame_delay.wait_us();
            if (wait > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(wait));
            }
            if (impl.frame_delay.active() != impl.frame_delay_said) {
                impl.frame_delay_said = impl.frame_delay.active();
                log_info(impl.frame_delay_said
                             ? "drawing each frame just before the display's refresh (every " +
                                   std::to_string(impl.frame_delay.period_us() / 100 / 10.0)
                                       .substr(0, 4) +
                                   "ms), waiting " + std::to_string(wait / 1000) +
                                   "ms first so the newest picture is the one shown"
                             : "no longer waiting before each frame: the display's refresh is "
                               "not steady enough to aim at");
            }
        }
        // A game played by Cemu paces this loop instead of this window's own
        // refresh: each pass waits for its next frame and takes it the moment
        // it is announced. This window is not what anybody is watching while
        // Cemu plays -- its own window is -- and a loop held to the refresh of
        // a window in the background was only ever as regular as the
        // compositor made it: every frame that arrived twice within one of
        // its passes was one this app never took.
        if (impl.vsync_held_off && steady_micros() >= impl.vsync_held_off_until_us) {
            impl.vsync_held_off = false;
            log_info("waiting for the display again; measuring its refresh first");
        }
        impl.update_swap_pacing();
        if (impl.paced_by_external_game()) {
            const int longest_ms = std::max(
                4, static_cast<int>(impl.frame_period * 1000.0 * 1.5 + 0.5));
            impl.external_source->wait_for_frame(longest_ms);
            impl.report_external_frames();
        }
        impl.pass_began_us = steady_micros();
        impl.pass_times = Impl::PassTimes();
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
            // Away, the display may have been doing anything: its period is
            // measured again before frames are paced by it.
            impl.locked_period_us = 0;
            impl.locked_learn_count = 0;
            impl.locked_queue_full = false;
            // And whatever it did meanwhile is not the display falling
            // behind: the first frames after a Switch wakes wait while its
            // display comes back, and taken for falling behind they held
            // vsync off -- for longer each time -- and the game ran unpaced.
            impl.vsync_held_off = false;
            impl.vsync_hold_off_us = 10'000'000;
            impl.locked_waits_in_window = 0;
            impl.resume_settle_until_us = steady_micros() + 3'000'000;
            impl.audio.flush();
            impl.refresh_output_size();
            impl.flush_saves();
        }

        impl.last_delta_seconds = elapsed;
        impl.update_auto_render_scale();
        if (impl.solo_screen_unchecked) {
            impl.update_solo_screen();
        }
        // Read from the setting every frame rather than pushed in when it
        // changes. An "apply this setting" step is a step that can be forgotten,
        // and forgetting it made every pointer mode behave as the default.
        impl.cursor.set_mode(impl.settings.cursor_mode);
        impl.cursor.set_choosing_point(
            (impl.state == Impl::State::ButtonMapping && impl.button_mapping_screen.placing()) ||
            (impl.companion_session && impl.companion_session->host_button_mapping_view().open &&
             impl.companion_session->host_button_mapping_view().placing));
        impl.cursor.update(elapsed);
        impl.input.begin_frame();
        impl.tick_identify();

        // Test hook: hold a touch at a fixed spot by pushing the same events a
        // real device would, so everything downstream is the production path.
        // From the second frame, once the window is the size it will be, so
        // the finger goes down where it stays.
        if (options.simulate_touch_x >= 0.0f && options.simulate_touch_y >= 0.0f && impl.frames_rendered >= 2) {
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
        if (simulated_next < simulated_keys.size() && impl.frames_rendered >= 2 &&
            !still_arriving && impl.frames_rendered >= simulated_at_frame) {
            const std::string& name = simulated_keys[simulated_next++];
            simulated_at_frame = impl.frames_rendered + 6;
            const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
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
                log_warn("--simulate-key does not name a key SDL knows: " + name);
            }
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
                    // Losing focus often precedes being suspended -- and in
                    // Game Mode, pressing the Steam button or putting the Deck
                    // to sleep is how a game is put down.
                    impl.flush_saves("the window lost the focus");
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
#ifdef __SWITCH__
        impl.apply_button_style();
#endif

        // Touches are mapped against what this device is actually showing, not
        // against the shared layout. On "one screen per device" the two differ
        // completely: the device holding the bottom screen fills its window
        // with it, while the shared layout still describes a stacked pair, so
        // mapping against the shared one put every touch in the wrong place.
        const Layout touch_layout = impl.device_layout();
        float panel_width = 0.0f;
        float panel_height = 0.0f;
        impl.panel_size(panel_width, panel_height);
        impl.input.end_frame(touch_layout, static_cast<float>(impl.output_width),
                             static_cast<float>(impl.output_height), impl.screens_geometry(),
                             panel_width, panel_height);

        if (impl.input.quit_requested() && impl.running) {
            // The window being closed, or the system asking the app to end --
            // SIGTERM arrives as the same event. Said, because whether an
            // "Exit game" from SteamOS lands here (and so writes the save and
            // the resume point on the way out) or kills the app outright is
            // only knowable from a log on the device.
            log_info("asked to close by the window or the system; leaving normally");
            impl.running = false;
            impl.quit_chosen = true;
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
            impl.host_session->set_codec_choice(impl.codec_choice());
            impl.host_session->set_audio_enabled(
                audio_reaches(impl.audio_target, impl.host_session->companion_screen()));
            // A menu on either device pauses the game, so the two are always
            // looking at the same frame -- except a Wii U game, which is only
            // paused once it says it has, and goes on under the menu when it
            // does not. The other device blurs the picture only then.
            const bool paused_here =
                impl.state != Impl::State::Playing &&
                (impl.system != SystemId::WiiU || impl.wii_u_paused);
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
                                   impl.state == Impl::State::Controllers ||
                                   impl.state == Impl::State::DataManagement ||
                                   impl.on_cemu_page();
            shared.graphic_packs_available = impl.graphic_packs_available();
            // As the menu has it while it is up: what the game wants can
            // change under it, and the two devices' rows have to match.
            shared.amiibo_available = impl.picker && impl.picker->is_open()
                                          ? impl.picker->state().amiibo_available
                                          : impl.amiibo_in_menu();
            shared.favorite_color = impl.settings.ds_favorite_color;
            // Whatever is in this device's corner is in the other's too.
            shared.corner_notice = impl.shown_corner_notice();
            if (impl.picker && impl.system == SystemId::WiiU) {
                shared.producer_options = impl.picker->state().producer_options;
                shared.producer_choices = impl.picker->state().producer_choices;
                shared.producer_controls_from = impl.picker->state().producer_controls_from;
                shared.amiibo = impl.picker->state().amiibo;
                shared.amiibo_folder = impl.picker->state().amiibo_folder;
            }
            // Every controller, whatever is up: the player's page needs them
            // as much as the menu does.
            shared.wii_u_controllers = impl.wii_u_controller_rows();
            shared.menu_focus = impl.picker ? impl.picker->focus() : 0;
            shared.menu_confirm =
                impl.picker ? static_cast<uint8_t>(impl.picker->confirmation()) : 0;
            shared.menu_confirm_focus =
                impl.picker ? static_cast<uint8_t>(impl.picker->confirm_focus_wire()) : 0;
            // Only that notice: the other device has its own words for it,
            // and none for an amiibo's.
            shared.menu_unavailable_notice =
                impl.corner_notice_up() && impl.corner_notice == kMenuUnavailableNotice;
            shared.max_render_scale = impl.max_render_scale();
            shared.cursor_choosable = impl.pointer_choosable();
            shared.cursor_shown = impl.settings.cursor_mode == CursorMode::Shown;
            shared.right_stick_choosable =
                impl.local_game() && system_profile(impl.system).offers_right_stick_mode;
            shared.button_mapping = impl.button_mapping;
            shared.rumble_choosable = impl.source && impl.source->offers_rumble_pak();
            shared.rumble_pak = impl.source && impl.source->rumble_pak();
            shared.custom_textures_choosable = impl.source && impl.source->offers_custom_textures();
            shared.custom_textures = impl.source && impl.source->custom_textures();
            shared.lcd_filter_choosable = impl.system == SystemId::Ds;
            shared.lcd_filter = impl.lcd_filter_in_effect();
            shared.save_points_available = impl.save_points_available();
            shared.save_sync_available = impl.save_sync_available();
            shared.resume_choosable = impl.save_points_available();
            shared.resume_from_save_state = impl.resume_in_effect();
            shared.resume_notice = impl.state == Impl::State::ResumeNotice;
            impl.host_session->set_session_state(shared);
            impl.publish_game_dialog();
            impl.publish_mirrored_pages();
            // And the Controller Order screen, for the other device to show
            // the same -- see ControllersViewMessage. Closed is a message too:
            // it is how the other device learns to put it away.
            if (impl.state == Impl::State::Controllers) {
                impl.host_session->set_controllers_view(controllers_view_of(
                    impl.controllers_screen, impl.gather_pads(impl.companion_joined()),
                    impl.holds_gamepad(), impl.pad_order, impl.settings.device_name,
                    impl.host_session->companion_name()));
            } else {
                impl.host_session->set_controllers_view(net::ControllersViewMessage());
            }
            // And the Graphics Packs page, the same way -- made again only
            // when something on it has changed: it is every pack Cemu has,
            // descriptions and all, and the cursor moving is most changes.
            if (impl.state == Impl::State::GraphicPacks) {
                const GraphicPacksScreen& packs = impl.graphic_packs_screen;
                const std::array<int64_t, 7> key{
                    static_cast<int64_t>(packs.packs_revision()), packs.focus(), packs.in_pack() ? 1 : 0,
                    packs.item(), packs.choosing() ? packs.choice_focus() + 1 : 0, packs.waiting() ? 1 : 0,
                    packs.restart_pending() ? 1 : 0};
                if (!impl.graphic_packs_view_sent || key != impl.graphic_packs_view_key) {
                    impl.host_session->set_graphic_packs_view(graphic_packs_view_of(packs));
                    impl.graphic_packs_view_key = key;
                    impl.graphic_packs_view_sent = true;
                }
            } else if (impl.graphic_packs_view_sent) {
                impl.host_session->set_graphic_packs_view(net::GraphicPacksViewMessage());
                impl.graphic_packs_view_sent = false;
            }
            // And a Wii U player's page, which is a few numbers too.
            {
                const net::WiiUPlayerViewMessage player =
                    impl.state == Impl::State::WiiUPlayer ? wii_u_player_view_of(impl.wii_u_player_screen)
                                                          : net::WiiUPlayerViewMessage();
                if (player.open || impl.wii_u_player_view_sent) {
                    if (!impl.wii_u_player_view_sent || !(player == impl.wii_u_player_view_said)) {
                        impl.host_session->set_wii_u_player_view(player);
                        impl.wii_u_player_view_said = player;
                    }
                    impl.wii_u_player_view_sent = player.open;
                }
            }
            // And Button Mapping, which is a few numbers.
            {
                const net::ButtonMappingViewMessage mapping =
                    impl.state == Impl::State::ButtonMapping ? button_mapping_view_of(impl.button_mapping_screen)
                                                             : net::ButtonMappingViewMessage();
                if (mapping.open || impl.button_mapping_view_sent) {
                    if (!impl.button_mapping_view_sent || !(mapping == impl.button_mapping_view_said)) {
                        impl.host_session->set_button_mapping_view(mapping);
                        impl.button_mapping_view_said = mapping;
                    }
                    impl.button_mapping_view_sent = mapping.open;
                }
            }
            const uint64_t poll_began_us = steady_micros();
            impl.host_session->poll(network_now);
            impl.pass_times.network_us += steady_micros() - poll_began_us;

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
                // The Pointer setting, for Cemu's window too: whether the
                // mouse pointer shows there. Only a change goes over.
                if (external_now_running) {
                    impl.external_source->set_pointer_shown(impl.settings.cursor_mode ==
                                                            CursorMode::Shown);
                }
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
                if (request.wants_lcd_filter) {
                    impl.adopt_lcd_filter(request.lcd_filter);
                }
                if (request.wants_touch_spot) {
                    impl.button_mapping_screen.set_spot(TouchSpot{request.touch_spot_u, request.touch_spot_v});
                }
                if (request.wants_rumble_pak) {
                    impl.adopt_rumble_pak(request.rumble_pak);
                }
                if (request.wants_custom_textures) {
                    impl.adopt_custom_textures(request.custom_textures);
                }
                if (request.wants_resume_from_save_state) {
                    impl.adopt_resume(request.resume_from_save_state);
                }
                if (request.wants_reset) {
                    impl.request_reset();
                }
                if (request.wants_screen_swap && impl.active_layout().swaps_screens) {
                    impl.swap_separate_screens();
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
                // The companion's presses earn sounds as this device's own do,
                // and those sounds go back to it: see play_ui_sound().
                {
                    const double now_seconds =
                        static_cast<double>(SDL_GetPerformanceCounter() - started_counter) /
                        static_cast<double>(counter_frequency);
                    impl.ui_sound_judge.note_input(
                        press.has_action ? std::vector<UiAction>{ui_action_for(press.action)}
                                         : std::vector<UiAction>{},
                        press.has_focus && !press.has_action, now_seconds);
                }
                // The resumed game's popup, shown on both devices: OK there
                // is OK here.
                if (impl.state == Impl::State::ResumeNotice) {
                    if (press.has_action && (press.action == net::MenuAction::Confirm ||
                                             press.action == net::MenuAction::Cancel)) {
                        impl.dismiss_resume_notice();
                    }
                    continue;
                }
                // A press from the other pad answers whichever of the two
                // screens this device is actually showing, and is dropped when
                // it is showing neither: a press aimed at a menu that has since
                // closed is not a press at the game.
                const bool for_menu = impl.state == Impl::State::Menu && impl.picker;
                const bool for_settings = impl.state == Impl::State::Settings ||
                                          impl.state == Impl::State::Controllers ||
                                          impl.state == Impl::State::DataManagement ||
                                          impl.on_cemu_page();
                if (!for_menu && !for_settings) continue;
                // A tap on the other device's drawing of a page, resolved
                // there to what it landed on, heard by the page with its own
                // taps -- see MenuPage::take_taps() -- as the update below,
                // with or without a press, asks it for them.
                MenuPage* const page = press.has_focus ? impl.page_shown() : nullptr;
                if (page != nullptr) page->press_entry(press.focus);
                if (!press.has_action && page != nullptr) {
                    impl.update_shown_page({});
                    continue;
                }
                if (!press.has_action) continue;
                const UiAction action = ui_action_for(press.action);
                // A popup over Settings -- an update's offer, or its answer --
                // takes the press first, as it does a press made here. Sent
                // straight to the page under it, the popup could never be
                // put away from the other device, and its A and B worked the
                // page instead.
                if (for_settings && impl.update_prompt.open()) {
                    impl.update_prompt.update({action}, {}, static_cast<float>(impl.output_width),
                                              static_cast<float>(impl.output_height));
                    continue;
                }
                if (for_settings && impl.menu_key_back_to_game({action})) continue;
                if (impl.state == Impl::State::Controllers) {
                    impl.update_controllers_screen({action}, {});
                    continue;
                }
                if (impl.state == Impl::State::GraphicPacks) {
                    impl.update_graphic_packs_screen({action}, {});
                    continue;
                }
                if (impl.state == Impl::State::Amiibo) {
                    impl.update_amiibo_browser({action}, {});
                    continue;
                }
                if (impl.state == Impl::State::ButtonMapping) {
                    impl.update_button_mapping_screen({action}, {});
                    continue;
                }
                if (impl.state == Impl::State::WiiUPlayer) {
                    impl.update_wii_u_player_screen({action}, {});
                    continue;
                }
                if (impl.state == Impl::State::DataManagement) {
                    impl.apply_data_management_result(impl.data_screen->update(
                        {action}, {}, static_cast<float>(impl.output_width),
                        static_cast<float>(impl.output_height)));
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
                impl.quit_chosen = true;
                impl.quit_from_companion = true;
            }
            // Only when --video-codec h264 forced video and it does not
            // work: the automatic choice falls back to tiles by itself.
            // Non-fatal: the local game, and whatever this device shows of
            // it, is untouched either way.
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
                impl.companion_saw_host_page = impl.companion_session->host_settings_open();
                if (impl.state == Impl::State::Menu && impl.picker) {
                    impl.picker->show(
                        impl.menu_state_from_host(), impl.companion_session->host_menu_focus(),
                        static_cast<MenuConfirm>(impl.companion_session->host_menu_confirm()),
                        impl.companion_session->host_menu_confirm_focus());
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
                // Every button does what the host's game has it doing.
                impl.adopt_button_mapping(impl.companion_session->host_button_mapping());
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
            // Up from the start, as the screen asked for: there is nothing it
            // came up over, so nothing to animate.
            impl.menu_entrance.finish();
        }
        if (open_save_points_when_playing && impl.state == Impl::State::Playing &&
            impl.source && impl.source->core_frame_for_test().valid()) {
            open_save_points_when_playing = false;
            impl.set_menu_open(true);
            // Up from the start, as the screen asked for: there is nothing it
            // came up over, so nothing to animate.
            impl.menu_entrance.finish();
            impl.picker->set_focus(LayoutPicker::focus_of(MenuSection::CreateSavePoint));
        }
        if (open_options_when_playing && impl.state == Impl::State::Playing &&
            impl.source && impl.source->core_frame_for_test().valid()) {
            open_options_when_playing = false;
            impl.set_menu_open(true);
            // Up from the start, as the screen asked for: there is nothing it
            // came up over, so nothing to animate.
            impl.menu_entrance.finish();
            impl.picker->set_focus(LayoutPicker::focus_of(MenuSection::Options, 0));
        }

        set_live_pointer(impl.input.pointer_active(), impl.input.pointer_x(),
                         impl.input.pointer_y());
        set_live_wheel(impl.input.wheel(), impl.input.mouse_x(), impl.input.mouse_y());
        // Connected to another device's game, its menus' color: the host's.
        follow_favorite_color_accent(impl.settings.ds_favorite_color,
                                     impl.companion_session
                                         ? impl.companion_session->host_favorite_color()
                                         : -1);
        {
            std::vector<PointerEvent> pointers = impl.input.pointer_events();
            // Taps on the menu as Cemu draws it over the game, which land
            // where they would on this window's copy of it: the two are
            // drawn at the same shape. See wants_menu_overlay(). Only the
            // menu's: a corner notice has nothing to tap.
            if (impl.external_source && impl.external_source->overlay_pointer_supported()) {
                // The whole of what the pointer did over it -- down, every
                // move, up, the wheel -- handed to the menu exactly as this
                // window's own pointer is: a finger drags a list there, and
                // the wheel turns it, as they do here.
                const float width = static_cast<float>(impl.output_width);
                const float height = static_cast<float>(impl.output_height);
                ExternalOverlayPointer over;
                float wheel = 0.0f;
                float wheel_x = 0.0f;
                float wheel_y = 0.0f;
                bool moved = false;
                while (impl.external_source->take_overlay_pointer(over)) {
                    const float x = over.x * width;
                    const float y = over.y * height;
                    if (!impl.menu_over_wii_u_game()) {
                        impl.overlay_pointer_down = false;
                        continue;
                    }
                    moved = true;
                    switch (over.kind) {
                        case ExternalOverlayPointerKind::Down: {
                            impl.overlay_pointer_down = true;
                            impl.overlay_press_x = x;
                            impl.overlay_press_y = y;
                            PointerEvent pressed;
                            pressed.normalized = false;
                            pressed.pressed = true;
                            pressed.x = pressed.press_x = x;
                            pressed.y = pressed.press_y = y;
                            pointers.push_back(pressed);
                            break;
                        }
                        case ExternalOverlayPointerKind::Move: break;
                        case ExternalOverlayPointerKind::Up: {
                            if (!impl.overlay_pointer_down) break;
                            impl.overlay_pointer_down = false;
                            PointerEvent released;
                            released.normalized = false;
                            released.released = true;
                            released.x = x;
                            released.y = y;
                            released.press_x = impl.overlay_press_x;
                            released.press_y = impl.overlay_press_y;
                            pointers.push_back(released);
                            break;
                        }
                        case ExternalOverlayPointerKind::Wheel:
                            wheel += over.wheel;
                            wheel_x = x;
                            wheel_y = y;
                            break;
                    }
                    if (over.kind != ExternalOverlayPointerKind::Wheel) {
                        impl.overlay_pointer_x = x;
                        impl.overlay_pointer_y = y;
                    }
                }
                if (moved || impl.overlay_pointer_down) {
                    set_live_pointer(impl.overlay_pointer_down, impl.overlay_pointer_x, impl.overlay_pointer_y);
                }
                if (wheel != 0.0f) set_live_wheel(wheel, wheel_x, wheel_y);
            } else if (impl.external_source) {
                float press_x = 0.0f, press_y = 0.0f, x = 0.0f, y = 0.0f;
                while (impl.external_source->take_overlay_tap(press_x, press_y, x, y)) {
                    if (!impl.menu_over_wii_u_game()) continue;
                    PointerEvent tap;
                    tap.normalized = false;
                    tap.released = true;
                    tap.press_x = press_x * static_cast<float>(impl.output_width);
                    tap.press_y = press_y * static_cast<float>(impl.output_height);
                    tap.x = x * static_cast<float>(impl.output_width);
                    tap.y = y * static_cast<float>(impl.output_height);
                    pointers.push_back(tap);
                }
            }
            const bool tapped = std::any_of(pointers.begin(), pointers.end(),
                                            [](const PointerEvent& pointer) { return pointer.released; });
            const double now_seconds = static_cast<double>(SDL_GetPerformanceCounter() - started_counter) /
                                       static_cast<double>(counter_frequency);
            // Tab or F1 pressed in Cemu's window, which has the keyboard
            // while a Wii U game plays in it: the same as pressing it here.
            std::vector<UiAction> ui_actions = impl.input.ui_actions();
            if (impl.external_source && impl.external_source->take_menu_key()) {
                log_info("the menu key was pressed in Cemu's window");
                ui_actions.push_back(UiAction::ToggleMenu);
            }
            // And the keys that move through the menu, which Cemu's window
            // hears while the menu is drawn over it: whatever they would do
            // in this one, a held arrow repeating as one held here does.
            // Escape is Back in the menu, as here, and opens it when it is
            // not up, which Cemu sends as Open.
            if (impl.external_source) {
                const bool menu_there = impl.menu_over_wii_u_game();
                ExternalMenuKey key = ExternalMenuKey::Up;
                bool down = false;
                while (impl.external_source->take_menu_keyboard(key, down)) {
                    if (key == ExternalMenuKey::Open) {
                        if (down) {
                            log_info("Escape was pressed in Cemu's window");
                            ui_actions.push_back(UiAction::OpenMenu);
                        }
                        continue;
                    }
                    if (!menu_there) continue;
                    static const UiAction kDirections[4] = {UiAction::Up, UiAction::Down,
                                                            UiAction::Left, UiAction::Right};
                    const int direction = static_cast<int>(key);
                    if (direction < 4) {
                        impl.input.set_forwarded_direction_held(direction, down);
                        if (down) ui_actions.push_back(kDirections[direction]);
                        continue;
                    }
                    if (!down) continue;
                    switch (key) {
                        case ExternalMenuKey::PageUp: ui_actions.push_back(UiAction::PageUp); break;
                        case ExternalMenuKey::PageDown: ui_actions.push_back(UiAction::PageDown); break;
                        case ExternalMenuKey::Confirm: ui_actions.push_back(UiAction::Confirm); break;
                        case ExternalMenuKey::Back: ui_actions.push_back(UiAction::Cancel); break;
                        case ExternalMenuKey::Filter: ui_actions.push_back(UiAction::ToggleFilter); break;
                        case ExternalMenuKey::SwapLayout: ui_actions.push_back(UiAction::SwapLayout); break;
                        case ExternalMenuKey::Settings: ui_actions.push_back(UiAction::MenuSettings); break;
                        case ExternalMenuKey::Reset: ui_actions.push_back(UiAction::MenuReset); break;
                        case ExternalMenuKey::Quit: ui_actions.push_back(UiAction::MenuQuit); break;
                        case ExternalMenuKey::Disconnect:
                            ui_actions.push_back(UiAction::MenuDisconnect);
                            break;
                        default: break;
                    }
                }
                // A key let go after the menu closed never says so here.
                if (!menu_there) {
                    for (int direction = 0; direction < 4; ++direction) {
                        impl.input.set_forwarded_direction_held(direction, false);
                    }
                }
            }
            // A finger going down hides the cursor, and the pad's next press
            // in a menu only brings it back -- see menu_cursor_input().
            ui_actions = menu_cursor_input(ui_actions, pointers, impl.state != Impl::State::Playing);
            begin_menu_frame();
            impl.ui_sound_judge.note_input(ui_actions, tapped, now_seconds);
            impl.handle_ui(ui_actions, pointers);
            // The host's page drawn here, scrolled where the host has it --
            // or, scrolled here, the host told.
            impl.sync_companion_scroll();
            // The keyboard types into a name being typed, from the next
            // frame's events on -- and on a Steam Deck, Steam's own keyboard
            // comes up for it, there being no other.
            {
                // A game's keyboard is typed on the companion when it is the
                // GamePad; here otherwise.
                const bool typing = (impl.state == Impl::State::Settings && impl.settings_screen &&
                                     impl.settings_screen->wants_text()) ||
                                    (impl.state == Impl::State::GameDialog && impl.game_dialog_screen.typing() &&
                                     !impl.companion_types_game_dialog()) ||
                                    (impl.state == Impl::State::Companion &&
                                     impl.mirrored_game_dialog.typing_here());
                if (typing != impl.input.text_entry()) impl.show_steam_keyboard(typing);
                impl.input.set_text_entry(typing);
            }
            impl.update_wii_u_pause();
            const UiSoundView view = impl.sound_view();
            // Nothing for a press that closes the app: the sound would be cut
            // off part way through as it goes.
            if (const std::optional<UiSound> sound = impl.ui_sound_judge.observe(view, now_seconds);
                sound && impl.running) {
                impl.play_ui_sound(*sound, view);
            }
            if (impl.companion_session) {
                net::MenuSound from_host;
                while (impl.companion_session->take_menu_sound(from_host)) {
                    const std::optional<UiSound> sound = ui_sound_from_wire(static_cast<uint8_t>(from_host));
                    if (sound && impl.host_decides_sounds()) impl.ui_sounds.play(*sound);
                }
            }
        }

        // Emulation only advances while the game is actually in front; opening
        // the layout menu pauses it, which is what makes comparing layouts easy.
        // A menu on the *other* device counts just as much: otherwise one player
        // reads a menu while the game carries on without them.
        // Opening the menu pauses the game, wherever it was opened from: the
        // host holds the one menu, so its own state already says so.
        //
        // Except a game played by another program, which this app cannot stop:
        // a Wii U game pauses itself when asked (see update_wii_u_pause()),
        // and with the setting off goes on running under a menu. Either way
        // its link goes on being read -- it carries the menu's own taps and
        // key, and the game's answer about pausing -- and whatever it sends
        // passed on, to a companion that would otherwise freeze. Only the pads
        // are held back from it; see pads_to_game.
        const bool page_over_game =
            impl.state == Impl::State::Menu || impl.state == Impl::State::Settings ||
            impl.state == Impl::State::Controllers || impl.state == Impl::State::DataManagement ||
            impl.state == Impl::State::Message || impl.state == Impl::State::GameDialog ||
            impl.on_cemu_page();
        const bool game_running = impl.state == Impl::State::Playing ||
                                  (impl.external_source && page_over_game);
        const bool pads_to_game = impl.state == Impl::State::Playing;
        if (impl.system == SystemId::WiiU && !impl.is_companion() && impl.external_source) impl.keep_pad_order();
        // A controller the game was rumbling goes still under a menu, and
        // takes up again as the game has it when the menu goes.
        if (impl.rumble_quiet == pads_to_game) {
            impl.rumble_quiet = !pads_to_game;
            impl.apply_pad_rumble();
        }
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
        if (!(playing != nullptr && game_running) && impl.speed_window_start != 0) {
            impl.reset_speed_window();
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
            if (pads_to_game) {
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
                    impl.apply_stylus_cursor(combined);
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
            } else if (impl.external_source) {
                // A menu is up over a game played by another program, paused
                // or not: nothing pressed reaches it, the same as the paused
                // case below.
                impl.external_source->submit_input(DsInputState());
                const RoutedPads routed = route_pads(impl.known_pads, impl.pad_slots);
                for (int player = 0; player < kPadPlayers; ++player) {
                    impl.external_source->submit_player_input(player, routed.player_fed[player],
                                                              DsInputState());
                }
            }

            // Put away while the game is not being played, and told to the
            // other device either way.
            if (!pads_to_game) impl.stylus_cursor.hide();
            impl.publish_stylus_cursor();

            impl.frame_accumulator += elapsed;
            // Cap catch-up so a hitch cannot spiral into a long fast-forward.
            const int max_frames_per_iteration = 4;
            int advanced = 0;
            // A game played by another program runs on its own clock: taking
            // its frames is collecting what it has made, every pass, not
            // stepping a core -- so it is not held to this clock's ticks, which
            // left a frame that landed just before a tick waiting a whole pass.
            if (impl.external_source && impl.frame_accumulator < impl.frame_period) {
                impl.frame_accumulator = impl.frame_period;
            }
            // With the window waiting for each refresh, a refresh is the
            // clock: one frame for each, the way melonDS's own Switch app runs
            // a DS game, rather than frames at the game's own rate -- a DS's
            // is 59.83 a second, a display's 60, and paced by the game's own
            // clock one refresh in every few hundred had no new frame and
            // showed the last again. The game runs that much fast; the audio
            // already follows its queue (see AudioOutput). Only where the two
            // rates are that close, never for a game paced by another
            // program, and not while another device is connected: this loop
            // carries its input too, and once a refresh is too seldom for it.
            impl.note_display_refresh();
            const bool display_locked =
                impl.vsync_in_effect() && !impl.external_source && impl.display_refresh_hz > 0.0 &&
                std::abs(impl.display_refresh_hz * impl.frame_period - 1.0) < 0.015 &&
                !impl.companion_joined();
            const double frame_step =
                display_locked ? 1.0 / impl.display_refresh_hz : impl.frame_period;
            // Locked, refreshes are counted, not thresholded: a pass that came
            // round a little early is still a refresh, and one that came round
            // twice as late is two.
            int frames_due =
                display_locked ? static_cast<int>(std::floor(impl.frame_accumulator / frame_step + 0.5))
                               : max_frames_per_iteration;
#ifdef __SWITCH__
            // Paced by the clock (see the end of the loop), each pass is a
            // refresh: one frame, and more only for a pass that came round a
            // refresh or more late. Counted from the time that passed
            // instead, the clock's few passes a minute more than the display
            // were empty ones -- the display showing a frame again.
            if (display_locked && impl.locked_period_us != 0) {
                const double late = impl.frame_accumulator / frame_step;
                frames_due = late > 1.5 ? static_cast<int>(std::floor(late + 0.5)) : 1;
                impl.frame_accumulator = static_cast<double>(frames_due) * frame_step;
            }
#endif
            impl.display_locked_now = display_locked;
            if (display_locked && impl.locked_queue_full) {
                // The last frame went up with one still waiting to be shown:
                // the display is a refresh behind this loop, and every press
                // a refresh later on screen for it. One refresh without a new
                // frame -- the one waiting fills it -- puts that right, and
                // the game is a frame later for it, never a frame shown twice.
                impl.locked_queue_full = false;
                frames_due = 0;
                impl.frame_accumulator = std::max(0.0, impl.frame_accumulator - frame_step);
                ++impl.speed_queue_waits;
                // Not a frame shown late: the one waiting is shown meanwhile.
                impl.last_frame_presented_us = 0;
            }
            const uint64_t core_started = SDL_GetPerformanceCounter();
            while ((display_locked ? advanced < frames_due
                                   : impl.frame_accumulator >= impl.frame_period) &&
                   advanced < max_frames_per_iteration) {
                std::string frame_error;
                const uint64_t frame_started = SDL_GetPerformanceCounter();
                const bool ran = playing->advance_frame(&frame_error);
                if (ran && impl.source) {
                    const double took =
                        static_cast<double>(SDL_GetPerformanceCounter() - frame_started) /
                        static_cast<double>(counter_frequency);
                    impl.speed_slowest_frame = std::max(impl.speed_slowest_frame, took);
                    if (took > impl.frame_period) ++impl.speed_long_frames;
                }
                if (!ran) {
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
                impl.frame_accumulator -= frame_step;
                ++advanced;
            }
            if (impl.frame_accumulator > impl.frame_period * max_frames_per_iteration) {
                impl.frame_accumulator = 0.0;
            }
            if (advanced > 0) impl.core_frame_undrawn = true;
            impl.pass_times.frames += advanced;
            impl.pass_times.core_us += static_cast<uint64_t>(
                static_cast<double>(SDL_GetPerformanceCounter() - core_started) * 1e6 /
                static_cast<double>(counter_frequency));
            if (impl.source) {
                impl.speed_core_seconds +=
                    static_cast<double>(SDL_GetPerformanceCounter() - core_started) /
                    static_cast<double>(counter_frequency);
                impl.speed_frames += advanced;
                impl.speed_passes += 1;
                if (advanced > 1) ++impl.speed_catch_ups;
                // By the clock, from the start of the first pass counted, so
                // the core's time and the window's are the same span.
                if (impl.speed_window_start == 0) impl.speed_window_start = core_started;
                impl.speed_seconds =
                    static_cast<double>(SDL_GetPerformanceCounter() - impl.speed_window_start) /
                    static_cast<double>(counter_frequency);
                if (impl.speed_seconds >= kSpeedReportSeconds) {
                    impl.report_speed();
                }
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
                    impl.flush_saves();
                }
                impl.resume_point_accumulator += elapsed;
                if (impl.resume_point_accumulator >= kResumePointIntervalSeconds) {
                    impl.write_resume_point("half a minute of play");
                }
            }

            if (playing->wants_shutdown()) {
                impl.running = false;
            }
        }

        const uint64_t stream_began_us = steady_micros();
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
            const SystemScreens& native = system_profile(impl.system).screens;
            const auto full_resolution = [&](ScreenId screen) {
                const ScreenSize size = screen == ScreenId::Top ? native.top : native.bottom;
                return impl.host_session->sends_video(screen, size.width * impl.effective_render_scale,
                                                      size.height * impl.effective_render_scale);
            };
            if (impl.host_session->companion_wants(ScreenId::Top)) {
                if (full_resolution(ScreenId::Top)) {
                    playing->read_screen(ScreenId::Top, top, native.top.width * impl.effective_render_scale,
                                         native.top.height * impl.effective_render_scale);
                } else {
                    playing->read_screen(ScreenId::Top, top);
                }
            }
            if (impl.host_session->companion_wants(ScreenId::Bottom)) {
                if (full_resolution(ScreenId::Bottom)) {
                    playing->read_screen(ScreenId::Bottom, bottom,
                                         native.bottom.width * impl.effective_render_scale,
                                         native.bottom.height * impl.effective_render_scale);
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
        impl.pass_times.stream_us += steady_micros() - stream_began_us;

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

        // Drawn at most once a game frame while the game beside it sets the
        // pace: without the refresh to hold it, a pass woken by every audio
        // message would draw this window hundreds of times a second -- on the
        // GPU the game itself is running on.
        //
        // And a game played here, without the refresh to hold it either, is
        // drawn when its core has made a frame and not in between: nothing on
        // screen changes between two of them, and drawing the same picture on
        // every pass of a loop that comes round each millisecond is hundreds
        // of draws a second on the thread the core runs on. A Steam Deck
        // shrugs that off; a Switch, running the core on the same cores the
        // GPU driver is using, ran the game well short of full speed.
        const uint64_t render_now_us = steady_micros();
        // Locked to the display and paced by the clock (see below), the
        // same: a pass with no new frame draws nothing, so nothing but new
        // frames waits to be shown. Until the display's period is known, every
        // pass draws, so that the swap waits for the refresh that measures it.
        const bool core_paced = (!impl.vsync_in_effect() ||
                                 (impl.display_locked_now && impl.locked_period_us != 0)) &&
                                !impl.paced_by_external_game() &&
                                impl.state == Impl::State::Playing && impl.source != nullptr;
        const bool draw_this_pass =
            core_paced ? impl.core_frame_undrawn
            : !impl.paced_by_external_game() ||
                render_now_us - impl.last_render_us >=
                    static_cast<uint64_t>(impl.frame_period * 1'000'000.0 * 0.9);
        if (draw_this_pass) {
            const bool new_core_frame = impl.core_frame_undrawn;
            impl.last_render_us = render_now_us;
            impl.core_frame_undrawn = false;
            impl.render();
            const uint64_t presented_us = steady_micros();
            impl.pass_times.draw_us += presented_us - render_now_us;
            if (new_core_frame && impl.state == Impl::State::Playing && impl.source != nullptr) {
                const double refresh_us = 1'000'000.0 / (impl.display_refresh_hz > 0.0
                                                             ? impl.display_refresh_hz
                                                             : 1.0 / impl.frame_period);
                const uint64_t draw_us = presented_us - render_now_us;
                impl.speed_draw_us += draw_us;
                impl.speed_slowest_draw_us = std::max(impl.speed_slowest_draw_us, draw_us);
                ++impl.speed_draws;
                if (impl.last_frame_presented_us != 0 && impl.speed_window_start != 0 &&
                    static_cast<double>(presented_us - impl.last_frame_presented_us) > refresh_us * 1.5) {
                    ++impl.speed_frame_gaps;
                }
                impl.last_frame_presented_us = presented_us;
            }
#ifdef __SWITCH__
            // A frame that waited for a buffer found the queue full: one frame
            // on the display and two more waiting. Until the display's period is known, those
            // waits are what measures it -- the time across a run of them,
            // each held to a refresh. After, one means the loop has got a
            // frame ahead (see locked_queue_full).
            if (new_core_frame && impl.display_locked_now) {
                // Mesa waits for a free buffer when a frame starts drawing,
                // not in the swap: the whole of drawing and presenting is
                // what waits.
                const uint64_t waited_us = presented_us - render_now_us;
                const bool waited = waited_us > 4000;
                // A display that is not taking frames at its own pace -- one
                // held a tenth of a second, or several finding the queue full
                // within a second -- would hold the game to whatever pace it
                // is managing, in fits and starts. Vsync is held off instead,
                // and tried again later: whatever makes the display fall
                // behind -- or drawing slow enough to look like it, which a
                // leak in deko3d once did (see melonds_deko/gfx.cpp) -- costs
                // the picture, not the game. A wait of a second or more is
                // the app being put away mid-frame, not the display, and the
                // frame clock already starts over for that.
                if (waited) {
                    if (presented_us - impl.locked_waits_window_us > 1'000'000) {
                        impl.locked_waits_window_us = presented_us;
                        impl.locked_waits_in_window = 0;
                    }
                    ++impl.locked_waits_in_window;
                }
                const bool falling_behind =
                    presented_us > impl.resume_settle_until_us &&
                    ((waited_us > 100'000 && waited_us < 1'000'000) ||
                     (impl.locked_period_us != 0 && impl.locked_waits_in_window >= 3));
                if (falling_behind) {
                    impl.vsync_held_off = true;
                    impl.vsync_held_off_until_us = presented_us + impl.vsync_hold_off_us;
                    char text[200];
                    std::snprintf(text, sizeof(text),
                                  "the display is not keeping up (a frame waited %.1f ms, %d "
                                  "waits in a second); not waiting for it for %d s",
                                  waited_us / 1000.0, impl.locked_waits_in_window,
                                  static_cast<int>(impl.vsync_hold_off_us / 1'000'000));
                    log_info(text);
                    impl.vsync_hold_off_us = std::min<uint64_t>(impl.vsync_hold_off_us * 2, 120'000'000);
                    impl.locked_period_us = 0;
                    impl.locked_learn_count = 0;
                    impl.locked_queue_full = false;
                    impl.locked_waits_in_window = 0;
                } else if (impl.locked_period_us == 0) {
                    constexpr int kLearnRefreshes = 120;
                    if (!waited) {
                        impl.locked_learn_count = 0;
                    } else if (impl.locked_learn_count++ == 0) {
                        impl.locked_learn_start_us = presented_us;
                    } else if (impl.locked_learn_count > kLearnRefreshes) {
                        const uint64_t measured =
                            (presented_us - impl.locked_learn_start_us) / kLearnRefreshes;
                        // Only a measurement within a couple of percent of what
                        // the display says it does is its refresh. Frames that
                        // waited on drawing rather than on the display -- a
                        // busy 3D scene, the frames after a reset -- come
                        // further apart than that, and a run of them taken for
                        // the period paced every frame after it: the game ran
                        // slow for as long as it was played.
                        const double nominal_us =
                            impl.display_refresh_hz > 0.0 ? 1'000'000.0 / impl.display_refresh_hz : 0.0;
                        if (nominal_us > 0.0 &&
                            std::abs(static_cast<double>(measured) / nominal_us - 1.0) > 0.02) {
                            char text[160];
                            std::snprintf(text, sizeof(text),
                                          "frames came %.3f ms apart, not the display's %.3f ms; "
                                          "measuring its refresh again",
                                          measured / 1000.0, nominal_us / 1000.0);
                            log_info(text);
                            impl.locked_learn_count = 0;
                        } else {
                            impl.locked_period_us = measured;
                            impl.locked_since_us = presented_us;
                            // Every frame waited while measuring; none of that is
                            // the display falling behind.
                            impl.locked_waits_in_window = 0;
                            impl.locked_waits_window_us = presented_us;
                            char text[128];
                            std::snprintf(text, sizeof(text),
                                          "the display refreshes every %.3f ms; a frame goes up "
                                          "each refresh, with no more than one waiting",
                                          impl.locked_period_us / 1000.0);
                            log_info(text);
                        }
                    }
                } else if (waited) {
                    impl.locked_queue_full = true;
                    ++impl.speed_swap_waits;
                }
                // A minute locked without trouble: the next hold-off, if any,
                // starts short again.
                if (impl.locked_period_us != 0 && presented_us - impl.locked_since_us > 60'000'000) {
                    impl.vsync_hold_off_us = 10'000'000;
                }
            }
#endif
        }

        if (!impl.vsync_in_effect()) {
            // Without vsync, hand the CPU back rather than spinning.
            SDL_Delay(1);
        }
#ifdef __SWITCH__
        // Locked to the display, a pass starts every refresh by the clock,
        // not when the swap lets go: three buffers let the swap return with
        // two frames still waiting to be shown, every press two refreshes
        // late. A fifth of a percent faster than the display, as measured, so
        // the loop is never the one behind; when it gets a frame ahead, see
        // locked_queue_full.
        if (impl.display_locked_now && impl.state == Impl::State::Playing &&
            impl.locked_period_us != 0) {
            const uint64_t period_us = impl.locked_period_us * 998 / 1000;
            const uint64_t now_us = steady_micros();
            if (impl.locked_next_pass_us == 0 || now_us > impl.locked_next_pass_us + period_us) {
                impl.locked_next_pass_us = now_us + period_us;
            } else {
                if (impl.locked_next_pass_us > now_us) {
                    std::this_thread::sleep_for(
                        std::chrono::microseconds(impl.locked_next_pass_us - now_us));
                }
                impl.locked_next_pass_us += period_us;
            }
        } else {
            impl.locked_next_pass_us = 0;
            impl.locked_queue_full = false;
        }
#endif
        if (impl.state == Impl::State::Playing && impl.source != nullptr) {
            impl.report_slow_pass(steady_micros() - impl.pass_began_us);
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
    // Where the game was, for Resume from Save State: taken before the core
    // closes, and on the disk before anything below that might not answer.
    impl.write_resume_point("the app is closing");
    impl.resume_writer.finish();
    if (impl.source) {
        impl.source->close();
        impl.source.reset();
        impl.note_game_closed();
    }

    // Everything after this is a call into something that may not answer.
    // Each step is named in the log as it starts, and if they have not all
    // finished within kShutdownDeadline the process ends without them.
    ShutdownWatchdog watchdog(kShutdownDeadline);
    if (impl.host_session) {
        watchdog.at("saying goodbye to the other device");
        impl.host_session->stop(impl.quit_from_companion);
        impl.host_session.reset();
    }
    if (impl.companion_session) {
        watchdog.at("leaving the other device's session");
        impl.companion_session->disconnect();
        impl.companion_session.reset();
    }
    watchdog.at("closing the audio device");
    impl.audio.close();
    impl.ui_sounds.close();
    // Before the window goes away rather than with everything else at the end:
    // the emulator has a window of its own, and the tidy order to close two is
    // the one the player would have chosen.
    if (impl.external_program.running()) {
        watchdog.at("stopping the emulator this app started");
    }
    impl.external_program.stop();
    impl.external_source.reset();
    impl.note_external_game_closed();
    watchdog.at("letting go of the controllers");
    impl.input.shutdown();
    watchdog.at("closing the window");
    impl.destroy_window();
    SDL_Quit();
    watchdog.finish();
    log_info("dualscreen exiting normally");
    if (!options.keep_log_file) close_log_file();
    return 0;
}

}  // namespace ds
