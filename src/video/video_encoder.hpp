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
    // What open() was actually given, before any scaling down to width/height
    // above -- see EncoderConfig::source_width/source_height. The caller's
    // job to notice this no longer matches either, same as width()/height().
    // Follows a source whose size changed without disturbing the H.264
    // stream coming out. What goes in is resampled to the same output size as
    // before, so the far end's decoder sees one continuous stream rather than
    // a new one -- see the definition for why that matters so much.
    bool set_source_size(int width, int height, std::string* error);

    int source_width() const { return source_width_; }
    int source_height() const { return source_height_; }
    // The byte order open() was told the pixels come in, which the same
    // caller has to reopen for if it changes -- see EncoderConfig.
    PixelOrder pixel_order() const { return pixel_order_; }

    // XRGB8888 in, at `stride_pixels` per row. `out` is emptied first, so an
    // encoder that produced nothing this frame is `out.empty()` rather than
    // last frame's bytes again.
    bool encode(const uint32_t* pixels, int stride_pixels, EncodedFrame& out, std::string* error);

    // Where encode()'s time actually goes, accumulated since the last call to
    // take_cost(), which resets it. Split three ways because the fix differs
    // completely per stage: the colour conversion is CPU work that could move
    // to another thread or a cheaper scaler, the upload is this machine's bus
    // and the GPU's willingness to take a surface while a game is using it,
    // and the submit is what is left once the encoder no longer waits for the
    // packet. Worth knowing before assuming which one is worth attacking.
    struct EncodeCost {
        double scale_ms = 0.0;
        double upload_ms = 0.0;
        double submit_ms = 0.0;
        unsigned frames = 0;
    };
    EncodeCost take_cost() {
        const EncodeCost taken = cost_;
        cost_ = EncodeCost();
        return taken;
    }

    // The pts encode() gave the frame it most recently took in. The encoder
    // pipelines, so the packet that same call handed back is usually an
    // earlier one -- pairing this with EncodedFrame::pts is how a caller
    // works out which of its own frames the bytes it just received describe.
    int64_t last_submitted_pts() const { return last_submitted_pts_; }

    // The next frame stands alone, whatever the encoder had planned. A device
    // that has just joined can draw nothing until one arrives, and waiting for
    // the next scheduled one is a second of black.
    void request_keyframe() { force_keyframe_ = true; }

private:
    bool open_backend(const std::string& name, const EncoderConfig& config, std::string* error);
    // (Re)builds the colour conversion and resampling step for a source of
    // this size. The encoder itself is untouched.
    bool rebuild_scaler(int source_width, int source_height, std::string* error);

    AVCodecContext* context_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* scaler_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVFrame* hw_frame_ = nullptr;
    std::string backend_;
    int width_ = 0;
    int height_ = 0;
    // What rebuild_scaler() needs to make one without the config it came
    // from: the two ends of the conversion it performs.
    // Which of the two orders the caller hands pixels in, kept as this app's
    // own enum as well as the FFmpeg format below, because rgb_to_nv12() reads
    // it directly.
    PixelOrder pixel_order_ = PixelOrder::Bgra;
    int source_format_ = 0;
    int software_format_ = 0;
    int source_width_ = 0;
    int source_height_ = 0;
    int64_t next_pts_ = 0;
    bool force_keyframe_ = true;
    int64_t last_submitted_pts_ = -1;
    EncodeCost cost_;
};

}  // namespace ds::video
