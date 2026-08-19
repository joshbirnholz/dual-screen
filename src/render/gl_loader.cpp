#include "render/gl_loader.hpp"

#include <SDL.h>

#include <vector>

#include "util/log.hpp"

namespace ds::gl {
namespace {

bool g_is_gles = false;
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

bool is_gles() { return g_is_gles; }

bool load(bool gles_context, std::string* error) {
    g_is_gles = gles_context;

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

    g_missing = nullptr;

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

    if (GetString != nullptr) {
        const GLubyte* version = GetString(GL_VERSION);
        const GLubyte* renderer = GetString(GL_RENDERER);
        log_info(std::string("GL renderer: ") + (renderer ? reinterpret_cast<const char*>(renderer) : "?") +
                 " / version: " + (version ? reinterpret_cast<const char*>(version) : "?"));
    }
    return true;
}

}  // namespace ds::gl
