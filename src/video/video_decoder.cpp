#include "video/video_decoder.hpp"

#include "util/log.hpp"

#include <chrono>
#include <cstring>

#if DS_HAVE_VIDEO_CODEC

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace ds::video {
namespace {

std::string av_error(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

// The one place a platform is named. Everything else on this path is the same
// FFmpeg calls whichever of these it turns out to be.
//
// Not every backend below wants a device context: VAAPI and NVTEGRA are the
// plain software decoder with one attached, so its absence is what asks
// FFmpeg to negotiate the hardware pixel format through the ordinary
// get_format callback -- see decoder_name_for() just below, which is the
// other half of that same fact. MediaCodec decodes through Android's own API
// via JNI regardless, and a device context on top of that is what asks it for
// zero-copy Surface output instead of the plain CPU-readable frames to_rgb()
// below already knows how to read -- a second pipeline (EGL/GL interop with
// an Android Surface) this app does not have, so none is attached here and
// the decoder is left to hand back ordinary frames the same as any other.
AVHWDeviceType hw_device_for(const std::string& backend) {
    if (backend.find("nvtegra") != std::string::npos) {
        // A Switch. Averne's NVDEC work reaches FFmpeg as an ordinary hwaccel,
        // which is why there is nothing else to say about it here.
#ifdef AV_HWDEVICE_TYPE_NVTEGRA
        return AV_HWDEVICE_TYPE_NVTEGRA;
#else
        return AV_HWDEVICE_TYPE_NONE;
#endif
    }
    if (backend.find("_vaapi") != std::string::npos) return AV_HWDEVICE_TYPE_VAAPI;
    if (backend.find("_videotoolbox") != std::string::npos) {
        // Apple's, on every platform it has.
        //
        // Deliberately not wrapped in an #ifdef the way the Switch's is above.
        // These names are enumerators in AVHWDeviceType, not macros, so the
        // preprocessor cannot see them at all: #ifdef on one is false however
        // new the FFmpeg is, and the guarded line is simply never compiled.
        // Writing that here would have meant a Mac silently decoding in
        // software while the log said it had opened the hardware backend --
        // which is the quietest possible way for this to be wrong, and
        // indistinguishable from a slow machine.
        //
        // Nothing is lost by naming it outright: AVHWDeviceType has carried
        // this value since FFmpeg 3.4, long below anything that could build
        // the rest of this.
        return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    }
    if (backend.find("_cuvid") != std::string::npos) return AV_HWDEVICE_TYPE_CUDA;
    return AV_HWDEVICE_TYPE_NONE;
}

}  // namespace

void VideoDecoder::close_ffmpeg() {
    if (scaler_ != nullptr) {
        sws_freeContext(scaler_);
        scaler_ = nullptr;
    }
    if (software_frame_ != nullptr) av_frame_free(&software_frame_);
    if (frame_ != nullptr) av_frame_free(&frame_);
    if (packet_ != nullptr) av_packet_free(&packet_);
    if (context_ != nullptr) avcodec_free_context(&context_);
    if (hw_device_ != nullptr) av_buffer_unref(&hw_device_);
    backend_.clear();
    scaler_width_ = 0;
    scaler_height_ = 0;
    scaler_format_ = -1;
}

bool VideoDecoder::open_ffmpeg(const DecoderConfig& config, std::string* error) {
    quieten_ffmpeg();
    close();
    std::string last;
    for (const std::string& name : config.backends) {
        std::string reason;
        if (open_backend(name, &reason)) {
            log_info("video decoder: " + backend_);
            return true;
        }
        log_info("video decoder: " + name + " would not open: " + reason);
        last = name + ": " + reason;
        close();
    }
    if (error != nullptr) {
        *error = config.backends.empty() ? "no video decoder was asked for"
                                         : "no video decoder would open (" + last + ")";
    }
    return false;
}

bool VideoDecoder::open_backend(const std::string& name, std::string* error) {
    const AVCodec* codec = avcodec_find_decoder_by_name(decoder_name_for(name).c_str());
    if (codec == nullptr) {
        if (error != nullptr) *error = "this build has no such decoder";
        return false;
    }

    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) {
        if (error != nullptr) *error = "out of memory";
        return false;
    }

    const AVHWDeviceType hw_type = hw_device_for(name);
    if (hw_type != AV_HWDEVICE_TYPE_NONE) {
        const int rc = av_hwdevice_ctx_create(&hw_device_, hw_type, nullptr, nullptr, 0);
        if (rc < 0) {
            if (error != nullptr) *error = "no such device here: " + av_error(rc);
            return false;
        }
        context_->hw_device_ctx = av_buffer_ref(hw_device_);
    }
    // Nothing is held back waiting for a frame that might reorder ahead of it:
    // the encoder emits no B-frames, so display order is decode order and any
    // delay here would be latency for nothing.
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // What to do about a frame that arrived damaged, which on a UDP media
    // channel is an ordinary event rather than a broken one.
    //
    // By default FFmpeg discards such a frame and, having no complete
    // reference to work from, discards everything after it until the next
    // keyframe -- which with a ten-second GOP is a freeze measured in seconds
    // for one lost datagram. There is already a keyframe request for that
    // case, but it still costs a round trip.
    //
    // These three say the opposite: decode it anyway (SHOW_ALL, which also
    // lets decoding start mid-GOP instead of waiting for a keyframe to join),
    // hand it over even though it is known to be wrong (OUTPUT_CORRUPT), and
    // take the cheap non-conformant shortcuts while doing it (FAST). The
    // picture goes briefly smeared in the damaged region and comes right on
    // the next frames that reference clean data, which is what a viewer would
    // choose over a hold. Moonlight decodes with the same three for the same
    // reason.
    context_->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    context_->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    context_->flags2 |= AV_CODEC_FLAG2_FAST;
    // FFmpeg's default threading model for H.264 is frame-level: several
    // frames decoded across several threads at once for throughput, each held
    // until its turn to be emitted in order. That is exactly the buffering
    // LOW_DELAY above is trying to avoid -- a live stream pays for it as
    // several frames of decode latency, not as anything faster arriving on
    // screen. Slice-level threading parallelises inside one frame instead of
    // across several, so it can still use more than one core without ever
    // holding a frame back waiting on the next one.
    context_->thread_type = FF_THREAD_SLICE;

    const int rc = avcodec_open2(context_, codec, nullptr);
    if (rc < 0) {
        if (error != nullptr) *error = av_error(rc);
        return false;
    }

    frame_ = av_frame_alloc();
    software_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (frame_ == nullptr || software_frame_ == nullptr || packet_ == nullptr) {
        if (error != nullptr) *error = "out of memory";
        return false;
    }
    backend_ = name;
    return true;
}

bool VideoDecoder::to_rgb(AVFrame* frame, DecodedFrame& out, std::string* error) {
    // A frame that decoded onto the GPU has to come back before anything here
    // can look at it. This is the copy a texture path would later avoid, and the
    // reason that path is worth building once the rest works.
    AVFrame* source = frame;
    if (frame->hw_frames_ctx != nullptr) {
        av_frame_unref(software_frame_);
        // Timed on its own, as VideoToolbox's is: on an Apple TV the copy out
        // of the decoder's memory turned out to be half of everything a
        // picture cost, and a decode time that includes it cannot say so.
        const auto transfer_began = std::chrono::steady_clock::now();
        const int rc = av_hwframe_transfer_data(software_frame_, frame, 0);
        out.readback_micros = static_cast<int>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - transfer_began)
                .count());
        if (rc < 0) {
            if (error != nullptr) *error = "the frame would not come back off the device: " +
                                           av_error(rc);
            return false;
        }
        source = software_frame_;
    }

    // Planar YUV goes onward as planes, and is converted while it is drawn --
    // see DecodedLayout. This is the whole point of the exercise: the
    // sws_scale below is a pass over every pixel of every frame, and on a
    // handheld it costs more than the hardware decode it follows.
    const bool nv12 = source->format == AV_PIX_FMT_NV12;
    const bool yuv420p = source->format == AV_PIX_FMT_YUV420P;
    if (want_planar_ && (nv12 || yuv420p)) {
        out.layout = nv12 ? DecodedLayout::Nv12 : DecodedLayout::Yuv420p;
        out.width = source->width;
        out.height = source->height;
        out.pixels.clear();
        // Copied rather than pointed at: the AVFrame is reused by the next
        // decode, and what is here has to survive until it is drawn.
        //
        // A plain memcpy of whole rows including the decoder's own padding,
        // which is both fewer, larger copies than trimming each row would be
        // and exactly what the upload wants -- the stride goes with it.
        const int planes = nv12 ? 2 : 3;
        for (int index = 0; index < 3; ++index) {
            if (index >= planes || source->data[index] == nullptr) {
                out.plane[index].clear();
                out.stride[index] = 0;
                continue;
            }
            // Chroma is half the height in both layouts; luma is full.
            const int rows = index == 0 ? source->height : (source->height + 1) / 2;
            const int stride = source->linesize[index];
            out.stride[index] = stride;
            out.plane[index].resize(static_cast<std::size_t>(stride) * rows);
            std::memcpy(out.plane[index].data(), source->data[index],
                        static_cast<std::size_t>(stride) * rows);
        }
        return true;
    }

    if (scaler_ == nullptr || scaler_width_ != source->width ||
        scaler_height_ != source->height || scaler_format_ != source->format) {
        if (scaler_ != nullptr) sws_freeContext(scaler_);
        scaler_ = sws_getContext(source->width, source->height,
                                 static_cast<AVPixelFormat>(source->format), source->width,
                                 source->height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                                 nullptr);
        if (scaler_ == nullptr) {
            if (error != nullptr) *error = "could not set up colour conversion";
            return false;
        }
        // Matching the encoder's own color_range/dstRange (see
        // video_encoder.cpp) -- the only encoder this ever decodes is this
        // app's own, and it always signals full range now, so this reads it
        // the same way rather than assuming the conventional limited range
        // most YUV comes in. Only srcRange changes here: the destination is
        // RGB, which has no range of its own.
        sws_setColorspaceDetails(scaler_, sws_getCoefficients(SWS_CS_ITU601), 1,
                                 sws_getCoefficients(SWS_CS_ITU601), 0, 0, 1 << 16, 1 << 16);
        scaler_width_ = source->width;
        scaler_height_ = source->height;
        scaler_format_ = source->format;
    }

    out.layout = DecodedLayout::Xrgb8888;
    out.width = source->width;
    out.height = source->height;
    for (std::vector<uint8_t>& plane : out.plane) {
        plane.clear();
    }
    out.pixels.resize(static_cast<std::size_t>(source->width) * source->height);
    auto* destination = reinterpret_cast<uint8_t*>(out.pixels.data());
    const int destination_stride = source->width * 4;
    sws_scale(scaler_, source->data, source->linesize, 0, source->height, &destination,
              &destination_stride);
    return true;
}

bool VideoDecoder::decode_ffmpeg(const uint8_t* data,
                                 std::size_t size,
                                 DecodedFrame& out,
                                 std::string* error) {
    out.width = 0;
    out.height = 0;
    if (context_ == nullptr) {
        if (error != nullptr) *error = "the decoder is not open";
        return false;
    }
    if (data == nullptr || size == 0) {
        return true;  // nothing to do, which is not a failure
    }

    av_packet_unref(packet_);
    packet_->data = const_cast<uint8_t*>(data);
    packet_->size = static_cast<int>(size);

    int rc = avcodec_send_packet(context_, packet_);
    packet_->data = nullptr;
    packet_->size = 0;
    if (rc < 0) {
        if (error != nullptr) *error = "the decoder refused a packet: " + av_error(rc);
        return false;
    }

    bool produced = false;
    while (true) {
        av_frame_unref(frame_);
        rc = avcodec_receive_frame(context_, frame_);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) {
            break;
        }
        if (rc < 0) {
            if (error != nullptr) *error = "the decoder failed: " + av_error(rc);
            return false;
        }
        // The most recent one wins if more than one came out at once: this is a
        // live screen, and an older frame has nothing to offer a player.
        if (!to_rgb(frame_, out, error)) {
            return false;
        }
        produced = true;
    }
    (void)produced;
    return true;
}

}  // namespace ds::video

#else  // DS_HAVE_VIDEO_CODEC

namespace ds::video {

void VideoDecoder::close_ffmpeg() {}

bool VideoDecoder::open_ffmpeg(const DecoderConfig&, std::string* error) {
    if (error != nullptr) *error = "this build has no video decoder";
    return false;
}

bool VideoDecoder::open_backend(const std::string&, std::string*) { return false; }

bool VideoDecoder::to_rgb(AVFrame*, DecodedFrame&, std::string*) { return false; }

bool VideoDecoder::decode_ffmpeg(const uint8_t*, std::size_t, DecodedFrame& out, std::string* error) {
    out.width = 0;
    out.height = 0;
    if (error != nullptr) *error = "this build has no video decoder";
    return false;
}

}  // namespace ds::video

#endif

// The public half, which is the same on every platform: try Apple's own
// decoder first where there is one, and FFmpeg otherwise.
//
// Written once here rather than twice above because iOS and tvOS are built
// with DS_HAVE_VIDEO_CODEC=0 -- there is no Homebrew for a phone -- and
// VideoToolbox has to work in exactly that build. It is the build it was
// written for.
namespace ds::video {

// FFmpeg encodes or it does not; there is no second encoder anywhere.
bool can_encode() { return have_ffmpeg(); }

// Decoding has two possible answers, and on a phone only the second one is
// yes. This is what a companion tells a host when it says whether to bother
// sending video -- answering it with have_ffmpeg() is answering a different
// question, and the host believes the answer.
bool can_decode() { return have_ffmpeg() || VideoToolboxDecoder::available(); }

VideoDecoder::~VideoDecoder() { close(); }

bool VideoDecoder::is_open() const {
    return context_ != nullptr || (apple_ != nullptr && apple_->is_open());
}

bool VideoDecoder::open(const DecoderConfig& config, std::string* error) {
    close();
    // Only when the caller asked for it. The backend list is where a platform
    // says what it has (see DecoderConfig), and honouring it here keeps a test
    // able to ask for the software decoder on a Mac.
    bool wants_videotoolbox = false;
    for (const std::string& name : config.backends) {
        if (name.find("videotoolbox") != std::string::npos) {
            wants_videotoolbox = true;
            break;
        }
    }
    if (wants_videotoolbox && VideoToolboxDecoder::available()) {
        auto apple = std::make_unique<VideoToolboxDecoder>();
        std::string apple_error;
        if (apple->open(&apple_error)) {
            apple_ = std::move(apple);
            backend_ = "videotoolbox";
            return true;
        }
        // Not fatal. A Mac with FFmpeg can still go the long way round, and
        // saying which way it went is the whole of what the log needs to show.
        log_info("video decoder: VideoToolbox would not open (" + apple_error +
                 "), falling back");
    }
    return open_ffmpeg(config, error);
}

void VideoDecoder::close() {
    apple_.reset();
    close_ffmpeg();
}

bool VideoDecoder::decode(const uint8_t* data, std::size_t size, DecodedFrame& out,
                          std::string* error) {
    if (apple_ != nullptr) {
        return apple_->decode(data, size, out, error);
    }
    return decode_ffmpeg(data, size, out, error);
}

}  // namespace ds::video
