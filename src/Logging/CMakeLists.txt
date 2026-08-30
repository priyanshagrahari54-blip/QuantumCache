# QuantumCache.Logging
# Portable leveled logging used by every other component. No Win32
# dependency (real Windows builds still use this exact code — file I/O via
# std::ofstream works identically under MinGW/MSVC on Windows). ETW /
# Windows Event Log integration is explicitly deferred past Stage 1.

add_library(QuantumCache.Logging STATIC
    src/Logger.cpp
    src/FileLogSink.cpp
)

target_include_directories(QuantumCache.Logging
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(QuantumCache.Logging PUBLIC QuantumCache::Common)
target_compile_features(QuantumCache.Logging PUBLIC cxx_std_20)

add_library(QuantumCache::Logging ALIAS QuantumCache.Logging)
