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
