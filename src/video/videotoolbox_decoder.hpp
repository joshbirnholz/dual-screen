#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "video/video_codec.hpp"

namespace ds::video {

// H.264 through Apple's own decoder, with no FFmpeg anywhere in it.
//
// VideoToolbox is what decodes every frame on an Apple platform already:
// FFmpeg's h264 decoder with AV_HWDEVICE_TYPE_VIDEOTOOLBOX attached is a
// wrapper around this. What FFmpeg supplies on top is bitstream plumbing --
// splitting NAL units, lifting the parameter sets out, re-framing the rest --
// and that is h264_bitstream.hpp, which is a few hundred lines of arithmetic
// over a byte array and is tested on Linux.
//
// Removing the wrapper is worth two things:
//
//   * iOS and tvOS get H.264 at all. There is no Homebrew for a phone, so
//     FFmpeg there means cross-compiling it, and without H.264 a Wii U's
//     GamePad screen costs about seventeen times the bandwidth as tiles.
//   * The copy goes away, eventually. VideoToolbox hands back a CVPixelBuffer
//     that the GPU can already see; FFmpeg's path pulls it into system memory
//     with av_hwframe_transfer_data and this app then copies the planes again.
//     Handing that buffer straight to a texture is the real win, and it is a
//     second change rather than this one -- see apple/README.md.
//
// Off an Apple platform every one of these is a stub and available() is false,
// so callers need no conditionals.
class VideoToolboxDecoder {
public:
    VideoToolboxDecoder();
    ~VideoToolboxDecoder();

    VideoToolboxDecoder(const VideoToolboxDecoder&) = delete;
    VideoToolboxDecoder& operator=(const VideoToolboxDecoder&) = delete;

    // Whether this build has it. False everywhere but Apple.
    static bool available();

    // Nothing is created here. A decode session is built from the parameter
    // sets, and those arrive inside the first keyframe -- so opening is
    // really just being ready to be fed.
    bool open(std::string* error);
    void close();
    bool is_open() const;

    // Feeds one Annex B access unit, exactly as it comes off the wire.
    //
    // `out` is left invalid when there is nothing to show yet, which is not a
    // failure: everything before the first keyframe has no session to be
    // decoded against and is dropped. Returns false only for a real error.
    bool decode(const uint8_t* data, std::size_t size, DecodedFrame& out, std::string* error);

    // Whether a session has been built, which happens at the first keyframe.
    // A caller that has been fed nothing but inter frames since connecting
    // should ask the host for a keyframe rather than wait.
    bool needs_keyframe() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ds::video
