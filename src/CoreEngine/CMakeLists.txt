# QuantumCache.CoreEngine
#
# STAGE 1: interface-only. See include/QuantumCache/CoreEngine/ICacheEngine.h.
# CreateCacheEngine() intentionally fails with NotImplementedStage1 — there
# is no cache or deferred-write logic in this library yet, by design.

add_library(QuantumCache.CoreEngine STATIC
    src/JournalRecordCodec.cpp
    src/CacheEngine.cpp
)

target_include_directories(QuantumCache.CoreEngine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(QuantumCache.CoreEngine
    PUBLIC
        QuantumCache::Common
        QuantumCache::PowerResilience
        QuantumCache::Storage
        QuantumCache::Logging
        QuantumCache::Configuration
)

target_compile_features(QuantumCache.CoreEngine PUBLIC cxx_std_20)

add_library(QuantumCache::CoreEngine ALIAS QuantumCache.CoreEngine)
