# QuantumCache.Service
#
# Windows Service Control Manager integration (IServiceHost,
# ServiceInstaller) plus the QuantumCacheService.exe entry point. Win32
# only — there is no meaningful portable substitute for a Windows Service,
# so this whole component is skipped (not stubbed) on non-Windows builds.

if (WIN32)
    add_library(QuantumCache.Service STATIC
        src/Win32ServiceHost.cpp
        src/Win32ServiceInstaller.cpp
    )

    target_include_directories(QuantumCache.Service
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )

    target_link_libraries(QuantumCache.Service
        PUBLIC
            QuantumCache::Common
        PRIVATE
            advapi32
    )

    target_compile_features(QuantumCache.Service PUBLIC cxx_std_20)

    add_library(QuantumCache::Service ALIAS QuantumCache.Service)

    add_executable(QuantumCacheService src/main_service.cpp)
    if (MINGW)
        # wmain() (wide-character console entry point) requires -municode
        # so MinGW's CRT selects the wmain-compatible startup object
        # instead of defaulting to the WinMain (GUI subsystem) entry stub.
        # Not needed under real MSVC, which resolves wmain automatically.
        target_link_options(QuantumCacheService PRIVATE -municode -mconsole)
    endif()
    target_link_libraries(QuantumCacheService
        PRIVATE
            QuantumCache::Service
            QuantumCache::PowerResilience
            QuantumCache::Storage
            QuantumCache::Configuration
            QuantumCache::Logging
            QuantumCache::CoreEngine
            QuantumCache::Ipc
            QuantumCache::Common
            advapi32
    )
    target_compile_features(QuantumCacheService PRIVATE cxx_std_20)
else()
    message(STATUS
        "QuantumCache.Service: skipped on non-Windows build. "
        "A Windows Service has no meaningful portable equivalent, so this "
        "component is intentionally absent here rather than stubbed out. "
        "See docs/ENVIRONMENT.md.")
endif()
