# QuantumCache.Common
# Portable, platform-independent utility types shared by every other
# component (Result<T>, ErrorCode, Crc32). No Win32 dependency here on
# purpose, so it can be unit tested on any host, including this Linux
# sandbox, without pretending to test Windows-specific behavior.

add_library(QuantumCache.Common STATIC
    src/Crc32.cpp
)

target_include_directories(QuantumCache.Common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(QuantumCache.Common PUBLIC cxx_std_20)

add_library(QuantumCache::Common ALIAS QuantumCache.Common)
