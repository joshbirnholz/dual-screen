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

// Whether the current context is OpenGL ES (decides which GLSL preamble the
// shaders get).
bool is_gles();

// Must be called with a current GL context. Returns false and fills `error`
// when an entry point the renderer needs is missing.
bool load(bool gles_context, std::string* error);

}  // namespace ds::gl
