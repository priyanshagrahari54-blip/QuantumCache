# CMake toolchain file for cross-compiling the Windows-only parts of
# QuantumCache for 32-bit Windows (x86) using the i686-w64-mingw32
# cross-compiler, so 32-bit compilation can be verified from this Linux
# development sandbox.
#
# See toolchain-mingw-w64.cmake (the x64 sibling of this file) for the
# general rationale; this file only differs in target processor/compiler
# triple. As with the x64 toolchain, this is a cross-compilation
# convenience for development/CI, NOT a substitute for building with
# real MSVC on Windows and NOT a substitute for actually running the
# resulting x86 binary on real 32-bit (or WoW64) Windows — neither of
# which this sandbox can do. See docs/ENVIRONMENT.md and
# docs/STAGE2_ARCHITECTURE.md "32-bit Windows support" for exactly what
# has and has not been verified.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER   i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  i686-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Header-only nlohmann::json is installed as an arch:all Debian package
# under /usr/include, outside the mingw sysroot; make it visible to the
# cross-compiler without letting other Linux-native headers leak in ahead
# of the MinGW ones (see toolchain-mingw-w64.cmake's matching comment).
set(CMAKE_CXX_FLAGS_INIT "-idirafter /usr/include")
