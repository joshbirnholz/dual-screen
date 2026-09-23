#pragma once

#include <cstdint>
#include <memory>

#include "render/gl_loader.hpp"

namespace ds {

// A 2D RGBA texture whose storage is (re)allocated on demand. Filtering is
// nearest by default — DS output is 256x192 pixel art and bilinear scaling
// makes it mushy.
// How the bytes in a texture are arranged. Core output arrives as XRGB8888 and
// is uploaded verbatim, so the shader has to swizzle it back; a texture the GPU
// filled itself is already in the right order and must not be swizzled again.
// Planar layouts are the two a video decoder hands back (see DecodedLayout in
// video/video_codec.hpp): the conversion to RGB happens in the fragment shader
// rather than on the CPU, which on a handheld is the difference between 22ms a
// frame and about 4. A texture in one of these carries its chroma planes with
// it -- see upload_nv12()/upload_yuv420p() -- so everything that draws a
// screen carries on drawing one texture and never has to know.
enum class ChannelOrder { XrgbBytes, Rgba, Nv12, Yuv420p };

// True for the layouts whose colour is worked out while drawing.
// Whether an NV12 texture's V component lands in .a (GL_LUMINANCE_ALPHA, the
// OpenGL ES 2.0 spelling) or .g (GL_RG, its core-profile replacement). The
// shader has to be told, since it is the only difference between the two.
bool nv12_chroma_v_in_alpha();

inline bool is_planar(ChannelOrder order) {
    return order == ChannelOrder::Nv12 || order == ChannelOrder::Yuv420p;
}

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

    // Uploads a tightly packed or strided region that is already byte order
    // R,G,B,A -- what a GPU-rendered screen (a Wii U's, over the external
    // frame link) arrives as, unlike a CPU-rendered libretro core's XRGB8888.
    // No swizzling needed downstream; see ChannelOrder.
    void upload_rgba8888(const uint32_t* source,
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

    // A decoded video frame, still in the planes its decoder produced.
    //
    // `luma` is width x height, one byte a pixel. `chroma` is half that in
    // each direction, two bytes a pixel for NV12 (U and V interleaved) or one
    // byte a pixel each for the two planes of YUV420P. Each `stride` is that
    // plane's own row length in bytes, which a decoder is free to make wider
    // than the picture -- passing it through means no repacking pass.
    //
    // What arrives is uploaded as-is and turned into colour by the shader:
    // see ChannelOrder above for why that is worth the extra texture units.
    void upload_nv12(const uint8_t* luma, int luma_stride, const uint8_t* chroma,
                     int chroma_stride, int width, int height);
    void upload_yuv420p(const uint8_t* luma, int luma_stride, const uint8_t* u, int u_stride,
                        const uint8_t* v, int v_stride, int width, int height);

    // A decoded picture the GPU already holds -- DecodedFrame::native, a
    // CVPixelBuffer on an iPhone or an Apple TV -- drawn from where it is, as
    // the same two planes upload_nv12() would have made. Nothing is copied
    // and nothing is uploaded; the texture keeps the picture alive until it
    // is handed another one.
    //
    // False, with the texture as it was, where this cannot be done: every
    // other platform, and a device whose texture cache refused. The caller
    // then copies the planes out and uploads them the ordinary way.
    bool adopt_native_nv12(const std::shared_ptr<void>& picture, int width, int height);

    // Whether this texture's chroma carries V in .a (GL_LUMINANCE_ALPHA) or
    // .g (GL_RG). Per texture rather than per context, because a texture the
    // decoder's memory was mapped into may not have been given the format an
    // upload would have chosen -- see adopt_native_nv12().
    bool chroma_v_in_alpha() const { return chroma_v_in_alpha_; }

    // The chroma planes of a planar screen, for the renderer to bind
    // alongside this one. Zero on a texture that is not planar.
    GLuint chroma_id() const { return chroma_[0]; }
    GLuint chroma_v_id() const { return chroma_[1]; }

    ChannelOrder channel_order() const { return channel_order_; }

    void set_linear_filtering(bool linear);

    GLuint id() const { return id_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool valid() const { return id_ != 0; }

    void destroy();

private:
    // False when there is no context to create one against, which every
    // upload treats as "nothing to upload into".
    bool ensure_created();
    // Shared by upload_xrgb8888() and upload_rgba8888(), which differ only in
    // the ChannelOrder they leave the texture tagged with afterward.
    void upload_four_bytes_a_pixel(const uint32_t* source, int width, int height,
                                   int source_stride_pixels);
    // Uploads one 8-bit-per-channel plane into `id`, (re)allocating it when
    // its size or format changed. `format` is GL_LUMINANCE for a one-byte
    // plane or GL_LUMINANCE_ALPHA for NV12's interleaved pair -- both of them
    // core in OpenGL ES 2.0, which is what this runs on.
    void upload_plane(GLuint& id, int& cached_width, int& cached_height, GLenum& cached_format,
                      GLenum format, const uint8_t* source, int source_stride, int width,
                      int height);
    void destroy_chroma();
    // Lets go of an adopted picture and the texture names that came with it,
    // which belong to the texture cache and are never deleted here.
    void release_native();

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
    // The chroma planes of a planar screen: one for NV12, two for YUV420P.
    // Owned here so that a screen stays one Texture from every caller's point
    // of view -- see ChannelOrder.
    GLuint chroma_[2] = {0, 0};
    int chroma_width_[2] = {0, 0};
    int chroma_height_[2] = {0, 0};
    GLenum chroma_format_[2] = {0, 0};
    bool chroma_v_in_alpha_ = true;
    // Set while id_ and chroma_[0] are an adopted picture's rather than this
    // texture's own. Holds the picture and the cache's two textures, and
    // releases all three when it goes.
    std::shared_ptr<void> native_;
};

}  // namespace ds
