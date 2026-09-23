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

#include "config/settings.hpp"
#include "render/button_glyphs.hpp"
#include "ui/control_guide.hpp"

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

// Positional names each button by where it is, and nothing else: the dots
// for its own corner, and the Nintendo symbols for the buttons that have no
// corner.
void test_positional_prompts_are_the_four_corners() {
    CHECK(glyph_cell(PadGlyphStyle::Positional, ButtonGlyph::A) == GlyphCell::DotsEast);
    CHECK(glyph_cell(PadGlyphStyle::Positional, ButtonGlyph::B) == GlyphCell::DotsSouth);
    CHECK(glyph_cell(PadGlyphStyle::Positional, ButtonGlyph::X) == GlyphCell::DotsNorth);
    CHECK(glyph_cell(PadGlyphStyle::Positional, ButtonGlyph::Y) == GlyphCell::DotsWest);
    CHECK(glyph_cell(PadGlyphStyle::Positional, ButtonGlyph::Plus) == GlyphCell::Plus);
    CHECK(glyph_cell(PadGlyphStyle::Positional, ButtonGlyph::Minus) == GlyphCell::Minus);
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

// Choosing which button means yes changes the picture and nothing else.
//
// The pair is what moves: a prompt that means "confirm" is drawn as the east
// button by default and as the south one when that is what was chosen. What
// must not move is what the prompt *means*, because a finger lands on the
// prompt and the prompt is still the confirm prompt.
void test_choosing_south_swaps_the_picture_and_not_the_meaning() {
    const ConfirmButton was = confirm_button();

    set_confirm_button(ConfirmButton::East);
    CHECK(glyph_as_drawn(ButtonGlyph::A) == ButtonGlyph::A);
    CHECK(glyph_as_drawn(ButtonGlyph::B) == ButtonGlyph::B);

    set_confirm_button(ConfirmButton::South);
    CHECK(glyph_as_drawn(ButtonGlyph::A) == ButtonGlyph::B);
    CHECK(glyph_as_drawn(ButtonGlyph::B) == ButtonGlyph::A);

    // The meaning is untouched: the confirm prompt still confirms.
    UiAction action = UiAction::Cancel;
    CHECK(ui_action_for_glyph(ButtonGlyph::A, action) && action == UiAction::Confirm);
    CHECK(ui_action_for_glyph(ButtonGlyph::B, action) && action == UiAction::Cancel);

    // And nothing else in the row moves with it.
    for (const ButtonGlyph other : {ButtonGlyph::X, ButtonGlyph::Y, ButtonGlyph::Plus,
                                    ButtonGlyph::Minus, ButtonGlyph::LeftStick,
                                    ButtonGlyph::RightStick}) {
        CHECK(glyph_as_drawn(other) == other);
    }
    set_confirm_button(was);
}

// The two buttons are always the two lower face buttons and always different,
// whichever way round they are put.
void test_the_pair_is_always_two_different_buttons() {
    for (int index = 0; index < static_cast<int>(ConfirmButton::Count); ++index) {
        const ConfirmButton choice = static_cast<ConfirmButton>(index);
        CHECK(confirm_button_for(choice) != cancel_button_for(choice));
        CHECK(confirm_glyph_for(choice) != cancel_glyph_for(choice));
        // Swapped, not replaced: the two halves are each other's opposite.
        CHECK(confirm_button_for(choice) == cancel_button_for(
                  choice == ConfirmButton::East ? ConfirmButton::South : ConfirmButton::East));
    }
    // East is the default and is what this app has always done.
    CHECK(confirm_button_for(ConfirmButton::East) == SDL_CONTROLLER_BUTTON_B);
    CHECK(cancel_button_for(ConfirmButton::East) == SDL_CONTROLLER_BUTTON_A);
    CHECK(Settings().confirm_button == ConfirmButton::East);
    CHECK(Settings().pad_glyphs == PadGlyphStyle::Nintendo);
}

void test_the_choice_survives_settings_json() {
    for (int index = 0; index < static_cast<int>(ConfirmButton::Count); ++index) {
        const ConfirmButton choice = static_cast<ConfirmButton>(index);
        ConfirmButton back = ConfirmButton::East;
        CHECK(confirm_button_from_string(to_string(choice), back));
        CHECK(back == choice);
    }
    ConfirmButton kept = ConfirmButton::South;
    CHECK(!confirm_button_from_string("Northwest", kept));
    CHECK(kept == ConfirmButton::South);
}

}  // namespace

int main() {
    test_positional_prompts_are_the_four_corners();
    test_nintendo_is_the_artwork_this_started_with();
    test_xbox_is_the_same_letters_in_the_other_corners();
    test_playstation_is_shapes();
    test_no_style_draws_two_face_buttons_the_same();
    test_the_stick_clicks_never_change();
    test_every_cell_is_in_the_sheet();
    test_a_style_survives_the_round_trip_through_settings_json();
    test_choosing_south_swaps_the_picture_and_not_the_meaning();
    test_the_pair_is_always_two_different_buttons();
    test_the_choice_survives_settings_json();
    return test::finish("pad-glyphs");
}
