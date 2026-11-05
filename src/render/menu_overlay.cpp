#include "render/menu_overlay.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "render/gl_loader.hpp"

#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif

namespace ds {

MenuOverlay::~MenuOverlay() { release(); }

bool MenuOverlay::supported() {
    return gl::GenFramebuffers != nullptr && gl::BindFramebuffer != nullptr &&
           gl::FramebufferTexture2D != nullptr && gl::CheckFramebufferStatus != nullptr &&
           gl::BlendFuncSeparate != nullptr && gl::ReadPixels != nullptr;
}

void MenuOverlay::fit(int width, int height, int max_width, int max_height, int& out_width,
                      int& out_height) {
    out_width = std::max(1, width);
    out_height = std::max(1, height);
    if (out_width <= max_width && out_height <= max_height) return;
    const double scale = std::min(static_cast<double>(max_width) / out_width,
                                  static_cast<double>(max_height) / out_height);
    out_width = std::clamp(static_cast<int>(std::floor(out_width * scale)), 1, max_width);
    out_height = std::clamp(static_cast<int>(std::floor(out_height * scale)), 1, max_height);
}

void MenuOverlay::release() {
    if (framebuffer_ != 0 && gl::DeleteFramebuffers != nullptr) {
        const GLuint framebuffer = framebuffer_;
        gl::DeleteFramebuffers(1, &framebuffer);
    }
    framebuffer_ = 0;
    texture_ = Texture();
    width_ = 0;
    height_ = 0;
}

bool MenuOverlay::begin(int width, int height) {
    if (!supported() || width <= 0 || height <= 0) return false;
    if (framebuffer_ == 0 || width != width_ || height != height_) {
        release();
        texture_.allocate_rgba(width, height);
        GLuint framebuffer = 0;
        gl::GenFramebuffers(1, &framebuffer);
        framebuffer_ = framebuffer;
        gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                 texture_.id(), 0);
        if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            release();
            gl::bind_window_framebuffer();
            return false;
        }
        width_ = width;
        height_ = height;
        last_.clear();
    }
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    return true;
}

bool MenuOverlay::finish() {
    if (framebuffer_ == 0) return false;
    read_.resize(static_cast<std::size_t>(width_) * height_ * 4);
    if (gl::PixelStorei != nullptr) gl::PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl::ReadPixels(0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE, read_.data());
    gl::bind_window_framebuffer();
    // Compared as read, before the work of straightening it: most frames of
    // a menu are the frame before, and at a 1440p or 4K display's size that
    // work is most of what drawing the menu over the game costs.
    if (!last_.empty() && read_ == last_) return false;
    last_ = read_;
    straighten(read_, width_, height_, pixels_);
    return true;
}

void MenuOverlay::straighten(const std::vector<uint8_t>& read, int width, int rows,
                             std::vector<uint8_t>& out) {
    // value * 255 / alpha, rounded and capped, for every value and alpha: a
    // look-up rather than three divisions a pixel.
    static const std::vector<uint8_t> unpremultiplied = [] {
        std::vector<uint8_t> table(256u * 256u, 0);
        for (unsigned alpha = 1; alpha < 256; ++alpha) {
            for (unsigned value = 0; value < 256; ++value) {
                table[alpha * 256u + value] =
                    static_cast<uint8_t>(std::min(255u, (value * 255u + alpha / 2) / alpha));
            }
        }
        return table;
    }();
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 4;
    out.resize(row_bytes * static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        const uint8_t* from = read.data() + row_bytes * static_cast<std::size_t>(rows - 1 - row);
        uint8_t* to = out.data() + row_bytes * static_cast<std::size_t>(row);
        for (std::size_t at = 0; at < row_bytes; at += 4) {
            const unsigned alpha = from[at + 3];
            if (alpha == 255) {
                std::memcpy(to + at, from + at, 4);
                continue;
            }
            if (alpha == 0) {
                std::memset(to + at, 0, 4);
                continue;
            }
            const uint8_t* scale = unpremultiplied.data() + alpha * 256u;
            to[at] = scale[from[at]];
            to[at + 1] = scale[from[at + 1]];
            to[at + 2] = scale[from[at + 2]];
            to[at + 3] = static_cast<uint8_t>(alpha);
        }
    }
}

}  // namespace ds
