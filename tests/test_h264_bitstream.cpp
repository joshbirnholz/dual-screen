// The bitstream handling a VideoToolbox decoder needs, checked on Linux.
//
// The point of this file: Apple's decoder is VideoToolbox, and on Apple
// platforms it is already what does the decoding -- FFmpeg with a VideoToolbox
// device attached is a wrapper around it. What FFmpeg genuinely supplies is
// splitting NAL units, pulling the parameter sets out, and re-framing the rest
// the way the decoder wants it.
//
// That part is arithmetic over a byte array. It has no Apple in it at all, so
// it can be written and checked from a machine with none -- which is the whole
// reason the VideoToolbox port is worth attempting from here. The session
// wiring that follows is thin; this is the part with the bugs in it.
//
// The second half of the file runs it over frames from this app's own encoder,
// which is the only way to know the shape assumed here is the shape produced.

#include <cstring>
#include <string>
#include <vector>

#include "test_support.hpp"
#include "video/h264_bitstream.hpp"
#include "video/video_codec.hpp"
#include "video/video_encoder.hpp"

using namespace ds;
using namespace ds::video;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> values) {
    std::vector<uint8_t> out;
    out.reserve(values.size());
    for (int value : values) out.push_back(static_cast<uint8_t>(value));
    return out;
}

uint32_t read_be32(const std::vector<uint8_t>& buffer, std::size_t at) {
    return (static_cast<uint32_t>(buffer[at]) << 24) |
           (static_cast<uint32_t>(buffer[at + 1]) << 16) |
           (static_cast<uint32_t>(buffer[at + 2]) << 8) |
           static_cast<uint32_t>(buffer[at + 3]);
}

// Both spellings of a start code appear in one stream, and which is used where
// is the encoder's business rather than anything to rely on.
void test_both_lengths_of_start_code_are_found() {
    // 4-byte, then 3-byte, then 4-byte.
    const std::vector<uint8_t> stream = bytes({
        0, 0, 0, 1, 0x67, 0xaa, 0xbb,     // SPS
        0, 0, 1, 0x68, 0xcc,              // PPS
        0, 0, 0, 1, 0x65, 0x11, 0x22,     // IDR
    });
    std::vector<NalUnit> units;
    CHECK_EQ(find_nal_units(stream.data(), stream.size(), units), static_cast<std::size_t>(3));
    CHECK_EQ(units[0].type, static_cast<uint8_t>(7));
    CHECK_EQ(units[0].size, static_cast<std::size_t>(3));
    CHECK_EQ(units[1].type, static_cast<uint8_t>(8));
    CHECK_EQ(units[1].size, static_cast<std::size_t>(2));
    CHECK_EQ(units[2].type, static_cast<uint8_t>(5));
    CHECK_EQ(units[2].size, static_cast<std::size_t>(3));
    CHECK(units[2].is_picture());
    CHECK(units[0].is(NalType::SequenceParams));
}

// A NAL unit's last byte carries the rbsp_stop_one_bit, so a well-formed one
// never ends in zero. Anything that does is the padding a four-byte start code
// is spelled with, or the padding at the end of a buffer, and it is not
// payload.
void test_padding_before_a_start_code_is_not_part_of_the_nal() {
    const std::vector<uint8_t> stream = bytes({
        0, 0, 0, 1, 0x65, 0x11, 0x22, 0,  // that trailing 0 is the next start code's
        0, 0, 1, 0x41, 0x33,
        0, 0, 0,                          // and this is padding at the end
    });
    std::vector<NalUnit> units;
    CHECK_EQ(find_nal_units(stream.data(), stream.size(), units), static_cast<std::size_t>(2));
    CHECK_EQ(units[0].size, static_cast<std::size_t>(3));
    CHECK_EQ(units[1].size, static_cast<std::size_t>(2));
}

// A zero *inside* a NAL is data and has to survive. Emulation prevention is
// what stops three of them looking like a start code, and unescaping it is the
// decoder's job -- doing it here would corrupt every frame.
void test_zeroes_inside_a_nal_are_left_alone() {
    const std::vector<uint8_t> stream = bytes({
        0, 0, 0, 1, 0x65, 0x00, 0x00, 0x03, 0x01, 0x99,
    });
    std::vector<NalUnit> units;
    CHECK_EQ(find_nal_units(stream.data(), stream.size(), units), static_cast<std::size_t>(1));
    CHECK_EQ(units[0].size, static_cast<std::size_t>(6));
    // The escape byte is still there.
    CHECK_EQ(units[0].data[3], static_cast<uint8_t>(0x03));
}

// Rebuilding a decode session drops the frames in flight through it, and a
// stream repeats its parameter sets on every keyframe -- so "did these change"
// has to mean changed, not merely "were they present again".
void test_repeated_parameter_sets_are_not_a_change() {
    const std::vector<uint8_t> keyframe = bytes({
        0, 0, 0, 1, 0x67, 0xaa, 0xbb,
        0, 0, 0, 1, 0x68, 0xcc,
        0, 0, 0, 1, 0x65, 0x11,
    });
    ParameterSets sets;
    CHECK(collect_parameter_sets(keyframe.data(), keyframe.size(), sets));
    CHECK(sets.complete());
    CHECK_EQ(sets.sps.size(), static_cast<std::size_t>(3));
    CHECK_EQ(sets.pps.size(), static_cast<std::size_t>(2));

    // The very same keyframe again: nothing changed, so nothing is rebuilt.
    CHECK(!collect_parameter_sets(keyframe.data(), keyframe.size(), sets));

    // A different SPS is a change, and only the SPS moves.
    const std::vector<uint8_t> resized = bytes({
        0, 0, 0, 1, 0x67, 0xaa, 0xdd,
        0, 0, 0, 1, 0x68, 0xcc,
        0, 0, 0, 1, 0x65, 0x11,
    });
    CHECK(collect_parameter_sets(resized.data(), resized.size(), sets));
    CHECK_EQ(sets.sps[2], static_cast<uint8_t>(0xdd));
    CHECK_EQ(sets.pps.size(), static_cast<std::size_t>(2));

    // And a frame with no parameter sets in it at all leaves them standing --
    // every frame after a keyframe is decoded against the ones it carried.
    const std::vector<uint8_t> ordinary = bytes({0, 0, 0, 1, 0x41, 0x77});
    CHECK(!collect_parameter_sets(ordinary.data(), ordinary.size(), sets));
    CHECK(sets.complete());
}

// The conversion itself: four-byte big-endian lengths, parameter sets left out
// because they are already in the format description the session was built
// from.
void test_length_prefixing_drops_what_the_session_already_has() {
    const std::vector<uint8_t> keyframe = bytes({
        0, 0, 0, 1, 0x09, 0x10,            // access unit delimiter, dropped
        0, 0, 0, 1, 0x67, 0xaa, 0xbb,      // SPS, dropped
        0, 0, 0, 1, 0x68, 0xcc,            // PPS, dropped
        0, 0, 0, 1, 0x06, 0x05, 0x01,      // SEI, kept
        0, 0, 0, 1, 0x65, 0x11, 0x22, 0x33 // IDR, kept
    });
    std::vector<uint8_t> avcc;
    CHECK_EQ(to_length_prefixed(keyframe.data(), keyframe.size(), avcc),
             static_cast<std::size_t>(2));

    // SEI first: 3 bytes.
    CHECK_EQ(read_be32(avcc, 0), 3u);
    CHECK_EQ(avcc[4], static_cast<uint8_t>(0x06));
    // Then the picture: 4 bytes, at 4 + 3.
    CHECK_EQ(read_be32(avcc, 7), 4u);
    CHECK_EQ(avcc[11], static_cast<uint8_t>(0x65));
    CHECK_EQ(avcc.size(), static_cast<std::size_t>(4 + 3 + 4 + 4));

    // A buffer with nothing but parameter sets has no picture in it, and
    // submitting that to a decoder is how you get an error rather than a
    // frame.
    const std::vector<uint8_t> headers_only = bytes({
        0, 0, 0, 1, 0x67, 0xaa,
        0, 0, 0, 1, 0x68, 0xcc,
    });
    CHECK_EQ(to_length_prefixed(headers_only.data(), headers_only.size(), avcc),
             static_cast<std::size_t>(0));
    CHECK(avcc.empty());
}

void test_a_keyframe_is_told_from_an_ordinary_frame() {
    const std::vector<uint8_t> keyframe = bytes({0, 0, 0, 1, 0x65, 0x11});
    const std::vector<uint8_t> ordinary = bytes({0, 0, 0, 1, 0x41, 0x11});
    CHECK(contains_keyframe(keyframe.data(), keyframe.size()));
    CHECK(!contains_keyframe(ordinary.data(), ordinary.size()));
}

// Nothing here may read off the end of a buffer, whatever it is handed. A
// stream arrives over a network and a truncated frame is a thing that happens.
void test_rubbish_in_is_not_a_crash() {
    std::vector<NalUnit> units;
    CHECK_EQ(find_nal_units(nullptr, 0, units), static_cast<std::size_t>(0));
    const std::vector<uint8_t> tiny = bytes({0, 0, 0});
    CHECK_EQ(find_nal_units(tiny.data(), tiny.size(), units), static_cast<std::size_t>(0));
    // A start code and nothing after it.
    const std::vector<uint8_t> empty_nal = bytes({0, 0, 0, 1});
    CHECK_EQ(find_nal_units(empty_nal.data(), empty_nal.size(), units),
             static_cast<std::size_t>(0));
    // Zeroes all the way down.
    const std::vector<uint8_t> zeroes(64, 0);
    CHECK_EQ(find_nal_units(zeroes.data(), zeroes.size(), units), static_cast<std::size_t>(0));
    std::vector<uint8_t> avcc;
    CHECK_EQ(to_length_prefixed(zeroes.data(), zeroes.size(), avcc), static_cast<std::size_t>(0));
    CHECK(!contains_keyframe(zeroes.data(), zeroes.size()));
}

// And the half that matters: the shape assumed above is the shape this app's
// encoder actually emits. Everything before this is a test of a parser against
// bytes chosen to suit it.
void test_real_encoder_output_parses_and_converts() {
    EncoderConfig config;
    config.width = 256;
    config.height = 192;
    config.fps = 30;
    config.bitrate_kbps = 2000;
    config.backends = {"libx264"};

    VideoEncoder encoder;
    std::string error;
    if (!encoder.open(config, &error)) {
        test::skip("no H.264 encoder here, so there was no real bitstream to parse");
        return;
    }

    std::vector<uint32_t> picture(static_cast<std::size_t>(config.width) * config.height,
                                 0xff2060a0u);
    ParameterSets sets;
    std::vector<uint8_t> avcc;
    int keyframes = 0;
    int pictures = 0;

    for (int index = 0; index < 8; ++index) {
        // Something different every frame, so the encoder has work to do and
        // does not emit a run of identical skip frames.
        picture[static_cast<std::size_t>(index) * 37 % picture.size()] = 0xff000000u;
        EncodedFrame encoded;
        if (!encoder.encode(picture.data(), config.width * 4, encoded, &error) ||
            encoded.bytes.empty()) {
            continue;
        }
        const uint8_t* data = encoded.bytes.data();
        const std::size_t size = encoded.bytes.size();

        // Every frame the encoder produces is Annex B and splits into NALs.
        std::vector<NalUnit> units;
        CHECK_MSG(find_nal_units(data, size, units) > 0,
                  "a frame from this app's own encoder split into no NAL units at all");

        if (contains_keyframe(data, size)) {
            ++keyframes;
            // A keyframe carries what a decoder needs to be configured with.
            collect_parameter_sets(data, size, sets);
            CHECK_MSG(sets.complete(),
                      "a keyframe carried no SPS/PPS, so a VideoToolbox session could not "
                      "be built from it");
        }

        // The conversion produces at least one picture NAL, and the lengths it
        // writes have to add up to exactly the buffer -- which is the property
        // a decoder relies on and the one a bug here breaks.
        const std::size_t written = to_length_prefixed(data, size, avcc);
        if (written == 0) {
            continue;
        }
        ++pictures;
        std::size_t at = 0;
        std::size_t counted = 0;
        while (at + 4 <= avcc.size()) {
            const uint32_t length = read_be32(avcc, at);
            CHECK_MSG(length > 0, "a zero-length NAL was written");
            at += 4 + length;
            ++counted;
        }
        CHECK_MSG(at == avcc.size(),
                  "the lengths written do not add up to the buffer, so a decoder would read "
                  "off the end of it");
        CHECK_EQ(counted, written);
    }

    CHECK_MSG(keyframes > 0, "the encoder produced no keyframe in eight frames");
    CHECK_MSG(pictures > 0, "no frame converted to anything a decoder could be given");
    // The parameter sets are the thing that has to survive the keyframe they
    // arrived in, so check they are still intact at the end.
    CHECK(sets.complete());
}

}  // namespace

int main() {
    test_both_lengths_of_start_code_are_found();
    test_padding_before_a_start_code_is_not_part_of_the_nal();
    test_zeroes_inside_a_nal_are_left_alone();
    test_repeated_parameter_sets_are_not_a_change();
    test_length_prefixing_drops_what_the_session_already_has();
    test_a_keyframe_is_told_from_an_ordinary_frame();
    test_rubbish_in_is_not_a_crash();
    test_real_encoder_output_parses_and_converts();
    return test::finish("h264-bitstream");
}
