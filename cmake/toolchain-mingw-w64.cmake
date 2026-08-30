# CMake toolchain file for cross-compiling the Windows-only parts of
# QuantumCache (Storage's Win32 backend, PowerResilience atop it, Ipc's
# named pipe transport, and the Service component/executable) using the
# MinGW-w64 cross-compiler, so they can be verified to actually produce
# real Windows PE binaries from this Linux development sandbox.
#
# This is a cross-compilation convenience for development/CI on
# non-Windows machines. The officially supported, fully verified way to
# build this project for production is the MSVC / "Visual Studio 17 2022"
# CMake generator on real Windows with the Windows SDK. See
# docs/ENVIRONMENT.md for exactly what has and has not been verified with
# each toolchain.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Header-only nlohmann::json is installed as an arch:all Debian package
# under /usr/include, outside the mingw sysroot; make it visible to the
# cross-compiler without letting other Linux-native headers leak in ahead
# of the MinGW ones (hence -idirafter, applied per-target below rather
# than polluting CMAKE_CXX_FLAGS globally is preferred, but this toolchain
# file is the simplest place to guarantee it applies to every target).
set(CMAKE_CXX_FLAGS_INIT "-idirafter /usr/include")
