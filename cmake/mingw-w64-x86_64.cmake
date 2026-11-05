# Cross-compiling Dualscreen for 64-bit Windows with MinGW-w64, from Linux.
# Used by tools/build-windows.sh, locally and in CI alike. DUALSCREEN_WINDOWS_DEPS
# is the prefix that script puts SDL2, zlib, zstd and FFmpeg into.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
# The -posix flavour: std::thread and std::mutex need winpthreads, which the
# -win32 flavour of Debian/Ubuntu's MinGW leaves out.
find_program(CMAKE_C_COMPILER NAMES ${TOOLCHAIN_PREFIX}-gcc-posix ${TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++-posix ${TOOLCHAIN_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${TOOLCHAIN_PREFIX}-windres REQUIRED)

# Kept in the cache, so a reconfigure started by the build itself -- with no
# such variable in its environment -- still finds them.
if(DEFINED ENV{DUALSCREEN_WINDOWS_DEPS})
    set(DUALSCREEN_WINDOWS_DEPS "$ENV{DUALSCREEN_WINDOWS_DEPS}" CACHE PATH "Windows dependencies' prefix" FORCE)
endif()
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX} ${DUALSCREEN_WINDOWS_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Tests built for Windows run under Wine on the Linux machine that built them.
find_program(DUALSCREEN_WINE NAMES wine wine64)
if(DUALSCREEN_WINE)
    set(CMAKE_CROSSCOMPILING_EMULATOR ${DUALSCREEN_WINE})
endif()

# The C++ runtime linked into every program rather than shipped as DLLs, as
# DUALSCREEN_PORTABLE does on Linux. Only winpthread stays a DLL, beside the
# programs (tools/build-windows.sh copies it).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
