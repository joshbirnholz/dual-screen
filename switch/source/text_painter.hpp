#pragma once

#include <SDL.h>

#include <string>

namespace ds::sw {

// Draws text with the app's own built-in 5x7 font, through SDL's renderer.
//
// The desktop build draws the same glyphs through its GL text renderer. This
// client has no GL renderer of its own -- it draws everything through SDL's,
// which is what it also scales the streamed screens with -- so the atlas is
// uploaded once as a single texture here and the glyphs are blitted out of it.
// A Switch has no system font to borrow, which is why the font is built in at
// all.
class TextPainter {
public:
    ~TextPainter();

    bool create(SDL_Renderer* renderer);
    void destroy();

    // `scale` is whole pixels per font pixel; anything else makes a 5x7 glyph
    // ragged, since there is no room in it to lose a row.
    void draw(SDL_Renderer* renderer,
              const std::string& text,
              float x,
              float y,
              int scale,
              SDL_Color colour) const;

    static float width_of(const std::string& text, int scale);
    static float height_of(int scale);

private:
    SDL_Texture* atlas_ = nullptr;
};

}  // namespace ds::sw
