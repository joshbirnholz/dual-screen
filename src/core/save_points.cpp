#include "core/save_points.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

#include <SDL.h>
#include <zlib.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#elif defined(__SWITCH__)
#include <switch.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <langinfo.h>
#include <locale.h>
#endif

#include "config/paths.hpp"
#include "core/screen_source.hpp"

namespace ds {

namespace {

// The file, all little-endian:
//
//   "DSSP"  u32 version  i64 saved_at
//   u16 thumbnail width  u16 thumbnail height
//   u32 compressed thumbnail size, then that many bytes (zlib, XRGB words)
//   u32 state size  u32 compressed state size, then that many bytes (zlib)
//
// The picture comes first so the slot list can read it and stop.
constexpr char kMagic[4] = {'D', 'S', 'S', 'P'};
constexpr uint32_t kVersion = 1;
// Far past anything a DS or a 3DS core writes, and small enough that a
// corrupt size cannot talk this into asking for all of memory.
constexpr uint32_t kMaxStateBytes = 512u * 1024u * 1024u;
constexpr int kMaxThumbnailSide = 2048;

void put_u16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }
}

bool compress_bytes(const uint8_t* data, std::size_t size, std::vector<uint8_t>& out) {
    uLongf bound = compressBound(static_cast<uLong>(size));
    out.resize(bound);
    // Fastest: a 3DS state is tens of megabytes, and the game is paused
    // while this runs.
    if (compress2(out.data(), &bound, data, static_cast<uLong>(size), 1) != Z_OK) return false;
    out.resize(bound);
    return true;
}

// Reads a file in pieces, so the slot list never has to read a state it is
// not going to use.
class Reader {
public:
    explicit Reader(const std::string& path) : file_(std::fopen(path.c_str(), "rb")) {}
    ~Reader() {
        if (file_ != nullptr) std::fclose(file_);
    }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    bool is_open() const { return file_ != nullptr; }
    bool bytes(void* out, std::size_t size) {
        return file_ != nullptr && std::fread(out, 1, size, file_) == size;
    }
    bool u16(uint32_t& value) {
        uint8_t raw[2];
        if (!bytes(raw, sizeof raw)) return false;
        value = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8);
        return true;
    }
    bool u32(uint32_t& value) {
        uint8_t raw[4];
        if (!bytes(raw, sizeof raw)) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) value |= static_cast<uint32_t>(raw[index]) << (index * 8);
        return true;
    }
    bool u64(uint64_t& value) {
        uint8_t raw[8];
        if (!bytes(raw, sizeof raw)) return false;
        value = 0;
        for (int index = 0; index < 8; ++index) value |= static_cast<uint64_t>(raw[index]) << (index * 8);
        return true;
    }

private:
    std::FILE* file_;
};

// Through the thumbnail, leaving `reader` at the state.
bool read_header(Reader& reader, SavePointInfo& info, bool keep_thumbnail) {
    char magic[4];
    uint32_t version = 0;
    uint64_t saved_at = 0;
    if (!reader.bytes(magic, sizeof magic) || !std::equal(magic, magic + 4, kMagic)) return false;
    if (!reader.u32(version) || version != kVersion) return false;
    if (!reader.u64(saved_at)) return false;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t packed_size = 0;
    if (!reader.u16(width) || !reader.u16(height) || !reader.u32(packed_size)) return false;
    if (static_cast<int>(width) > kMaxThumbnailSide || static_cast<int>(height) > kMaxThumbnailSide) {
        return false;
    }
    std::vector<uint8_t> packed(packed_size);
    if (packed_size > 0 && !reader.bytes(packed.data(), packed.size())) return false;

    info.saved_at = static_cast<int64_t>(saved_at);
    info.thumbnail = SavePointThumbnail{};
    if (keep_thumbnail && width > 0 && height > 0) {
        std::vector<uint32_t> pixels(static_cast<std::size_t>(width) * height);
        uLongf size = static_cast<uLongf>(pixels.size() * sizeof(uint32_t));
        if (uncompress(reinterpret_cast<Bytef*>(pixels.data()), &size, packed.data(),
                       static_cast<uLong>(packed.size())) == Z_OK &&
            size == pixels.size() * sizeof(uint32_t)) {
            // Stored little-endian, like everything else in the file.
            for (uint32_t& pixel : pixels) {
                const auto* raw = reinterpret_cast<const uint8_t*>(&pixel);
                pixel = static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) |
                        (static_cast<uint32_t>(raw[2]) << 16) |
                        (static_cast<uint32_t>(raw[3]) << 24);
            }
            info.thumbnail.width = static_cast<int>(width);
            info.thumbnail.height = static_cast<int>(height);
            info.thumbnail.pixels = std::move(pixels);
        }
    }
    return true;
}

// One pixel of a readback as 0x00RRGGBB.
uint32_t xrgb_at(const ScreenReadback& screen, int x, int y) {
    const uint32_t pixel =
        screen.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(screen.stride_pixels) + x];
    if (screen.channel_order == ChannelOrder::Rgba) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&pixel);
        return (static_cast<uint32_t>(bytes[0]) << 16) | (static_cast<uint32_t>(bytes[1]) << 8) |
               bytes[2];
    }
    return pixel & 0x00FFFFFFu;
}

// `screen` shrunk to `height` tall, averaging every source pixel under each
// one -- a DS's text is thin enough that just picking pixels loses it.
void shrink_into(const ScreenReadback& screen, int height, std::vector<uint32_t>& out, int& width) {
    width = std::max(1, (screen.width * height + screen.height / 2) / screen.height);
    out.assign(static_cast<std::size_t>(width) * height, 0);
    for (int y = 0; y < height; ++y) {
        const int y0 = y * screen.height / height;
        const int y1 = std::max(y0 + 1, (y + 1) * screen.height / height);
        for (int x = 0; x < width; ++x) {
            const int x0 = x * screen.width / width;
            const int x1 = std::max(x0 + 1, (x + 1) * screen.width / width);
            uint32_t red = 0, green = 0, blue = 0, count = 0;
            for (int sy = y0; sy < y1; ++sy) {
                for (int sx = x0; sx < x1; ++sx) {
                    const uint32_t pixel = xrgb_at(screen, sx, sy);
                    red += (pixel >> 16) & 0xFF;
                    green += (pixel >> 8) & 0xFF;
                    blue += pixel & 0xFF;
                    ++count;
                }
            }
            out[static_cast<std::size_t>(y) * width + x] =
                ((red / count) << 16) | ((green / count) << 8) | (blue / count);
        }
    }
}

}  // namespace

std::string save_point_path(const std::string& saves_dir, const std::string& rom_path, int slot) {
    return path_join(saves_dir, path_stem(rom_path) + ".savestate" + std::to_string(slot + 1));
}

std::string resume_point_path(const std::string& saves_dir, const std::string& rom_path) {
    return path_join(saves_dir, path_stem(rom_path) + ".resume");
}

BackgroundSavePointWriter::BackgroundSavePointWriter() : thread_([this] { run(); }) {}

BackgroundSavePointWriter::~BackgroundSavePointWriter() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void BackgroundSavePointWriter::write(std::string path, std::vector<uint8_t> state,
                                      int64_t saved_at) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job_.path = std::move(path);
        job_.state = std::move(state);
        job_.saved_at = saved_at;
        has_job_ = true;
    }
    wake_.notify_all();
}

void BackgroundSavePointWriter::finish() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return !has_job_ && !busy_; });
}

void BackgroundSavePointWriter::cancel() {
    std::unique_lock<std::mutex> lock(mutex_);
    has_job_ = false;
    job_ = Job();
    idle_.wait(lock, [this] { return !busy_; });
}

unsigned BackgroundSavePointWriter::take_failures(std::string* last_error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const unsigned failures = failures_;
    failures_ = 0;
    if (last_error != nullptr) *last_error = last_error_;
    return failures;
}

void BackgroundSavePointWriter::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [this] { return stopping_ || has_job_; });
        if (!has_job_) {
            break;  // stopping, with nothing left to write
        }
        Job job = std::move(job_);
        job_ = Job();
        has_job_ = false;
        busy_ = true;
        lock.unlock();
        std::string error;
        const bool written =
            write_save_point(job.path, job.state, SavePointThumbnail(), job.saved_at, &error);
        lock.lock();
        busy_ = false;
        if (!written) {
            ++failures_;
            last_error_ = error;
        }
        idle_.notify_all();
    }
    idle_.notify_all();
}

void adopt_old_save_point_names(const std::string& saves_dir, const std::string& rom_path) {
    for (int slot = 0; slot < kSavePointSlots; ++slot) {
        const std::string current = save_point_path(saves_dir, rom_path, slot);
        const std::string old = path_join(
            saves_dir, path_stem(rom_path) + ".savepoint" + std::to_string(slot + 1));
        if (!file_exists(current) && file_exists(old)) {
            std::rename(old.c_str(), current.c_str());
        }
    }
}

bool write_save_point(const std::string& path,
                      const std::vector<uint8_t>& state,
                      const SavePointThumbnail& thumbnail,
                      int64_t saved_at,
                      std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    if (state.empty() || state.size() > kMaxStateBytes) return fail("The game's state is not something that can be saved.");

    std::vector<uint8_t> out(kMagic, kMagic + 4);
    put_u32(out, kVersion);
    put_u64(out, static_cast<uint64_t>(saved_at));

    std::vector<uint8_t> picture;
    if (thumbnail.valid() && thumbnail.width <= kMaxThumbnailSide &&
        thumbnail.height <= kMaxThumbnailSide) {
        std::vector<uint8_t> raw;
        raw.reserve(thumbnail.pixels.size() * 4);
        for (const uint32_t pixel : thumbnail.pixels) put_u32(raw, pixel);
        if (!compress_bytes(raw.data(), raw.size(), picture)) picture.clear();
    }
    put_u16(out, picture.empty() ? 0 : static_cast<uint32_t>(thumbnail.width));
    put_u16(out, picture.empty() ? 0 : static_cast<uint32_t>(thumbnail.height));
    put_u32(out, static_cast<uint32_t>(picture.size()));
    out.insert(out.end(), picture.begin(), picture.end());

    std::vector<uint8_t> packed;
    if (!compress_bytes(state.data(), state.size(), packed)) return fail("The save state could not be compressed.");
    put_u32(out, static_cast<uint32_t>(state.size()));
    put_u32(out, static_cast<uint32_t>(packed.size()));
    out.insert(out.end(), packed.begin(), packed.end());

    std::string why;
    if (!write_file_atomically(path, out.data(), out.size(), &why)) {
        if (error != nullptr) *error = "The save state could not be written: " + why;
        return false;
    }
    return true;
}

bool read_save_point_info(const std::string& path, SavePointInfo& out) {
    out = SavePointInfo{};
    Reader reader(path);
    if (!reader.is_open()) return false;
    SavePointInfo info;
    if (!read_header(reader, info, true)) return false;
    info.exists = true;
    out = std::move(info);
    return true;
}

bool read_save_point_state(const std::string& path, std::vector<uint8_t>& out,
                           std::string* error) {
    const auto fail = [&](const char* why) {
        if (error != nullptr) *error = why;
        return false;
    };
    Reader reader(path);
    if (!reader.is_open()) return fail("That save state is not there any more.");
    SavePointInfo info;
    uint32_t size = 0;
    uint32_t packed_size = 0;
    if (!read_header(reader, info, false) || !reader.u32(size) || !reader.u32(packed_size) ||
        size == 0 || size > kMaxStateBytes || packed_size > kMaxStateBytes) {
        return fail("That save state is damaged.");
    }
    std::vector<uint8_t> packed(packed_size);
    if (!reader.bytes(packed.data(), packed.size())) return fail("That save state is damaged.");
    out.resize(size);
    uLongf unpacked = size;
    if (uncompress(out.data(), &unpacked, packed.data(), static_cast<uLong>(packed.size())) != Z_OK ||
        unpacked != size) {
        out.clear();
        return fail("That save state is damaged.");
    }
    return true;
}

SavePointThumbnail capture_save_point_thumbnail(ScreenSource& source, int height) {
    SavePointThumbnail thumbnail;
    if (height <= 0) return thumbnail;
    // A readback is only good until the next one, so each screen is shrunk
    // before the other is asked for.
    std::vector<uint32_t> halves[2];
    int widths[2] = {0, 0};
    const ScreenId order[2] = {ScreenId::Top, ScreenId::Bottom};
    for (int index = 0; index < 2; ++index) {
        ScreenReadback screen;
        if (!source.read_screen(order[index], screen) || !screen.valid() ||
            is_planar(screen.channel_order)) {
            return thumbnail;
        }
        shrink_into(screen, height, halves[index], widths[index]);
    }
    // One above the other, the way the console holds them, with a dark seam
    // between where the hinge sits. The narrower screen -- a 3DS's bottom one
    // -- is centred under the wider, as it is on the console.
    const int seam = std::max(1, height / 24);
    thumbnail.width = std::max(widths[0], widths[1]);
    thumbnail.height = height * 2 + seam;
    thumbnail.pixels.assign(static_cast<std::size_t>(thumbnail.width) * thumbnail.height, 0);
    for (int half = 0; half < 2; ++half) {
        const int left = (thumbnail.width - widths[half]) / 2;
        const int top = half == 0 ? 0 : height + seam;
        for (int y = 0; y < height; ++y) {
            std::copy_n(halves[half].data() + static_cast<std::size_t>(y) * widths[half],
                        widths[half],
                        thumbnail.pixels.data() +
                            static_cast<std::size_t>(top + y) * thumbnail.width + left);
        }
    }
    return thumbnail;
}

namespace {

bool in(const std::string& code, std::initializer_list<const char*> codes) {
    for (const char* each : codes) {
        if (code == each) return true;
    }
    return false;
}

// The order, and the separator, of a date format written with the letters
// y, M/m and d -- a strftime format ("%m/%d/%Y") or a Unicode one
// ("dd.MM.yyyy") alike. False when it has not got all three.
bool read_date_format(const std::string& format, TimeStyle& style) {
    const auto year = format.find_first_of("yY");
    const auto month = format.find_first_of("mM");
    const auto day = format.find_first_of("d");
    if (year == std::string::npos || month == std::string::npos || day == std::string::npos) {
        return false;
    }
    if (year < month && month < day) {
        style.order = TimeStyle::Order::YearMonthDay;
    } else if (day < month) {
        style.order = TimeStyle::Order::DayMonthYear;
    } else {
        style.order = TimeStyle::Order::MonthDayYear;
    }
    for (const char mark : format) {
        if (mark == '/' || mark == '.' || mark == '-') {
            style.separator = mark;
            break;
        }
    }
    return true;
}

TimeStyle style_from_sdl() {
    TimeStyle style = time_style_for_region("en", "US");
    SDL_Locale* locales = SDL_GetPreferredLocales();
    if (locales != nullptr) {
        if (locales[0].language != nullptr) {
            style = time_style_for_region(locales[0].language,
                                          locales[0].country != nullptr ? locales[0].country : "");
        }
        SDL_free(locales);
    }
    return style;
}

TimeStyle ask_the_device() {
#if defined(__APPLE__)
    // The user's own choices, 24-Hour Time included, which is more than the
    // region alone would say.
    TimeStyle style;
    CFLocaleRef locale = CFLocaleCopyCurrent();
    const auto pattern = [&](CFStringRef skeleton) {
        std::string out;
        CFStringRef format = CFDateFormatterCreateDateFormatFromTemplate(nullptr, skeleton, 0, locale);
        if (format != nullptr) {
            char buffer[64] = {};
            if (CFStringGetCString(format, buffer, sizeof buffer, kCFStringEncodingUTF8)) out = buffer;
            CFRelease(format);
        }
        return out;
    };
    const std::string date = pattern(CFSTR("yyyyMMdd"));
    const std::string hour = pattern(CFSTR("j"));
    CFRelease(locale);
    if (!read_date_format(date, style)) return style_from_sdl();
    style.twelve_hour = hour.find('a') != std::string::npos || hour.find('h') != std::string::npos;
    return style;
#elif defined(__SWITCH__)
    // A Switch has no 12/24-hour setting of its own: it goes by its language,
    // which carries the region with it ("en-US", "en-GB", "fr-CA").
    TimeStyle style = time_style_for_region("en", "US");
    if (R_SUCCEEDED(setInitialize())) {
        u64 code = 0;
        if (R_SUCCEEDED(setGetSystemLanguage(&code))) {
            char text[9] = {};
            std::memcpy(text, &code, sizeof code);
            const std::string tag = text;
            const auto dash = tag.find('-');
            style = time_style_for_region(tag.substr(0, dash),
                                          dash == std::string::npos ? "" : tag.substr(dash + 1));
        }
        setExit();
    }
    return style;
#elif defined(__linux__) && !defined(__ANDROID__)
    // The locale's own formats, read without changing this program's locale.
    locale_t locale = newlocale(LC_TIME_MASK, "", static_cast<locale_t>(0));
    if (locale == static_cast<locale_t>(0)) return style_from_sdl();
    const std::string date = nl_langinfo_l(D_FMT, locale);
    const std::string time = nl_langinfo_l(T_FMT, locale);
    freelocale(locale);
    // The C locale's own formats: no locale set at all, which is a missing
    // answer rather than an American one.
    if (date == "%m/%d/%y" && time == "%H:%M:%S") return style_from_sdl();
    TimeStyle style;
    if (!read_date_format(date, style)) return style_from_sdl();
    style.twelve_hour = time.find("%p") != std::string::npos || time.find("%r") != std::string::npos ||
                        time.find("%I") != std::string::npos || time.find("%l") != std::string::npos;
    return style;
#else
    return style_from_sdl();
#endif
}

}  // namespace

TimeStyle time_style_for_region(const std::string& language_in, const std::string& country_in) {
    std::string language = language_in;
    std::string country = country_in;
    for (char& letter : language) letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    for (char& letter : country) letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
    if (country.empty()) {
        // A language alone: where it is mostly written from.
        if (language == "en") country = "US";
        else if (language == "ja") country = "JP";
        else if (language == "zh") country = "CN";
        else if (language == "ko") country = "KR";
        else if (language == "de") country = "DE";
        else if (language == "nl") country = "NL";
        else if (language == "ru") country = "RU";
    }

    TimeStyle style;
    style.order = TimeStyle::Order::DayMonthYear;
    style.separator = '/';
    if (in(country, {"US", "PH", "FM", "MH", "PR", "GU", "AS", "UM", "VI"})) {
        style.order = TimeStyle::Order::MonthDayYear;
    } else if (in(country, {"JP", "CN", "TW", "KR", "HU", "LT", "MN", "IR"})) {
        style.order = TimeStyle::Order::YearMonthDay;
        if (country == "HU" || country == "KR") style.separator = '.';
    } else if (in(country, {"SE", "CA"}) && language != "fr") {
        style.order = TimeStyle::Order::YearMonthDay;
        style.separator = '-';
    } else if (in(country, {"DE", "AT", "CH", "RU", "PL", "NO", "FI", "DK", "CZ", "SK", "TR",
                            "UA", "RO", "BG", "HR", "SI", "RS", "EE", "LV", "KZ", "BY", "IS"})) {
        style.separator = '.';
    } else if (in(country, {"NL"})) {
        style.separator = '-';
    }
    // Where the clock is read with AM and PM.
    style.twelve_hour =
        in(country, {"US", "AU", "NZ", "IN", "PH", "PK", "BD", "EG", "SA", "MY", "SG", "HK",
                     "TW", "KR", "MX", "CO", "VE", "SV", "HN", "NI", "PR", "JO", "IQ", "AE",
                     "QA", "KW", "OM", "BH", "GU", "AS", "VI"}) ||
        (country == "CA" && language != "fr");
    return style;
}

TimeStyle device_time_style() {
    static const TimeStyle style = ask_the_device();
    return style;
}

std::string format_save_time(int64_t saved_at, const TimeStyle& style) {
    const std::time_t when = static_cast<std::time_t>(saved_at);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &when);
#else
    localtime_r(&when, &local);
#endif
    char date[48];
    const int year = local.tm_year + 1900;
    const int month = local.tm_mon + 1;
    const int day = local.tm_mday;
    const char mark = style.separator;
    switch (style.order) {
        case TimeStyle::Order::MonthDayYear:
            std::snprintf(date, sizeof date, "%02d%c%02d%c%04d", month, mark, day, mark, year);
            break;
        case TimeStyle::Order::DayMonthYear:
            std::snprintf(date, sizeof date, "%02d%c%02d%c%04d", day, mark, month, mark, year);
            break;
        case TimeStyle::Order::YearMonthDay:
            std::snprintf(date, sizeof date, "%04d%c%02d%c%02d", year, mark, month, mark, day);
            break;
    }
    char time[32];
    if (style.twelve_hour) {
        const int hour = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
        std::snprintf(time, sizeof time, "%d:%02d %s", hour, local.tm_min,
                      local.tm_hour < 12 ? "AM" : "PM");
    } else {
        std::snprintf(time, sizeof time, "%02d:%02d", local.tm_hour, local.tm_min);
    }
    return std::string(date) + " - " + time;
}

std::string format_save_time(int64_t saved_at) {
    return format_save_time(saved_at, device_time_style());
}

}  // namespace ds
