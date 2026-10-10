#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ds::video {

// The part of decoding H.264 that is not decoding H.264.
//
// Apple's decoder is VideoToolbox, and on every Apple platform it is already
// what does the work -- FFmpeg's h264 decoder with a VideoToolbox device
// attached is a wrapper around it. What FFmpeg actually supplies is this:
// finding where one NAL unit ends and the next begins, pulling the parameter
// sets out, and handing the rest over in the shape the decoder wants.
//
// It is a few hundred lines rather than a dependency, and unlike the decoder
// itself it is arithmetic over a byte array -- so it can be written and checked
// on a machine with no Apple hardware anywhere near it, which is where this was
// written. test_h264_bitstream runs it against frames from this app's own
// encoder.
//
// Two shapes of the same data, which is the whole of the conversion:
//
//   Annex B   00 00 00 01 <nal> 00 00 01 <nal> ...   what the encoder emits
//   AVCC      <4-byte length> <nal> <4-byte length> <nal> ...   what VideoToolbox takes
//
// Emulation prevention bytes are left exactly as they are. A NAL unit is
// handed to VideoToolbox still escaped -- unescaping it is the decoder's job,
// not this one's, and doing it here would corrupt every frame.

// Which kind of NAL unit, in the five values this app can encounter. The
// numbers are H.264's own (Table 7-1), not this app's.
enum class NalType : uint8_t {
    NonIdrSlice = 1,   // an ordinary frame
    IdrSlice = 5,      // a keyframe
    Sei = 6,           // timing and the like; harmless to pass on
    SequenceParams = 7,  // SPS
    PictureParams = 8,   // PPS
    AccessUnitDelimiter = 9,
};

// One NAL unit, pointing into the buffer it was found in rather than owning
// anything. Valid only as long as that buffer is.
struct NalUnit {
    const uint8_t* data = nullptr;  // the NAL itself, start code already removed
    std::size_t size = 0;
    uint8_t type = 0;  // the low five bits of the first byte

    bool is(NalType wanted) const { return type == static_cast<uint8_t>(wanted); }
    // The ones that carry picture data, which are the ones a decoder is fed.
    bool is_picture() const { return type == 1 || type == 5; }
};

// Splits an Annex B buffer into its NAL units.
//
// Start codes are three bytes or four; both appear in the same stream, and
// which one is used where is the encoder's business. A trailing zero before
// the next start code belongs to that start code rather than to the NAL, and
// is trimmed -- passing it on is harmless to a decoder and makes the lengths
// disagree with what every other tool reports, which costs an afternoon the
// first time somebody compares them.
//
// Appends to `out` rather than clearing it, so one vector can be reused across
// frames without reallocating. Returns how many were found.
std::size_t find_nal_units(const uint8_t* data, std::size_t size, std::vector<NalUnit>& out);

// The parameter sets a decoder has to be configured with before it will take
// any picture at all.
//
// Kept as copies, because they arrive inside a keyframe and have to outlive
// it: every later frame is decoded against them, and the buffer they came in
// is long gone by then.
struct ParameterSets {
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    bool complete() const { return !sps.empty() && !pps.empty(); }
    void clear() {
        sps.clear();
        pps.clear();
    }
};

// Takes any SPS and PPS out of an Annex B buffer and into `out`.
//
// Returns true when `out` changed -- which is the question the caller actually
// has, because a VideoToolbox session is built from these and has to be
// rebuilt when they change. A stream that repeats identical parameter sets on
// every keyframe, which is the usual arrangement and is what this app's
// encoder does, must not rebuild the session sixty times a second.
bool collect_parameter_sets(const uint8_t* data, std::size_t size, ParameterSets& out);

// Rewrites an Annex B buffer as length-prefixed NAL units, which is what a
// CMBlockBuffer holds.
//
// Parameter sets and access unit delimiters are dropped: the first two are
// already in the format description the session was built from, and passing
// them again in the sample data is at best redundant. SEI and the picture NALs
// go through.
//
// `out` is cleared and refilled, and keeps its capacity, so the steady state
// allocates nothing. Returns the number of NAL units written, which is zero
// for a buffer with no picture in it -- a caller should not submit that.
std::size_t to_length_prefixed(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out);

// The size of the picture an SPS describes, after its cropping -- 1280x720
// for a stream coded as 1280x736 with eight rows cropped off the bottom.
//
// What MediaCodec has to be told before it will take a single frame: a
// decoder there is configured with a size up front, and only learns the real
// one from the stream afterwards. Told the wrong one, some decoders allocate
// for it and then reconfigure, and some simply refuse.
//
// `sps` is one SPS NAL unit as find_nal_units() returns it -- the header byte
// included, start code not, emulation prevention bytes still in. False for
// anything it cannot read, with the outputs untouched.
bool sps_picture_size(const uint8_t* sps, std::size_t size, int* width, int* height);

// Whether a buffer contains a keyframe, which is what tells a decoder it may
// start. Cheaper than it looks: it stops at the first picture NAL.
bool contains_keyframe(const uint8_t* data, std::size_t size);

}  // namespace ds::video
