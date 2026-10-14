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
    // `target_scale` draws the same picture smaller: everything is still laid
    // out in output_width x output_height, and lands in a target that much
    // smaller -- the quick menu drawn for Cemu's window, which the link caps
    // in size, laid out as this window's own copy is so that both scroll and
    // place alike.
    void begin_frame(int output_width, int output_height, float target_scale = 1.0f);
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
    // An LCD's pixel grid over `rect`: the edges of `cells_across` by
    // `cells_down` pixels darkened in `color`, its alpha the darkest an edge
    // gets. Nothing is sampled: it is laid over a screen already drawn.
    void draw_lcd_grid(const PixelRect& rect, int cells_across, int cells_down, const Color& color);
    // Draws an RGBA picture -- uploaded with upload_rgba8888() -- keeping its
    // own transparency, which a screen has none of: a game's icon.
    void draw_image(const PixelRect& rect, const Texture& texture, const Color& tint = Color{});
    // A 1px-per-side outline drawn as four solid quads.
    void draw_outline(const PixelRect& rect, const Color& color, float thickness);

    // A capsule: a rectangle with its two short ends rounded into
    // semicircles. Drawn as a solid middle and two caps cut from a disc,
    // because this shader has no rounded primitive and a curve sampled from a
    // texture is both cheaper to add and smoother at the edge than teaching
    // it one. The disc is built once, on first use.
    //
    // The radius is always half the height, which is what makes it a capsule
    // rather than a rounded rectangle: there is no radius to choose, so two
    // capsules of different widths always agree.
    void draw_capsule(const PixelRect& rect, const Color& color);
    // A rectangle with its corners rounded to `radius` -- the capsule's disc,
    // cut in quarters.
    void draw_rounded_rect(const PixelRect& rect, float radius, const Color& color);
    // The ring round the inside edge of a rounded rectangle, `thickness`
    // wide, its colour running from `left` to `right` across it -- or, as
    // thick as the rect, the whole of it filled, the edges smooth. Drawn in
    // one quad, nothing under it covered but the ring itself.
    void draw_rounded_ring(const PixelRect& rect, float radius, float thickness, const Color& left,
                           const Color& right);
    // The same with each corner's own radius, clockwise from the top left.
    void draw_rounded_ring(const PixelRect& rect, const float radii[4], float thickness,
                           const Color& left, const Color& right);

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
        Image = 6,
        LcdGrid = 7,
        RoundedRing = 8,
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
    // Which slot of the vertex buffer the next quad goes in -- see
    // kQuadsPerBuffer in the definition.
    std::size_t next_quad_ = 0;
    // Fresh storage for the buffer, so nothing written from here on has to
    // wait for a draw the GPU has not done yet.
    void orphan_vertex_buffer();
    GLuint vertex_array_ = 0;
    GLint attr_position_ = -1;
    GLint attr_texcoord_ = -1;
    GLint uniform_viewport_ = -1;
    GLint uniform_color_ = -1;
    GLint uniform_mode_ = -1;
    GLint uniform_shape_ = -1;
    GLint uniform_color2_ = -1;
    GLint uniform_radii_ = -1;
    float ring_radii_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    // The next rounded ring's shape and right-hand colour -- see
    // draw_rounded_ring().
    float ring_shape_[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    Color ring_color2_{};
    GLint uniform_sampler_ = -1;
    GLint uniform_chroma_ = -1;
    GLint uniform_chroma_v_ = -1;
    GLint uniform_chroma_v_in_alpha_ = -1;

    // The alpha disc the capsule caps are cut from. Built on first use rather
    // than at start-up: most of what this renderer draws is rectangles, and a
    // build that never draws a capsule never pays for it.
    Texture disc_;
    bool build_disc();

    int output_width_ = 0;
    float target_scale_ = 1.0f;
    int output_height_ = 0;
};

}  // namespace ds
