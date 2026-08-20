#pragma once

namespace ds::assets {

// The Nintendo DS the Standard layout draws its screens into, as a PNG. It is
// generated into a source file at build time from assets/ds_bezel.png rather
// than loaded from disk: a copied-out binary that needs a file beside it to
// look right is a binary that will turn up somewhere without it.
extern const unsigned char kDsBezelPng[];
extern const unsigned long kDsBezelPng_size;

}  // namespace ds::assets
