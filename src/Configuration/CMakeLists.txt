# QuantumCache.Configuration
# Real JSON-backed load/save/validate for the Stage 1 config schema only
# (see AppConfig.h) — no cache-engine-specific settings yet.

add_library(QuantumCache.Configuration STATIC
    src/JsonConfigStore.cpp
)

target_include_directories(QuantumCache.Configuration
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(QuantumCache.Configuration
    PUBLIC
        QuantumCache::Common
    PRIVATE
        nlohmann_json::nlohmann_json
)

target_compile_features(QuantumCache.Configuration PUBLIC cxx_std_20)

add_library(QuantumCache::Configuration ALIAS QuantumCache.Configuration)
