#include "render/gl_loader.hpp"

// Desktop GL 3.2 and later report which profile they are through these two.
// OpenGL ES has no profiles at all, so its headers do not define them -- and
// the code below only reads them when the context is not ES, which a device's
// compiler cannot know while it is still refusing to compile the line.
#ifndef GL_CONTEXT_PROFILE_MASK
#define GL_CONTEXT_PROFILE_MASK 0x9126
#endif
#ifndef GL_CONTEXT_CORE_PROFILE_BIT
#define GL_CONTEXT_CORE_PROFILE_BIT 0x00000001
#endif

#include <SDL.h>

#include <cstdlib>
#include <set>
#include <vector>

#include "util/log.hpp"

namespace ds::gl {
namespace {

bool g_is_gles = false;
bool g_is_core_profile = false;
std::vector<std::string>* g_missing = nullptr;

template <typename T>
bool resolve(T& out, const char* name) {
    void* symbol = SDL_GL_GetProcAddress(name);
    if (symbol == nullptr) {
        if (g_missing != nullptr) {
            g_missing->emplace_back(name);
        }
        return false;
    }
    out = reinterpret_cast<T>(symbol);
    return true;
}

}  // namespace

PFN_Viewport Viewport = nullptr;
PFN_ClearColor ClearColor = nullptr;
PFN_Clear Clear = nullptr;
PFN_Enable Enable = nullptr;
PFN_Disable Disable = nullptr;
PFN_BlendFunc BlendFunc = nullptr;
PFN_GetError GetError = nullptr;
PFN_Finish Finish = nullptr;
PFN_GetString GetString = nullptr;
PFN_PixelStorei PixelStorei = nullptr;
PFN_ReadPixels ReadPixels = nullptr;
PFN_Scissor Scissor = nullptr;
PFN_GenTextures GenTextures = nullptr;
PFN_DeleteTextures DeleteTextures = nullptr;
PFN_BindTexture BindTexture = nullptr;
PFN_TexImage2D TexImage2D = nullptr;
PFN_TexSubImage2D TexSubImage2D = nullptr;
PFN_TexParameteri TexParameteri = nullptr;
PFN_ActiveTexture ActiveTexture = nullptr;
PFN_CreateShader CreateShader = nullptr;
PFN_ShaderSource ShaderSource = nullptr;
PFN_CompileShader CompileShader = nullptr;
PFN_GetShaderiv GetShaderiv = nullptr;
PFN_GetShaderInfoLog GetShaderInfoLog = nullptr;
PFN_DeleteShader DeleteShader = nullptr;
PFN_CreateProgram CreateProgram = nullptr;
PFN_AttachShader AttachShader = nullptr;
PFN_LinkProgram LinkProgram = nullptr;
PFN_GetProgramiv GetProgramiv = nullptr;
PFN_GetProgramInfoLog GetProgramInfoLog = nullptr;
PFN_UseProgram UseProgram = nullptr;
PFN_DeleteProgram DeleteProgram = nullptr;
PFN_GetAttribLocation GetAttribLocation = nullptr;
PFN_GetUniformLocation GetUniformLocation = nullptr;
PFN_Uniform1i Uniform1i = nullptr;
PFN_Uniform1f Uniform1f = nullptr;
PFN_Uniform2f Uniform2f = nullptr;
PFN_Uniform4f Uniform4f = nullptr;
PFN_GenBuffers GenBuffers = nullptr;
PFN_DeleteBuffers DeleteBuffers = nullptr;
PFN_BindBuffer BindBuffer = nullptr;
PFN_BufferData BufferData = nullptr;
PFN_BufferSubData BufferSubData = nullptr;
PFN_VertexAttribPointer VertexAttribPointer = nullptr;
PFN_EnableVertexAttribArray EnableVertexAttribArray = nullptr;
PFN_DisableVertexAttribArray DisableVertexAttribArray = nullptr;
PFN_DrawArrays DrawArrays = nullptr;
PFN_GetIntegerv GetIntegerv = nullptr;
PFN_DepthMask DepthMask = nullptr;
PFN_ColorMask ColorMask = nullptr;
PFN_BlendEquation BlendEquation = nullptr;
PFN_BlendFuncSeparate BlendFuncSeparate = nullptr;
PFN_DepthFunc DepthFunc = nullptr;
PFN_CullFace CullFace = nullptr;
PFN_FrontFace FrontFace = nullptr;
PFN_StencilMask StencilMask = nullptr;
PFN_IsEnabled IsEnabled = nullptr;
PFN_BindSampler BindSampler = nullptr;
PFN_GenFramebuffers GenFramebuffers = nullptr;
PFN_DeleteFramebuffers DeleteFramebuffers = nullptr;
PFN_BindFramebuffer BindFramebuffer = nullptr;
PFN_FramebufferTexture2D FramebufferTexture2D = nullptr;
PFN_CheckFramebufferStatus CheckFramebufferStatus = nullptr;
PFN_GenRenderbuffers GenRenderbuffers = nullptr;
PFN_DeleteRenderbuffers DeleteRenderbuffers = nullptr;
PFN_BindRenderbuffer BindRenderbuffer = nullptr;
PFN_RenderbufferStorage RenderbufferStorage = nullptr;
PFN_FramebufferRenderbuffer FramebufferRenderbuffer = nullptr;
PFN_BlitFramebuffer BlitFramebuffer = nullptr;
PFN_GenVertexArrays GenVertexArrays = nullptr;
PFN_DeleteVertexArrays DeleteVertexArrays = nullptr;
PFN_BindVertexArray BindVertexArray = nullptr;

void reset_render_state() {
    if (Enable == nullptr) return;
    // A sampler object bound to a texture unit overrides that texture's own
    // filter and wrap settings. A core that leaves one bound -- and any core
    // with a modern renderer uses them -- silently reinterprets every texture
    // this app samples afterwards. If its filter asks for mipmaps and the
    // texture has none, the texture is incomplete and samples as black: solid
    // quads keep drawing, textured ones quietly stop.
    if (BindSampler != nullptr) BindSampler(0, 0);
    Enable(GL_BLEND);
    BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    if (BlendEquation != nullptr) BlendEquation(GL_FUNC_ADD);
    Disable(GL_DEPTH_TEST);
    if (DepthMask != nullptr) DepthMask(GL_FALSE);
    Disable(GL_STENCIL_TEST);
    Disable(GL_SCISSOR_TEST);
    Disable(GL_CULL_FACE);
    Disable(GL_DITHER);
    if (ColorMask != nullptr) ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (ActiveTexture != nullptr) ActiveTexture(GL_TEXTURE0);
}

std::string take_error() {
    if (GetError == nullptr) return std::string();
    std::string first;
    // Drain: the queue can hold several, and leaving them behind would blame
    // the next caller for this one's mistake.
    for (int guard = 0; guard < 16; ++guard) {
        const GLenum error = GetError();
        if (error == GL_NO_ERROR) break;
        if (!first.empty()) continue;
        switch (error) {
            case GL_INVALID_ENUM: first = "GL_INVALID_ENUM"; break;
            case GL_INVALID_VALUE: first = "GL_INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: first = "GL_INVALID_OPERATION"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION:
                first = "GL_INVALID_FRAMEBUFFER_OPERATION";
                break;
            case GL_OUT_OF_MEMORY: first = "GL_OUT_OF_MEMORY"; break;
            default: first = "GL error " + std::to_string(error); break;
        }
    }
    return first;
}

void log_error_once(const char* label) {
    static std::set<std::string> reported;
    const std::string error = take_error();
    if (error.empty()) return;
    if (!reported.insert(label).second) return;
    log_warn(std::string(label) + ": " + error);
}

#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif

namespace {
GLuint g_window_framebuffer = 0;
}  // namespace

void remember_window_framebuffer() {
    GLint bound = 0;
    if (GetIntegerv != nullptr) {
        GetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
    }
    g_window_framebuffer = static_cast<GLuint>(bound > 0 ? bound : 0);
    if (g_window_framebuffer != 0) {
        log_info("the window is drawn into framebuffer " + std::to_string(g_window_framebuffer) +
                 ", not 0");
    }
}

void bind_window_framebuffer() {
    if (BindFramebuffer != nullptr) {
        BindFramebuffer(GL_FRAMEBUFFER, g_window_framebuffer);
    }
}

unsigned window_framebuffer() { return g_window_framebuffer; }

void reset_state_for_core() {
    if (Enable == nullptr) return;
    // OpenGL's defaults, exactly, because that is where a core's own state
    // tracking believes it is starting from.
    Disable(GL_BLEND);
    BlendFunc(GL_ONE, GL_ZERO);
    if (BlendEquation != nullptr) BlendEquation(GL_FUNC_ADD);
    Disable(GL_DEPTH_TEST);
    if (DepthFunc != nullptr) DepthFunc(GL_LESS);
    if (DepthMask != nullptr) DepthMask(GL_TRUE);
    Disable(GL_STENCIL_TEST);
    if (StencilMask != nullptr) StencilMask(0xFFFFFFFFu);
    Disable(GL_SCISSOR_TEST);
    Disable(GL_CULL_FACE);
    if (CullFace != nullptr) CullFace(GL_BACK);
    if (FrontFace != nullptr) FrontFace(GL_CCW);
    Enable(GL_DITHER);  // on by default, unlike everything else here
    if (ColorMask != nullptr) ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (ActiveTexture != nullptr) ActiveTexture(GL_TEXTURE0);
    if (UseProgram != nullptr) UseProgram(0);
    if (BindVertexArray != nullptr) BindVertexArray(0);
    if (PixelStorei != nullptr) {
        PixelStorei(GL_UNPACK_ALIGNMENT, 4);
        PixelStorei(GL_PACK_ALIGNMENT, 4);
    }
}

bool is_gles() { return g_is_gles; }

namespace {
// Whether the context has the unpack state beyond the alignment -- a desktop
// context, or ES 3 -- or -1 until asked.
int g_full_unpack_state = -1;
}  // namespace

void prepare_unpack(GLint alignment) {
    if (PixelStorei == nullptr) return;
    if (g_full_unpack_state < 0) g_full_unpack_state = (!g_is_gles || at_least(3, 0)) ? 1 : 0;
    if (g_full_unpack_state == 1) {
        constexpr GLenum kPixelUnpackBuffer = 0x88EC;
        constexpr GLenum kUnpackRowLength = 0x0CF2;
        constexpr GLenum kUnpackSkipRows = 0x0CF3;
        constexpr GLenum kUnpackSkipPixels = 0x0CF4;
        if (BindBuffer != nullptr) BindBuffer(kPixelUnpackBuffer, 0);
        PixelStorei(kUnpackRowLength, 0);
        PixelStorei(kUnpackSkipRows, 0);
        PixelStorei(kUnpackSkipPixels, 0);
    }
    PixelStorei(GL_UNPACK_ALIGNMENT, alignment);
}
bool is_core_profile() { return g_is_core_profile; }

void version(int& out_major, int& out_minor) {
    out_major = 0;
    out_minor = 0;
    // GL_MAJOR_VERSION only exists from 3.0 on, so a failure here is itself the
    // answer: parse the version string, which every context has had since 1.0.
    if (GetString == nullptr) return;
    const GLubyte* text = GetString(GL_VERSION);
    if (text == nullptr) return;
    const char* cursor = reinterpret_cast<const char*>(text);
    // Desktop reports "3.3.0 ...", ES reports "OpenGL ES 3.2 ..."; in both the
    // first digit run is the major version.
    while (*cursor != '\0' && (*cursor < '0' || *cursor > '9')) ++cursor;
    if (*cursor == '\0') return;
    out_major = std::atoi(cursor);
    while (*cursor >= '0' && *cursor <= '9') ++cursor;
    if (*cursor == '.') out_minor = std::atoi(cursor + 1);
}

bool at_least(int major, int minor) {
    int have_major = 0;
    int have_minor = 0;
    version(have_major, have_minor);
    return have_major > major || (have_major == major && have_minor >= minor);
}
bool has_blit() { return BlitFramebuffer != nullptr; }
bool has_vertex_arrays() {
    return GenVertexArrays != nullptr && BindVertexArray != nullptr &&
           DeleteVertexArrays != nullptr;
}

void unload() {
    // Everything the resolver can fill in, back to null in one sweep. Anything
    // added to load() has to be added here too, which is why they sit together.
    Viewport = nullptr; ClearColor = nullptr; Clear = nullptr; Enable = nullptr;
    Disable = nullptr; BlendFunc = nullptr; GetError = nullptr; Finish = nullptr; GetString = nullptr;
    PixelStorei = nullptr; ReadPixels = nullptr; Scissor = nullptr; GenTextures = nullptr;
    DeleteTextures = nullptr; BindTexture = nullptr; TexImage2D = nullptr;
    TexSubImage2D = nullptr; TexParameteri = nullptr; ActiveTexture = nullptr;
    CreateShader = nullptr; ShaderSource = nullptr; CompileShader = nullptr;
    GetShaderiv = nullptr; GetShaderInfoLog = nullptr; DeleteShader = nullptr;
    CreateProgram = nullptr; AttachShader = nullptr; LinkProgram = nullptr;
    GetProgramiv = nullptr; GetProgramInfoLog = nullptr; UseProgram = nullptr;
    DeleteProgram = nullptr; GetAttribLocation = nullptr; GetUniformLocation = nullptr;
    Uniform1i = nullptr; Uniform1f = nullptr; Uniform2f = nullptr; Uniform4f = nullptr;
    GenBuffers = nullptr; DeleteBuffers = nullptr; BindBuffer = nullptr;
    BufferData = nullptr; BufferSubData = nullptr; VertexAttribPointer = nullptr;
    EnableVertexAttribArray = nullptr; DisableVertexAttribArray = nullptr;
    DrawArrays = nullptr; GetIntegerv = nullptr; DepthMask = nullptr; ColorMask = nullptr;
    BlendEquation = nullptr; BlendFuncSeparate = nullptr; DepthFunc = nullptr; CullFace = nullptr; FrontFace = nullptr;
    StencilMask = nullptr; IsEnabled = nullptr; BindSampler = nullptr; GenFramebuffers = nullptr;
    DeleteFramebuffers = nullptr; BindFramebuffer = nullptr; FramebufferTexture2D = nullptr;
    CheckFramebufferStatus = nullptr; GenRenderbuffers = nullptr;
    DeleteRenderbuffers = nullptr; BindRenderbuffer = nullptr; RenderbufferStorage = nullptr;
    FramebufferRenderbuffer = nullptr; BlitFramebuffer = nullptr; GenVertexArrays = nullptr;
    DeleteVertexArrays = nullptr; BindVertexArray = nullptr;
}

bool load(bool gles_context, std::string* error) {
    g_is_gles = gles_context;
    g_full_unpack_state = -1;
    g_is_core_profile = false;

    std::vector<std::string> missing;
    g_missing = &missing;

    resolve(Viewport, "glViewport");
    resolve(ClearColor, "glClearColor");
    resolve(Clear, "glClear");
    resolve(Enable, "glEnable");
    resolve(Disable, "glDisable");
    resolve(BlendFunc, "glBlendFunc");
    resolve(GetError, "glGetError");
    resolve(Finish, "glFinish");
    resolve(GetString, "glGetString");
    resolve(PixelStorei, "glPixelStorei");
    resolve(ReadPixels, "glReadPixels");
    resolve(Scissor, "glScissor");
    resolve(GenTextures, "glGenTextures");
    resolve(DeleteTextures, "glDeleteTextures");
    resolve(BindTexture, "glBindTexture");
    resolve(TexImage2D, "glTexImage2D");
    resolve(TexSubImage2D, "glTexSubImage2D");
    resolve(TexParameteri, "glTexParameteri");
    resolve(ActiveTexture, "glActiveTexture");
    resolve(CreateShader, "glCreateShader");
    resolve(ShaderSource, "glShaderSource");
    resolve(CompileShader, "glCompileShader");
    resolve(GetShaderiv, "glGetShaderiv");
    resolve(GetShaderInfoLog, "glGetShaderInfoLog");
    resolve(DeleteShader, "glDeleteShader");
    resolve(CreateProgram, "glCreateProgram");
    resolve(AttachShader, "glAttachShader");
    resolve(LinkProgram, "glLinkProgram");
    resolve(GetProgramiv, "glGetProgramiv");
    resolve(GetProgramInfoLog, "glGetProgramInfoLog");
    resolve(UseProgram, "glUseProgram");
    resolve(DeleteProgram, "glDeleteProgram");
    resolve(GetAttribLocation, "glGetAttribLocation");
    resolve(GetUniformLocation, "glGetUniformLocation");
    resolve(Uniform1i, "glUniform1i");
    resolve(Uniform1f, "glUniform1f");
    resolve(Uniform2f, "glUniform2f");
    resolve(Uniform4f, "glUniform4f");
    resolve(GenBuffers, "glGenBuffers");
    resolve(DeleteBuffers, "glDeleteBuffers");
    resolve(BindBuffer, "glBindBuffer");
    resolve(BufferData, "glBufferData");
    resolve(BufferSubData, "glBufferSubData");
    resolve(VertexAttribPointer, "glVertexAttribPointer");
    resolve(EnableVertexAttribArray, "glEnableVertexAttribArray");
    resolve(DisableVertexAttribArray, "glDisableVertexAttribArray");
    resolve(DrawArrays, "glDrawArrays");
    resolve(GetIntegerv, "glGetIntegerv");
    resolve(DepthMask, "glDepthMask");
    resolve(ColorMask, "glColorMask");
    resolve(BlendEquation, "glBlendEquation");
    resolve(DepthFunc, "glDepthFunc");
    resolve(CullFace, "glCullFace");
    resolve(FrontFace, "glFrontFace");
    resolve(StencilMask, "glStencilMask");
    resolve(IsEnabled, "glIsEnabled");
    resolve(GenFramebuffers, "glGenFramebuffers");
    resolve(DeleteFramebuffers, "glDeleteFramebuffers");
    resolve(BindFramebuffer, "glBindFramebuffer");
    resolve(FramebufferTexture2D, "glFramebufferTexture2D");
    resolve(CheckFramebufferStatus, "glCheckFramebufferStatus");
    resolve(GenRenderbuffers, "glGenRenderbuffers");
    resolve(DeleteRenderbuffers, "glDeleteRenderbuffers");
    resolve(BindRenderbuffer, "glBindRenderbuffer");
    resolve(RenderbufferStorage, "glRenderbufferStorage");
    resolve(FramebufferRenderbuffer, "glFramebufferRenderbuffer");

    g_missing = nullptr;

    // Optional: present on GL 3.0 / ES 3.0 and later, absent on ES 2.0. Their
    // absence is not an error -- only a core that asks for a modern context
    // needs them, and that context is what brings them along.
    resolve(BlitFramebuffer, "glBlitFramebuffer");
    resolve(BindSampler, "glBindSampler");
    resolve(GenVertexArrays, "glGenVertexArrays");
    resolve(DeleteVertexArrays, "glDeleteVertexArrays");
    resolve(BindVertexArray, "glBindVertexArray");
    resolve(BlendFuncSeparate, "glBlendFuncSeparate");

    if (!missing.empty()) {
        std::string names;
        for (const std::string& name : missing) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        if (error != nullptr) {
            *error = "this OpenGL driver is missing required entry points: " + names;
        }
        return false;
    }

    // Desktop 3.2 and later report which profile they are; ES contexts and
    // anything older are not core profiles by definition.
    if (!g_is_gles && GetIntegerv != nullptr && at_least(3, 2)) {
        GLint mask = 0;
        GetIntegerv(GL_CONTEXT_PROFILE_MASK, &mask);
        // A driver that does not answer leaves the error flag set and mask 0;
        // clear it so the first real draw is not blamed for this query.
        if (GetError != nullptr) GetError();
        g_is_core_profile = (mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
    }

    if (GetString != nullptr) {
        const GLubyte* version = GetString(GL_VERSION);
        const GLubyte* renderer = GetString(GL_RENDERER);
        log_info(std::string("GL renderer: ") + (renderer ? reinterpret_cast<const char*>(renderer) : "?") +
                 " / version: " + (version ? reinterpret_cast<const char*>(version) : "?") +
                 (g_is_core_profile ? " (core profile)" : ""));
    }
    return true;
}

}  // namespace ds::gl
