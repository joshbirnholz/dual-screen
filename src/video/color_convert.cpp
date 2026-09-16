#include "video/color_convert.hpp"

#include <algorithm>
#include <thread>
#include <vector>

namespace ds::video {
namespace {

// Full-range BT.601, the same coefficients sws_getCoefficients(SWS_CS_ITU601)
// hands swscale, in 16-bit fixed point:
//
//   Y  =  0.299 R + 0.587 G + 0.114 B
//   Cb = -0.168736 R - 0.331264 G + 0.5 B + 128
//   Cr =  0.5 R - 0.418688 G - 0.081312 B + 128
//
// No clamping on luma: those three weights sum to exactly one, so an 8-bit
// input cannot produce an out-of-range result. Chroma can, and is clamped.
constexpr int kYR = 19595, kYG = 38470, kYB = 7471;
constexpr int kUR = -11059, kUG = -21709, kUB = 32768;
constexpr int kVR = 32768, kVG = -27439, kVB = -5329;
constexpr int kHalf = 32768;  // rounding, at 16 fractional bits

// One horizontal band, two rows at a time -- a chroma sample spans two rows
// and two columns, so both are handled in one pass rather than reading the
// picture twice.
void convert_band(const uint8_t* source,
                  std::size_t source_stride,
                  int width,
                  int first_row,
                  int last_row,
                  bool red_first,
                  uint8_t* luma,
                  std::size_t luma_stride,
                  uint8_t* chroma,
                  std::size_t chroma_stride) {
    const int red = red_first ? 0 : 2;
    const int blue = red_first ? 2 : 0;
    for (int y = first_row; y < last_row; y += 2) {
        const uint8_t* upper = source + static_cast<std::size_t>(y) * source_stride;
        const uint8_t* lower = source + static_cast<std::size_t>(y + 1) * source_stride;
        uint8_t* luma_upper = luma + static_cast<std::size_t>(y) * luma_stride;
        uint8_t* luma_lower = luma + static_cast<std::size_t>(y + 1) * luma_stride;
        uint8_t* chroma_row = chroma + static_cast<std::size_t>(y / 2) * chroma_stride;
        for (int x = 0; x < width; x += 2) {
            int sum_r = 0, sum_g = 0, sum_b = 0;
            for (int half = 0; half < 2; ++half) {
                const uint8_t* row = half == 0 ? upper : lower;
                uint8_t* out = half == 0 ? luma_upper : luma_lower;
                for (int side = 0; side < 2; ++side) {
                    const uint8_t* pixel = row + static_cast<std::size_t>(x + side) * 4;
                    const int r = pixel[red];
                    const int g = pixel[1];
                    const int b = pixel[blue];
                    out[x + side] =
                        static_cast<uint8_t>((kYR * r + kYG * g + kYB * b + kHalf) >> 16);
                    sum_r += r;
                    sum_g += g;
                    sum_b += b;
                }
            }
            // The 2x2 block's average colour, converted once. swscale converts
            // each pixel and averages afterwards, which agrees with this to
            // within a step.
            const int r = (sum_r + 2) >> 2;
            const int g = (sum_g + 2) >> 2;
            const int b = (sum_b + 2) >> 2;
            const int u = ((kUR * r + kUG * g + kUB * b + kHalf) >> 16) + 128;
            const int v = ((kVR * r + kVG * g + kVB * b + kHalf) >> 16) + 128;
            chroma_row[x + 0] = static_cast<uint8_t>(std::clamp(u, 0, 255));
            chroma_row[x + 1] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
}

}  // namespace

void rgb_to_nv12(const uint8_t* source,
                 std::size_t source_stride_bytes,
                 int width,
                 int height,
                 PixelOrder order,
                 uint8_t* luma,
                 std::size_t luma_stride,
                 uint8_t* chroma,
                 std::size_t chroma_stride,
                 unsigned threads) {
    if (source == nullptr || luma == nullptr || chroma == nullptr || width <= 0 || height <= 0) {
        return;
    }
    // Odd sizes have no whole 2x2 block at the far edge. Nothing this streams
    // is odd -- every screen it carries is even in both directions -- and the
    // caller checks, but the last row or column is left alone rather than read
    // past the end of the picture if one ever is.
    const int rows = height & ~1;
    const int columns = width & ~1;
    const bool red_first = order == PixelOrder::Rgba;

    const unsigned wanted = std::max(1u, threads);
    if (wanted == 1 || rows <= 2) {
        convert_band(source, source_stride_bytes, columns, 0, rows, red_first, luma, luma_stride,
                     chroma, chroma_stride);
        return;
    }

    // Bands of whole chroma rows, so no two threads write the same output byte.
    const int pairs = rows / 2;
    const int pairs_each = (pairs + static_cast<int>(wanted) - 1) / static_cast<int>(wanted);
    const int rows_each = pairs_each * 2;

    std::vector<std::thread> helpers;
    helpers.reserve(wanted - 1);
    for (unsigned index = 1; index < wanted; ++index) {
        const int first = static_cast<int>(index) * rows_each;
        if (first >= rows) {
            break;
        }
        const int last = std::min(rows, first + rows_each);
        helpers.emplace_back([=] {
            convert_band(source, source_stride_bytes, columns, first, last, red_first, luma,
                         luma_stride, chroma, chroma_stride);
        });
    }
    // The first band on this thread, so one core's worth is never idle waiting
    // for the others.
    convert_band(source, source_stride_bytes, columns, 0, std::min(rows, rows_each), red_first,
                 luma, luma_stride, chroma, chroma_stride);
    for (std::thread& helper : helpers) {
        helper.join();
    }
}

unsigned conversion_threads() {
    const unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) {
        return 1;
    }
    // Four is where the measured gain flattens anyway (1.80ms against 3.40ms
    // at two on a 1080p frame), and leaving the rest alone matters more than
    // the next fraction of a millisecond: the emulator on the other cores is
    // drawing the frame after this one.
    return std::min(cores, 4u);
}

}  // namespace ds::video
