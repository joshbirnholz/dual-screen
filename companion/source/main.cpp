// Dualscreen, as it runs on a device that is being a screen for another.
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
// One program for three devices: a Nintendo Switch, an Android handheld, and a
// desktop -- where it is the easiest way to exercise all of the above against a
// real host (see tests/run_switch_client_test.sh). Only start-up, shutdown and
// how a device describes its own screen differ, which is the whole of the
// __SWITCH__ and __ANDROID__ in this file. Everything a player sees is the
// same code on all three.

#include <SDL.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
// iOS, iPadOS and tvOS, which are one platform for nearly everything here and
// are not macOS for any of it. TARGET_OS_IPHONE covers all three of the
// handheld ones; TARGET_OS_TV narrows it to an Apple TV, which is the one with
// no touch screen and nowhere to write a file.
#if TARGET_OS_IPHONE
#define DS_APPLE_EMBEDDED 1
#endif
#endif
#ifndef DS_APPLE_EMBEDDED
#define DS_APPLE_EMBEDDED 0
#endif

#ifdef __SWITCH__
#include <switch.h>
#endif

#ifdef __ANDROID__
#include <SDL_system.h>
#include <jni.h>
// FFmpeg's MediaCodec decoder reaches Android's own decoder through JNI, and
// needs the JavaVM handed to it before anything asks it to open one -- see
// where av_jni_set_java_vm() is called below. Guarded the same way every
// other FFmpeg header in this codebase is: DS_HAVE_VIDEO_CODEC is always
// defined (0 or 1) by whichever build is running, so a build with no video
// codec compiled in never needs libavcodec's own headers to exist at all.
#if DS_HAVE_VIDEO_CODEC
extern "C" {
#include <libavcodec/jni.h>
}
#endif
#endif

#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/audio_output.hpp"
#include "audio/ui_sounds.hpp"
#include "config/paths.hpp"
#include "config/settings.hpp"
#include "core/system.hpp"
#include "input/controller_mapping.hpp"
#include "input/cursor_visibility.hpp"
#include "input/input_manager.hpp"
#include "input/switch_console.hpp"
#include "layout/layout.hpp"
#include "layout/layout_registry.hpp"
#include "net/companion_session.hpp"
#include "net/transport.hpp"
#include "video/video_codec.hpp"
#include "render/bezel_art.hpp"
#include "render/gl_loader.hpp"
#include "render/button_glyphs.hpp"
#include "render/frame_delay.hpp"
#include "render/menu_backdrop.hpp"
#include "ui/menu_actions.hpp"
#include "render/compositor.hpp"
#include "render/gl_window.hpp"
#include "render/quad_renderer.hpp"
#include "render/system_video_layer.hpp"
#include "render/text_renderer.hpp"
#include "ui/connecting_screen.hpp"
#include "ui/launcher_screen.hpp"
#include "ui/list_touch.hpp"
#include "ui/controllers_screen.hpp"
#include "ui/layout_picker.hpp"
#include "ui/menu_state.hpp"
#include "ui/notice_popup.hpp"
#include "ui/corner_notice.hpp"
#include "util/build_stamp.hpp"
#include "util/log.hpp"

#include "local_play.hpp"

#if DS_LOCAL_PLAY
#include "core/save_store.hpp"
#include "net/save_exchange.hpp"
#include "config/melonds_tuning.hpp"
#include "core/libretro_core.hpp"
#include "ui/data_management_screen.hpp"
#include "ui/settings_screen.hpp"
#include "ui/update_prompt.hpp"
#include "update/updater.hpp"
#include "android_updater.hpp"
#include "switch_updater.hpp"
#endif

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_TV
// A phone or a tablet: something that can be turned over, unlike an Apple TV.
#define DS_APPLE_HANDHELD 1
#endif
#if TARGET_OS_TV
// A television: no touch screen, and a remote that is a controller.
#define DS_APPLE_TELEVISION 1
#endif
#endif

namespace ds::sw {
namespace {

// Whether this program can play a game itself: a build with local play in it,
// and a DS core actually there to play it with. A Switch's is linked in and
// always is; an Android APK's rides beside this library and may have been
// left out of the build, which leaves a companion and nothing else.
bool can_play_here() {
#if DS_LOCAL_PLAY
    return builtin_core_system().has_value();
#else
    return false;
#endif
}

#ifdef __ANDROID__
// One of DualscreenActivity's own methods, taking nothing and returning a
// boolean -- see that class for what each one does. False when there is no
// activity to ask, which is not a case a running app meets.
bool ask_activity(const char* method) {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) return false;
    bool answer = false;
    jclass type = env->GetObjectClass(activity);
    jmethodID id = env->GetMethodID(type, method, "()Z");
    if (id != nullptr) {
        answer = env->CallBooleanMethod(activity, id) == JNI_TRUE;
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        answer = false;
    }
    env->DeleteLocalRef(type);
    env->DeleteLocalRef(activity);
    return answer;
}

// Said over the opening screen when Load a game cannot go ahead yet.
constexpr const char* kFileAccessNotice =
    "Dualscreen needs access to your files to find games. Allow it, then come back.";
#endif

struct Options {
    // This program's own path, argv[0]: what an update replaces, and starts
    // again -- see switch_updater.hpp.
    std::string self_path;
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
    // Opens the quick menu and leaves the session from it, as a player would.
    // Two devices and a wire between them are the whole of what this behaviour
    // is about, so there is nowhere smaller it can be driven from.
    double disconnect_at_seconds = 0.0;
    std::string screenshot_path;
    // A game to play on this device, or its game browser, on a build that
    // can -- see local_play.hpp. A game's forwarder names the ROM, as a plain
    // argument or after --game; --browse is how the menu here asks for the
    // browser, by starting this program again.
    LocalPlayRequest local_play;
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
        } else if (argument == "--disconnect-at-seconds") {
            options.disconnect_at_seconds = std::strtod(
                next_argument(argc, argv, index, "--disconnect-at-seconds", options.parse_error)
                    .c_str(),
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
        } else if (argument == "--game") {
            options.local_play.game_path =
                next_argument(argc, argv, index, "--game", options.parse_error);
        } else if (argument == kBrowseArgument) {
            options.local_play.browse = true;
        } else if (!argument.empty() && argument[0] == '-') {
            options.parse_error = "unknown option " + argument;
        } else if (!argument.empty() && options.local_play.game_path.empty()) {
            // A bare path: what a forwarder passes, and what dropping a ROM
            // on a desktop build's icon would.
            options.local_play.game_path = argument;
        }
        if (!options.parse_error.empty()) {
            break;
        }
    }
    return options;
}

// What a run of the desktop app playing here is given of this program's own
// options -- see local_play.hpp.
[[maybe_unused]] LocalPlayRequest local_play_request(const Options& options) {
    LocalPlayRequest request = options.local_play;
    request.windowed = options.windowed;
    request.verbose = options.verbose;
    request.exit_after_seconds = options.exit_after_seconds;
    request.screenshot_at_seconds = options.screenshot_at_seconds;
    request.screenshot_path = options.screenshot_path;
    return request;
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

// When each part of one pass of the loop finished, on the same clock, so a
// pass that took too long can say where the time went.
//
// A Switch playing Nintendo Land went seconds at a time drawing nothing at
// all -- passes of a second or more, while decoding was 13ms a picture -- and
// the log could only say that it had happened, not whether it was input,
// receiving, drawing or waiting to present. Each of those has a different fix.
struct PassTimes {
    uint64_t began = 0;
    uint64_t events_done = 0;
    uint64_t poll_done = 0;
    uint64_t draw_began = 0;
    uint64_t draw_done = 0;
    uint64_t swap_done = 0;
};

// Long enough to be a picture visibly stopping -- three frames at 60 -- and
// not so long that the passes worth knowing about go unsaid.
constexpr uint64_t kSlowPassMs = 50;

// Where each streamed screen goes: the system's own video layer where it can
// go there (see SystemVideoLayers), and the compositor's GL everywhere else.
// Decided each frame before the session's textures are asked for, so a screen
// headed for a layer is not mapped into a texture as well.
class CompanionOutputRouter final : public OutputRouter {
public:
    CompanionOutputRouter(net::CompanionSession& session, SystemVideoLayers& layers)
        : session_(session), layers_(layers) {}

    // `nothing_drawn_over` is false while anything is drawn over the screens
    // -- the quick menu, which blurs the picture behind itself and so has to
    // have it drawn with GL.
    void decide(bool nothing_drawn_over) {
        for (ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
            const bool layered = nothing_drawn_over && layers_.usable() &&
                                 session_.native_picture(screen).picture != nullptr;
            layered_[screen == ScreenId::Top ? 0 : 1] = layered;
            session_.set_screen_shown_elsewhere(screen, layered);
        }
    }
    void reset() {
        for (ScreenId screen : {ScreenId::Top, ScreenId::Bottom}) {
            layered_[screen == ScreenId::Top ? 0 : 1] = false;
            session_.set_screen_shown_elsewhere(screen, false);
        }
    }

    OutputRoute route_for(ScreenId screen) const override {
        return layered_[screen == ScreenId::Top ? 0 : 1] ? OutputRoute::SystemLayer
                                                         : OutputRoute::Local;
    }
    void place_in_layer(const PlacedScreen& placed, bool smoothing_requested,
                        float output_width, float output_height) override {
        const net::CompanionSession::NativePicture native = session_.native_picture(placed.screen);
        layers_.show(placed.screen, native.picture, native.serial, placed,
                     screen_needs_smoothing(placed, native.width, native.height,
                                            smoothing_requested),
                     output_width, output_height);
    }

private:
    net::CompanionSession& session_;
    SystemVideoLayers& layers_;
    bool layered_[2] = {false, false};
};

class Client {
public:
    explicit Client(const Options& options) : options_(options) {}

    bool start();
    void run();
    void stop();
    // Why start() said no. On a console there is no window to have printed it
    // in and no terminal behind the app, so the caller has to be able to ask.
    const std::string& start_error() const { return start_error_; }
    // Whether the player chose Load a game, which is played by this program
    // starting again -- see local_play.hpp.
    bool wants_to_play_here() const { return wants_to_play_here_; }

private:
    // Launcher is the opening screen -- which screen to be; Browsing is the
    // host picker; Playing is a live session, with or without this device's
    // own menu up over it.
    // DataManagement only where this device plays games itself.
    // Settings, and Data Management from there, only on a build that plays
    // games here -- see local_play.hpp.
    enum class State { Launcher, Browsing, Connecting, Playing, Failed, DataManagement, Settings };

    void open_launcher();
#if DS_LOCAL_PLAY
    void start_updater(const AppPaths& paths);
    void poll_updates();
    void open_settings();
    void open_data_management();
#endif
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
    // Where the prompts currently on screen are, whichever screen that is.
    const std::vector<GuideHit>& guide_hits_on_screen() const;
    // Turns a finger landing on a prompt into the action that prompt names,
    // and takes that pointer out of the list so the screen underneath does
    // not also act on it.
    void route_guide_taps(std::vector<UiAction>& actions, std::vector<PointerEvent>& pointers);
    void handle_menu(const std::vector<UiAction>& actions,
                     const std::vector<PointerEvent>& pointers);
    void open_menu();
    LayoutPickerState menu_state_from_host() const;
    // The save point slots the host last described, into the menu: the saves
    // are on its disk, so this end has only what it is told.
    void show_host_save_slots() {
        const uint32_t revision = session_.host_save_slots_revision();
        if (revision == save_slots_shown_) return;
        save_slots_shown_ = revision;
        picker_.set_save_slots(session_.host_save_slots().slots);
        picker_.set_save_status(session_.host_save_slots().status,
                                session_.host_save_slots().status_failed);
    }
    void close_menu();
    void draw();
    void draw_scene();
    // Says where a pass that took kSlowPassMs or more went. At most once a
    // second, with a count of the ones in between, so a device that is slow
    // for a stretch does not also spend the stretch writing to its card.
    void report_slow_pass(const PassTimes& pass);
    // Keeps the screen on and the device out of sleep for as long as a
    // session is connected, and lets it go again after. Only a Switch needs
    // telling: everywhere else SDL_DisableScreenSaver() at start-up already
    // does it, for the whole run.
    void keep_awake(bool awake);
    bool kept_awake_ = false;
    uint64_t slow_pass_said_at_ = 0;
    unsigned slow_passes_unsaid_ = 0;
    // Builds every GL object this program owns. Called at start-up, and again
    // whenever the context may have been taken away and given back.
    bool load_gl_resources(std::string* error);

    Options options_;
    GlWindow gl_;
    int output_width_ = 1280;
    int output_height_ = 720;

    LayoutRegistry registry_;
    net::CompanionSession session_;
    InputManager input_;
    // The mouse pointer, on a device that has one -- a Mac. The window hides
    // it when it is made (see GlWindow); this is what brings it back while
    // the mouse is moving, and hides it again once it has been still, the
    // same as the desktop app does. It follows the host's Pointer setting --
    // the one the menu shows -- while a session is up, and is shown otherwise.
    CursorVisibility cursor_;
    bool cursor_shown_ = false;
    uint64_t cursor_updated_ms_ = 0;
    AudioOutput audio_;
    uint32_t audio_rate_ = 0;
    // The menus' own sounds, the same as the desktop app's -- see
    // UiSoundJudge and UiSoundPlayer.
    UiSoundPlayer ui_sounds_;
    UiSoundJudge ui_sound_judge_;
    UiSoundView sound_view() const;
    bool disconnect_asked_ = false;
    // Whether opening it has already been reported, so a device that will not
    // open is said once rather than on every packet that arrives.
    bool audio_open_failed_ = false;

    // The app's own renderer, not a second one: the same compositor draws the
    // screens here as on the device running the game, and the same picker
    // draws the menu, with the same previews and the same footer.
    QuadRenderer quads_;
    TextRenderer text_;
    ButtonGlyphSheet button_glyphs_;
    MenuBackdrop menu_backdrop_;
    // The game's icon the host sent, for the quick menu's header: decoded
    // once for each game, keyed by its title and the icon's size.
    Texture game_icon_;
    std::string game_icon_for_;
    const Texture* host_game_icon();
    // Each pass started as late before the display's refresh as it can be --
    // see FrameDelay -- and whether that is currently on, so the log says
    // when it changes.
    FrameDelay frame_delay_;
    bool frame_delay_said_ = false;
    Compositor compositor_;
    SystemVideoLayers video_layers_;
    CompanionOutputRouter router_{session_, video_layers_};
    BezelArt bezel_art_;
    LayoutPicker picker_{registry_};
    // The host's Controller Order screen, drawn here while it is up.
    ControllersScreen mirrored_controllers_;
    // Which of the host's save slot lists the menu is showing -- see
    // show_host_save_slots().
    uint32_t save_slots_shown_ = 0;
    LauncherScreen launcher_;
    HostPickerScreen host_picker_;
    // Where the connecting screen's prompts ended up -- it is a free
    // function rather than an object, so it has nowhere of its own to keep
    // them.
    std::vector<GuideHit> connecting_hits_;

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
    bool wants_to_play_here_ = false;
    // Said over the opening screen until it is answered -- why Load a game
    // did not go ahead. Empty for nothing.
    std::string launcher_notice_;
#ifdef __ANDROID__
    // Load a game was chosen on a device that has not yet let this app read
    // its files; it goes ahead by itself once that is allowed.
    bool awaiting_file_access_ = false;
#endif
    // Whether there is a surface to draw on. Only ever false on a device that
    // can take the app off screen without ending it.
    bool foreground_ = true;
#if DS_LOCAL_PLAY
    // This device plays games too, so it keeps their saves, and answers other
    // devices about them from its menu as well as while a game is going --
    // the same as the desktop app does.
    std::unique_ptr<SaveStore> save_store_;
    std::unique_ptr<net::SaveExchangeService> save_service_;
    std::unique_ptr<DataManagementScreen> data_screen_;
    // The same Settings a game's menu opens, from the main menu, kept in the
    // same settings.json; Data Management is its first row.
    std::unique_ptr<SettingsScreen> settings_screen_;
    Settings settings_;
    // Updating this app from the nightly builds, on a Switch or an Android
    // device built by CI: offered on the opening screen when a check at
    // start finds one, and from Settings. See update/updater.hpp.
    std::unique_ptr<update::Updater> updater_;
    UpdatePrompt update_prompt_;
#endif
};

bool Client::start() {
#if DS_APPLE_EMBEDDED
    // Set before SDL_Init, because this is the one that decides what the audio
    // session is opened as and it is read on the way up.
    //
    // SDL opens an iOS audio session as "ambient" unless told otherwise, and
    // ambient is the category that obeys the ring/silent switch. A companion
    // is a screen for a game, so a phone with its switch flipped would show
    // the picture in silence and there would be nothing in this app to look
    // at to find out why. Playback is what a game asks for.
    SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
    // The bar at the bottom of a phone, over a picture whose whole point is
    // being the other machine's screen exactly. "2" is hidden until a swipe
    // asks for it.
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
#if TARGET_OS_TV
    // The Siri Remote as a controller rather than as a source of arrow keys.
    // This app navigates its menus with a d-pad and face buttons on every
    // other device, and the remote's touch surface reports as one -- so the
    // Apple TV gets the same menu behaviour as the Switch, which is the
    // no-touch client this one is shaped like.
    SDL_SetHint(SDL_HINT_APPLE_TV_CONTROLLER_UI_EVENTS, "0");
    // Holding the remote sideways should not rotate anything. There is one
    // orientation on a television.
    SDL_SetHint(SDL_HINT_APPLE_TV_REMOTE_ALLOW_ROTATION, "0");
#endif
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        start_error_ = std::string("SDL would not start: ") + SDL_GetError();
        log_error(start_error_);
        return false;
    }
    log_info("SDL is up");
    // Said here rather than left to only show up as the host logging that this
    // device "cannot decode video" once connected -- that line names the
    // symptom, not the cause, and the cause is knowable the moment the
    // process starts.
    log_info(std::string("video decode: ") +
             (video::can_decode() ? "available" : "not built into this binary"));
    // Audio is asked for separately, and its failure is not this program's
    // failure: a second screen with no sound is still a second screen.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        log_warn(std::string("no sound on this device: ") + SDL_GetError());
    }
    // The same convention the app uses: face buttons are read by where they
    // are, not by the letter printed on them.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, kFaceButtonsByPosition);
#if defined(DS_APPLE_HANDHELD)
    // Landscape, and only landscape, on a phone or a tablet.
    //
    // A screen being streamed here is a television's shape or a handheld's,
    // and both of those are wider than they are tall. Portrait was never
    // useful and it was actively harmful: SDL 2 works out its window size
    // through UIApplication.statusBarOrientation, which iOS 27 has turned
    // into a no-op, so every rotation left SDL believing something different
    // from the framebuffer it was drawing into. On a device that showed up as
    // a panel flapping between 2556x1179, 1179x2556 and -- once -- 1377x2556,
    // which is the two subtracted from each other rather than either of them,
    // and a picture composited into a rectangle that was not on the screen.
    //
    // Said in three places because three things ask separately: here for SDL,
    // which reads this hint before anything else; the Info.plist, which is
    // what the system enforces; and the application delegate, which SDL asks
    // first of all. See apple/ios_scene_lifecycle.m.
    //
    // Must be set before the window is made. SDL reads it when the window's
    // view controller is created and not again.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    // Watching a screen is not idling. Every platform this runs on will dim,
    // lock or put up a screensaver over a picture nobody is touching, and on a
    // phone or an Apple TV that is a matter of a minute or two -- which is
    // exactly how long somebody watches a cutscene without pressing anything.
    SDL_DisableScreenSaver();

    GlWindowRequest request;
    request.title = "Dualscreen";
    request.width = 1280;
    request.height = 720;
    request.windowed = options_.windowed;
    // No core runs here, so any context the driver offers will do.
    request.need = GlContextNeed::Any;
    if (!create_gl_window(request, gl_, &start_error_)) {
        log_error(start_error_);
        return false;
    }
    if (SystemVideoLayers::available()) {
        video_layers_.attach(gl_.window);
    }

    if (!load_gl_resources(&start_error_)) {
        log_error(start_error_);
        return false;
    }
    refresh_output_size();

    input_.initialize(Settings());
    input_.set_game_input_enabled(false);
    picker_.set_repeat_source([this](UiAction direction) { return input_.direction_repeating(direction); });

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
                    // Said once. Retrying on every packet turned one broken
                    // device into sixty log lines a second.
                    if (!audio_open_failed_) {
                        log_warn("cannot play the streamed sound: " + audio_error);
                        audio_open_failed_ = true;
                    }
                    audio_rate_ = rate;
                    return;
                }
                audio_open_failed_ = false;
                audio_rate_ = rate;
                audio_.set_paused(false);
            }
            audio_.push(samples, frames);
        });

    // The whole device for the GamePad's motor, or one pad for a player's --
    // see RumbleMessage::pad.
    session_.set_rumble_callback([this](bool active, uint32_t pad, uint16_t ping_ms) {
        if (ping_ms > 0) {
            // Which pad is this? A short buzz that ends here, on time.
            input_.ping_pad(static_cast<SDL_JoystickID>(pad), ping_ms);
        } else if (pad == net::RumbleMessage::kWholeDevice) {
            input_.set_rumble(active);
        } else {
            input_.set_pad_rumble(static_cast<SDL_JoystickID>(pad), active);
        }
    });

#if DS_LOCAL_PLAY
    {
        const AppPaths paths = AppPaths::resolve();
        ensure_directory(paths.saves_dir);
        // Read now rather than when Settings is first opened: its button
        // prompts and Confirm/Cancel apply to these menus from the start.
        settings_ = Settings::load(paths);
        save_store_.reset(new SaveStore(paths.saves_dir, settings_.device_name));
        save_service_.reset(
            new net::SaveExchangeService(*save_store_, net::SaveExchangeService::Config{}));
        std::string error;
        if (!save_service_->start(&error)) {
            log_warn("not answering other devices about saves: " + error);
            save_service_.reset();
        }
        start_updater(paths);
    }
#endif

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
#if DS_LOCAL_PLAY
    // Stops a check or a download under way, and waits for it.
    update_prompt_.set_updater(nullptr);
    updater_.reset();
    // Before a game here starts answering in its place. The screen first: it
    // may be waiting on a search, and holds textures the context below owns.
    data_screen_.reset();
    settings_screen_.reset();
    save_service_.reset();
#endif
    session_.disconnect();
    audio_.close();
    // Before a game played here opens its own: see play_here().
    ui_sounds_.close();
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
    // Nothing is reading how this device is being held any more, so the
    // sensors stop -- on a handheld that is battery not being spent sitting
    // on a menu. Picked up again by the Playing case in poll(), from
    // whatever the next host asks for.
    input_.set_motion_enabled(false);
    // The app's own opening screen, minus the two entries that mean nothing
    // here: this device has no ROMs to load and nothing in Settings that would
    // apply to it. What is left is which screen to be, and leaving.
#ifdef __SWITCH__
    // Every piece of Switch homebrew exits on plus, and a player will try it
    // before looking for a row to select.
    launcher_.set_plus_quits(true);
#endif
#if defined(DS_APPLE_HANDHELD) || defined(DS_APPLE_TELEVISION)
    // An Apple app is left with the Home gesture or the app switcher, never
    // from inside -- Apple's guidelines say not to quit programmatically, and
    // a row that closes the window is the wrong shape for a platform where
    // the system decides when an app stops. On an Apple TV it is worse than
    // useless: pressing it drops the player back to the home screen with
    // nothing to say why.
    launcher_.set_offers_quit(false);
#endif
    // Unless this build carries its own emulator, in which case it can play a
    // game here too -- see local_play.hpp.
    launcher_.set_offers_load_game(can_play_here());
    launcher_.open(/*runs_games=*/false);
}

#if DS_LOCAL_PLAY
void Client::start_updater(const AppPaths& paths) {
    std::unique_ptr<update::Backend> backend;
    const char* asset = "";
#if defined(__SWITCH__)
    backend = sw::make_switch_update_backend(options_.self_path, path_join(paths.data_dir, "update"));
    asset = sw::kSwitchAsset;
#elif defined(__ANDROID__)
    backend = android::make_android_update_backend();
    asset = android::kAndroidAsset;
#endif
    if (!backend) return;
    updater_ = std::make_unique<update::Updater>(
        std::move(backend), update::components_for(asset, build_number(), false, update::kNotInstalled));
    update_prompt_.set_updater(updater_.get());
    update_prompt_.set_declined_file(path_join(path_join(paths.data_dir, "update"), "declined"));
    // Only a build from CI looks at start-up -- one built by hand is offered
    // the nightly only when it asks -- and never one joining a game straight
    // from its command line, or run by a test.
    if (build_number() > 0 && !options_.auto_connect && options_.exit_after_seconds <= 0.0) {
        update_prompt_.check_at_start();
    }
}

void Client::poll_updates() {
    if (!updater_) return;
    const UpdatePromptResult result = update_prompt_.poll(state_ == State::Launcher);
    if (result.restart) {
#if defined(__SWITCH__)
        // The homebrew loader starts the new one once this has exited.
        relaunch(options_.self_path, "");
#endif
        log_info("leaving to start the new build");
        running_ = false;
    }
    if (settings_screen_) settings_screen_->set_updates_value(update_prompt_.row_value());
}

void Client::open_settings() {
    const AppPaths paths = AppPaths::resolve();
    settings_ = Settings::load(paths);
    if (!settings_screen_) {
        settings_screen_.reset(new SettingsScreen(paths));
        settings_screen_->set_offers_data_management(save_store_ != nullptr);
        settings_screen_->set_offers_updates(updater_ != nullptr);
        settings_screen_->set_updates_value(update_prompt_.row_value());
#ifdef __SWITCH__
#ifdef DS_DEKO_CORE
        settings_screen_->set_melonds_page(true, kMelonDsTuningPath);
#else
        settings_screen_->set_melonds_page(false, kMelonDsTuningPath);
#endif
#endif
    }
    // The one emulator this build has, linked in.
    settings_screen_->open(settings_, {{"Nintendo DS", kBuiltInCorePath, SystemId::Ds, "melonDS"}});
    state_ = State::Settings;
    log_info("the settings screen is up");
}

void Client::open_data_management() {
    if (!save_store_) return;
    if (!data_screen_) {
        data_screen_.reset(new DataManagementScreen(
            *save_store_, save_service_ ? save_service_->instance() : new_save_version_id()));
        if (save_service_) {
            net::SaveExchangeService* service = save_service_.get();
            data_screen_->set_change_source([service] { return service->saves_received(); });
        }
    }
    data_screen_->open();
    state_ = State::DataManagement;
}
#endif

void Client::begin_search(ScreenId wanted) {
    state_ = State::Browsing;
    close_menu();
    host_picker_.open(wanted);

    // What the host calls this device, on its Controllers screen among other
    // places. A Switch says which Switch rather than nothing, which the host
    // would otherwise show as "companion".
    std::string name = options_.device_name;
#ifdef __SWITCH__
    if (name.empty()) name = switch_console_name();
#elif defined(__ANDROID__)
    // Which Android device, as Android names it -- see default_device_name().
    if (name.empty()) name = Settings::load(AppPaths::resolve()).device_name;
#endif
    std::string error;
    if (!session_.begin_search(name, wanted, &error)) {
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
    Layout chosen = resolve_for_handedness(
        resolve_gap_choice(registry_.resolve(session_.host_layout_id()), session_.host_screen_gap()),
        session_.host_handedness());
    // Holding the touch screen means being the thing that is touched, so the
    // panel is filled with it whatever its size -- see Layout::fill_display.
    chosen.fill_display = session_.granted_screen() == ScreenId::Bottom;
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
    picker_.set_display_size(static_cast<float>(width), static_cast<float>(height),
                             session_.granted_screen() == ScreenId::Bottom);

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

LayoutPickerState Client::menu_state_from_host() const {
    // The same one the desktop app draws from -- see ui/menu_state.hpp for
    // why there is only one.
    return ds::menu_state_from_host(session_);
}

void Client::open_menu() {
    // With the console, not without it. Left off, the picker fell back to its
    // default of a DS -- so a companion joining a Wii U host drew the DS's
    // grid of layouts to choose between, on a console that has no layouts to
    // choose and whose menu is a plain list. The host had it right and this
    // end did not, which is exactly the disagreement the shared menu exists
    // to prevent.
    picker_.set_screens(session_.screens_geometry(), session_.system());
    picker_.set_bezel_art(bezel_art_.texture(session_.system()));
#if DS_LOCAL_PLAY
    // A companion with no Settings of its own (no local play) always shows it.
    picker_.set_show_battery(Settings::load(AppPaths::resolve()).show_battery);
#endif
    picker_.open(menu_state_from_host());
    save_slots_shown_ = session_.host_save_slots_revision() - 1;
    show_host_save_slots();
    // Which console's menu this is. Said out loud because the two devices
    // drawing different menus is otherwise only visible by holding them side
    // by side -- and it happened: this end defaulted to a DS's menu on a
    // Wii U host until set_screens() above was told the console.
    log_info(std::string("opening the ") + system_profile(session_.system()).name + " menu");

    menu_open_ = true;
    // Kept in step with the host even when the host is the one that opened it,
    // so that closing always has something to clear.
    session_.set_menu_open(true);
    // The game has stopped behind this menu and stopped sending, so what is
    // still queued is its tail: a quarter second of a game that is no longer
    // running, which would keep playing over the menu that silenced it.
    audio_.flush();
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
    // Nothing is decided here. There is one menu and the host holds it, so a
    // press made on this device is sent over and this device redraws whatever
    // the host says the menu now looks like -- which is how the same menu can
    // be worked from either pad while the player watches either screen.
    for (const UiAction action : actions) {
        net::MenuAction wire = net::MenuAction::Confirm;
        if (!menu_action_for(action, wire)) continue;
        session_.request_menu_action(wire);
    }
    for (const PointerEvent& pointer : pointers) {
        if (!pointer.released) continue;
        // Resolved here rather than there: the two devices lay the menu out for
        // different-sized screens, so only this one knows what its own tap was
        // on.
        // A finger on one of the control prompts is that button being
        // pressed, forwarded like any other press.
        const ButtonGlyph on_guide = picker_.guide_button_at(pointer.x, pointer.y);
        if (on_guide != ButtonGlyph::Count &&
            on_guide == picker_.guide_button_at(pointer.press_x, pointer.press_y)) {
            net::MenuAction wire = net::MenuAction::Confirm;
            bool forward = true;
            switch (on_guide) {
                case ButtonGlyph::A: wire = net::MenuAction::Confirm; break;
                case ButtonGlyph::B: wire = net::MenuAction::Cancel; break;
                case ButtonGlyph::X: wire = net::MenuAction::Quit; break;
                case ButtonGlyph::Y: wire = net::MenuAction::Reset; break;
                case ButtonGlyph::Plus: wire = net::MenuAction::Disconnect; break;
                case ButtonGlyph::Minus: wire = net::MenuAction::Settings; break;
                // Smoothing is a shared setting rather than a menu press,
                // asked for the way its button asks for it.
                case ButtonGlyph::RightStick:
                    session_.request_filtering(!session_.host_filtering());
                    forward = false;
                    break;
                default: forward = false; break;
            }
            if (forward) session_.request_menu_action(wire);
            continue;
        }
        const auto entry_under = [&](float x, float y) {
            return picker_.entry_at(x, y, static_cast<float>(output_width_),
                                    static_cast<float>(output_height_));
        };
        const int entry = entry_under(pointer.x, pointer.y);
        // Only where the finger came down on the same entry it left on.
        if (entry >= 0 && entry == entry_under(pointer.press_x, pointer.press_y)) {
            session_.request_menu_press(entry, net::MenuAction::Confirm);
        }
    }
}

const std::vector<GuideHit>& Client::guide_hits_on_screen() const {
    static const std::vector<GuideHit> none;
    switch (state_) {
        case State::Launcher: return launcher_.guide_hits();
        case State::Connecting: return connecting_hits_;
        case State::Browsing:
        case State::Failed: return host_picker_.guide_hits();
        // Nothing is drawn over a game. The way in to the menu there is two
        // fingers at once -- see InputManager -- rather than a button sitting
        // on top of the picture the whole session. The quick menu itself
        // draws prompts but routes them itself, since a press there is
        // forwarded to the host rather than acted on here. The host's
        // Controller Order, drawn here, is routed here: the presses its
        // prompts stand for go over with the rest -- see handle_actions().
        case State::Playing:
            if (!menu_open_ && session_.host_settings_open() && session_.host_controllers_view().open) {
                return mirrored_controllers_.guide_hits();
            }
            return none;
        // Its prompts are its own, and so are taps on them.
        case State::DataManagement:
        case State::Settings: return none;
    }
    return none;
}

void Client::route_guide_taps(std::vector<UiAction>& actions,
                              std::vector<PointerEvent>& pointers) {
    take_guide_taps(guide_hits_on_screen(), actions, pointers);
}

void Client::handle_actions(const std::vector<UiAction>& actions,
                            const std::vector<PointerEvent>& pointers) {
#if DS_LOCAL_PLAY
    poll_updates();
    // An update being offered, downloaded or reported takes every press.
    if (update_prompt_.open()) {
        update_prompt_.update(actions, pointers, static_cast<float>(output_width_),
                              static_cast<float>(output_height_));
        return;
    }
#endif
    if (state_ == State::Playing && menu_open_) {
        handle_menu(actions, pointers);
        return;
    }
    // The host's Settings or Controllers are up. This device is not drawing
    // them, but the player is still holding a pad here, and it is still
    // their session -- so the presses go over and get the same answers a
    // press on the host would.
    if (state_ == State::Playing && session_.host_settings_open()) {
        for (const UiAction action : actions) {
            net::MenuAction wire = net::MenuAction::Confirm;
            if (menu_action_for(action, wire)) {
                session_.request_menu_action(wire);
            }
        }
        return;
    }
    // The host has just resumed its game from where it was left, and is
    // waiting, paused, under a popup that says so. The same popup is up here,
    // and OK here is OK there. Nothing else is answered until then -- on a
    // remote, back included, which would otherwise open the menu.
    if (state_ == State::Playing && session_.host_resume_notice()) {
        if (notice_popup_answered(actions, pointers, static_cast<float>(output_width_),
                                  static_cast<float>(output_height_), kResumedNotice)) {
            session_.request_menu_action(net::MenuAction::Confirm);
        }
        return;
    }

    if (state_ == State::DataManagement) {
#if DS_LOCAL_PLAY
        // Nothing is being played here while this is up -- the menu is this
        // program and the game is the desktop app's -- so nothing restarts.
        // Back to Settings, where it was opened from, on its row.
        if (data_screen_ &&
            data_screen_->update(actions, pointers, static_cast<float>(output_width_),
                                 static_cast<float>(output_height_))
                .close_requested) {
            if (settings_screen_) {
                settings_screen_->return_from_data_management();
                state_ = State::Settings;
            } else {
                open_launcher();
            }
        }
#endif
        return;
    }

    if (state_ == State::Settings) {
#if DS_LOCAL_PLAY
        const SettingsScreenResult result =
            settings_screen_->update(actions, pointers, static_cast<float>(output_width_),
                                     static_cast<float>(output_height_));
        if (result.settings_changed) {
            settings_ = settings_screen_->settings();
            ui_sounds_.set_style(settings_.ui_sound_style);
            ui_sounds_.set_enabled(settings_.ui_sounds);
            if (!settings_.save(AppPaths::resolve())) log_warn("could not save settings.json");
        }
        if (result.open_updates) {
            update_prompt_.choose();
        } else if (result.open_data_management) {
            open_data_management();
        } else if (result.close_requested) {
            open_launcher();
        }
#endif
        return;
    }

    if (state_ == State::Launcher && !launcher_notice_.empty()) {
        if (notice_popup_answered(actions, pointers, static_cast<float>(output_width_),
                                  static_cast<float>(output_height_), launcher_notice_)) {
            launcher_notice_.clear();
        }
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
            case LauncherChoice::Settings:
#if DS_LOCAL_PLAY
                open_settings();
#endif
                break;
            case LauncherChoice::Quit:
                log_info("leaving: the launcher's Quit row was chosen");
                running_ = false;
                break;
            case LauncherChoice::LoadGame:
#ifdef __ANDROID__
                // Games live in shared storage, which an app may not read
                // until the player has said so in the system's own settings
                // -- "access to all files" from Android 11 on. Asked here, the
                // first time it is needed, rather than at start-up, where a
                // device only ever joining games would be asked for nothing.
                if (!ask_activity("hasFileAccess")) {
                    log_info("asking for access to the device's files before opening the game "
                             "browser");
                    ask_activity("requestFileAccess");
                    awaiting_file_access_ = true;
                    launcher_notice_ = kFileAccessNotice;
                    break;
                }
#endif
                log_info("leaving the menu to play a game on this device");
                wants_to_play_here_ = true;
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
            case State::DataManagement:
            case State::Settings:
                break;
            case State::Playing:
                // On a Siri Remote, back opens the quick menu.
                //
                // Nothing else here could. A remote has no L3 and R3 to press
                // together and no second finger to tap with, so without this
                // there is no way into the menu on an Apple TV at all -- and
                // no way to disconnect, or to quit the game, once a session
                // has started.
                //
                // It is also what that button is for. tvOS means it as "out of
                // whatever I am in", and in a game that is the menu, not the
                // app. A press this app does not consume is one tvOS acts on
                // itself, and what tvOS does with it is close the app.
                //
                // Pressing it again closes the menu: with the menu up,
                // handle_menu() takes every action and forwards it to the
                // host, whose menu answers back the way it does for any other
                // device.
                if (prompt_style() == PromptStyle::Remote && action == UiAction::Cancel) {
                    session_.set_menu_open(true);
                    break;
                }
                switch (action) {
                    case UiAction::OpenMenu:
                    case UiAction::ToggleMenu:
                        // Asked for rather than opened here. The host holds the
                        // one menu; this device puts it up when told to, which
                        // is what keeps the two screens showing the same thing.
                        session_.set_menu_open(true);
                        break;
                    case UiAction::ToggleFilter:
                        // R3, which means the same thing here as it does on the
                        // device running the game.
                        session_.request_filtering(!session_.host_filtering());
                        break;
                    case UiAction::SwapLayout: {
                        // L3, likewise: the two focus layouts swap, and the
                        // stylus stays on the side it was -- see
                        // layout_swap_from().
                        const Layout& from = registry_.resolve(session_.host_layout_id());
                        // Separate Screens: the two devices trade screens,
                        // which the host does and then says.
                        if (from.swaps_screens) {
                            session_.request_screen_swap();
                            break;
                        }
                        const LayoutSwap swap = layout_swap_from(registry_, from);
                        if (swap.layout_id.empty()) break;
                        if (swap.keeps_hand) {
                            const auto& left = session_.host_left_handed_layouts();
                            const bool left_handed =
                                std::find(left.begin(), left.end(), from.id) != left.end();
                            session_.request_layout(
                                swap.layout_id, left_handed ? Handedness::Left : Handedness::Right);
                        } else {
                            session_.request_layout(swap.layout_id);
                        }
                        break;
                    }
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

bool Client::load_gl_resources(std::string* error) {
    quads_.shutdown();
    text_.shutdown();
    bezel_art_.destroy();

    std::string reason;
    if (!quads_.initialize(&reason) || !text_.initialize(&reason) ||
        !button_glyphs_.initialize(&reason)) {
        if (error != nullptr) *error = "the renderer would not start: " + reason;
        return false;
    }
    // The prompts stay in the default style here. Choosing one lives on the
    // settings screen, and that screen belongs to the machine running the
    // game -- this app has no settings of its own to read, and giving it
    // settings.json to read would pull a JSON library into a build that is
    // otherwise deliberately lean. A companion wanting its own choice needs
    // its own settings screen, which is a bigger thing than this.
    // Not fatal: the Standard layout still puts the screens where the console
    // puts them, it just has nothing drawn behind them.
    if (bezel_art_.load(&reason)) {
        log_info("console artwork loaded");
    } else {
        log_warn("cannot draw the console artwork: " + reason);
    }
    // The streamed screens live in textures too, and those are named by
    // numbers that mean nothing in a new context.
    session_.forget_screen_textures();
    return true;
}

void Client::keep_awake(bool awake) {
    if (awake == kept_awake_) {
        return;
    }
    kept_awake_ = awake;
#ifdef __SWITCH__
    // A connected Switch put down for a moment is still the other half of a
    // game in progress. Left alone it dims, then sleeps, and sleeping drops
    // the connection -- so while a session is up it is told what a video
    // player tells it: something is playing. That turns off both the dimming
    // and the auto-sleep, and only until it is told otherwise.
    const Result result = appletSetMediaPlaybackState(awake);
    if (R_FAILED(result)) {
        log_warn("could not " + std::string(awake ? "keep" : "stop keeping") +
                 " the console awake: " + std::to_string(result));
        return;
    }
#endif
    log_info(awake ? "a session is connected: keeping the screen on"
                   : "no session: the screen may sleep again");
}

namespace {
uint64_t steady_micros() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}
}  // namespace

UiSoundView Client::sound_view() const {
    UiSoundView view;
    view.screen = static_cast<int>(state_);
    if (state_ == State::Playing) {
        // The host's Settings, opened from its quick menu, count as the menu
        // too: they are drawn there, not here, but they are over the game.
        const bool host_page = session_.host_settings_open();
        view.menu = menu_open_ || host_page;
        view.in_game = !view.menu && !session_.host_resume_notice();
        if (menu_open_) picker_.describe_for_sound(view);
        mix_sound_state(view.rest, host_page ? 1u : 0u);
        return view;
    }
    switch (state_) {
        case State::Launcher: launcher_.describe_for_sound(view); break;
        case State::Browsing: host_picker_.describe_for_sound(view); break;
        case State::Failed: view.error = true; break;
#if DS_LOCAL_PLAY
        case State::DataManagement:
            if (data_screen_) data_screen_->describe_for_sound(view);
            break;
        case State::Settings:
            if (settings_screen_) settings_screen_->describe_for_sound(view);
            break;
#endif
        default: break;
    }
#if DS_LOCAL_PLAY
    update_prompt_.describe_for_sound(view);
#endif
    return view;
}

void Client::run() {
    const uint64_t started_ms = net::monotonic_millis();
    bool screenshot_taken = false;
    {
        // Only a build that plays games here has Settings to turn them off in.
#if DS_LOCAL_PLAY
        const Settings sound_settings = Settings::load(AppPaths::resolve());
        const bool wanted = sound_settings.ui_sounds;
        ui_sounds_.set_style(sound_settings.ui_sound_style);
#else
        const bool wanted = true;
#endif
        std::string sound_error;
        if (ui_sounds_.open(&sound_error)) {
            log_info(std::string("menu sounds ready") + (wanted ? "" : ", but turned off in Settings"));
        } else {
            log_info("no menu sounds: " + sound_error);
        }
        ui_sounds_.set_enabled(wanted);
    }

    while (running_) {
#ifdef __SWITCH__
        // The console can take the app away -- sleep, the home menu, a system
        // update prompt -- and this is how it says so.
        if (!appletMainLoop()) {
            break;
        }
#endif
        // While a picture is streaming, wait out most of the refresh first,
        // so everything below -- input, network, decode, draw -- happens just
        // before the refresh the result is shown at, rather than just after
        // the one before it. See FrameDelay.
        if (foreground_ && state_ == State::Playing) {
            const uint64_t wait = frame_delay_.wait_us();
            if (wait > 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(wait));
            }
            if (frame_delay_.active() != frame_delay_said_) {
                frame_delay_said_ = frame_delay_.active();
                log_info(frame_delay_said_
                             ? "drawing each frame just before the display's refresh (every " +
                                   std::to_string(frame_delay_.period_us() / 100 / 10.0).substr(0, 4) +
                                   "ms), waiting " + std::to_string(wait / 1000) +
                                   "ms first so the newest picture is the one shown"
                             : "no longer waiting before each frame: the display's refresh is "
                               "not steady enough to aim at");
            }
        }
        const uint64_t pass_began_us = steady_micros();
        const uint64_t now = net::monotonic_millis();
        // Where a slow pass went -- see report_slow_pass().
        PassTimes pass;
        pass.began = now;

        input_.begin_frame();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            // Android can take the surface away while the app keeps running --
            // the task switcher, a notification pulled down, the screen locked.
            // Drawing has to stop for as long as that lasts, and every GL
            // object has to be assumed gone when it comes back: the context is
            // not promised to survive, and a texture that did not survive is
            // still a valid-looking number.
            if (event.type == SDL_APP_WILLENTERBACKGROUND) {
                log_info("going off screen");
                foreground_ = false;
            } else if (event.type == SDL_APP_DIDENTERFOREGROUND) {
                log_info("back on screen");
                foreground_ = true;
                std::string reload_error;
                if (!load_gl_resources(&reload_error)) {
                    log_error("cannot draw any more: " + reload_error);
                    running_ = false;
                }
#ifdef __ANDROID__
                // Back from the system's settings, where Load a game sent the
                // player to let this app read their files: on with it, if
                // they did, as though it had just been chosen.
                if (awaiting_file_access_ && state_ == State::Launcher &&
                    ask_activity("hasFileAccess")) {
                    log_info("file access allowed; opening the game browser");
                    awaiting_file_access_ = false;
                    launcher_notice_.clear();
                    wants_to_play_here_ = true;
                    running_ = false;
                }
#endif
            }
            // A real mouse brings the pointer back; a finger does not -- SDL
            // reports touches as mouse events from SDL_TOUCH_MOUSEID.
            if ((event.type == SDL_MOUSEMOTION && event.motion.which != SDL_TOUCH_MOUSEID) ||
                (event.type == SDL_MOUSEBUTTONDOWN && event.button.which != SDL_TOUCH_MOUSEID) ||
                (event.type == SDL_MOUSEWHEEL && event.wheel.which != SDL_TOUCH_MOUSEID)) {
                cursor_.notice_mouse_activity();
            }
            input_.handle_event(event);
        }
        cursor_.set_mode(state_ != State::Playing || session_.host_cursor_shown()
                             ? CursorMode::Shown
                             : CursorMode::Hidden);
        cursor_.update(cursor_updated_ms_ == 0
                           ? 0.0
                           : static_cast<double>(now - cursor_updated_ms_) / 1000.0);
        cursor_updated_ms_ = now;
        // Asked of SDL each frame rather than remembered here: anything else
        // that hides the pointer -- the window being made, the system taking
        // the app into or out of full screen -- would otherwise leave it
        // hidden while this still thought it was showing.
        const bool want_shown = cursor_.visible();
        const bool is_shown = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE;
        if (want_shown != is_shown) {
            SDL_ShowCursor(want_shown ? SDL_ENABLE : SDL_DISABLE);
        }
        if (want_shown != cursor_shown_) {
            cursor_shown_ = want_shown;
            log_info(std::string("pointer: ") + (want_shown ? "shown" : "hidden") + " (" +
                      (cursor_.mode() == CursorMode::Shown
                           ? (want_shown ? "the mouse moved" : "the mouse has been still")
                           : "the Pointer setting is off") +
                      ")");
        }
        refresh_output_size();
        follow_panel_size();
        pass.events_done = net::monotonic_millis();

        session_.poll(now);
        pass.poll_done = net::monotonic_millis();

        switch (state_) {
            case State::Launcher:
            case State::DataManagement:
            case State::Settings:
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
                    input_.set_game_owns_controls(profile.game_uses_every_control);
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
                // Followed every frame rather than set once on connecting:
                // the host sends its whole state on every change, and a
                // session that changed console (a different game started on
                // the host) would otherwise leave this device's sensors in
                // whatever the last one wanted. Cheap when it has not moved
                // -- set_motion_enabled() with what is already set does
                // nothing at all.
                input_.set_motion_enabled(session_.host_wants_motion());
                // Alongside the session's own stream report, on the same
                // clock, so the two lines can be read together.
                audio_.report(now);
                // The menu is the host's too. It says whether one is up and
                // where its cursor is; this device draws that and nothing of
                // its own, so both screens show the same menu however it was
                // opened and whichever pad is working it.
                if (session_.host_menu_open() && !menu_open_) {
                    open_menu();
                } else if (!session_.host_menu_open() && menu_open_) {
                    close_menu();
                }
                if (menu_open_) {
                    // Every frame, all of it: this device shows the host's
                    // menu and holds no copy of its own to drift.
                    picker_.set_smoothing(session_.host_filtering());
                    picker_.show(menu_state_from_host(), session_.host_menu_focus(),
                                 static_cast<MenuConfirm>(session_.host_menu_confirm()),
                                 session_.host_menu_confirm_focus());
                    show_host_save_slots();
                }
                // The host owns all of this and can change any of it from its
                // own end; this device's pad drives the same game, so it
                // follows rather than deciding.
                if (input_.right_stick_mode() != session_.host_right_stick_mode()) {
                    input_.set_right_stick_mode(session_.host_right_stick_mode());
                    log_info(std::string("following the host: right stick ") +
                             to_string(session_.host_right_stick_mode()));
                }
                // And what L3 is: the microphone or the layout swap, as the
                // host has it -- see LeftStickPress.
                input_.set_microphone_button(
                    left_stick_is_microphone(system_profile(session_.system()).has_microphone,
                                             session_.host_left_stick_press()));
                if (session_.status() != net::CompanionSession::Status::Connected) {
                    const std::string detail = session_.status_detail();
                    audio_.close();
                    audio_rate_ = 0;
                    log_info("the session ended: " + detail);
#if !DS_APPLE_EMBEDDED
                    // Quit from this device's side of the menu: the game has
                    // closed on the host, and this app closes with it rather
                    // than going back to its main menu. Not on an iPhone, iPad
                    // or Apple TV, where an app does not close itself -- iOS
                    // and tvOS treat that as a crash -- so it goes back to its
                    // main menu there, as before.
                    if (session_.host_quit_for_this_device()) {
                        log_info("leaving: the game was quit from this device");
                        running_ = false;
                        break;
                    }
#endif
                    open_launcher();
                }
                break;
            case State::Failed:
                break;
        }

        keep_awake(state_ == State::Playing);

        const Layout layout =
            state_ == State::Playing ? session_layout() : registry_.fallback();
        input_.end_frame(layout, static_cast<float>(output_width_),
                         static_cast<float>(output_height_), session_.screens_geometry(),
                         static_cast<float>(panel_width_), static_cast<float>(panel_height_));
        if (input_.quit_requested()) {
            // Which of the ways out this was, because they want opposite
            // fixes and the exit looks identical from the log otherwise. On
            // an Apple TV the app exited "while navigating the main menu" and
            // there was no telling whether something in here had asked to
            // stop or whether tvOS had closed the app over our heads -- an
            // unhandled Menu press at the top level does exactly that.
            log_info("leaving: something asked this app to quit (SDL_QUIT, or a key that means "
                     "leave)");
            running_ = false;
        }
        // Which kind of prompt to draw, asked every frame because a pad can
        // be connected or taken away at any point in a session.
        //
        // Whether this is a television is a platform fact rather than a
        // question for SDL: a Siri Remote's touch surface is a touch device as
        // far as SDL is concerned, and asking it would answer the wrong way
        // round. Everything after that is counting -- see prompt_style_for.
#if defined(DS_APPLE_TELEVISION)
        constexpr bool kTelevision = true;
#else
        constexpr bool kTelevision = false;
#endif
        const PromptStyle style = prompt_style_for(kTelevision, input_.connected_controllers(),
                                                   input_.connected_gamepads());
        set_prompt_style(style);
        // And which button means yes, which a Siri Remote does not get a say
        // in: it has one button on its face and the click of it arrives as
        // SDL's *south*. This app's menus confirm on east by default, so on an
        // Apple TV clicking selected nothing -- and, because south is the
        // other half of that pair, what it actually did was cancel. Choosing
        // south here puts yes on the only button there is.
        //
        // Set every frame rather than once, because a pad can be paired with
        // the Apple TV at any point and then east is right again.
        const ConfirmButton confirms = confirm_button_for_style(style);
        input_.set_confirm_button(confirms);
        set_confirm_button(confirms);
#ifdef __SWITCH__
        // The letters on the buttons, as the Switch's own menus draw them --
        // or, with one Joy-Con held sideways, their positions, as they draw
        // those. The same rule as a game played here: see the desktop app's
        // apply_button_style().
        button_glyphs_.set_style(input_.single_joycon_in_use() ? PadGlyphStyle::Positional
                                                               : PadGlyphStyle::Nintendo);
#elif DS_LOCAL_PLAY && !defined(DS_APPLE_TELEVISION)
        // Everywhere else that has Settings -- an Android device, a desktop --
        // the player's own choice of prompts and of which button says yes,
        // the same two the desktop app follows. Only Settings offered them:
        // these menus always drew a Switch's letters, whatever was chosen.
        button_glyphs_.set_style(settings_.pad_glyphs);
        input_.set_confirm_button(settings_.confirm_button);
        set_confirm_button(settings_.confirm_button);
#endif
        // And drawn for whichever pad was pressed last -- see
        // InputManager::reads_by_letter().
        button_glyphs_.set_by_letter(input_.last_pad_reads_by_letter());
        std::vector<UiAction> actions = input_.ui_actions();
        std::vector<PointerEvent> pointers = input_.pointer_events();
        set_live_pointer(input_.pointer_active(), input_.pointer_x(), input_.pointer_y());
        route_guide_taps(actions, pointers);
        const bool tapped = std::any_of(pointers.begin(), pointers.end(),
                                        [](const PointerEvent& pointer) { return pointer.released; });
        const double now_seconds = static_cast<double>(net::monotonic_millis() - started_ms) / 1000.0;
        ui_sound_judge_.note_input(actions, tapped, now_seconds);
        handle_actions(actions, pointers);
        // In a session the host decides, for both devices: its menu is the
        // one on screen here, and its sounds come over with it.
        const bool host_decides = state_ == State::Playing;
        if (const std::optional<UiSound> sound = ui_sound_judge_.observe(sound_view(), now_seconds)) {
            // Nothing for a press that closes the app -- Quit, or Load a game,
            // which starts it again: the sound would be cut off as it goes.
            if (!host_decides && running_) ui_sounds_.play(*sound);
        }
        net::MenuSound from_host;
        while (session_.take_menu_sound(from_host)) {
            const std::optional<UiSound> sound = ui_sound_from_wire(static_cast<uint8_t>(from_host));
            if (sound && host_decides) ui_sounds_.play(*sound);
        }

        if (state_ == State::Playing && !menu_open_ && !session_.host_settings_open() &&
            !session_.host_resume_notice()) {
            // Each pad on its own as well, so a Wii U host can make them
            // different players.
            const std::vector<net::ReportedPad> pads = net::reported_pads(input_.pad_readings());
            session_.send_input(input_.ds_state(), &pads);
        }

        const double alive = static_cast<double>(now - started_ms) / 1000.0;
        // Two steps: the menu has to be up before the press that leaves from it
        // means anything, and the host is the one that puts it up.
        if (options_.disconnect_at_seconds > 0.0 && !disconnect_asked_ &&
            state_ == State::Playing && alive >= options_.disconnect_at_seconds) {
            if (!menu_open_) {
                session_.set_menu_open(true);
            } else {
                session_.request_menu_action(net::MenuAction::Disconnect);
                disconnect_asked_ = true;
                log_info("asked to leave the session from the quick menu");
            }
        }
        // Off screen there is no surface to draw on, and swapping against one
        // that has gone away is at best wasted work. The session is still
        // polled, so a glance at something else and back does not cost the
        // connection.
        if (foreground_) {
            pass.draw_began = net::monotonic_millis();
            draw();
            pass.draw_done = net::monotonic_millis();
            if (options_.screenshot_at_seconds > 0.0 && !screenshot_taken &&
                !options_.screenshot_path.empty() && alive >= options_.screenshot_at_seconds) {
                screenshot_taken =
                    write_screenshot(output_width_, output_height_, options_.screenshot_path);
            }
            const uint64_t swap_began_us = steady_micros();
            frame_delay_.worked(swap_began_us - pass_began_us);
            SDL_GL_SwapWindow(gl_.window);
            const uint64_t swap_done_us = steady_micros();
            frame_delay_.presented(swap_done_us, swap_done_us - swap_began_us);
            pass.swap_done = net::monotonic_millis();
            session_.note_frame_drawn();
            report_slow_pass(pass);
        } else {
            SDL_Delay(16);
        }
        if (options_.exit_after_seconds > 0.0 && alive >= options_.exit_after_seconds) {
            running_ = false;
        }
    }
    keep_awake(false);
}

void Client::report_slow_pass(const PassTimes& pass) {
    if (pass.swap_done < pass.began || pass.swap_done - pass.began < kSlowPassMs) {
        return;
    }
    if (slow_pass_said_at_ != 0 && pass.swap_done - slow_pass_said_at_ < 1000) {
        ++slow_passes_unsaid_;
        return;
    }
    const auto span = [](uint64_t from, uint64_t to) {
        return std::to_string(to >= from ? to - from : 0) + "ms";
    };
    std::string line = "a pass took " + span(pass.began, pass.swap_done) + ": " +
                       span(pass.began, pass.events_done) + " reading input, " +
                       span(pass.events_done, pass.poll_done) + " receiving and decoding, " +
                       span(pass.poll_done, pass.draw_began) + " working out what to show, " +
                       span(pass.draw_began, pass.draw_done) + " drawing, " +
                       span(pass.draw_done, pass.swap_done) + " presenting";
    if (slow_passes_unsaid_ > 0) {
        line += " (and " + std::to_string(slow_passes_unsaid_) +
                " more slow passes since the last one said)";
    }
    log_info(line);
    slow_pass_said_at_ = pass.swap_done;
    slow_passes_unsaid_ = 0;
}

const Texture* Client::host_game_icon() {
    const std::string key =
        session_.game_title() + ":" + std::to_string(session_.game_icon_rgba().size());
    if (key != game_icon_for_) {
        game_icon_for_ = key;
        game_icon_ = Texture();
        const int width = session_.game_icon_width();
        const int height = session_.game_icon_height();
        const std::vector<uint8_t>& rgba = session_.game_icon_rgba();
        if (width > 0 && height > 0 &&
            rgba.size() == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u) {
            game_icon_.upload_rgba8888(reinterpret_cast<const uint32_t*>(rgba.data()), width,
                                       height, width);
            game_icon_.set_linear_filtering(true);
        }
    }
    return game_icon_.valid() ? &game_icon_ : nullptr;
}

void Client::draw() {
    // Whatever a layer is not given this frame, it stops showing -- see
    // SystemVideoLayers::end_frame().
    router_.reset();
    video_layers_.begin_frame();
    draw_scene();
    video_layers_.end_frame();
}

void Client::draw_scene() {
    const float width = static_cast<float>(output_width_);
    const float height = static_cast<float>(output_height_);
    gl::log_error_once("something in the last frame was rejected by OpenGL");
    gl::bind_window_framebuffer();

    if (state_ == State::DataManagement) {
#if DS_LOCAL_PLAY
        if (data_screen_) data_screen_->render(quads_, text_, button_glyphs_, width, height);
#endif
        return;
    }
    if (state_ == State::Settings) {
#if DS_LOCAL_PLAY
        if (settings_screen_) settings_screen_->render(quads_, text_, button_glyphs_, width, height);
        update_prompt_.render(quads_, text_, button_glyphs_, width, height);
#endif
        return;
    }
    if (state_ == State::Launcher) {
        // The app's own opening screen, so choosing which screen to be looks
        // the same here as picking it there does.
        launcher_.render(quads_, text_, button_glyphs_, width, height,
                         static_cast<int>(session_.hosts().size()));
        if (!launcher_notice_.empty()) {
            render_notice_popup(quads_, text_, button_glyphs_, width, height, launcher_notice_);
        }
#if DS_LOCAL_PLAY
        update_prompt_.render(quads_, text_, button_glyphs_, width, height);
#endif
        return;
    }
    if (state_ == State::Connecting) {
        render_connecting_screen(quads_, text_, button_glyphs_, width, height,
                                 joining_.device_name, joining_.game_title, std::string(),
                                 &connecting_hits_);
        return;
    }
    if (state_ != State::Playing) {
        // The app's own host picker, so finding a game looks the same here as
        // it does there. A failure is shown in the same place, as its status.
        host_picker_.render(quads_, text_, button_glyphs_, width, height, session_.hosts(),
                            state_ == State::Failed ? failure_ : session_.status_detail());
        return;
    }

    if (session_.host_settings_open()) {
        // The host's own screens, whose presses still go to it -- see
        // handle_actions(). Its Controller Order screen is drawn here as well,
        // as the quick menu is; Settings and Data Management are about that
        // machine, and are looked at there.
        const net::ControllersViewMessage& view = session_.host_controllers_view();
        if (view.open) {
            // Over the game, darkened as the host's own page is: live for a
            // Wii U game, which goes on, and the blurred still otherwise.
            router_.decide(false);
            const ScreenTextures& textures = session_.screens();
            compositor_.render(quads_, session_layout(), textures, session_.screens_geometry(),
                               width, height, router_);
            if (session_.system() == SystemId::WiiU) {
                menu_backdrop_.forget();
                quads_.draw_solid(PixelRect{0.0f, 0.0f, width, height},
                                  Color{0.0f, 0.0f, 0.0f, kLiveGameDim});
            } else {
                if (!menu_backdrop_.matches(compositor_.composition_key())) {
                    menu_backdrop_.capture(static_cast<int>(width), static_cast<int>(height),
                                           compositor_.composition_key());
                }
                menu_backdrop_.draw(quads_, width, height);
                quads_.draw_solid(PixelRect{0.0f, 0.0f, width, height},
                                  Color{0.0f, 0.0f, 0.0f, 0.6f});
            }
            render_mirrored_controllers(mirrored_controllers_, quads_, text_, button_glyphs_,
                                        width, height, view, /*over_game=*/true);
        } else {
            render_centred_message(quads_, text_, width, height, "Look at the other screen.");
        }
        return;
    }

    compositor_.set_smoothing(session_.host_filtering());
    // All three are drawn over the game with GL, so the game has to be drawn
    // that way too while any is up.
    const bool resume_notice = session_.host_resume_notice();
    const bool unavailable_notice = session_.host_menu_unavailable_notice();
    router_.decide(!menu_open_ && !resume_notice && !unavailable_notice);
    const ScreenTextures& textures = session_.screens();
    compositor_.render(quads_, session_layout(), textures, session_.screens_geometry(), width,
                       height, router_);

    if (menu_open_ && session_.system() == SystemId::WiiU) {
        // A Wii U game goes on running under the menu -- Cemu cannot pause
        // it -- so it is shown live, darkened, the same as on the other device.
        menu_backdrop_.forget();
        quads_.draw_solid(PixelRect{0.0f, 0.0f, width, height},
                          Color{0.0f, 0.0f, 0.0f, kLiveGameDim});
        picker_.set_preview_screen(session_.granted_screen());
        picker_.set_device_screen(session_.system() == SystemId::WiiU
                                      ? std::optional<ScreenId>(session_.granted_screen())
                                      : std::nullopt);
        picker_.set_game_icon(host_game_icon());
        picker_.render(quads_, text_, button_glyphs_, width, height, session_.game_title(),
                       &textures);
    } else if (menu_open_) {
        // Taken between the game and the menu, the same as on the other device:
        // one menu, drawn over the same blurred frame wherever it is opened,
        // and taken again when the host moves the screens underneath it.
        if (!menu_backdrop_.matches(compositor_.composition_key())) {
            menu_backdrop_.capture(static_cast<int>(width), static_cast<int>(height),
                                   compositor_.composition_key());
        }
        menu_backdrop_.draw(quads_, width, height);
        // The previews are drawn from the frame the game is paused on, so each
        // tile shows what picking it will actually look like -- the same as on
        // the other device, because it is the same picker.
        picker_.set_preview_screen(session_.granted_screen());
        picker_.set_device_screen(session_.system() == SystemId::WiiU
                                      ? std::optional<ScreenId>(session_.granted_screen())
                                      : std::nullopt);
        picker_.set_game_icon(host_game_icon());
        picker_.render(quads_, text_, button_glyphs_, width, height, session_.game_title(),
                       &textures);
    } else {
        menu_backdrop_.forget();
        if (resume_notice) {
            render_notice_popup(quads_, text_, button_glyphs_, width, height, kResumedNotice);
        }
    }
    // The menu the host could not open, for as long as the host shows it.
    if (unavailable_notice) {
        render_corner_notice(quads_, text_, width, height, kMenuUnavailableNotice);
    }
}

#ifdef __SWITCH__

// Where the log goes on a console. There is no terminal behind the app and no
// shell to have started it from, so the only way to read what it said after
// the fact is to leave it on the SD card -- and the only way to read it as it
// happens is nxlink, which needs a computer already listening.
constexpr const char* kSwitchLogPath = "sdmc:/switch/Dualscreen.log";
// And a game played here, in a file of its own. Leaving a game starts the menu
// again, which starts its log afresh -- so a game sharing the menu's file would
// lose its whole log the moment the player left it, which is exactly when
// somebody would want to read it.
constexpr const char* kSwitchGameLogPath = "sdmc:/switch/Dualscreen-game.log";

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
    std::printf("Dualscreen could not start.\n\n");
    std::printf("  %s\n\n", message.c_str());
    std::printf("The whole log is on the SD card at\n");
    std::printf("  /switch/Dualscreen.log\n\n");
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
    set_log_file(DS_LOCAL_PLAY && options.local_play.wanted() ? kSwitchGameLogPath
                                                              : kSwitchLogPath);
    log_info("Dualscreen starting");

    // Sockets are not on by default, and without them there is nothing this
    // app can do at all. nxlink, if a computer is listening for it, gets the
    // same lines as they happen.
    // Not socketInitializeDefault(): its UDP receive buffer is 42 KB, and a
    // single H.264 keyframe for a 1280x720 screen at this app's bitrate is
    // 40-80 KB. One keyframe therefore overflowed the buffer on its own,
    // losing fragments -- which made this end ask for another keyframe, which
    // overflowed it again. Measured on a real session: 5-8 frames lost every
    // second, 7-8 keyframes asked for every second, and a picture that never
    // stopped breaking up. Everything else in that config is libnx's own
    // default; only the two buffers this app actually fills are raised.
    SocketInitConfig socket_config = *socketGetDefaultInitConfig();
    socket_config.udp_rx_buf_size = 1 * 1024 * 1024;
    socket_config.udp_tx_buf_size = 256 * 1024;
    // The transfer memory the buffers above are carved out of has to grow
    // with them, or the larger sizes are quietly clamped back down.
    socket_config.sb_efficiency = 8;
    const Result socket_result = socketInitialize(&socket_config);
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

#ifdef __ANDROID__
    // The app's own external files directory: readable over USB or by a file
    // manager without any storage permission, and taken away with the app when
    // it is uninstalled. Everything here also goes to logcat -- see log.cpp --
    // which is the equivalent of the Switch build's nxlink.
    if (const char* storage = SDL_AndroidGetExternalStoragePath()) {
        set_log_file(std::string(storage) + "/Dualscreen.log");
        // And everything else this app keeps -- settings, per-game options,
        // saves, save states, BIOS files -- under the same directory, in
        // dualscreen/ there, where a file manager or a USB cable can reach
        // it too. AppPaths reads where to put things the way a Linux desktop
        // says, and nothing on Android says anything, so this is said here.
        setenv("HOME", storage, 1);
        setenv("XDG_CONFIG_HOME", storage, 1);
        setenv("XDG_DATA_HOME", storage, 1);
    }
    log_info("Dualscreen starting");
#if DS_HAVE_VIDEO_CODEC
    // Once, before anything can ask this build's video decoder to open --
    // CompanionSession does that lazily, on the first frame the host actually
    // sends as video, which is well after this point but never before it.
    // SDL already holds the JNIEnv this thread was handed at start-up; all
    // this does is ask it for the JavaVM underneath, which is what FFmpeg's
    // side of the MediaCodec bridge wants instead.
    if (JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv())) {
        JavaVM* vm = nullptr;
        if (env->GetJavaVM(&vm) == 0 && vm != nullptr) {
            if (av_jni_set_java_vm(vm, nullptr) < 0) {
                log_warn("could not hand this device's video decoder a Java VM");
            }
        }
    }
#endif
#endif

#ifdef __APPLE__
    // A log on disk, because an Apple application has nowhere else to put one.
    // Double-clicked from Finder -- which is how this is going to be started
    // -- stdout goes to a system log nobody thinks to look in, and the app
    // may be the only thing on the network that knows why it could not find a
    // host. ~/Library/Logs is where a Mac keeps these and where Console.app
    // will show it; see PathConvention for the layout and the test that pins
    // it down from a machine that is not a Mac.
    //
    // It matters more on a phone or an Apple TV than on a Mac, not less: there
    // is no Console there, no terminal, and nothing to read but the container.
    // tvOS has nowhere permanent at all and gets Library/Caches -- again see
    // PathConvention, which answers for all three.
    {
        const AppPaths paths = AppPaths::resolve();
        if (ensure_directory(path_dirname(paths.log_file)) && set_log_file(paths.log_file)) {
            log_info("Dualscreen starting; log at " + paths.log_file);
        } else {
            log_info("Dualscreen starting");
        }
    }
#endif

    // Which build this actually is, on every platform rather than only the one
    // whose bundle hides it best. See build_stamp.hpp.
    log_build_stamp();

    // Whether this build can play a game itself, said once up front: a build
    // made without the core in place looks exactly like one with it until
    // the menu is missing a row.
    log_info(can_play_here() ? "this build carries a DS core and can play DS games here"
                             : "this build has no DS core; it joins games running elsewhere");

    // This program's own path, for starting it again -- see local_play.hpp.
    [[maybe_unused]] const std::string self_path = argc > 0 && argv[0] != nullptr ? argv[0] : "";

    int status = 0;
    // The player chose Quit while playing here, or the system closed the
    // app: nothing to go back to -- see leave_to_home().
    [[maybe_unused]] bool quit_chosen = false;
    if (!options.parse_error.empty()) {
        log_error(options.parse_error);
        status = 2;
#if DS_LOCAL_PLAY
    } else if (options.local_play.wanted()) {
        // A game, or the browser to choose one: the desktop app does the
        // playing, in this process. Sent here from the menu, the menu comes
        // back when the game is over; started by a game's forwarder, leaving
        // the game leaves the program, as leaving any game does.
        LocalPlayRequest request = local_play_request(options);
        request.self_path = self_path;
        status = play_here(request, &quit_chosen);
        if (options.local_play.browse && !quit_chosen) {
            relaunch(self_path, "");
        }
#endif
    } else {
        // Round again only on a device that plays a game in this process
        // and comes back to the menu after -- see below.
        Options menu_options = options;
        menu_options.self_path = self_path;
        for (bool again = true; again;) {
            again = false;
            Client client(menu_options);
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
#if DS_LOCAL_PLAY
            if (client.wants_to_play_here() && !relaunch(self_path, kBrowseArgument)) {
                // Nothing to ask to start this again, so it plays here, now
                // that the menu's window is gone.
                LocalPlayRequest browse = local_play_request(options);
                browse.browse = true;
                browse.self_path = self_path;
                status = play_here(browse, &quit_chosen);
#ifdef __ANDROID__
                // And back to the menu afterwards, the way a Switch comes
                // back to it by being started again: an Android app is one
                // process the system starts, and has nobody to ask for
                // another. Quit closes the app instead, as it does there.
                // The menu comes back as itself, not joining anything the
                // command line asked for the first time round.
                again = !quit_chosen;
                menu_options.auto_connect = false;
#endif
            }
#endif
        }
    }

#if DS_LOCAL_PLAY
    if (quit_chosen) leave_to_home();
#endif
    log_info("Dualscreen exiting");
#ifdef __SWITCH__
    socketExit();
#endif
    close_log_file();
    return status;
}
