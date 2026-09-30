#include "net/protocol.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>

namespace ds::net {
namespace {

void put_u8(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
    }
}

// A float goes on the wire as its own IEEE-754 bits, little-endian like every
// other number here. Exact rather than scaled into a fixed-point integer,
// because what these carry (see DsMotionState) is a raw sensor reading whose
// useful range and precision are the sensor's business, not this file's --
// and every machine this builds for stores a float this way already.
void put_f32(std::vector<uint8_t>& out, float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "a float is not four bytes here");
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

void put_string(std::vector<uint8_t>& out, const std::string& value) {
    // Bounded so a hostile or corrupt sender cannot make us allocate wildly.
    const uint16_t length = static_cast<uint16_t>(std::min<std::size_t>(value.size(), 0xffffu));
    put_u16(out, length);
    out.insert(out.end(), value.begin(), value.begin() + length);
}

// A cursor that refuses to read past the end, so every decode is bounds-checked
// without repeating the check at each field.
class Reader {
public:
    Reader(const uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool u8(uint8_t& out) {
        if (position_ + 1 > size_) return fail();
        out = data_[position_++];
        return true;
    }
    bool u16(uint16_t& out) {
        if (position_ + 2 > size_) return fail();
        out = static_cast<uint16_t>(data_[position_] | (data_[position_ + 1] << 8));
        position_ += 2;
        return true;
    }
    bool f32(float& out) {
        uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }
    bool u32(uint32_t& out) {
        if (position_ + 4 > size_) return fail();
        out = 0;
        for (int index = 0; index < 4; ++index) {
            out |= static_cast<uint32_t>(data_[position_ + index]) << (index * 8);
        }
        position_ += 4;
        return true;
    }
    bool u64(uint64_t& out) {
        if (position_ + 8 > size_) return fail();
        out = 0;
        for (int index = 0; index < 8; ++index) {
            out |= static_cast<uint64_t>(data_[position_ + index]) << (index * 8);
        }
        position_ += 8;
        return true;
    }
    bool string(std::string& out) {
        uint16_t length = 0;
        if (!u16(length)) return false;
        if (position_ + length > size_) return fail();
        out.assign(reinterpret_cast<const char*>(data_ + position_), length);
        position_ += length;
        return true;
    }
    bool bytes(std::vector<uint8_t>& out, std::size_t count) {
        if (position_ + count > size_) return fail();
        out.assign(data_ + position_, data_ + position_ + count);
        position_ += count;
        return true;
    }
    std::size_t remaining() const { return size_ - position_; }
    bool ok() const { return ok_; }

private:
    bool fail() {
        ok_ = false;
        return false;
    }

    const uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

void finish_message(std::vector<uint8_t>& out, std::size_t header_start, MessageType type) {
    const std::size_t payload_size = out.size() - header_start - 6;
    out[header_start + 0] = static_cast<uint8_t>(payload_size & 0xffu);
    out[header_start + 1] = static_cast<uint8_t>((payload_size >> 8) & 0xffu);
    out[header_start + 2] = static_cast<uint8_t>((payload_size >> 16) & 0xffu);
    out[header_start + 3] = static_cast<uint8_t>((payload_size >> 24) & 0xffu);
    const uint16_t raw_type = static_cast<uint16_t>(type);
    out[header_start + 4] = static_cast<uint8_t>(raw_type & 0xffu);
    out[header_start + 5] = static_cast<uint8_t>((raw_type >> 8) & 0xffu);
}

std::size_t begin_message(std::vector<uint8_t>& out) {
    const std::size_t start = out.size();
    out.resize(start + 6, 0);  // length + type, filled in by finish_message
    return start;
}

uint8_t screen_to_wire(ScreenId screen) { return screen == ScreenId::Top ? 0u : 1u; }

// The console, as a number that means the same thing at both ends. A switch
// rather than a cast, so that adding one to SystemId stops the build here
// rather than telling the other device it is playing a DS.
uint8_t system_to_wire(SystemId system) {
    switch (system) {
        case SystemId::Ds: return 0;
        case SystemId::ThreeDs: return 1;
        case SystemId::WiiU: return 2;
    }
    return 0;
}

bool system_from_wire(uint8_t value, SystemId& out) {
    switch (value) {
        case 0: out = SystemId::Ds; return true;
        case 1: out = SystemId::ThreeDs; return true;
        case 2: out = SystemId::WiiU; return true;
        default: break;
    }
    return false;
}

uint8_t audio_target_to_wire(AudioTarget target) {
    switch (target) {
        case AudioTarget::TopScreen: return 0u;
        case AudioTarget::BottomScreen: return 1u;
        case AudioTarget::Both: break;
        case AudioTarget::Off: return 3u;
    }
    return 2u;
}

bool audio_target_from_wire(uint8_t value, AudioTarget& out) {
    switch (value) {
        case 0: out = AudioTarget::TopScreen; return true;
        case 1: out = AudioTarget::BottomScreen; return true;
        case 2: out = AudioTarget::Both; return true;
        case 3: out = AudioTarget::Off; return true;
        default: break;
    }
    return false;
}

uint8_t handedness_to_wire(Handedness handedness) {
    return handedness == Handedness::Left ? 1u : 0u;
}

bool handedness_from_wire(uint8_t value, Handedness& out) {
    if (value > 1u) return false;
    out = (value == 1u) ? Handedness::Left : Handedness::Right;
    return true;
}

uint8_t right_stick_mode_to_wire(RightStickMode mode) {
    switch (mode) {
        case RightStickMode::CStick: return 0u;
        case RightStickMode::CirclePad: return 1u;
        case RightStickMode::DPad: return 2u;
        case RightStickMode::Off: break;
    }
    return 3u;
}

bool right_stick_mode_from_wire(uint8_t value, RightStickMode& out) {
    switch (value) {
        case 0u: out = RightStickMode::CStick; return true;
        case 1u: out = RightStickMode::CirclePad; return true;
        case 2u: out = RightStickMode::DPad; return true;
        case 3u: out = RightStickMode::Off; return true;
        default: break;
    }
    return false;
}

bool screen_from_wire(uint8_t value, ScreenId& out) {
    if (value == 0) {
        out = ScreenId::Top;
        return true;
    }
    if (value == 1) {
        out = ScreenId::Bottom;
        return true;
    }
    return false;
}

}  // namespace

// --- encoders ---------------------------------------------------------------

void encode_hello(const HelloMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u32(out, kProtocolMagic);
    put_u16(out, message.protocol_version);
    put_string(out, message.device_name);
    put_u8(out, screen_to_wire(message.wanted_screen));
    put_u8(out, message.accepts_video ? 1u : 0u);
    put_u8(out, message.has_pointer ? 1u : 0u);
    finish_message(out, start, MessageType::Hello);
}

void encode_welcome(const WelcomeMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u16(out, message.protocol_version);
    put_string(out, message.game_title);
    put_string(out, message.rom_sha1);
    put_u8(out, screen_to_wire(message.granted_screen));
    put_u8(out, system_to_wire(message.system));
    put_u32(out, message.audio_sample_rate);
    put_u64(out, message.session_token);
    finish_message(out, start, MessageType::Welcome);
}

void encode_reject(const RejectMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_string(out, message.reason);
    finish_message(out, start, MessageType::Reject);
}

void encode_session_state(const SessionStateMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_string(out, message.layout_id);
    put_u8(out, screen_to_wire(message.companion_screen));
    put_u8(out, message.paused ? 1u : 0u);
    put_u8(out, message.bilinear_filtering ? 1u : 0u);
    put_u8(out, audio_target_to_wire(message.audio_target));
    put_u8(out, handedness_to_wire(message.handedness));
    put_u8(out, message.render_scale);
    put_u8(out, message.screen_gap ? 1u : 0u);
    put_u8(out, right_stick_mode_to_wire(message.right_stick_mode));
    put_u8(out, message.menu_open ? 1u : 0u);
    put_u8(out, message.menu_focus);
    put_u8(out, message.menu_confirm);
    put_u8(out, message.max_render_scale);
    put_u8(out, message.cursor_choosable ? 1u : 0u);
    put_u8(out, message.right_stick_choosable ? 1u : 0u);
    put_u8(out, message.cursor_shown ? 1u : 0u);
    put_u8(out, message.settings_open ? 1u : 0u);
    put_u8(out, message.motion_wanted ? 1u : 0u);
    put_u8(out, message.save_points_available ? 1u : 0u);
    const std::size_t flipped = std::min<std::size_t>(message.left_handed_layouts.size(), 255);
    put_u8(out, static_cast<uint8_t>(flipped));
    for (std::size_t index = 0; index < flipped; ++index) {
        put_string(out, message.left_handed_layouts[index]);
    }
    // 0 when the menu does not offer it, otherwise which way it is set.
    put_u8(out, !message.left_stick_choosable ? 0u
                : message.left_stick_press == LeftStickPress::Microphone ? 1u
                                                                         : 2u);
    // Likewise: 0 when not offered, 1 out, 2 in.
    put_u8(out, !message.rumble_choosable ? 0u : message.rumble_pak ? 2u : 1u);
    // And again: 0 when not offered, 1 off, 2 on.
    put_u8(out, !message.resume_choosable ? 0u : message.resume_from_save_state ? 2u : 1u);
    put_u8(out, message.resume_notice ? 1u : 0u);
    put_u8(out, message.save_sync_available ? 1u : 0u);
    put_u8(out, message.menu_confirm_focus);
    finish_message(out, start, MessageType::SessionState);
}

void encode_frame(const FrameMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u8(out, screen_to_wire(message.screen));
    put_u64(out, message.serial);
    put_u16(out, message.width);
    put_u16(out, message.height);
    put_u8(out, static_cast<uint8_t>(message.encoding));
    put_u32(out, static_cast<uint32_t>(message.payload.size()));
    out.insert(out.end(), message.payload.begin(), message.payload.end());
    finish_message(out, start, MessageType::Frame);
}

void encode_input_payload(const InputMessage& message, std::vector<uint8_t>& out) {
    put_u32(out, message.buttons);
    put_u8(out, message.touching ? 1u : 0u);
    put_u16(out, static_cast<uint16_t>(message.touch_x));
    put_u16(out, static_cast<uint16_t>(message.touch_y));
    put_u16(out, static_cast<uint16_t>(message.circle_x));
    put_u16(out, static_cast<uint16_t>(message.circle_y));
    put_u16(out, static_cast<uint16_t>(message.c_stick_x));
    put_u16(out, static_cast<uint16_t>(message.c_stick_y));
    // Always written, even when absent, so the payload is one fixed size --
    // a variable-length tail on a message this small buys nothing and costs a
    // second thing that can disagree between the two ends.
    put_u8(out, message.motion.present ? 1u : 0u);
    put_f32(out, message.motion.gyro_x);
    put_f32(out, message.motion.gyro_y);
    put_f32(out, message.motion.gyro_z);
    put_f32(out, message.motion.accel_x);
    put_f32(out, message.motion.accel_y);
    put_f32(out, message.motion.accel_z);
    put_u32(out, message.motion.sampled_us);
    // The pads, as a tail -- see InputMessage::pads. Absent altogether for a
    // companion that does not report them, which is what an older one sends.
    if (message.reports_pads) {
        const std::size_t count = std::min(message.pads.size(), kMaxReportedPads);
        put_u8(out, static_cast<uint8_t>(count));
        for (std::size_t index = 0; index < count; ++index) {
            const ReportedPad& pad = message.pads[index];
            put_u32(out, pad.id);
            put_u32(out, pad.held.buttons);
            put_u16(out, static_cast<uint16_t>(pad.held.circle_x));
            put_u16(out, static_cast<uint16_t>(pad.held.circle_y));
            put_u16(out, static_cast<uint16_t>(pad.held.c_stick_x));
            put_u16(out, static_cast<uint16_t>(pad.held.c_stick_y));
            put_u8(out, pad.held.motion.present ? 1u : 0u);
            put_f32(out, pad.held.motion.gyro_x);
            put_f32(out, pad.held.motion.gyro_y);
            put_f32(out, pad.held.motion.gyro_z);
            put_f32(out, pad.held.motion.accel_x);
            put_f32(out, pad.held.motion.accel_y);
            put_f32(out, pad.held.motion.accel_z);
            put_u32(out, pad.held.motion.sampled_us);
        }
        // Their names after all of them rather than inside each, so a host
        // that knows the pads but not their names reads the pads and stops.
        for (std::size_t index = 0; index < count; ++index) {
            const std::string& name = message.pads[index].name;
            const std::size_t length = std::min(name.size(), kMaxReportedPadName);
            put_u8(out, static_cast<uint8_t>(length));
            out.insert(out.end(), name.begin(), name.begin() + static_cast<std::ptrdiff_t>(length));
        }
        // And their slots, one byte each after the names, 0xFF for none --
        // after, for the same reason the names come after the pads.
        for (std::size_t index = 0; index < count; ++index) {
            const int slot = message.pads[index].slot;
            put_u8(out, slot < 0 || slot > 0xFE ? 0xFFu : static_cast<uint8_t>(slot));
        }
        // Which input this is -- see InputMessage::sequence.
        if (message.has_sequence) {
            put_u32(out, message.sequence);
        }
    }
}

void encode_input_echo_payload(const InputEchoMessage& message, std::vector<uint8_t>& out) {
    put_u32(out, message.sequence);
    put_u64(out, message.serial);
}

bool decode_input_echo_payload(const uint8_t* data, std::size_t size, InputEchoMessage& out) {
    Reader reader(data, size);
    reader.u32(out.sequence);
    reader.u64(out.serial);
    return reader.ok();
}

void encode_input(const InputMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    encode_input_payload(message, out);
    finish_message(out, start, MessageType::Input);
}

void encode_beacon(const BeaconMessage& message, std::vector<uint8_t>& out) {
    out.clear();
    put_u32(out, kProtocolMagic);
    put_u16(out, message.protocol_version);
    put_string(out, message.device_name);
    put_string(out, message.game_title);
    put_string(out, message.rom_sha1);
    put_u16(out, message.session_port);
    put_u8(out, message.top_available ? 1u : 0u);
    put_u8(out, message.bottom_available ? 1u : 0u);
}

bool decode_beacon(const uint8_t* data, std::size_t size, BeaconMessage& out) {
    Reader reader(data, size);
    uint32_t magic = 0;
    uint8_t top = 0;
    uint8_t bottom = 0;
    reader.u32(magic);
    reader.u16(out.protocol_version);
    reader.string(out.device_name);
    reader.string(out.game_title);
    reader.string(out.rom_sha1);
    reader.u16(out.session_port);
    reader.u8(top);
    reader.u8(bottom);
    if (!reader.ok() || magic != kProtocolMagic) {
        return false;
    }
    out.top_available = (top != 0);
    out.bottom_available = (bottom != 0);
    return true;
}

void encode_audio_payload(const AudioMessage& message, std::vector<uint8_t>& out) {
    put_u8(out, static_cast<uint8_t>(message.stream));
    put_u32(out, message.sample_rate);
    put_u32(out, message.sequence);
    put_u32(out, static_cast<uint32_t>(message.samples.size()));
    for (const int16_t sample : message.samples) {
        put_u16(out, static_cast<uint16_t>(sample));
    }
}

void encode_audio(const AudioMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    encode_audio_payload(message, out);
    finish_message(out, start, MessageType::Audio);
}

bool decode_audio(const uint8_t* data, std::size_t size, AudioMessage& out) {
    Reader reader(data, size);
    uint8_t stream = 0;
    uint32_t count = 0;
    reader.u8(stream);
    reader.u32(out.sample_rate);
    reader.u32(out.sequence);
    reader.u32(count);
    if (!reader.ok()) return false;
    // Two bytes per sample must actually be there before anything is reserved.
    if (static_cast<std::size_t>(count) * 2u > reader.remaining()) {
        return false;
    }
    if (stream > static_cast<uint8_t>(AudioStreamId::BottomDevice)) {
        return false;
    }
    out.stream = static_cast<AudioStreamId>(stream);
    out.samples.resize(count);
    for (uint32_t index = 0; index < count; ++index) {
        uint16_t raw = 0;
        if (!reader.u16(raw)) {
            return false;
        }
        out.samples[index] = static_cast<int16_t>(raw);
    }
    return true;
}

void encode_rumble(const RumbleMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u8(out, message.active ? 1u : 0u);
    // Which pad, as a tail an older companion never reads -- and which it does
    // not need to: it only ever hears about the whole device.
    put_u32(out, message.pad);
    if (message.ping_ms > 0) {
        put_u16(out, message.ping_ms);
    }
    finish_message(out, start, MessageType::Rumble);
}

// Each slot's picture goes deflated: a small one, but four of them, on the
// same connection as every press.
void encode_save_slots(const SaveSlotsMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    const std::size_t count = std::min<std::size_t>(message.slots.size(), 255);
    put_u8(out, static_cast<uint8_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        const SavePointInfo& slot = message.slots[index];
        put_u8(out, slot.exists ? 1u : 0u);
        put_u64(out, static_cast<uint64_t>(slot.saved_at));
        std::vector<uint8_t> packed;
        const SavePointThumbnail& picture = slot.thumbnail;
        if (slot.exists && picture.valid() && picture.width <= 0xffff && picture.height <= 0xffff) {
            std::vector<uint8_t> raw;
            raw.reserve(picture.pixels.size() * 4);
            for (const uint32_t pixel : picture.pixels) put_u32(raw, pixel);
            uLongf bound = compressBound(static_cast<uLong>(raw.size()));
            packed.resize(bound);
            if (compress2(packed.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), 6) ==
                Z_OK) {
                packed.resize(bound);
            } else {
                packed.clear();
            }
        }
        put_u16(out, packed.empty() ? 0u : static_cast<uint16_t>(picture.width));
        put_u16(out, packed.empty() ? 0u : static_cast<uint16_t>(picture.height));
        put_u32(out, static_cast<uint32_t>(packed.size()));
        out.insert(out.end(), packed.begin(), packed.end());
    }
    put_string(out, message.status);
    put_u8(out, message.status_failed ? 1u : 0u);
    finish_message(out, start, MessageType::SaveSlots);
}

bool decode_save_slots(const uint8_t* data, std::size_t size, SaveSlotsMessage& out) {
    Reader reader(data, size);
    uint8_t count = 0;
    if (!reader.u8(count)) return false;
    out.slots.assign(count, SavePointInfo{});
    for (SavePointInfo& slot : out.slots) {
        uint8_t exists = 0;
        uint64_t saved_at = 0;
        uint16_t width = 0;
        uint16_t height = 0;
        uint32_t packed_size = 0;
        std::vector<uint8_t> packed;
        reader.u8(exists);
        reader.u64(saved_at);
        reader.u16(width);
        reader.u16(height);
        reader.u32(packed_size);
        if (!reader.ok() || packed_size > reader.remaining()) return false;
        reader.bytes(packed, packed_size);
        slot.exists = exists != 0;
        slot.saved_at = static_cast<int64_t>(saved_at);
        // A picture that does not unpack is only a missing picture.
        if (width > 0 && height > 0 && width <= 1024 && height <= 1024 && !packed.empty()) {
            std::vector<uint8_t> raw(static_cast<std::size_t>(width) * height * 4);
            uLongf raw_size = static_cast<uLongf>(raw.size());
            if (uncompress(raw.data(), &raw_size, packed.data(), static_cast<uLong>(packed.size())) ==
                    Z_OK &&
                raw_size == raw.size()) {
                slot.thumbnail.width = width;
                slot.thumbnail.height = height;
                slot.thumbnail.pixels.resize(raw.size() / 4);
                for (std::size_t pixel = 0; pixel < slot.thumbnail.pixels.size(); ++pixel) {
                    const uint8_t* bytes = raw.data() + pixel * 4;
                    slot.thumbnail.pixels[pixel] =
                        static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
                        (static_cast<uint32_t>(bytes[2]) << 16) |
                        (static_cast<uint32_t>(bytes[3]) << 24);
                }
            }
        }
    }
    uint8_t failed = 0;
    reader.string(out.status);
    reader.u8(failed);
    out.status_failed = failed != 0;
    return reader.ok();
}

bool decode_rumble(const uint8_t* data, std::size_t size, RumbleMessage& out) {
    Reader reader(data, size);
    uint8_t active = 0;
    reader.u8(active);
    if (!reader.ok()) return false;
    out.active = active != 0;
    out.pad = RumbleMessage::kWholeDevice;
    out.ping_ms = 0;
    if (reader.remaining() >= 4) {
        reader.u32(out.pad);
    }
    if (reader.remaining() >= 2) {
        reader.u16(out.ping_ms);
    }
    return true;
}

namespace {

bool menu_action_from_wire(uint8_t value, MenuAction& out) {
    switch (value) {
        case 1: out = MenuAction::Up; return true;
        case 2: out = MenuAction::Down; return true;
        case 3: out = MenuAction::Left; return true;
        case 4: out = MenuAction::Right; return true;
        case 5: out = MenuAction::Confirm; return true;
        case 6: out = MenuAction::Cancel; return true;
        case 7: out = MenuAction::Quit; return true;
        case 8: out = MenuAction::Reset; return true;
        case 9: out = MenuAction::Disconnect; return true;
        case 10: out = MenuAction::Settings; return true;
        default: break;
    }
    return false;
}

}  // namespace

void encode_request(const RequestMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_string(out, message.layout_id);
    put_u8(out, message.wants_filtering ? 1u : 0u);
    put_u8(out, message.bilinear_filtering ? 1u : 0u);
    put_u8(out, message.wants_audio_target ? 1u : 0u);
    put_u8(out, audio_target_to_wire(message.audio_target));
    put_u8(out, message.wants_handedness ? 1u : 0u);
    put_u8(out, handedness_to_wire(message.handedness));
    put_u8(out, message.wants_render_scale ? 1u : 0u);
    put_u8(out, message.render_scale);
    put_u8(out, message.wants_display_size ? 1u : 0u);
    put_u16(out, message.display_width);
    put_u16(out, message.display_height);
    put_u8(out, message.menu_open ? 1u : 0u);
    put_u8(out, message.wants_reset ? 1u : 0u);
    put_u8(out, message.wants_quit ? 1u : 0u);
    put_u8(out, message.wants_screen_gap ? 1u : 0u);
    put_u8(out, message.screen_gap ? 1u : 0u);
    put_u8(out, message.wants_right_stick_mode ? 1u : 0u);
    put_u8(out, right_stick_mode_to_wire(message.right_stick_mode));
    put_u8(out, message.wants_menu_action ? 1u : 0u);
    put_u8(out, static_cast<uint8_t>(message.menu_action));
    put_u8(out, message.wants_menu_focus ? 1u : 0u);
    put_u8(out, message.menu_focus);
    // 0 for no change, otherwise which way it should be set.
    put_u8(out, !message.wants_left_stick_press ? 0u
                : message.left_stick_press == LeftStickPress::Microphone ? 1u
                                                                         : 2u);
    // 0 for no change, 1 out, 2 in.
    put_u8(out, !message.wants_rumble_pak ? 0u : message.rumble_pak ? 2u : 1u);
    // 0 for no change, 1 off, 2 on.
    put_u8(out, !message.wants_resume_from_save_state ? 0u
                : message.resume_from_save_state   ? 2u
                                                   : 1u);
    finish_message(out, start, MessageType::Request);
}

bool decode_request(const uint8_t* data, std::size_t size, RequestMessage& out) {
    Reader reader(data, size);
    uint8_t wants_filtering = 0;
    uint8_t filtering = 0;
    uint8_t wants_audio = 0;
    uint8_t audio = 0;
    uint8_t wants_hands = 0;
    uint8_t hands = 0;
    uint8_t wants_scale = 0;
    uint8_t wants_size = 0;
    uint8_t menu_open = 0;
    uint8_t wants_reset = 0;
    uint8_t wants_quit = 0;
    uint8_t wants_screen_gap = 0;
    uint8_t screen_gap = 0;
    reader.string(out.layout_id);
    reader.u8(wants_filtering);
    reader.u8(filtering);
    reader.u8(wants_audio);
    reader.u8(audio);
    reader.u8(wants_hands);
    reader.u8(hands);
    reader.u8(wants_scale);
    reader.u8(out.render_scale);
    reader.u8(wants_size);
    reader.u16(out.display_width);
    reader.u16(out.display_height);
    reader.u8(menu_open);
    reader.u8(wants_reset);
    reader.u8(wants_quit);
    reader.u8(wants_screen_gap);
    reader.u8(screen_gap);
    uint8_t wants_right_stick = 0;
    uint8_t right_stick = 0;
    reader.u8(wants_right_stick);
    reader.u8(right_stick);
    uint8_t wants_menu_action = 0;
    uint8_t menu_action = 0;
    uint8_t wants_menu_focus = 0;
    reader.u8(wants_menu_action);
    reader.u8(menu_action);
    reader.u8(wants_menu_focus);
    reader.u8(out.menu_focus);
    if (!reader.ok()) return false;
    out.wants_left_stick_press = false;
    if (reader.remaining() > 0) {
        uint8_t left_stick = 0;
        reader.u8(left_stick);
        if (left_stick > 2) return false;
        out.wants_left_stick_press = left_stick != 0;
        out.left_stick_press =
            left_stick == 2 ? LeftStickPress::SwapLayout : LeftStickPress::Microphone;
    }
    out.wants_rumble_pak = false;
    out.rumble_pak = false;
    if (reader.remaining() > 0) {
        uint8_t rumble = 0;
        reader.u8(rumble);
        if (rumble > 2) return false;
        out.wants_rumble_pak = rumble != 0;
        out.rumble_pak = rumble == 2;
    }
    out.wants_resume_from_save_state = false;
    out.resume_from_save_state = false;
    if (reader.remaining() > 0) {
        uint8_t resume = 0;
        reader.u8(resume);
        if (resume > 2) return false;
        out.wants_resume_from_save_state = resume != 0;
        out.resume_from_save_state = resume == 2;
    }
    out.wants_menu_action = (wants_menu_action != 0);
    out.wants_menu_focus = (wants_menu_focus != 0);
    if (out.wants_menu_action && !menu_action_from_wire(menu_action, out.menu_action)) return false;
    out.wants_filtering = (wants_filtering != 0);
    out.bilinear_filtering = (filtering != 0);
    out.wants_audio_target = (wants_audio != 0);
    out.wants_handedness = (wants_hands != 0);
    out.wants_render_scale = (wants_scale != 0);
    out.wants_display_size = (wants_size != 0);
    out.menu_open = (menu_open != 0);
    out.wants_reset = (wants_reset != 0);
    out.wants_quit = (wants_quit != 0);
    out.wants_screen_gap = (wants_screen_gap != 0);
    out.screen_gap = (screen_gap != 0);
    out.wants_right_stick_mode = (wants_right_stick != 0);
    if (!right_stick_mode_from_wire(right_stick, out.right_stick_mode)) return false;
    if (!handedness_from_wire(hands, out.handedness)) return false;
    return audio_target_from_wire(audio, out.audio_target);
}

void encode_bye(std::vector<uint8_t>& out, bool quit_too) {
    const std::size_t start = begin_message(out);
    if (quit_too) {
        put_u8(out, 1);
    }
    finish_message(out, start, MessageType::Bye);
}

bool bye_says_quit_too(const std::vector<uint8_t>& payload) {
    return !payload.empty() && payload[0] == 1;
}

void encode_frame_ack_payload(uint64_t serial, std::vector<uint8_t>& out) { put_u64(out, serial); }

void encode_frame_ack(uint64_t serial, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    encode_frame_ack_payload(serial, out);
    finish_message(out, start, MessageType::FrameAck);
}

bool decode_frame_ack(const uint8_t* data, std::size_t size, uint64_t& out_serial) {
    uint16_t link_delay_ms = kUnknownLinkDelay;
    return decode_frame_ack(data, size, out_serial, link_delay_ms);
}

void encode_frame_ack_payload(uint64_t serial, uint16_t link_delay_ms,
                              std::vector<uint8_t>& out) {
    put_u64(out, serial);
    put_u16(out, link_delay_ms);
}

bool decode_frame_ack(const uint8_t* data, std::size_t size, uint64_t& out_serial,
                      uint16_t& out_link_delay_ms) {
    out_link_delay_ms = kUnknownLinkDelay;
    Reader reader(data, size);
    reader.u64(out_serial);
    if (!reader.ok()) {
        return false;
    }
    if (size >= 10) {
        reader.u16(out_link_delay_ms);
    }
    return reader.ok();
}

void encode_ping(std::vector<uint8_t>& out) {
    finish_message(out, begin_message(out), MessageType::Ping);
}

// --- decoders ---------------------------------------------------------------

bool decode_hello(const uint8_t* data, std::size_t size, HelloMessage& out) {
    Reader reader(data, size);
    uint32_t magic = 0;
    uint8_t screen = 0;
    reader.u32(magic);
    reader.u16(out.protocol_version);
    reader.string(out.device_name);
    reader.u8(screen);
    uint8_t accepts_video = 0;
    reader.u8(accepts_video);
    if (!reader.ok() || magic != kProtocolMagic) {
        return false;
    }
    out.accepts_video = accepts_video != 0;
    uint8_t has_pointer = 1;
    if (reader.remaining() > 0) reader.u8(has_pointer);
    out.has_pointer = has_pointer != 0;
    return screen_from_wire(screen, out.wanted_screen);
}

bool decode_welcome(const uint8_t* data, std::size_t size, WelcomeMessage& out) {
    Reader reader(data, size);
    uint8_t screen = 0;
    uint8_t system = 0;
    reader.u16(out.protocol_version);
    reader.string(out.game_title);
    reader.string(out.rom_sha1);
    reader.u8(screen);
    reader.u8(system);
    reader.u32(out.audio_sample_rate);
    reader.u64(out.session_token);
    if (!reader.ok() || !system_from_wire(system, out.system)) return false;
    return screen_from_wire(screen, out.granted_screen);
}

bool decode_reject(const uint8_t* data, std::size_t size, RejectMessage& out) {
    Reader reader(data, size);
    reader.string(out.reason);
    return reader.ok();
}

bool decode_session_state(const uint8_t* data, std::size_t size, SessionStateMessage& out) {
    Reader reader(data, size);
    uint8_t screen = 0;
    uint8_t paused = 0;
    uint8_t filtering = 0;
    uint8_t audio = 0;
    uint8_t hands = 0;
    uint8_t screen_gap = 0;
    reader.string(out.layout_id);
    reader.u8(screen);
    reader.u8(paused);
    reader.u8(filtering);
    reader.u8(audio);
    reader.u8(hands);
    reader.u8(out.render_scale);
    reader.u8(screen_gap);
    uint8_t right_stick = 0;
    reader.u8(right_stick);
    uint8_t menu_open = 0;
    uint8_t cursor_choosable = 0;
    uint8_t right_stick_choosable = 0;
    reader.u8(menu_open);
    reader.u8(out.menu_focus);
    reader.u8(out.menu_confirm);
    reader.u8(out.max_render_scale);
    reader.u8(cursor_choosable);
    reader.u8(right_stick_choosable);
    uint8_t cursor_shown = 0;
    uint8_t settings_open = 0;
    uint8_t motion_wanted = 0;
    reader.u8(cursor_shown);
    reader.u8(settings_open);
    reader.u8(motion_wanted);
    if (!reader.ok()) return false;
    uint8_t save_points_available = 0;
    if (reader.remaining() > 0) reader.u8(save_points_available);
    out.save_points_available = (save_points_available != 0);
    out.left_handed_layouts.clear();
    bool listed = false;
    if (reader.remaining() > 0) {
        uint8_t flipped = 0;
        reader.u8(flipped);
        for (uint8_t index = 0; index < flipped && reader.ok(); ++index) {
            std::string id;
            reader.string(id);
            out.left_handed_layouts.push_back(id);
        }
        if (!reader.ok()) return false;
        listed = true;
    }
    // A host from before the left stick was on the menu offers no such entry.
    out.left_stick_choosable = false;
    out.left_stick_press = LeftStickPress::Microphone;
    if (reader.remaining() > 0) {
        uint8_t left_stick = 0;
        reader.u8(left_stick);
        if (left_stick > 2) return false;
        out.left_stick_choosable = left_stick != 0;
        out.left_stick_press =
            left_stick == 2 ? LeftStickPress::SwapLayout : LeftStickPress::Microphone;
    }
    // And one from before the Rumble Pak was: none on offer.
    out.rumble_choosable = false;
    out.rumble_pak = false;
    if (reader.remaining() > 0) {
        uint8_t rumble = 0;
        reader.u8(rumble);
        if (rumble > 2) return false;
        out.rumble_choosable = rumble != 0;
        out.rumble_pak = rumble == 2;
    }
    out.resume_choosable = false;
    out.resume_from_save_state = false;
    out.resume_notice = false;
    if (reader.remaining() > 0) {
        uint8_t resume = 0;
        reader.u8(resume);
        if (resume > 2) return false;
        out.resume_choosable = resume != 0;
        out.resume_from_save_state = resume == 2;
    }
    if (reader.remaining() > 0) {
        uint8_t notice = 0;
        reader.u8(notice);
        out.resume_notice = notice != 0;
    }
    out.save_sync_available = false;
    if (reader.remaining() > 0) {
        uint8_t save_sync = 0;
        reader.u8(save_sync);
        out.save_sync_available = save_sync != 0;
    }
    out.menu_confirm_focus = 0;
    if (reader.remaining() > 0) reader.u8(out.menu_confirm_focus);
    out.motion_wanted = (motion_wanted != 0);
    out.cursor_shown = (cursor_shown != 0);
    out.settings_open = (settings_open != 0);
    out.paused = (paused != 0);
    out.bilinear_filtering = (filtering != 0);
    out.screen_gap = (screen_gap != 0);
    out.menu_open = (menu_open != 0);
    out.cursor_choosable = (cursor_choosable != 0);
    out.right_stick_choosable = (right_stick_choosable != 0);
    if (!right_stick_mode_from_wire(right_stick, out.right_stick_mode)) return false;
    if (!audio_target_from_wire(audio, out.audio_target)) return false;
    if (!handedness_from_wire(hands, out.handedness)) return false;
    // A host from before the list: all it says is the hand of the layout in
    // use, which is all it had.
    if (!listed && out.handedness == Handedness::Left) {
        out.left_handed_layouts.push_back(out.layout_id);
    }
    return screen_from_wire(screen, out.companion_screen);
}

bool decode_frame(const uint8_t* data, std::size_t size, FrameMessage& out) {
    Reader reader(data, size);
    uint8_t screen = 0;
    uint8_t encoding = 0;
    uint32_t payload_size = 0;
    reader.u8(screen);
    reader.u64(out.serial);
    reader.u16(out.width);
    reader.u16(out.height);
    reader.u8(encoding);
    reader.u32(payload_size);
    if (!reader.ok() || payload_size > reader.remaining()) {
        return false;
    }
    if (!reader.bytes(out.payload, payload_size)) {
        return false;
    }
    switch (static_cast<FrameEncoding>(encoding)) {
        case FrameEncoding::Rgb565Deflate:
        case FrameEncoding::Rgb565DeflateTiles:
        case FrameEncoding::H264:
            out.encoding = static_cast<FrameEncoding>(encoding);
            break;
        default:
            // An encoding this build has no decoder for. Both ends agreed a
            // protocol version during the handshake, so this is a corrupt or
            // hostile frame rather than a newer peer.
            return false;
    }
    return screen_from_wire(screen, out.screen);
}

bool decode_input(const uint8_t* data, std::size_t size, InputMessage& out) {
    Reader reader(data, size);
    uint8_t touching = 0;
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t circle_x = 0;
    uint16_t circle_y = 0;
    uint16_t c_stick_x = 0;
    uint16_t c_stick_y = 0;
    reader.u32(out.buttons);
    reader.u8(touching);
    reader.u16(x);
    reader.u16(y);
    reader.u16(circle_x);
    reader.u16(circle_y);
    reader.u16(c_stick_x);
    reader.u16(c_stick_y);
    uint8_t has_motion = 0;
    reader.u8(has_motion);
    reader.f32(out.motion.gyro_x);
    reader.f32(out.motion.gyro_y);
    reader.f32(out.motion.gyro_z);
    reader.f32(out.motion.accel_x);
    reader.f32(out.motion.accel_y);
    reader.f32(out.motion.accel_z);
    reader.u32(out.motion.sampled_us);
    if (!reader.ok()) return false;
    out.motion.present = (has_motion != 0);
    out.touching = (touching != 0);
    out.touch_x = static_cast<int16_t>(x);
    out.touch_y = static_cast<int16_t>(y);
    out.circle_x = static_cast<int16_t>(circle_x);
    out.circle_y = static_cast<int16_t>(circle_y);
    out.c_stick_x = static_cast<int16_t>(c_stick_x);
    out.c_stick_y = static_cast<int16_t>(c_stick_y);
    // The pads' tail, when the companion sent one -- see InputMessage::pads.
    out.reports_pads = false;
    out.has_sequence = false;
    out.pads.clear();
    if (reader.remaining() >= 1) {
        uint8_t count = 0;
        reader.u8(count);
        if (!reader.ok() || count > kMaxReportedPads) return false;
        out.pads.resize(count);
        for (ReportedPad& pad : out.pads) {
            uint16_t sticks[4] = {0, 0, 0, 0};
            uint8_t pad_motion = 0;
            reader.u32(pad.id);
            reader.u32(pad.held.buttons);
            for (uint16_t& stick : sticks) reader.u16(stick);
            reader.u8(pad_motion);
            reader.f32(pad.held.motion.gyro_x);
            reader.f32(pad.held.motion.gyro_y);
            reader.f32(pad.held.motion.gyro_z);
            reader.f32(pad.held.motion.accel_x);
            reader.f32(pad.held.motion.accel_y);
            reader.f32(pad.held.motion.accel_z);
            reader.u32(pad.held.motion.sampled_us);
            pad.held.circle_x = static_cast<int16_t>(sticks[0]);
            pad.held.circle_y = static_cast<int16_t>(sticks[1]);
            pad.held.c_stick_x = static_cast<int16_t>(sticks[2]);
            pad.held.c_stick_y = static_cast<int16_t>(sticks[3]);
            pad.held.motion.present = pad_motion != 0;
        }
        if (!reader.ok()) return false;
        out.reports_pads = true;
        // The names, from a companion new enough to send them.
        if (reader.remaining() >= 1) {
            for (ReportedPad& pad : out.pads) {
                uint8_t length = 0;
                std::vector<uint8_t> bytes;
                if (!reader.u8(length) || length > kMaxReportedPadName ||
                    !reader.bytes(bytes, length)) {
                    return false;
                }
                pad.name.assign(bytes.begin(), bytes.end());
            }
            // The slots, from a companion new enough to send them.
            if (reader.remaining() >= out.pads.size()) {
                for (ReportedPad& pad : out.pads) {
                    uint8_t slot = 0xFF;
                    reader.u8(slot);
                    pad.slot = slot == 0xFF ? -1 : slot;
                }
                // And which input this is, from a companion that numbers them.
                out.has_sequence = false;
                if (reader.remaining() >= 4) {
                    out.has_sequence = reader.u32(out.sequence);
                }
            }
        }
    }
    return true;
}

bool take_message(std::vector<uint8_t>& buffer,
                  MessageType& out_type,
                  std::vector<uint8_t>& out_payload,
                  bool& fatal) {
    fatal = false;
    if (buffer.size() < 6) {
        return false;
    }
    uint32_t length = 0;
    for (int index = 0; index < 4; ++index) {
        length |= static_cast<uint32_t>(buffer[index]) << (index * 8);
    }
    if (length > kMaxMessageBytes) {
        // Either a desynchronised stream or a sender we should not be talking
        // to. Either way the connection is finished.
        fatal = true;
        return false;
    }
    if (buffer.size() < 6u + length) {
        return false;
    }
    out_type = static_cast<MessageType>(buffer[4] | (buffer[5] << 8));
    out_payload.assign(buffer.begin() + 6, buffer.begin() + 6 + length);
    buffer.erase(buffer.begin(), buffer.begin() + 6 + length);
    return true;
}

// --- the UDP media channel ---------------------------------------------------

void encode_media_header(uint64_t session_token, MessageType type, std::vector<uint8_t>& out) {
    put_u64(out, session_token);
    put_u8(out, static_cast<uint8_t>(type));
}

bool stamp_media_datagram_sent_at(uint8_t* datagram, std::size_t size, uint32_t sent_at_ms) {
    // u64 session_token + u8 type, then the fragment header.
    constexpr std::size_t kMediaHeaderBytes = 9;
    // Inside FrameFragmentHeader: serial(8) + screen(1) + encoding(1) +
    // width(2) + height(2) + fragment_index(2) + fragment_count(2) +
    // keyframe(1) lands sent_at_ms here. Asserted against the encoder in a
    // test rather than trusted, because the two drifting apart would put four
    // bytes of clock through the middle of a picture.
    constexpr std::size_t kSentAtOffsetInFragment = 19;
    constexpr std::size_t kOffset = kMediaHeaderBytes + kSentAtOffsetInFragment;
    if (datagram == nullptr || size < kMediaHeaderBytes + kFrameFragmentHeaderBytes) {
        return false;
    }
    if (datagram[8] != static_cast<uint8_t>(MessageType::Frame)) {
        return false;
    }
    datagram[kOffset + 0] = static_cast<uint8_t>(sent_at_ms & 0xffu);
    datagram[kOffset + 1] = static_cast<uint8_t>((sent_at_ms >> 8) & 0xffu);
    datagram[kOffset + 2] = static_cast<uint8_t>((sent_at_ms >> 16) & 0xffu);
    datagram[kOffset + 3] = static_cast<uint8_t>((sent_at_ms >> 24) & 0xffu);
    return true;
}

bool decode_media_header(const uint8_t* data, std::size_t size, uint64_t& out_token,
                         MessageType& out_type, const uint8_t*& out_payload,
                         std::size_t& out_payload_size) {
    Reader reader(data, size);
    uint8_t type = 0;
    reader.u64(out_token);
    reader.u8(type);
    if (!reader.ok()) {
        return false;
    }
    out_type = static_cast<MessageType>(type);
    // 8 (session_token) + 1 (type).
    out_payload = data + 9;
    out_payload_size = size - 9;
    return true;
}

void encode_frame_fragment(const FrameFragmentHeader& header, const uint8_t* payload,
                           std::size_t payload_size, std::vector<uint8_t>& out) {
    put_u64(out, header.serial);
    put_u8(out, screen_to_wire(header.screen));
    put_u8(out, static_cast<uint8_t>(header.encoding));
    put_u16(out, header.width);
    put_u16(out, header.height);
    put_u16(out, header.fragment_index);
    put_u16(out, header.fragment_count);
    put_u8(out, header.keyframe ? 1u : 0u);
    put_u32(out, header.sent_at_ms);
    put_u16(out, header.sent_index);
    out.insert(out.end(), payload, payload + payload_size);
}

bool decode_frame_fragment(const uint8_t* data, std::size_t size, FrameFragmentHeader& out_header,
                           const uint8_t*& out_payload, std::size_t& out_payload_size) {
    Reader reader(data, size);
    uint8_t screen = 0;
    uint8_t encoding = 0;
    reader.u64(out_header.serial);
    reader.u8(screen);
    reader.u8(encoding);
    reader.u16(out_header.width);
    reader.u16(out_header.height);
    reader.u16(out_header.fragment_index);
    reader.u16(out_header.fragment_count);
    uint8_t keyframe = 0;
    reader.u8(keyframe);
    reader.u32(out_header.sent_at_ms);
    reader.u16(out_header.sent_index);
    if (!reader.ok() || !screen_from_wire(screen, out_header.screen)) {
        return false;
    }
    out_header.encoding = static_cast<FrameEncoding>(encoding);
    out_header.keyframe = keyframe != 0u;
    out_payload = data + kFrameFragmentHeaderBytes;
    out_payload_size = size - kFrameFragmentHeaderBytes;
    return true;
}

namespace {

// GF(2^8) over x^8 + x^4 + x^3 + x^2 + 1 (0x11D), the field Reed-Solomon codes
// are usually built on. A whole multiplication table is 64KB and makes the
// inner loops one lookup a byte, which is what makes a keyframe's recovery
// pieces a few milliseconds of work rather than tens.
struct GaloisField {
    uint8_t exp[512];
    uint8_t log[256];
    uint8_t mul[256][256];
    GaloisField() {
        unsigned value = 1;
        for (unsigned power = 0; power < 255; ++power) {
            exp[power] = static_cast<uint8_t>(value);
            log[value] = static_cast<uint8_t>(power);
            value <<= 1;
            if (value & 0x100u) value ^= 0x11Du;
        }
        for (unsigned power = 255; power < 512; ++power) exp[power] = exp[power - 255];
        log[0] = 0;
        for (unsigned a = 0; a < 256; ++a) {
            for (unsigned b = 0; b < 256; ++b) {
                mul[a][b] = (a == 0 || b == 0) ? 0 : exp[log[a] + log[b]];
            }
        }
    }
    uint8_t inverse(uint8_t value) const { return exp[255 - log[value]]; }
};

const GaloisField& field() {
    static const GaloisField instance;
    return instance;
}

// The coefficient recovery piece `row` of a block gives data piece `column`:
// a Cauchy matrix, 1 / (x_row + y_column) with the x and y all different, so
// that every square piece of it can be inverted -- which is what lets any
// recovery pieces stand in for any missing ones.
uint8_t cauchy(std::size_t row, std::size_t column) {
    const uint8_t x = static_cast<uint8_t>(255 - row);
    const uint8_t y = static_cast<uint8_t>(column);
    return field().inverse(static_cast<uint8_t>(x ^ y));
}

// out ^= coefficient * in, a byte at a time.
void multiply_add(uint8_t* out, const uint8_t* in, std::size_t size, uint8_t coefficient) {
    if (coefficient == 0) return;
    const uint8_t* row = field().mul[coefficient];
    for (std::size_t index = 0; index < size; ++index) {
        out[index] ^= row[in[index]];
    }
}

// How a frame of `count` pieces is cut into blocks: every block the same size
// but the last, and how many recovery pieces each gets.
struct ParityBlock {
    std::size_t first = 0;         // index of its first data piece
    std::size_t count = 0;         // data pieces in it
    std::size_t parity_first = 0;  // index of its first recovery piece, from 0
    std::size_t parity_count = 0;
};

std::vector<ParityBlock> parity_blocks(std::size_t count) {
    std::vector<ParityBlock> blocks;
    if (count == 0) return blocks;
    const std::size_t block_count =
        (count + kMaxDataPerParityBlock - 1) / kMaxDataPerParityBlock;
    const std::size_t per_block = (count + block_count - 1) / block_count;
    std::size_t parity_so_far = 0;
    for (std::size_t first = 0; first < count; first += per_block) {
        ParityBlock block;
        block.first = first;
        block.count = std::min(per_block, count - first);
        block.parity_first = parity_so_far;
        block.parity_count =
            std::max(kMinParityPerBlock, (block.count * kParityPercent + 99) / 100);
        parity_so_far += block.parity_count;
        blocks.push_back(block);
    }
    return blocks;
}

}  // namespace

std::size_t frame_parity_first_index(std::size_t fragment_count) { return fragment_count; }

std::size_t frame_parity_count(std::size_t fragment_count) {
    std::size_t total = 0;
    for (const ParityBlock& block : parity_blocks(fragment_count)) {
        total += block.parity_count;
    }
    return total;
}

std::vector<std::vector<uint8_t>> build_frame_parity(const std::vector<uint8_t>& payload,
                                                     std::size_t fragment_size) {
    std::vector<std::vector<uint8_t>> parity;
    if (payload.empty() || fragment_size == 0) {
        return parity;
    }
    const std::size_t count = (payload.size() + fragment_size - 1) / fragment_size;
    const std::size_t last_size = payload.size() - (count - 1) * fragment_size;
    for (const ParityBlock& block : parity_blocks(count)) {
        // As long as the block's longest piece: full length, unless the block
        // is the frame's short last piece alone.
        const bool holds_last = block.first + block.count == count;
        const std::size_t span = (holds_last && block.count == 1) ? last_size : fragment_size;
        for (std::size_t row = 0; row < block.parity_count; ++row) {
            std::vector<uint8_t> out(kParityPrefixBytes + span, 0);
            out[0] = static_cast<uint8_t>(last_size & 0xFFu);
            out[1] = static_cast<uint8_t>((last_size >> 8) & 0xFFu);
            for (std::size_t column = 0; column < block.count; ++column) {
                const std::size_t index = block.first + column;
                const std::size_t offset = index * fragment_size;
                const std::size_t size = std::min(fragment_size, payload.size() - offset);
                multiply_add(out.data() + kParityPrefixBytes, payload.data() + offset, size,
                             cauchy(row, column));
            }
            parity.push_back(std::move(out));
        }
    }
    return parity;
}

unsigned recover_fragments(std::vector<std::vector<uint8_t>>& fragments,
                           std::vector<bool>& received,
                           const std::vector<std::vector<uint8_t>>& parity,
                           const std::vector<bool>& parity_received) {
    const std::size_t count = fragments.size();
    if (count == 0 || received.size() != count || parity.size() != parity_received.size() ||
        parity.size() != frame_parity_count(count)) {
        return 0;
    }
    const GaloisField& gf = field();
    unsigned recovered = 0;
    for (const ParityBlock& block : parity_blocks(count)) {
        std::vector<std::size_t> missing;
        for (std::size_t column = 0; column < block.count; ++column) {
            if (!received[block.first + column]) missing.push_back(column);
        }
        if (missing.empty()) continue;
        std::vector<std::size_t> rows;
        for (std::size_t row = 0; row < block.parity_count && rows.size() < missing.size(); ++row) {
            const std::vector<uint8_t>& piece = parity[block.parity_first + row];
            if (parity_received[block.parity_first + row] && piece.size() > kParityPrefixBytes) {
                rows.push_back(row);
            }
        }
        if (rows.size() < missing.size()) continue;  // more lost than this can cover
        const std::size_t m = missing.size();
        const std::vector<uint8_t>& first_row = parity[block.parity_first + rows[0]];
        const std::size_t span = first_row.size() - kParityPrefixBytes;
        const std::size_t last_size = static_cast<std::size_t>(first_row[0]) |
                                      (static_cast<std::size_t>(first_row[1]) << 8);
        bool consistent = true;
        for (const std::size_t row : rows) {
            if (parity[block.parity_first + row].size() != first_row.size()) consistent = false;
        }
        // Recovery pieces of one block all have one length, and no piece is
        // longer than they are: anything else was not made by this frame.
        if (!consistent || last_size > span) {
            continue;
        }

        // What each recovery piece used holds of the missing pieces alone:
        // itself, with every piece that did arrive taken back out of it.
        std::vector<std::vector<uint8_t>> syndromes(m);
        for (std::size_t r = 0; r < m; ++r) {
            const std::vector<uint8_t>& piece = parity[block.parity_first + rows[r]];
            syndromes[r].assign(piece.begin() + kParityPrefixBytes, piece.end());
            for (std::size_t column = 0; column < block.count; ++column) {
                const std::size_t index = block.first + column;
                if (!received[index]) continue;
                const std::vector<uint8_t>& data = fragments[index];
                multiply_add(syndromes[r].data(), data.data(), std::min(data.size(), span),
                             cauchy(rows[r], column));
            }
        }

        // The m by m corner of the matrix that ties those to the missing
        // pieces, inverted by Gauss-Jordan elimination.
        std::vector<uint8_t> a(m * m);
        std::vector<uint8_t> inverse(m * m, 0);
        for (std::size_t r = 0; r < m; ++r) {
            for (std::size_t k = 0; k < m; ++k) a[r * m + k] = cauchy(rows[r], missing[k]);
            inverse[r * m + r] = 1;
        }
        bool singular = false;
        for (std::size_t column = 0; column < m && !singular; ++column) {
            std::size_t pivot = column;
            while (pivot < m && a[pivot * m + column] == 0) ++pivot;
            if (pivot == m) {
                singular = true;
                break;
            }
            if (pivot != column) {
                for (std::size_t k = 0; k < m; ++k) {
                    std::swap(a[pivot * m + k], a[column * m + k]);
                    std::swap(inverse[pivot * m + k], inverse[column * m + k]);
                }
            }
            const uint8_t scale = gf.inverse(a[column * m + column]);
            for (std::size_t k = 0; k < m; ++k) {
                a[column * m + k] = gf.mul[scale][a[column * m + k]];
                inverse[column * m + k] = gf.mul[scale][inverse[column * m + k]];
            }
            for (std::size_t r = 0; r < m; ++r) {
                const uint8_t factor = a[r * m + column];
                if (r == column || factor == 0) continue;
                for (std::size_t k = 0; k < m; ++k) {
                    a[r * m + k] ^= gf.mul[factor][a[column * m + k]];
                    inverse[r * m + k] ^= gf.mul[factor][inverse[column * m + k]];
                }
            }
        }
        if (singular) continue;  // cannot happen with a Cauchy matrix; not trusted blindly

        for (std::size_t k = 0; k < m; ++k) {
            std::vector<uint8_t> rebuilt(span, 0);
            for (std::size_t r = 0; r < m; ++r) {
                multiply_add(rebuilt.data(), syndromes[r].data(), span, inverse[k * m + r]);
            }
            const std::size_t index = block.first + missing[k];
            // Every piece but the frame's last is full length.
            rebuilt.resize(index == count - 1 ? std::min(last_size, span) : span);
            fragments[index] = std::move(rebuilt);
            received[index] = true;
            ++recovered;
        }
    }
    return recovered;
}

unsigned frames_missing_between(uint16_t last_seen, uint16_t arrived) {
    const auto step = static_cast<int16_t>(arrived - last_seen);
    if (step <= 0) {
        return 0;
    }
    return static_cast<unsigned>(step - 1);
}

void encode_keyframe_request_payload(const KeyframeRequestMessage& request,
                                     std::vector<uint8_t>& out) {
    put_u8(out, screen_to_wire(request.screen));
    put_u64(out, request.last_good_serial);
}

bool decode_keyframe_request_payload(const uint8_t* data, std::size_t size,
                                     KeyframeRequestMessage& out) {
    Reader reader(data, size);
    uint8_t screen = 0;
    reader.u8(screen);
    reader.u64(out.last_good_serial);
    return reader.ok() && screen_from_wire(screen, out.screen);
}

void encode_repair_request_payload(const RepairRequestMessage& request, std::vector<uint8_t>& out) {
    put_u8(out, screen_to_wire(request.screen));
    put_u8(out, request.whole_frame ? 1u : 0u);
    put_u64(out, request.serial);
    put_u16(out, request.sent_index);
    const std::size_t count = std::min(request.pieces.size(), kMaxRepairPieces);
    put_u16(out, static_cast<uint16_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        put_u16(out, request.pieces[index]);
    }
}

bool decode_repair_request_payload(const uint8_t* data, std::size_t size,
                                   RepairRequestMessage& out) {
    Reader reader(data, size);
    uint8_t screen = 0;
    uint8_t whole = 0;
    uint16_t count = 0;
    reader.u8(screen);
    reader.u8(whole);
    reader.u64(out.serial);
    reader.u16(out.sent_index);
    reader.u16(count);
    out.whole_frame = whole != 0;
    if (!reader.ok() || !screen_from_wire(screen, out.screen) || count > kMaxRepairPieces) {
        return false;
    }
    out.pieces.resize(count);
    for (uint16_t index = 0; index < count; ++index) {
        reader.u16(out.pieces[index]);
    }
    return reader.ok();
}

// --- pixels -----------------------------------------------------------------

namespace {

// -> RGB565 halves the bytes before compression, and the two bits per channel
// this drops are not visible on a DS screen scaled to a TV. Which byte holds
// red and which holds blue depends on the source; green is always the middle
// byte either way.
void pack_rgb565(const uint32_t* pixels,
                 int width,
                 int height,
                 int stride_pixels,
                 bool rgba,
                 std::vector<uint8_t>& out) {
    out.resize(static_cast<std::size_t>(width) * height * 2);
    std::size_t offset = 0;
    for (int row = 0; row < height; ++row) {
        const uint32_t* source = pixels + static_cast<std::size_t>(row) * stride_pixels;
        for (int column = 0; column < width; ++column) {
            const uint32_t pixel = source[column];
            const uint32_t red_bits = rgba ? (pixel >> 3) : (pixel >> 19);
            const uint32_t blue_bits = rgba ? (pixel >> 19) : (pixel >> 3);
            const uint16_t packed = static_cast<uint16_t>(((red_bits & 0x1fu) << 11) |
                                                          (((pixel >> 10) & 0x3fu) << 5) |
                                                          (blue_bits & 0x1fu));
            out[offset++] = static_cast<uint8_t>(packed & 0xffu);
            out[offset++] = static_cast<uint8_t>((packed >> 8) & 0xffu);
        }
    }
}

// Level 1 throughout: this runs once per screen per frame, so time matters far
// more than the last few percent of ratio -- level 6 was measured at under two
// percent smaller for four times the work.
bool deflate_onto(const std::vector<uint8_t>& body, std::vector<uint8_t>& out) {
    const std::size_t start = out.size();
    uLongf bound = compressBound(static_cast<uLong>(body.size()));
    out.resize(start + bound);
    const int result =
        compress2(out.data() + start, &bound, body.data(), static_cast<uLong>(body.size()), 1);
    if (result != Z_OK) {
        out.clear();
        return false;
    }
    out.resize(start + bound);
    return true;
}

// The grid Rgb565DeflateTiles cuts a screen into. Edge tiles are clipped, so a
// screen whose size is not a multiple of the tile is handled without padding.
struct TileGrid {
    int size = 0;
    int across = 0;
    int down = 0;
    std::size_t bitmap_bytes = 0;

    std::size_t count() const { return static_cast<std::size_t>(across) * down; }
};

TileGrid tile_grid(int width, int height, int shift) {
    TileGrid grid;
    grid.size = 1 << shift;
    grid.across = (width + grid.size - 1) / grid.size;
    grid.down = (height + grid.size - 1) / grid.size;
    grid.bitmap_bytes = (grid.count() + 7) / 8;
    return grid;
}

bool tile_is_marked(const uint8_t* bitmap, std::size_t index) {
    return (bitmap[index / 8] & (1u << (index % 8))) != 0;
}

}  // namespace

bool encode_screen_pixels(const uint32_t* pixels,
                          int width,
                          int height,
                          int stride_pixels,
                          std::vector<uint8_t>& out,
                          PixelChannelOrder channel_order) {
    out.clear();
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    std::vector<uint8_t> raw;
    pack_rgb565(pixels, width, height, stride_pixels, channel_order == PixelChannelOrder::Rgba,
                raw);
    return deflate_onto(raw, out);
}

bool encode_screen_tiles(const uint32_t* pixels,
                         int width,
                         int height,
                         int stride_pixels,
                         std::vector<uint8_t>& previous,
                         std::vector<uint8_t>& out,
                         PixelChannelOrder channel_order,
                         const std::vector<const std::vector<uint8_t>*>& also_unlike) {
    out.clear();
    if (pixels == nullptr || width <= 0 || height <= 0) {
        return false;
    }

    std::vector<uint8_t> raw;
    pack_rgb565(pixels, width, height, stride_pixels, channel_order == PixelChannelOrder::Rgba,
                raw);

    const TileGrid grid = tile_grid(width, height, kFrameTileShift);
    // Nothing to compare against, or something of a different shape: every
    // tile goes, which is a whole frame in tile order.
    const bool everything = (previous.size() != raw.size());

    std::vector<uint8_t> bitmap(grid.bitmap_bytes, 0);
    std::vector<uint8_t> body;
    body.reserve(raw.size());

    std::size_t index = 0;
    for (int down = 0; down < grid.down; ++down) {
        const int top = down * grid.size;
        const int rows = std::min(grid.size, height - top);
        for (int across = 0; across < grid.across; ++across, ++index) {
            const int left = across * grid.size;
            const std::size_t row_bytes = static_cast<std::size_t>(
                                              std::min(grid.size, width - left)) * 2;

            bool changed = everything;
            for (int row = 0; !changed && row < rows; ++row) {
                const std::size_t offset =
                    (static_cast<std::size_t>(top + row) * width + left) * 2;
                changed = std::memcmp(&raw[offset], &previous[offset], row_bytes) != 0;
            }
            for (const std::vector<uint8_t>* other : also_unlike) {
                if (changed) break;
                if (other == nullptr || other->size() != raw.size()) {
                    changed = true;
                    break;
                }
                for (int row = 0; !changed && row < rows; ++row) {
                    const std::size_t offset =
                        (static_cast<std::size_t>(top + row) * width + left) * 2;
                    changed = std::memcmp(&raw[offset], other->data() + offset, row_bytes) != 0;
                }
            }
            if (!changed) {
                continue;
            }
            bitmap[index / 8] |= static_cast<uint8_t>(1u << (index % 8));
            for (int row = 0; row < rows; ++row) {
                const std::size_t offset =
                    (static_cast<std::size_t>(top + row) * width + left) * 2;
                body.insert(body.end(), raw.begin() + offset, raw.begin() + offset + row_bytes);
            }
        }
    }

    out.push_back(static_cast<uint8_t>(kFrameTileShift));
    out.insert(out.end(), bitmap.begin(), bitmap.end());
    if (!deflate_onto(body, out)) {
        return false;
    }
    previous = std::move(raw);
    return true;
}

bool decode_screen_rgb565(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint8_t>& out) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(width) * height * 2);
    uLongf raw_size = static_cast<uLongf>(out.size());
    if (uncompress(out.data(), &raw_size, payload.data(), static_cast<uLong>(payload.size())) !=
            Z_OK ||
        raw_size != out.size()) {
        return false;
    }
    return true;
}

bool decode_screen_tiles(const std::vector<uint8_t>& payload,
                         int width,
                         int height,
                         std::vector<uint8_t>& screen) {
    if (width <= 0 || height <= 0 || payload.empty()) {
        return false;
    }
    const int shift = payload[0];
    // Anything outside this is not a grid any sender here produces, and a
    // wild shift would size the bitmap from a number this end was told.
    if (shift < 2 || shift > 8) {
        return false;
    }
    const TileGrid grid = tile_grid(width, height, shift);
    if (payload.size() < 1 + grid.bitmap_bytes) {
        return false;
    }
    const uint8_t* bitmap = payload.data() + 1;

    const std::size_t frame_bytes = static_cast<std::size_t>(width) * height * 2;
    if (screen.size() != frame_bytes) {
        // Nothing to patch onto yet. Black rather than whatever was here, so a
        // payload that turns out not to cover the screen shows as missing
        // rather than as somebody else's pixels.
        screen.assign(frame_bytes, 0);
    }

    // What the marked tiles come to, so the inflate is bounded by what was
    // actually asked for rather than by anything the sender claims.
    std::size_t body_bytes = 0;
    for (std::size_t index = 0; index < grid.count(); ++index) {
        if (!tile_is_marked(bitmap, index)) {
            continue;
        }
        const int top = static_cast<int>(index / grid.across) * grid.size;
        const int left = static_cast<int>(index % grid.across) * grid.size;
        body_bytes += static_cast<std::size_t>(std::min(grid.size, height - top)) *
                      static_cast<std::size_t>(std::min(grid.size, width - left)) * 2;
    }
    if (body_bytes == 0) {
        return true;  // nothing moved on this screen
    }

    std::vector<uint8_t> body(body_bytes);
    uLongf body_size = static_cast<uLongf>(body.size());
    const std::size_t header = 1 + grid.bitmap_bytes;
    if (uncompress(body.data(), &body_size, payload.data() + header,
                   static_cast<uLong>(payload.size() - header)) != Z_OK ||
        body_size != body.size()) {
        return false;
    }

    std::size_t read = 0;
    for (std::size_t index = 0; index < grid.count(); ++index) {
        if (!tile_is_marked(bitmap, index)) {
            continue;
        }
        const int top = static_cast<int>(index / grid.across) * grid.size;
        const int left = static_cast<int>(index % grid.across) * grid.size;
        const int rows = std::min(grid.size, height - top);
        const std::size_t row_bytes = static_cast<std::size_t>(std::min(grid.size, width - left)) * 2;
        for (int row = 0; row < rows; ++row) {
            const std::size_t offset = (static_cast<std::size_t>(top + row) * width + left) * 2;
            std::memcpy(&screen[offset], &body[read], row_bytes);
            read += row_bytes;
        }
    }
    return true;
}

void expand_rgb565(const std::vector<uint8_t>& raw, std::vector<uint32_t>& out) {
    const std::size_t pixel_count = raw.size() / 2;
    out.resize(pixel_count);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        const uint16_t packed =
            static_cast<uint16_t>(raw[index * 2] | (raw[index * 2 + 1] << 8));
        // Replicate the high bits into the low ones so white stays white.
        const uint32_t r = ((packed >> 11) & 0x1fu);
        const uint32_t g = ((packed >> 5) & 0x3fu);
        const uint32_t b = (packed & 0x1fu);
        out[index] = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) |
                     ((b << 3) | (b >> 2));
    }
}

bool decode_screen_pixels(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint32_t>& out) {
    std::vector<uint8_t> raw;
    if (!decode_screen_rgb565(payload, width, height, raw)) {
        return false;
    }
    expand_rgb565(raw, out);
    return true;
}

}  // namespace ds::net
