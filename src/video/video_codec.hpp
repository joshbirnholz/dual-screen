#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
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

// Whether FFmpeg is compiled into this build. The Switch and Android
// toolchains do not have it wired up yet, and everything DS and 3DS do works
// without it, so it stays optional until those are ready.
//
// This is a fact about the build, not an answer to a question anybody asks.
// The two questions people actually have are below, and they no longer have
// the same answer: iOS and tvOS ship with no FFmpeg at all and decode through
// VideoToolbox instead.
bool have_ffmpeg();
// Whether that FFmpeg has any of the H.264 encoders EncoderConfig tries.
bool have_h264_encoder();

// Whether this build can turn a screen into H.264. FFmpeg's business; there
// is no second encoder on any platform.
bool can_encode();

// Whether this build can turn H.264 back into a picture. FFmpeg or
// VideoToolbox -- a device build has only the second, and telling a host
// otherwise is telling it not to send any video.
bool can_decode();

// Keeps FFmpeg's own chatter out of this app's log; only errors survive.
// Called for you when an encoder or decoder opens.
void quieten_ffmpeg();

struct EncodedFrame {
    std::vector<uint8_t> bytes;
    // Whether this frame stands alone. A device joining mid-stream can draw
    // nothing until one of these arrives.
    bool keyframe = false;
    // Which submitted frame these bytes are. A hardware encoder is allowed to
    // hand back the packet for an earlier submission than the one that just
    // went in -- that is the whole point of letting it pipeline -- so the
    // caller cannot assume bytes coming out now describe the picture it just
    // put in. -1 when nothing came out. See VideoEncoder::encode().
    int64_t pts = -1;

    bool empty() const { return bytes.empty(); }
};

// How a decoded frame's bytes are arranged.
//
// A decoder hands frames back as planar YUV -- that is what H.264 is -- and
// turning them into RGB is a whole pass over every pixel. On a desktop that is
// cheap next to everything else; on a handheld it is the single most expensive
// thing in the pipeline, and it is work a GPU does for nothing while drawing.
// Measured on a Switch at 1280x720: 22ms a frame with the conversion done on
// the CPU, against a 16ms frame period.
//
// So a frame comes back in whichever of these its decoder produced, and the
// conversion happens in the fragment shader instead -- see ChannelOrder in
// render/texture.hpp, whose values these mirror.
enum class DecodedLayout {
    // Already converted. What a decoder whose output this app has no planar
    // path for falls back to, and what every caller understood before the
    // other two existed.
    Xrgb8888,
    // One full-size plane of luma, then one half-size plane of chroma with U
    // and V interleaved. What every hardware decoder here produces.
    Nv12,
    // One full-size plane of luma and two half-size planes of chroma. What
    // FFmpeg's software H.264 decoder produces.
    Yuv420p,
};

struct DecodedFrame {
    DecodedLayout layout = DecodedLayout::Xrgb8888;
    // Set only for Xrgb8888: the whole picture, four bytes a pixel.
    std::vector<uint32_t> pixels;
    // Set only for the two planar layouts. plane[0] is luma at the full size;
    // plane[1] is either the interleaved chroma (Nv12) or U (Yuv420p);
    // plane[2] is V, and only for Yuv420p. `stride` is each plane's own row
    // length in bytes, which a decoder is free to make wider than the picture.
    std::vector<uint8_t> plane[3];
    int stride[3] = {0, 0, 0};
    int width = 0;
    int height = 0;
    // How long this frame took to copy out of the decoder's own memory, in
    // microseconds, for the decoders that have to. Zero where the question
    // does not arise.
    //
    // Separated from decoding because on an Apple device they are not the
    // same cost and not fixed the same way: a hardware decoder hands back a
    // buffer the GPU already holds, and reading it on the CPU is the
    // expensive part. One number for both cannot tell you which you are
    // paying for -- see adapt_to_what_this_device_can_decode().
    int readback_micros = 0;

    // And how much of that was spent taking the lock rather than moving bytes.
    //
    // A lock on a decoder's own buffer is where the CPU waits for a decoder
    // that has not finished with it, which is a wait; the rest is memory being
    // read, which is bandwidth. One number for both says 23ms a frame and
    // cannot say whether the answer is to stop copying or to stop waiting.
    int lock_micros = 0;

    // The decoder's own picture, left where it is, instead of `plane`: an
    // Nv12 frame whose bytes were never copied anywhere. On an iPhone or an
    // Apple TV this is a retained CVPixelBufferRef, which the renderer turns
    // into two textures without touching a byte -- see
    // Texture::adopt_native_nv12(). Released when the last holder lets go.
    //
    // Why it exists: an Apple TV read those bytes back at about 110MB/s, which
    // is 27ms for one 1080p frame and half of everything it spent per
    // picture. Memory a hardware decoder writes is not memory a CPU reads
    // quickly, and the only fast way to read it is not to.
    //
    // Anything that does want bytes gets them from copy_native_planes().
    std::shared_ptr<void> native;

    bool valid() const {
        if (width <= 0 || height <= 0) return false;
        if (layout == DecodedLayout::Xrgb8888) return !pixels.empty();
        return !plane[0].empty() || native != nullptr;
    }
};

// Whether a decoder may hand frames back as DecodedFrame::native. True where
// the renderer can draw them (iOS and tvOS) until it says otherwise: a
// renderer that fails to make textures from them turns this off, and every
// frame after that is copied out the old way instead. False everywhere else.
bool native_frames_usable();
void set_native_frames_usable(bool usable);

// The planes of a native frame, copied into `out` the old way -- for anything
// that wants bytes rather than a texture. False, with `out` untouched, for a
// frame that has no native picture or on a build that has none at all.
bool copy_native_planes(const std::shared_ptr<void>& native, DecodedFrame& out);

// The byte order of the pixels VideoEncoder::encode() is handed. Most screens
// come from a libretro core's software framebuffer, XRGB8888 -- BGRA in
// memory on a little-endian machine. A screen read back off the GPU (a
// hardware-rendered core, or Cemu across the external frame link) comes back
// R,G,B,A instead: see ChannelOrder in core/screen_source.hpp for why that is
// tagged rather than swizzled back to match on the way out.
enum class PixelOrder { Bgra, Rgba };

// How long the companion may hold a decoded frame back before showing it, so
// that an uneven link arrives evenly.
//
// A setting because the measurements say there is no single right answer, not
// because it was easier than deciding. Steam Deck to Mac over wifi, same room:
// half of all frames land within 17ms of the quietest and one in twenty lands
// 72ms behind it. Hold for the body of that and the tail frames find nothing
// waiting, so the picture stops for a beat about once a second. Hold for the
// tail and every single frame pays 72ms for the one in twenty. The link
// decides which of those hurts more, and the link is different in every house.
//
// Moonlight has the same fork in the road and answers it the same way, with a
// frame-pacing toggle. Its default is the responsive end.
enum class VideoHold {
    // Pace at one frame period and show whatever has arrived. The lowest
    // latency this can run at, and the one that judders when the link does.
    Responsive,
    // Follow the measured spread, and buy a little more cover whenever the
    // picture actually runs dry. What this app did before the setting existed.
    Balanced,
    // The same, but allowed to buy twice as much cover before it stops. For a
    // link whose tail is bad enough that Balanced still stutters.
    Smooth,
};

inline VideoHold next_video_hold(VideoHold hold) {
    switch (hold) {
        case VideoHold::Responsive: return VideoHold::Balanced;
        case VideoHold::Balanced: return VideoHold::Smooth;
        case VideoHold::Smooth: break;
    }
    return VideoHold::Responsive;
}

inline const char* video_hold_key(VideoHold hold) {
    switch (hold) {
        case VideoHold::Responsive: return "responsive";
        case VideoHold::Balanced: return "balanced";
        case VideoHold::Smooth: break;
    }
    return "smooth";
}

inline VideoHold video_hold_from_key(const std::string& key, VideoHold fallback) {
    if (key == video_hold_key(VideoHold::Responsive)) return VideoHold::Responsive;
    if (key == video_hold_key(VideoHold::Balanced)) return VideoHold::Balanced;
    if (key == video_hold_key(VideoHold::Smooth)) return VideoHold::Smooth;
    return fallback;
}

inline const char* to_string(VideoHold hold) {
    switch (hold) {
        case VideoHold::Responsive: return "Lowest latency";
        case VideoHold::Balanced: return "Balanced";
        case VideoHold::Smooth: break;
    }
    return "Smoothest";
}

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

// The name to hand FFmpeg for a backend label, which is not always the label.
//
// Three of the labels this app uses name a *hardware device* to attach to the
// ordinary h264 decoder rather than a decoder of their own: FFmpeg registers
// no h264_vaapi, h264_nvtegra or h264_videotoolbox decoder. MediaCodec and
// CUVID are the other way round -- those really are separate decoders under
// exactly those names.
//
// Apple's is the one that punishes a wrong guess quietly, because
// "h264_videotoolbox" *does* exist -- as an encoder. Asking for it by name on
// the decode side finds nothing and reads, from the outside, like a build
// with no hardware decoding in it.
//
// Here rather than beside the FFmpeg calls so it can be checked without
// FFmpeg: it is a fact about naming, and the cost of getting it wrong has
// already been paid once.
inline std::string decoder_name_for(const std::string& backend) {
    for (const char* suffix : {"_vaapi", "_nvtegra", "_videotoolbox"}) {
        const std::size_t length = std::char_traits<char>::length(suffix);
        if (backend.size() > length &&
            backend.compare(backend.size() - length, length, suffix) == 0) {
            return backend.substr(0, backend.size() - length);
        }
    }
    return backend;
}

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
#elif defined(__APPLE__)
    // VideoToolbox is every Apple platform's decoder -- the same API on a
    // Mac, an iPhone and an Apple TV, and hardware on all of them. It behaves
    // like VAAPI rather than like MediaCodec: FFmpeg registers no decoder of
    // that name, only a device context that turns the plain h264 decoder into
    // a hardware one. See hw_device_for() and decoder_name_for().
    std::vector<std::string> backends{"h264_videotoolbox", "h264"};
#else
    std::vector<std::string> backends{"h264_vaapi", "h264"};
#endif
};

}  // namespace ds::video
