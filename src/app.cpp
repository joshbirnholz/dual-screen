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
#include "input/input_manager.hpp"
#include "layout/layout_registry.hpp"
#include "render/compositor.hpp"
#include "render/gl_loader.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/layout_picker.hpp"
#include "ui/game_browser.hpp"
#include "ui/message_screen.hpp"
#include "ui/settings_screen.hpp"
#include "util/log.hpp"

namespace ds {
namespace {

// Anything longer than this between frames means the machine was asleep (or the
// process was stopped), not that we ran slow. The frame clock is restarted and
// the stale audio queue thrown away rather than played out.
constexpr double kSuspendGapSeconds = 1.0;

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
        "  --list-layouts       Print the available layout ids and exit\n"
        "  --verbose            Log core and frontend debug output\n"
        "  --help               Show this text\n"
        "\n"
        "Run with no --game to pick one from a file browser instead.\n"
        "\n"
        "Layouts are remembered per game, keyed by the ROM's SHA-1, in\n"
        "~/.config/dualscreen/games/. Open the layout menu in-game with R3 or a\n"
        "two-finger tap.\n";
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
    enum class State { Playing, Menu, Settings, Message, Browser };

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
    MessageScreen message;

    State state = State::Playing;
    bool message_is_fatal = false;
    bool running = true;
    std::string active_layout_id;
    std::string game_title;
    std::string resolved_core_path;

    uint64_t last_counter = 0;
    double frame_period = 1.0 / 60.0;
    double frame_accumulator = 0.0;
    int frames_rendered = 0;

    bool create_window(std::string* error);
    void destroy_window();
    void refresh_output_size();
    const Layout& active_layout() const;
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
    const ContextAttempt attempts[] = {
        {SDL_GL_CONTEXT_PROFILE_ES, 2, 0, true, "OpenGL ES 2.0"},
        {SDL_GL_CONTEXT_PROFILE_COMPATIBILITY, 2, 1, false, "OpenGL 2.1"},
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
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, attempt.profile);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, attempt.major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, attempt.minor);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

        window = SDL_CreateWindow("dualscreen", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  1280, 800, flags);
        if (window == nullptr) {
            last_error = SDL_GetError();
            continue;
        }
        gl_context = SDL_GL_CreateContext(window);
        if (gl_context == nullptr) {
            last_error = SDL_GetError();
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        SDL_GL_MakeCurrent(window, gl_context);
        std::string load_error;
        if (!gl::load(attempt.gles, &load_error)) {
            last_error = load_error;
            SDL_GL_DeleteContext(gl_context);
            gl_context = nullptr;
            SDL_DestroyWindow(window);
            window = nullptr;
            continue;
        }

        log_info(std::string("created a ") + attempt.description + " context");
        SDL_GL_SetSwapInterval(settings.vsync ? 1 : 0);
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

    const std::string rom_sha1 = Sha1::hash_file(rom_path);
    if (rom_sha1.empty()) {
        log_warn("could not hash the ROM; this session's layout will not be remembered");
    } else {
        game_config = GameConfig::load_or_default(paths, settings, rom_sha1, game_title);
        have_game_config = true;
        active_layout_id = game_config.layout_id;
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

    source.reset(new LibretroCoreScreenSource());
    source->set_audio_callback(
        [this](const int16_t* samples, std::size_t frames) { audio.push(samples, frames); });

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
    frame_accumulator = 0.0;
    last_counter = SDL_GetPerformanceCounter();

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
    game_config.touch_last_played();
    if (!game_config.save(paths)) {
        log_warn("could not save this game's layout");
    }
}

void App::Impl::handle_ui(const std::vector<UiAction>& actions,
                          const std::vector<PointerEvent>& pointers) {
    switch (state) {
        case State::Message: {
            if (message.update(actions, pointers)) {
                message.clear();
                if (!message_is_fatal) {
                    state = State::Playing;
                    input.set_game_input_enabled(true);
                } else if (browser) {
                    // A game that would not start is not the end of the
                    // session when the player got here through the browser --
                    // send them back to pick another.
                    open_browser();
                } else {
                    running = false;
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
                persist_game_config();
            }
            if (result.quit_requested) {
                running = false;
            } else if (result.open_settings) {
                settings_screen->open(settings, resolved_core_path);
                state = State::Settings;
            } else if (result.close_requested) {
                state = State::Playing;
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
                picker->open(active_layout_id);
                state = State::Menu;
            }
            break;
        }

        case State::Playing: {
            for (const UiAction action : actions) {
                if (action == UiAction::ToggleMenu) {
                    picker->open(active_layout_id);
                    state = State::Menu;
                    input.set_game_input_enabled(false);
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
        browser->render(quads, text, width, height);
        SDL_GL_SwapWindow(window);
        ++frames_rendered;
        return;
    }

    if (source) {
        compositor.render(quads, active_layout(), source->screens(), width, height, router);
    } else {
        quads.begin_frame(output_width, output_height);
        quads.clear(compositor.style().background);
    }

    if (state == State::Menu) {
        picker->render(quads, text, width, height, game_title);
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

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "dualscreen: could not start SDL: %s\n", SDL_GetError());
        return 1;
    }
    // Touch on a desktop should not also arrive as synthetic mouse events, or
    // every tap would register twice.
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "0");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    std::string error;
    if (!impl.create_window(&error)) {
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        SDL_Quit();
        return 1;
    }
    if (!impl.quads.initialize(&error) || !impl.text.initialize(&error)) {
        std::fprintf(stderr, "dualscreen: %s\n", error.c_str());
        impl.destroy_window();
        SDL_Quit();
        return 1;
    }

    impl.input.initialize(impl.settings);
    impl.picker.reset(new LayoutPicker(impl.registry));
    impl.settings_screen.reset(new SettingsScreen(impl.registry, impl.paths));
    impl.active_layout_id = impl.settings.default_layout_id;
    impl.resolved_core_path = resolve_core_path(options, impl.settings);

    // --- decide what to boot -------------------------------------------------
    if (options.game_path.empty()) {
        // No ROM named, so let the player pick one. Failing to start a game
        // chosen this way returns to the browser rather than ending the
        // session, which is why start_game's failures are non-fatal here.
        impl.open_browser();
    } else {
        impl.start_game(expand_user_path(options.game_path), true);
    }

    // Test hook: bring a menu up before the first frame so the UI can be
    // captured without synthesising input events.
    if (options.start_screen == "layout" && impl.state == Impl::State::Playing) {
        impl.picker->open(impl.active_layout_id);
        impl.state = Impl::State::Menu;
        impl.input.set_game_input_enabled(false);
    } else if (options.start_screen == "settings" && impl.state == Impl::State::Playing) {
        impl.settings_screen->open(impl.settings, impl.resolved_core_path);
        impl.state = Impl::State::Settings;
        impl.input.set_game_input_enabled(false);
    } else if (options.start_screen == "browser") {
        impl.open_browser();
    }

    // --- main loop -----------------------------------------------------------
    const uint64_t counter_frequency = SDL_GetPerformanceFrequency();
    impl.last_counter = SDL_GetPerformanceCounter();

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
        }

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
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    impl.audio.set_paused(true);
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    impl.audio.flush();
                    impl.audio.set_paused(false);
                }
            }
        }
        if (impl.input.window_size_changed()) {
            impl.refresh_output_size();
        }

        impl.input.end_frame(impl.active_layout(), static_cast<float>(impl.output_width),
                             static_cast<float>(impl.output_height));

        if (impl.input.quit_requested()) {
            impl.running = false;
        }
        impl.handle_ui(impl.input.ui_actions(), impl.input.pointer_events());

        // Emulation only advances while the game is actually in front; opening
        // the layout menu pauses it, which is what makes comparing layouts easy.
        if (impl.source && impl.state == Impl::State::Playing) {
            impl.source->input_sink().submit(impl.input.ds_state());

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
            if (impl.source->wants_shutdown()) {
                impl.running = false;
            }
        } else if (impl.source) {
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
    }

    if (!options.screenshot_path.empty()) {
        // Re-render once so the buffer being read is the one just drawn.
        impl.render();
        if (impl.write_screenshot(options.screenshot_path)) {
            log_info("wrote " + options.screenshot_path);
        } else {
            log_warn("could not write " + options.screenshot_path);
        }
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
    return 0;
}

}  // namespace ds
