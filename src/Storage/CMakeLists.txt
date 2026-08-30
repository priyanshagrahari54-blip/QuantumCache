# QuantumCache.Storage
# Volume/File durability primitives. Deliberately does NOT contain any
# cache engine or deferred-write logic in Stage 1 — only the abstractions
# (IFile, IVolume) needed by PowerResilience/Configuration, plus a real
# Win32 backend and a portable backend used strictly for non-Windows unit
# testing (see docs/ENVIRONMENT.md).

add_library(QuantumCache.Storage STATIC
    include/QuantumCache/Storage/IFile.h
    include/QuantumCache/Storage/IVolume.h
    include/QuantumCache/Storage/IBackingStore.h
    src/FileBackingStore.cpp
)

target_include_directories(QuantumCache.Storage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if (WIN32)
    target_sources(QuantumCache.Storage PRIVATE
        src/Win32File.cpp
        src/Win32Volume.cpp
    )
else()
    target_sources(QuantumCache.Storage PRIVATE
        src/PortableFile.cpp
        src/PortableVolume.cpp
    )
endif()

target_link_libraries(QuantumCache.Storage PUBLIC QuantumCache::Common)
target_compile_features(QuantumCache.Storage PUBLIC cxx_std_20)

add_library(QuantumCache::Storage ALIAS QuantumCache.Storage)
