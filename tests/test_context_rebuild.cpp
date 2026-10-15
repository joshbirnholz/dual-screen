// The window is rebuilt for a 3DS game, which needs a core-profile context:
// every texture the menus keep has to be made again in the new one. One left
// behind kept a name from the old context, the new context gave that name to
// something else, and the layout picker's Swap Screens button drew the whole
// button sheet while the stylus hand drew the swap arrows.

#include "test_support.hpp"

#include <SDL.h>

#include <string>
#include <vector>

#include "render/button_glyphs.hpp"
#include "render/gl_loader.hpp"
#include "render/quad_renderer.hpp"
#include "render/text_renderer.hpp"

using namespace ds;

namespace {

constexpr int kWidth = 200;
constexpr int kHeight = 100;

SDL_GLContext make_context(SDL_Window* window, bool core) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        core ? SDL_GL_CONTEXT_PROFILE_CORE : SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, core ? 3 : 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, core ? 3 : 1);
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == nullptr) return nullptr;
    SDL_GL_MakeCurrent(window, context);
    std::string error;
    if (!gl::load(false, &error)) return nullptr;
    return context;
}

// How much of a square is lit, by quarter: the swap arrows are two upright
// strokes, so their corners are empty; the button sheet fills its top left.
float lit_in(const std::vector<unsigned char>& pixels, int x0, int y0, int size) {
    int lit = 0;
    for (int y = y0; y < y0 + size; ++y) {
        for (int x = x0; x < x0 + size; ++x) {
            // Read back bottom row first.
            const std::size_t at = (static_cast<std::size_t>(kHeight - 1 - y) * kWidth + x) * 4;
            if (pixels[at] > 128) ++lit;
        }
    }
    return static_cast<float>(lit) / static_cast<float>(size * size);
}

void test_the_symbols_survive_a_new_context() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return;
    SDL_Window* window = SDL_CreateWindow("t", 0, 0, kWidth, kHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == nullptr) return;
    SDL_GLContext first = make_context(window, false);
    if (first == nullptr) return;
    std::string error;
    QuadRenderer quads;
    TextRenderer text;
    ButtonGlyphSheet glyphs;
    CHECK(quads.initialize(&error) && text.initialize(&error) && glyphs.initialize(&error));
    // As App::Impl::destroy_window() does.
    glyphs.shutdown();
    text.shutdown();
    quads.shutdown();
    SDL_GL_DeleteContext(first);
    SDL_DestroyWindow(window);

    window = SDL_CreateWindow("t", 0, 0, kWidth, kHeight, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext second = make_context(window, true);
    if (second == nullptr) return;
    CHECK(quads.initialize(&error) && text.initialize(&error) && glyphs.initialize(&error));
    quads.begin_frame(kWidth, kHeight);
    quads.clear(Color{0.0f, 0.0f, 0.0f, 1.0f});
    glyphs.draw_swap_screens(quads, PixelRect{0.0f, 0.0f, 100.0f, 100.0f}, Color{1, 1, 1, 1});
    glyphs.draw_stylus_hand(quads, PixelRect{100.0f, 0.0f, 100.0f, 100.0f}, Color{1, 1, 1, 1}, false);
    std::vector<unsigned char> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    gl::ReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // The arrows: their top-left corner empty, their middle columns not.
    CHECK_MSG(lit_in(pixels, 2, 2, 18) < 0.02f, "the Swap Screens symbol is not the swap arrows");
    CHECK(lit_in(pixels, 20, 30, 60) > 0.05f);
    // The hand: a picture, not two upright arrows -- its bottom right is filled.
    CHECK_MSG(lit_in(pixels, 170, 60, 20) > 0.3f, "the stylus hand is not the hand");

    glyphs.shutdown();
    text.shutdown();
    quads.shutdown();
    SDL_GL_DeleteContext(second);
    SDL_DestroyWindow(window);
    SDL_Quit();
}

}  // namespace

int main() {
    test_the_symbols_survive_a_new_context();
    return test::finish("context-rebuild");
}
