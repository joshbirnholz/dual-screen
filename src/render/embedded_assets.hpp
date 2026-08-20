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

}  // namespace ds::assets
