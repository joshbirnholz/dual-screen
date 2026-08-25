// dualscreen companion for the Nintendo Switch.
//
// It runs no core and holds no ROM. It finds a device on the LAN that is
// already playing something, joins as that game's top or bottom screen, draws
// whatever arrives, and sends its own buttons and touches back. Everything the
// two devices have to agree on -- the layout, smoothing, handedness, the gap,
// which device makes the sound -- belongs to the host, exactly as it does
// between two desktop devices; this end asks and then follows.
//
// The wire protocol, discovery, the session, the layout arithmetic and the
// controller mapping are the desktop app's own, compiled unchanged, so there
// is one implementation of each rather than a second one to keep in step. What
// is here is the shell around them: a window, a renderer, a host picker and a
// small in-session menu.
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
#include "util/log.hpp"

#include "host_list.hpp"
#include "screen_quad.hpp"
#include "text_painter.hpp"

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

// One received screen as a texture. The pixels arrive at whatever size the
// host decided was worth sending -- a 3DS above native resolution sends more
// than a DS ever does -- so the texture is rebuilt when that changes rather
// than assuming the console's own size.
class ScreenTexture {
public:
    ~ScreenTexture() { destroy(); }

    void destroy() {
        if (texture_ != nullptr) {
            SDL_DestroyTexture(texture_);
            texture_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
        uploaded_serial_ = 0;
        has_frame_ = false;
    }

    // Uploads only when a frame this texture has not already been given
    // arrives, which is why the serial travels with the pixels.
    void update(SDL_Renderer* renderer, const net::CompanionSession::ScreenPixels& frame) {
        if (!frame.valid()) {
            return;
        }
        if (texture_ == nullptr || frame.width != width_ || frame.height != height_) {
            if (texture_ != nullptr) {
                SDL_DestroyTexture(texture_);
            }
            texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
            if (texture_ == nullptr) {
                log_warn(std::string("could not make a texture for a screen: ") + SDL_GetError());
                width_ = 0;
                height_ = 0;
                return;
            }
            // The screens are opaque and the wire format carries no alpha, so
            // blending them would only read whatever the unused byte holds.
            SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_NONE);
            width_ = frame.width;
            height_ = frame.height;
            uploaded_serial_ = 0;
            has_frame_ = false;
        }
        if (has_frame_ && frame.serial == uploaded_serial_) {
            return;
        }
        SDL_UpdateTexture(texture_, nullptr, frame.pixels,
                          frame.width * static_cast<int>(sizeof(uint32_t)));
        uploaded_serial_ = frame.serial;
        has_frame_ = true;
    }

    void set_smoothing(bool smooth) {
        if (texture_ != nullptr) {
            SDL_SetTextureScaleMode(texture_, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        }
    }

    SDL_Texture* texture() const { return texture_; }
    bool ready() const { return texture_ != nullptr && has_frame_; }

private:
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    uint64_t uploaded_serial_ = 0;
    bool has_frame_ = false;
};

bool write_screenshot(SDL_Renderer* renderer, int width, int height, const std::string& path) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 3);
    if (SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGB24, pixels.data(), width * 3) !=
        0) {
        log_warn(std::string("could not read the window back: ") + SDL_GetError());
        return false;
    }
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        log_warn("could not open " + path);
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", width, height);
    std::fwrite(pixels.data(), 1, pixels.size(), file);
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
    // Browsing is the host picker; Playing is a live session, with or without
    // this device's own menu up over it.
    enum class State { Browsing, Connecting, Playing, Failed };

    void begin_search();
    void join(const net::DiscoveredHost& host);
    void leave(const std::string& reason);

    Layout session_layout() const;
    void refresh_output_size();

    void handle_actions(const std::vector<UiAction>& actions);
    void handle_browsing_action(UiAction action);
    void handle_menu_action(UiAction action);

    void draw();
    void draw_browser();
    void draw_session();
    void draw_menu();
    void draw_banner(const std::string& title, const std::string& detail);

    Options options_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int output_width_ = 1280;
    int output_height_ = 720;

    LayoutRegistry registry_;
    net::CompanionSession session_;
    HostList list_;
    InputManager input_;
    AudioOutput audio_;
    uint32_t audio_rate_ = 0;
    TextPainter text_;
    ScreenTexture top_;
    ScreenTexture bottom_;

    State state_ = State::Browsing;
    std::string start_error_;
    std::string failure_;
    bool menu_open_ = false;
    int menu_focus_ = 0;
    bool running_ = true;
};

// The in-session menu. Deliberately short: everything else on the desktop's
// version of this menu is a choice the host owns and can be made there, and a
// screen you are holding in your hands is not where a settings list belongs.
const char* const kMenuEntries[] = {"Resume", "Disconnect", "Quit"};
constexpr int kMenuEntryCount = 3;

bool Client::start() {
    // Audio is asked for separately, and its failure is not this program's
    // failure: a second screen with no sound is still a second screen, and
    // taking the whole app down over the sound device is the wrong trade.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        start_error_ = std::string("SDL would not start: ") + SDL_GetError();
        log_error(start_error_);
        return false;
    }
    log_info("SDL is up");
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log_warn(std::string("no sound on this device: ") + SDL_GetError());
    }
    // The same convention the desktop app uses: face buttons are read by where
    // they are, not by the letter printed on them, so a Switch pad's east
    // button is DS A exactly as an Xbox pad's is.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kFaceButtonsByPosition);

    const uint32_t flags = options_.windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP;
    window_ = SDL_CreateWindow("dualscreen companion", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
    if (window_ == nullptr) {
        start_error_ = std::string("no window: ") + SDL_GetError();
        log_error(start_error_);
        return false;
    }
    log_info("window created");
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        // Software is slow but readable, which beats a black screen on a
        // device whose only diagnostic is what it draws.
        log_warn(std::string("no accelerated renderer (") + SDL_GetError() + "), falling back");
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
    }
    if (renderer_ == nullptr) {
        start_error_ = std::string("no renderer: ") + SDL_GetError();
        log_error(start_error_);
        return false;
    }
    SDL_RendererInfo renderer_info;
    if (SDL_GetRendererInfo(renderer_, &renderer_info) == 0 && renderer_info.name != nullptr) {
        log_info(std::string("renderer: ") + renderer_info.name);
    }
    if (!text_.create(renderer_)) {
        start_error_ = std::string("could not build the font atlas: ") + SDL_GetError();
        log_error(start_error_);
        return false;
    }
    refresh_output_size();
    log_info("output is " + std::to_string(output_width_) + "x" +
             std::to_string(output_height_));

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
                std::string error;
                if (!audio_.open(static_cast<double>(rate), &error)) {
                    log_warn("cannot play the streamed sound: " + error);
                    audio_rate_ = rate;  // do not try again for every packet
                    return;
                }
                audio_rate_ = rate;
                audio_.set_paused(false);
            }
            audio_.push(samples, frames);
        });

    list_.set_wanted_screen(options_.wanted_screen);
    begin_search();
    return true;
}

void Client::stop() {
    session_.disconnect();
    audio_.close();
    top_.destroy();
    bottom_.destroy();
    text_.destroy();
    input_.shutdown();
    if (renderer_ != nullptr) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_ != nullptr) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void Client::begin_search() {
    state_ = State::Browsing;
    menu_open_ = false;
    input_.set_game_input_enabled(false);
    top_.destroy();
    bottom_.destroy();

    std::string error;
    if (!session_.begin_search(options_.device_name, list_.wanted_screen(), &error)) {
        state_ = State::Failed;
        failure_ = error;
        log_error("cannot look for hosts: " + error);
    }
}

void Client::join(const net::DiscoveredHost& host) {
    log_info("joining " + host.device_name + " (" + host.address + ") as the " +
             to_string(list_.wanted_screen()) + " screen");
    session_.connect_to(host);
    state_ = State::Connecting;
}

void Client::leave(const std::string& reason) {
    if (!reason.empty()) {
        log_info("left the session: " + reason);
    }
    session_.disconnect();
    audio_.close();
    audio_rate_ = 0;
    begin_search();
}

Layout Client::session_layout() const {
    // The companion's half of the desktop app's device_layout(): the host's
    // layout with the host's handedness and gap choice, then resolved for the
    // screen this device was granted. None of it is decided here.
    const Layout chosen = resolve_for_handedness(
        resolve_gap_choice(registry_.resolve(session_.host_layout_id()), session_.host_screen_gap()),
        session_.host_handedness());
    if (!chosen.per_device) {
        return chosen;
    }
    return resolve_for_device(chosen, session_.granted_screen());
}

void Client::refresh_output_size() {
    int width = 0;
    int height = 0;
    SDL_GetRendererOutputSize(renderer_, &width, &height);
    if (width <= 0 || height <= 0) {
        return;
    }
    if (width == output_width_ && height == output_height_) {
        return;
    }
    output_width_ = width;
    output_height_ = height;
    // The host sends each screen at the size it will actually be drawn at, so
    // it has to be told when that changes -- docking a Switch is exactly such
    // a change.
    if (state_ == State::Playing) {
        session_.request_display_size(output_width_, output_height_);
    }
}

void Client::handle_browsing_action(UiAction action) {
    switch (action) {
        case UiAction::Up:
            list_.move_focus(-1);
            break;
        case UiAction::Down:
            list_.move_focus(1);
            break;
        case UiAction::Left:
        case UiAction::Right:
            // Which screen to be. The search itself carries it, so it is
            // restarted rather than only remembered here.
            list_.set_wanted_screen(list_.wanted_screen() == ScreenId::Top ? ScreenId::Bottom
                                                                          : ScreenId::Top);
            begin_search();
            break;
        case UiAction::Confirm:
            if (list_.focused_is_joinable()) {
                join(*list_.focused());
            }
            break;
        case UiAction::Cancel:
        case UiAction::Quit:
            running_ = false;
            break;
        default:
            break;
    }
}

void Client::handle_menu_action(UiAction action) {
    switch (action) {
        case UiAction::Up:
            menu_focus_ = (menu_focus_ + kMenuEntryCount - 1) % kMenuEntryCount;
            return;
        case UiAction::Down:
            menu_focus_ = (menu_focus_ + 1) % kMenuEntryCount;
            return;
        case UiAction::Cancel:
        case UiAction::ToggleMenu:
            menu_open_ = false;
            break;
        case UiAction::Confirm:
            switch (menu_focus_) {
                case 0:
                    menu_open_ = false;
                    break;
                case 1:
                    leave("you ended the session");
                    return;
                default:
                    // Quit means the whole session ends, not just this device's
                    // part of it -- the same as choosing it on the other
                    // device. Ask the host to shut down, then leave.
                    session_.request_quit();
                    session_.disconnect();
                    running_ = false;
                    return;
            }
            break;
        default:
            return;
    }
    session_.set_menu_open(menu_open_);
    input_.set_game_input_enabled(!menu_open_);
}

void Client::handle_actions(const std::vector<UiAction>& actions) {
    for (const UiAction action : actions) {
        if (action == UiAction::Quit && state_ != State::Playing) {
            running_ = false;
            continue;
        }
        switch (state_) {
            case State::Browsing:
                handle_browsing_action(action);
                break;
            case State::Connecting:
                if (action == UiAction::Cancel) {
                    leave("you stopped connecting");
                }
                break;
            case State::Failed:
                if (action == UiAction::Confirm || action == UiAction::Cancel) {
                    begin_search();
                }
                break;
            case State::Playing:
                if (menu_open_) {
                    handle_menu_action(action);
                    break;
                }
                switch (action) {
                    case UiAction::ToggleMenu:
                        menu_open_ = true;
                        menu_focus_ = 0;
                        session_.set_menu_open(true);
                        input_.set_game_input_enabled(false);
                        break;
                    case UiAction::ToggleFilter:
                        // R3, which means the same thing here as it does on the
                        // device running the game. The host owns it and tells
                        // both devices what it landed on.
                        session_.request_filtering(!session_.host_filtering());
                        break;
                    case UiAction::SwapLayout:
                        // L3, likewise: the two focus layouts swap.
                        session_.request_layout(
                            registry_.resolve(session_.host_layout_id()).swap_layout_id);
                        break;
                    default:
                        break;
                }
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

        session_.poll(now);
        switch (state_) {
            case State::Browsing:
                list_.update(session_.hosts());
                if (options_.auto_connect && list_.first_joinable() != nullptr) {
                    join(*list_.first_joinable());
                }
                break;
            case State::Connecting:
                if (session_.status() == net::CompanionSession::Status::Connected) {
                    state_ = State::Playing;
                    menu_open_ = false;
                    input_.set_game_input_enabled(true);
                    session_.request_display_size(output_width_, output_height_);
                    // A console with a stick of its own reads the stick as that
                    // stick; one without lets it stand in for the d-pad.
                    input_.set_stick_is_dpad(!system_profile(session_.system()).has_analog_stick);
                    log_info("connected to " + session_.game_title() + " as the " +
                             to_string(session_.granted_screen()) + " screen");
                } else if (session_.status() == net::CompanionSession::Status::Failed) {
                    state_ = State::Failed;
                    failure_ = session_.status_detail();
                    log_warn("could not join: " + failure_);
                }
                break;
            case State::Playing:
                if (session_.status() != net::CompanionSession::Status::Connected) {
                    // The host went away, or ended the session from its end.
                    // Either way this device goes back to looking, which is
                    // where it can join again without being restarted.
                    const std::string detail = session_.status_detail();
                    audio_.close();
                    audio_rate_ = 0;
                    begin_search();
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
        handle_actions(input_.ui_actions());

        if (state_ == State::Playing && !menu_open_) {
            session_.send_input(input_.ds_state());
        }

        draw();
        SDL_RenderPresent(renderer_);

        const double alive = static_cast<double>(now - started_ms) / 1000.0;
        if (options_.screenshot_at_seconds > 0.0 && !screenshot_taken &&
            !options_.screenshot_path.empty() && alive >= options_.screenshot_at_seconds) {
            screenshot_taken =
                write_screenshot(renderer_, output_width_, output_height_, options_.screenshot_path);
        }
        if (options_.exit_after_seconds > 0.0 && alive >= options_.exit_after_seconds) {
            running_ = false;
        }
    }
}

void Client::draw_banner(const std::string& title, const std::string& detail) {
    const int scale = std::max(2, output_height_ / 240);
    const float title_width = TextPainter::width_of(title, scale);
    float y = static_cast<float>(output_height_) * 0.4f;
    text_.draw(renderer_, title, (static_cast<float>(output_width_) - title_width) * 0.5f, y, scale,
               kTextColour);
    if (detail.empty()) {
        return;
    }
    const int small = std::max(1, scale - 1);
    y += TextPainter::height_of(scale) * 2.0f;
    text_.draw(renderer_, detail,
               (static_cast<float>(output_width_) - TextPainter::width_of(detail, small)) * 0.5f, y,
               small, kDimColour);
}

void Client::draw_browser() {
    const int scale = std::max(2, output_height_ / 240);
    const int small = std::max(1, scale - 1);
    const float margin = static_cast<float>(output_width_) * 0.08f;
    const float line = TextPainter::height_of(scale) * 1.8f;
    float y = static_cast<float>(output_height_) * 0.12f;

    text_.draw(renderer_, "dualscreen companion", margin, y, scale, kTextColour);
    y += line;
    const std::string screen_line =
        std::string("Joining as the ") + to_string(list_.wanted_screen()) +
        " screen   (Left/Right to change)";
    text_.draw(renderer_, screen_line, margin, y, small, kDimColour);
    y += line;

    if (list_.empty()) {
        text_.draw(renderer_, "Looking for a game on this network...", margin, y, small,
                   kDimColour);
        y += line;
        text_.draw(renderer_,
                   "Start a game on the other device and leave both on the same Wi-Fi.", margin, y,
                   small, kDimColour);
    }

    for (std::size_t index = 0; index < list_.hosts().size(); ++index) {
        const net::DiscoveredHost& host = list_.hosts()[index];
        const bool focused = static_cast<int>(index) == list_.focus();
        const bool joinable = host.offers(list_.wanted_screen());
        const SDL_Color colour = !joinable ? kDimColour : (focused ? kFocusColour : kTextColour);

        const std::string row = std::string(focused ? "> " : "  ") + host.device_name + " - " +
                                host.game_title + (joinable ? "" : "   (that screen is taken)");
        text_.draw(renderer_, row, margin, y, small, colour);
        y += TextPainter::height_of(small) * 1.6f;
    }

    // Drawn whether or not anything has been found: on a device with no
    // keyboard and no window to close, how to get out is worth saying even
    // when there is nothing yet to get into.
    text_.draw(renderer_, list_.empty() ? "B: quit" : "A: join    B: quit", margin,
               static_cast<float>(output_height_) - line * 1.5f, small, kDimColour);
}

void Client::draw_session() {
    top_.update(renderer_, session_.screen_pixels(ScreenId::Top));
    bottom_.update(renderer_, session_.screen_pixels(ScreenId::Bottom));

    const Layout layout = session_layout();
    const SystemScreens screens = session_.screens_geometry();
    bool drew_anything = false;
    for (const PlacedScreen& placed :
         place_screens(layout, static_cast<float>(output_width_),
                       static_cast<float>(output_height_), screens)) {
        ScreenTexture& source = (placed.screen == ScreenId::Top) ? top_ : bottom_;
        if (!source.ready() || placed.rect.w <= 0.0f || placed.rect.h <= 0.0f) {
            continue;
        }
        // A screen landing at exactly its own size has nothing to resample, so
        // it is never smoothed however the host has the setting -- the same
        // rule the desktop compositor draws by.
        source.set_smoothing(session_.host_filtering() && !placed.native);

        const ScreenQuad quad = screen_quad_for(placed);
        const SDL_Rect destination{static_cast<int>(std::lround(quad.x)),
                                   static_cast<int>(std::lround(quad.y)),
                                   static_cast<int>(std::lround(quad.w)),
                                   static_cast<int>(std::lround(quad.h))};
        SDL_RenderCopyEx(renderer_, source.texture(), nullptr, &destination, quad.angle_degrees,
                         nullptr, SDL_FLIP_NONE);
        drew_anything = true;
    }

    if (!drew_anything) {
        draw_banner("Waiting for the first frame", session_.game_title());
    }
}

void Client::draw_menu() {
    // A wash over the game rather than a panel: this menu is three words long
    // and the frame behind it is the thing worth still being able to see.
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0x08, 0x0c, 0x12, 0xc0);
    SDL_RenderFillRect(renderer_, nullptr);

    const int scale = std::max(2, output_height_ / 240);
    const float line = TextPainter::height_of(scale) * 2.0f;
    float y = static_cast<float>(output_height_) * 0.5f - line * 1.5f;
    for (int index = 0; index < kMenuEntryCount; ++index) {
        const std::string row =
            std::string(index == menu_focus_ ? "> " : "  ") + kMenuEntries[index];
        text_.draw(renderer_, row,
                   (static_cast<float>(output_width_) - TextPainter::width_of(row, scale)) * 0.5f, y,
                   scale, index == menu_focus_ ? kFocusColour : kTextColour);
        y += line;
    }
}

void Client::draw() {
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0xff);
    SDL_RenderClear(renderer_);

    switch (state_) {
        case State::Browsing:
            draw_browser();
            break;
        case State::Connecting:
            draw_banner("Connecting...", session_.status_detail());
            break;
        case State::Failed:
            draw_banner("Could not join", failure_.empty() ? "A: try again" : failure_);
            break;
        case State::Playing:
            draw_session();
            if (menu_open_) {
                draw_menu();
            }
            break;
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
