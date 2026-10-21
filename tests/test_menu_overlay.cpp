// The quick menu drawn on its own for Cemu to put over a Wii U game: what it
// leaves see-through, and what it hands over. See MenuOverlay.

#include "test_support.hpp"

#include <SDL.h>

#include <string>
#include <vector>

#include "core/system.hpp"
#include "layout/layout.hpp"
#include "layout/layout_registry.hpp"
#include "render/compositor.hpp"
#include "render/gl_loader.hpp"
#include "render/menu_overlay.hpp"
#include "render/quad_renderer.hpp"
#include "render/texture.hpp"

using namespace ds;

namespace {

// Read back bottom row first and premultiplied, handed over top row first and
// straight: a half-covering dark red is dark red at half alpha, not a darker
// red, and nothing at all is nothing.
void test_what_is_read_back_is_turned_the_right_way_up_and_straightened() {
    // Two rows of two pixels; the bottom row (read first) is the covered one.
    const std::vector<uint8_t> read = {
        64, 32, 0, 128, 255, 255, 255, 255,  // bottom
        0, 0, 0, 0, 10, 20, 30, 0,           // top: nothing, and nothing with noise in it
    };
    std::vector<uint8_t> out;
    MenuOverlay::straighten(read, 2, 2, out);
    const std::vector<uint8_t> expected = {
        0, 0, 0, 0, 0, 0, 0, 0,
        128, 64, 0, 128, 255, 255, 255, 255,
    };
    CHECK(out == expected);
}

void test_it_is_drawn_no_bigger_than_the_link_carries() {
    int width = 0;
    int height = 0;
    MenuOverlay::fit(1280, 800, 1920, 1080, width, height);
    CHECK_EQ(width, 1280);
    CHECK_EQ(height, 800);
    // A 2560x1600 display, the same shape, at most 1080 tall.
    MenuOverlay::fit(2560, 1600, 1920, 1080, width, height);
    CHECK_EQ(width, 1728);
    CHECK_EQ(height, 1080);
    MenuOverlay::fit(3840, 1080, 1920, 1080, width, height);
    CHECK_EQ(width, 1920);
    CHECK_EQ(height, 540);
}

class GlFixture {
public:
    bool start() {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) return false;
        initialized_ = true;
        SDL_GL_ResetAttributes();
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        window_ = SDL_CreateWindow("test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (window_ == nullptr) return false;
        context_ = SDL_GL_CreateContext(window_);
        if (context_ == nullptr) return false;
        SDL_GL_MakeCurrent(window_, context_);
        std::string error;
        return gl::load(false, &error) && gl::at_least(3, 3);
    }

    ~GlFixture() {
        if (context_ != nullptr) {
            gl::unload();
            SDL_GL_DeleteContext(context_);
        }
        if (window_ != nullptr) SDL_DestroyWindow(window_);
        if (initialized_) SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    bool initialized_ = false;
};

// Drawn the way the app draws it: cleared to nothing, a dimming layer over
// everything, a panel over part of it. Where there is only the dimming, the
// game shows through by as much as it does in this app's own window; where the
// panel is, nothing of the game does; and drawing the same again is no change.
void test_the_menu_hides_the_game_only_where_it_is_drawn() {
    GlFixture fixture;
    if (!fixture.start()) {
        test::skip("no OpenGL 3.3 context available");
        return;
    }
    CHECK(MenuOverlay::supported());
    QuadRenderer quads;
    std::string error;
    CHECK_MSG(quads.initialize(&error), error);

    MenuOverlay overlay;
    constexpr int kWidth = 8;
    constexpr int kHeight = 4;
    auto draw = [&]() {
        CHECK(overlay.begin(kWidth, kHeight));
        quads.begin_frame(kWidth, kHeight);
        quads.clear(Color{0.0f, 0.0f, 0.0f, 0.0f});
        gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                              GL_ONE_MINUS_SRC_ALPHA);
        quads.draw_solid(PixelRect{0.0f, 0.0f, kWidth, kHeight}, Color{0.0f, 0.0f, 0.0f, 0.5f});
        // An opaque panel over the top-left quarter -- top in pixel space.
        quads.draw_solid(PixelRect{0.0f, 0.0f, kWidth / 2.0f, kHeight / 2.0f},
                         Color{1.0f, 1.0f, 1.0f, 1.0f});
        return overlay.finish();
    };
    CHECK(draw());
    CHECK_EQ(overlay.width(), kWidth);
    CHECK_EQ(overlay.height(), kHeight);
    const std::vector<uint8_t>& pixels = overlay.pixels();
    CHECK_EQ(pixels.size(), static_cast<std::size_t>(kWidth * kHeight * 4));
    auto at = [&](int x, int y, int channel) {
        return static_cast<int>(pixels[(static_cast<std::size_t>(y) * kWidth + x) * 4 + channel]);
    };
    // The panel, at the top left: opaque white.
    CHECK_EQ(at(0, 0, 0), 255);
    CHECK_EQ(at(0, 0, 3), 255);
    // Only dimmed, at the bottom right: black at about half.
    CHECK_EQ(at(kWidth - 1, kHeight - 1, 0), 0);
    CHECK(at(kWidth - 1, kHeight - 1, 3) >= 126 && at(kWidth - 1, kHeight - 1, 3) <= 129);
    CHECK_MSG(!draw(), "the same menu drawn again was reported as a change");
    overlay.forget();
    CHECK(draw());

    // Laid out at twice the target's size and drawn into it: the window's
    // own layout, scaled -- clips included -- rather than laid out afresh.
    CHECK(overlay.begin(kWidth, kHeight));
    quads.begin_frame(kWidth * 2, kHeight * 2, 0.5f);
    quads.clear(Color{0.0f, 0.0f, 0.0f, 0.0f});
    gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    quads.set_clip(PixelRect{0.0f, 0.0f, kWidth, kHeight});
    quads.draw_solid(PixelRect{0.0f, 0.0f, kWidth * 2.0f, kHeight * 2.0f}, Color{1.0f, 1.0f, 1.0f, 1.0f});
    quads.clear_clip();
    overlay.finish();
    CHECK_EQ(at(0, 0, 3), 255);
    CHECK_EQ(at(kWidth / 2 - 1, kHeight / 2 - 1, 3), 255);
    CHECK_MSG(at(kWidth / 2, kHeight / 2, 3) == 0, "the clip was not scaled with the picture");
    CHECK_EQ(at(kWidth - 1, kHeight - 1, 3), 0);
}

// The menu's blurred backdrop is retaken when the composition key changes. A
// companion that joins while the host's menu is up takes it before the
// host's first picture has arrived, so the picture arriving has to change the
// key -- or the menu sits over a black screen for as long as it is up.
void test_a_picture_arriving_changes_the_composition() {
    GlFixture fixture;
    if (!fixture.start()) {
        test::skip("no OpenGL 3.3 context available");
        return;
    }
    QuadRenderer quads;
    std::string error;
    CHECK_MSG(quads.initialize(&error), error);
    const LayoutRegistry registry;
    const Layout& layout = *registry.find("vertical");
    const SystemScreens& screens = system_profile(SystemId::Ds).screens;
    LocalOutputRouter router;
    Compositor compositor;
    const auto key_for = [&](const ScreenTextures& textures) {
        compositor.render(quads, layout, textures, screens, 1280.0f, 800.0f, router);
        return compositor.composition_key();
    };

    const ScreenTextures nothing;
    const std::size_t empty = key_for(nothing);
    CHECK_MSG(empty == key_for(nothing), "nothing drawn twice read as two different pictures");

    Texture bottom;
    const std::vector<uint32_t> pixels(4u * 4u, 0x00ff0000u);
    bottom.upload_xrgb8888(pixels.data(), 4, 4, 4);
    CHECK(bottom.valid());
    ScreenTextures arrived;
    arrived.bottom = &bottom;
    const std::size_t with_picture = key_for(arrived);
    CHECK_MSG(with_picture != empty,
              "a picture arriving left the key as it was, so the backdrop stays black");
    CHECK_MSG(with_picture == key_for(arrived),
              "the same picture drawn twice read as a change, so the backdrop would be "
              "retaken every frame");
}

}  // namespace

int main() {
    test_what_is_read_back_is_turned_the_right_way_up_and_straightened();
    test_it_is_drawn_no_bigger_than_the_link_carries();
    test_the_menu_hides_the_game_only_where_it_is_drawn();
    test_a_picture_arriving_changes_the_composition();
    return test::finish("menu-overlay");
}
