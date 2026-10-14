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

// Drawing a decoded picture from where the decoder left it -- see
// adopt_native_nv12(). iOS and tvOS only: CoreVideo's OpenGL ES texture cache
// is theirs, and a Mac's equivalent hands back rectangle textures the shader
// does not sample. DS_TYPECHECK_APPLE is the test that compiles this half on a
// machine with no Apple SDK, against the declarations in tests/apple_stub.
#if defined(__APPLE__) || defined(DS_TYPECHECK_APPLE)
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#define DS_NATIVE_VIDEO_TEXTURES 1
#endif
#endif

#if DS_NATIVE_VIDEO_TEXTURES
// The OpenGL ES half of CoreVideo is marked deprecated along with OpenGL ES
// itself, which this whole renderer is built on; being told so on every call
// is noise.
#define COREVIDEO_SILENCE_GL_DEPRECATION 1
#include <CoreVideo/CoreVideo.h>
#include <SDL_video.h>

#include "util/log.hpp"
#endif

namespace ds {

Texture::~Texture() { destroy(); }

Texture::Texture(Texture&& other) noexcept
    : id_(other.id_),
      width_(other.width_),
      height_(other.height_),
      channel_order_(other.channel_order_),
      linear_(other.linear_),
      filter_dirty_(other.filter_dirty_),
      chroma_v_in_alpha_(other.chroma_v_in_alpha_),
      native_(std::move(other.native_)) {
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
        chroma_v_in_alpha_ = other.chroma_v_in_alpha_;
        native_ = std::move(other.native_);
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

void Texture::release_native() {
    // The names are the cache's, and go back to it when native_ lets go of
    // the textures that own them. Deleting them here as well would be a
    // double free of somebody else's texture.
    native_.reset();
    id_ = 0;
    width_ = 0;
    height_ = 0;
    format_ = 0;
    for (int index = 0; index < 2; ++index) {
        chroma_[index] = 0;
        chroma_width_[index] = 0;
        chroma_height_[index] = 0;
        chroma_format_[index] = 0;
    }
}

void Texture::destroy_chroma() {
    // An adopted picture's chroma is not this texture's to delete, and its
    // luma goes with it: the two are one picture.
    if (native_ != nullptr) {
        release_native();
        return;
    }
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
    if (native_ != nullptr) {
        release_native();
    }
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
        gl::prepare_unpack(1);
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
    gl::prepare_unpack(1);
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
    chroma_v_in_alpha_ = nv12_chroma_v_in_alpha();
}

#if DS_NATIVE_VIDEO_TEXTURES
namespace {

// One per GL context, made the first time a picture needs it. The context is
// SDL's, and on iOS and tvOS what SDL hands back as one is the EAGLContext
// itself -- which is what the cache has to be made against.
struct NativeTextureCache {
    void* context = nullptr;
    CVOpenGLESTextureCacheRef cache = nullptr;
};

NativeTextureCache& native_texture_cache() {
    static NativeTextureCache cache;
    return cache;
}

CVOpenGLESTextureCacheRef cache_for_current_context() {
    NativeTextureCache& state = native_texture_cache();
    void* context = SDL_GL_GetCurrentContext();
    if (context == nullptr) {
        return nullptr;
    }
    if (state.cache != nullptr && state.context == context) {
        return state.cache;
    }
    if (state.cache != nullptr) {
        CFRelease(state.cache);
        state.cache = nullptr;
        state.context = nullptr;
    }
    const CVReturn made =
        CVOpenGLESTextureCacheCreate(kCFAllocatorDefault, nullptr, context, nullptr, &state.cache);
    if (made != kCVReturnSuccess || state.cache == nullptr) {
        log_warn("video: this device would not make a texture cache (" + std::to_string(made) +
                 "), so decoded pictures are copied out instead");
        state.cache = nullptr;
        return nullptr;
    }
    state.context = context;
    return state.cache;
}

// What an adopted picture keeps alive: the picture itself, and the two
// textures the cache mapped its planes into. Released together, the textures
// first, when the screen moves on to its next picture.
struct AdoptedPicture {
    std::shared_ptr<void> picture;
    CVOpenGLESTextureRef luma = nullptr;
    CVOpenGLESTextureRef chroma = nullptr;

    AdoptedPicture() = default;
    AdoptedPicture(const AdoptedPicture&) = delete;
    AdoptedPicture& operator=(const AdoptedPicture&) = delete;
    ~AdoptedPicture() {
        if (luma != nullptr) CFRelease(luma);
        if (chroma != nullptr) CFRelease(chroma);
    }
};

CVOpenGLESTextureRef map_plane(CVOpenGLESTextureCacheRef cache, CVPixelBufferRef picture,
                               std::size_t plane, GLenum format) {
    const GLsizei width = static_cast<GLsizei>(CVPixelBufferGetWidthOfPlane(picture, plane));
    const GLsizei height = static_cast<GLsizei>(CVPixelBufferGetHeightOfPlane(picture, plane));
    CVOpenGLESTextureRef texture = nullptr;
    const CVReturn made = CVOpenGLESTextureCacheCreateTextureFromImage(
        kCFAllocatorDefault, cache, picture, nullptr, GL_TEXTURE_2D, static_cast<GLint>(format),
        width, height, format, GL_UNSIGNED_BYTE, plane, &texture);
    return made == kCVReturnSuccess ? texture : nullptr;
}

}  // namespace

bool Texture::adopt_native_nv12(const std::shared_ptr<void>& picture, int width, int height) {
    if (picture == nullptr || width <= 0 || height <= 0 || gl::BindTexture == nullptr) {
        return false;
    }
    CVOpenGLESTextureCacheRef cache = cache_for_current_context();
    if (cache == nullptr) {
        return false;
    }
    // Textures the last pictures were mapped into, handed back now that
    // nothing holds them. Without this the cache keeps every one it ever made.
    CVOpenGLESTextureCacheFlush(cache, 0);

    auto adopted = std::make_shared<AdoptedPicture>();
    adopted->picture = picture;
    auto* buffer = static_cast<CVPixelBufferRef>(picture.get());
    // The formats an upload would have used first, so the shader is told the
    // same thing either way; the core-profile spellings if the cache will not
    // take those. Which one the chroma ended up in is recorded per texture --
    // see chroma_v_in_alpha().
    bool v_in_alpha = true;
    adopted->luma = map_plane(cache, buffer, 0, GL_LUMINANCE);
    adopted->chroma = map_plane(cache, buffer, 1, GL_LUMINANCE_ALPHA);
    if (adopted->luma == nullptr || adopted->chroma == nullptr) {
        adopted = std::make_shared<AdoptedPicture>();
        adopted->picture = picture;
        adopted->luma = map_plane(cache, buffer, 0, GL_RED);
        adopted->chroma = map_plane(cache, buffer, 1, GL_RG);
        v_in_alpha = false;
    }
    if (adopted->luma == nullptr || adopted->chroma == nullptr) {
        return false;
    }

    // Only now, with the new picture in hand: a texture of this one's own
    // storage is deleted, and an earlier adopted picture goes back to the
    // cache.
    destroy();
    for (CVOpenGLESTextureRef plane : {adopted->luma, adopted->chroma}) {
        gl::BindTexture(CVOpenGLESTextureGetTarget(plane), CVOpenGLESTextureGetName(plane));
        // As upload_plane() sets them. The luma's own filter is the screen's
        // choice and is set again when it is drawn.
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    gl::BindTexture(GL_TEXTURE_2D, 0);
    id_ = CVOpenGLESTextureGetName(adopted->luma);
    chroma_[0] = CVOpenGLESTextureGetName(adopted->chroma);
    width_ = width;
    height_ = height;
    format_ = v_in_alpha ? GL_LUMINANCE : GL_RED;
    chroma_width_[0] = (width + 1) / 2;
    chroma_height_[0] = (height + 1) / 2;
    chroma_format_[0] = v_in_alpha ? GL_LUMINANCE_ALPHA : GL_RG;
    chroma_v_in_alpha_ = v_in_alpha;
    channel_order_ = ChannelOrder::Nv12;
    native_ = std::move(adopted);
    return true;
}
#else
bool Texture::adopt_native_nv12(const std::shared_ptr<void>&, int, int) { return false; }
#endif

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
    // An adopted picture's names are not this texture's to upload into.
    if (native_ != nullptr) {
        release_native();
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

    gl::prepare_unpack(4);
    // The format too, not just the size: the same screen at the same size can
    // arrive as 5-6-5 tiles one frame and four bytes a pixel the next, or as
    // the one-channel luma of a video frame, and four bytes written into the
    // storage one of those left behind is an error on OpenGL ES and, on
    // desktop GL, a picture quietly cut down to that storage's channels.
    if (width != width_ || height != height_ || format_ != GL_RGBA) {
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
    gl::prepare_unpack(2);
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
        gl::prepare_unpack(4);
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
