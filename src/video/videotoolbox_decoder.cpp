#include "video/videotoolbox_decoder.hpp"

#if defined(__APPLE__)

#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

#include <chrono>
#include <cstring>
#include <vector>

#include "util/log.hpp"
#include "video/h264_bitstream.hpp"

namespace ds::video {
namespace {

std::string os_status(OSStatus status) { return std::to_string(static_cast<long>(status)); }

// A CoreFoundation handle that releases itself. There are five of these in
// play per frame and a missed release is a leak of a whole picture.
template <typename Ref>
class Owned {
public:
    Owned() = default;
    explicit Owned(Ref ref) : ref_(ref) {}
    ~Owned() { reset(); }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;

    void reset(Ref ref = nullptr) {
        if (ref_ != nullptr) CFRelease(ref_);
        ref_ = ref;
    }
    Ref get() const { return ref_; }
    Ref* slot() {
        reset();
        return &ref_;
    }
    explicit operator bool() const { return ref_ != nullptr; }

private:
    Ref ref_ = nullptr;
};

void set_number(CFMutableDictionaryRef dictionary, CFStringRef key, int32_t value) {
    CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
    CFDictionarySetValue(dictionary, key, number);
    CFRelease(number);
}

}  // namespace

struct VideoToolboxDecoder::Impl {
    ParameterSets sets;
    Owned<CMFormatDescriptionRef> format;
    VTDecompressionSessionRef session = nullptr;
    // Filled by the decode callback, which runs before DecodeFrame returns
    // because this asks for synchronous decoding -- see decode().
    CVPixelBufferRef latest = nullptr;
    OSStatus latest_status = noErr;
    std::vector<uint8_t> avcc;
    bool open = false;
    bool said_format = false;

    ~Impl() { tear_down(); }

    void tear_down() {
        if (session != nullptr) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
            session = nullptr;
        }
        format.reset();
        release_latest();
    }

    void release_latest() {
        if (latest != nullptr) {
            CVPixelBufferRelease(latest);
            latest = nullptr;
        }
    }

    static void on_decoded(void* context, void* /*frame_context*/, OSStatus status,
                           VTDecodeInfoFlags /*flags*/, CVImageBufferRef image,
                           CMTime /*presented_at*/, CMTime /*duration*/) {
        auto* self = static_cast<Impl*>(context);
        self->latest_status = status;
        if (status != noErr || image == nullptr) {
            return;
        }
        // Retained because the callback's reference does not outlive it, and
        // the planes are read after DecodeFrame returns.
        self->release_latest();
        self->latest = CVPixelBufferRetain(image);
    }

    bool build_session(std::string* error) {
        tear_down();
        if (!sets.complete()) {
            if (error != nullptr) *error = "no SPS/PPS to build a decoder from";
            return false;
        }

        const uint8_t* parameters[2] = {sets.sps.data(), sets.pps.data()};
        const std::size_t sizes[2] = {sets.sps.size(), sets.pps.size()};
        // Four, matching the length prefixes to_length_prefixed() writes.
        OSStatus status = CMVideoFormatDescriptionCreateFromH264ParameterSets(
            kCFAllocatorDefault, 2, parameters, sizes, 4, format.slot());
        if (status != noErr) {
            if (error != nullptr) {
                *error = "the parameter sets would not make a format description: " +
                         os_status(status);
            }
            return false;
        }

        CFMutableDictionaryRef attributes = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 4, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        // Full range, not video range, and this is not a detail.
        //
        // This app encodes full range on purpose -- limited range throws away
        // the outer 14% of every channel before compression and that reads as
        // banding -- and the fragment shader that converts these planes uses
        // full-range BT.601 coefficients to match (see ds_yuv_to_rgb in
        // quad_renderer.cpp, and color_convert.cpp at the other end). Asking
        // for video range here would hand those coefficients the wrong numbers
        // and every frame would come out washed out and clipped, which looks
        // like a bad encoder rather than a wrong constant.
        set_number(attributes, kCVPixelBufferPixelFormatTypeKey,
                   kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
        // So the buffer can become a texture without a trip through system
        // memory. Nothing here uses that yet -- the planes are still copied
        // below -- but a buffer allocated without it cannot be used that way
        // later, and the flag costs nothing now.
#if TARGET_OS_IPHONE
        CFDictionarySetValue(attributes, kCVPixelBufferOpenGLESCompatibilityKey, kCFBooleanTrue);
#else
        CFDictionarySetValue(attributes, kCVPixelBufferOpenGLCompatibilityKey, kCFBooleanTrue);
#endif
        // An empty IOSurface dictionary means "back this with an IOSurface,
        // defaults for everything" -- which is what makes it shareable with
        // the GPU at all. Released after the set, which retains it.
        CFDictionaryRef surface = CFDictionaryCreate(kCFAllocatorDefault, nullptr, nullptr, 0,
                                                     &kCFTypeDictionaryKeyCallBacks,
                                                     &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(attributes, kCVPixelBufferIOSurfacePropertiesKey, surface);
        CFRelease(surface);

        CFMutableDictionaryRef specification = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        // Ask for the hardware rather than accept whichever -- on a Mac, and
        // only on a Mac.
        //
        // A Mac has a software H.264 decoder to fall back to, so there is a
        // choice to express here. Asked rather than demanded, because refusing
        // the fallback would mean no picture at all on a machine whose decoder
        // is busy.
        //
        // iOS and tvOS have no such choice to express: VideoToolbox there is
        // the hardware and nothing else. The key is available to them as of
        // 17, which is what this deploys to, but available is not the same as
        // meaningful.
#if TARGET_OS_OSX
        CFDictionarySetValue(
            specification,
            kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
            kCFBooleanTrue);
#endif

        VTDecompressionOutputCallbackRecord callback {};
        callback.decompressionOutputCallback = &Impl::on_decoded;
        callback.decompressionOutputRefCon = this;

        status = VTDecompressionSessionCreate(kCFAllocatorDefault, format.get(), specification,
                                              attributes, &callback, &session);
        CFRelease(attributes);
        CFRelease(specification);
        if (status != noErr) {
            if (error != nullptr) {
                *error = "VideoToolbox would not open a decoder: " + os_status(status);
            }
            session = nullptr;
            return false;
        }

        // Latency over throughput, which is the whole point of this app. A
        // decoder left to its own devices may buffer to keep a steady rate.
        VTSessionSetProperty(session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);

        // And what was actually opened, because 26ms a frame for 1080p on an
        // A15 is not what its hardware decoder does, and there are two ways
        // to get a number like that without anything looking wrong:
        //
        //  - the decoder is not the hardware at all. VideoToolbox will fall
        //    back silently, and a silent fallback reads exactly like a slow
        //    machine.
        //  - the decoder is the hardware but is converting on the way out.
        //    This asks for full-range NV12 on purpose (see the attributes
        //    above); if the stream is video range, or if the hardware hands
        //    back video range regardless, something has to convert every
        //    frame, and that something is not free.
        //
        // Both are one query each, and neither can be guessed at from a
        // machine that has no VideoToolbox on it.
        CFBooleanRef hardware = nullptr;
        const bool asked =
            VTSessionCopyProperty(
                session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
                kCFAllocatorDefault, &hardware) == noErr &&
            hardware != nullptr;
        std::string how = "VideoToolbox, no FFmpeg in the path";
        if (asked) {
            how += CFBooleanGetValue(hardware) ? ", using the hardware decoder"
                                               : ", NOT using the hardware decoder";
            CFRelease(hardware);
        } else {
            how += ", which would not say whether it is the hardware";
        }
        log_info("video decoder: " + how);
        return true;
    }
};

VideoToolboxDecoder::VideoToolboxDecoder() = default;
VideoToolboxDecoder::~VideoToolboxDecoder() = default;

bool VideoToolboxDecoder::available() { return true; }

bool VideoToolboxDecoder::open(std::string* /*error*/) {
    impl_ = std::make_unique<Impl>();
    impl_->open = true;
    return true;
}

void VideoToolboxDecoder::close() { impl_.reset(); }

bool VideoToolboxDecoder::is_open() const { return impl_ != nullptr && impl_->open; }

bool VideoToolboxDecoder::needs_keyframe() const {
    return impl_ == nullptr || impl_->session == nullptr;
}

bool VideoToolboxDecoder::decode(const uint8_t* data, std::size_t size, DecodedFrame& out,
                                 std::string* error) {
    out = DecodedFrame();
    if (impl_ == nullptr || data == nullptr || size == 0) {
        return true;
    }
    Impl& impl = *impl_;

    // Parameter sets arrive inside a keyframe, and only a change is worth
    // acting on -- a stream repeats them on every keyframe and rebuilding the
    // session each time would drop the frames in flight through it.
    if (collect_parameter_sets(data, size, impl.sets) || impl.session == nullptr) {
        if (impl.sets.complete() && !impl.build_session(error)) {
            return false;
        }
    }
    if (impl.session == nullptr) {
        // Nothing to decode against yet. Not an error: everything before the
        // first keyframe is dropped, and the caller asks for one.
        return true;
    }

    if (to_length_prefixed(data, size, impl.avcc) == 0) {
        // Parameter sets and nothing else. Submitting that is how you get an
        // error rather than a frame.
        return true;
    }

    // The block buffer does not copy: it points at impl.avcc, which outlives
    // the synchronous decode below and is refilled on the next frame.
    Owned<CMBlockBufferRef> block;
    OSStatus status = CMBlockBufferCreateWithMemoryBlock(
        kCFAllocatorDefault, impl.avcc.data(), impl.avcc.size(), kCFAllocatorNull, nullptr, 0,
        impl.avcc.size(), 0, block.slot());
    if (status != noErr) {
        if (error != nullptr) *error = "could not wrap the frame: " + os_status(status);
        return false;
    }

    Owned<CMSampleBufferRef> sample;
    const std::size_t sample_size = impl.avcc.size();
    status = CMSampleBufferCreateReady(kCFAllocatorDefault, block.get(), impl.format.get(), 1, 0,
                                       nullptr, 1, &sample_size, sample.slot());
    if (status != noErr) {
        if (error != nullptr) *error = "could not make a sample buffer: " + os_status(status);
        return false;
    }

    impl.latest_status = noErr;
    impl.release_latest();
    // Synchronous: no kVTDecodeFrame_EnableAsynchronousDecompression, so the
    // callback has run by the time this returns. The stream has no B-frames
    // (the encoder sets max_b_frames to zero and AV_CODEC_FLAG_LOW_DELAY), so
    // there is no reordering to wait for and nothing to gain by going async
    // except a queue to sit in.
    VTDecodeInfoFlags produced = 0;
    status = VTDecompressionSessionDecodeFrame(impl.session, sample.get(), 0, nullptr, &produced);
    if (status != noErr) {
        if (error != nullptr) *error = "the frame would not decode: " + os_status(status);
        return false;
    }
    if (impl.latest_status != noErr) {
        if (error != nullptr) {
            *error = "the decoder rejected the frame: " + os_status(impl.latest_status);
        }
        return false;
    }
    if (impl.latest == nullptr) {
        return true;  // nothing yet, which is not a failure
    }

    // Out as NV12 planes, which is what the renderer wants: the conversion to
    // RGB happens in the fragment shader, and on a handheld that is the
    // difference between 22ms a frame and about 4.
    //
    // This copy is the one the whole exercise is meant to remove. The buffer
    // is already where the GPU can see it -- it was allocated GL-compatible
    // above -- so the next change hands it straight to a texture and touches
    // none of these bytes. See apple/README.md.
    CVPixelBufferRef picture = impl.latest;
    // Said once: what came back, against what was asked for. A decoder that
    // hands back a different format from the one requested is converting every
    // frame to do it.
    if (!impl.said_format) {
        impl.said_format = true;
        const OSType got = CVPixelBufferGetPixelFormatType(picture);
        const OSType wanted = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
        char got_name[5] = {static_cast<char>((got >> 24) & 0xff), static_cast<char>((got >> 16) & 0xff),
                            static_cast<char>((got >> 8) & 0xff), static_cast<char>(got & 0xff), 0};
        log_info(std::string("video decoder: frames arrive as '") + got_name + "', which is " +
                 (got == wanted ? "the format asked for"
                                : "NOT the full-range NV12 asked for -- something is converting "
                                  "every frame"));
    }
    const auto readback_began = std::chrono::steady_clock::now();
    const auto lock_began = readback_began;
    if (CVPixelBufferLockBaseAddress(picture, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        if (error != nullptr) *error = "the decoded frame would not lock";
        return false;
    }
    // How much of the "copying out" is the lock rather than the bytes. The
    // lock is where a CPU waits for a decoder that has not finished, and that
    // is a different problem from memory being slow to read.
    out.lock_micros = static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - lock_began)
                                           .count());
    out.layout = DecodedLayout::Nv12;
    out.width = static_cast<int>(CVPixelBufferGetWidth(picture));
    out.height = static_cast<int>(CVPixelBufferGetHeight(picture));
    for (int plane = 0; plane < 2; ++plane) {
        const auto* source =
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(picture, plane));
        const std::size_t stride = CVPixelBufferGetBytesPerRowOfPlane(picture, plane);
        const std::size_t rows = CVPixelBufferGetHeightOfPlane(picture, plane);
        if (source == nullptr || stride == 0 || rows == 0) {
            continue;
        }
        out.stride[plane] = static_cast<int>(stride);
        // assign() rather than resize() and memcpy(). resize() on an empty
        // vector value-initialises every byte, and every one of those bytes
        // is then overwritten -- at 1920x1080 that is 3MB of zeroes written
        // per frame for nothing. assign() from a pointer pair allocates once
        // and copies once.
        out.plane[plane].assign(source, source + stride * rows);
    }
    CVPixelBufferUnlockBaseAddress(picture, kCVPixelBufferLock_ReadOnly);
    impl.release_latest();
    out.readback_micros = static_cast<int>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - readback_began)
            .count());
    return true;
}

}  // namespace ds::video

#else  // not Apple

namespace ds::video {

struct VideoToolboxDecoder::Impl {};

VideoToolboxDecoder::VideoToolboxDecoder() = default;
VideoToolboxDecoder::~VideoToolboxDecoder() = default;

bool VideoToolboxDecoder::available() { return false; }

bool VideoToolboxDecoder::open(std::string* error) {
    if (error != nullptr) *error = "this build has no VideoToolbox";
    return false;
}

void VideoToolboxDecoder::close() {}
bool VideoToolboxDecoder::is_open() const { return false; }
bool VideoToolboxDecoder::needs_keyframe() const { return true; }

bool VideoToolboxDecoder::decode(const uint8_t*, std::size_t, DecodedFrame& out, std::string*) {
    out = DecodedFrame();
    return false;
}

}  // namespace ds::video

#endif
