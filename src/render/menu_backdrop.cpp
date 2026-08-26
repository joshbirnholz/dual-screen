#include "render/menu_backdrop.hpp"

#include <algorithm>
#include <cmath>

#include "render/gl_loader.hpp"

namespace ds {
namespace {

// The capture is shrunk before it is blurred. A blur this wide throws away
// everything the extra resolution held anyway, and the cost of every pass falls
// with the square of the shrink.
constexpr int kShrink = 4;
// The blur that was designed against, as a radius in pixels of a full-size
// frame. Scaled down with the capture, so a 4K display and a Switch panel end
// up equally soft rather than the big one looking sharper.
constexpr float kBlurRadius = 30.0f;
// Photoshop's Hue/Saturation, where the mockup's look was built: saturation
// +23 and lightness -44 on its -100..100 scales.
constexpr float kSaturation = 1.23f;
constexpr float kLightness = 0.56f;

uint32_t pack(float red, float green, float blue) {
    const auto channel = [](float value) {
        return static_cast<uint32_t>(std::min(255.0f, std::max(0.0f, value)) + 0.5f);
    };
    return 0xFF000000u | (channel(red) << 16) | (channel(green) << 8) | channel(blue);
}

// Three box blurs approximate a gaussian closely enough that nothing behind a
// menu could tell them apart, and each one is a running sum rather than a
// kernel: the cost does not grow with how wide the blur is.
int box_width_for(float sigma) {
    const int width = static_cast<int>(std::sqrt(4.0f * sigma * sigma + 1.0f));
    return std::max(1, width | 1);  // odd, so the box is centred on its pixel
}

void box_blur_horizontal(const std::vector<uint32_t>& source,
                         std::vector<uint32_t>& target,
                         int width,
                         int height,
                         int radius) {
    const float span = static_cast<float>(radius * 2 + 1);
    for (int y = 0; y < height; ++y) {
        const uint32_t* row = source.data() + static_cast<std::size_t>(y) * width;
        uint32_t* out = target.data() + static_cast<std::size_t>(y) * width;
        int red = 0;
        int green = 0;
        int blue = 0;
        const auto add = [&](int x) {
            const uint32_t pixel = row[std::min(width - 1, std::max(0, x))];
            red += static_cast<int>((pixel >> 16) & 0xFFu);
            green += static_cast<int>((pixel >> 8) & 0xFFu);
            blue += static_cast<int>(pixel & 0xFFu);
        };
        const auto remove = [&](int x) {
            const uint32_t pixel = row[std::min(width - 1, std::max(0, x))];
            red -= static_cast<int>((pixel >> 16) & 0xFFu);
            green -= static_cast<int>((pixel >> 8) & 0xFFu);
            blue -= static_cast<int>(pixel & 0xFFu);
        };
        for (int x = -radius; x <= radius; ++x) {
            add(x);
        }
        for (int x = 0; x < width; ++x) {
            out[x] = pack(red / span, green / span, blue / span);
            remove(x - radius);
            add(x + radius + 1);
        }
    }
}

void transpose(const std::vector<uint32_t>& source,
               std::vector<uint32_t>& target,
               int width,
               int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            target[static_cast<std::size_t>(x) * height + y] =
                source[static_cast<std::size_t>(y) * width + x];
        }
    }
}

}  // namespace

void blur_and_darken(std::vector<uint32_t>& pixels,
                     std::vector<uint32_t>& scratch,
                     int width,
                     int height,
                     float blur_radius) {
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
        return;
    }
    scratch.resize(pixels.size());

    // Blurred along one axis, transposed, blurred along that axis, transposed
    // back: one direction of blur code, and every pass walks memory in order.
    const int radius = box_width_for(blur_radius) / 2;
    if (radius > 0) {
        for (int axis = 0; axis < 2; ++axis) {
            const int span = axis == 0 ? width : height;
            const int lines = axis == 0 ? height : width;
            for (int pass = 0; pass < 3; ++pass) {
                box_blur_horizontal(pixels, scratch, span, lines, radius);
                pixels.swap(scratch);
            }
            transpose(pixels, scratch, span, lines);
            pixels.swap(scratch);
        }
    }

    for (std::size_t index = 0;
         index < static_cast<std::size_t>(width) * static_cast<std::size_t>(height); ++index) {
        const uint32_t pixel = pixels[index];
        const float red = static_cast<float>((pixel >> 16) & 0xFFu);
        const float green = static_cast<float>((pixel >> 8) & 0xFFu);
        const float blue = static_cast<float>(pixel & 0xFFu);
        const float luma = 0.2126f * red + 0.7152f * green + 0.0722f * blue;
        pixels[index] = pack((luma + (red - luma) * kSaturation) * kLightness,
                             (luma + (green - luma) * kSaturation) * kLightness,
                             (luma + (blue - luma) * kSaturation) * kLightness);
    }
}

void MenuBackdrop::capture(int output_width, int output_height) {
    forget();
    if (gl::ReadPixels == nullptr || output_width <= 0 || output_height <= 0) {
        return;
    }

    std::vector<uint32_t> frame(static_cast<std::size_t>(output_width) * output_height);
    gl::ReadPixels(0, 0, output_width, output_height, GL_RGBA, GL_UNSIGNED_BYTE, frame.data());

    width_ = std::max(1, output_width / kShrink);
    height_ = std::max(1, output_height / kShrink);
    pixels_.assign(static_cast<std::size_t>(width_) * height_, 0);

    // Read back bottom-up, as OpenGL hands it over, and averaged over the block
    // each small pixel stands for rather than point-sampled: a blur cannot put
    // back detail that was dropped as aliasing on the way in.
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            uint32_t red = 0;
            uint32_t green = 0;
            uint32_t blue = 0;
            uint32_t taken = 0;
            for (int step_y = 0; step_y < kShrink; ++step_y) {
                const int source_y = output_height - 1 - (y * kShrink + step_y);
                if (source_y < 0) continue;
                for (int step_x = 0; step_x < kShrink; ++step_x) {
                    const int source_x = x * kShrink + step_x;
                    if (source_x >= output_width) continue;
                    const uint32_t rgba =
                        frame[static_cast<std::size_t>(source_y) * output_width + source_x];
                    red += rgba & 0xFFu;
                    green += (rgba >> 8) & 0xFFu;
                    blue += (rgba >> 16) & 0xFFu;
                    ++taken;
                }
            }
            if (taken == 0) taken = 1;
            pixels_[static_cast<std::size_t>(y) * width_ + x] =
                pack(static_cast<float>(red / taken), static_cast<float>(green / taken),
                     static_cast<float>(blue / taken));
        }
    }

    blur_and_darken(pixels_, scratch_, width_, height_, kBlurRadius / kShrink);
    texture_.upload_xrgb8888(pixels_.data(), width_, height_, width_);
    texture_.set_linear_filtering(true);
}

void MenuBackdrop::forget() {
    texture_.destroy();
    pixels_.clear();
    scratch_.clear();
    width_ = 0;
    height_ = 0;
}

void MenuBackdrop::draw(QuadRenderer& quads, float output_width, float output_height) const {
    if (!texture_.valid()) {
        return;
    }
    quads.draw_screen(PixelRect{0.0f, 0.0f, output_width, output_height}, texture_, Color{},
                      ScreenRotation::None, true);
}

}  // namespace ds
