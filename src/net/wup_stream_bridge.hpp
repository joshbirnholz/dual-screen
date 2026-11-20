#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "input/ds_input.hpp"
#include "core/system.hpp"

namespace ds::net {

// Adapter between Dualscreen's host-side frame/input types and libwupstream.
// This deliberately does not own screen selection or menu policy: the caller
// chooses the GamePad view, composites any overlay, then submits RGBA frames.
//
// open() waits for a WUP Stream-compatible GamePad and should be called from a
// worker if the application must remain responsive while discovery runs.
class WupStreamBridge {
public:
    WupStreamBridge();
    ~WupStreamBridge();
    WupStreamBridge(const WupStreamBridge&) = delete;
    WupStreamBridge& operator=(const WupStreamBridge&) = delete;

    bool open(const std::string& address, int timeout_ms, std::string* error);
    void close();
    bool connected() const;

    // RGBA byte pixels, top row first. The frame is scaled to the GamePad's
    // 854x480 panel. submit_frame() is paced by libwupstream to the pad refresh
    // and may block until that frame has been sent.
    bool submit_frame(const uint8_t* rgba, int width, int height, int stride_bytes);

    // Interleaved signed 16-bit stereo at 48 kHz, as required by libwupstream.
    bool submit_audio(const int16_t* samples, int frames);

    // Read the latest GamePad state and map it into Dualscreen's normal input
    // state. If system is DS/3DS, touch coordinates are scaled from the pad's
    // 854x480 surface into the console touch-screen coordinates.
    bool poll_input(SystemId system, int touch_width, int touch_height,
                    DsInputState& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ds::net
