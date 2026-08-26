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

// The UI font, subset to the characters the shell draws so it costs kilobytes
// rather than the megabytes a full Japanese family would.
extern const unsigned char kRodinOtf[];
extern const unsigned long kRodinOtf_size;

// The controller buttons a control prompt names, as one horizontal strip of
// equal cells in ButtonGlyph order.
extern const unsigned char kButtonIconsPng[];
extern const unsigned long kButtonIconsPng_size;

// The shell's own sounds, resampled to one rate and packed into one blob with
// an index -- see decode_ui_sound_pack().

}  // namespace ds::assets
