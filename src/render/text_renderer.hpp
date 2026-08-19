#pragma once

#include <string>
#include <vector>

#include "layout/layout.hpp"
#include "render/quad_renderer.hpp"
#include "render/texture.hpp"

namespace ds {

enum class TextAlign { Left, Center, Right };

// Draws strings from the built-in bitmap font. Scale is an integer multiplier
// so glyphs stay pixel-crisp at any window size.
class TextRenderer {
public:
    bool initialize(std::string* error);
    void shutdown();

    // Width/height in pixels a string will occupy at the given scale.
    static float measure_width(const std::string& text, float scale);
    static float line_height(float scale);

    void draw(QuadRenderer& quads,
              const std::string& text,
              float x,
              float y,
              float scale,
              const Color& color,
              TextAlign align = TextAlign::Left);

    // Word-wraps `text` to `max_width` and draws it, returning the height used.
    float draw_wrapped(QuadRenderer& quads,
                       const std::string& text,
                       float x,
                       float y,
                       float max_width,
                       float scale,
                       const Color& color,
                       TextAlign align = TextAlign::Left);

    static std::vector<std::string> wrap(const std::string& text, float max_width, float scale);

    // Shortens `text` from the left to fit `max_width`, keeping the tail. Paths
    // are the common case and their last components are the informative part.
    static std::string elide_left(const std::string& text, float max_width, float scale);

private:
    Texture atlas_;
};

}  // namespace ds
