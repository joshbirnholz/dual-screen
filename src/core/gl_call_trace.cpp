#include "core/gl_call_trace.hpp"

#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdio>
#include <cstdlib>
#include <map>

#include "util/log.hpp"

namespace ds::gl_trace {
namespace {

bool g_enabled = false;
bool g_checked_environment = false;
int g_frame = 0;
int g_lines = 0;

// Enough to see a frame's shape without burying the log. A core that draws
// nothing does so on every frame, so the first few are as good as all of them.
constexpr int kMaxFrames = 3;
constexpr int kMaxLines = 400;

GLenum (*real_GetError)() = nullptr;

void* real(const char* name) { return SDL_GL_GetProcAddress(name); }

bool tracing_now() { return g_enabled && g_frame < kMaxFrames && g_lines < kMaxLines; }

// The call has already happened; this says whether OpenGL objected to it.
void note(const std::string& call) {
    if (!tracing_now()) return;
    GLenum error = (real_GetError != nullptr) ? real_GetError() : 0;
    if (error == 0) {
        ++g_lines;
        log_debug("core gl: " + call);
        return;
    }
    const char* text = "an error";
    switch (error) {
        case GL_INVALID_ENUM: text = "GL_INVALID_ENUM"; break;
        case GL_INVALID_VALUE: text = "GL_INVALID_VALUE"; break;
        case GL_INVALID_OPERATION: text = "GL_INVALID_OPERATION"; break;
        case GL_INVALID_FRAMEBUFFER_OPERATION: text = "GL_INVALID_FRAMEBUFFER_OPERATION"; break;
        case GL_OUT_OF_MEMORY: text = "GL_OUT_OF_MEMORY"; break;
        default: break;
    }
    ++g_lines;
    log_error("core gl: " + call + "  ->  " + text);
}

std::string n(long long value) { return std::to_string(value); }

// Real hex, because GL enums are only recognisable that way -- and every table
// in the specification lists them in hex.
std::string hex(unsigned value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%04X", value);
    return buffer;
}

// --- the wrappers ---------------------------------------------------------
// Each one calls through and then reports. Signatures match the GL entry
// points exactly, because the core calls these through its own loader.

// Variadic because a function-pointer type contains commas of its own, which
// a two-parameter macro would split on.
#define REAL(fn, ...)                                                  \
    using RealFn = __VA_ARGS__;                                        \
    static RealFn real_fn = nullptr;                                   \
    if (real_fn == nullptr) real_fn = reinterpret_cast<RealFn>(real(fn)); \
    if (real_fn == nullptr) return;

void APIENTRY trace_Viewport(GLint x, GLint y, GLsizei w, GLsizei h) {
    REAL("glViewport", void (APIENTRY*)(GLint, GLint, GLsizei, GLsizei))
    real_fn(x, y, w, h);
    note("glViewport(" + n(x) + ", " + n(y) + ", " + n(w) + ", " + n(h) + ")");
}

void APIENTRY trace_Scissor(GLint x, GLint y, GLsizei w, GLsizei h) {
    REAL("glScissor", void (APIENTRY*)(GLint, GLint, GLsizei, GLsizei))
    real_fn(x, y, w, h);
    note("glScissor(" + n(x) + ", " + n(y) + ", " + n(w) + ", " + n(h) + ")");
}

void APIENTRY trace_BindFramebuffer(GLenum target, GLuint framebuffer) {
    REAL("glBindFramebuffer", void (APIENTRY*)(GLenum, GLuint))
    real_fn(target, framebuffer);
    note("glBindFramebuffer(" + hex(target) + ", " + n(framebuffer) + ")");
}

void APIENTRY trace_DrawArrays(GLenum mode, GLint first, GLsizei count) {
    REAL("glDrawArrays", void (APIENTRY*)(GLenum, GLint, GLsizei))
    real_fn(mode, first, count);
    note("glDrawArrays(mode=" + hex(mode) + ", first=" + n(first) + ", count=" +
         n(count) + ")");
}

void APIENTRY trace_DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    REAL("glDrawElements", void (APIENTRY*)(GLenum, GLsizei, GLenum, const void*))
    real_fn(mode, count, type, indices);
    note("glDrawElements(mode=" + hex(mode) + ", count=" + n(count) + ")");
}

void APIENTRY trace_Clear(GLbitfield mask) {
    REAL("glClear", void (APIENTRY*)(GLbitfield))
    real_fn(mask);
    note("glClear(" + hex(mask) + ")");
}

void APIENTRY trace_UseProgram(GLuint program) {
    REAL("glUseProgram", void (APIENTRY*)(GLuint))
    real_fn(program);
    note("glUseProgram(" + n(program) + ")");
}

void APIENTRY trace_BindTexture(GLenum target, GLuint texture) {
    REAL("glBindTexture", void (APIENTRY*)(GLenum, GLuint))
    real_fn(target, texture);
    note("glBindTexture(" + hex(target) + ", " + n(texture) + ")");
}

void APIENTRY trace_PixelStorei(GLenum name, GLint param) {
    REAL("glPixelStorei", void (APIENTRY*)(GLenum, GLint))
    real_fn(name, param);
    note("glPixelStorei(" + hex(name) + ", " + n(param) + ")");
}

void APIENTRY trace_TexImage2D(GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h,
                               GLint border, GLenum format, GLenum type, const void* pixels) {
    REAL("glTexImage2D", void (APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum,
                                          GLenum, const void*))
    real_fn(target, level, internal, w, h, border, format, type, pixels);
    note("glTexImage2D(level=" + n(level) + ", internal=" + hex(internal) + ", " +
         n(w) + "x" + n(h) + ")");
}

void APIENTRY trace_TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                                  GLsizei h, GLenum format, GLenum type, const void* pixels) {
    REAL("glTexSubImage2D", void (APIENTRY*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum,
                                             GLenum, const void*))
    real_fn(target, level, x, y, w, h, format, type, pixels);
    note("glTexSubImage2D(at " + n(x) + "," + n(y) + " size " + n(w) + "x" + n(h) + ")");
}

void APIENTRY trace_BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    REAL("glBufferData", void (APIENTRY*)(GLenum, GLsizeiptr, const void*, GLenum))
    real_fn(target, size, data, usage);
    note("glBufferData(target=" + hex(target) + ", size=" +
         n(static_cast<long long>(size)) + ")");
}

void APIENTRY trace_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                  const void* data) {
    REAL("glBufferSubData", void (APIENTRY*)(GLenum, GLintptr, GLsizeiptr, const void*))
    real_fn(target, offset, size, data);
    note("glBufferSubData(target=" + hex(target) + ", offset=" +
         n(static_cast<long long>(offset)) + ", size=" + n(static_cast<long long>(size)) + ")");
}

void APIENTRY trace_BindVertexArray(GLuint array) {
    REAL("glBindVertexArray", void (APIENTRY*)(GLuint))
    real_fn(array);
    note("glBindVertexArray(" + n(array) + ")");
}

void APIENTRY trace_BindBufferBase(GLenum target, GLuint index, GLuint buffer) {
    REAL("glBindBufferBase", void (APIENTRY*)(GLenum, GLuint, GLuint))
    real_fn(target, index, buffer);
    note("glBindBufferBase(target=" + hex(target) + ", index=" + n(index) +
         ", buffer=" + n(buffer) + ")");
}

void APIENTRY trace_BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                                    GLsizeiptr size) {
    REAL("glBindBufferRange", void (APIENTRY*)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr))
    real_fn(target, index, buffer, offset, size);
    note("glBindBufferRange(index=" + n(index) + ", buffer=" + n(buffer) + ", offset=" +
         n(static_cast<long long>(offset)) + ", size=" + n(static_cast<long long>(size)) + ")");
}

#undef REAL

const std::map<std::string, void*>& table() {
    static const std::map<std::string, void*> entries = {
        {"glViewport", reinterpret_cast<void*>(&trace_Viewport)},
        {"glScissor", reinterpret_cast<void*>(&trace_Scissor)},
        {"glBindFramebuffer", reinterpret_cast<void*>(&trace_BindFramebuffer)},
        {"glDrawArrays", reinterpret_cast<void*>(&trace_DrawArrays)},
        {"glDrawElements", reinterpret_cast<void*>(&trace_DrawElements)},
        {"glClear", reinterpret_cast<void*>(&trace_Clear)},
        {"glUseProgram", reinterpret_cast<void*>(&trace_UseProgram)},
        {"glBindTexture", reinterpret_cast<void*>(&trace_BindTexture)},
        {"glPixelStorei", reinterpret_cast<void*>(&trace_PixelStorei)},
        {"glTexImage2D", reinterpret_cast<void*>(&trace_TexImage2D)},
        {"glTexSubImage2D", reinterpret_cast<void*>(&trace_TexSubImage2D)},
        {"glBufferData", reinterpret_cast<void*>(&trace_BufferData)},
        {"glBufferSubData", reinterpret_cast<void*>(&trace_BufferSubData)},
        {"glBindVertexArray", reinterpret_cast<void*>(&trace_BindVertexArray)},
        {"glBindBufferBase", reinterpret_cast<void*>(&trace_BindBufferBase)},
        {"glBindBufferRange", reinterpret_cast<void*>(&trace_BindBufferRange)},
    };
    return entries;
}

}  // namespace

bool enabled() {
    if (!g_checked_environment) {
        g_checked_environment = true;
        const char* value = std::getenv("DUALSCREEN_TRACE_CORE_GL");
        g_enabled = value != nullptr && value[0] != '\0' && value[0] != '0';
        if (g_enabled) {
            real_GetError = reinterpret_cast<GLenum (*)()>(real("glGetError"));
            log_info("tracing the core's OpenGL calls for the first few frames");
        }
    }
    return g_enabled;
}

void* wrapper_for(const std::string& name) {
    if (!enabled()) return nullptr;
    const auto it = table().find(name);
    return it == table().end() ? nullptr : it->second;
}

void begin_frame() {
    if (!g_enabled) return;
    if (g_frame < kMaxFrames) {
        log_info("--- core frame " + std::to_string(g_frame) + " ---");
    }
    ++g_frame;
}

}  // namespace ds::gl_trace
