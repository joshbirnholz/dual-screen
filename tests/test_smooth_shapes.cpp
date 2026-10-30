// Switches, the battery, rounded corners, the menus' icons and the buttons'
// artwork were jagged: the round parts were cut from one 128-pixel disc and
// the artwork was a coverage mask, both filtered to whatever size they were
// drawn -- stair-stepped shrunk to a handheld's size, blurred into steps
// grown to a 4K window's. Now the round parts are worked out per pixel and
// the artwork is a distance field, and both edges are about one of the
// target's pixels wide at any size.

#include "test_support.hpp"

#include <SDL.h>

#include <cmath>
#include <string>
#include <vector>

#include "render/button_glyphs.hpp"
#include "render/distance_field.hpp"
#include "render/gl_loader.hpp"
#include "render/quad_renderer.hpp"

using namespace ds;

namespace {

constexpr int kWidth = 300;
constexpr int kHeight = 200;

// A disc's distance field comes back as its distance from the rim, to well
// within a texel, though the mask it is made from only knows the rim as a
// soft pixel.
void test_a_disc_becomes_its_distance() {
    constexpr int kSize = 64;
    constexpr float kRadius = 20.0f;
    std::vector<uint8_t> coverage(kSize * kSize);
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float d = std::hypot(x + 0.5f - kSize * 0.5f, y + 0.5f - kSize * 0.5f);
            coverage[y * kSize + x] = static_cast<uint8_t>(std::clamp(kRadius - d + 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    const std::vector<uint8_t> field = coverage_to_distance_field(coverage, kSize, kSize);
    CHECK(field.size() == coverage.size());
    float worst = 0.0f;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const float inside = kRadius - std::hypot(x + 0.5f - kSize * 0.5f, y + 0.5f - kSize * 0.5f);
            if (std::fabs(inside) > kDistanceFieldSpread - 1.0f) continue;
            const float read = (field[y * kSize + x] - 128.0f) * kDistanceFieldSpread / 128.0f;
            worst = std::max(worst, std::fabs(read - inside));
        }
    }
    CHECK_MSG(worst < 0.35f, "a disc's distance field is off by " + std::to_string(worst) + " texels");

    // Nothing in it at all is all outside, and does not fail.
    const std::vector<uint8_t> empty = coverage_to_distance_field(std::vector<uint8_t>(16 * 16, 0), 16, 16);
    CHECK(empty.size() == 256 && empty[0] == 0 && empty[8 * 16 + 8] == 0);
}

SDL_GLContext make_context(SDL_Window* window) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr) return nullptr;
    SDL_GL_MakeCurrent(window, context);
    std::string error;
    if (!gl::load(false, &error)) return nullptr;
    return context;
}

// The red channel, top row first, of everything drawn so far.
std::vector<int> read_back() {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    gl::ReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    std::vector<int> red(static_cast<std::size_t>(kWidth) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            red[static_cast<std::size_t>(y) * kWidth + x] =
                pixels[(static_cast<std::size_t>(kHeight - 1 - y) * kWidth + x) * 4];
        }
    }
    return red;
}

// Along row `y` from `x0` to `x1`: how many times it crosses from dark to
// light or back, and how many pixels are neither -- the edge's softness.
struct Crossings {
    int edges = 0;
    int soft = 0;
};

Crossings crossings(const std::vector<int>& red, int y, int x0, int x1) {
    Crossings found;
    bool light = red[static_cast<std::size_t>(y) * kWidth + x0] >= 128;
    for (int x = x0; x < x1; ++x) {
        const int value = red[static_cast<std::size_t>(y) * kWidth + x];
        if (value > 16 && value < 239) ++found.soft;
        if ((value >= 128) != light) {
            light = !light;
            ++found.edges;
        }
    }
    return found;
}

void begin(QuadRenderer& quads) {
    quads.begin_frame(kWidth, kHeight);
    quads.clear(Color{0.0f, 0.0f, 0.0f, 1.0f});
}

void test_the_edges_are_one_pixel_wide_at_any_size() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        test::skip("no video");
        return;
    }
    SDL_Window* window = SDL_CreateWindow("t", 0, 0, kWidth, kHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window != nullptr ? make_context(window) : nullptr;
    if (context == nullptr) {
        test::skip("no GL context");
        return;
    }
    std::string error;
    QuadRenderer quads;
    ButtonGlyphSheet glyphs;
    CHECK(quads.initialize(&error) && glyphs.initialize(&error));
    const Color white{1.0f, 1.0f, 1.0f, 1.0f};

    // A switch's track, as large as a 4K window draws one: its rounded end
    // is soft -- not a staircase of whole pixels -- but no softer than a
    // pixel or so where each row crosses it. Rows, so only where the curve
    // is steeper than it is flat; near its top a row runs along it.
    begin(quads);
    quads.draw_capsule(PixelRect{20.0f, 20.0f, 260.0f, 140.0f}, white);
    std::vector<int> red = read_back();
    int soft_rows = 0;
    for (int y = 45; y < 135; ++y) {
        const Crossings row = crossings(red, y, 0, 150);
        CHECK_MSG(row.soft <= 2, "a capsule's edge is " + std::to_string(row.soft) + " pixels soft on row " +
                                     std::to_string(y));
        if (row.soft > 0) ++soft_rows;
    }
    CHECK_MSG(soft_rows > 30, "a capsule's curve is not smoothed: " + std::to_string(soft_rows) + " soft rows");

    // The same in a target drawn at a third of the size, zoomed back up --
    // the quick menu drawn for Cemu's window, a page growing into place: the
    // edge is still a pixel of the target's, not three.
    begin(quads);
    quads.set_zoom(3.0f, 0.0f, 0.0f);
    quads.draw_rounded_rect(PixelRect{10.0f, 10.0f, 60.0f, 40.0f}, 18.0f, white);
    red = read_back();
    for (int y = 50; y < 140; y += 3) {
        const Crossings row = crossings(red, y, 0, 120);
        CHECK_MSG(row.soft <= 2, "a zoomed rounded corner is " + std::to_string(row.soft) +
                                     " pixels soft on row " + std::to_string(y));
    }

    // A filled shape drawn translucent has no seams: every pixel well inside
    // it is the same.
    begin(quads);
    quads.draw_rounded_rect(PixelRect{20.0f, 20.0f, 200.0f, 120.0f}, 30.0f, Color{1.0f, 1.0f, 1.0f, 0.5f});
    red = read_back();
    int lowest = 255;
    int highest = 0;
    for (int y = 25; y < 135; ++y) {
        for (int x = 50; x < 190; ++x) {
            const int value = red[static_cast<std::size_t>(y) * kWidth + x];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
        }
    }
    CHECK_MSG(highest - lowest <= 1, "a translucent rounded rect is uneven inside");

    // A button's artwork from a 64-pixel cell, drawn nearly three times that:
    // its edges sharp, where a filtered mask would be three pixels of blur.
    begin(quads);
    glyphs.draw(quads, ButtonGlyph::B, 10.0f, 10.0f, 180.0f, white);
    red = read_back();
    int edges = 0;
    int soft = 0;
    for (int y = 30; y < 170; y += 7) {
        const Crossings row = crossings(red, y, 0, kWidth);
        edges += row.edges;
        soft += row.soft;
    }
    CHECK_MSG(edges > 20, "the button's artwork did not draw");
    CHECK_MSG(soft <= edges * 2, "a large button's edges are " + std::to_string(soft) + " soft pixels over " +
                                     std::to_string(edges) + " edges");

    glyphs.shutdown();
    quads.shutdown();
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

}  // namespace

int main() {
    test_a_disc_becomes_its_distance();
    test_the_edges_are_one_pixel_wide_at_any_size();
    return test::finish("smooth-shapes");
}
