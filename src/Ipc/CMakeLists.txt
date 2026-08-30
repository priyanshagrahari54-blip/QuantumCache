# QuantumCache.Ipc
#
# MessageCodec (wire format) is portable and unit tested on this sandbox.
# The named-pipe transport is real Win32 code (CreateNamedPipeW etc.),
# compiled only for Windows targets; it is NOT compiled or tested on
# non-Windows hosts because there is no meaningful portable substitute for
# a Windows named pipe crossing a LocalSystem/user privilege boundary.

add_library(QuantumCache.Ipc STATIC
    src/MessageCodec.cpp
)

target_include_directories(QuantumCache.Ipc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if (WIN32)
    target_sources(QuantumCache.Ipc PRIVATE src/Win32NamedPipeTransport.cpp)
    target_link_libraries(QuantumCache.Ipc PRIVATE advapi32)
endif()

target_link_libraries(QuantumCache.Ipc PUBLIC QuantumCache::Common)
target_compile_features(QuantumCache.Ipc PUBLIC cxx_std_20)

add_library(QuantumCache::Ipc ALIAS QuantumCache.Ipc)
