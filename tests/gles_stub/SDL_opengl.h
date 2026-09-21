#pragma once

// Stands in for SDL's SDL_opengl.h, to reproduce on Linux what a device sees.
//
// On iOS, tvOS and the Switch the GL that exists is OpenGL ES, and the desktop
// header is either empty or absent. Everything that is core in desktop GL and
// not in ES -- GL_RED, GL_RG, GL_CONTEXT_PROFILE_MASK, GL_UNPACK_ROW_LENGTH,
// GL_BGRA -- then stops resolving, and each one is a compile error that no
// build in this repository can see, because every build here has desktop GL.
//
// Putting the ES2 headers behind the name the sources already include makes
// the ordinary host compiler produce the device's answer. It is the same trick
// as tests/switch_stub, and for the same reason: the build that breaks is the
// one nothing here can run.
//
// ES2 rather than ES3 deliberately. ES3 defines GL_RED and GL_RG, so testing
// against it would hide exactly the failure this is here to catch, and the
// floor these sources are written to is ES 2.0 -- see gl_loader.hpp.
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
