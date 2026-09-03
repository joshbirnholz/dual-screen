#pragma once

#include <string>

#include "layout/layout.hpp"
#include "render/gl_loader.hpp"
#include "render/texture.hpp"

namespace ds {

struct Color {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

// Draws axis-aligned textured and solid quads in output-pixel coordinates
// (origin top-left). This is deliberately the whole of the renderer: the app is
// a compositor, not a game engine.
class QuadRenderer {
public:
    QuadRenderer() = default;
    ~QuadRenderer();

    QuadRenderer(const QuadRenderer&) = delete;
    QuadRenderer& operator=(const QuadRenderer&) = delete;

    bool initialize(std::string* error);
    void shutdown();
    bool ready() const { return program_ != 0; }

    // Sets the pixel-space viewport all subsequent draws map into.
    void begin_frame(int output_width, int output_height);
    void clear(const Color& color);

    // Confines everything drawn until clear_clip() to `rect`. A layout can now
    // deliberately overflow the box it is drawn into -- a console picture
    // filling a small display runs off the edges -- and a preview tile drawing
    // one must not spill over its neighbours.
    void set_clip(const PixelRect& rect);
    void clear_clip();

    void draw_solid(const PixelRect& rect, const Color& color);
    // Draws an XRGB8888 screen texture; alpha comes from `tint`, not the texture.
    // A quarter turn is a permutation of the corner texture coordinates, so a
    // sideways screen costs exactly as much to draw as an upright one.
    //
    // `smooth` is decided per draw rather than held on the texture, because the
    // same screen can be drawn large in the game and small in a preview tile in
    // the same frame, and how it should be sampled depends on which.
    void draw_screen(const PixelRect& rect,
                     const Texture& texture,
                     const Color& tint = Color{},
                     ScreenRotation rotation = ScreenRotation::None,
                     bool smooth = true);
    // Draws a coverage/alpha texture (a font atlas) in a flat colour.
    void draw_alpha_mask(const PixelRect& rect,
                         const Texture& texture,
                         const Color& color,
                         float u0 = 0.0f,
                         float v0 = 0.0f,
                         float u1 = 1.0f,
                         float v1 = 1.0f);
    // A 1px-per-side outline drawn as four solid quads.
    void draw_outline(const PixelRect& rect, const Color& color, float thickness);

private:
    // Matches the u_mode the fragment shader switches on. The two planar
    // ones are video frames converted while drawing -- see ChannelOrder.
    enum class Mode {
        Solid = 0,
        Screen = 1,
        AlphaMask = 2,
        ScreenRgba = 3,
        ScreenNv12 = 4,
        ScreenYuv420p = 5,
    };

    void draw(const PixelRect& rect,
              const Texture* texture,
              const Color& color,
              Mode mode,
              float u0,
              float v0,
              float u1,
              float v1);
    // The general form: texture coordinates given per corner, clockwise from
    // the top-left of the destination rect.
    void draw_corners(const PixelRect& rect,
                      const Texture* texture,
                      const Color& color,
                      Mode mode,
                      const float corner_uv[8],
                      const bool* smooth = nullptr);

    GLuint program_ = 0;
    GLuint vertex_buffer_ = 0;
    GLuint vertex_array_ = 0;
    GLint attr_position_ = -1;
    GLint attr_texcoord_ = -1;
    GLint uniform_viewport_ = -1;
    GLint uniform_color_ = -1;
    GLint uniform_mode_ = -1;
    GLint uniform_sampler_ = -1;
    GLint uniform_chroma_ = -1;
    GLint uniform_chroma_v_ = -1;
    GLint uniform_chroma_v_in_alpha_ = -1;

    int output_width_ = 0;
    int output_height_ = 0;
};

}  // namespace ds
