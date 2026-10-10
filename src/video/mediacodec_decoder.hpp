#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "video/video_codec.hpp"

namespace ds::video {

// H.264 through Android's own decoder, with no FFmpeg anywhere in it.
//
// The Android counterpart of VideoToolboxDecoder, and here for the same
// reason. FFmpeg's h264_mediacodec is a wrapper around MediaCodec reached
// through JNI, which means cross-compiling FFmpeg with the NDK before an
// Android build has any H.264 at all -- a step that needs a Unix shell and
// make, and that nobody building this on Windows could get through. The NDK
// has carried MediaCodec as a plain C API (libmediandk) since Android 5.0,
// which is this app's minSdk, so the wrapper buys nothing a few hundred
// lines here do not.
//
// Frames come back as bytes in whatever layout the device's decoder writes --
// NV12 almost everywhere, I420 from a few software decoders -- and go to the
// renderer as planes, which converts them while drawing. See DecodedLayout.
//
// Off Android every one of these is a stub and available() is false, so
// callers need no conditionals.
class MediaCodecDecoder {
public:
    MediaCodecDecoder();
    ~MediaCodecDecoder();

    MediaCodecDecoder(const MediaCodecDecoder&) = delete;
    MediaCodecDecoder& operator=(const MediaCodecDecoder&) = delete;

    // Whether this build has it. False everywhere but Android.
    static bool available();

    // Nothing is created here. MediaCodec is configured with the stream's
    // parameter sets, and those arrive inside the first keyframe -- so
    // opening is really just being ready to be fed.
    bool open(std::string* error);
    void close();
    bool is_open() const;

    // The decoder's own name once one has been made -- "c2.qti.avc.decoder",
    // say -- which says whether it is the hardware. Empty until then.
    const std::string& codec_name() const;

    // Feeds one Annex B access unit, exactly as it comes off the wire.
    //
    // `out` is left invalid when there is nothing to show yet, which is not a
    // failure: everything before the first keyframe is dropped, and a decoder
    // still working on a frame hands it back on the next call instead. Returns
    // false only for a real error.
    bool decode(const uint8_t* data, std::size_t size, DecodedFrame& out, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ds::video
