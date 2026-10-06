#pragma once

#include <map>
#include <string>
#include <vector>

#include "layout/layout.hpp"
#include "render/quad_renderer.hpp"
#include "render/texture.hpp"

namespace ds {

// One scale step is a line of text this many pixels tall. It is the old bitmap
// font's cell height, kept as the unit so every screen's existing spacing holds.
constexpr float kTextLineUnit = 8.0f;

enum class TextAlign { Left, Center, Right };

// Draws strings in the embedded UI font, positioning every glyph by its own
// width and its kerning against its neighbour rather than on a fixed grid.
class TextRenderer {
public:
    bool initialize(std::string* error);
    void shutdown();

    // Width/height in pixels a string will occupy at the given scale.
    static float measure_width(const std::string& text, float scale);
    static float line_height(float scale);
    // The step between lines of one wrapped block. Wider than a line box: a
    // paragraph's descenders and the next line's capitals would otherwise sit
    // a hair apart, which a single row of text in a menu never has to deal with.
    static float wrapped_line_step(float scale);

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
    // Shortens `text` from the end instead, for a title too long for its room.
    static std::string elide_right(const std::string& text, float max_width, float scale);

private:
    // One atlas per size actually drawn. A face rasterised for 24px cannot be
    // stretched to 48px without the softness this whole change is meant to
    // remove, so each size gets its own.
    std::map<int, Texture> atlases_;
    // The face's revision each was uploaded at: a glyph added since (a
    // Japanese name's, say) means uploading it again.
    std::map<int, int> atlas_revisions_;
    const Texture* atlas_for(float scale);
    int default_atlas_width_ = 0;
    int default_atlas_height_ = 0;

public:
    // For the startup diagnostics: an atlas that did not upload is the
    // difference between text and blank rectangles.
    int atlas_width() const { return default_atlas_width_; }
    int atlas_height() const { return default_atlas_height_; }

private:
};

}  // namespace ds
