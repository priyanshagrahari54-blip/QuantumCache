# Third-party dependency resolution.
#
# nlohmann_json: header-only JSON library used by QuantumCache.Configuration.
#   - On this Linux development sandbox it is resolved via the
#     `nlohmann-json3-dev` system package (installed via apt), which ships
#     a CMake package config (find_package works out of the box).
#   - On Windows, the same package is available via vcpkg
#     (`vcpkg install nlohmann-json`) or NuGet; CMAKE_TOOLCHAIN_FILE should
#     point at vcpkg's toolchain file when configuring on Windows so
#     find_package resolves identically. This is a standard, widely used
#     dependency — not something invented for this project.
#
# GoogleTest: used only by tests/ (see QUANTUMCACHE_BUILD_TESTS). Resolved
#   the same way (system package here; vcpkg on Windows).

find_package(nlohmann_json 3.10 REQUIRED)

if (QUANTUMCACHE_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif()
