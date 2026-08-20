#pragma once

#include <SDL_opengl.h>

#include <string>

// Every GL entry point is resolved through SDL at runtime and lives in
// ds::gl:: rather than as a global `gl*` symbol. That keeps the app agnostic
// about whether it got a desktop GL context or an OpenGL ES one — which is what
// lets the same renderer target SteamOS today and Android/Switch later.
namespace ds::gl {

using PFN_Viewport = void (*)(GLint, GLint, GLsizei, GLsizei);
using PFN_ClearColor = void (*)(GLclampf, GLclampf, GLclampf, GLclampf);
using PFN_Clear = void (*)(GLbitfield);
using PFN_Enable = void (*)(GLenum);
using PFN_Disable = void (*)(GLenum);
using PFN_BlendFunc = void (*)(GLenum, GLenum);
using PFN_GetError = GLenum (*)();
using PFN_GetString = const GLubyte* (*)(GLenum);
using PFN_PixelStorei = void (*)(GLenum, GLint);
using PFN_ReadPixels = void (*)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
using PFN_Scissor = void (*)(GLint, GLint, GLsizei, GLsizei);

using PFN_GenTextures = void (*)(GLsizei, GLuint*);
using PFN_DeleteTextures = void (*)(GLsizei, const GLuint*);
using PFN_BindTexture = void (*)(GLenum, GLuint);
using PFN_TexImage2D = void (*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
using PFN_TexSubImage2D = void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*);
using PFN_TexParameteri = void (*)(GLenum, GLenum, GLint);
using PFN_ActiveTexture = void (*)(GLenum);

using PFN_CreateShader = GLuint (*)(GLenum);
using PFN_ShaderSource = void (*)(GLuint, GLsizei, const GLchar* const*, const GLint*);
using PFN_CompileShader = void (*)(GLuint);
using PFN_GetShaderiv = void (*)(GLuint, GLenum, GLint*);
using PFN_GetShaderInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_DeleteShader = void (*)(GLuint);
using PFN_CreateProgram = GLuint (*)();
using PFN_AttachShader = void (*)(GLuint, GLuint);
using PFN_LinkProgram = void (*)(GLuint);
using PFN_GetProgramiv = void (*)(GLuint, GLenum, GLint*);
using PFN_GetProgramInfoLog = void (*)(GLuint, GLsizei, GLsizei*, GLchar*);
using PFN_UseProgram = void (*)(GLuint);
using PFN_DeleteProgram = void (*)(GLuint);
using PFN_GetAttribLocation = GLint (*)(GLuint, const GLchar*);
using PFN_GetUniformLocation = GLint (*)(GLuint, const GLchar*);
using PFN_Uniform1i = void (*)(GLint, GLint);
using PFN_Uniform1f = void (*)(GLint, GLfloat);
using PFN_Uniform2f = void (*)(GLint, GLfloat, GLfloat);
using PFN_Uniform4f = void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);

using PFN_GenBuffers = void (*)(GLsizei, GLuint*);
using PFN_DeleteBuffers = void (*)(GLsizei, const GLuint*);
using PFN_BindBuffer = void (*)(GLenum, GLuint);
using PFN_BufferData = void (*)(GLenum, GLsizeiptr, const void*, GLenum);
using PFN_BufferSubData = void (*)(GLenum, GLintptr, GLsizeiptr, const void*);
using PFN_VertexAttribPointer = void (*)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*);
using PFN_EnableVertexAttribArray = void (*)(GLuint);
using PFN_DisableVertexAttribArray = void (*)(GLuint);
using PFN_DrawArrays = void (*)(GLenum, GLint, GLsizei);
using PFN_GetIntegerv = void (*)(GLenum, GLint*);

// Framebuffer objects. Core to ES 2.0 and to desktop GL 3.0, so these are
// required: the hardware-rendering path draws the core's picture into one.
using PFN_GenFramebuffers = void (*)(GLsizei, GLuint*);
using PFN_DeleteFramebuffers = void (*)(GLsizei, const GLuint*);
using PFN_BindFramebuffer = void (*)(GLenum, GLuint);
using PFN_FramebufferTexture2D = void (*)(GLenum, GLenum, GLenum, GLuint, GLint);
using PFN_CheckFramebufferStatus = GLenum (*)(GLenum);
using PFN_GenRenderbuffers = void (*)(GLsizei, GLuint*);
using PFN_DeleteRenderbuffers = void (*)(GLsizei, const GLuint*);
using PFN_BindRenderbuffer = void (*)(GLenum, GLuint);
using PFN_RenderbufferStorage = void (*)(GLenum, GLenum, GLsizei, GLsizei);
using PFN_FramebufferRenderbuffer = void (*)(GLenum, GLenum, GLenum, GLuint);

// GL 3.0 / ES 3.0 and later only, so these are optional -- see has_blit() and
// has_vertex_arrays().
using PFN_BlitFramebuffer =
    void (*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
using PFN_GenVertexArrays = void (*)(GLsizei, GLuint*);
using PFN_DeleteVertexArrays = void (*)(GLsizei, const GLuint*);
using PFN_BindVertexArray = void (*)(GLuint);

extern PFN_Viewport Viewport;
extern PFN_ClearColor ClearColor;
extern PFN_Clear Clear;
extern PFN_Enable Enable;
extern PFN_Disable Disable;
extern PFN_BlendFunc BlendFunc;
extern PFN_GetError GetError;
extern PFN_GetString GetString;
extern PFN_PixelStorei PixelStorei;
extern PFN_ReadPixels ReadPixels;
extern PFN_Scissor Scissor;
extern PFN_GenTextures GenTextures;
extern PFN_DeleteTextures DeleteTextures;
extern PFN_BindTexture BindTexture;
extern PFN_TexImage2D TexImage2D;
extern PFN_TexSubImage2D TexSubImage2D;
extern PFN_TexParameteri TexParameteri;
extern PFN_ActiveTexture ActiveTexture;
extern PFN_CreateShader CreateShader;
extern PFN_ShaderSource ShaderSource;
extern PFN_CompileShader CompileShader;
extern PFN_GetShaderiv GetShaderiv;
extern PFN_GetShaderInfoLog GetShaderInfoLog;
extern PFN_DeleteShader DeleteShader;
extern PFN_CreateProgram CreateProgram;
extern PFN_AttachShader AttachShader;
extern PFN_LinkProgram LinkProgram;
extern PFN_GetProgramiv GetProgramiv;
extern PFN_GetProgramInfoLog GetProgramInfoLog;
extern PFN_UseProgram UseProgram;
extern PFN_DeleteProgram DeleteProgram;
extern PFN_GetAttribLocation GetAttribLocation;
extern PFN_GetUniformLocation GetUniformLocation;
extern PFN_Uniform1i Uniform1i;
extern PFN_Uniform1f Uniform1f;
extern PFN_Uniform2f Uniform2f;
extern PFN_Uniform4f Uniform4f;
extern PFN_GenBuffers GenBuffers;
extern PFN_DeleteBuffers DeleteBuffers;
extern PFN_BindBuffer BindBuffer;
extern PFN_BufferData BufferData;
extern PFN_BufferSubData BufferSubData;
extern PFN_VertexAttribPointer VertexAttribPointer;
extern PFN_EnableVertexAttribArray EnableVertexAttribArray;
extern PFN_DisableVertexAttribArray DisableVertexAttribArray;
extern PFN_DrawArrays DrawArrays;
extern PFN_GetIntegerv GetIntegerv;
extern PFN_GenFramebuffers GenFramebuffers;
extern PFN_DeleteFramebuffers DeleteFramebuffers;
extern PFN_BindFramebuffer BindFramebuffer;
extern PFN_FramebufferTexture2D FramebufferTexture2D;
extern PFN_CheckFramebufferStatus CheckFramebufferStatus;
extern PFN_GenRenderbuffers GenRenderbuffers;
extern PFN_DeleteRenderbuffers DeleteRenderbuffers;
extern PFN_BindRenderbuffer BindRenderbuffer;
extern PFN_RenderbufferStorage RenderbufferStorage;
extern PFN_FramebufferRenderbuffer FramebufferRenderbuffer;
extern PFN_BlitFramebuffer BlitFramebuffer;
extern PFN_GenVertexArrays GenVertexArrays;
extern PFN_DeleteVertexArrays DeleteVertexArrays;
extern PFN_BindVertexArray BindVertexArray;

// Whether the optional entry points above turned up. A GL 3.3 core profile has
// both; an ES 2.0 context has neither.
bool has_blit();
bool has_vertex_arrays();

// Whether the current context is OpenGL ES (decides which GLSL preamble the
// shaders get).
bool is_gles();
// Whether it is a desktop core profile, which removed the GLSL 1.20 syntax the
// shaders are written in and needs the compatibility defines putting back.
bool is_core_profile();

// The context's version, as reported by the context itself rather than by
// whatever was asked for. Zero when it could not be determined.
void version(int& out_major, int& out_minor);
// True when the current context is at least this version.
bool at_least(int major, int minor);

// Drops every resolved entry point. Call before deleting a context, so a
// stale function pointer from the old one can never be called against the new.
void unload();

// Must be called with a current GL context. Returns false and fills `error`
// when an entry point the renderer needs is missing.
bool load(bool gles_context, std::string* error);

}  // namespace ds::gl
