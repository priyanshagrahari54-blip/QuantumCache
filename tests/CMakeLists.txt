# QuantumCache.Tests
#
# By default (native Linux/GCC build) this target builds and runs
# directly on the host, covering the platform-independent logic: Common,
# the PowerResilience state machine (against PortableFile/PortableVolume),
# Configuration, Logging, the Ipc wire-format codec, and CoreEngine.
# Win32-specific sources (Win32File, Win32Volume, Win32NamedPipeTransport,
# IServiceHost, ServiceInstaller) are excluded from the LIBRARIES this
# target links against on non-Windows hosts (see each component's own
# CMakeLists.txt `if (WIN32)` guards) — this target itself is platform-
# generic and does not special-case WIN32 beyond the one conditional
# source file below.
#
# Stage 2 hardening update: this same target CAN ALSO be configured with
# a MinGW-w64 cross-compile toolchain (see cmake/toolchain-mingw-w64.cmake
# / toolchain-mingw-w64-x86.cmake) plus a MinGW-cross-compiled GoogleTest,
# producing a real Windows PE test executable. That executable is NOT
# runnable directly in this Linux sandbox via ctest/gtest_discover_tests
# below (which invokes the built binary natively at configure time to
# enumerate tests — this fails immediately against a foreign-architecture
# PE binary, which is why `-DQUANTUMCACHE_BUILD_TESTS=ON` cross-compiles
# historically built the Service exe but not this target). It CAN,
# however, be executed under Wine (`wine QuantumCache.Tests.exe`), which
# was done as part of Stage 2 hardening to get real (if emulated, not
# real-hardware) Windows-runtime execution of the ENTIRE test suite,
# including the Win32File-backed code paths this comment used to say were
# "excluded... and NOT claimed to be tested" — see
# docs/ENVIRONMENT.md "Windows runtime testing" for exactly what Wine
# execution does and does not prove, and why it is still not a substitute
# for real Windows/NTFS validation.

find_package(Threads REQUIRED)

add_executable(QuantumCache.Tests
    CommonTests.cpp
    StorageFileTests.cpp
    FileBackingStoreTests.cpp
    PowerResilienceTests.cpp
    ConfigurationTests.cpp
    LoggingTests.cpp
    IpcCodecTests.cpp
    CacheEngineTests.cpp
    CacheEngineCoreTests.cpp
    CacheEngineReadWriteRaceTests.cpp
    CacheEngineShutdownRaceTests.cpp
    CacheEngineBackpressureTests.cpp
    CacheEngineCompactionAuditTests.cpp
    CrashPointMatrixTests.cpp
    RepeatedRecoveryTests.cpp
    IpcFuzzTests.cpp
    PerformanceResourceTests.cpp
)

if (WIN32)
    # Real Win32 security-descriptor API test (ConvertStringSecurityDescriptorToSecurityDescriptorW
    # etc.) for the named-pipe SDDL string — only meaningful/compilable
    # against real <windows.h>/<sddl.h>, hence WIN32-only. See
    # Win32NamedPipeSecurityTests.cpp's file header for the full
    # rationale and docs/ENVIRONMENT.md for how this was actually
    # executed (via Wine) during Stage 2 hardening.
    target_sources(QuantumCache.Tests PRIVATE Win32NamedPipeSecurityTests.cpp)
    target_link_libraries(QuantumCache.Tests PRIVATE advapi32)
endif()

target_link_libraries(QuantumCache.Tests
    PRIVATE
        QuantumCache::Common
        QuantumCache::Storage
        QuantumCache::PowerResilience
        QuantumCache::Configuration
        QuantumCache::Logging
        QuantumCache::Ipc
        QuantumCache::CoreEngine
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_compile_features(QuantumCache.Tests PRIVATE cxx_std_20)

include(GoogleTest)
gtest_discover_tests(QuantumCache.Tests)
