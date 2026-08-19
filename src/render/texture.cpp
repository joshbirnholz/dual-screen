#include "render/texture.hpp"

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

void Texture::ensure_created() {
    if (id_ == 0) {
        gl::GenTextures(1, &id_);
        filter_dirty_ = true;
    }
}

void Texture::set_linear_filtering(bool linear) {
    if (linear_ != linear) {
        linear_ = linear;
        filter_dirty_ = true;
    }
}

void Texture::upload_xrgb8888(const uint32_t* source,
                              int width,
                              int height,
                              int source_stride_pixels) {
    if (source == nullptr || width <= 0 || height <= 0) {
        return;
    }
    ensure_created();
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
    } else {
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, upload_from);
    }
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
