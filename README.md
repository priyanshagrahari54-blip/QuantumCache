# QuantumCache — Stage 2: Real Cache Engine

Stage 1 built the project foundation and the power-loss detection/recovery
infrastructure. **Stage 2 adds the first genuinely functional caching
subsystem**: a real, working, sharded, LRU, journal-backed RAM cache in
front of a real file-based backing store, fully integrated with Stage 1's
`PowerResilience` recovery machinery. See
[`docs/STAGE2_ARCHITECTURE.md`](docs/STAGE2_ARCHITECTURE.md) for the full
design, state machine, and durability guarantees, and
[`docs/ENVIRONMENT.md`](docs/ENVIRONMENT.md) for exactly what has and has
not been verified in the sandbox that produced this code.

**Short version**: everything platform-independent (the cache engine
itself, the backing store, the journal record codec, IPC codec,
configuration) is built with native Linux g++ and tested with GoogleTest
— 114/114 tests passing, plus the full suite re-run under ThreadSanitizer
with zero data races. Everything Win32-specific (the Service, the Win32
file/volume/pipe backends) compiles and links via MinGW-w64 into genuine
PE32+ binaries, verified with `file`/`objdump`, but has never executed
against a real Windows kernel/SCM — that requires an actual Windows
machine and is explicitly not claimed here.

## Layout

```
QuantumCache/
├── CMakeLists.txt              Top-level CMake project (backend only)
├── CMakePresets.json            Presets for MSVC/Windows, MinGW cross-compile, and Linux native tests
├── QuantumCache.sln             Visual Studio solution wrapping the WinUI 3 GUI project
├── cmake/
│   ├── Dependencies.cmake       Third-party dependency resolution (nlohmann_json, GoogleTest, benchmark)
│   └── toolchain-mingw-w64.cmake  Cross-compilation toolchain file (dev/CI convenience only)
├── docs/
│   ├── ENVIRONMENT.md           What has/has not been verified, and why (read this first)
│   └── STAGE2_ARCHITECTURE.md   Cache engine design, state machine, durability guarantees (read this second)
├── src/
│   ├── Common/                  Result<T>, ErrorCode, Crc32 — portable, no Win32 dependency
│   ├── Logging/                 Leveled logger + file/memory sinks — portable
│   ├── Storage/                 IFile / IVolume / IBackingStore — Win32 + portable backends
│   ├── PowerResilience/         Session marker, write-ahead journal, recovery manager
│   ├── Configuration/           JSON-backed AppConfig load/save/validate (Stage 1 + Stage 2 fields)
│   ├── CoreEngine/               REAL cache engine (Stage 2): sharded LRU, journaled, crash-recoverable
│   ├── Ipc/                     Named-pipe wire protocol incl. Stage 2 cache-management messages
│   └── Service/                 Windows Service wiring recovery -> cache engine -> IPC, in the required order
├── gui/
│   └── QuantumCacheGui/         WinUI 3 / C++/WinRT desktop GUI (separate MSBuild project, unchanged from Stage 1)
├── benchmarks/                  Real Google Benchmark microbenchmarks for the cache engine
└── tests/                       110 GoogleTest cases across every portable component
```

## What Stage 2 actually implements

- **Real `ICacheEngine`**: sharded (power-of-two shard count), LRU
  eviction over `Clean` entries only, real memory/entry-count capacity
  accounting and enforcement, real concurrent access (see "Concurrency
  model" in `docs/STAGE2_ARCHITECTURE.md`), explicit `ErrorCode`s on every
  failure path — never a silent no-op.
- **Real backing-store integration**: `Storage::IBackingStore` /
  `FileBackingStore`, built on the *existing* `IFile` abstraction, with
  its own CRC-32-checked record framing and torn-tail handling. Cache
  metadata (dirty state, LRU position, versions) is never stored in the
  backing store; the backing store never sees anything but key/value
  bytes. This is a real RAM cache in front of a real file — explicitly
  **not** an SSD cache and **not** a kernel-mode driver (out of Stage 2
  scope; see `docs/STAGE2_ARCHITECTURE.md` "Scope recap").
- **Real read path**: lookup → hit returns cached data / miss reads the
  backing store → validates the result → inserts into cache → returns
  data, with real hit/miss counters incremented only at those exact
  points.
- **Real write path** with an explicit durability boundary per
  `WritePolicyKind` (`WriteBackDeferred` durably-journaled-only by
  default, or `WriteThrough` journaled-and-backing-store-confirmed) —
  `Put()` never returns success before the applicable boundary is
  actually reached.
- **Real dirty-data/deferred-write state model**:
  `EntryDirtyState::{Clean, Dirty, FlushInProgress}`, with dirty data
  *never* silently evicted or discarded, and safe `Flush`/`FlushAll`
  implementations that journal `FlushIntent`/`FlushComplete` around the
  actual backing-store write.
- **Full power-loss integration with Stage 1**: `ICacheEngine` reuses
  Stage 1's `IWriteAheadJournal` and `IRecoveryManager` directly — no
  second, incompatible recovery mechanism. Recovery is structurally
  required before any data-plane access (`Get`/`Put`/etc. all return
  `ErrorCode::RecoveryNotComplete` until `MarkRecoveryComplete()` has been
  called by the Service, which itself only happens after
  `IRecoveryManager::InitializeAndRecover()` succeeds). A failed recovery
  leaves the engine permanently not-ready rather than serving
  possibly-inconsistent data.
- **Versioned journal record schema** for cache semantics
  (`Upsert`/`FlushIntent`/`FlushComplete`/`Invalidate`), decoded with its
  own independent format-version check and length sanity bounds, replayed
  idempotently.
- **Extended `AppConfig`** with the cache settings Stage 2 actually needs
  (capacity, shard count, eviction/write/flush policy, enable/disable),
  every one validated with a real enforced constraint, with a tested
  migration path for old Stage 1 config files.
- **Extended IPC protocol** with real cache-status/management messages
  (`GetCacheStatistics`, `FlushAll`, `InvalidateKey`) — no messages for
  functionality that doesn't exist (no remote Get/Put, no SSD-tier
  controls).
- **Windows Service** rewritten to perform the exact required sequence:
  recovery initialization → journal replay (only if needed) → recovery
  completion → cache engine initialization → IPC server starts → only
  then does the Service report `SERVICE_RUNNING`. Shutdown: stop accepting
  unsafe writes → flush → persist (conditionally truncate the journal only
  if everything is Clean) → mark clean shutdown → stop.

See [`docs/STAGE2_ARCHITECTURE.md`](docs/STAGE2_ARCHITECTURE.md) for
diagrams of every sequence above, the full journal record wire format, the
concurrency/locking model, and a section-by-section list of known,
explicitly-stated limitations (e.g. per-shard rather than global capacity
enforcement).

## Explicitly NOT in Stage 2 (per scope)

- No SSD/L2 cache tier.
- No kernel-mode driver functionality.
- No predictive/adaptive caching (eviction is plain LRU only).
- No benchmarking or comparison against any third-party product.
- No GUI additions to demonstrate functionality that doesn't exist.

## Building

### Backend (Core Engine, Storage, Configuration, PowerResilience,
Logging, Ipc, Service, Tests, Benchmarks) — CMake

```bash
# On real Windows, with Visual Studio 2022 + Windows SDK installed:
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64
ctest --preset windows-msvc-x64

# Cross-compiling the Win32 pieces from Linux (dev/CI convenience only —
# see docs/ENVIRONMENT.md for why this is not a substitute for the above):
cmake --preset linux-mingw-cross
cmake --build --preset linux-mingw-cross

# Running the portable unit test suite on Linux (this is what was
# actually executed while building this stage — 114/114 passing):
cmake --preset linux-native-tests
cmake --build --preset linux-native-tests
ctest --preset linux-native-tests

# Optional: real Google Benchmark microbenchmarks (requires libbenchmark-dev
# or vcpkg's "benchmark" package):
cmake -S . -B out/build/linux-native-tests -DQUANTUMCACHE_BUILD_BENCHMARKS=ON
cmake --build out/build/linux-native-tests --target QuantumCache.Benchmarks
./out/build/linux-native-tests/bin/Debug/QuantumCache.Benchmarks
```

All three CMake presets are real and have been exercised for Stage 2:
`linux-native-tests` built and ran **114/114 passing** GoogleTest cases
(including the full Stage 2 cache-engine, journal-replay, crash-recovery,
and concurrency suites — the latter additionally verified with
ThreadSanitizer reporting zero races); `linux-mingw-cross` produced a
genuine PE32+ `QuantumCacheService.exe` — now including the full Stage 2
cache engine linked in — that imports real `ADVAPI32.dll` Service Control
Manager functions (`StartServiceCtrlDispatcherW`,
`RegisterServiceCtrlHandlerExW`), verified with `file` and `objdump`.
`windows-msvc-x64` has **not** been exercised because no Windows/MSVC
environment was available.

### GUI (WinUI 3 / C++/WinRT) — MSBuild, separate from CMake

Unchanged from Stage 1: open `QuantumCache.sln` in Visual Studio 2022
(17.8+) with the "Desktop development with C++" workload and the "Windows
App SDK C++" component installed. The GUI still only talks to the backend
Service via the IPC contract (now including Stage 2's cache-management
messages) and has not been extended with any new UI in Stage 2, per scope
("do not build a fake GUI to demonstrate unfinished features"). See
`docs/ENVIRONMENT.md` for why this remains a separate build system, and
note again: **this project has not been built or run by any tool in the
environment that produced it.**

## Test status (honest accounting)

| Suite | Where it runs | Status |
|---|---|---|
| `QuantumCache.Tests` — 114 cases (Common, PowerResilience, Configuration, Logging, Ipc codec, **CoreEngine/CacheEngine, JournalRecordCodec, isolated Stage 2A cache-core suite, Stage 2B periodic-flush suite, deterministic + stress read-miss/write race regression suite**) | Native Linux g++ + GoogleTest | **Passing, verified in this repo's sandbox** |
| Same suite's concurrency tests, additionally | Native Linux g++ + **ThreadSanitizer** | **Zero data races reported** |
| `QuantumCache.Benchmarks` | Native Linux g++ + Google Benchmark | **Real measurements captured in this sandbox** (see `docs/STAGE2_ARCHITECTURE.md`); not a claim about Windows/NTFS performance |
| Win32 backends (`Win32File`, `Win32Volume`, `Win32NamedPipeTransport`, `IServiceHost`, `ServiceInstaller`) + the full Stage 2 Service wiring | N/A | **Compiles/links via MinGW-w64 only**, producing a genuine PE32+ executable. No runtime testing has been done and none is claimed. |
| `QuantumCacheGui` (WinUI 3) | N/A | **Unbuilt.** No compiler for this exists in the current sandbox. |
