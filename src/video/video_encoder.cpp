#include "video/video_encoder.hpp"

#include <cstdarg>
#include <cstdio>

#include "util/log.hpp"

#if DS_HAVE_VIDEO_CODEC

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace ds::video {
namespace {

std::string av_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// Which hardware device a backend needs, or NONE for one that runs on the CPU.
// The name carries it: this is the whole of the platform difference between
// encoding on a Deck and encoding anywhere else.
AVHWDeviceType hw_device_for(const std::string& backend) {
    if (backend.find("_vaapi") != std::string::npos) return AV_HWDEVICE_TYPE_VAAPI;
    if (backend.find("_nvenc") != std::string::npos) return AV_HWDEVICE_TYPE_CUDA;
    if (backend.find("_qsv") != std::string::npos) return AV_HWDEVICE_TYPE_QSV;
    return AV_HWDEVICE_TYPE_NONE;
}

AVPixelFormat hw_pixel_format(AVHWDeviceType type) {
    switch (type) {
        case AV_HWDEVICE_TYPE_VAAPI: return AV_PIX_FMT_VAAPI;
        case AV_HWDEVICE_TYPE_CUDA: return AV_PIX_FMT_CUDA;
        case AV_HWDEVICE_TYPE_QSV: return AV_PIX_FMT_QSV;
        default: return AV_PIX_FMT_NONE;
    }
}

// FFmpeg's default log handler writes only to stderr, which Game Mode has no
// terminal for -- so on its own, a VAAPI failure detailed enough to actually
// explain itself (which entrypoint or rate-control mode a driver rejected,
// for instance) never reaches anyone. This forwards it into the app's own
// log instead, at the level FFmpeg itself already picked.
void ffmpeg_log_callback(void* /*avcl*/, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level()) return;
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    std::string message(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    if (message.empty()) return;
    if (level <= AV_LOG_ERROR) {
        log_error("ffmpeg: " + message);
    } else if (level <= AV_LOG_WARNING) {
        log_warn("ffmpeg: " + message);
    } else {
        log_info("ffmpeg: " + message);
    }
}

}  // namespace

bool available() { return true; }

// FFmpeg logs to stderr by default, and an encoder's end-of-stream statistics
// are pages of it. This app's own log is the thing anybody reads when something
// has gone wrong, so only what went wrong goes in it.
void quieten_ffmpeg() {
    static bool done = false;
    if (done) return;
    done = true;
    av_log_set_level(AV_LOG_ERROR);
    av_log_set_callback(ffmpeg_log_callback);
}

VideoEncoder::~VideoEncoder() { close(); }

void VideoEncoder::close() {
    if (scaler_ != nullptr) {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }
    if (hw_frame_ != nullptr) av_frame_free(&hw_frame_);
    if (frame_ != nullptr) av_frame_free(&frame_);
    if (packet_ != nullptr) av_packet_free(&packet_);
    if (context_ != nullptr) avcodec_free_context(&context_);
    if (hw_device_ != nullptr) av_buffer_unref(&hw_device_);
    backend_.clear();
    next_pts_ = 0;
    force_keyframe_ = true;
}

bool VideoEncoder::open(const EncoderConfig& config, std::string* error) {
    quieten_ffmpeg();
    close();
    if (config.width <= 0 || config.height <= 0) {
        if (error != nullptr) *error = "a screen with no size cannot be encoded";
        return false;
    }
    // H.264 macroblocks are 16 wide; an odd size is not encodable at all in
    // 4:2:0. Caught here rather than as an FFmpeg error nobody can read.
    if ((config.width % 2) != 0 || (config.height % 2) != 0) {
        if (error != nullptr) {
            *error = "a screen has to be an even number of pixels each way to be encoded";
        }
        return false;
    }

    std::string last;
    for (const std::string& name : config.backends) {
        std::string reason;
        if (open_backend(name, config, &reason)) {
            log_info("video encoder: " + backend_ + " at " + std::to_string(config.width) + "x" +
                     std::to_string(config.height) + ", " + std::to_string(config.bitrate_kbps) +
                     " kbit/s");
            return true;
        }
        log_info("video encoder: " + name + " would not open: " + reason);
        last = name + ": " + reason;
        close();
    }
    if (error != nullptr) {
        *error = config.backends.empty() ? "no video encoder was asked for"
                                         : "no video encoder would open (" + last + ")";
    }
    return false;
}

bool VideoEncoder::open_backend(const std::string& name,
                                const EncoderConfig& config,
                                std::string* error) {
    const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
    if (codec == nullptr) {
        if (error != nullptr) *error = "this build has no such encoder";
        return false;
    }

    const AVHWDeviceType hw_type = hw_device_for(name);
    if (hw_type != AV_HWDEVICE_TYPE_NONE) {
        const int rc = av_hwdevice_ctx_create(&hw_device_, hw_type, nullptr, nullptr, 0);
        if (rc < 0) {
            if (error != nullptr) *error = "no such device here: " + av_error(rc);
            return false;
        }
    }

    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
        if (error != nullptr) *error = "out of memory";
        return false;
    }
    context_->width = config.width;
    context_->height = config.height;
    context_->time_base = AVRational{1, config.fps};
    context_->framerate = AVRational{config.fps, 1};
    context_->bit_rate = static_cast<int64_t>(config.bitrate_kbps) * 1000;
    // Without this, FFmpeg's VAAPI rate-control picker (vaapi_encode_init_
    // rate_control) only chooses CBR when rc_max_rate == bit_rate; left at 0,
    // it tries AVBR and VBR first and takes whichever the driver claims to
    // support, and on the Steam Deck's own AMD driver that is a real
    // difference -- CBR is confirmed working there
    // (github.com/ValveSoftware/SteamOS/issues/1609 catches the same driver
    // getting a *different* rate-control mode, CQP, wrong), while a
    // silently-broken VBR/AVBR path is exactly the shape of the "invalid
    // VASurfaceID" failure this is fixing. Harmless for the software and
    // other hardware backends too: a live stream has no use for a max rate
    // above its target rate anyway.
    context_->rc_max_rate = context_->bit_rate;
    // A keyframe costs several times what a delta does, and on a LAN nothing is
    // lost that needs one on a schedule -- a device that joins asks for one.
    // Two seconds keeps recovery bounded without paying for it constantly.
    context_->gop_size = config.fps * 2;
    // The whole point. A B-frame cannot be sent until the frame after it has
    // been encoded, which is a frame of latency bought for bytes this link does
    // not need to save.
    context_->max_b_frames = 0;
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // A screen is not a camera: there is no broadcast-video convention this
    // stream needs to honour, and left unset, encoding it into YUV's
    // conventional limited range (16-235/16-240) throws away the outer ~14%
    // of every channel before compression even starts -- fewer discrete
    // steps across a gradient than the source actually has, which reads as
    // banding whether or not the far end interprets the range correctly.
    // Signalled in the bitstream (VUI) so a decoder on another device --
    // including a hardware one this app did not write -- reads it the same
    // way rather than guessing at "unspecified".
    context_->color_range = AVCOL_RANGE_JPEG;

    // Hardware encoders -- VAAPI included -- work in NV12 (one luma plane,
    // one interleaved chroma plane), not YUV420P's three separate planes;
    // AMD's own VAAPI driver enforces this strictly, silently accepting a
    // YUV420P surface all the way through allocation and upload and only
    // rejecting it once the actual hardware encode runs, which is what the
    // "invalid VASurfaceID" failure at vaEndPicture actually was. Software
    // encoding has no such constraint, and libx264 wants YUV420P.
    const AVPixelFormat software_format =
        (hw_type != AV_HWDEVICE_TYPE_NONE) ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    if (hw_type != AV_HWDEVICE_TYPE_NONE) {
        context_->pix_fmt = hw_pixel_format(hw_type);
        AVBufferRef* frames = av_hwframe_ctx_alloc(hw_device_);
        if (frames == nullptr) {
            if (error != nullptr) *error = "could not make a frame pool on the device";
            return false;
        }
        auto* frames_ctx = reinterpret_cast<AVHWFramesContext*>(frames->data);
        frames_ctx->format = context_->pix_fmt;
        frames_ctx->sw_format = software_format;
        frames_ctx->width = config.width;
        frames_ctx->height = config.height;
        frames_ctx->initial_pool_size = 4;
        const int rc = av_hwframe_ctx_init(frames);
        if (rc < 0) {
            av_buffer_unref(&frames);
            if (error != nullptr) *error = "the device refused a frame pool: " + av_error(rc);
            return false;
        }
        context_->hw_frames_ctx = frames;
    } else {
        context_->pix_fmt = software_format;
        // Software encoders are the ones that would otherwise buffer for
        // quality. Both of these are the difference between "one packet per
        // frame" and "a packet eventually".
        av_opt_set(context_->priv_data, "preset", "veryfast", 0);
        av_opt_set(context_->priv_data, "tune", "zerolatency", 0);
    }

    const int rc = avcodec_open2(context_, codec, nullptr);
    if (rc < 0) {
        if (error != nullptr) *error = av_error(rc);
        return false;
    }

    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (frame_ == nullptr || packet_ == nullptr) {
        if (error != nullptr) *error = "out of memory";
        return false;
    }
    frame_->format = software_format;
    frame_->width = config.width;
    frame_->height = config.height;
    if (av_frame_get_buffer(frame_, 0) < 0) {
        if (error != nullptr) *error = "out of memory for a frame";
        return false;
    }

    if (hw_type != AV_HWDEVICE_TYPE_NONE) {
        // Not filled from the pool here -- encode() does that fresh for every
        // frame. VAAPI encode is asynchronous, so the driver can still be
        // working on (or holding as a reference) the surface behind a frame
        // submitted a call or two ago; a single surface reused for every
        // frame gets overwritten out from under it, which is what
        // initial_pool_size above is there to prevent.
        hw_frame_ = av_frame_alloc();
        if (hw_frame_ == nullptr) {
            if (error != nullptr) *error = "out of memory";
            return false;
        }
    }

    // XRGB8888 as this app stores it is BGRA in memory on a little-endian
    // machine, which is what AV_PIX_FMT_BGRA means -- but a GPU-read-back
    // screen (see PixelOrder's own comment) arrives R,G,B,A instead, and
    // feeding that in as BGRA swaps red and blue across the whole picture.
    const AVPixelFormat source_format =
        (config.pixel_order == PixelOrder::Rgba) ? AV_PIX_FMT_RGBA : AV_PIX_FMT_BGRA;
    scaler_ = sws_getContext(config.width, config.height, source_format, config.width,
                             config.height, software_format, SWS_BILINEAR, nullptr, nullptr,
                             nullptr);
    if (scaler_ == nullptr) {
        if (error != nullptr) *error = "could not set up colour conversion";
        return false;
    }
    // Full range out of this conversion too, matching context_->color_range
    // above -- the two have to agree, since this is what actually produces
    // the YUV bytes the encoder reads. Only dstRange changes: the source is
    // RGB, which has no range of its own to set, and BT.601's coefficients
    // are still the right ones for a plain colour conversion with no
    // particular colour space asked for.
    sws_setColorspaceDetails(scaler_, sws_getCoefficients(SWS_CS_ITU601), 0,
                             sws_getCoefficients(SWS_CS_ITU601), 1, 0, 1 << 16, 1 << 16);

    backend_ = name;
    width_ = config.width;
    height_ = config.height;
    return true;
}

bool VideoEncoder::encode(const uint32_t* pixels,
                          int stride_pixels,
                          EncodedFrame& out,
                          std::string* error) {
    out.bytes.clear();
    out.keyframe = false;
    if (context_ == nullptr || pixels == nullptr) {
        if (error != nullptr) *error = "the encoder is not open";
        return false;
    }
    if (av_frame_make_writable(frame_) < 0) {
        if (error != nullptr) *error = "the frame could not be written to";
        return false;
    }

    const auto* source = reinterpret_cast<const uint8_t*>(pixels);
    const int source_stride = stride_pixels * 4;
    sws_scale(scaler_, &source, &source_stride, 0, height_, frame_->data, frame_->linesize);
    frame_->pts = next_pts_++;
    if (force_keyframe_) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        force_keyframe_ = false;
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    AVFrame* submitted = frame_;
    if (hw_frame_ != nullptr) {
        // A fresh surface from the pool every frame -- see the comment in
        // open_backend(). av_frame_unref() releases this encoder's hold on
        // whichever surface hw_frame_ pointed at last time; the pool gives
        // one back that is not still in use.
        av_frame_unref(hw_frame_);
        if (av_hwframe_get_buffer(context_->hw_frames_ctx, hw_frame_, 0) < 0) {
            if (error != nullptr) *error = "could not take a frame from the device pool";
            return false;
        }
        if (av_hwframe_transfer_data(hw_frame_, frame_, 0) < 0) {
            if (error != nullptr) *error = "the frame would not go to the device";
            return false;
        }
        hw_frame_->pts = frame_->pts;
        submitted = hw_frame_;
    }

    int rc = avcodec_send_frame(context_, submitted);
    if (rc < 0) {
        if (error != nullptr) *error = "the encoder refused a frame: " + av_error(rc);
        return false;
    }
    while (true) {
        rc = avcodec_receive_packet(context_, packet_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            if (error != nullptr) *error = "the encoder failed: " + av_error(rc);
            return false;
        }
        out.bytes.insert(out.bytes.end(), packet_->data, packet_->data + packet_->size);
        out.keyframe = out.keyframe || (packet_->flags & AV_PKT_FLAG_KEY) != 0;
        av_packet_unref(packet_);
    }
    return true;
}

}  // namespace ds::video

#else  // DS_HAVE_VIDEO_CODEC

namespace ds::video {

bool available() { return false; }
void quieten_ffmpeg() {}

VideoEncoder::~VideoEncoder() = default;
void VideoEncoder::close() {}

bool VideoEncoder::open(const EncoderConfig&, std::string* error) {
    if (error != nullptr) *error = "this build has no video encoder";
    return false;
}

bool VideoEncoder::open_backend(const std::string&, const EncoderConfig&, std::string*) {
    return false;
}

bool VideoEncoder::encode(const uint32_t*, int, EncodedFrame& out, std::string* error) {
    out.bytes.clear();
    if (error != nullptr) *error = "this build has no video encoder";
    return false;
}

}  // namespace ds::video

#endif
