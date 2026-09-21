#pragma once

#include <memory>
#include <string>

#include "video/video_codec.hpp"
#include "video/videotoolbox_decoder.hpp"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace ds::video {

// The companion's half. One packet in, one frame out.
//
// The platform differences all live in DecoderConfig::backends and the hardware
// device the name implies: NVTEGRA on a Switch, MediaCodec on Android, VAAPI on
// a desktop with a GPU, and plain "h264" on the CPU when none of those are
// there. Every one of them is the same FFmpeg calls underneath, which is the
// finding that made this worth building rather than adopting a streaming stack.
class VideoDecoder {
public:
    ~VideoDecoder();

    bool open(const DecoderConfig& config, std::string* error);
    void close();
    bool is_open() const;
    const std::string& backend() const { return backend_; }

    // Feeds one packet and takes whatever comes out. `out` is left invalid when
    // the decoder has nothing yet, which is not a failure: a stream that has not
    // reached its first keyframe has nothing to show.
    bool decode(const uint8_t* data, std::size_t size, DecodedFrame& out, std::string* error);

    // Whether frames may come back as planar YUV rather than converted to
    // RGB -- see DecodedLayout. Off by default, so a caller that only knows
    // how to read pixels keeps getting pixels; on for a caller whose renderer
    // can convert while drawing, which is where the win is.
    //
    // A frame in some other format still comes back converted whatever this
    // says: this asks for the planes when they are there, it does not promise
    // them.
    void set_planar_output_wanted(bool wanted) { want_planar_ = wanted; }

private:
    // The FFmpeg half. Private because open()/decode()/close() above choose
    // between this and Apple's own decoder, which on iOS and tvOS is the only
    // one there is -- those builds have no FFmpeg at all.
    bool open_ffmpeg(const DecoderConfig& config, std::string* error);
    bool decode_ffmpeg(const uint8_t* data, std::size_t size, DecodedFrame& out,
                       std::string* error);
    void close_ffmpeg();

    bool open_backend(const std::string& name, std::string* error);
    bool to_rgb(AVFrame* frame, DecodedFrame& out, std::string* error);

    // Set only where VideoToolbox is both present and asked for.
    std::unique_ptr<VideoToolboxDecoder> apple_;

    AVCodecContext* context_ = nullptr;
    bool want_planar_ = false;
    std::string backend_;

    // The rest of the FFmpeg half's state, which a build without FFmpeg still
    // carries and never touches. [[maybe_unused]] rather than #if, because the
    // macro is a compile definition and a header that changed shape with it
    // would quietly hand two translation units two different classes.
    //
    // A few bytes of nothing in a decoder that is holding video frames is not
    // worth that, and the attribute is the honest description anyway: these
    // are used in some builds and not others.
    [[maybe_unused]] AVFrame* frame_ = nullptr;
    [[maybe_unused]] AVFrame* software_frame_ = nullptr;
    [[maybe_unused]] AVPacket* packet_ = nullptr;
    [[maybe_unused]] SwsContext* scaler_ = nullptr;
    [[maybe_unused]] AVBufferRef* hw_device_ = nullptr;
    [[maybe_unused]] int scaler_width_ = 0;
    [[maybe_unused]] int scaler_height_ = 0;
    [[maybe_unused]] int scaler_format_ = -1;
};

}  // namespace ds::video
