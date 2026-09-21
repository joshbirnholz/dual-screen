#include "render/texture.hpp"

// Constants that are core in desktop GL but absent from the OpenGL ES headers,
// which is what iOS, tvOS and the Switch compile against. Each is the same
// number everywhere -- GL enum values are fixed by the registry, not by the
// header that happens to spell them -- so defining them when the header did
// not is filling a gap rather than guessing.
//
// GL_RED and GL_RG are only ever reached on a desktop core profile, where
// GL_LUMINANCE was removed (see plane_format below). A device never evaluates
// that branch; it still has to compile it.
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif
#ifndef GL_RED
#define GL_RED 0x1903
#endif
#ifndef GL_RG
#define GL_RG 0x8227
#endif

#include <algorithm>
#include <utility>
#include <cstring>
#include <vector>

namespace ds {

Texture::~Texture() { destroy(); }

Texture::Texture(Texture&& other) noexcept
    : id_(other.id_),
      width_(other.width_),
      height_(other.height_),
      channel_order_(other.channel_order_),
      linear_(other.linear_),
      filter_dirty_(other.filter_dirty_) {
    for (int index = 0; index < 2; ++index) {
        chroma_[index] = other.chroma_[index];
        chroma_width_[index] = other.chroma_width_[index];
        chroma_height_[index] = other.chroma_height_[index];
        chroma_format_[index] = other.chroma_format_[index];
        other.chroma_[index] = 0;
    }
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
        channel_order_ = other.channel_order_;
        linear_ = other.linear_;
        filter_dirty_ = other.filter_dirty_;
        for (int index = 0; index < 2; ++index) {
            chroma_[index] = other.chroma_[index];
            chroma_width_[index] = other.chroma_width_[index];
            chroma_height_[index] = other.chroma_height_[index];
            chroma_format_[index] = other.chroma_format_[index];
            other.chroma_[index] = 0;
        }
        other.id_ = 0;
        other.width_ = 0;
        other.height_ = 0;
    }
    return *this;
}

void Texture::destroy_chroma() {
    for (int index = 0; index < 2; ++index) {
        if (chroma_[index] != 0 && gl::DeleteTextures != nullptr) {
            gl::DeleteTextures(1, &chroma_[index]);
        }
        chroma_[index] = 0;
        chroma_width_[index] = 0;
        chroma_height_[index] = 0;
        chroma_format_[index] = 0;
    }
}

void Texture::destroy() {
    if (id_ != 0 && gl::DeleteTextures != nullptr) {
        gl::DeleteTextures(1, &id_);
    }
    id_ = 0;
    width_ = 0;
    height_ = 0;
    destroy_chroma();
}

namespace {

// Whether this context still has the old single- and two-channel formats.
//
// GL_LUMINANCE and GL_LUMINANCE_ALPHA are core in OpenGL ES 2.0, which is what
// every device this runs on actually creates -- but gl_window will settle for
// a 3.3 core profile, and those two were removed there. GL_RED and GL_RG are
// the replacements, and the only visible difference is which component of a
// two-channel texel the second byte lands in: .a for LUMINANCE_ALPHA, .g for
// RG. The shader is told which; see u_chroma_v_in_alpha.
//
// Asked of the driver rather than inferred from the profile, because that is
// the thing actually being asked -- and it costs one throwaway texture, once.
bool luminance_formats_available() {
    static const bool available = [] {
        if (gl::GenTextures == nullptr || gl::GetError == nullptr) {
            return true;  // no context to ask; the ES answer is the safe guess
        }
        while (gl::GetError() != GL_NO_ERROR) {
            // Someone else's error, cleared so it is not mistaken for ours.
        }
        GLuint probe = 0;
        gl::GenTextures(1, &probe);
        if (probe == 0) return true;
        gl::BindTexture(GL_TEXTURE_2D, probe);
        const uint8_t pixels[4] = {0, 0, 0, 0};
        gl::TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_LUMINANCE), 2, 2, 0, GL_LUMINANCE,
                       GL_UNSIGNED_BYTE, pixels);
        const bool ok = gl::GetError() == GL_NO_ERROR;
        gl::BindTexture(GL_TEXTURE_2D, 0);
        gl::DeleteTextures(1, &probe);
        return ok;
    }();
    return available;
}

// The format to actually ask for, given how many channels the plane has.
GLenum plane_format(int channels) {
    if (luminance_formats_available()) {
        return channels == 2 ? GL_LUMINANCE_ALPHA : GL_LUMINANCE;
    }
    return channels == 2 ? GL_RG : GL_RED;
}

}  // namespace

bool nv12_chroma_v_in_alpha() { return luminance_formats_available(); }

namespace {
}  // namespace

void Texture::upload_plane(GLuint& id, int& cached_width, int& cached_height,
                           GLenum& cached_format, GLenum format, const uint8_t* source,
                           int source_stride, int width, int height) {
    if (gl::GenTextures == nullptr || source == nullptr || width <= 0 || height <= 0) {
        return;
    }
    if (id == 0) {
        gl::GenTextures(1, &id);
        if (id == 0) return;
        cached_width = 0;
        cached_height = 0;
        cached_format = 0;
    }
    gl::BindTexture(GL_TEXTURE_2D, id);
    // Always linear, whatever the screen's own filtering says. These are not
    // the picture, they are its parts: chroma is half resolution and reads as
    // blocky colour edges when point-sampled, and the luma plane is resampled
    // again by the draw itself, where the screen's own choice does apply.
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // One byte a channel, so rows can start anywhere.
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    const int bytes_per_pixel = (format == GL_LUMINANCE_ALPHA || format == GL_RG) ? 2 : 1;
    // A decoder pads its rows, and GL_UNPACK_ROW_LENGTH is desktop-only, so a
    // padded plane is packed here first. Still far cheaper than converting it:
    // this copies bytes, it does not compute a colour per pixel.
    static thread_local std::vector<uint8_t> staging;
    const uint8_t* upload_from = source;
    if (source_stride != width * bytes_per_pixel) {
        const std::size_t row_bytes = static_cast<std::size_t>(width) * bytes_per_pixel;
        staging.resize(row_bytes * height);
        for (int row = 0; row < height; ++row) {
            std::memcpy(staging.data() + row_bytes * row,
                        source + static_cast<std::size_t>(row) * source_stride, row_bytes);
        }
        upload_from = staging.data();
    }

    if (width != cached_width || height != cached_height || format != cached_format) {
        gl::TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0, format,
                       GL_UNSIGNED_BYTE, upload_from);
        cached_width = width;
        cached_height = height;
        cached_format = format;
    } else {
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE,
                          upload_from);
    }
}

void Texture::upload_nv12(const uint8_t* luma, int luma_stride, const uint8_t* chroma,
                          int chroma_stride, int width, int height) {
    if (!ensure_created()) return;
    // The luma plane is this texture itself, so everything that already draws
    // a screen keeps drawing one.
    upload_plane(id_, width_, height_, format_, plane_format(1), luma, luma_stride, width,
                 height);
    upload_plane(chroma_[0], chroma_width_[0], chroma_height_[0], chroma_format_[0],
                 plane_format(2), chroma, chroma_stride, (width + 1) / 2, (height + 1) / 2);
    channel_order_ = ChannelOrder::Nv12;
}

void Texture::upload_yuv420p(const uint8_t* luma, int luma_stride, const uint8_t* u, int u_stride,
                             const uint8_t* v, int v_stride, int width, int height) {
    if (!ensure_created()) return;
    upload_plane(id_, width_, height_, format_, plane_format(1), luma, luma_stride, width,
                 height);
    upload_plane(chroma_[0], chroma_width_[0], chroma_height_[0], chroma_format_[0],
                 plane_format(1), u, u_stride, (width + 1) / 2, (height + 1) / 2);
    upload_plane(chroma_[1], chroma_width_[1], chroma_height_[1], chroma_format_[1],
                 plane_format(1), v, v_stride, (width + 1) / 2, (height + 1) / 2);
    channel_order_ = ChannelOrder::Yuv420p;
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
    // A screen that was a video frame and is now ordinary pixels must not
    // keep the planes it had: the shader picks its path from channel_order_,
    // but leaving them allocated would hold video memory for a picture that
    // has stopped being one.
    destroy_chroma();
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
    // See upload_four_bytes_a_pixel(): a screen that has stopped being a video
    // frame lets go of the planes it had.
    destroy_chroma();
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
