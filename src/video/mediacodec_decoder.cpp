#include "video/mediacodec_decoder.hpp"

#if defined(__ANDROID__)

#include <dlfcn.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

#include "util/log.hpp"
#include "video/h264_bitstream.hpp"

namespace ds::video {
namespace {

// MediaCodecInfo.CodecCapabilities' colour formats, by number: the NDK has no
// names for them. These are the ones a decoder hands back as bytes.
constexpr int32_t kColorFormatYuv420Planar = 19;          // I420
constexpr int32_t kColorFormatYuv420PackedPlanar = 20;    // I420 again
constexpr int32_t kColorFormatYuv420SemiPlanar = 21;      // NV12
constexpr int32_t kColorFormatYuv420PackedSemiPlanar = 39;
constexpr int32_t kColorFormatYuv420Flexible = 0x7f420888;
constexpr int32_t kColorFormatTiYuv420PackedSemiPlanar = 0x7f000100;
constexpr int32_t kColorFormatQcomYuv420SemiPlanar = 0x7fa30c00;
// Qualcomm's own NV12, which is most Android handhelds' -- a G Cloud's
// Snapdragon among them. The same bytes as plain NV12, with rows padded to
// 128 and the luma plane padded to a multiple of 32 rows.
constexpr int32_t kColorFormatQcomYuv420SemiPlanar32m = 0x7fa30c04;
// Qualcomm's tiled layout, which is not rows at all. Older chips only; a
// decoder that insists on it is one this app cannot read.
constexpr int32_t kColorFormatQcomTiled = 0x7fa30c03;

// MediaCodec.BUFFER_FLAG_CODEC_CONFIG.
constexpr uint32_t kBufferFlagCodecConfig = 2;

// How long one decode() may wait for the picture it just fed to come out.
//
// A hardware decoder takes a few milliseconds over a frame, and handing the
// picture back on this call rather than the next is a whole frame period of
// latency saved. But some decoders keep a frame or two inside however long
// they are waited for, and waiting on one of those costs this long every
// frame for nothing -- so the wait stops being offered once it has been seen
// to be useless. See Impl::wait_us.
constexpr int64_t kOutputWaitUs = 12000;
constexpr int kUselessWaitsBeforeGivingUp = 4;
// And how long to wait for room to feed a frame in, which is only ever short
// unless the decoder has wedged.
constexpr int64_t kInputWaitUs = 20000;

int64_t micros_since(std::chrono::steady_clock::time_point began) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - began)
        .count();
}

int32_t format_int(AMediaFormat* format, const char* key, int32_t fallback) {
    int32_t value = 0;
    return AMediaFormat_getInt32(format, key, &value) ? value : fallback;
}

int32_t align_up(int32_t value, int32_t to) { return (value + to - 1) / to * to; }

std::string hex(int32_t value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%x", static_cast<unsigned>(value));
    return buffer;
}

// AMediaCodec_getName arrived in Android 9 (API 28), above this app's floor,
// so it is looked up rather than linked: a device that has it says which
// decoder it chose, and one that does not just says less.
std::string codec_name_of(AMediaCodec* codec) {
    using GetName = media_status_t (*)(AMediaCodec*, char**);
    using ReleaseName = void (*)(AMediaCodec*, char*);
    static void* library = dlopen("libmediandk.so", RTLD_NOW);
    if (library == nullptr) return "";
    static auto get_name = reinterpret_cast<GetName>(dlsym(library, "AMediaCodec_getName"));
    static auto release_name =
        reinterpret_cast<ReleaseName>(dlsym(library, "AMediaCodec_releaseName"));
    if (get_name == nullptr) return "";
    char* name = nullptr;
    if (get_name(codec, &name) != AMEDIA_OK || name == nullptr) return "";
    std::string copy = name;
    if (release_name != nullptr) release_name(codec, name);
    return copy;
}

// How a decoded picture is laid out in an output buffer, read off the
// decoder's output format whenever it changes.
struct OutputLayout {
    bool known = false;
    int32_t color_format = 0;
    int32_t stride = 0;
    int32_t slice_height = 0;
    int32_t crop_left = 0;
    int32_t crop_top = 0;
    int32_t width = 0;   // what is shown, after cropping
    int32_t height = 0;
    bool planar = false;  // I420 rather than NV12
};

OutputLayout read_layout(AMediaFormat* format, int fallback_width, int fallback_height) {
    OutputLayout layout;
    const int32_t width = format_int(format, "width", fallback_width);
    const int32_t height = format_int(format, "height", fallback_height);
    layout.color_format = format_int(format, "color-format", kColorFormatYuv420SemiPlanar);

    // The crop rectangle is what is actually picture. A 1280x720 stream is
    // coded 1280x736 and many decoders report that as their width and height,
    // with the real size only here.
    const int32_t crop_left = format_int(format, "crop-left", 0);
    const int32_t crop_top = format_int(format, "crop-top", 0);
    const int32_t crop_right = format_int(format, "crop-right", width - 1);
    const int32_t crop_bottom = format_int(format, "crop-bottom", height - 1);
    layout.crop_left = std::max(0, crop_left) & ~1;
    layout.crop_top = std::max(0, crop_top) & ~1;
    layout.width = crop_right - crop_left + 1;
    layout.height = crop_bottom - crop_top + 1;
    if (layout.width <= 0 || layout.height <= 0) {
        layout.crop_left = layout.crop_top = 0;
        layout.width = width;
        layout.height = height;
    }

    layout.stride = format_int(format, "stride", 0);
    layout.slice_height = format_int(format, "slice-height", 0);
    if (layout.color_format == kColorFormatQcomYuv420SemiPlanar32m) {
        // The padding its name promises, for a decoder that does not say.
        if (layout.stride <= 0) layout.stride = align_up(width, 128);
        if (layout.slice_height <= 0) layout.slice_height = align_up(height, 32);
    }
    if (layout.stride < width) layout.stride = width;
    if (layout.slice_height < height) layout.slice_height = height;

    switch (layout.color_format) {
        case kColorFormatYuv420Planar:
        case kColorFormatYuv420PackedPlanar:
            layout.planar = true;
            layout.known = true;
            break;
        case kColorFormatYuv420SemiPlanar:
        case kColorFormatYuv420PackedSemiPlanar:
        case kColorFormatTiYuv420PackedSemiPlanar:
        case kColorFormatQcomYuv420SemiPlanar:
        case kColorFormatQcomYuv420SemiPlanar32m:
            layout.known = true;
            break;
        case kColorFormatQcomTiled:
            layout.known = false;
            break;
        case kColorFormatYuv420Flexible:
        default:
            // Flexible, or a vendor's own number. In a byte buffer, rather
            // than an Image, those are NV12 on every device that has been
            // looked at; read as that, and say so once (see decode()).
            layout.known = true;
            break;
    }
    return layout;
}

}  // namespace

struct MediaCodecDecoder::Impl {
    bool open = false;
    ParameterSets sets;
    AMediaCodec* codec = nullptr;
    std::string name;
    OutputLayout layout;
    int configured_width = 0;
    int configured_height = 0;
    int64_t presentation_us = 0;
    // Frames fed in and not yet come out.
    int64_t in_flight = 0;
    int64_t wait_us = kOutputWaitUs;
    int useless_waits = 0;
    bool waited_last_time_for_nothing = false;
    bool said_format = false;
    bool said_first_picture = false;

    ~Impl() { destroy_codec(); }

    void destroy_codec() {
        if (codec != nullptr) {
            AMediaCodec_stop(codec);
            AMediaCodec_delete(codec);
            codec = nullptr;
        }
        layout = OutputLayout();
        in_flight = 0;
    }

    AMediaFormat* make_format(bool low_latency) const {
        AMediaFormat* format = AMediaFormat_new();
        AMediaFormat_setString(format, "mime", "video/avc");
        AMediaFormat_setInt32(format, "width", configured_width);
        AMediaFormat_setInt32(format, "height", configured_height);
        // The parameter sets, start codes and all: "csd-0" and "csd-1" are
        // documented as Annex B for H.264.
        std::vector<uint8_t> csd;
        auto set_csd = [&](const char* key, const std::vector<uint8_t>& nal) {
            csd.assign({0, 0, 0, 1});
            csd.insert(csd.end(), nal.begin(), nal.end());
            AMediaFormat_setBuffer(format, key, csd.data(), csd.size());
        };
        set_csd("csd-0", sets.sps);
        set_csd("csd-1", sets.pps);
        if (low_latency) {
            // Hand each picture out as soon as it is decoded rather than
            // holding a few for smoothness. The standard key since Android 11,
            // and the vendors' own before that -- the same set Moonlight uses.
            // Keys a decoder does not know are ignored, but a decoder that
            // dislikes them can refuse to configure at all, which is why
            // configure() is tried again without them.
            AMediaFormat_setInt32(format, "low-latency", 1);
            AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
            AMediaFormat_setInt32(format, "vendor.qti-ext-dec-picture-order.enable", 1);
            AMediaFormat_setInt32(format, "vendor.rtc-ext-dec-low-latency.enable", 1);
            AMediaFormat_setInt32(format, "vendor.low-latency.enable", 1);
            // Realtime, as against best effort.
            AMediaFormat_setInt32(format, "priority", 0);
        }
        return format;
    }

    bool build_codec(std::string* error) {
        destroy_codec();
        int width = 0;
        int height = 0;
        if (!sps_picture_size(sets.sps.data(), sets.sps.size(), &width, &height)) {
            // Only a hint to the decoder, which reads the real size from the
            // stream; this is what most of them are happy to be told.
            width = 1280;
            height = 720;
        }
        configured_width = width;
        configured_height = height;

        codec = AMediaCodec_createDecoderByType("video/avc");
        if (codec == nullptr) {
            if (error != nullptr) *error = "this device has no H.264 decoder";
            return false;
        }
        media_status_t status = AMEDIA_ERROR_UNKNOWN;
        for (bool low_latency : {true, false}) {
            AMediaFormat* format = make_format(low_latency);
            status = AMediaCodec_configure(codec, format, nullptr, nullptr, 0);
            AMediaFormat_delete(format);
            if (status == AMEDIA_OK) {
                break;
            }
            if (low_latency) {
                log_info("video decoder: MediaCodec would not take the low-latency settings (" +
                         std::to_string(status) + "), trying without them");
                // A codec that has refused a configure is not promised to take
                // another, so the second try is on a fresh one.
                AMediaCodec_delete(codec);
                codec = AMediaCodec_createDecoderByType("video/avc");
                if (codec == nullptr) break;
            }
        }
        if (codec == nullptr || status != AMEDIA_OK) {
            if (error != nullptr) {
                *error = "MediaCodec would not configure: " + std::to_string(status);
            }
            if (codec != nullptr) {
                AMediaCodec_delete(codec);
                codec = nullptr;
            }
            return false;
        }
        status = AMediaCodec_start(codec);
        if (status != AMEDIA_OK) {
            if (error != nullptr) *error = "MediaCodec would not start: " + std::to_string(status);
            AMediaCodec_delete(codec);
            codec = nullptr;
            return false;
        }
        name = codec_name_of(codec);
        said_first_picture = false;
        log_info("video decoder: MediaCodec " + (name.empty() ? std::string("(unnamed)") : name) +
                 ", configured for " + std::to_string(width) + "x" + std::to_string(height));
        return true;
    }

    void take_output_format() {
        AMediaFormat* format = AMediaCodec_getOutputFormat(codec);
        if (format == nullptr) return;
        layout = read_layout(format, configured_width, configured_height);
        AMediaFormat_delete(format);
        if (!said_format) {
            said_format = true;
            std::string what = "colour format " + hex(layout.color_format);
            if (!layout.known) {
                what += ", a tiled layout this app cannot read";
            } else if (layout.planar) {
                what += " (I420)";
            } else if (layout.color_format == kColorFormatYuv420SemiPlanar ||
                       layout.color_format == kColorFormatQcomYuv420SemiPlanar32m ||
                       layout.color_format == kColorFormatQcomYuv420SemiPlanar) {
                what += " (NV12)";
            } else {
                what += ", read as NV12";
            }
            log_info("video decoder: pictures arrive " + std::to_string(layout.width) + "x" +
                     std::to_string(layout.height) + ", stride " + std::to_string(layout.stride) +
                     ", " + std::to_string(layout.slice_height) + " rows to a plane, " + what);
        }
    }

    // One output buffer, copied out into `out` as planes.
    bool copy_out(const uint8_t* buffer, std::size_t size, DecodedFrame& out, std::string* error) {
        if (!layout.known) {
            if (error != nullptr) *error = "the decoder writes a layout this app cannot read";
            return false;
        }
        const auto began = std::chrono::steady_clock::now();
        const std::size_t stride = static_cast<std::size_t>(layout.stride);
        const std::size_t rows = static_cast<std::size_t>(layout.height);
        const std::size_t chroma_rows = (rows + 1) / 2;
        const std::size_t luma_plane = stride * static_cast<std::size_t>(layout.slice_height);
        const std::size_t luma_at =
            static_cast<std::size_t>(layout.crop_top) * stride + layout.crop_left;

        DecodedFrame frame;
        frame.width = layout.width;
        frame.height = layout.height;
        // `row_bytes` is how much of a row is picture. The last row needs
        // only that, not its padding: a decoder is free to end the buffer
        // straight after it.
        auto copy_plane = [&](int index, std::size_t at, std::size_t plane_stride,
                              std::size_t plane_rows, std::size_t row_bytes) {
            const std::size_t needed = plane_stride * (plane_rows - 1) + row_bytes;
            if (at > size || needed > size - at) {
                return false;
            }
            const std::size_t available = std::min(plane_stride * plane_rows, size - at);
            frame.plane[index].assign(buffer + at, buffer + at + available);
            frame.plane[index].resize(plane_stride * plane_rows);
            frame.stride[index] = static_cast<int>(plane_stride);
            return true;
        };
        const std::size_t width = static_cast<std::size_t>(layout.width);

        bool copied = copy_plane(0, luma_at, stride, rows, width);
        if (layout.planar) {
            frame.layout = DecodedLayout::Yuv420p;
            const std::size_t chroma_stride = stride / 2;
            const std::size_t chroma_plane =
                chroma_stride * static_cast<std::size_t>((layout.slice_height + 1) / 2);
            const std::size_t chroma_at = static_cast<std::size_t>(layout.crop_top / 2) *
                                              chroma_stride + layout.crop_left / 2;
            copied = copied && copy_plane(1, luma_plane + chroma_at, chroma_stride, chroma_rows,
                                (width + 1) / 2) &&
                     copy_plane(2, luma_plane + chroma_plane + chroma_at, chroma_stride,
                                chroma_rows, (width + 1) / 2);
        } else {
            frame.layout = DecodedLayout::Nv12;
            const std::size_t chroma_at =
                static_cast<std::size_t>(layout.crop_top / 2) * stride + layout.crop_left;
            copied = copied && copy_plane(1, luma_plane + chroma_at, stride, chroma_rows,
                                         (width + 1) & ~std::size_t{1});
        }
        if (!copied) {
            if (error != nullptr) {
                *error = "a decoded picture was smaller than its format says (" +
                         std::to_string(size) + " bytes for " + std::to_string(layout.width) +
                         "x" + std::to_string(layout.height) + ", stride " +
                         std::to_string(layout.stride) + ")";
            }
            return false;
        }
        frame.readback_micros = static_cast<int>(micros_since(began));
        frame.lock_micros = out.lock_micros;
        out = std::move(frame);
        return true;
    }

    // Takes everything the decoder has finished, keeping the newest picture:
    // this is a live screen, and an older frame has nothing to offer a
    // player. Waits up to `wait_us` for the first one.
    bool drain(DecodedFrame& out, int64_t wait_us_now, std::string* error) {
        bool produced = false;
        int64_t timeout = wait_us_now;
        const auto began = std::chrono::steady_clock::now();
        while (true) {
            AMediaCodecBufferInfo info;
            const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec, &info, timeout);
            if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
                break;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                take_output_format();
                continue;
            }
            if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
                continue;  // the NDK fetches each buffer by index anyway
            }
            if (index < 0) {
                if (error != nullptr) {
                    *error = "MediaCodec failed while decoding: " + std::to_string(index);
                }
                return false;
            }
            // Once one has come out, the rest are only taken if they are
            // already there -- never waited for.
            timeout = 0;
            if (in_flight > 0) --in_flight;
            if ((info.flags & kBufferFlagCodecConfig) != 0 || info.size <= 0) {
                AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
                continue;
            }
            if (!layout.known && layout.color_format == 0) {
                take_output_format();
            }
            std::size_t capacity = 0;
            uint8_t* buffer = AMediaCodec_getOutputBuffer(codec, static_cast<size_t>(index),
                                                          &capacity);
            bool ok = true;
            if (buffer != nullptr && info.offset >= 0 &&
                static_cast<std::size_t>(info.offset) + static_cast<std::size_t>(info.size) <=
                    capacity) {
                out.lock_micros = static_cast<int>(micros_since(began));
                ok = copy_out(buffer + info.offset, static_cast<std::size_t>(info.size), out,
                              error);
                produced = produced || ok;
            }
            AMediaCodec_releaseOutputBuffer(codec, static_cast<size_t>(index), false);
            if (!ok) {
                return false;
            }
        }
        if (produced && !said_first_picture) {
            said_first_picture = true;
            log_info("video decoder: first picture out of MediaCodec, " +
                     std::to_string(out.width) + "x" + std::to_string(out.height));
        }
        return true;
    }
};

MediaCodecDecoder::MediaCodecDecoder() = default;
MediaCodecDecoder::~MediaCodecDecoder() = default;

bool MediaCodecDecoder::available() { return true; }

bool MediaCodecDecoder::open(std::string* /*error*/) {
    impl_ = std::make_unique<Impl>();
    impl_->open = true;
    return true;
}

void MediaCodecDecoder::close() { impl_.reset(); }

bool MediaCodecDecoder::is_open() const { return impl_ != nullptr && impl_->open; }

const std::string& MediaCodecDecoder::codec_name() const {
    static const std::string none;
    return impl_ != nullptr ? impl_->name : none;
}

bool MediaCodecDecoder::decode(const uint8_t* data, std::size_t size, DecodedFrame& out,
                               std::string* error) {
    out = DecodedFrame();
    if (impl_ == nullptr || data == nullptr || size == 0) {
        return true;
    }
    Impl& impl = *impl_;

    // Parameter sets arrive inside a keyframe, and only a change is worth
    // acting on: a stream repeats them on every keyframe, and a rebuilt codec
    // drops whatever was in flight through it.
    if (collect_parameter_sets(data, size, impl.sets) || impl.codec == nullptr) {
        if (impl.sets.complete() && !impl.build_codec(error)) {
            return false;
        }
    }
    if (impl.codec == nullptr) {
        // Nothing to decode against yet; everything before the first
        // keyframe is dropped, and the caller asks for one.
        return true;
    }

    // Whatever finished since last time comes out first, so that it is not
    // what is still sitting there when the wait below gives up.
    if (!impl.drain(out, 0, error)) {
        return false;
    }
    const bool had_old_picture = out.valid();
    if (impl.waited_last_time_for_nothing) {
        impl.waited_last_time_for_nothing = false;
        // The picture that wait gave up on came out just now, unasked: this
        // decoder holds frames for as long as it is waited on.
        if (had_old_picture && ++impl.useless_waits >= kUselessWaitsBeforeGivingUp &&
            impl.wait_us > 0) {
            impl.wait_us = 0;
            log_info("video decoder: " + (impl.name.empty() ? std::string("MediaCodec") : impl.name) +
                     " keeps each picture until the next one is fed in, so it is no longer "
                     "waited for");
        }
    }

    ssize_t slot = AMediaCodec_dequeueInputBuffer(impl.codec, kInputWaitUs);
    if (slot < 0) {
        // Full. Taking its pictures out is what frees room to put one in.
        if (!impl.drain(out, kInputWaitUs, error)) {
            return false;
        }
        slot = AMediaCodec_dequeueInputBuffer(impl.codec, kInputWaitUs);
    }
    if (slot < 0) {
        if (error != nullptr) *error = "MediaCodec has taken no frame in for too long";
        return false;
    }
    std::size_t capacity = 0;
    uint8_t* input = AMediaCodec_getInputBuffer(impl.codec, static_cast<size_t>(slot), &capacity);
    if (input == nullptr || capacity < size) {
        AMediaCodec_queueInputBuffer(impl.codec, static_cast<size_t>(slot), 0, 0,
                                     impl.presentation_us, 0);
        if (error != nullptr) {
            *error = "a frame of " + std::to_string(size) + " bytes does not fit MediaCodec's " +
                     std::to_string(capacity);
        }
        return false;
    }
    // The access unit exactly as it came, parameter sets included: MediaCodec
    // takes Annex B, and in-band SPS/PPS are what a stream carries anyway.
    std::memcpy(input, data, size);
    // Steady, made-up timestamps: nothing here is presented by them, but a
    // decoder that sees them go backwards or repeat may reorder or drop.
    impl.presentation_us += 16667;
    const media_status_t queued = AMediaCodec_queueInputBuffer(
        impl.codec, static_cast<size_t>(slot), 0, size, impl.presentation_us, 0);
    if (queued != AMEDIA_OK) {
        if (error != nullptr) *error = "MediaCodec refused a frame: " + std::to_string(queued);
        return false;
    }
    ++impl.in_flight;

    DecodedFrame newest;
    if (!impl.drain(newest, impl.wait_us, error)) {
        return false;
    }
    if (newest.valid()) {
        out = std::move(newest);
        impl.useless_waits = 0;
    } else if (impl.wait_us > 0) {
        impl.waited_last_time_for_nothing = true;
    }
    return true;
}

}  // namespace ds::video

#else  // not Android

namespace ds::video {

struct MediaCodecDecoder::Impl {};

MediaCodecDecoder::MediaCodecDecoder() = default;
MediaCodecDecoder::~MediaCodecDecoder() = default;

bool MediaCodecDecoder::available() { return false; }

bool MediaCodecDecoder::open(std::string* error) {
    if (error != nullptr) *error = "this build has no MediaCodec";
    return false;
}

void MediaCodecDecoder::close() {}
bool MediaCodecDecoder::is_open() const { return false; }

const std::string& MediaCodecDecoder::codec_name() const {
    static const std::string none;
    return none;
}

bool MediaCodecDecoder::decode(const uint8_t*, std::size_t, DecodedFrame& out, std::string*) {
    out = DecodedFrame();
    return false;
}

}  // namespace ds::video

#endif
