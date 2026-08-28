#include "render/texture.hpp"

#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#include <algorithm>
#include <utility>
#include <vector>

namespace ds {

Texture::~Texture() { destroy(); }

Texture::Texture(Texture&& other) noexcept
    : id_(other.id_),
      width_(other.width_),
      height_(other.height_),
      linear_(other.linear_),
      filter_dirty_(other.filter_dirty_) {
    other.id_ = 0;
    other.width_ = 0;
    other.height_ = 0;
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        destroy();
        id_ = other.id_;
        width_ = other.width_;
        height_ = other.height_;
        linear_ = other.linear_;
        filter_dirty_ = other.filter_dirty_;
        other.id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

void Texture::destroy() {
    if (id_ != 0 && gl::DeleteTextures != nullptr) {
        gl::DeleteTextures(1, &id_);
    }
    id_ = 0;
    width_ = 0;
    height_ = 0;
}

bool Texture::ensure_created() {
    // Nothing to create against: the loader has not resolved its entry points,
    // so there is no context yet. Every upload below checks valid() before
    // doing anything with the result, so a texture that was never made simply
    // never draws -- rather than calling through a null function pointer.
    if (gl::GenTextures == nullptr) {
        return false;
    }
    if (id_ == 0) {
        gl::GenTextures(1, &id_);
        filter_dirty_ = true;
    }
    return id_ != 0;
}

void Texture::set_linear_filtering(bool linear) {
    if (linear_ != linear) {
        linear_ = linear;
        filter_dirty_ = true;
    }
}

// Shared by upload_xrgb8888() and upload_rgba8888(): the two differ only in
// which ChannelOrder they leave the texture tagged with, since GL is handed
// the same four bytes a pixel either way and it is only the shader on the far
// side that needs to know whether they arrived pre-swapped.
void Texture::upload_four_bytes_a_pixel(const uint32_t* source, int width, int height,
                                        int source_stride_pixels) {
    gl::BindTexture(GL_TEXTURE_2D, id_);

    if (filter_dirty_) {
        const GLint filter = linear_ ? GL_LINEAR : GL_NEAREST;
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        filter_dirty_ = false;
    }

    // GL_UNPACK_ROW_LENGTH is desktop-GL only, so rows are copied into a packed
    // staging buffer when the source is strided. That keeps this path working
    // unchanged on an OpenGL ES driver.
    const bool packed = (source_stride_pixels == width);
    static thread_local std::vector<uint32_t> staging;
    const uint32_t* upload_from = source;
    if (!packed) {
        staging.resize(static_cast<std::size_t>(width) * height);
        for (int row = 0; row < height; ++row) {
            std::copy(source + static_cast<std::size_t>(row) * source_stride_pixels,
                      source + static_cast<std::size_t>(row) * source_stride_pixels + width,
                      staging.begin() + static_cast<std::size_t>(row) * width);
        }
        upload_from = staging.data();
    }

    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (width != width_ || height != height_) {
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, upload_from);
        width_ = width;
        height_ = height;
        format_ = GL_RGBA;
    } else {
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, upload_from);
    }
}

void Texture::upload_xrgb8888(const uint32_t* source,
                              int width,
                              int height,
                              int source_stride_pixels) {
    if (source == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (!ensure_created()) {
        return;
    }
    channel_order_ = ChannelOrder::XrgbBytes;
    upload_four_bytes_a_pixel(source, width, height, source_stride_pixels);
}

void Texture::upload_rgba8888(const uint32_t* source,
                              int width,
                              int height,
                              int source_stride_pixels) {
    if (source == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (!ensure_created()) {
        return;
    }
    channel_order_ = ChannelOrder::Rgba;
    upload_four_bytes_a_pixel(source, width, height, source_stride_pixels);
}

void Texture::upload_rgb565(const uint8_t* source, int width, int height) {
    if (source == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (!ensure_created()) {
        return;
    }
    // Already in the right channel order, so the shader must not swizzle it.
    channel_order_ = ChannelOrder::Rgba;
    gl::BindTexture(GL_TEXTURE_2D, id_);

    if (filter_dirty_) {
        const GLint filter = linear_ ? GL_LINEAR : GL_NEAREST;
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        filter_dirty_ = false;
    }

    // Two bytes a pixel, so rows are only 4-byte aligned at even widths.
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 2);
    if (width != width_ || height != height_ || format_ != GL_RGB) {
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
                       GL_UNSIGNED_SHORT_5_6_5, source);
        width_ = width;
        height_ = height;
        format_ = GL_RGB;
    } else {
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGB, GL_UNSIGNED_SHORT_5_6_5,
                          source);
    }
}

void Texture::allocate_rgba(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (!ensure_created()) {
        return;
    }
    channel_order_ = ChannelOrder::Rgba;
    gl::BindTexture(GL_TEXTURE_2D, id_);
    if (filter_dirty_) {
        const GLint filter = linear_ ? GL_LINEAR : GL_NEAREST;
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        filter_dirty_ = false;
    }
    if (width != width_ || height != height_) {
        // GL_RGBA8, sized, rather than plain GL_RGBA. This texture exists to be
        // rendered into and copied into, and only the sized formats are
        // guaranteed colour-renderable in a core profile -- an unsized one can
        // leave the framebuffer it is attached to incomplete, at which point a
        // copy into it quietly does nothing.
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(width),
                       static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        width_ = width;
        height_ = height;
    }
    gl::BindTexture(GL_TEXTURE_2D, 0);
}

void Texture::upload_alpha8(const uint8_t* source, int width, int height) {
    if (source == nullptr || width <= 0 || height <= 0) {
        return;
    }
    // Expand to a full RGBA texture (white, variable alpha) so the same upload
    // path works; these textures are tiny (a font atlas) and uploaded once.
    std::vector<uint32_t> expanded(static_cast<std::size_t>(width) * height);
    for (std::size_t i = 0; i < expanded.size(); ++i) {
        const uint32_t alpha = source[i];
        expanded[i] = (alpha << 24) | 0x00ffffffu;  // little-endian bytes: ff ff ff alpha
    }
    upload_xrgb8888(expanded.data(), width, height, width);
}

}  // namespace ds
