#pragma once

namespace ds::assets {

// The consoles the Standard layout draws its screens into, as PNGs. They are
// generated into source files at build time from assets/ rather than loaded
// from disk: a copied-out binary that needs a file beside it to look right is a
// binary that will turn up somewhere without it.
extern const unsigned char kDsBezelPng[];
extern const unsigned long kDsBezelPng_size;
extern const unsigned char kThreeDsBezelPng[];
extern const unsigned long kThreeDsBezelPng_size;

// The UI font: Inter Medium, under the SIL Open Font License (see
// assets/inter-OFL.txt), subset to Latin so it costs kilobytes.
extern const unsigned char kUiFont[];
extern const unsigned long kUiFont_size;

// The controller buttons a control prompt names, as one horizontal strip of
// equal cells in ButtonGlyph order.
extern const unsigned char kButtonIconsPng[];
extern const unsigned long kButtonIconsPng_size;

// The Wii U's symbol for which hand holds the stylus, copied out of a capture
// of its Screen Settings and made square -- see assets/make_stylus_hand.py.
extern const unsigned char kStylusHandPng[];
extern const unsigned long kStylusHandPng_size;

// Data Management's icons -- get, send, earlier saves, delete, and the kinds of
// device -- as one strip of equal square cells. See assets/make_ui_icons.py.
// Only in the builds that play games.
extern const unsigned char kUiIconsPng[];
extern const unsigned long kUiIconsPng_size;

// The shell's own sounds, resampled to one rate and packed into one blob with
// an index -- see decode_ui_sound_pack().

}  // namespace ds::assets
