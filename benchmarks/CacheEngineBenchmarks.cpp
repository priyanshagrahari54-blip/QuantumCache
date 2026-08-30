// Real Google Benchmark microbenchmarks for the Stage 2 cache engine.
// These measure ACTUAL operations against the real CacheEngine/
// FileBackingStore/IWriteAheadJournal implementations on this host — no
// numbers here are invented or extrapolated. They are provided to
// characterize relative cost of the different code paths (hit vs miss,
// WriteThrough vs WriteBackDeferred, flush cost) on whatever machine runs
// them; they are NOT a claim about real Windows/NTFS performance (see
// docs/ENVIRONMENT.md) and NOT a comparison against any third-party
// product (explicitly out of Stage 2 scope).
#include "QuantumCache/CoreEngine/ICacheEngine.h"
#include "QuantumCache/PowerResilience/IWriteAheadJournal.h"
#include "QuantumCache/Storage/IBackingStore.h"
#include "QuantumCache/Storage/IFile.h"
#include <benchmark/benchmark.h>
#include <filesystem>
#include <random>

using namespace QuantumCache;
namespace fs = std::filesystem;

namespace {

std::vector<std::uint8_t> Bytes(std::size_t n, std::uint8_t fill = 0x42) {
    return std::vector<std::uint8_t>(n, fill);
}

struct Rig {
    fs::path dir;
    std::shared_ptr<PowerResilience::IWriteAheadJournal> journal;
    std::shared_ptr<Storage::IBackingStore> backingStore;
    std::unique_ptr<CoreEngine::ICacheEngine> engine;

    explicit Rig(const std::string& stem, CoreEngine::CacheEngineOptions options = {}) {
        dir = fs::temp_directory_path() / ("qc_bench_" + stem);
        fs::create_directories(dir);

        std::wstring journalPath;
        for (char c : (dir / "j.dat").string()) journalPath.push_back(static_cast<wchar_t>(c));
        auto journalFile = Storage::OpenFile(journalPath, Storage::OpenMode::OpenOrCreate);
        auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
        journal = std::move(journalResult.Value());

        std::wstring storePath;
        for (char c : (dir / "s.dat").string()) storePath.push_back(static_cast<wchar_t>(c));
        auto backingResult = Storage::OpenFileBackingStore(storePath);
        backingStore = std::move(backingResult.Value());

        auto engineResult = CoreEngine::CreateCacheEngine(options, backingStore, journal);
        engine = std::move(engineResult.Value());
        engine->MarkRecoveryComplete();
    }

    ~Rig() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

static void BM_Put_WriteBackDeferred(benchmark::State& state) {
    Rig rig("put_wbd");
    auto value = Bytes(static_cast<std::size_t>(state.range(0)));
    std::uint64_t counter = 0;
    for (auto _ : state) {
        std::string key = "key" + std::to_string(counter++);
        auto result = rig.engine->Put(key, value);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Put_WriteBackDeferred)->Arg(64)->Arg(4096)->Arg(65536);

static void BM_Put_WriteThrough(benchmark::State& state) {
    CoreEngine::CacheEngineOptions options;
    options.writePolicy = CoreEngine::WritePolicyKind::WriteThrough;
    Rig rig("put_wt", options);
    auto value = Bytes(static_cast<std::size_t>(state.range(0)));
    std::uint64_t counter = 0;
    for (auto _ : state) {
        std::string key = "key" + std::to_string(counter++);
        auto result = rig.engine->Put(key, value);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Put_WriteThrough)->Arg(64)->Arg(4096);

static void BM_Get_CacheHit(benchmark::State& state) {
    Rig rig("get_hit");
    auto value = Bytes(static_cast<std::size_t>(state.range(0)));
    rig.engine->Put("hot-key", value);
    for (auto _ : state) {
        auto result = rig.engine->Get("hot-key");
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Get_CacheHit)->Arg(64)->Arg(4096)->Arg(65536);

static void BM_Get_CacheMiss_BackingStoreHit(benchmark::State& state) {
    Rig rig("get_miss");
    auto value = Bytes(static_cast<std::size_t>(state.range(0)));
    rig.backingStore->Put("cold-key", value);
    // Force eviction from cache after each Get by invalidating is not
    // representative of a real workload; instead measure the FIRST-touch
    // miss cost by using a fresh key each iteration.
    std::uint64_t counter = 0;
    for (auto _ : state) {
        std::string key = "cold-key-" + std::to_string(counter++);
        state.PauseTiming();
        rig.backingStore->Put(key, value);
        state.ResumeTiming();
        auto result = rig.engine->Get(key);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Get_CacheMiss_BackingStoreHit)->Arg(64)->Arg(4096);

static void BM_Flush_SingleEntry(benchmark::State& state) {
    Rig rig("flush");
    auto value = Bytes(static_cast<std::size_t>(state.range(0)));
    std::uint64_t counter = 0;
    for (auto _ : state) {
        state.PauseTiming();
        std::string key = "flushkey" + std::to_string(counter++);
        rig.engine->Put(key, value);
        state.ResumeTiming();
        auto result = rig.engine->Flush(key);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Flush_SingleEntry)->Arg(64)->Arg(4096);

static void BM_JournalAppend_Raw(benchmark::State& state) {
    // Baseline: raw IWriteAheadJournal::Append cost (no cache-engine
    // overhead on top), to isolate the actual fsync/FlushDurable cost from
    // the cache engine's own bookkeeping.
    fs::path dir = fs::temp_directory_path() / "qc_bench_journal_raw";
    fs::create_directories(dir);
    std::wstring journalPath;
    for (char c : (dir / "j.dat").string()) journalPath.push_back(static_cast<wchar_t>(c));
    auto journalFile = Storage::OpenFile(journalPath, Storage::OpenMode::OpenOrCreate);
    auto journalResult = PowerResilience::CreateWriteAheadJournal(std::move(journalFile.Value()));
    auto journal = std::move(journalResult.Value());

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(state.range(0)), 0x11);
    for (auto _ : state) {
        auto result = journal->Append(payload);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());

    std::error_code ec;
    fs::remove_all(dir, ec);
}
BENCHMARK(BM_JournalAppend_Raw)->Arg(64)->Arg(4096);

BENCHMARK_MAIN();
