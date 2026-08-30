cmake_minimum_required(VERSION 3.24)

project(QuantumCache
    VERSION 0.1.0
    DESCRIPTION "QuantumCache backend (Stage 1: project foundation)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Ensures Debug/Release .lib and .exe land in predictable folders when
# using the (multi-config) "Visual Studio 17 2022" generator on real
# Windows, matching what a hand-authored VS solution would produce.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/bin/Debug)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_DEBUG   ${CMAKE_BINARY_DIR}/lib/Debug)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/lib/Release)

if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    # Unicode Win32 API surface (wide-character CreateFileW etc.) rather
    # than the legacy ANSI variants, matching how the code is written.
    add_compile_definitions(UNICODE _UNICODE)
else()
    add_compile_options(-Wall -Wextra)
    if (WIN32) # MinGW-w64 cross-compilation targeting Windows
        add_compile_definitions(UNICODE _UNICODE)
    endif()
endif()

option(QUANTUMCACHE_BUILD_TESTS "Build the GoogleTest-based test suite" ON)
option(QUANTUMCACHE_BUILD_BENCHMARKS "Build the Google Benchmark microbenchmarks" OFF)

include(cmake/Dependencies.cmake)

add_subdirectory(src/Common)
add_subdirectory(src/Logging)
add_subdirectory(src/Storage)
add_subdirectory(src/PowerResilience)
add_subdirectory(src/Configuration)
add_subdirectory(src/CoreEngine)
add_subdirectory(src/Ipc)
add_subdirectory(src/Service)

if (QUANTUMCACHE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if (QUANTUMCACHE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

message(STATUS "----------------------------------------------------------------")
message(STATUS "QuantumCache Stage 1 configuration summary")
message(STATUS "  CMAKE_SYSTEM_NAME       : ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Target is Windows (WIN32): ${WIN32}")
message(STATUS "  Compiler                : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  QuantumCache.Service    : ${WIN32}  (Win32-only component)")
message(STATUS "  Tests enabled           : ${QUANTUMCACHE_BUILD_TESTS}")
message(STATUS "  See docs/ENVIRONMENT.md for what has/has not been verified.")
message(STATUS "----------------------------------------------------------------")
