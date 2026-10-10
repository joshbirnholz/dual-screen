#include "video/h264_bitstream.hpp"

#include <algorithm>
#include <cstring>

namespace ds::video {
namespace {

// Where the next start code begins, at or after `from`, or `size` if there is
// none left. A start code is 00 00 01, optionally with one more leading zero.
std::size_t next_start_code(const uint8_t* data, std::size_t size, std::size_t from,
                            std::size_t& code_length) {
    for (std::size_t index = from; index + 2 < size; ++index) {
        if (data[index] != 0 || data[index + 1] != 0 || data[index + 2] != 1) {
            continue;
        }
        // A fourth byte belongs to the start code when the byte before the
        // three is a zero -- which is how a four-byte code is spelled.
        if (index > from && data[index - 1] == 0) {
            code_length = 4;
            return index - 1;
        }
        code_length = 3;
        return index;
    }
    code_length = 0;
    return size;
}

}  // namespace

std::size_t find_nal_units(const uint8_t* data, std::size_t size, std::vector<NalUnit>& out) {
    if (data == nullptr || size < 4) {
        return 0;
    }
    const std::size_t before = out.size();

    std::size_t code_length = 0;
    std::size_t at = next_start_code(data, size, 0, code_length);
    while (at < size) {
        const std::size_t body = at + code_length;
        if (body >= size) {
            break;
        }
        std::size_t next_code_length = 0;
        const std::size_t next = next_start_code(data, size, body, next_code_length);

        NalUnit unit;
        unit.data = data + body;
        unit.size = next - body;
        // Trailing zeroes are never payload, so they come off.
        //
        // This is safe rather than a guess: a NAL unit's last byte carries the
        // rbsp_stop_one_bit, so a well-formed one cannot end in 0x00. Anything
        // that does is the zero padding a four-byte start code is spelled with,
        // or the cbr padding at the end of a buffer. Passing it on decodes
        // fine and makes every length disagree with what other tools report,
        // which costs an afternoon the first time somebody compares them.
        while (unit.size > 1 && unit.data[unit.size - 1] == 0) {
            --unit.size;
        }
        if (unit.size > 0) {
            unit.type = static_cast<uint8_t>(unit.data[0] & 0x1f);
            out.push_back(unit);
        }

        at = next;
        code_length = next_code_length;
    }
    return out.size() - before;
}

// Scratch for the helpers below, which are called once a frame and should not
// allocate to do it. Thread-local rather than a member because these are free
// functions and the alternative is making them methods of something.
namespace {
std::vector<NalUnit>& scratch_units() {
    static thread_local std::vector<NalUnit> units;
    units.clear();
    return units;
}
}  // namespace

bool collect_parameter_sets(const uint8_t* data, std::size_t size, ParameterSets& out) {
    std::vector<NalUnit>& units = scratch_units();
    find_nal_units(data, size, units);

    bool changed = false;
    for (const NalUnit& unit : units) {
        std::vector<uint8_t>* destination = nullptr;
        if (unit.is(NalType::SequenceParams)) {
            destination = &out.sps;
        } else if (unit.is(NalType::PictureParams)) {
            destination = &out.pps;
        }
        if (destination == nullptr) {
            continue;
        }
        // Only when it is actually different. A stream repeats its parameter
        // sets on every keyframe, and rebuilding the decode session for each
        // one would mean rebuilding it every couple of seconds for nothing --
        // and a rebuilt session drops the frames in flight through it.
        if (destination->size() == unit.size &&
            std::memcmp(destination->data(), unit.data, unit.size) == 0) {
            continue;
        }
        destination->assign(unit.data, unit.data + unit.size);
        changed = true;
    }
    return changed;
}

std::size_t to_length_prefixed(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out) {
    out.clear();
    std::vector<NalUnit>& units = scratch_units();
    find_nal_units(data, size, units);

    std::size_t written = 0;
    for (const NalUnit& unit : units) {
        // The parameter sets went into the format description the session was
        // built from, and an access unit delimiter says nothing a decoder
        // being fed one frame at a time does not already know.
        if (unit.is(NalType::SequenceParams) || unit.is(NalType::PictureParams) ||
            unit.is(NalType::AccessUnitDelimiter)) {
            continue;
        }
        const std::size_t length = unit.size;
        const std::size_t at = out.size();
        out.resize(at + 4 + length);
        // Four bytes, big-endian, which is what a format description created
        // with a NAL length of four promises the decoder it will find.
        out[at + 0] = static_cast<uint8_t>((length >> 24) & 0xff);
        out[at + 1] = static_cast<uint8_t>((length >> 16) & 0xff);
        out[at + 2] = static_cast<uint8_t>((length >> 8) & 0xff);
        out[at + 3] = static_cast<uint8_t>(length & 0xff);
        std::memcpy(out.data() + at + 4, unit.data, length);
        ++written;
    }
    return written;
}

namespace {

// Exp-Golomb, the variable-length code nearly every SPS field is written in,
// over a NAL unit with its emulation prevention bytes already taken out.
class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

    bool failed() const { return failed_; }

    uint32_t bit() {
        if (position_ >= bytes_.size() * 8) {
            failed_ = true;
            return 0;
        }
        const uint32_t value = (bytes_[position_ / 8] >> (7 - position_ % 8)) & 1u;
        ++position_;
        return value;
    }

    uint32_t bits(int count) {
        uint32_t value = 0;
        for (int index = 0; index < count; ++index) value = (value << 1) | bit();
        return value;
    }

    uint32_t unsigned_golomb() {
        int zeroes = 0;
        while (bit() == 0) {
            // 32 leading zeroes is no number a real SPS holds, and is how a
            // buffer of nothing but zero bytes would otherwise spin forever.
            if (failed_ || ++zeroes > 31) {
                failed_ = true;
                return 0;
            }
        }
        return ((1u << zeroes) - 1) + bits(zeroes);
    }

    int32_t signed_golomb() {
        const uint32_t code = unsigned_golomb();
        return (code & 1) ? static_cast<int32_t>((code + 1) / 2)
                          : -static_cast<int32_t>(code / 2);
    }

private:
    const std::vector<uint8_t>& bytes_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

void skip_scaling_list(BitReader& reader, int entries) {
    int32_t last = 8;
    int32_t next = 8;
    for (int index = 0; index < entries; ++index) {
        if (next != 0) {
            next = (last + reader.signed_golomb() + 256) % 256;
        }
        last = next == 0 ? last : next;
    }
}

}  // namespace

bool sps_picture_size(const uint8_t* sps, std::size_t size, int* width, int* height) {
    if (sps == nullptr || size < 4 || (sps[0] & 0x1f) != 7) {
        return false;
    }
    // The emulation prevention bytes come out first: 00 00 03 is 00 00 in
    // the real field values, and reading through the 03 shifts every field
    // after it.
    std::vector<uint8_t> rbsp;
    rbsp.reserve(size);
    int zeroes = 0;
    for (std::size_t index = 1; index < size; ++index) {
        if (zeroes >= 2 && sps[index] == 3) {
            zeroes = 0;
            continue;
        }
        zeroes = sps[index] == 0 ? zeroes + 1 : 0;
        rbsp.push_back(sps[index]);
    }

    BitReader reader(rbsp);
    const uint32_t profile = reader.bits(8);
    reader.bits(16);  // constraint flags, level
    reader.unsigned_golomb();  // seq_parameter_set_id

    uint32_t chroma_format = 1;
    bool separate_planes = false;
    switch (profile) {
        case 100: case 110: case 122: case 244: case 44:
        case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
            chroma_format = reader.unsigned_golomb();
            if (chroma_format == 3) separate_planes = reader.bit() != 0;
            reader.unsigned_golomb();  // bit_depth_luma_minus8
            reader.unsigned_golomb();  // bit_depth_chroma_minus8
            reader.bit();              // qpprime_y_zero_transform_bypass_flag
            if (reader.bit() != 0) {   // seq_scaling_matrix_present_flag
                const int lists = chroma_format == 3 ? 12 : 8;
                for (int list = 0; list < lists; ++list) {
                    if (reader.bit() != 0) skip_scaling_list(reader, list < 6 ? 16 : 64);
                }
            }
            break;
        default:
            break;
    }

    reader.unsigned_golomb();  // log2_max_frame_num_minus4
    const uint32_t poc_type = reader.unsigned_golomb();
    if (poc_type == 0) {
        reader.unsigned_golomb();  // log2_max_pic_order_cnt_lsb_minus4
    } else if (poc_type == 1) {
        reader.bit();
        reader.signed_golomb();
        reader.signed_golomb();
        const uint32_t cycle = reader.unsigned_golomb();
        if (cycle > 255) return false;
        for (uint32_t index = 0; index < cycle; ++index) reader.signed_golomb();
    }
    reader.unsigned_golomb();  // max_num_ref_frames
    reader.bit();              // gaps_in_frame_num_value_allowed_flag
    const uint32_t width_in_macroblocks = reader.unsigned_golomb() + 1;
    const uint32_t height_in_map_units = reader.unsigned_golomb() + 1;
    const uint32_t frames_only = reader.bit();
    if (frames_only == 0) reader.bit();  // mb_adaptive_frame_field_flag
    reader.bit();                         // direct_8x8_inference_flag

    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (reader.bit() != 0) {
        crop_left = reader.unsigned_golomb();
        crop_right = reader.unsigned_golomb();
        crop_top = reader.unsigned_golomb();
        crop_bottom = reader.unsigned_golomb();
    }
    if (reader.failed()) {
        return false;
    }

    // Cropping is counted in chroma samples, which are two luma pixels wide
    // in 4:2:0 and 4:2:2 and two tall in 4:2:0 -- and twice that again
    // vertically when the picture is coded as fields.
    const uint32_t array_type = separate_planes ? 0 : chroma_format;
    const uint32_t crop_unit_x = (array_type == 1 || array_type == 2) ? 2 : 1;
    const uint32_t crop_unit_y =
        ((array_type == 1) ? 2 : 1) * (frames_only != 0 ? 1 : 2);
    const int64_t coded_width = static_cast<int64_t>(width_in_macroblocks) * 16;
    const int64_t coded_height =
        static_cast<int64_t>(height_in_map_units) * 16 * (frames_only != 0 ? 1 : 2);
    const int64_t shown_width = coded_width - crop_unit_x * (int64_t{crop_left} + crop_right);
    const int64_t shown_height = coded_height - crop_unit_y * (int64_t{crop_top} + crop_bottom);
    if (shown_width <= 0 || shown_height <= 0 || shown_width > 16384 || shown_height > 16384) {
        return false;
    }
    if (width != nullptr) *width = static_cast<int>(shown_width);
    if (height != nullptr) *height = static_cast<int>(shown_height);
    return true;
}

bool contains_keyframe(const uint8_t* data, std::size_t size) {
    std::vector<NalUnit>& units = scratch_units();
    find_nal_units(data, size, units);
    for (const NalUnit& unit : units) {
        if (unit.is(NalType::IdrSlice)) {
            return true;
        }
    }
    return false;
}

}  // namespace ds::video
