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

    void draw_solid(const PixelRect& rect, const Color& color);
    // Draws an XRGB8888 screen texture; alpha comes from `tint`, not the texture.
    void draw_screen(const PixelRect& rect, const Texture& texture, const Color& tint = Color{});
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
    enum class Mode { Solid = 0, Screen = 1, AlphaMask = 2 };

    void draw(const PixelRect& rect,
              const Texture* texture,
              const Color& color,
              Mode mode,
              float u0,
              float v0,
              float u1,
              float v1);

    GLuint program_ = 0;
    GLuint vertex_buffer_ = 0;
    GLint attr_position_ = -1;
    GLint attr_texcoord_ = -1;
    GLint uniform_viewport_ = -1;
    GLint uniform_color_ = -1;
    GLint uniform_mode_ = -1;
    GLint uniform_sampler_ = -1;

    int output_width_ = 0;
    int output_height_ = 0;
};

}  // namespace ds
