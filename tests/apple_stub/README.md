# tests/apple_stub

The slice of Apple's C frameworks that the Apple-only halves of
`src/video/videotoolbox_decoder.cpp` and `src/render/texture.cpp` name, declared
the way the SDK declares them. This is enough for a machine with no Apple SDK
to compile those halves.

`apple_typecheck` in `tests/CMakeLists.txt` compiles both files against these
headers with `DS_TYPECHECK_APPLE` defined, which the sources treat the same as
`__APPLE__`. The halves are compiled twice, once as iOS/tvOS and once as
macOS. Otherwise nothing but a person's Xcode would ever compile them, and it
would report their mistakes one build at a time.

What this proves is that the code agrees with these declarations. It does not
prove the declarations agree with Apple. They were copied from the SDK
headers, so a new call needs its real declaration copied in here as well, and
that copy is what has to be right.

`OpenGLES/gltypes.h` is included on purpose. CoreVideo's texture cache
declares its GL types through Apple's own header, and `texture.cpp` has
already seen SDL's copies of the same types by then. The two have to agree.
