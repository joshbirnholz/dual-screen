#include "app.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "audio/audio_output.hpp"
#include "config/game_config.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "config/sha1.hpp"
#include "core/libretro_screen_source.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/compositor.hpp"
#include "render/gl_loader.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/layout_picker.hpp"
#include "net/companion_session.hpp"
#include "net/host_session.hpp"
#include "ui/game_browser.hpp"
#include "ui/launcher_screen.hpp"
#include "ui/message_screen.hpp"
#include "ui/settings_screen.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// Anything longer than this between frames means the machine was asleep (or the
// process was stopped), not that we ran slow. The frame clock is restarted and
// the stale audio queue thrown away rather than played out.
constexpr double kSuspendGapSeconds = 1.0;

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

// Picks the core to load: an explicit --core, then settings.json, then
// $DUALSCREEN_CORE, then the usual install locations.
std::string resolve_core_path(const AppOptions& options, const Settings& settings) {
    if (!options.core_path.empty()) {
        return expand_user_path(options.core_path);
    }
    if (!settings.core_path.empty()) {
        return expand_user_path(settings.core_path);
    }
    const char* from_environment = std::getenv("DUALSCREEN_CORE");
    if (from_environment != nullptr && from_environment[0] != '\0') {
        return expand_user_path(from_environment);
    }
    for (const std::string& candidate : core_search_paths()) {
        if (file_exists(candidate)) {
            return candidate;
        }
    }
    return std::string();
}

}  // namespace

std::string usage_text() {
    return
        "dualscreen - a dual-screen shell for DS emulation\n"
        "\n"
        "Usage:\n"
        "  dualscreen --game /path/to/rom.nds [options]\n"
        "\n"
        "Options:\n"
        "  --game <path>        ROM to boot straight into\n"
        "  --core <path>        melonDS DS libretro core (.so) to load\n"
        "  --layout <id>        Override the layout for this run\n"
        "  --system-dir <path>  Folder holding your BIOS/firmware files\n"
        "  --windowed           Run in a window instead of fullscreen\n"
        "  --connect <screen>   Join the first game found on the network as its\n"
        "                       top or bottom screen, skipping the menu\n"
        "  --list-layouts       Print the available layout ids and exit\n"
        "  --verbose            Log core and frontend debug output\n"
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
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--list-layouts") {
            options.list_layouts = true;
        } else if (argument == "--exit-after-frames") {
            const std::string value =
                next_argument(argc, argv, index, "--exit-after-frames", options.parse_error);
            options.exit_after_frames = std::atoi(value.c_str());
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
    enum class State { Playing, Menu, Settings, Message, Browser, Launcher, HostPicker, Companion };

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
    LocalOutputRouter router;
    InputManager input;
    AudioOutput audio;

    std::unique_ptr<LibretroCoreScreenSource> source;
    std::unique_ptr<LayoutPicker> picker;
    std::unique_ptr<SettingsScreen> settings_screen;
    std::unique_ptr<GameBrowser> browser;
    std::unique_ptr<LauncherScreen> launcher;
    std::unique_ptr<HostPickerScreen> host_picker;
    MessageScreen message;

    // Two-device mode. Exactly one of these is live at a time: a device either
    // runs the game and serves a screen, or joins someone else's game.
    std::unique_ptr<net::HostSession> host_session;
    std::unique_ptr<net::CompanionSession> companion_session;
    ScreenId local_screen = ScreenId::Top;

    State state = State::Playing;
    bool message_is_fatal = false;
    bool running = true;
    std::string active_layout_id;
    bool bilinear_filtering = false;
    std::string game_title;
    std::string resolved_core_path;

    uint64_t last_counter = 0;
    double frame_period = 1.0 / 60.0;
    double frame_accumulator = 0.0;
    double save_flush_accumulator = 0.0;
    bool awaiting_auto_connect = false;
    bool screenshot_taken = false;
    uint32_t companion_audio_rate = 0;
    int frames_rendered = 0;

    bool create_window(std::string* error);
    void destroy_window();
    void refresh_output_size();
    const Layout& active_layout() const;
    // What this device actually draws: the chosen layout, resolved for this
    // device's role when it is a per-device layout.
    Layout device_layout() const;
    // The screen source currently feeding the compositor, whichever role we are
    // in. Null before anything is running.
    ScreenSource* active_source();
    void start_companion(ScreenId wanted_screen);
    void open_launcher();
    // Plays what arrives from the host, opening this device's audio output the
    // first time something does -- the rate is not known until then.
    void attach_companion_audio();
    // Pushes the current choice down to whichever source is drawing, and
    // remembers it. Cheap enough to call whenever it changes.
    void apply_filtering();
    // Loads a ROM and starts emulating it. On failure it puts a readable
    // message on screen and returns false; `fatal_on_failure` decides whether
    // dismissing that message quits or goes back to the browser.
    bool start_game(const std::string& rom_path, bool fatal_on_failure);
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

bool App::Impl::create_window(std::string* error) {
    // Try an OpenGL ES 2.0 context first: it is the common subset across every
    // platform on the roadmap, so the renderer is exercised against the same
    // feature set everywhere. Desktop GL 2.1 is the fallback for drivers that
    // will not hand out an ES context.
    struct ContextAttempt {
        int profile;
        int major;
        int minor;
        bool gles;
        const char* description;
    };
    // Tried in order, widening as it goes. The last entry asks for nothing at
    // all and takes whatever the driver's default context is: under gamescope
    // an explicitly-profiled request can be refused where the default succeeds,
    // and a working context of some kind always beats refusing to start.
    const ContextAttempt attempts[] = {
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, true, "OpenGL ES 2.0"},
        {SDL_GL_CONTEXT_PROFILE_ES, 3, 0, true, "OpenGL ES 3.0"},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, false, "OpenGL 2.1 compatibility"},
        {SDL_GL_CONTEXT_PROFILE_CORE, 3, 3, false, "OpenGL 3.3 core"},
        {0, 0, 0, false, "the driver's default context"},
    };

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!options.windowed) {
        // Borderless-desktop fullscreen is what gamescope expects from a
        // well-behaved client; it never asks to change the display mode.
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    std::string last_error;
    for (const ContextAttempt& attempt : attempts) {
        SDL_GL_ResetAttributes();
        if (attempt.profile != 0) {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, attempt.profile);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attempt.major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt.minor);
        }
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

        window = SDL_CreateWindow("dualscreen", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  1280, 800, flags);
        if (window == nullptr) {
            last_error = SDL_GetError();
            log_warn(std::string("no window with ") + attempt.description + ": " + last_error);
            continue;
        }
        gl_context = SDL_GL_CreateContext(window);
        if (gl_context == nullptr) {
            last_error = SDL_GetError();
            log_warn(std::string("no ") + attempt.description + " context: " + last_error);
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        SDL_GL_MakeCurrent(window, gl_context);
        std::string load_error;
        // Whether this is really an ES context is asked of the context itself
        // rather than assumed from what was requested, because the default
        // attempt does not request anything and the shaders differ.
        int profile = 0;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
        const bool is_gles = attempt.profile != 0
                                 ? attempt.gles
                                 : (profile == SDL_GL_CONTEXT_PROFILE_ES);
        if (!gl::load(is_gles, &load_error)) {
            last_error = load_error;
            log_warn(std::string("cannot use the ") + attempt.description + ": " + load_error);
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        log_info(std::string("created a ") + attempt.description + " context");
        SDL_GL_SetSwapInterval(settings.vsync ? 1 : 0);
        // No pointer on screen to begin with: an arrow parked over the game is
        // exactly the kind of thing that gives away that this is not a console.
        // It comes back while the mouse is being moved -- see CursorVisibility.
        SDL_ShowCursor(SDL_DISABLE);
        refresh_output_size();
        return true;
    }

    if (error != nullptr) {
        *error = "could not create an OpenGL window: " + last_error;
    }
    return false;
}

void App::Impl::destroy_window() {
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
    const Layout& chosen = active_layout();
    if (!chosen.per_device) {
        return chosen;
    }
    // A per-device layout with nobody on the other end would show half a game,
    // so it falls back to the whole thing until a companion is actually there.
    const bool paired = (host_session && host_session->has_companion()) ||
                        (companion_session &&
                         companion_session->status() == net::CompanionSession::Status::Connected);
    if (!paired) {
        Layout both = chosen;
        both.per_device = false;
        return both;
    }
    return resolve_for_device(chosen, local_screen);
}

ScreenSource* App::Impl::active_source() {
    if (source) return source.get();
    if (companion_session) return companion_session.get();
    return nullptr;
}

void App::Impl::apply_filtering() {
    if (ScreenSource* drawing = active_source()) {
        drawing->set_filtering(bilinear_filtering);
    }
    if (picker) {
        picker->set_filtering(bilinear_filtering);
    }
}

void App::Impl::attach_companion_audio() {
    if (!companion_session) {
        return;
    }
    companion_session->set_audio_callback(
        [this](const int16_t* samples, std::size_t frames, uint32_t rate) {
            if (!settings.stream_audio || rate == 0) {
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
    attach_companion_audio();
    std::string error;
    if (!companion_session->begin_search(settings.device_name, wanted_screen, &error)) {
        enter_message(MessageKind::Error, "Cannot look for other devices", error,
                      {"Another copy of dualscreen may already be searching on this device."},
                      false);
        return;
    }
    local_screen = wanted_screen;
    bilinear_filtering = settings.default_bilinear_filtering;
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

bool App::Impl::start_game(const std::string& rom_path, bool fatal_on_failure) {
    // Tear down anything already running, so picking a second game from the
    // browser is the same code path as booting the first.
    audio.close();
    if (source) {
        source->close();
        source.reset();
    }
    have_game_config = false;
    game_config = GameConfig();

    if (!file_exists(rom_path)) {
        enter_message(MessageKind::Error, "ROM not found", "There is no file at " + rom_path,
                      {"Check the path in this game's Steam shortcut."}, fatal_on_failure);
        return false;
    }

    game_title = path_stem(rom_path);
    active_layout_id = settings.default_layout_id;
    bilinear_filtering = settings.default_bilinear_filtering;

    const std::string rom_sha1 = Sha1::hash_file(rom_path);
    if (rom_sha1.empty()) {
        log_warn("could not hash the ROM; this session's layout will not be remembered");
    } else {
        game_config = GameConfig::load_or_default(paths, settings, rom_sha1, game_title);
        have_game_config = true;
        active_layout_id = game_config.layout_id;
        bilinear_filtering = game_config.bilinear_filtering;
    }
    if (!options.layout_id.empty()) {
        active_layout_id = options.layout_id;
    }
    // An unknown id from an old config or a typo'd --layout falls back rather
    // than leaving the player with a blank screen.
    active_layout_id = registry.resolve(active_layout_id).id;

    LibretroSourceConfig source_config;
    source_config.core_path = resolved_core_path;
    source_config.rom_path = rom_path;
    source_config.system_directory = settings.system_dir;
    source_config.save_directory = paths.saves_dir;
    // Named after the ROM rather than its hash, so saves are interchangeable
    // with RetroArch's and recognisable in a file listing.
    source_config.save_path = path_join(paths.saves_dir, path_stem(rom_path) + ".srm");

    source.reset(new LibretroCoreScreenSource());
    source->set_audio_callback([this](const int16_t* samples, std::size_t frames) {
        audio.push(samples, frames);
        // Mirrored to the companion as it is produced, so both devices hear the
        // same thing at the same time.
        if (host_session) {
            host_session->publish_audio(samples, frames,
                                        static_cast<uint32_t>(source->audio_sample_rate()));
        }
    });

    std::string source_error;
    if (!source->open(source_config, &source_error)) {
        std::vector<std::string> details;
        if (resolved_core_path.empty()) {
            details.push_back("Looked for melondsds_libretro.so in the RetroArch core folders and "
                              "/usr/lib/libretro.");
            details.push_back("Point at it directly with --core, or set \"core_path\" in " +
                              paths.settings_file + ".");
        } else {
            details.push_back("Core: " + resolved_core_path);
        }
        details.push_back("BIOS folder: " + settings.system_dir);
        source.reset();
        enter_message(MessageKind::Error, "Could not start the game", source_error, details,
                      fatal_on_failure);
        return false;
    }

    frame_period = 1.0 / std::max(1.0, source->frames_per_second());
    std::string audio_error;
    if (!audio.open(source->audio_sample_rate(), &audio_error)) {
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
    host_config.audio_sample_rate = static_cast<uint32_t>(source->audio_sample_rate());
    std::string host_error;
    if (!host_session->start(host_config, &host_error)) {
        log_warn("not hosting for other devices: " + host_error);
        host_session.reset();
    }

    if (!source->warnings().empty()) {
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
            bilinear_filtering = !bilinear_filtering;
            apply_filtering();
            if (have_game_config) {
                persist_game_config();
            } else {
                settings.default_bilinear_filtering = bilinear_filtering;
                settings.save(paths);
            }
            log_info(std::string("smoothing ") + (bilinear_filtering ? "on" : "off"));
            break;
        }
    }

    switch (state) {
        case State::Message: {
            if (message.update(actions, pointers, last_delta_seconds)) {
                message.clear();
                if (!message_is_fatal) {
                    const bool joined =
                        companion_session &&
                        companion_session->status() != net::CompanionSession::Status::Failed;
                    state = joined ? State::Companion : State::Playing;
                    input.set_game_input_enabled(true);
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
                companion_session->connect_to(result.host);
                state = State::Companion;
                // The companion's own touchscreen and buttons drive the host's
                // game, so local input is live from here on.
                input.set_game_input_enabled(true);
            }
            break;
        }

        case State::Companion: {
            for (const UiAction action : actions) {
                if (action == UiAction::ToggleMenu) {
                    // The same layout menu as on the host. Picking one asks the
                    // host to change it, and the host broadcasts the result, so
                    // both devices end up showing the same thing.
                    picker->open(active_layout_id, /*session_active=*/true,
                                 /*companion=*/true, bilinear_filtering);
                    state = State::Menu;
                    input.set_game_input_enabled(false);
                    break;
                }
            }
            if (companion_session->status() == net::CompanionSession::Status::Failed) {
                enter_message(MessageKind::Error, "Lost the connection",
                              companion_session->status_detail(), {}, false);
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
            if (result.quit_requested) {
                running = false;
            } else if (result.game_chosen) {
                settings.last_browse_dir = browser->current_directory();
                settings.save(paths);
                start_game(result.chosen_path, false);
            }
            break;
        }

        case State::Menu: {
            const LayoutPickerResult result =
                picker->update(actions, pointers, static_cast<float>(output_width),
                               static_cast<float>(output_height));
            if (result.selection_changed) {
                active_layout_id = picker->selected_layout_id();
                if (companion_session &&
                    companion_session->status() == net::CompanionSession::Status::Connected) {
                    // A companion does not own the choice: it asks, and adopts
                    // whatever the host broadcasts back.
                    companion_session->request_layout(active_layout_id);
                } else {
                    persist_game_config();
                }
            }
            if (result.filtering_toggled) {
                bilinear_filtering = picker->filtering();
                apply_filtering();
                if (have_game_config) {
                    persist_game_config();
                } else {
                    settings.default_bilinear_filtering = bilinear_filtering;
                    settings.save(paths);
                }
            }
            if (result.disconnect_requested) {
                if (host_session) {
                    host_session->disconnect_companion("you ended the session");
                    state = State::Playing;
                    input.set_game_input_enabled(true);
                } else if (companion_session) {
                    companion_session->disconnect();
                    open_launcher();
                }
            } else if (result.quit_requested) {
                running = false;
            } else if (result.open_settings) {
                settings_screen->open(settings, resolved_core_path);
                state = State::Settings;
            } else if (result.close_requested) {
                state = (companion_session && companion_session->status() ==
                                                  net::CompanionSession::Status::Connected)
                            ? State::Companion
                            : State::Playing;
                input.set_game_input_enabled(true);
            }
            break;
        }

        case State::Settings: {
            const SettingsScreenResult result =
                settings_screen->update(actions, pointers, static_cast<float>(output_width),
                                        static_cast<float>(output_height));
            if (result.settings_changed) {
                settings = settings_screen->settings();
                if (!settings.save(paths)) {
                    log_warn("could not save settings.json");
                }
                SDL_GL_SetSwapInterval(settings.vsync ? 1 : 0);
            }
            if (result.close_requested) {
                // Back out to the layout menu, which is where Settings was opened from.
                const bool paired =
                    (host_session && host_session->has_companion()) ||
                    (companion_session &&
                     companion_session->status() == net::CompanionSession::Status::Connected);
                picker->open(active_layout_id, paired, source == nullptr, bilinear_filtering);
                state = State::Menu;
            }
            break;
        }

        case State::Playing: {
            for (const UiAction action : actions) {
                if (action == UiAction::ToggleMenu) {
                    picker->open(active_layout_id,
                                 host_session && host_session->has_companion(), false,
                                 bilinear_filtering);
                    state = State::Menu;
                    input.set_game_input_enabled(false);
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

void App::Impl::render() {
    const float width = static_cast<float>(output_width);
    const float height = static_cast<float>(output_height);

    if (state == State::Browser) {
        browser->render(quads, text, width, height, input.menu_labels());
        SDL_GL_SwapWindow(window);
        ++frames_rendered;
        return;
    }
    if (state == State::Launcher) {
        launcher->render(quads, text, width, height, input.menu_labels(),
                         companion_session ? static_cast<int>(companion_session->hosts().size())
                                           : 0);
        SDL_GL_SwapWindow(window);
        ++frames_rendered;
        return;
    }
    if (state == State::HostPicker) {
        host_picker->render(quads, text, width, height, companion_session->hosts(),
                            input.menu_labels(), companion_session->status_detail());
        SDL_GL_SwapWindow(window);
        ++frames_rendered;
        return;
    }

    if (ScreenSource* drawing = active_source()) {
        compositor.render(quads, device_layout(), drawing->screens(), width, height, router);
    } else {
        quads.begin_frame(output_width, output_height);
        quads.clear(compositor.style().background);
    }

    if (state == State::Menu) {
        picker->render(quads, text, width, height, game_title, input.menu_labels());
    } else if (state == State::Settings) {
        settings_screen->render(quads, text, width, height);
    } else if (state == State::Message) {
        message.render(quads, text, width, height);
    }

    SDL_GL_SwapWindow(window);
    ++frames_rendered;
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
    // The environment is what actually differs between Game Mode and desktop,
    // so it is the first thing worth knowing when only one of them works.
    for (const char* name : {"XDG_SESSION_TYPE", "WAYLAND_DISPLAY", "DISPLAY", "SDL_VIDEODRIVER",
                             "XDG_RUNTIME_DIR", "GAMESCOPE_WAYLAND_DISPLAY", "STEAM_COMPAT_DATA_PATH"}) {
        const char* value = std::getenv(name);
        log_info(std::string("env ") + name + " = " + (value != nullptr ? value : "(unset)"));
    }

    impl.settings = Settings::load(impl.paths);
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

    // Must be set before any controller is opened. SDL's default is to report
    // face buttons by printed label; this app wants positions, so that one rule
    // -- east means A -- holds in menus and in game on every pad.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kFaceButtonsByPosition);

    // Video and events are the only subsystems worth failing over. The game
    // controller subsystem is initialised separately because it can fail on its
    // own -- device permissions differ inside Game Mode's sandbox -- and losing
    // controllers is a reason to warn, never a reason to refuse to start.
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

    std::string error;
    if (!impl.create_window(&error)) {
        log_error(error);
        log_error("no usable OpenGL context was available, so there is nothing to draw into");
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        SDL_Quit();
        close_log_file();
        return 1;
    }
    if (!impl.quads.initialize(&error) || !impl.text.initialize(&error)) {
        log_error("cannot set up the renderer: " + error);
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        impl.destroy_window();
        SDL_Quit();
        close_log_file();
        return 1;
    }

    impl.input.initialize(impl.settings);
    impl.picker.reset(new LayoutPicker(impl.registry));
    impl.settings_screen.reset(new SettingsScreen(impl.registry, impl.paths));
    impl.active_layout_id = impl.settings.default_layout_id;
    impl.resolved_core_path = resolve_core_path(options, impl.settings);

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
            impl.local_screen = wanted;
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

    // Test hook: bring a menu up before the first frame so the UI can be
    // captured without synthesising input events.
    if (options.start_screen == "layout" && impl.state == Impl::State::Playing) {
        impl.picker->open(impl.active_layout_id, false, false, impl.bilinear_filtering);
        impl.state = Impl::State::Menu;
        impl.input.set_game_input_enabled(false);
    } else if (options.start_screen == "settings" && impl.state == Impl::State::Playing) {
        impl.settings_screen->open(impl.settings, impl.resolved_core_path);
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
        impl.cursor.update(elapsed);
        impl.input.begin_frame();

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

        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            impl.input.handle_event(event);
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
                             static_cast<float>(impl.output_height));

        if (impl.input.quit_requested()) {
            impl.running = false;
        }
        // Both sessions are polled before the UI so a state change this frame
        // is acted on this frame.
        const uint64_t network_now = net::monotonic_millis();
        if (impl.host_session) {
            impl.host_session->set_audio_enabled(impl.settings.stream_audio);
            impl.host_session->set_layout(impl.active_layout_id, impl.state != Impl::State::Playing);
            impl.host_session->poll(network_now);

            // A companion can ask for a layout; the host owns the choice, so an
            // id it does not recognise is refused rather than adopted.
            std::string requested;
            if (impl.host_session->take_requested_layout(requested)) {
                const Layout& resolved = impl.registry.resolve(requested);
                if (resolved.id == requested && resolved.id != impl.active_layout_id) {
                    impl.active_layout_id = resolved.id;
                    impl.persist_game_config();
                    log_info("companion asked for the " + resolved.display_name + " layout");
                }
            }
            // Whichever screen the companion took, the host shows the other.
            impl.local_screen = impl.host_session->host_screen();
        }
        if (impl.companion_session) {
            impl.companion_session->poll(network_now);
            // A companion follows the host's layout: the host owns that choice,
            // and both devices have to agree or they draw different things.
            if (impl.state == Impl::State::Companion) {
                const std::string& host_layout = impl.companion_session->host_layout_id();
                if (!host_layout.empty() && host_layout != impl.active_layout_id) {
                    impl.active_layout_id = impl.registry.resolve(host_layout).id;
                }
                impl.local_screen = impl.companion_session->granted_screen();
            }
            // --connect joins the first host that offers the screen we asked
            // for, as soon as one appears.
            if (impl.awaiting_auto_connect &&
                impl.companion_session->status() == net::CompanionSession::Status::Searching) {
                for (const net::DiscoveredHost& host : impl.companion_session->hosts()) {
                    if (host.offers(impl.local_screen)) {
                        impl.companion_session->connect_to(host);
                        impl.awaiting_auto_connect = false;
                        break;
                    }
                }
            }
        }

        impl.handle_ui(impl.input.ui_actions(), impl.input.pointer_events());

        // Emulation only advances while the game is actually in front; opening
        // the layout menu pauses it, which is what makes comparing layouts easy.
        if (impl.source && impl.state == Impl::State::Playing) {
            // The companion's buttons and touches are merged with this device's,
            // so either player can press anything. This is the InputSink seam
            // doing exactly what it was left in place for.
            DsInputState combined = impl.input.ds_state();
            if (impl.host_session && impl.host_session->has_companion()) {
                const DsInputState& remote = impl.host_session->companion_input();
                combined.buttons |= remote.buttons;
                if (remote.touching && !combined.touching) {
                    combined.touching = true;
                    combined.touch_x = remote.touch_x;
                    combined.touch_y = remote.touch_y;
                }
            }
            impl.source->input_sink().submit(combined);

            impl.frame_accumulator += elapsed;
            // Cap catch-up so a hitch cannot spiral into a long fast-forward.
            const int max_frames_per_iteration = 4;
            int advanced = 0;
            while (impl.frame_accumulator >= impl.frame_period && advanced < max_frames_per_iteration) {
                std::string frame_error;
                if (!impl.source->advance_frame(&frame_error)) {
                    impl.enter_message(MessageKind::Error, "The core stopped running", frame_error, {},
                                       true);
                    break;
                }
                impl.frame_accumulator -= impl.frame_period;
                ++advanced;
            }
            if (impl.frame_accumulator > impl.frame_period * max_frames_per_iteration) {
                impl.frame_accumulator = 0.0;
            }
            // Hand the freshly rendered screens to the companion, if any. The
            // serial only advances when the pixels actually changed, so an
            // unchanged frame costs nothing on the wire.
            if (impl.host_session && impl.host_session->has_companion()) {
                const CoreVideoFrame& frame = impl.source->core_frame_for_test();
                const ScreenSplit& split = impl.source->split();
                if (frame.valid() && split.valid) {
                    const uint32_t* base = frame.pixels;
                    const int stride = static_cast<int>(frame.stride_pixels);
                    impl.host_session->publish_screens(
                        base + static_cast<std::size_t>(split.top_y) * stride + split.top_x,
                        base + static_cast<std::size_t>(split.bottom_y) * stride + split.bottom_x,
                        split.screen_width, split.screen_height, stride, frame.serial);
                }
            }

            impl.save_flush_accumulator += elapsed;
            if (impl.save_flush_accumulator >= kSaveFlushIntervalSeconds) {
                impl.save_flush_accumulator = 0.0;
                impl.source->flush_save_ram(false);
            }

            if (impl.source->wants_shutdown()) {
                impl.running = false;
            }
        } else if (impl.companion_session && impl.state == Impl::State::Companion) {
            impl.companion_session->send_input(impl.input.ds_state());
        }

        if (impl.source && impl.state != Impl::State::Playing) {
            // Paused: hold the sticks and buttons off the core entirely.
            impl.source->input_sink().submit(DsInputState());
            impl.audio.set_paused(true);
        }
        if (impl.source && impl.state == Impl::State::Playing) {
            impl.audio.set_paused(false);
        }

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

    if (impl.host_session) {
        impl.host_session->stop();
        impl.host_session.reset();
    }
    if (impl.companion_session) {
        impl.companion_session->disconnect();
        impl.companion_session.reset();
    }

    impl.persist_game_config();
    impl.audio.close();
    if (impl.source) {
        impl.source->close();
        impl.source.reset();
    }
    impl.input.shutdown();
    impl.destroy_window();
    SDL_Quit();
    log_info("dualscreen exiting normally");
    close_log_file();
    return 0;
}

}  // namespace ds
