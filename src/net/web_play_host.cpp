#include "net/web_play_host.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "util/log.hpp"
#include "video/video_encoder.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#define STBIW_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb/stb_image_resize2.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ds::net {

namespace {

int index_of(ScreenId screen) { return screen == ScreenId::Top ? 0 : 1; }

// The biggest even size inside the limit with the source's shape.
void fit_within(int width, int height, int max_width, int max_height, int& out_width, int& out_height) {
    out_width = width;
    out_height = height;
    if (out_width > max_width || out_height > max_height) {
        const double scale = std::min(static_cast<double>(max_width) / width, static_cast<double>(max_height) / height);
        out_width = static_cast<int>(width * scale);
        out_height = static_cast<int>(height * scale);
    }
    out_width = std::max(2, out_width & ~1);
    out_height = std::max(2, out_height & ~1);
}

void append_bytes(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(context);
    const auto* bytes = static_cast<const uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<uint8_t> web_menu_message(const uint8_t* rgba, int width, int height, int max_width, int max_height) {
    std::vector<uint8_t> out;
    if (rgba == nullptr || width <= 0 || height <= 0) return out;
    int w = width;
    int h = height;
    if (w > max_width || h > max_height) {
        const double scale = std::min(static_cast<double>(max_width) / w, static_cast<double>(max_height) / h);
        w = std::max(1, static_cast<int>(w * scale));
        h = std::max(1, static_cast<int>(h * scale));
    }
    std::vector<uint8_t> scaled;
    const uint8_t* pixels = rgba;
    if (w != width || h != height) {
        scaled.resize(static_cast<std::size_t>(w) * h * 4);
        stbir_resize_uint8_srgb(rgba, width, height, 0, scaled.data(), w, h, 0, STBIR_RGBA);
        pixels = scaled.data();
    }
    // Paused, the game's picture is behind all of it and a photo's
    // compression suits it; over a game still running, the menu needs its
    // see-through parts, which only a PNG keeps.
    bool opaque = true;
    for (std::size_t i = 3; i < static_cast<std::size_t>(w) * h * 4; i += 4) {
        if (pixels[i] != 255) {
            opaque = false;
            break;
        }
    }
    out.push_back(static_cast<uint8_t>(WebMessage::Menu));
    out.push_back(opaque ? 0 : 1);
    out.push_back(static_cast<uint8_t>(w & 0xFF));
    out.push_back(static_cast<uint8_t>(w >> 8));
    out.push_back(static_cast<uint8_t>(h & 0xFF));
    out.push_back(static_cast<uint8_t>(h >> 8));
    const std::size_t header = out.size();
    const int ok = opaque ? stbi_write_jpg_to_func(append_bytes, &out, w, h, 4, pixels, 85)
                          : stbi_write_png_to_func(append_bytes, &out, w, h, 4, pixels, w * 4);
    if (!ok || out.size() == header) out.clear();
    return out;
}

struct WebPlayHost::Encoder {
    video::VideoEncoder encoder;
    bool failed = false;
    video::EncodedFrame out;
};

WebPlayHost::~WebPlayHost() { stop(); }

bool WebPlayHost::start(const Config& config, std::string* error) {
    stop();
    if (!video::can_encode()) {
        if (error) *error = "this build has no video encoder";
        return false;
    }
    config_ = config;
    WebPlayServer::Config server_config;
    server_config.port = config.port;
    server_config.identity = config.identity;
    server_config.page = config.page;
    server_config.device_name = config.device_name;
    server_config.game_title = config.game_title;
    if (!server_.start(server_config, error)) return false;
    port_ = server_.port();
    for (auto*& encoder : encoders_) encoder = new Encoder();
    quit_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void WebPlayHost::stop() {
    if (thread_.joinable()) {
        quit_ = true;
        wake_.notify_all();
        thread_.join();
    }
    server_.stop();
    for (auto*& encoder : encoders_) {
        delete encoder;
        encoder = nullptr;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    viewers_.clear();
    pads_.clear();
    watched_[0] = watched_[1] = false;
    heard_[0] = heard_[1] = false;
    ++roster_serial_;
    running_ = false;
}

bool WebPlayHost::screen_watched(ScreenId screen) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return watched_[index_of(screen)];
}

void WebPlayHost::publish_screen(ScreenId screen, const uint32_t* pixels, int width, int height, int stride_pixels,
                                 video::PixelOrder order) {
    if (!running_ || pixels == nullptr || width <= 0 || height <= 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PendingScreen& pending = pending_[index_of(screen)];
        pending.pixels.resize(static_cast<std::size_t>(width) * height);
        for (int row = 0; row < height; ++row) {
            std::memcpy(&pending.pixels[static_cast<std::size_t>(row) * width],
                        pixels + static_cast<std::size_t>(row) * stride_pixels, sizeof(uint32_t) * width);
        }
        pending.width = width;
        pending.height = height;
        pending.order = order;
        pending.fresh = true;
    }
    wake_.notify_all();
}

void WebPlayHost::publish_menu(const uint8_t* rgba, int width, int height) {
    if (!running_ || rgba == nullptr || width <= 0 || height <= 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        menu_.pixels.assign(rgba, rgba + static_cast<std::size_t>(width) * height * 4);
        menu_.width = width;
        menu_.height = height;
        menu_.open = true;
        menu_.fresh = true;
    }
    wake_.notify_all();
}

void WebPlayHost::close_menu() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!menu_.open && !menu_.fresh) return;
        menu_.pixels.clear();
        menu_.open = false;
        menu_.fresh = true;
    }
    wake_.notify_all();
}

void WebPlayHost::publish_audio(ScreenId screen, const int16_t* samples, std::size_t frames, uint32_t rate) {
    if (!running_ || samples == nullptr || frames == 0) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        PendingAudio& audio = audio_[index_of(screen)];
        if (audio.rate != rate) audio.samples.clear();
        audio.rate = rate;
        // Never more than a quarter of a second waiting: past that the
        // thread has stalled, and old sound is worse than none.
        if (audio.samples.size() > static_cast<std::size_t>(rate) / 2) audio.samples.clear();
        audio.samples.insert(audio.samples.end(), samples, samples + frames * 2);
    }
    wake_.notify_all();
}

std::vector<WebViewer> WebPlayHost::viewers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return viewers_;
}

std::vector<WebPad> WebPlayHost::pads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pads_;
}

uint64_t WebPlayHost::roster_serial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return roster_serial_;
}

void WebPlayHost::set_viewer_screen(uint32_t viewer, ScreenId screen) {
    Command command;
    command.kind = Command::Kind::Screen;
    command.target = viewer;
    command.screen = screen;
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}

void WebPlayHost::send_rumble(uint32_t pad_id, uint16_t low, uint16_t high, uint32_t duration_ms) {
    Command command;
    command.kind = Command::Kind::Rumble;
    command.target = pad_id;
    command.low = low;
    command.high = high;
    command.duration_ms = duration_ms;
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}

void WebPlayHost::send_text(uint32_t viewer, const std::string& json) {
    Command command;
    command.kind = Command::Kind::Text;
    command.target = viewer;
    command.text = json;
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.push_back(std::move(command));
}

bool WebPlayHost::screen_heard(ScreenId screen) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heard_[index_of(screen)];
}

std::string WebPlayHost::backend(ScreenId screen) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backend_[index_of(screen)];
}

void WebPlayHost::encode(ScreenId screen, PendingScreen& frame) {
    Encoder& state = *encoders_[index_of(screen)];
    if (state.failed) return;
    video::VideoEncoder& encoder = state.encoder;
    std::string error;
    const bool tv = screen == ScreenId::Top;
    if (encoder.is_open() && encoder.pixel_order() != frame.order) encoder.close();
    if (encoder.is_open() && (encoder.source_width() != frame.width || encoder.source_height() != frame.height)) {
        // A new size starts a new stream: the browser's decoder is told by
        // the keyframe that comes first.
        encoder.close();
    }
    if (!encoder.is_open()) {
        video::EncoderConfig config;
        fit_within(frame.width, frame.height, tv ? config_.max_tv_width : config_.max_gamepad_width,
                   tv ? config_.max_tv_height : config_.max_gamepad_height, config.width, config.height);
        config.source_width = frame.width;
        config.source_height = frame.height;
        config.fps = 60;
        config.bitrate_kbps = config_.bitrate_kbps;
        config.pixel_order = frame.order;
        if (!encoder.open(config, &error)) {
            log_warn("Web Play: no video for the " + std::string(tv ? "TV" : "GamePad") + ": " + error);
            state.failed = true;
            return;
        }
        log_info("Web Play: sending the " + std::string(tv ? "TV" : "GamePad") + " at " +
                 std::to_string(encoder.width()) + "x" + std::to_string(encoder.height()) + " with " +
                 encoder.backend());
        std::lock_guard<std::mutex> lock(mutex_);
        backend_[index_of(screen)] = encoder.backend();
    }
    if (server_.take_keyframe_request(screen)) encoder.request_keyframe();
    if (!encoder.encode(frame.pixels.data(), frame.width, state.out, &error)) {
        log_warn("Web Play: encoding failed: " + error);
        encoder.close();
        return;
    }
    if (!state.out.empty()) {
        server_.send_video(screen, state.out.bytes.data(), state.out.bytes.size(), state.out.keyframe, encoder.width(),
                           encoder.height());
    }
}

void WebPlayHost::run() {
    PendingScreen working[2];
    std::vector<int16_t> sound[2];
    uint32_t sound_rate[2] = {0, 0};
    std::vector<Command> commands;
    PendingMenu menu;
    while (!quit_) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool anything = pending_[0].fresh || pending_[1].fresh || !audio_[0].samples.empty() ||
                                  !audio_[1].samples.empty() || !commands_.empty() || menu_.fresh;
            if (!anything) wake_.wait_for(lock, std::chrono::milliseconds(2));
            for (int i = 0; i < 2; ++i) {
                if (pending_[i].fresh) {
                    std::swap(working[i], pending_[i]);
                    pending_[i].fresh = false;
                }
                sound[i].clear();
                std::swap(sound[i], audio_[i].samples);
                sound_rate[i] = audio_[i].rate;
            }
            commands.clear();
            std::swap(commands, commands_);
            if (menu_.fresh) {
                std::swap(menu, menu_);
                menu_.fresh = false;
                menu_.open = menu.open;
            }
        }
        if (menu.fresh) {
            menu.fresh = false;
            if (menu.open) {
                std::vector<uint8_t> message = web_menu_message(menu.pixels.data(), menu.width, menu.height,
                                                                config_.max_tv_width, config_.max_tv_height);
                if (!message.empty()) server_.send_menu(true, std::move(message));
            } else {
                server_.send_menu(false, {});
            }
        }
        for (const Command& command : commands) {
            switch (command.kind) {
                case Command::Kind::Screen: server_.set_viewer_screen(command.target, command.screen); break;
                case Command::Kind::Rumble:
                    server_.send_rumble(command.target, command.low, command.high, command.duration_ms);
                    break;
                case Command::Kind::Text: server_.send_text(command.target, command.text); break;
            }
        }
        server_.pump();
        for (int i = 0; i < 2; ++i) {
            const ScreenId screen = i == 0 ? ScreenId::Top : ScreenId::Bottom;
            if (!sound[i].empty() && sound_rate[i] != 0) {
                server_.send_audio(screen, sound[i].data(), sound[i].size() / 2, sound_rate[i], 2);
            }
            if (working[i].fresh) {
                working[i].fresh = false;
                if (server_.screen_watched(screen)) encode(screen, working[i]);
            }
        }
        server_.pump();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // The pads every pass, since what they hold changes every pass.
            viewers_ = server_.viewers();
            pads_ = server_.pads();
            roster_serial_ = server_.roster_serial();
            watched_[0] = server_.screen_watched(ScreenId::Top);
            watched_[1] = server_.screen_watched(ScreenId::Bottom);
            heard_[0] = server_.screen_heard(ScreenId::Top);
            heard_[1] = server_.screen_heard(ScreenId::Bottom);
        }
    }
}

}  // namespace ds::net
