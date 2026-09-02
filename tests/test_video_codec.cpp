// The compressed path a Wii U GamePad needs, end to end: frames in one side,
// the same picture out the other.
//
// What is actually being defended here is latency rather than fidelity. A video
// encoder left to its own devices buffers -- B-frames, lookahead, a rate control
// window -- and every one of those is a frame that cannot be sent until a later
// one has been encoded. On this path that is a frame of input lag bought to save
// bytes the link has plenty of. So the test that matters is "one frame in, one
// packet out, every single time", and it is the first thing below.

#include "test_support.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "video/video_decoder.hpp"
#include "video/video_encoder.hpp"

using namespace ds;

namespace {

constexpr int kWidth = 854;   // a Wii U GamePad, which is what this is for
constexpr int kHeight = 480;
constexpr int kFps = 60;

// Moving content with enough detail to actually cost something. A flat frame
// would encode to nothing and prove nothing.
void draw(std::vector<uint32_t>& out, int width, int height, int frame) {
    out.resize(static_cast<std::size_t>(width) * height);
    const float shift = static_cast<float>(frame) * 3.0f;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float fx = static_cast<float>(x) + shift;
            const int r = static_cast<int>(90.0f + 80.0f * std::sin(fx * 0.02f));
            const int g = static_cast<int>(110.0f + 70.0f * std::sin(static_cast<float>(y) * 0.03f));
            const int b = (((x + frame * 3) / 24 + y / 24) % 2) == 0 ? 60 : 190;
            out[static_cast<std::size_t>(y) * width + x] =
                (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) |
                static_cast<uint32_t>(b);
        }
    }
}

// Mean absolute difference per channel. Lossy compression is expected to move
// these; losing the picture entirely is not.
double difference(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255.0;
    double total = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (int shift = 0; shift <= 16; shift += 8) {
            const int lhs = static_cast<int>((a[i] >> shift) & 0xFFu);
            const int rhs = static_cast<int>((b[i] >> shift) & 0xFFu);
            total += std::abs(lhs - rhs);
        }
    }
    return total / static_cast<double>(a.size() * 3);
}

// Software, deliberately: this machine has no GPU and the point is the pipeline
// rather than which chip ran it. A host picks hardware first -- see
// EncoderConfig's defaults.
video::EncoderConfig software_config() {
    video::EncoderConfig config;
    config.width = kWidth;
    config.height = kHeight;
    config.fps = kFps;
    config.bitrate_kbps = 4000;
    config.backends = {"libx264", "h264_v4l2m2m"};
    return config;
}

void test_every_frame_comes_straight_back_out() {
    video::VideoEncoder encoder;
    std::string error;
    CHECK_MSG(encoder.open(software_config(), &error), "no encoder to test with: " + error);

    std::vector<uint32_t> frame;
    int silent = 0;
    for (int index = 0; index < 30; ++index) {
        draw(frame, kWidth, kHeight, index);
        video::EncodedFrame encoded;
        CHECK_MSG(encoder.encode(frame.data(), kWidth, encoded, &error),
                  "the encoder failed on frame " + std::to_string(index) + ": " + error);
        if (encoded.empty()) ++silent;
    }
    CHECK_MSG(silent == 0,
              std::to_string(silent) +
                  " of 30 frames produced no packet, so the encoder is holding frames back and "
                  "every one of them is a frame of lag");
}

void test_the_first_frame_stands_alone_and_more_can_be_asked_for() {
    video::VideoEncoder encoder;
    std::string error;
    CHECK(encoder.open(software_config(), &error));

    std::vector<uint32_t> frame;
    draw(frame, kWidth, kHeight, 0);
    video::EncodedFrame first;
    CHECK(encoder.encode(frame.data(), kWidth, first, &error));
    CHECK_MSG(first.keyframe,
              "the first frame of a stream was not a keyframe, so a device joining has nothing to "
              "start from");

    // Several ordinary frames, then one asked for. A device that joins mid
    // stream cannot wait for the next scheduled keyframe -- that is seconds of
    // black -- so asking has to work.
    bool saw_delta = false;
    for (int index = 1; index < 10; ++index) {
        draw(frame, kWidth, kHeight, index);
        video::EncodedFrame encoded;
        CHECK(encoder.encode(frame.data(), kWidth, encoded, &error));
        if (!encoded.keyframe) saw_delta = true;
    }
    CHECK_MSG(saw_delta, "every frame came out whole, so nothing is being saved by sending deltas");

    encoder.request_keyframe();
    draw(frame, kWidth, kHeight, 10);
    video::EncodedFrame asked;
    CHECK(encoder.encode(frame.data(), kWidth, asked, &error));
    CHECK_MSG(asked.keyframe,
              "asking for a keyframe did not produce one, so a device joining mid-session would "
              "wait out the whole keyframe interval");
}

void test_the_picture_survives_the_round_trip() {
    video::VideoEncoder encoder;
    video::VideoDecoder decoder;
    std::string error;
    CHECK(encoder.open(software_config(), &error));
    video::DecoderConfig decoder_config;
    CHECK_MSG(decoder.open(decoder_config, &error), "no decoder to test with: " + error);

    std::vector<uint32_t> sent;
    video::DecodedFrame received;
    std::size_t total_bytes = 0;
    int decoded_frames = 0;
    double worst = 0.0;

    constexpr int kFrames = 30;
    for (int index = 0; index < kFrames; ++index) {
        draw(sent, kWidth, kHeight, index);
        video::EncodedFrame encoded;
        CHECK(encoder.encode(sent.data(), kWidth, encoded, &error));
        total_bytes += encoded.bytes.size();

        CHECK_MSG(decoder.decode(encoded.bytes.data(), encoded.bytes.size(), received, &error),
                  "the decoder failed on frame " + std::to_string(index) + ": " + error);
        if (!received.valid()) {
            continue;  // nothing out yet, which the first packet or two may do
        }
        ++decoded_frames;
        CHECK_EQ(received.width, kWidth);
        CHECK_EQ(received.height, kHeight);
        worst = std::max(worst, difference(sent, received.pixels));
    }

    CHECK_MSG(decoded_frames >= kFrames - 1,
              "only " + std::to_string(decoded_frames) + " of " + std::to_string(kFrames) +
                  " frames came back out, so the pipeline is holding frames somewhere");
    // Generous: this is lossy compression of a deliberately busy pattern, and
    // the check is that a picture arrived rather than that it is pristine. A
    // stream that broke down entirely lands far above this.
    CHECK_MSG(worst < 24.0,
              "the decoded picture was off by " + std::to_string(static_cast<int>(worst)) +
                  " per channel, which is not the frame that was sent");

    // Not an assertion, a record: the whole reason this path exists is that the
    // tile codec measured at 178 KB a frame on content like this.
    test::note("h264 at " + std::to_string(kWidth) + "x" + std::to_string(kHeight) + ": " +
               std::to_string(total_bytes / kFrames) + " bytes/frame, " +
               std::to_string(static_cast<int>(static_cast<double>(total_bytes) /
                                               kFrames * 8.0 * kFps / 1e6)) +
               " Mbit/s at " + std::to_string(kFps) + "fps");
}

// HostSession::try_encode_video downsizes a screen larger than the
// companion's own display before it is ever encoded -- see
// compute_capped_encode_size() in host_session.cpp -- by leaving
// EncoderConfig::width/height smaller than the source pixels it is actually
// handed. Exercised here at the VideoEncoder level, since that is where the
// scaler that has to make it happen lives.
void test_encoding_can_downscale_from_the_source() {
    // kWidth (854) is odd, which is fine for a source -- only what actually
    // gets encoded has to land on an even number of pixels, same as ever. A
    // multiple of 16 specifically, here: this app's own
    // compute_capped_encode_size() (host_session.cpp) always rounds a capped
    // size down to one, and not just for the usual 4:2:0 reason -- a small,
    // non-macroblock-aligned width has been observed to corrupt memory
    // outright encoding through libx264 on some builds, rather than merely
    // losing the partial edge macroblock the way a hardware encoder does.
    constexpr int kEncodeWidth = 432;
    constexpr int kEncodeHeight = 240;
    video::VideoEncoder encoder;
    video::EncoderConfig config = software_config();
    config.source_width = kWidth;
    config.source_height = kHeight;
    config.width = kEncodeWidth;
    config.height = kEncodeHeight;
    std::string error;
    CHECK_MSG(encoder.open(config, &error), "a downscaling config was refused: " + error);
    CHECK_EQ(encoder.width(), kEncodeWidth);
    CHECK_EQ(encoder.height(), kEncodeHeight);
    CHECK_EQ(encoder.source_width(), kWidth);
    CHECK_EQ(encoder.source_height(), kHeight);

    video::VideoDecoder decoder;
    video::DecoderConfig decoder_config;
    CHECK_MSG(decoder.open(decoder_config, &error), "no decoder to test with: " + error);

    std::vector<uint32_t> source;
    video::DecodedFrame received;
    int decoded_frames = 0;
    for (int index = 0; index < 5; ++index) {
        // Full source-sized pixels in, at the source's own stride -- exactly
        // what a screen wider than the companion's display hands over.
        draw(source, kWidth, kHeight, index);
        video::EncodedFrame encoded;
        CHECK_MSG(encoder.encode(source.data(), kWidth, encoded, &error),
                  "the encoder failed on a downscaled frame: " + error);
        if (encoded.empty()) continue;
        CHECK_MSG(decoder.decode(encoded.bytes.data(), encoded.bytes.size(), received, &error),
                  "the decoder failed on a downscaled frame: " + error);
        if (!received.valid()) continue;
        ++decoded_frames;
        // What actually left the encoder is what matters here -- the whole
        // point is that this is smaller than the source, not a copy of it.
        CHECK_EQ(received.width, kEncodeWidth);
        CHECK_EQ(received.height, kEncodeHeight);
    }
    CHECK_MSG(decoded_frames > 0, "no downscaled frame ever made it through decoding");
}

void test_a_screen_that_cannot_be_encoded_says_so() {
    video::VideoEncoder encoder;
    video::EncoderConfig config = software_config();
    // 4:2:0 has no way to represent an odd number of pixels, and an encoder
    // asked for one fails somewhere far less legible than here.
    config.width = 855;
    std::string error;
    CHECK_MSG(!encoder.open(config, &error), "an odd-width screen was accepted");
    CHECK_MSG(error.find("even") != std::string::npos,
              "an odd-width screen was refused without saying why: " + error);

    config = software_config();
    config.backends = {"no_such_encoder"};
    CHECK_MSG(!encoder.open(config, &error), "a backend that does not exist opened");
    CHECK_MSG(!encoder.is_open(), "a failed open left the encoder half-open");
}

// A device-attached backend -- VAAPI on a desktop, NVTEGRA on a Switch -- is
// the plain "h264" decoder with a hardware device context attached; FFmpeg
// registers no decoder literally named "h264_vaapi" or "h264_nvtegra". So a
// build with no such device (this machine has no VAAPI-capable render node)
// still has to fail for the *right* reason: the decoder was found and it was
// the device that would not create, not "no such decoder" -- which is what a
// build that still asked avcodec_find_decoder_by_name() for the label itself
// would say instead, wrongly, since "h264" always exists once video::available()
// is true at all.
void test_a_device_attached_backend_fails_on_the_device_not_the_decoder() {
    video::VideoDecoder decoder;
    video::DecoderConfig config;
    config.backends = {"h264_vaapi"};
    std::string error;
    const bool opened = decoder.open(config, &error);
    // Opens outright on a machine that does have a VAAPI device -- either
    // answer is a pass, since this is exercising which failure a build
    // without a real device gets, not asserting this sandbox lacks one.
    if (!opened) {
        CHECK_MSG(error.find("no such decoder") == std::string::npos,
                  "h264_vaapi was rejected as an unknown decoder rather than an unavailable "
                  "device: " +
                      error);
    }
}

}  // namespace

int main() {
    if (!video::available()) {
        // Not a failure: the codec is optional until every platform's toolchain
        // has it, and saying so is more use than a green tick that tested air.
        test::skip("this build has no video codec, so there was nothing to test");
        return test::finish("video-codec");
    }
    test_every_frame_comes_straight_back_out();
    test_the_first_frame_stands_alone_and_more_can_be_asked_for();
    test_the_picture_survives_the_round_trip();
    test_encoding_can_downscale_from_the_source();
    test_a_screen_that_cannot_be_encoded_says_so();
    test_a_device_attached_backend_fails_on_the_device_not_the_decoder();
    return test::finish("video-codec");
}
