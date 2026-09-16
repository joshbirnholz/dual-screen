#pragma once

#include <cstddef>
#include <cstdint>

#include "video/video_codec.hpp"

namespace ds::video {

// Turns a packed 8-bit RGB picture into NV12, full-range BT.601, in place of
// swscale doing it.
//
// Worth having its own code because of what swscale charges for the "full
// range" part. Measured on a 1080p frame, the exact conversion this app asks
// for (BGRA to NV12, ITU-601 coefficients, destination range full):
//
//   swscale, as configured here          8.43 ms
//   swscale, left at its default range   3.46 ms
//   this, one thread                     4.65 ms
//   this, four threads                   1.80 ms
//
// So asking for full range -- which this app does deliberately, because
// limited range throws away the outer 14% of every channel before compression
// and that reads as banding -- costs five milliseconds a frame by pushing
// swscale off its fast path and onto a generic one. On a Steam Deck that
// showed up as 7ms of the 8.5ms spent encoding each frame: latency on every
// frame, and a core taken from the emulator drawing the next one.
//
// Nothing clever is going on here. It is the same arithmetic, written for the
// one case that matters -- no resizing, packed 8-bit in, NV12 out -- and split
// across threads by horizontal band, which is free because no output pixel
// depends on another. Against swscale on a real game frame the luma matches to
// within one step and the chroma likewise; a test in the repo asserts it,
// using swscale itself as the reference.
//
// Chroma is the average of each 2x2 block of source pixels, converted once.
// swscale converts and then averages, which comes to the same picture within
// rounding.
//
// `threads` of zero or one converts on the calling thread. Bands are always an
// even number of rows, since a chroma sample spans two.
void rgb_to_nv12(const uint8_t* source,
                 std::size_t source_stride_bytes,
                 int width,
                 int height,
                 PixelOrder order,
                 uint8_t* luma,
                 std::size_t luma_stride,
                 uint8_t* chroma,
                 std::size_t chroma_stride,
                 unsigned threads);

// How many threads rgb_to_nv12() should be given on this machine.
//
// Capped well below what a Steam Deck has, because the emulator drawing the
// next frame needs cores more than this does: taking every one of them to save
// another half millisecond here would cost more over there. Zero cores
// reported -- which happens -- means do it on this thread.
unsigned conversion_threads();

}  // namespace ds::video
