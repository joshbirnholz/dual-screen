#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A compressed video path, for the one screen that cannot go down the tile
// codec. See docs/research-notes.md: a Wii U GamePad measures at 43 Mbit/s
// through Rgb565DeflateTiles and 2.5 Mbit/s through H.264, and the tile encoder
// alone does not fit in a 60fps frame at that size.
//
// FFmpeg rather than a streaming stack, deliberately. The hard part of this was
// hardware decode on a Switch, and that turns out to be an ordinary FFmpeg
// hwaccel (AV_HWDEVICE_TYPE_NVTEGRA) rather than anything belonging to a
// particular client -- so the platform differences here are a codec name and a
// device type, not a dependency. Moonlight and Sunshine are GPLv3; FFmpeg is
// LGPL unless it is built with GPL pieces, and hardware encoders are not those.
//
// That last point is a build-time obligation rather than a code one: a shipping
// build should have h264_vaapi and no libx264. The software encoder named below
// is for tests and for a desktop with no VAAPI, and is the one thing here that
// can drag a GPL dependency in.
namespace ds::video {

// Whether this build has a video codec at all. The Switch and Android
// toolchains do not have FFmpeg wired up yet, and everything DS and 3DS do
// works without it, so it stays optional until those are ready.
bool available();

// Keeps FFmpeg's own chatter out of this app's log; only errors survive.
// Called for you when an encoder or decoder opens.
void quieten_ffmpeg();

struct EncodedFrame {
    std::vector<uint8_t> bytes;
    // Whether this frame stands alone. A device joining mid-stream can draw
    // nothing until one of these arrives.
    bool keyframe = false;

    bool empty() const { return bytes.empty(); }
};

struct DecodedFrame {
    // XRGB8888, the byte order everything else in this app already uses.
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

struct EncoderConfig {
    int width = 0;
    int height = 0;
    int fps = 60;
    // What the link is being asked to carry. A LAN has room for far more than
    // this, but every bit spent is latency when a burst meets a slow radio.
    int bitrate_kbps = 4000;
    // Tried in order; the first that opens wins. Hardware first, always: the
    // host is running an emulator with the rest of the frame.
    std::vector<std::string> backends{"h264_vaapi", "libx264"};
};

struct DecoderConfig {
    // Tried in order, the same way. On a Switch this is where the NVTEGRA
    // hwaccel goes; on Android, MediaCodec.
    std::vector<std::string> backends{"h264"};
};

}  // namespace ds::video
