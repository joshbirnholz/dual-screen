// See tests/apple_stub/README.md. Apple's own spelling of the GL types, which
// CoreVideo's texture cache is declared with -- and which have to agree with
// SDL's spelling of the same types, since texture.cpp sees both.
#pragma once

#include <cstdint>

typedef uint32_t GLbitfield;
typedef uint8_t GLboolean;
typedef int8_t GLbyte;
typedef float GLclampf;
typedef uint32_t GLenum;
typedef float GLfloat;
typedef int32_t GLint;
typedef int16_t GLshort;
typedef int32_t GLsizei;
typedef uint8_t GLubyte;
typedef uint32_t GLuint;
typedef uint16_t GLushort;
typedef void GLvoid;
typedef char GLchar;
typedef int32_t GLclampx;
typedef int32_t GLfixed;
typedef intptr_t GLintptr;
typedef intptr_t GLsizeiptr;
