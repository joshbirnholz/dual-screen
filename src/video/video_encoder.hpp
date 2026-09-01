#pragma once

#include <string>

#include "video/video_codec.hpp"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVBufferRef;

namespace ds::video {

// One screen's frames, compressed.
//
// Configured for no delay at all: no B-frames, no lookahead, and one packet out
// for every frame in. An encoder that buffers is an encoder that is behind, and
// on this path a frame that arrives late is worse than one that arrives large.
class VideoEncoder {
public:
    ~VideoEncoder();

    bool open(const EncoderConfig& config, std::string* error);
    void close();
    bool is_open() const { return context_ != nullptr; }
    // Which of the configured backends actually opened. Worth logging: whether
    // the GPU took this is the difference between a few percent of a frame and
    // most of one.
    const std::string& backend() const { return backend_; }
    // What open() was last configured for. The caller's own job to notice
    // this no longer matches the screen it is encoding and reopen -- see
    // HostSession::try_encode_video, which is the only caller.
    int width() const { return width_; }
    int height() const { return height_; }

    // XRGB8888 in, at `stride_pixels` per row. `out` is emptied first, so an
    // encoder that produced nothing this frame is `out.empty()` rather than
    // last frame's bytes again.
    bool encode(const uint32_t* pixels, int stride_pixels, EncodedFrame& out, std::string* error);

    // The next frame stands alone, whatever the encoder had planned. A device
    // that has just joined can draw nothing until one arrives, and waiting for
    // the next scheduled one is a second of black.
    void request_keyframe() { force_keyframe_ = true; }

private:
    bool open_backend(const std::string& name, const EncoderConfig& config, std::string* error);

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* scaler_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVFrame* hw_frame_ = nullptr;
    std::string backend_;
    int width_ = 0;
    int height_ = 0;
    int64_t next_pts_ = 0;
    bool force_keyframe_ = true;
};

}  // namespace ds::video
