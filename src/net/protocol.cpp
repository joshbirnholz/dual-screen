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

uint8_t audio_target_to_wire(AudioTarget target) {
    switch (target) {
        case AudioTarget::TopScreen: return 0u;
        case AudioTarget::BottomScreen: return 1u;
        case AudioTarget::Both: break;
    }
    return 2u;
}

bool audio_target_from_wire(uint8_t value, AudioTarget& out) {
    switch (value) {
        case 0: out = AudioTarget::TopScreen; return true;
        case 1: out = AudioTarget::BottomScreen; return true;
        case 2: out = AudioTarget::Both; return true;
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
    finish_message(out, start, MessageType::Hello);
}

void encode_welcome(const WelcomeMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u16(out, message.protocol_version);
    put_string(out, message.game_title);
    put_string(out, message.rom_sha1);
    put_u8(out, screen_to_wire(message.granted_screen));
    put_u8(out, message.system == SystemId::ThreeDs ? 1u : 0u);
    put_u32(out, message.audio_sample_rate);
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

void encode_input(const InputMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u32(out, message.buttons);
    put_u8(out, message.touching ? 1u : 0u);
    put_u16(out, static_cast<uint16_t>(message.touch_x));
    put_u16(out, static_cast<uint16_t>(message.touch_y));
    put_u16(out, static_cast<uint16_t>(message.circle_x));
    put_u16(out, static_cast<uint16_t>(message.circle_y));
    put_u16(out, static_cast<uint16_t>(message.c_stick_x));
    put_u16(out, static_cast<uint16_t>(message.c_stick_y));
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

void encode_audio(const AudioMessage& message, std::vector<uint8_t>& out) {
    const std::size_t start = begin_message(out);
    put_u8(out, static_cast<uint8_t>(message.stream));
    put_u32(out, message.sample_rate);
    put_u32(out, static_cast<uint32_t>(message.samples.size()));
    for (const int16_t sample : message.samples) {
        put_u16(out, static_cast<uint16_t>(sample));
    }
    finish_message(out, start, MessageType::Audio);
}

bool decode_audio(const uint8_t* data, std::size_t size, AudioMessage& out) {
    Reader reader(data, size);
    uint8_t stream = 0;
    uint32_t count = 0;
    reader.u8(stream);
    reader.u32(out.sample_rate);
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
    if (!reader.ok()) return false;
    out.wants_filtering = (wants_filtering != 0);
    out.bilinear_filtering = (filtering != 0);
    out.wants_audio_target = (wants_audio != 0);
    out.wants_handedness = (wants_hands != 0);
    out.wants_render_scale = (wants_scale != 0);
    out.wants_display_size = (wants_size != 0);
    out.menu_open = (menu_open != 0);
    out.wants_reset = (wants_reset != 0);
    if (!handedness_from_wire(hands, out.handedness)) return false;
    return audio_target_from_wire(audio, out.audio_target);
}

void encode_bye(std::vector<uint8_t>& out) {
    finish_message(out, begin_message(out), MessageType::Bye);
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
    if (!reader.ok() || magic != kProtocolMagic) {
        return false;
    }
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
    if (!reader.ok() || system > 1u) return false;
    out.system = (system == 1u) ? SystemId::ThreeDs : SystemId::Ds;
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
    reader.string(out.layout_id);
    reader.u8(screen);
    reader.u8(paused);
    reader.u8(filtering);
    reader.u8(audio);
    reader.u8(hands);
    reader.u8(out.render_scale);
    if (!reader.ok()) return false;
    out.paused = (paused != 0);
    out.bilinear_filtering = (filtering != 0);
    if (!audio_target_from_wire(audio, out.audio_target)) return false;
    if (!handedness_from_wire(hands, out.handedness)) return false;
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
    if (encoding != static_cast<uint8_t>(FrameEncoding::Rgb565Deflate)) {
        return false;
    }
    out.encoding = FrameEncoding::Rgb565Deflate;
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
    if (!reader.ok()) return false;
    out.touching = (touching != 0);
    out.touch_x = static_cast<int16_t>(x);
    out.touch_y = static_cast<int16_t>(y);
    out.circle_x = static_cast<int16_t>(circle_x);
    out.circle_y = static_cast<int16_t>(circle_y);
    out.c_stick_x = static_cast<int16_t>(c_stick_x);
    out.c_stick_y = static_cast<int16_t>(c_stick_y);
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

// --- pixels -----------------------------------------------------------------

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

    // -> RGB565 halves the bytes before compression, and the two bits per
    // channel this drops are not visible on a DS screen scaled to a TV. Which
    // byte holds red and which holds blue depends on the source; green is
    // always the middle byte either way.
    const bool rgba = (channel_order == PixelChannelOrder::Rgba);
    std::vector<uint8_t> raw(static_cast<std::size_t>(width) * height * 2);
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
            raw[offset++] = static_cast<uint8_t>(packed & 0xffu);
            raw[offset++] = static_cast<uint8_t>((packed >> 8) & 0xffu);
        }
    }

    uLongf bound = compressBound(static_cast<uLong>(raw.size()));
    out.resize(bound);
    // Level 1: this runs once per screen per frame, so time matters far more
    // than the last few percent of ratio.
    const int result = compress2(out.data(), &bound, raw.data(), static_cast<uLong>(raw.size()), 1);
    if (result != Z_OK) {
        out.clear();
        return false;
    }
    out.resize(bound);
    return true;
}

bool decode_screen_pixels(const std::vector<uint8_t>& payload,
                          int width,
                          int height,
                          std::vector<uint32_t>& out) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    std::vector<uint8_t> raw(pixel_count * 2);
    uLongf raw_size = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &raw_size, payload.data(), static_cast<uLong>(payload.size())) != Z_OK ||
        raw_size != raw.size()) {
        return false;
    }

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
    return true;
}

}  // namespace ds::net
