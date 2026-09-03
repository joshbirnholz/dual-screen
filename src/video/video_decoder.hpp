#pragma once

#include <string>

#include "video/video_codec.hpp"

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
    bool is_open() const { return context_ != nullptr; }
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
    bool open_backend(const std::string& name, std::string* error);
    bool to_rgb(AVFrame* frame, DecodedFrame& out, std::string* error);

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* software_frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* scaler_ = nullptr;
    bool want_planar_ = false;
    AVBufferRef* hw_device_ = nullptr;
    std::string backend_;
    int scaler_width_ = 0;
    int scaler_height_ = 0;
    int scaler_format_ = -1;
};

}  // namespace ds::video
