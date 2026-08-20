#include "core/hw_render_target.hpp"

#include "util/log.hpp"

#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 0x81A6
#endif

namespace ds {
namespace {

std::string framebuffer_status_text(GLenum status) {
    switch (status) {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            return "an attachment is incomplete";
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            return "it has no attachments";
        case GL_FRAMEBUFFER_UNSUPPORTED:
            return "this driver does not support that combination of formats";
        default:
            return "status " + std::to_string(status);
    }
}

}  // namespace

HwRenderTarget::~HwRenderTarget() { destroy(); }

bool HwRenderTarget::create(unsigned width, unsigned height, bool want_depth, bool want_stencil,
                            std::string* error) {
    if (width == 0 || height == 0) {
        if (error != nullptr) *error = "the core asked for a zero-sized framebuffer";
        return false;
    }
    if (gl::GenFramebuffers == nullptr) {
        if (error != nullptr) {
            *error = "this OpenGL driver has no framebuffer objects, which GPU-rendering cores need";
        }
        return false;
    }
    // Already big enough, and already carrying everything the core wants.
    if (valid() && width <= width_ && height <= height_ && has_depth_ >= want_depth &&
        has_stencil_ >= want_stencil) {
        return true;
    }
    // Growing rather than shrinking: a core that reports a smaller size later
    // still draws into the top-left of the same texture.
    width = width > width_ ? width : width_;
    height = height > height_ ? height : height_;
    want_depth = want_depth || has_depth_;
    want_stencil = want_stencil || has_stencil_;
    destroy();

    gl::GenTextures(1, &texture_);
    gl::BindTexture(GL_TEXTURE_2D, texture_);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width),
                   static_cast<GLsizei>(height), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    // No mipmaps and clamped, or an incomplete texture would make the whole
    // framebuffer incomplete.
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::BindTexture(GL_TEXTURE_2D, 0);

    gl::GenFramebuffers(1, &framebuffer_);
    gl::BindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_, 0);

    if (want_depth || want_stencil) {
        // One packed buffer serves both. A core that asked for only depth is
        // not harmed by having a stencil it never uses, and the packed format
        // is the one drivers universally accept.
        gl::GenRenderbuffers(1, &depth_stencil_);
        gl::BindRenderbuffer(GL_RENDERBUFFER, depth_stencil_);
        gl::RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, static_cast<GLsizei>(width),
                                static_cast<GLsizei>(height));
        gl::BindRenderbuffer(GL_RENDERBUFFER, 0);
        gl::FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                                    depth_stencil_);
    }

    const GLenum status = gl::CheckFramebufferStatus(GL_FRAMEBUFFER);
    gl::BindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (error != nullptr) {
            *error = "could not build the core's framebuffer: " + framebuffer_status_text(status);
        }
        destroy();
        return false;
    }

    width_ = width;
    height_ = height;
    has_depth_ = want_depth;
    has_stencil_ = want_stencil;
    log_info("gave the core a " + std::to_string(width) + "x" + std::to_string(height) +
             " framebuffer to render into");
    return true;
}

void HwRenderTarget::destroy() {
    if (depth_stencil_ != 0 && gl::DeleteRenderbuffers != nullptr) {
        gl::DeleteRenderbuffers(1, &depth_stencil_);
    }
    if (framebuffer_ != 0 && gl::DeleteFramebuffers != nullptr) {
        gl::DeleteFramebuffers(1, &framebuffer_);
    }
    if (texture_ != 0 && gl::DeleteTextures != nullptr) {
        gl::DeleteTextures(1, &texture_);
    }
    depth_stencil_ = 0;
    framebuffer_ = 0;
    texture_ = 0;
    width_ = 0;
    height_ = 0;
    has_depth_ = false;
    has_stencil_ = false;
}

}  // namespace ds
