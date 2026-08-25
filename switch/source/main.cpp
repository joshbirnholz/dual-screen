// dualscreen companion for the Nintendo Switch.
//
// It runs no core and holds no ROM. It finds a device on the LAN that is
// already playing something, joins as that game's top or bottom screen, draws
// whatever arrives, and sends its own buttons and touches back. Everything the
// two devices have to agree on -- the layout, smoothing, handedness, the gap,
// which device makes the sound -- belongs to the host, exactly as it does
// between two desktop devices; this end asks and then follows.
//
// It is the desktop app's own code doing nearly all of it: the wire protocol,
// discovery, the session, the layout arithmetic, the controller mapping, the
// compositor that draws the screens, and the quick menu itself. Not a port of
// them -- the same source files, compiled again. What is left here is the loop
// around them and the console's own start-up.
//
// That is deliberate down to the pixels. A player moving between the two
// devices should find the same menu with the same entries in the same places,
// and every rule about how a screen is drawn -- the margin that drops away on
// a small display, the console artwork behind the Standard layout, when a
// screen is smoothed -- should be the one rule rather than two that agree
// until one of them is changed.
//
// The same source builds and runs on a desktop, where it is the easiest way to
// exercise all of that against a real host -- see tests/run_switch_client.sh.
// Only start-up and shutdown differ, which is the whole of the __SWITCH__ in
// this file.

#include <SDL.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "audio/audio_output.hpp"
#include "config/settings.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/input_manager.hpp"
#include "layout/layout.hpp"
#include "layout/layout_registry.hpp"
#include "net/companion_session.hpp"
#include "net/transport.hpp"
#include "render/bezel_art.hpp"
#include "render/gl_loader.hpp"
#include "render/compositor.hpp"
#include "render/gl_window.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"
#include "ui/connecting_screen.hpp"
#include "ui/launcher_screen.hpp"
#include "ui/layout_picker.hpp"
#include "util/log.hpp"

namespace ds::sw {
namespace {

constexpr SDL_Color kTextColour{0xf2, 0xf5, 0xf8, 0xff};
constexpr SDL_Color kDimColour{0x8a, 0x94, 0xa0, 0xff};
constexpr SDL_Color kFocusColour{0x6e, 0xc0, 0xff, 0xff};

struct Options {
    // Which screen to be. Given on the command line, this device joins the
    // first host offering it without anybody choosing -- the same as the
    // desktop app's --connect, and what a launcher shortcut wants.
    bool auto_connect = false;
    ScreenId wanted_screen = ScreenId::Bottom;
    std::string device_name;
    bool windowed = false;
    bool verbose = false;
    // Test hooks, the same ones the desktop app carries.
    double exit_after_seconds = 0.0;
    double screenshot_at_seconds = 0.0;
    std::string screenshot_path;
    std::string parse_error;
};

std::string next_argument(int argc, char** argv, int& index, const char* flag,
                          std::string& parse_error) {
    if (index + 1 >= argc) {
        parse_error = std::string(flag) + " needs a value";
        return std::string();
    }
    return argv[++index];
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--connect") {
            const std::string value =
                next_argument(argc, argv, index, "--connect", options.parse_error);
            if (value == "top") {
                options.wanted_screen = ScreenId::Top;
            } else if (value == "bottom") {
                options.wanted_screen = ScreenId::Bottom;
            } else if (options.parse_error.empty()) {
                options.parse_error = "--connect wants top or bottom";
            }
            options.auto_connect = true;
        } else if (argument == "--name") {
            options.device_name = next_argument(argc, argv, index, "--name", options.parse_error);
        } else if (argument == "--windowed") {
            options.windowed = true;
        } else if (argument == "--verbose") {
            options.verbose = true;
        } else if (argument == "--exit-after-seconds") {
            options.exit_after_seconds = std::strtod(
                next_argument(argc, argv, index, "--exit-after-seconds", options.parse_error).c_str(),
                nullptr);
        } else if (argument == "--screenshot-at-seconds") {
            options.screenshot_at_seconds =
                std::strtod(next_argument(argc, argv, index, "--screenshot-at-seconds",
                                          options.parse_error)
                                .c_str(),
                            nullptr);
        } else if (argument == "--screenshot") {
            options.screenshot_path =
                next_argument(argc, argv, index, "--screenshot", options.parse_error);
        } else if (!argument.empty() && argument[0] == '-') {
            options.parse_error = "unknown option " + argument;
        }
        if (!options.parse_error.empty()) {
            break;
        }
    }
    return options;
}

bool write_screenshot(int width, int height, const std::string& path) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    gl::ReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        log_warn("could not open " + path);
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    // GL hands back the bottom row first.
    std::vector<uint8_t> row(static_cast<std::size_t>(width) * 3);
    for (int y = height - 1; y >= 0; --y) {
        const uint8_t* source = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            row[static_cast<std::size_t>(x) * 3 + 0] = source[x * 4 + 0];
            row[static_cast<std::size_t>(x) * 3 + 1] = source[x * 4 + 1];
            row[static_cast<std::size_t>(x) * 3 + 2] = source[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    log_info("wrote " + path);
    return true;
}

class Client {
public:
    explicit Client(const Options& options) : options_(options) {}

    bool start();
    void run();
    void stop();
    // Why start() said no. On a console there is no window to have printed it
    // in and no terminal behind the app, so the caller has to be able to ask.
    const std::string& start_error() const { return start_error_; }

private:
    // Launcher is the opening screen -- which screen to be; Browsing is the
    // host picker; Playing is a live session, with or without this device's
    // own menu up over it.
    enum class State { Launcher, Browsing, Connecting, Playing, Failed };

    void open_launcher();
    void begin_search(ScreenId wanted);
    void join(const net::DiscoveredHost& host);
    void leave(const std::string& reason);

    Layout session_layout() const;
    void refresh_output_size();
    // The display the picture actually lands on, which is not always the size
    // being drawn at -- see Compositor::set_display_size().
    void panel_size(int& width, int& height) const;
    // Hands that size to everything that decides anything from it. Called
    // every frame: on a console it changes when the thing is docked or picked
    // up, which is not an event this app is told about.
    void follow_panel_size();

    void handle_actions(const std::vector<UiAction>& actions,
                        const std::vector<PointerEvent>& pointers);
    void handle_menu(const std::vector<UiAction>& actions,
                     const std::vector<PointerEvent>& pointers);
    void open_menu();
    void close_menu();
    void draw();

    Options options_;
    GlWindow gl_;
    int output_width_ = 1280;
    int output_height_ = 720;

    LayoutRegistry registry_;
    net::CompanionSession session_;
    InputManager input_;
    AudioOutput audio_;
    uint32_t audio_rate_ = 0;

    // The app's own renderer, not a second one: the same compositor draws the
    // screens here as on the device running the game, and the same picker
    // draws the menu, with the same previews and the same footer.
    QuadRenderer quads_;
    TextRenderer text_;
    Compositor compositor_;
    LocalOutputRouter router_;
    BezelArt bezel_art_;
    LayoutPicker picker_{registry_};
    LauncherScreen launcher_;
    HostPickerScreen host_picker_;

    // The panel size last acted on, so a change is noticed rather than
    // re-announced every frame.
    int panel_width_ = 0;
    int panel_height_ = 0;

    // The host being joined, kept so the connecting screen can name it and its
    // game before the handshake has said anything.
    net::DiscoveredHost joining_;

    State state_ = State::Launcher;
    std::string start_error_;
    std::string failure_;
    bool menu_open_ = false;
    bool running_ = true;
};

bool Client::start() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        start_error_ = std::string("SDL would not start: ") + SDL_GetError();
        log_error(start_error_);
        return false;
    }
    log_info("SDL is up");
    // Audio is asked for separately, and its failure is not this program's
    // failure: a second screen with no sound is still a second screen.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log_warn(std::string("no sound on this device: ") + SDL_GetError());
    }
    // The same convention the app uses: face buttons are read by where they
    // are, not by the letter printed on them.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kFaceButtonsByPosition);

    GlWindowRequest request;
    request.title = "dualscreen companion";
    request.width = 1280;
    request.height = 720;
    request.windowed = options_.windowed;
    // No core runs here, so any context the driver offers will do.
    request.need = GlContextNeed::Any;
    if (!create_gl_window(request, gl_, &start_error_)) {
        log_error(start_error_);
        return false;
    }

    std::string error;
    if (!quads_.initialize(&error) || !text_.initialize(&error)) {
        start_error_ = "the renderer would not start: " + error;
        log_error(start_error_);
        return false;
    }
    // Not fatal: the Standard layout still puts the screens where the console
    // puts them, it just has nothing drawn behind them.
    if (bezel_art_.load(&error)) {
        log_info("console artwork loaded");
    } else {
        log_warn("cannot draw the console artwork: " + error);
    }
    refresh_output_size();

    input_.initialize(Settings());
    input_.set_game_input_enabled(false);

    session_.set_audio_callback(
        [this](const int16_t* samples, std::size_t frames, uint32_t rate) {
            // Whatever arrives is meant for this device: the host stops sending
            // when the sound is not supposed to come out here.
            if (rate == 0) {
                return;
            }
            if (!audio_.is_open() || audio_rate_ != rate) {
                std::string audio_error;
                if (!audio_.open(static_cast<double>(rate), &audio_error)) {
                    log_warn("cannot play the streamed sound: " + audio_error);
                    audio_rate_ = rate;  // do not try again for every packet
                    return;
                }
                audio_rate_ = rate;
                audio_.set_paused(false);
            }
            audio_.push(samples, frames);
        });

    // Straight in when the command line said which screen to be; otherwise
    // the opening screen asks.
    if (options_.auto_connect) {
        begin_search(options_.wanted_screen);
    } else {
        open_launcher();
    }
    return true;
}

void Client::stop() {
    session_.disconnect();
    audio_.close();
    // Before the context that owns them.
    bezel_art_.destroy();
    compositor_.set_bezel_art(nullptr);
    picker_.set_bezel_art(nullptr);
    text_.shutdown();
    quads_.shutdown();
    input_.shutdown();
    gl_.destroy();
    SDL_Quit();
}

void Client::open_launcher() {
    state_ = State::Launcher;
    close_menu();
    session_.disconnect();
    // The app's own opening screen, minus the two entries that mean nothing
    // here: this device has no ROMs to load and nothing in Settings that would
    // apply to it. What is left is which screen to be, and leaving.
    launcher_.open(/*runs_games=*/false);
}

void Client::begin_search(ScreenId wanted) {
    state_ = State::Browsing;
    close_menu();
    host_picker_.open(wanted);

    std::string error;
    if (!session_.begin_search(options_.device_name, wanted, &error)) {
        state_ = State::Failed;
        failure_ = error;
        log_error("cannot look for hosts: " + error);
    }
}

void Client::join(const net::DiscoveredHost& host) {
    log_info("joining " + host.device_name + " (" + host.address + ") as the " +
             to_string(host_picker_.wanted_screen()) + " screen");
    session_.connect_to(host);
    joining_ = host;
    state_ = State::Connecting;
}

void Client::leave(const std::string& reason) {
    if (!reason.empty()) {
        log_info("left the session: " + reason);
    }
    audio_.close();
    audio_rate_ = 0;
    open_launcher();
}

Layout Client::session_layout() const {
    // The companion's half of the app's device_layout(): the host's layout
    // with the host's handedness and gap choice, then resolved for the screen
    // this device was granted. None of it is decided here.
    const Layout chosen = resolve_for_handedness(
        resolve_gap_choice(registry_.resolve(session_.host_layout_id()), session_.host_screen_gap()),
        session_.host_handedness());
    if (!chosen.per_device) {
        return chosen;
    }
    return resolve_for_device(chosen, session_.granted_screen());
}

void Client::panel_size(int& width, int& height) const {
    width = output_width_;
    height = output_height_;
#ifdef __SWITCH__
    // The console hands SDL a window the size of its docked output whichever
    // way it is being held: undocked, the panel is 1280x720 and a 1080p
    // picture is scaled down onto it. Asking the console which it is gives the
    // screen that is actually in front of the player, whatever size the
    // framebuffer happens to be -- and that is what decides whether there is
    // room to spend on margin.
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    width = handheld ? 1280 : 1920;
    height = handheld ? 720 : 1080;
#endif
}

void Client::follow_panel_size() {
    int width = 0;
    int height = 0;
    panel_size(width, height);

    // Every layout decision that is about how much room there really is reads
    // this rather than the window: chiefly whether to spend any of it on
    // margin, which on a handheld it should not.
    compositor_.set_display_size(static_cast<float>(width), static_cast<float>(height));
    picker_.set_display_size(static_cast<float>(width), static_cast<float>(height));

    if (width == panel_width_ && height == panel_height_) {
        return;
    }
    panel_width_ = width;
    panel_height_ = height;
    log_info("the panel is " + std::to_string(width) + "x" + std::to_string(height));
    // How big a picture is worth sending here: more than the panel can show is
    // bandwidth spent on detail it will throw away.
    if (state_ == State::Playing) {
        session_.request_display_size(width, height);
    }
}

void Client::refresh_output_size() {
    int width = 0;
    int height = 0;
    SDL_GL_GetDrawableSize(gl_.window, &width, &height);
    if (width <= 0 || height <= 0 || (width == output_width_ && height == output_height_)) {
        return;
    }
    output_width_ = width;
    output_height_ = height;
    log_info("drawing at " + std::to_string(output_width_) + "x" +
             std::to_string(output_height_));
}

void Client::open_menu() {
    // Exactly what the app puts in it, read off the session the host owns.
    LayoutPickerState state;
    state.layout_id = session_.host_layout_id();
    state.session_active = true;  // there is always a host at the other end here
    state.bilinear_filtering = session_.host_filtering();
    state.audio_target = session_.host_audio_target();
    state.handedness = session_.host_handedness();
    state.render_scale = session_.host_render_scale();
    state.screen_gap = session_.host_screen_gap();
    state.right_stick_mode = session_.host_right_stick_mode();
    // A console has no pointer to show or hide, so that entry is not offered
    // here. It is also the one entry on this menu that belongs to the device
    // rather than to the host, which is exactly why it has nothing to say on
    // a device with no mouse.
    state.cursor_choosable = false;

    const SystemProfile& profile = system_profile(session_.system());
    state.max_render_scale = profile.max_render_scale;
    state.right_stick_choosable = profile.has_analog_stick;

    picker_.set_screens(session_.screens_geometry());
    picker_.set_bezel_art(bezel_art_.texture(session_.system()));
    picker_.set_smoothing(session_.host_filtering());
    picker_.open(state);

    menu_open_ = true;
    session_.set_menu_open(true);
    input_.set_game_input_enabled(false);
}

void Client::close_menu() {
    if (picker_.is_open()) {
        picker_.close();
    }
    if (!menu_open_) {
        return;
    }
    menu_open_ = false;
    session_.set_menu_open(false);
    input_.set_game_input_enabled(state_ == State::Playing);
}

void Client::handle_menu(const std::vector<UiAction>& actions,
                         const std::vector<PointerEvent>& pointers) {
    // The host can change any of this from its own end while this is open, so
    // the picker is told before it is asked.
    picker_.set_filtering(session_.host_filtering());
    picker_.set_audio_target(session_.host_audio_target());
    picker_.set_handedness(session_.host_handedness());
    picker_.set_render_scale(session_.host_render_scale());
    picker_.set_screen_gap(session_.host_screen_gap());
    picker_.set_right_stick_mode(session_.host_right_stick_mode());
    picker_.set_selected_layout(session_.host_layout_id());
    picker_.set_smoothing(session_.host_filtering());

    const LayoutPickerResult result =
        picker_.update(actions, pointers, static_cast<float>(output_width_),
                       static_cast<float>(output_height_));

    // Every one of these is a request. Nothing is applied here: the host owns
    // the session and broadcasts what it decided, which is the whole reason
    // the two devices cannot end up disagreeing.
    if (result.selection_changed) {
        session_.request_layout(picker_.selected_layout_id());
    }
    if (result.filtering_toggled) {
        session_.request_filtering(!session_.host_filtering());
    }
    if (result.render_scale_changed) {
        const int maximum = std::max(1, system_profile(session_.system()).max_render_scale);
        const int scale = session_.host_render_scale();
        session_.request_render_scale(scale >= maximum ? 1 : scale + 1);
    }
    if (result.audio_target_changed) {
        session_.request_audio_target(next_audio_target(session_.host_audio_target()));
    }
    if (result.handedness_changed) {
        session_.request_handedness(other_hand(session_.host_handedness()));
    }
    if (result.screen_gap_toggled) {
        session_.request_screen_gap(!session_.host_screen_gap());
    }
    if (result.right_stick_mode_changed) {
        session_.request_right_stick_mode(next_right_stick_mode(session_.host_right_stick_mode()));
    }
    if (result.reset_requested) {
        session_.request_reset();
    }

    if (result.disconnect_requested) {
        leave("you ended the session");
        return;
    }
    if (result.quit_requested) {
        // Quit means the whole session ends, not just this device's part of
        // it -- the same as choosing it on the other device.
        session_.request_quit();
        session_.disconnect();
        running_ = false;
        return;
    }
    if (result.close_requested || result.reset_requested) {
        close_menu();
    }
}

void Client::handle_actions(const std::vector<UiAction>& actions,
                            const std::vector<PointerEvent>& pointers) {
    if (state_ == State::Playing && menu_open_) {
        handle_menu(actions, pointers);
        return;
    }

    if (state_ == State::Launcher) {
        const LauncherResult result =
            launcher_.update(actions, pointers, static_cast<float>(output_width_),
                             static_cast<float>(output_height_));
        switch (result.choice) {
            case LauncherChoice::ConnectAsTopScreen:
                begin_search(ScreenId::Top);
                break;
            case LauncherChoice::ConnectAsBottomScreen:
                begin_search(ScreenId::Bottom);
                break;
            case LauncherChoice::Quit:
                running_ = false;
                break;
            default:
                break;
        }
        return;
    }

    if (state_ == State::Browsing) {
        const HostPickerResult result =
            host_picker_.update(session_.hosts(), actions, pointers,
                                static_cast<float>(output_width_),
                                static_cast<float>(output_height_));
        if (result.chosen) {
            join(result.host);
        } else if (result.cancelled) {
            // Back to choosing which screen to be, rather than out of the app:
            // that is what the button that got here undoes.
            open_launcher();
        }
        return;
    }

    for (const UiAction action : actions) {
        switch (state_) {
            case State::Connecting:
                if (action == UiAction::Cancel) {
                    leave("you stopped connecting");
                }
                break;
            case State::Failed:
                if (action == UiAction::Confirm || action == UiAction::Cancel) {
                    open_launcher();
                }
                break;
            case State::Launcher:
                break;
            case State::Playing:
                switch (action) {
                    case UiAction::ToggleMenu:
                        open_menu();
                        break;
                    case UiAction::ToggleFilter:
                        // R3, which means the same thing here as it does on the
                        // device running the game.
                        session_.request_filtering(!session_.host_filtering());
                        break;
                    case UiAction::SwapLayout:
                        // L3, likewise: the two focus layouts swap.
                        session_.request_layout(
                            registry_.resolve(session_.host_layout_id()).swap_layout_id);
                        break;
                    case UiAction::Quit:
                        break;  // Quit lives in the menu here, not on a button
                    default:
                        break;
                }
                break;
            case State::Browsing:
                break;
        }
    }
}

void Client::run() {
    const uint64_t started_ms = net::monotonic_millis();
    bool screenshot_taken = false;

    while (running_) {
#ifdef __SWITCH__
        // The console can take the app away -- sleep, the home menu, a system
        // update prompt -- and this is how it says so.
        if (!appletMainLoop()) {
            break;
        }
#endif
        const uint64_t now = net::monotonic_millis();

        input_.begin_frame();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            input_.handle_event(event);
        }
        refresh_output_size();
        follow_panel_size();

        session_.poll(now);
        switch (state_) {
            case State::Launcher:
                break;
            case State::Browsing:
                if (options_.auto_connect) {
                    for (const net::DiscoveredHost& host : session_.hosts()) {
                        if (host.offers(host_picker_.wanted_screen())) {
                            join(host);
                            break;
                        }
                    }
                }
                break;
            case State::Connecting:
                if (session_.status() == net::CompanionSession::Status::Connected) {
                    state_ = State::Playing;
                    close_menu();
                    input_.set_game_input_enabled(true);
                    session_.request_display_size(panel_width_, panel_height_);
                    // A console with a stick of its own reads the stick as that
                    // stick; one without lets it stand in for the d-pad.
                    const SystemProfile& profile = system_profile(session_.system());
                    input_.set_stick_is_dpad(!profile.has_analog_stick);
                    input_.set_right_stick_mode(session_.host_right_stick_mode());
                    compositor_.set_bezel_art(bezel_art_.texture(session_.system()));
                    log_info("connected to " + session_.game_title() + " as the " +
                             to_string(session_.granted_screen()) + " screen");
                } else if (session_.status() == net::CompanionSession::Status::Failed) {
                    state_ = State::Failed;
                    failure_ = session_.status_detail();
                    log_warn("could not join: " + failure_);
                }
                break;
            case State::Playing:
                // The host owns all of this and can change any of it from its
                // own end; this device's pad drives the same game, so it
                // follows rather than deciding.
                if (input_.right_stick_mode() != session_.host_right_stick_mode()) {
                    input_.set_right_stick_mode(session_.host_right_stick_mode());
                    log_info(std::string("following the host: right stick ") +
                             to_string(session_.host_right_stick_mode()));
                }
                if (session_.status() != net::CompanionSession::Status::Connected) {
                    const std::string detail = session_.status_detail();
                    audio_.close();
                    audio_rate_ = 0;
                    open_launcher();
                    log_info("the session ended: " + detail);
                }
                break;
            case State::Failed:
                break;
        }

        const Layout layout =
            state_ == State::Playing ? session_layout() : registry_.fallback();
        input_.end_frame(layout, static_cast<float>(output_width_),
                         static_cast<float>(output_height_), session_.screens_geometry());
        if (input_.quit_requested()) {
            running_ = false;
        }
        handle_actions(input_.ui_actions(), input_.pointer_events());

        if (state_ == State::Playing && !menu_open_) {
            session_.send_input(input_.ds_state());
        }

        draw();

        const double alive = static_cast<double>(now - started_ms) / 1000.0;
        if (options_.screenshot_at_seconds > 0.0 && !screenshot_taken &&
            !options_.screenshot_path.empty() && alive >= options_.screenshot_at_seconds) {
            screenshot_taken =
                write_screenshot(output_width_, output_height_, options_.screenshot_path);
        }
        SDL_GL_SwapWindow(gl_.window);
        if (options_.exit_after_seconds > 0.0 && alive >= options_.exit_after_seconds) {
            running_ = false;
        }
    }
}

void Client::draw() {
    const float width = static_cast<float>(output_width_);
    const float height = static_cast<float>(output_height_);
    gl::log_error_once("something in the last frame was rejected by OpenGL");

    if (state_ == State::Launcher) {
        // The app's own opening screen, so choosing which screen to be looks
        // the same here as picking it there does.
        launcher_.render(quads_, text_, width, height, input_.menu_labels(),
                         static_cast<int>(session_.hosts().size()));
        return;
    }
    if (state_ == State::Connecting) {
        render_connecting_screen(quads_, text_, width, height, joining_.device_name,
                                 joining_.game_title, input_.menu_labels());
        return;
    }
    if (state_ != State::Playing) {
        // The app's own host picker, so finding a game looks the same here as
        // it does there. A failure is shown in the same place, as its status.
        host_picker_.render(quads_, text_, width, height, session_.hosts(), input_.menu_labels(),
                            state_ == State::Failed ? failure_ : session_.status_detail());
        return;
    }

    compositor_.set_smoothing(session_.host_filtering());
    const ScreenTextures& textures = session_.screens();
    compositor_.render(quads_, session_layout(), textures, session_.screens_geometry(), width,
                       height, router_);

    if (menu_open_) {
        // The previews are drawn from the frame the game is paused on, so each
        // tile shows what picking it will actually look like -- the same as on
        // the other device, because it is the same picker.
        picker_.render(quads_, text_, width, height, session_.game_title(), input_.menu_labels(),
                       &textures);
    }
}

#ifdef __SWITCH__

// Where the log goes on a console. There is no terminal behind the app and no
// shell to have started it from, so the only way to read what it said after
// the fact is to leave it on the SD card -- and the only way to read it as it
// happens is nxlink, which needs a computer already listening.
constexpr const char* kSwitchLogPath = "sdmc:/switch/dualscreen-companion.log";

// Start-up failing has to be visible. Without this the app returns to the
// homebrew menu the instant something goes wrong, which looks exactly like it
// working and being asked to leave -- and nothing has a window yet to say
// otherwise in, so this goes to the console's own framebuffer instead.
void show_failure(const std::string& message) {
    consoleInit(nullptr);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    std::printf("\x1b[2J");
    std::printf("dualscreen companion could not start.\n\n");
    std::printf("  %s\n\n", message.c_str());
    std::printf("The whole log is on the SD card at\n");
    std::printf("  /switch/dualscreen-companion.log\n\n");
    std::printf("If it mentions memory or a renderer, start the homebrew menu\n");
    std::printf("in full-memory mode: hold R while launching a game.\n\n");
    std::printf("Press + to close.\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) != 0) {
            break;
        }
        consoleUpdate(nullptr);
    }
    consoleExit(nullptr);
}

#endif

}  // namespace
}  // namespace ds::sw

int main(int argc, char** argv) {
    using namespace ds;
    using namespace ds::sw;

    const Options options = parse_options(argc, argv);
    set_min_log_level(options.verbose ? LogLevel::Debug : LogLevel::Info);

#ifdef __SWITCH__
    // Opened before anything else can fail, and written a line at a time, so
    // the log survives whatever happens next -- including the app being killed
    // outright rather than exiting.
    set_log_file(kSwitchLogPath);
    log_info("dualscreen companion starting");

    // Sockets are not on by default, and without them there is nothing this
    // app can do at all. nxlink, if a computer is listening for it, gets the
    // same lines as they happen.
    const Result socket_result = socketInitializeDefault();
    if (R_FAILED(socket_result)) {
        const std::string reason =
            "the network would not start (error " + std::to_string(socket_result) + ")";
        log_error(reason);
        show_failure(reason);
        close_log_file();
        return 1;
    }
    nxlinkStdio();
    log_info("sockets are up");
#endif

    int status = 0;
    if (!options.parse_error.empty()) {
        log_error(options.parse_error);
        status = 2;
    } else {
        Client client(options);
        if (client.start()) {
            client.run();
        } else {
            status = 1;
#ifdef __SWITCH__
            show_failure(client.start_error().empty() ? "no reason was recorded"
                                                      : client.start_error());
#endif
        }
        client.stop();
    }

    log_info("dualscreen companion exiting");
#ifdef __SWITCH__
    socketExit();
#endif
    close_log_file();
    return status;
}
