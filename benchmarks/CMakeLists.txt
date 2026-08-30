# QuantumCache.Benchmarks
#
# Real Google Benchmark microbenchmarks measuring actual operations against
# this host's filesystem (see file header comment in
# CacheEngineBenchmarks.cpp for what these numbers do and do not mean).
# Built only when benchmark is found and QUANTUMCACHE_BUILD_BENCHMARKS is
# enabled, so its absence never breaks the main build/test flow.

find_package(benchmark QUIET)

if (benchmark_FOUND)
    add_executable(QuantumCache.Benchmarks
        CacheEngineBenchmarks.cpp
    )
    target_link_libraries(QuantumCache.Benchmarks
        PRIVATE
            QuantumCache::Common
            QuantumCache::Storage
            QuantumCache::PowerResilience
            QuantumCache::CoreEngine
            benchmark::benchmark
    )
    target_compile_features(QuantumCache.Benchmarks PRIVATE cxx_std_20)
else()
    message(STATUS
        "QuantumCache.Benchmarks: google/benchmark not found; skipping. "
        "Install libbenchmark-dev (or vcpkg's 'benchmark' on Windows) to build it.")
endif()
