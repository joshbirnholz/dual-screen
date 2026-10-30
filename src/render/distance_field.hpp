#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "render/texture.hpp"

namespace ds {

// How far either side of an edge a distance field reaches, in its texels.
// Past it everything reads as wholly in or wholly out -- so this is also as
// small as a picture can be drawn before its edges stop being soft: an edge
// spread over one screen pixel needs half a pixel each side, which at a
// tenth of the texels' size is five of them.
constexpr float kDistanceFieldSpread = 8.0f;

namespace distance_field_detail {

// Felzenszwalb and Huttenlocher's squared distance transform along one row:
// each f[q] replaced by the least (q - p)^2 + f[p] over every p, or by `far`
// where that is more. Only the texels nearer than `far` are looked at, which
// away from the shapes is none. Where two parabolas cross is worked out from
// their difference, which stays small, so floats are exact enough however
// long the row is.
inline void squared_distance_1d(const float* f, float* d, int n, float far, std::vector<int>& v,
                                std::vector<float>& z) {
    constexpr float kEnd = 1e30f;
    int k = -1;
    for (int q = 0; q < n; ++q) {
        if (f[q] >= far) continue;
        if (k < 0) {
            k = 0;
            v[0] = q;
            z[0] = -kEnd;
            z[1] = kEnd;
            continue;
        }
        float s = 0.0f;
        while (true) {
            const int p = v[k];
            s = (f[q] - f[p]) / (2.0f * static_cast<float>(q - p)) + static_cast<float>(q + p) * 0.5f;
            if (s > z[k] || k == 0) break;
            --k;
        }
        if (s <= z[k]) {
            // Only with k == 0: this parabola is lower than the first everywhere.
            v[0] = q;
            z[1] = kEnd;
            continue;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kEnd;
    }
    if (k < 0) {
        std::fill(d, d + n, far);
        return;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<float>(q)) ++k;
        const float dq = static_cast<float>(q - v[k]);
        d[q] = std::min(far, dq * dq + f[v[k]]);
    }
}

// Each texel's squared distance to the nearest texel where `seed` is set, as
// far as `reach`: anything further reads as about reach squared.
inline std::vector<float> squared_distances(const std::vector<uint8_t>& seed, int width, int height, int reach) {
    // Down the columns first, each texel's distance to the nearest seed above
    // or below it: two sweeps, a row at a time.
    std::vector<int> up(seed.size());
    const int far = reach + 1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            up[i] = seed[i] ? 0 : (y == 0 ? far : std::min(far, up[i - width] + 1));
        }
    }
    std::vector<float> grid(seed.size());
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            if (y < height - 1) up[i] = std::min(up[i], up[i + width] + 1);
            const float g = static_cast<float>(up[i]);
            grid[i] = g * g;
        }
    }
    // Then along the rows.
    std::vector<float> in(static_cast<std::size_t>(width));
    std::vector<int> v(static_cast<std::size_t>(width));
    std::vector<float> z(static_cast<std::size_t>(width) + 1);
    const float far_squared = static_cast<float>(far) * static_cast<float>(far);
    for (int y = 0; y < height; ++y) {
        float* row = grid.data() + static_cast<std::size_t>(y) * width;
        std::copy(row, row + width, in.begin());
        squared_distance_1d(in.data(), row, width, far_squared, v, z);
    }
    return grid;
}

}  // namespace distance_field_detail

// Turns a coverage mask -- 255 inside a shape, 0 outside, soft at the edge --
// into a signed distance field the same size: 128 on the edge, rising
// inside and falling outside by 128 / kDistanceFieldSpread a texel. Drawn
// with QuadRenderer::draw_distance_field(), the edge comes back sharp
// however large the picture is drawn, and evenly soft however small, which
// a coverage mask filtered up or down is neither.
//
// The edge is found on the mask enlarged `detail` times, each texel's
// coverage spread over the enlarged texels by bilinear interpolation, so it
// lands between texels rather than on them.
inline std::vector<uint8_t> coverage_to_distance_field(const std::vector<uint8_t>& coverage, int width,
                                                       int height, int detail = 4) {
    std::vector<uint8_t> field(coverage.size(), 0);
    if (width <= 0 || height <= 0 || coverage.size() < static_cast<std::size_t>(width) * height) {
        return field;
    }
    detail = std::max(1, detail);
    const int big_w = width * detail;
    const int big_h = height * detail;
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, width - 1);
        y = std::clamp(y, 0, height - 1);
        return static_cast<float>(coverage[static_cast<std::size_t>(y) * width + x]);
    };
    std::vector<uint8_t> inside(static_cast<std::size_t>(big_w) * big_h);
    std::vector<uint8_t> outside(inside.size());
    // A block of `detail` by `detail` enlarged texels at a time, all between
    // the same four of the mask's: where those four agree, so does all of it.
    for (int y0 = -1; y0 < height; ++y0) {
        for (int x0 = -1; x0 < width; ++x0) {
            const float tl = at(x0, y0);
            const float tr = at(x0 + 1, y0);
            const float bl = at(x0, y0 + 1);
            const float br = at(x0 + 1, y0 + 1);
            const bool all_in = std::min(std::min(tl, tr), std::min(bl, br)) >= 127.5f;
            const bool all_out = std::max(std::max(tl, tr), std::max(bl, br)) < 127.5f;
            // The enlarged texels whose centres fall between these four.
            const int first_x = std::max(0, x0 * detail + (detail + 1) / 2);
            const int last_x = std::min(big_w, (x0 + 1) * detail + (detail + 1) / 2);
            const int first_y = std::max(0, y0 * detail + (detail + 1) / 2);
            const int last_y = std::min(big_h, (y0 + 1) * detail + (detail + 1) / 2);
            for (int y = first_y; y < last_y; ++y) {
                const float fy = (static_cast<float>(y) + 0.5f) / detail - 0.5f - static_cast<float>(y0);
                for (int x = first_x; x < last_x; ++x) {
                    bool in = all_in;
                    if (!all_in && !all_out) {
                        const float fx = (static_cast<float>(x) + 0.5f) / detail - 0.5f - static_cast<float>(x0);
                        const float top = tl + (tr - tl) * fx;
                        const float bottom = bl + (br - bl) * fx;
                        in = top + (bottom - top) * fy >= 127.5f;
                    }
                    const std::size_t index = static_cast<std::size_t>(y) * big_w + x;
                    inside[index] = in ? 1 : 0;
                    outside[index] = in ? 0 : 1;
                }
            }
        }
    }
    // Past the spread every texel reads the same, so nothing further than
    // that need be measured.
    const int reach = static_cast<int>(kDistanceFieldSpread) * detail + detail;
    const std::vector<float> to_inside = distance_field_detail::squared_distances(inside, big_w, big_h, reach);
    const std::vector<float> to_outside = distance_field_detail::squared_distances(outside, big_w, big_h, reach);
    // Each texel's distance is read at its centre, which on the enlarged
    // grid falls between texels when `detail` is even: the mean of the four
    // around it.
    const float scale = 128.0f / (kDistanceFieldSpread * static_cast<float>(detail));
    const auto signed_at = [&](int x, int y) {
        const std::size_t index = static_cast<std::size_t>(y) * big_w + x;
        // Half an enlarged texel either way puts the edge between the last
        // texel in and the first one out.
        if (inside[index]) return std::sqrt(to_outside[index]) - 0.5f;
        return 0.5f - std::sqrt(to_inside[index]);
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int bx = x * detail + detail / 2;
            const int by = y * detail + detail / 2;
            float distance = signed_at(bx, by);
            if (detail % 2 == 0) {
                distance = (distance + signed_at(bx - 1, by) + signed_at(bx, by - 1) +
                            signed_at(bx - 1, by - 1)) *
                           0.25f;
            }
            const float value = 128.0f + distance * scale;
            field[static_cast<std::size_t>(y) * width + x] =
                static_cast<uint8_t>(std::clamp(value, 0.0f, 255.0f) + 0.5f);
        }
    }
    return field;
}

// A coverage mask uploaded as its distance field, filtered, ready for
// QuadRenderer::draw_distance_field().
inline void upload_distance_field(Texture& texture, const std::vector<uint8_t>& coverage, int width,
                                  int height) {
    const std::vector<uint8_t> field = coverage_to_distance_field(coverage, width, height);
    texture.upload_alpha8(field.data(), width, height);
    texture.set_linear_filtering(true);
}

}  // namespace ds
