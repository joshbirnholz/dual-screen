#include "net/wup_stream_bridge.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>

#include "wupstream.h"

namespace ds::net {

struct WupStreamBridge::Impl {
    mutable std::mutex mutex;
    wupstream* stream = nullptr;
};

namespace {

int16_t stick_axis(int value) {
    constexpr int kWupFullScale = 1100;
    const int clamped = std::clamp(value, -kWupFullScale, kWupFullScale);
    return static_cast<int16_t>((clamped * 32767) / kWupFullScale);
}

void set_button(DsInputState& state, DsButton button, uint32_t buttons, uint32_t mask) {
    state.set(button, (buttons & mask) != 0);
}

}  // namespace

WupStreamBridge::WupStreamBridge() : impl_(new Impl) {}
WupStreamBridge::~WupStreamBridge() { close(); }

bool WupStreamBridge::open(const std::string& address, int timeout_ms, std::string* error) {
    close();
    const char* ip = address.empty() ? nullptr : address.c_str();
    wupstream* stream = wupstream_open(ip, timeout_ms);
    if (stream == nullptr) {
        if (error != nullptr) {
            *error = address.empty()
                         ? "no WUP Stream-compatible GamePad answered discovery"
                         : "could not connect to the GamePad at " + address;
        }
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stream = stream;
    return true;
}

void WupStreamBridge::close() {
    wupstream* stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        stream = impl_->stream;
        impl_->stream = nullptr;
    }
    if (stream != nullptr) wupstream_close(stream);
}

bool WupStreamBridge::connected() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->stream != nullptr;
}

bool WupStreamBridge::submit_frame(const uint8_t* rgba, int width, int height, int stride_bytes) {
    if (rgba == nullptr || width <= 0 || height <= 0 || stride_bytes < width * 4) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stream == nullptr) return false;

    // Nearest-neighbour scaling is intentionally small and predictable. The
    // GamePad's own panel is 854x480; a future compositor can provide a
    // pre-scaled frame and take this loop out of the hot path.
    constexpr int kWidth = WUPSTREAM_W;
    constexpr int kHeight = WUPSTREAM_H;
    static thread_local uint8_t scaled[kWidth * kHeight * 4];
    for (int y = 0; y < kHeight; ++y) {
        const int sy = std::min(height - 1, (static_cast<int64_t>(y) * height) / kHeight);
        const uint8_t* source_row = rgba + static_cast<std::size_t>(sy) * stride_bytes;
        uint8_t* destination_row = scaled + static_cast<std::size_t>(y) * kWidth * 4;
        for (int x = 0; x < kWidth; ++x) {
            const int sx = std::min(width - 1, (static_cast<int64_t>(x) * width) / kWidth);
            const uint8_t* source = source_row + sx * 4;
            uint8_t* destination = destination_row + x * 4;
            destination[0] = source[0];
            destination[1] = source[1];
            destination[2] = source[2];
            destination[3] = 0xff;
        }
    }
    if (wupstream_frame(impl_->stream, scaled, kWidth * 4, WUPSTREAM_RGBA) != 0) {
        impl_->stream = nullptr;
        return false;
    }
    return true;
}

bool WupStreamBridge::submit_audio(const int16_t* samples, int frames) {
    if (frames < 0 || (frames > 0 && samples == nullptr)) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stream == nullptr) return false;
    if (wupstream_audio(impl_->stream, samples, frames) != 0) {
        impl_->stream = nullptr;
        return false;
    }
    return true;
}

bool WupStreamBridge::poll_input(SystemId system, int touch_width, int touch_height,
                                 DsInputState& out) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->stream == nullptr) return false;
    wupstream_input_t input{};
    if (wupstream_input(impl_->stream, &input) != 0) {
        impl_->stream = nullptr;
        out.clear();
        return false;
    }

    out.clear();
    set_button(out, DsButton::A, input.buttons, WUPSTREAM_A);
    set_button(out, DsButton::B, input.buttons, WUPSTREAM_B);
    set_button(out, DsButton::X, input.buttons, WUPSTREAM_X);
    set_button(out, DsButton::Y, input.buttons, WUPSTREAM_Y);
    set_button(out, DsButton::L, input.buttons, WUPSTREAM_L);
    set_button(out, DsButton::R, input.buttons, WUPSTREAM_R);
    set_button(out, DsButton::Zl, input.buttons, WUPSTREAM_ZL);
    set_button(out, DsButton::Zr, input.buttons, WUPSTREAM_ZR);
    set_button(out, DsButton::Start, input.buttons, WUPSTREAM_PLUS);
    set_button(out, DsButton::Select, input.buttons, WUPSTREAM_MINUS);
    set_button(out, DsButton::Up, input.buttons, WUPSTREAM_UP);
    set_button(out, DsButton::Down, input.buttons, WUPSTREAM_DOWN);
    set_button(out, DsButton::Left, input.buttons, WUPSTREAM_LEFT);
    set_button(out, DsButton::Right, input.buttons, WUPSTREAM_RIGHT);
    set_button(out, DsButton::LeftStick, input.buttons, WUPSTREAM_L3);
    set_button(out, DsButton::RightStick, input.buttons, WUPSTREAM_R3);

    out.circle_x = stick_axis(input.lx);
    out.circle_y = stick_axis(input.ly);
    out.c_stick_x = stick_axis(input.rx);
    out.c_stick_y = stick_axis(input.ry);
    out.touching = input.touched != 0;
    if (out.touching && touch_width > 0 && touch_height > 0) {
        out.touch_x = std::clamp(input.tx * touch_width / WUPSTREAM_W, 0, touch_width - 1);
        out.touch_y = std::clamp(input.ty * touch_height / WUPSTREAM_H, 0, touch_height - 1);
    }

    // WUP Stream reports raw sensor samples. The exact calibration/units
    // conversion must be verified against Cemu's GamePad expectations before
    // enabling motion forwarding; do not mark it present with guessed units.
    (void)system;
    return true;
}

}  // namespace ds::net
