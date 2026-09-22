// Which symbol a prompt is drawn with, in each of the three styles.
//
// The positions are fixed and only the printing changes: ButtonGlyph::A is
// the east button whatever pad is in somebody's hands, and the whole point of
// the setting is that east is printed A on a Nintendo pad, B on an Xbox one
// and a circle on a PlayStation one. Getting that table wrong does not fail
// anything -- it draws the wrong letter, quietly, on a screen telling
// somebody which button to press.

#include "test_support.hpp"

#include <set>
#include <string>

#include "render/button_glyphs.hpp"

using namespace ds;

namespace {

void test_nintendo_is_the_artwork_this_started_with() {
    CHECK(glyph_cell(PadGlyphStyle::Nintendo, ButtonGlyph::A) == GlyphCell::LetterA);
    CHECK(glyph_cell(PadGlyphStyle::Nintendo, ButtonGlyph::B) == GlyphCell::LetterB);
    CHECK(glyph_cell(PadGlyphStyle::Nintendo, ButtonGlyph::X) == GlyphCell::LetterX);
    CHECK(glyph_cell(PadGlyphStyle::Nintendo, ButtonGlyph::Y) == GlyphCell::LetterY);
    CHECK(glyph_cell(PadGlyphStyle::Nintendo, ButtonGlyph::Plus) == GlyphCell::Plus);
    CHECK(glyph_cell(PadGlyphStyle::Nintendo, ButtonGlyph::Minus) == GlyphCell::Minus);
}

// An Xbox pad prints the same four letters as a Nintendo one, in different
// corners. That swap is the confusion this setting exists to end, so it is
// the thing most worth pinning down.
void test_xbox_is_the_same_letters_in_the_other_corners() {
    CHECK_MSG(glyph_cell(PadGlyphStyle::Xbox, ButtonGlyph::A) == GlyphCell::LetterB,
              "east is printed B on an Xbox pad");
    CHECK_MSG(glyph_cell(PadGlyphStyle::Xbox, ButtonGlyph::B) == GlyphCell::LetterA,
              "south is printed A on an Xbox pad");
    CHECK_MSG(glyph_cell(PadGlyphStyle::Xbox, ButtonGlyph::X) == GlyphCell::LetterY,
              "north is printed Y on an Xbox pad");
    CHECK_MSG(glyph_cell(PadGlyphStyle::Xbox, ButtonGlyph::Y) == GlyphCell::LetterX,
              "west is printed X on an Xbox pad");
}

void test_playstation_is_shapes() {
    CHECK(glyph_cell(PadGlyphStyle::PlayStation, ButtonGlyph::A) == GlyphCell::Circle);
    CHECK(glyph_cell(PadGlyphStyle::PlayStation, ButtonGlyph::B) == GlyphCell::Cross);
    CHECK(glyph_cell(PadGlyphStyle::PlayStation, ButtonGlyph::X) == GlyphCell::Triangle);
    CHECK(glyph_cell(PadGlyphStyle::PlayStation, ButtonGlyph::Y) == GlyphCell::Square);
}

// Four face buttons, four different symbols, in every style. A table that
// sent two positions to one cell would draw two prompts the same and make a
// menu unreadable rather than merely wrong.
void test_no_style_draws_two_face_buttons_the_same() {
    for (int index = 0; index < static_cast<int>(PadGlyphStyle::Count); ++index) {
        const PadGlyphStyle style = static_cast<PadGlyphStyle>(index);
        std::set<GlyphCell> seen;
        for (const ButtonGlyph face : {ButtonGlyph::A, ButtonGlyph::B, ButtonGlyph::X,
                                       ButtonGlyph::Y}) {
            seen.insert(glyph_cell(style, face));
        }
        CHECK_MSG(seen.size() == 4, to_string(style));
    }
}

// A stick click is a stick click: no pad prints anything else on it, so no
// style may quietly move it.
void test_the_stick_clicks_never_change() {
    for (int index = 0; index < static_cast<int>(PadGlyphStyle::Count); ++index) {
        const PadGlyphStyle style = static_cast<PadGlyphStyle>(index);
        CHECK(glyph_cell(style, ButtonGlyph::LeftStick) == GlyphCell::LeftStick);
        CHECK(glyph_cell(style, ButtonGlyph::RightStick) == GlyphCell::RightStick);
    }
}

// Every cell a style asks for has to be in the strip, or it samples past the
// end of the artwork and draws whatever is there.
void test_every_cell_is_in_the_sheet() {
    for (int s = 0; s < static_cast<int>(PadGlyphStyle::Count); ++s) {
        for (int g = 0; g < static_cast<int>(ButtonGlyph::Count); ++g) {
            const GlyphCell cell =
                glyph_cell(static_cast<PadGlyphStyle>(s), static_cast<ButtonGlyph>(g));
            CHECK(static_cast<int>(cell) >= 0);
            CHECK(static_cast<int>(cell) < static_cast<int>(GlyphCell::Count));
        }
    }
}

void test_a_style_survives_the_round_trip_through_settings_json() {
    for (int index = 0; index < static_cast<int>(PadGlyphStyle::Count); ++index) {
        const PadGlyphStyle style = static_cast<PadGlyphStyle>(index);
        PadGlyphStyle back = PadGlyphStyle::Nintendo;
        CHECK(pad_glyph_style_from_string(to_string(style), back));
        CHECK(back == style);
    }
    // And a name from a later version leaves the setting where it was rather
    // than resetting it.
    PadGlyphStyle kept = PadGlyphStyle::Xbox;
    CHECK(!pad_glyph_style_from_string("SteamDeck", kept));
    CHECK(kept == PadGlyphStyle::Xbox);
}

}  // namespace

int main() {
    test_nintendo_is_the_artwork_this_started_with();
    test_xbox_is_the_same_letters_in_the_other_corners();
    test_playstation_is_shapes();
    test_no_style_draws_two_face_buttons_the_same();
    test_the_stick_clicks_never_change();
    test_every_cell_is_in_the_sheet();
    test_a_style_survives_the_round_trip_through_settings_json();
    return test::finish("pad-glyphs");
}
