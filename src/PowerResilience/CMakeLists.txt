# QuantumCache.PowerResilience
#
# This is the component the user explicitly required: interfaces AND real
# working implementations for detecting an unclean shutdown (power cut)
# and replaying a write-ahead journal to a consistent point. It does NOT
# implement the cache engine or deferred-write engine — the journal is
# generic/opaque-payload infrastructure that a future engine will build on.

add_library(QuantumCache.PowerResilience STATIC
    src/SessionMarker.cpp
    src/WriteAheadJournal.cpp
    src/RecoveryManager.cpp
)

target_include_directories(QuantumCache.PowerResilience
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(QuantumCache.PowerResilience
    PUBLIC
        QuantumCache::Common
        QuantumCache::Storage
)

target_compile_features(QuantumCache.PowerResilience PUBLIC cxx_std_20)

add_library(QuantumCache::PowerResilience ALIAS QuantumCache.PowerResilience)
