#include "render/gl_loader.hpp"

#include <SDL.h>

#include <cstdlib>
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

bool is_gles() { return g_is_gles; }
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
    Disable = nullptr; BlendFunc = nullptr; GetError = nullptr; GetString = nullptr;
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
    DrawArrays = nullptr; GetIntegerv = nullptr; GenFramebuffers = nullptr;
    DeleteFramebuffers = nullptr; BindFramebuffer = nullptr; FramebufferTexture2D = nullptr;
    CheckFramebufferStatus = nullptr; GenRenderbuffers = nullptr;
    DeleteRenderbuffers = nullptr; BindRenderbuffer = nullptr; RenderbufferStorage = nullptr;
    FramebufferRenderbuffer = nullptr; BlitFramebuffer = nullptr; GenVertexArrays = nullptr;
    DeleteVertexArrays = nullptr; BindVertexArray = nullptr;
}

bool load(bool gles_context, std::string* error) {
    g_is_gles = gles_context;
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
    resolve(GenVertexArrays, "glGenVertexArrays");
    resolve(DeleteVertexArrays, "glDeleteVertexArrays");
    resolve(BindVertexArray, "glBindVertexArray");

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
