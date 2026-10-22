#include "core/external_screen_source.hpp"

#include "util/log.hpp"

namespace ds {

ExternalScreenSource::ExternalScreenSource(SystemId system) : system_(system) {}

ExternalScreenSource::~ExternalScreenSource() { close(); }

bool ExternalScreenSource::open(const std::string& socket_path, std::string* error) {
    close();
    if (!link_.listen(socket_path, error)) {
        return false;
    }
    ended_ = false;
    ended_reason_.clear();
    log_info("waiting for a game on " + socket_path + " (" +
             std::to_string(ExternalSharedHeader::total_bytes() / (1024 * 1024)) +
             " MB shared)");
    return true;
}

void ExternalScreenSource::close() {
    link_.close();
    top_ = Screen();
    bottom_ = Screen();
    textures_ = ScreenTextures();
    audio_[0].clear();
    audio_[1].clear();
}

bool ExternalScreenSource::advance_frame(std::string* error) {
    if (ended_) {
        // Already reported. Saying so again every frame would fill a log with
        // one event.
        if (error != nullptr) *error = ended_reason_;
        return false;
    }
    std::string reason;
    const bool was_connected = link_.connected();
    if (!link_.poll(&reason)) {
        // A game closing is how a session ends here, not a fault -- but there
        // is nothing more to draw either way, so it stops the source.
        ended_ = true;
        ended_reason_ = reason;
        log_info("the game ended: " + reason);
        if (error != nullptr) *error = reason;
        return false;
    }
    if (!was_connected && link_.connected()) {
        // Said here rather than inside the link, so that the half of the link a
        // fork of Cemu compiles brings nothing of this app with it.
        const ExternalStreamInfo& info = link_.info();
        // Without the sizes, which were never the stream's.
        //
        // They are what the producer declared before the game had drawn
        // anything: a Wii U's nominal television and GamePad, hardcoded, and
        // nothing anywhere clamps a frame to them -- the shared memory is
        // sized for 4K, and a title rendering its television at 1080p
        // publishes 1080p. But the line read as "this stream is 720p", which
        // is the single most useful-looking thing in the log and was wrong,
        // and it got read that way by the person who wrote it while
        // diagnosing exactly this.
        //
        // What actually arrives is logged per screen below, as it arrives.
        log_info("a game connected at " + std::to_string(info.audio_sample_rate) + " Hz");
    }

    for (const ScreenId id : {ScreenId::Top, ScreenId::Bottom}) {
        ExternalFrame frame;
        if (!link_.take_frame(id, frame) || !frame.valid()) {
            continue;  // nothing new for this screen, which is not a problem
        }
        Screen& screen = screen_for(id);
        // The size the game is really drawing this screen at, which is the
        // top of the chain and the only number in it nothing here chooses.
        // Everything downstream is visible already -- what the encoder was
        // opened at and why, and what the far end composites from -- so with
        // this one, "the picture is lower resolution than it should be" can
        // be traced from end to end in a single log instead of inferred by
        // laying several side by side.
        if (screen.width != frame.width || screen.height != frame.height) {
            log_info(std::string("the game is drawing its ") + to_string(id) + " screen at " +
                     std::to_string(frame.width) + "x" + std::to_string(frame.height));
        }
        screen.pixels = std::move(frame.pixels);
        screen.width = frame.width;
        screen.height = frame.height;
        screen.serial = frame.serial;
    }

    for (const ScreenId id : {ScreenId::Top, ScreenId::Bottom}) {
        std::vector<int16_t>& audio = audio_[ExternalSharedHeader::screen_index(id)];
        link_.take_audio(id, audio);
        if (!audio.empty() && audio_callback_) {
            audio_callback_(id, audio.data(), audio.size() / 2);
        }
    }

    bool rumble_active = false;
    if (link_.take_rumble(rumble_active) && rumble_callback_) {
        rumble_callback_(rumble_active);
    }
    int player = 0;
    bool player_active = false;
    while (link_.take_player_rumble(player, player_active)) {
        if (player_rumble_callback_) {
            player_rumble_callback_(player, player_active);
        }
    }
    return true;
}

const ScreenTextures& ExternalScreenSource::screens() {
    for (Screen* screen : {&top_, &bottom_}) {
        const bool stale = !screen->texture.valid() || screen->serial != screen->uploaded_serial;
        if (stale && screen->width > 0 && screen->height > 0 && !screen->pixels.empty()) {
            // RGBA, not upload_xrgb8888()'s byte-swapped XRGB: what crosses
            // this link is a GPU's own render targets (Cemu's Vulkan
            // readback), already in the order a GPU-filled texture is in,
            // not a CPU-rendered libretro core's swapped one.
            screen->texture.upload_rgba8888(screen->pixels.data(), screen->width, screen->height,
                                            screen->width);
            screen->uploaded_serial = screen->serial;
        }
    }
    textures_.top = top_.texture.valid() ? &top_.texture : nullptr;
    textures_.bottom = bottom_.texture.valid() ? &bottom_.texture : nullptr;
    return textures_;
}

SystemScreens ExternalScreenSource::screens_geometry() const {
    // The console's own, not whatever is arriving. An emulator may be rendering
    // above native, and everything that reads this -- where a touch lands,
    // which layout is which shape -- is asking about the console rather than
    // about how many pixels happen to be in flight.
    return system_profile(system_).screens;
}

bool ExternalScreenSource::read_screen(ScreenId screen, ScreenReadback& out, int, int) {
    const Screen& source = screen_for(screen);
    if (source.width <= 0 || source.height <= 0 || source.pixels.empty()) {
        return false;
    }
    // Already on the CPU: it arrived that way through shared memory, so unlike
    // a core rendering on the GPU there is nothing to read back and no reason
    // to care what size the recipient wanted.
    out.pixels = source.pixels.data();
    out.width = source.width;
    out.height = source.height;
    out.stride_pixels = source.width;
    // A GPU's own render targets (Cemu's Vulkan readback), not a CPU-rendered
    // libretro core's byte-swapped XRGB8888 -- see upload_rgba8888().
    out.channel_order = ChannelOrder::Rgba;
    return true;
}

namespace {

ExternalInput to_external_input(const DsInputState& state) {
    // A pair per row: this app's name for a control, and the GamePad's. Written
    // as data so that adding a button is adding a line, and so that nothing is
    // silently left out -- the static_assert below is what says so.
    static const struct {
        DsButton from;
        ExternalButton to;
    } kButtons[] = {
        {DsButton::A, ExternalButton::A},
        {DsButton::B, ExternalButton::B},
        {DsButton::X, ExternalButton::X},
        {DsButton::Y, ExternalButton::Y},
        {DsButton::L, ExternalButton::L},
        {DsButton::R, ExternalButton::R},
        {DsButton::Zl, ExternalButton::Zl},
        {DsButton::Zr, ExternalButton::Zr},
        {DsButton::Start, ExternalButton::Plus},
        {DsButton::Select, ExternalButton::Minus},
        {DsButton::Up, ExternalButton::Up},
        {DsButton::Down, ExternalButton::Down},
        {DsButton::Left, ExternalButton::Left},
        {DsButton::Right, ExternalButton::Right},
        {DsButton::LeftStick, ExternalButton::LeftStick},
        {DsButton::RightStick, ExternalButton::RightStick},
    };
    static_assert(sizeof(kButtons) / sizeof(kButtons[0]) ==
                      static_cast<std::size_t>(DsButton::Count),
                  "a button was added without saying what it is on a GamePad");

    ExternalInput input;
    for (const auto& pair : kButtons) {
        input.set(pair.to, state.pressed(pair.from));
    }
    input.left_x = state.circle_x;
    input.left_y = state.circle_y;
    input.right_x = state.c_stick_x;
    input.right_y = state.c_stick_y;
    input.touching = state.touching;
    // Straight across, unconverted: DsMotionState and ExternalMotion are the
    // same readings in the same units and axes on purpose, so that the one
    // place they are interpreted is the producer's own sensor fusion.
    input.motion.present = state.motion.present;
    input.motion.gyro_x = state.motion.gyro_x;
    input.motion.gyro_y = state.motion.gyro_y;
    input.motion.gyro_z = state.motion.gyro_z;
    input.motion.accel_x = state.motion.accel_x;
    input.motion.accel_y = state.motion.accel_y;
    input.motion.accel_z = state.motion.accel_z;
    input.motion.sampled_us = state.motion.sampled_us;
    // Already in the touch screen's own pixels -- see DsInputState -- which for
    // this console is the GamePad's.
    input.touch_x = static_cast<uint16_t>(state.touch_x < 0 ? 0 : state.touch_x);
    input.touch_y = static_cast<uint16_t>(state.touch_y < 0 ? 0 : state.touch_y);
    return input;
}

}  // namespace

void ExternalScreenSource::submit_input(const DsInputState& state) {
    link_.send_input(to_external_input(state));
}

void ExternalScreenSource::submit_player_input(int player, bool fed, const DsInputState& state) {
    // A player nothing is feeding is sent at rest as well as unplugged, so a
    // pad taken away mid-press does not leave its buttons held down.
    link_.send_player_input(player, fed, fed ? to_external_input(state) : ExternalInput());
}

void ExternalScreenSource::submit_input(const DsInputState& state, uint32_t buttons) {
    ExternalInput input = to_external_input(state);
    input.buttons = buttons;
    link_.send_input(input);
}

void ExternalScreenSource::submit_player_input(int player, bool fed, const DsInputState& state, uint32_t buttons) {
    ExternalInput input;
    if (fed) {
        input = to_external_input(state);
        input.buttons = buttons;
    }
    link_.send_player_input(player, fed, input);
}

double ExternalScreenSource::audio_sample_rate() const {
    const int rate = link_.info().audio_sample_rate;
    return rate > 0 ? static_cast<double>(rate) : 48000.0;
}

double ExternalScreenSource::frames_per_second() const {
    const double rate = link_.info().frames_per_second;
    return rate > 0.0 ? rate : 60.0;
}

}  // namespace ds
