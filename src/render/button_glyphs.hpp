#pragma once

#include "render/quad_renderer.hpp"
#include "render/texture.hpp"

namespace ds {

// The controller buttons a control prompt names, drawn as the console's own
// symbols. Always the Nintendo positions -- the A that means "confirm" is the
// east button whatever the pad in the player's hands calls it -- because the
// prompt is naming the thing the game names, not the hardware.
enum class ButtonGlyph { A, B, X, Y, LeftStick, RightStick, Plus, Minus, Count };

// The strip of button artwork, drawn a cell at a time. Every glyph is a black
// disc with its letter knocked out of it, so the letter picks up whatever is
// behind the prompt and one image reads correctly on a dark backdrop and on a
// white dialog alike.
class ButtonGlyphSheet {
public:
    bool initialize(std::string* error);
    void shutdown() { texture_.destroy(); }
    bool valid() const { return texture_.valid(); }

    // How wide a glyph is when drawn `height` tall, so a caller can lay out a
    // line of prompts before drawing any of it.
    float width_for(float height) const;
    void draw(QuadRenderer& quads, ButtonGlyph glyph, float x, float y, float height) const;

private:
    Texture texture_;
    int cell_width_ = 0;
    int cell_height_ = 0;
};

}  // namespace ds
