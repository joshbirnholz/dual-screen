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
// Set per system being played -- Settings has one of these each for a DS, a
// 3DS and a Wii U -- and taken exactly as chosen, with no automatic fallback
// to the other if this one turns out not to work. That is deliberate: a
// silent substitution is indistinguishable from the format actually working,
// which is the one thing this exists to let a player tell apart. If it
// cannot be honoured -- the far end cannot decode, this build has no encoder,
// a screen an odd number of pixels wide or tall meets 4:2:0 -- the session
// says so instead of quietly sending the other one. See
// HostSession::take_video_error().
enum class ScreenCodec {
    Tiles,  // changed tiles
    H264,   // video
};

// The settings screen only ever toggles between the two.
inline ScreenCodec next_screen_codec(ScreenCodec codec) {
    switch (codec) {
        case ScreenCodec::Tiles: return ScreenCodec::H264;
        case ScreenCodec::H264: return ScreenCodec::Tiles;
    }
    return ScreenCodec::Tiles;
}

// What settings.json holds, and what --video-codec accepts.
inline const char* screen_codec_key(ScreenCodec codec) {
    switch (codec) {
        case ScreenCodec::Tiles: return "tiles";
        case ScreenCodec::H264: return "h264";
    }
    return "tiles";
}

// Anything unrecognised falls back to the caller's own current value rather
// than to one of these two picked for them: a typo in a config file should
// leave the setting where it already was rather than silently choosing.
inline ScreenCodec screen_codec_from_key(const std::string& key, ScreenCodec fallback) {
    if (key == screen_codec_key(ScreenCodec::Tiles)) return ScreenCodec::Tiles;
    if (key == screen_codec_key(ScreenCodec::H264)) return ScreenCodec::H264;
    return fallback;
}

inline const char* to_string(ScreenCodec codec) {
    switch (codec) {
        case ScreenCodec::Tiles: return "Tiles";
        case ScreenCodec::H264: return "H264";
    }
    return "Tiles";
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

// The byte order of the pixels VideoEncoder::encode() is handed. Most screens
// come from a libretro core's software framebuffer, XRGB8888 -- BGRA in
// memory on a little-endian machine. A screen read back off the GPU (a
// hardware-rendered core, or Cemu across the external frame link) comes back
// R,G,B,A instead: see ChannelOrder in core/screen_source.hpp for why that is
// tagged rather than swizzled back to match on the way out.
enum class PixelOrder { Bgra, Rgba };

struct EncoderConfig {
    // What actually gets encoded and sent. May be smaller than the source
    // pixels handed to encode() -- see source_width/source_height below --
    // when the far end's own display cannot show the source at full size
    // anyway: fewer pixels to compress, send and decode, and a smaller
    // keyframe to ever have to get there in one piece.
    int width = 0;
    int height = 0;
    // The size of the pixel buffer encode() is actually given. Zero means
    // "the same as width/height": nothing to scale, which is every caller
    // except HostSession::try_encode_video. When the two differ, open()
    // sets the scaler up to resize on the way to the encoder rather than
    // just convert pixel format in place.
    int source_width = 0;
    int source_height = 0;
    int fps = 60;
    // What the link is being asked to carry. A LAN has room for far more than
    // this, but every bit spent is latency when a burst meets a slow radio.
    int bitrate_kbps = 4000;
    // Tried in order; the first that opens wins. Hardware first, always: the
    // host is running an emulator with the rest of the frame.
    std::vector<std::string> backends{"h264_vaapi", "libx264"};
    PixelOrder pixel_order = PixelOrder::Bgra;
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
