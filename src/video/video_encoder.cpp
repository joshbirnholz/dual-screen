#include "video/video_encoder.hpp"

#include "video/color_convert.hpp"

#include <algorithm>
#include <chrono>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

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

bool have_ffmpeg() { return true; }

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
            // Says outright whether this size is the source's or this app's
            // doing. Without that the line is a number with no provenance, and
            // "the stream looks lower resolution than it should" cannot be
            // told from the log: a Wii U title drawing its television at
            // 960x540 and a companion small enough to have capped it there
            // produce the same line. They are very different answers -- one is
            // the game, the other is this end -- and reading it wrong costs a
            // round trip to the far side of the world and back.
            const bool reduced = config.source_width > 0 && config.source_height > 0 &&
                                 (config.source_width != config.width ||
                                  config.source_height != config.height);
            const std::string provenance =
                reduced ? ", scaled down from the " + std::to_string(config.source_width) + "x" +
                              std::to_string(config.source_height) +
                              " the game draws it at, to fit what the far end can show"
                        : ", which is the size the game itself is drawing it at";
            log_info("video encoder: " + backend_ + " at " + std::to_string(config.width) + "x" +
                     std::to_string(config.height) + provenance + ", " +
                     std::to_string(config.bitrate_kbps) + " kbit/s");
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
    // And a window for it to hold that rate over.
    //
    // Left at zero, FFmpeg picks one for the driver, and on the Steam Deck's
    // AMD VAAPI what comes out is not rate-controlled at all: asked for 43.5
    // Mbit/s it produced 20.1, asked for 22.9 it produced 23.0, asked for 34.6
    // it produced 21.5, asked for 22.7 it produced 23.5. Four different
    // requests, one stream -- constant quality wearing a bitrate's clothes.
    //
    // That is not merely a number being ignored. At 30fps it means a typical
    // frame of 90KB and 23 Mbit/s of UDP going out in eighty-odd datagram
    // bursts, thirty times a second, which is real work for a handheld's
    // wifi -- and the far end measures the result as a tail: three quarters of
    // frames within 17ms of the quietest and one in twenty 75ms behind, with
    // not one frame lost in a whole session. A tail with no loss is queueing,
    // and after ruling out this end's pacing, the other end's loop and the
    // evenness of the frames, what is left to queue is the air.
    //
    // Half a second of audio-style buffer would let it wander; two frames'
    // worth holds it to the rate it was given without making every frame the
    // same size as every other, which is what a keyframe needs room for.
    if (config.fps > 0) {
        context_->rc_buffer_size = static_cast<int>(context_->bit_rate * 2 / config.fps);
        context_->rc_initial_buffer_occupancy = context_->rc_buffer_size * 3 / 4;
    }
    // A keyframe costs several times what a delta does -- tens of times, at
    // the bitrates this runs at -- and every one of them is a burst the
    // sender's pacing queue then has to spread out over the next several
    // frames. That shows up on this app's own stream report as a periodic
    // pacing-queue spike (measured at 92-160ms, arriving on exactly the
    // two-second cadence this used to be set to), which is latency paid on a
    // schedule for a recovery nothing had asked for.
    //
    // Nothing needs one on a schedule now: a device that joins asks for one,
    // and since MessageType::KeyframeRequest a device that loses a frame asks
    // too. So this is only a backstop for the case neither covers -- a
    // request and its answer both lost repeatedly -- and ten seconds is far
    // enough apart that the burst stops being a rhythm the picture can be
    // felt keeping time with.
    context_->gop_size = config.fps * 10;
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
        if (name.find("_vaapi") != std::string::npos) {
            // One: each picture comes out of the encoder as soon as it is
            // encoded, with nothing queued behind it -- what Sunshine runs.
            //
            // This was two for a while, and the reason is worth keeping. With
            // one, submitting a frame waits for it to finish encoding, and
            // that wait used to happen on the thread that also runs the game,
            // paces sending, processes acks and forwards input. Against a game
            // that keeps the GPU busy (Bayonetta 2) it was 17ms a frame -- the
            // encode queues behind the game's own rendering -- and the stream
            // fell to a third of the frames the game was producing. Two hid
            // the wait by handing back the previous submission's packet: a
            // whole frame of latency on every picture, to avoid a stall.
            //
            // Encoding has its own thread now (HostSession::encoder_loop()),
            // so the wait holds up nothing but the encode itself, and the
            // frame of latency is gone. DUALSCREEN_VAAPI_ASYNC_DEPTH sets it
            // anyway, for comparing the two on a real machine. EncodedFrame
            // still carries the pts of whatever actually came out, so a depth
            // above one keeps working.
            int depth = 1;
            if (const char* wanted = std::getenv("DUALSCREEN_VAAPI_ASYNC_DEPTH")) {
                depth = std::max(1, std::atoi(wanted));
            }
            av_opt_set_int(context_->priv_data, "async_depth", depth, 0);
        }
    } else {
        context_->pix_fmt = software_format;
        // Software encoders are the ones that would otherwise buffer for
        // quality. Both of these are the difference between "one packet per
        // frame" and "a packet eventually".
        av_opt_set(context_->priv_data, "preset", "veryfast", 0);
        av_opt_set(context_->priv_data, "tune", "zerolatency", 0);
    }

    // Asked for by name where the encoder has the option, because the picker
    // FFmpeg uses on its own is what produced the constant-quality stream
    // above. A driver that does not offer it fails this harmlessly -- the
    // option simply is not there -- and one that offers it and then cannot do
    // it fails the open below, which is what the retry is for.
    const bool asked_for_rate_control =
        context_->priv_data != nullptr &&
        av_opt_set(context_->priv_data, "rc_mode", "CBR", 0) == 0;

    int rc = avcodec_open2(context_, codec, nullptr);
    if (rc < 0 && asked_for_rate_control) {
        // It took the option and then would not open with it. Better a stream
        // that runs at whatever rate this driver likes than no stream: this is
        // exactly the shape of the VAAPI failures this file already carries
        // scars from, and none of them are worth a black screen.
        log_info("video encoder: " + name +
                 " would not open in constant-bitrate mode (" + av_error(rc) +
                 "); letting the driver choose its own rate control instead");
        av_opt_set(context_->priv_data, "rc_mode", "auto", 0);
        context_->rc_buffer_size = 0;
        context_->rc_initial_buffer_occupancy = 0;
        rc = avcodec_open2(context_, codec, nullptr);
    } else if (rc == 0) {
        log_info(std::string("video encoder: rate control is ") +
                 (asked_for_rate_control ? "the constant bitrate it was asked for"
                                         : "whatever this encoder chose; it has no rc_mode to set"));
    }
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
    pixel_order_ = config.pixel_order;
    const int source_width = config.source_width > 0 ? config.source_width : config.width;
    const int source_height = config.source_height > 0 ? config.source_height : config.height;
    source_format_ = static_cast<int>(source_format);
    software_format_ = static_cast<int>(software_format);
    width_ = config.width;
    height_ = config.height;
    if (!rebuild_scaler(source_width, source_height, error)) {
        return false;
    }

    backend_ = name;
    return true;
}

bool VideoEncoder::rebuild_scaler(int source_width, int source_height, std::string* error) {
    if (scaler_ != nullptr) {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }
    // Built by hand rather than through sws_getContext, for one option:
    // threads.
    //
    // This step is not a resize most of the time -- the Wii U hands over
    // 1920x1080 and it goes out at 1920x1080 -- it is a colour conversion,
    // RGB to NV12, and swscale does it on one core. Measured on a Steam Deck
    // that is 7ms of the 8.5ms this app spends encoding a frame: 7ms of
    // latency on every frame, and 7ms of a core taken away from the emulator
    // that is trying to render the next one. Sunshine does not pay it,
    // because it converts on the GPU on the way into the encoder.
    //
    // Doing it on the GPU here is a larger change than it looks -- the frames
    // arrive over a shared-memory link as plain bytes -- and the cheap half,
    // asking swscale to thread it, turned out not to work through this call.
    // See the option below. The 7ms is still there.
    //
    // sws_getContext cannot be told this because it initialises the context
    // before returning it, and the option has to be set before that.
    scaler_ = sws_alloc_context();
    if (scaler_ != nullptr) {
        av_opt_set_int(scaler_, "srcw", source_width, 0);
        av_opt_set_int(scaler_, "srch", source_height, 0);
        av_opt_set_int(scaler_, "src_format", source_format_, 0);
        av_opt_set_int(scaler_, "dstw", width_, 0);
        av_opt_set_int(scaler_, "dsth", height_, 0);
        av_opt_set_int(scaler_, "dst_format", software_format_, 0);
        // SWS_BILINEAR does double duty: it is what turns "convert pixel
        // format" into "convert pixel format and resample" the moment source
        // and dest sizes differ -- see EncoderConfig::source_width/height.
        av_opt_set_int(scaler_, "sws_flags", SWS_BILINEAR, 0);
        // Asked for, and measured as having no effect: 7.28ms before, 7.01ms
        // after, which is noise. swscale only spreads the work across these
        // threads when the picture goes through sws_scale_frame(); the plain
        // sws_scale() this calls runs on the caller's thread whatever the
        // context was told. Left in place because it costs nothing and the
        // day this moves to sws_scale_frame() it starts working -- but it is
        // not the 5ms it was meant to be, and the comment above should not be
        // read as though it were.
        av_opt_set_int(scaler_, "threads", 4, 0);
        if (sws_init_context(scaler_, nullptr, nullptr) < 0) {
            sws_freeContext(scaler_);
            scaler_ = nullptr;
        }
    }
    if (scaler_ == nullptr) {
        // Whatever went wrong above, the plain path still works and always
        // has. A slower conversion is not worth a session for.
        scaler_ = sws_getContext(source_width, source_height,
                                 static_cast<AVPixelFormat>(source_format_), width_, height_,
                                 static_cast<AVPixelFormat>(software_format_), SWS_BILINEAR,
                                 nullptr, nullptr, nullptr);
    }
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
    source_width_ = source_width;
    source_height_ = source_height;
    return true;
}

bool VideoEncoder::set_source_size(int width, int height, std::string* error) {
    if (context_ == nullptr) {
        if (error != nullptr) *error = "the encoder is not open";
        return false;
    }
    if (width == source_width_ && height == source_height_) {
        return true;
    }
    // Only the scaler, deliberately. Reopening the encoder for this would
    // start a new H.264 stream -- new parameter sets, a fresh IDR, and a
    // decoder at the far end having to reconfigure itself mid-play. That was
    // measured happening several times a second on a real session (Cemu's
    // GamePad scan buffer alternating between two sizes), and it is what a
    // corrupt picture on the far end actually looked like. What comes out of
    // this encoder is the same size as before; only what goes in has changed,
    // and resampling is exactly what this scaler is already for.
    return rebuild_scaler(width, height, error);
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

    const auto milliseconds_since = [](std::chrono::steady_clock::time_point from) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - from)
            .count();
    };
    const auto scale_started = std::chrono::steady_clock::now();

    const auto* source = reinterpret_cast<const uint8_t*>(pixels);
    const int source_stride = stride_pixels * 4;
    // Straight to NV12 where there is nothing to resize, which is the ordinary
    // case: a Wii U hands over 1920x1080 and it goes out at 1920x1080. That is
    // a colour conversion, and swscale charges five milliseconds a frame for
    // this app's full-range request alone -- see rgb_to_nv12(), which does the
    // same arithmetic in under two.
    //
    // swscale still does the rest: anything being scaled down for a smaller
    // far end, and any format pairing this does not cover.
    const bool no_resizing = source_width_ == width_ && source_height_ == height_;
    const bool plain_nv12 = software_format_ == static_cast<int>(AV_PIX_FMT_NV12);
    const bool even = (width_ % 2) == 0 && (height_ % 2) == 0;
    if (no_resizing && plain_nv12 && even) {
        rgb_to_nv12(source, static_cast<std::size_t>(source_stride), width_, height_, pixel_order_,
                    frame_->data[0], static_cast<std::size_t>(frame_->linesize[0]),
                    frame_->data[1], static_cast<std::size_t>(frame_->linesize[1]),
                    conversion_threads());
    } else {
        // The number of rows sws_scale reads from the source, which is its own
        // height, not the encoded frame's -- the two differ whenever
        // EncoderConfig::source_width/source_height asked for downscaling.
        sws_scale(scaler_, &source, &source_stride, 0, source_height_, frame_->data,
                 frame_->linesize);
    }
    cost_.scale_ms += milliseconds_since(scale_started);
    frame_->pts = next_pts_++;
    last_submitted_pts_ = frame_->pts;
    if (force_keyframe_) {
        frame_->pict_type = AV_PICTURE_TYPE_I;
        force_keyframe_ = false;
    } else {
        frame_->pict_type = AV_PICTURE_TYPE_NONE;
    }

    const auto upload_started = std::chrono::steady_clock::now();
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
        // And whether it is to be a keyframe, which is set on frame_ above and
        // was not being carried across: the transfer copies pixels and nothing
        // else. Every keyframe a companion asked a hardware encoder for came
        // out as an ordinary frame, so a lost frame froze the picture until
        // the encoder's own keyframe, up to ten seconds later -- seen on an
        // Apple TV as five seconds of pictures arriving whole and every one
        // passed over, with the Deck answering two or three requests a second
        // the whole time.
        hw_frame_->pict_type = frame_->pict_type;
        submitted = hw_frame_;
    }
    cost_.upload_ms += milliseconds_since(upload_started);
    const auto submit_started = std::chrono::steady_clock::now();

    int rc = avcodec_send_frame(context_, submitted);
    if (rc < 0) {
        if (error != nullptr) *error = "the encoder refused a frame: " + av_error(rc);
        return false;
    }
    // One packet, not everything available: with the encoder pipelining, two
    // packets waiting are two different pictures, and concatenating them
    // would hand the caller one frame's worth of bytes labelled with the
    // other's moment. Anything still queued comes out of the next call, which
    // stays balanced because each call also puts exactly one frame in.
    rc = avcodec_receive_packet(context_, packet_);
    if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
        cost_.submit_ms += milliseconds_since(submit_started);
        ++cost_.frames;
        return true;  // still encoding; out stays empty, which is not a failure
    }
    if (rc < 0) {
        if (error != nullptr) *error = "the encoder failed: " + av_error(rc);
        return false;
    }
    out.bytes.assign(packet_->data, packet_->data + packet_->size);
    out.keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;
    out.pts = packet_->pts;
    av_packet_unref(packet_);
    cost_.submit_ms += milliseconds_since(submit_started);
    ++cost_.frames;
    return true;
}

}  // namespace ds::video

#else  // DS_HAVE_VIDEO_CODEC

namespace ds::video {

bool have_ffmpeg() { return false; }
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
