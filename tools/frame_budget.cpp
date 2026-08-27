// What a screen costs on the wire, measured through the encoder that actually
// sends it.
//
// The tile codec was chosen for a console whose screens are flat-shaded: the
// comment on FrameEncoding says as much -- "a few kilobytes, which is what
// makes 60fps over wifi realistic without pulling in a video encoder". A Wii U
// GamePad is 854x480 of moving 3D, which is 8.3x the pixels of a DS screen with
// none of the flatness, so that assumption has to be measured rather than
// assumed either way before anything is built on top of it.
//
// This drives the real encode_screen_tiles(), not a model of it, and reports
// what it produced. Two ways to feed it:
//
//   --scene <still|sprites|pan>   synthesized frames, to bracket the answer
//   --raw <file>                  real frames, XRGB8888, one after another
//
// --dump writes out whatever was fed in, so a different codec can be put to the
// same frames rather than to a description of them.
//
// The scenes are a bracket and nothing more. `sprites` should reproduce the few
// kilobytes a DS screen costs today, which is what says the tool is measuring
// the right thing; `pan` moves everything every frame, which is what a camera
// turning does. Neither is evidence about a particular game. For that, capture
// the real thing and feed it in:
//
//   ffmpeg -i capture.mp4 -f rawvideo -pix_fmt bgra -s 854x480 gamepad.raw
//   frame_budget --raw gamepad.raw --width 854 --height 480 --fps 30
//
// Note also that this measures the encoder alone. It says nothing about whether
// a given link carries the result, or what the latency is once it does.

#include <algorithm>
#include <chrono>
#include <numeric>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "net/protocol.hpp"

namespace {

struct Options {
    int width = 854;
    int height = 480;
    int frames = 300;
    double fps = 30.0;
    std::string scene = "pan";
    std::string raw_path;
    // Writes out whatever it fed the encoder, so another codec can be measured
    // against the same bytes rather than against a description of them.
    std::string dump_path;
};

// Value noise on an 8-pixel lattice, smoothed. Band-limited on purpose: white
// noise is incompressible in a way no renderer's output is, and would make the
// codec look worse than it is.
float lattice(int x, int y, int seed) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u +
                 static_cast<uint32_t>(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

float texture_at(float x, float y, int seed) {
    const float scale = 1.0f / 8.0f;
    const float fx = x * scale;
    const float fy = y * scale;
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    // Smoothstep, so the lattice does not show as a grid of creases.
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    const float a = lattice(x0, y0, seed);
    const float b = lattice(x0 + 1, y0, seed);
    const float c = lattice(x0, y0 + 1, seed);
    const float d = lattice(x0 + 1, y0 + 1, seed);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}

uint32_t pack(float r, float g, float b) {
    const auto clamp8 = [](float v) {
        const int i = static_cast<int>(v * 255.0f + 0.5f);
        return static_cast<uint32_t>(std::max(0, std::min(255, i)));
    };
    return (clamp8(r) << 16) | (clamp8(g) << 8) | clamp8(b);
}

// Something with the spatial statistics of a rendered scene rather than of a
// test card: large smooth-shaded regions, hard edges where they meet, and a
// mid-frequency texture over the top. `shift` scrolls the whole thing, which is
// what a camera turning does to every tile at once.
void draw_scene(std::vector<uint32_t>& out, int width, int height, float shift) {
    out.resize(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float sx = static_cast<float>(x) + shift;
            const float sy = static_cast<float>(y) + shift * 0.35f;
            // Which surface this pixel belongs to. The boundaries are hard,
            // which is where a delta codec's tiles earn or lose their keep.
            const float horizon = static_cast<float>(height) * 0.45f +
                                  std::sin(sx * 0.004f) * static_cast<float>(height) * 0.08f;
            float r, g, b;
            if (static_cast<float>(y) < horizon) {
                // Sky: a broad vertical gradient, very little detail.
                const float t = static_cast<float>(y) / std::max(1.0f, horizon);
                r = 0.30f + 0.35f * t;
                g = 0.45f + 0.35f * t;
                b = 0.70f + 0.25f * t;
                r += texture_at(sx * 0.25f, sy * 0.25f, 3) * 0.04f;
                g += texture_at(sx * 0.25f, sy * 0.25f, 3) * 0.04f;
                b += texture_at(sx * 0.25f, sy * 0.25f, 3) * 0.04f;
            } else {
                // Ground: shaded, textured, and busier the nearer it is.
                const float depth = (static_cast<float>(y) - horizon) /
                                    std::max(1.0f, static_cast<float>(height) - horizon);
                const float detail = texture_at(sx * (0.5f + depth * 2.0f),
                                                sy * (0.5f + depth * 2.0f), 1);
                const float shade = 0.35f + 0.40f * depth;
                r = shade * (0.55f + detail * 0.45f);
                g = shade * (0.60f + detail * 0.40f);
                b = shade * (0.40f + detail * 0.35f);
            }
            // A couple of hard-edged objects, so not every edge is the horizon.
            const float ox = std::fmod(sx * 0.7f, static_cast<float>(width));
            if (ox > static_cast<float>(width) * 0.30f && ox < static_cast<float>(width) * 0.36f &&
                static_cast<float>(y) > horizon * 0.6f) {
                r = 0.18f;
                g = 0.16f;
                b = 0.20f;
            }
            out[static_cast<std::size_t>(y) * width + x] = pack(r, g, b);
        }
    }
}

// What a DS screen mostly is: a flat background with a few things moving on it.
// Here to prove the tool measures what the running app already does -- if this
// does not come out at a few kilobytes, the numbers below mean nothing.
void draw_sprites(std::vector<uint32_t>& out, int width, int height, int frame) {
    out.assign(static_cast<std::size_t>(width) * height, pack(0.12f, 0.20f, 0.35f));
    for (int sprite = 0; sprite < 6; ++sprite) {
        const int size = 24 + sprite * 4;
        const double phase = static_cast<double>(frame) * (0.9 + sprite * 0.3);
        const int cx = static_cast<int>((std::sin(phase * 0.05) * 0.5 + 0.5) *
                                        static_cast<double>(width - size));
        const int cy = static_cast<int>((std::cos(phase * 0.037) * 0.5 + 0.5) *
                                        static_cast<double>(height - size));
        const uint32_t colour = pack(0.2f + 0.13f * static_cast<float>(sprite), 0.8f, 0.3f);
        for (int y = cy; y < std::min(height, cy + size); ++y) {
            for (int x = cx; x < std::min(width, cx + size); ++x) {
                out[static_cast<std::size_t>(y) * width + x] = colour;
            }
        }
    }
}

int percentile(const std::vector<std::size_t>& sorted, double fraction) {
    if (sorted.empty()) return 0;
    const std::size_t index = static_cast<std::size_t>(fraction *
                                                       static_cast<double>(sorted.size() - 1));
    return static_cast<int>(sorted[index]);
}

void usage() {
    std::fprintf(stderr,
                 "frame_budget [--width N] [--height N] [--frames N] [--fps N]\n"
                 "             [--scene still|sprites|pan] [--raw FILE] [--dump FILE]\n");
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--width") options.width = std::atoi(value().c_str());
        else if (arg == "--height") options.height = std::atoi(value().c_str());
        else if (arg == "--frames") options.frames = std::atoi(value().c_str());
        else if (arg == "--fps") options.fps = std::atof(value().c_str());
        else if (arg == "--scene") options.scene = value();
        else if (arg == "--raw") options.raw_path = value();
        else if (arg == "--dump") options.dump_path = value();
        else {
            usage();
            return 2;
        }
    }
    if (options.width <= 0 || options.height <= 0 || options.frames <= 0 || options.fps <= 0.0) {
        usage();
        return 2;
    }

    std::FILE* dump = nullptr;
    if (!options.dump_path.empty()) {
        dump = std::fopen(options.dump_path.c_str(), "wb");
        if (dump == nullptr) {
            std::fprintf(stderr, "frame_budget: cannot write %s\n", options.dump_path.c_str());
            return 1;
        }
    }

    std::FILE* raw = nullptr;
    if (!options.raw_path.empty()) {
        raw = std::fopen(options.raw_path.c_str(), "rb");
        if (raw == nullptr) {
            std::fprintf(stderr, "frame_budget: cannot read %s\n", options.raw_path.c_str());
            return 1;
        }
    }

    const std::size_t pixel_count = static_cast<std::size_t>(options.width) * options.height;
    std::vector<uint32_t> frame(pixel_count);
    // What the recipient was last actually sent, which is what the codec deltas
    // against. Owned by the caller in the real host too.
    std::vector<uint8_t> previous;
    std::vector<uint8_t> encoded;

    std::vector<std::size_t> sizes;
    std::vector<std::size_t> whole_sizes;
    sizes.reserve(static_cast<std::size_t>(options.frames));
    double encode_seconds = 0.0;
    int produced = 0;

    for (int index = 0; index < options.frames; ++index) {
        if (raw != nullptr) {
            if (std::fread(frame.data(), 4, pixel_count, raw) != pixel_count) {
                break;  // ran out of real frames
            }
        } else if (options.scene == "still") {
            if (index == 0) draw_scene(frame, options.width, options.height, 0.0f);
        } else if (options.scene == "sprites") {
            draw_sprites(frame, options.width, options.height, index);
        } else {
            // Three pixels a frame: slow enough to be a pan rather than a cut,
            // fast enough that every tile differs from the one before.
            draw_scene(frame, options.width, options.height, static_cast<float>(index) * 3.0f);
        }

        if (dump != nullptr) std::fwrite(frame.data(), 4, pixel_count, dump);

        const auto started = std::chrono::steady_clock::now();
        if (!ds::net::encode_screen_tiles(frame.data(), options.width, options.height,
                                          options.width, previous, encoded)) {
            std::fprintf(stderr, "frame_budget: the encoder refused frame %d\n", index);
            return 1;
        }
        encode_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
                              .count();
        sizes.push_back(encoded.size());

        // What the same frame would cost with no delta at all, so the tiles can
        // be credited with what they actually save.
        std::vector<uint8_t> whole;
        if (ds::net::encode_screen_pixels(frame.data(), options.width, options.height,
                                          options.width, whole)) {
            whole_sizes.push_back(whole.size());
        }
        ++produced;
    }
    if (raw != nullptr) std::fclose(raw);
    if (dump != nullptr) std::fclose(dump);

    if (produced == 0) {
        std::fprintf(stderr, "frame_budget: no frames\n");
        return 1;
    }

    // The first frame is whole by construction -- there is nothing to delta
    // against -- so it is reported apart rather than averaged into a steady
    // state it is not part of.
    const std::size_t first = sizes.front();
    std::vector<std::size_t> steady(sizes.begin() + (sizes.size() > 1 ? 1 : 0), sizes.end());
    std::sort(steady.begin(), steady.end());
    const double total = static_cast<double>(
        std::accumulate(steady.begin(), steady.end(), static_cast<std::size_t>(0)));
    const double mean = total / static_cast<double>(steady.size());

    std::sort(whole_sizes.begin(), whole_sizes.end());

    std::printf("source        %s\n",
                raw != nullptr ? options.raw_path.c_str() : options.scene.c_str());
    std::printf("screen        %dx%d, %d frames at %.0f fps\n", options.width, options.height,
                produced, options.fps);
    std::printf("first frame   %zu bytes (whole, nothing to delta against)\n", first);
    std::printf("per frame     mean %.0f  median %d  p95 %d  max %d bytes\n", mean,
                percentile(steady, 0.50), percentile(steady, 0.95), percentile(steady, 1.0));
    if (!whole_sizes.empty()) {
        std::printf("undeltaed     median %d bytes -- what the tiles are saving against\n",
                    percentile(whole_sizes, 0.50));
    }
    std::printf("bitrate       mean %.1f Mbit/s, p95 %.1f Mbit/s at %.0f fps\n",
                mean * 8.0 * options.fps / 1e6,
                static_cast<double>(percentile(steady, 0.95)) * 8.0 * options.fps / 1e6,
                options.fps);
    std::printf("encode cost   %.2f ms per frame (%.0f%% of a %.1f ms budget)\n",
                encode_seconds * 1000.0 / static_cast<double>(produced),
                encode_seconds * options.fps * 100.0 / static_cast<double>(produced),
                1000.0 / options.fps);
    return 0;
}
