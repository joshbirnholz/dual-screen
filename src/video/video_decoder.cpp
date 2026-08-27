#include "video/video_decoder.hpp"

#include "util/log.hpp"

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
    if (backend.find("mediacodec") != std::string::npos) return AV_HWDEVICE_TYPE_MEDIACODEC;
    if (backend.find("_cuvid") != std::string::npos) return AV_HWDEVICE_TYPE_CUDA;
    return AV_HWDEVICE_TYPE_NONE;
}

}  // namespace

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close() {
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

bool VideoDecoder::open(const DecoderConfig& config, std::string* error) {
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
    const AVCodec* codec = avcodec_find_decoder_by_name(name.c_str());
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
        const int rc = av_hwframe_transfer_data(software_frame_, frame, 0);
        if (rc < 0) {
            if (error != nullptr) *error = "the frame would not come back off the device: " +
                                           av_error(rc);
            return false;
        }
        source = software_frame_;
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
        scaler_width_ = source->width;
        scaler_height_ = source->height;
        scaler_format_ = source->format;
    }

    out.width = source->width;
    out.height = source->height;
    out.pixels.resize(static_cast<std::size_t>(source->width) * source->height);
    auto* destination = reinterpret_cast<uint8_t*>(out.pixels.data());
    const int destination_stride = source->width * 4;
    sws_scale(scaler_, source->data, source->linesize, 0, source->height, &destination,
              &destination_stride);
    return true;
}

bool VideoDecoder::decode(const uint8_t* data,
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

VideoDecoder::~VideoDecoder() = default;
void VideoDecoder::close() {}

bool VideoDecoder::open(const DecoderConfig&, std::string* error) {
    if (error != nullptr) *error = "this build has no video decoder";
    return false;
}

bool VideoDecoder::open_backend(const std::string&, std::string*) { return false; }

bool VideoDecoder::to_rgb(AVFrame*, DecodedFrame&, std::string*) { return false; }

bool VideoDecoder::decode(const uint8_t*, std::size_t, DecodedFrame& out, std::string* error) {
    out.width = 0;
    out.height = 0;
    if (error != nullptr) *error = "this build has no video decoder";
    return false;
}

}  // namespace ds::video

#endif
