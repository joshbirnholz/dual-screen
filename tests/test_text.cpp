#include <string>
#include <vector>

#include "render/font.hpp"
#include "render/text_renderer.hpp"
#include "test_support.hpp"

using namespace ds;

namespace {

void test_letters_take_the_width_of_their_own_shape() {
    const float narrow = TextRenderer::measure_width("iiii", 3.0f);
    const float wide = TextRenderer::measure_width("WWWW", 3.0f);
    CHECK_MSG(narrow > 0.0f && wide > 0.0f, "measuring text at a normal size returned nothing");
    CHECK_MSG(wide > narrow * 2.0f,
              "four Ws measured " + std::to_string(wide) + "px against four is at " +
                  std::to_string(narrow) + "px, which is the spacing of a fixed-width font");
}

void test_a_wider_scale_draws_wider_text() {
    const float small = TextRenderer::measure_width("Wii U", 2.0f);
    const float large = TextRenderer::measure_width("Wii U", 4.0f);
    CHECK_MSG(large > small * 1.8f,
              "doubling the scale took the same string from " + std::to_string(small) + "px to " +
                  std::to_string(large) + "px");
}

void test_a_pair_that_kerns_sits_closer_than_its_advances() {
    const FontFace& face = font_face(24.0f);
    CHECK_MSG(face.valid(), "the embedded font did not rasterise");
    const Glyph* a = face.glyph('A');
    const Glyph* v = face.glyph('V');
    CHECK_MSG(a != nullptr && v != nullptr, "the font is missing letters the UI draws");
    CHECK_MSG(face.kerning('A', 'V') < 0.0f,
              "A and V were not kerned together, so the font's own spacing is being ignored");
}

void test_wrapping_measures_the_line_rather_than_counting_letters() {
    // Every line has to fit the width it was given, which a count of characters
    // cannot promise once the letters are different widths.
    // Deliberately mixed: a run of wide letters is short enough to pass a
    // count of characters and far too wide to fit the line it is given.
    const std::string text =
        "iiiiiiiiii WWWWWWWWWW illicit MMMMMMMMMM lilt mimsy wobbling minimalism";
    const float scale = 3.0f;
    const float width = 200.0f;
    const std::vector<std::string> lines = TextRenderer::wrap(text, width, scale);
    CHECK_MSG(lines.size() > 1, "a long string was not wrapped at all");
    for (const std::string& line : lines) {
        CHECK_MSG(TextRenderer::measure_width(line, scale) <= width,
                  "a wrapped line ran " + std::to_string(TextRenderer::measure_width(line, scale)) +
                      "px past a " + std::to_string(width) + "px limit: " + line);
    }
}

void test_eliding_keeps_the_tail_and_fits() {
    const std::string path = "/home/deck/Emulation/roms/nds/Mario Kart DS.nds";
    const float scale = 3.0f;
    const float width = 160.0f;
    const std::string elided = TextRenderer::elide_left(path, width, scale);
    CHECK_MSG(TextRenderer::measure_width(elided, scale) <= width,
              "an elided path still did not fit: " + elided);
    CHECK_MSG(elided.size() >= 4 && elided.compare(0, 3, "...") == 0,
              "an elided path lost its marker: " + elided);
    CHECK_MSG(path.compare(path.size() - 4, 4, elided.substr(elided.size() - 4)) == 0,
              "eliding kept the head rather than the informative tail: " + elided);
}

void test_text_that_fits_is_left_alone() {
    const std::string text = "Load a game";
    const float wide_enough = TextRenderer::measure_width(text, 3.0f) + 1.0f;
    CHECK_MSG(TextRenderer::elide_left(text, wide_enough, 3.0f) == text,
              "a string that already fitted was elided anyway");
}

void test_utf8_is_read_as_characters_rather_than_bytes() {
    const std::vector<uint32_t> codepoints = decode_utf8("a\xE2\x80\xA6");  // "a" then an ellipsis
    CHECK_MSG(codepoints.size() == 2,
              "a two-character string decoded to " + std::to_string(codepoints.size()) +
                  " codepoints");
    CHECK_MSG(codepoints[1] == 0x2026, "the ellipsis did not decode to its own codepoint");
}

}  // namespace

int main() {
    test_letters_take_the_width_of_their_own_shape();
    test_a_wider_scale_draws_wider_text();
    test_a_pair_that_kerns_sits_closer_than_its_advances();
    test_wrapping_measures_the_line_rather_than_counting_letters();
    test_eliding_keeps_the_tail_and_fits();
    test_text_that_fits_is_left_alone();
    test_utf8_is_read_as_characters_rather_than_bytes();
    return test::finish("text");
}
