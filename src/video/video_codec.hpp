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

// How this device sends screens to another one.
//
// A preference rather than a capability: what the build can do is available()
// below, and what the console being played wants is SystemProfile::prefers_video.
// This is the player overruling both, which is the whole of what it is for --
// the only reason to reach for it is to see what the other answer looks like on
// a particular pair of machines and a particular network.
//
// Overruling it towards video is still a request rather than a guarantee. The
// far end has to be able to decode, and a screen with an odd number of pixels
// cannot go through 4:2:0 at all; either way the session falls back to tiles
// and says so in the log.
enum class ScreenCodec {
    Auto,   // whatever the console being played asks for
    Tiles,  // changed tiles, whatever it asks for
    H264,   // video, wherever the far end can take it
};

// The order the settings screen cycles through, which is also the order above.
inline ScreenCodec next_screen_codec(ScreenCodec codec) {
    switch (codec) {
        case ScreenCodec::Auto: return ScreenCodec::Tiles;
        case ScreenCodec::Tiles: return ScreenCodec::H264;
        case ScreenCodec::H264: return ScreenCodec::Auto;
    }
    return ScreenCodec::Auto;
}

// What settings.json holds, and what --video-codec accepts.
inline const char* screen_codec_key(ScreenCodec codec) {
    switch (codec) {
        case ScreenCodec::Auto: return "auto";
        case ScreenCodec::Tiles: return "tiles";
        case ScreenCodec::H264: return "h264";
    }
    return "auto";
}

// Anything unrecognised is Auto rather than an error: a typo in a config file
// should leave the app working the way it would have anyway.
inline ScreenCodec screen_codec_from_key(const std::string& key) {
    if (key == screen_codec_key(ScreenCodec::Tiles)) return ScreenCodec::Tiles;
    if (key == screen_codec_key(ScreenCodec::H264)) return ScreenCodec::H264;
    return ScreenCodec::Auto;
}

inline const char* to_string(ScreenCodec codec) {
    switch (codec) {
        case ScreenCodec::Auto: return "Automatic";
        case ScreenCodec::Tiles: return "Changed tiles";
        case ScreenCodec::H264: return "H.264 video";
    }
    return "Automatic";
}

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
    // Tried in order, the same way. Named per platform because unlike the
    // encoder above -- which only ever runs on a desktop host -- every one of
    // these might be the one running this decoder: a Switch, an Android
    // device, or a desktop companion, each with a different hardware decode
    // path FFmpeg reaches under a different name. See VideoDecoder::open()
    // and hw_device_for()/decoder_name_for() in video_decoder.cpp for what
    // each one actually does once it opens.
#if defined(__SWITCH__)
    std::vector<std::string> backends{"h264_nvtegra", "h264"};
#elif defined(__ANDROID__)
    std::vector<std::string> backends{"h264_mediacodec", "h264"};
#else
    std::vector<std::string> backends{"h264_vaapi", "h264"};
#endif
};

}  // namespace ds::video
