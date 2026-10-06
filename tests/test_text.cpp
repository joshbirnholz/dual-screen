#include <cstdio>
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

// A Japanese game's name -- Ouendan 2, here -- drawn from a fallback font: the
// embedded one is Latin only. Wherever this runs has to have one for there to
// be anything to check, as a desktop with a CJK font installed (or a Switch,
// with its own) does.
void test_names_the_ui_font_lacks_come_from_a_fallback() {
    const FontFace& face = font_face(26.0f);
    const Glyph* a = face.glyph(0x3042);  // あ
    if (a == nullptr) {
        std::printf("note: no Japanese font on this machine; the fallback was not checked\n");
        return;
    }
    CHECK_MSG(a->font != 0, "あ came from the embedded font, which has no kana");
    CHECK_MSG(a->width > 0 && a->height > 0, "あ was drawn as nothing");
    const std::string name = "\xE6\x8A\xBC\xE5\xBF\x8D\xEF\xBC\x81\xE9\x97\x98\xE3\x81\x88\xEF\xBC\x81"
                             "\xE5\xBF\x9C\xE6\x8F\xB4\xE5\x9B\xA3 2";  // 押忍！闘え！応援団 2
    const float width = TextRenderer::measure_width(name, 3.0f);
    CHECK_MSG(width > TextRenderer::measure_width(" 2", 3.0f) * 4.0f,
              "the name measured " + std::to_string(width) + "px, as if its kanji were not there");
    // As wide as the Latin letters beside it would suggest: about an em each.
    const Glyph* kanji = face.glyph(0x95D8);  // 闘
    const Glyph* capital = face.glyph('H');
    CHECK(kanji != nullptr && capital != nullptr);
    if (kanji != nullptr && capital != nullptr) {
        CHECK_MSG(kanji->height > capital->height && kanji->height < capital->height * 2,
                  "闘 is " + std::to_string(kanji->height) + "px tall beside an H of " +
                      std::to_string(capital->height) + "px");
    }

    // Thousands of them grow the atlas, and what was in it stays where it was.
    const Glyph* h = face.glyph('H');
    std::vector<uint8_t> before;
    for (int y = 0; y < h->height; ++y) {
        const uint8_t* row = face.pixels().data() + static_cast<std::size_t>(h->atlas_y + y) * face.atlas_width();
        before.insert(before.end(), row + h->atlas_x, row + h->atlas_x + h->width);
    }
    const int width_before = face.atlas_width();
    const int revision_before = face.revision();
    for (uint32_t codepoint = 0x4E00; codepoint < 0x4E00 + 1500; ++codepoint) face.glyph(codepoint);
    CHECK_MSG(face.atlas_width() > width_before, "1500 kanji fitted in the atlas they started in");
    CHECK(face.revision() > revision_before);
    std::vector<uint8_t> after;
    for (int y = 0; y < h->height; ++y) {
        const uint8_t* row = face.pixels().data() + static_cast<std::size_t>(h->atlas_y + y) * face.atlas_width();
        after.insert(after.end(), row + h->atlas_x, row + h->atlas_x + h->width);
    }
    CHECK_MSG(before == after, "growing the atlas moved or spoiled the glyphs already in it");
    const Glyph* last = face.glyph(0x4E00 + 1499);
    if (last != nullptr) {
        CHECK(last->atlas_x + last->width <= face.atlas_width());
        CHECK(last->atlas_y + last->height <= face.atlas_height());
    }
    // A codepoint no font has is still nothing.
    CHECK(face.glyph(0x10FFFD) == nullptr);
}

int main() {
    test_names_the_ui_font_lacks_come_from_a_fallback();
    test_letters_take_the_width_of_their_own_shape();
    test_a_wider_scale_draws_wider_text();
    test_a_pair_that_kerns_sits_closer_than_its_advances();
    test_wrapping_measures_the_line_rather_than_counting_letters();
    test_eliding_keeps_the_tail_and_fits();
    test_text_that_fits_is_left_alone();
    test_utf8_is_read_as_characters_rather_than_bytes();
    return test::finish("text");
}
