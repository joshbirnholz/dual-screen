#pragma once

#include <cstdint>

#include "render/gl_loader.hpp"

namespace ds {

// A 2D RGBA texture whose storage is (re)allocated on demand. Filtering is
// nearest by default — DS output is 256x192 pixel art and bilinear scaling
// makes it mushy.
// How the bytes in a texture are arranged. Core output arrives as XRGB8888 and
// is uploaded verbatim, so the shader has to swizzle it back; a texture the GPU
// filled itself is already in the right order and must not be swizzled again.
enum class ChannelOrder { XrgbBytes, Rgba };

class Texture {
public:
    Texture() = default;
    ~Texture();

    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    // Uploads a tightly packed or strided XRGB8888 region. `source` points at
    // the top-left pixel of the region to upload; `source_stride_pixels` is the
    // stride of the buffer it lives in.
    //
    // The bytes go to the GPU as GL_RGBA and are swizzled back to the right
    // channel order in the shader, rather than uploaded as GL_BGRA — BGRA
    // upload is a desktop-GL-only path, and the roadmap has this renderer
    // running on OpenGL ES.
    void upload_xrgb8888(const uint32_t* source,
                         int width,
                         int height,
                         int source_stride_pixels);

    // Uploads tightly packed RGB565, the form a streamed screen arrives in.
    //
    // Uploading it as itself rather than expanding to 8 bits a channel first
    // saves a whole pass over the frame and halves what goes to the GPU, every
    // frame, on both devices. GL_UNSIGNED_SHORT_5_6_5 is core in both desktop
    // GL and ES, and the channels come out in the right order, so nothing
    // downstream has to swizzle -- see ChannelOrder.
    void upload_rgb565(const uint8_t* source, int width, int height);

    // Uploads 8-bit coverage data as an alpha-only-style RGBA texture (white
    // with the given alpha), used by the text renderer.
    void upload_alpha8(const uint8_t* source, int width, int height);

    // Storage with no data in it, for a texture something else will render
    // into. The contents are whatever the GPU puts there, in RGBA order.
    void allocate_rgba(int width, int height);

    ChannelOrder channel_order() const { return channel_order_; }

    void set_linear_filtering(bool linear);

    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool valid() const { return id_ != 0; }

    void destroy();

private:
    void ensure_created();

    GLuint id_ = 0;
    int width_ = 0;
    int height_ = 0;
    // Which GL format the storage was allocated with. A texture that was
    // 5-6-5 and is handed 8-bit pixels (or the reverse) has to be reallocated
    // rather than sub-imaged, whatever its size.
    GLenum format_ = 0;
    ChannelOrder channel_order_ = ChannelOrder::XrgbBytes;
    bool linear_ = false;
    bool filter_dirty_ = true;
};

}  // namespace ds
