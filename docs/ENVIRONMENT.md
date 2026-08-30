# Build/Test Environment — Honesty Statement

This file states exactly what has and has not been verified in the environment that
produced this codebase, so nobody downstream mistakes "compiles" for "works on Windows."
It was written for Stage 1 and has been updated in place for Stage 2 (the real cache
engine); see `docs/STAGE2_ARCHITECTURE.md` for everything Stage-2-specific, including
its own detailed "what was actually tested" table and benchmark results.

## Sandbox used to produce Stage 1 / Stage 2

- Host OS: Debian GNU/Linux 13 (trixie), x86_64, no hardware virtualization exposed
  (`/dev/kvm` absent, no vmx/svm CPU flags) — a real Windows VM could not be booted here.
- No Microsoft Visual Studio, MSVC (`cl.exe`), MSBuild, Windows SDK, or Windows App SDK /
  WinUI 3 / C++/WinRT tooling is installed or installable in this sandbox (these are
  proprietary and only ship for Windows / via NuGet inside a Windows+VS install).
- **Stage 2 hardening update**: Wine 10.0 (Debian package, both `wine32:i386` and the
  64-bit loader) IS installed and used — see "Windows RUNTIME testing via Wine" below.
  This is a meaningful upgrade over Stage 1's "no Wine" state, but Wine is a
  reimplementation of the Win32 API on Linux, NOT a real Windows kernel — it does not
  replace real Windows/NTFS validation, and this document is explicit below about
  exactly which claims Wine execution does and does not support.

## What WAS actually verified here (real, reproducible)

- **MinGW-w64 cross-compiler** (`x86_64-w64-mingw32-g++`, GCC 14, POSIX threading variant)
  is installed and produces genuine **PE32+ Windows executables** (verified with `file`).
- All Win32-only source in this repo (Service, IPC named pipes, Win32 file system, journal)
  **compiles and links successfully** against MinGW-w64's Win32 headers/import libraries
  (`windows.h`, `winsvc.h`, etc.) into real `.exe`/`.lib` artifacts.
- **CMake 3.31 + Ninja** are installed; the CMake build in this repo has been run and
  produces the described artifacts, including a cross-compiled `QuantumCacheService.exe`.
- **Portable (non-Win32) logic** — `Result<T>`, CRC32, the power-resilience state
  machine, the write-ahead journal encode/replay logic, config load/save/validation,
  the file-backed logger — is compiled with the **native Linux g++ 14** and executed
  under **GoogleTest** on this host. Those test executions are real and their pass/fail
  output is real.
- **Stage 2 addition**: the real cache engine (`CoreEngine::CacheEngine`), the real
  file-based backing store (`Storage::FileBackingStore`), and the cache-aware journal
  record codec are all portable (no Win32-only code) and are compiled with the same
  native Linux g++ 14 / GoogleTest setup — 114/114 tests passing as of Stage 2B (including a
  dedicated audit and fix of a read-miss/write race, with deterministic and stress regression
  tests — see docs/STAGE2_ARCHITECTURE.md "Read-miss/write race"), including
  crash/recovery simulation and concurrent-access tests. The full suite was additionally
  run under **ThreadSanitizer** with zero data races reported. See
  `docs/STAGE2_ARCHITECTURE.md` for the full test inventory and real (not invented)
  Google Benchmark performance numbers measured in this sandbox.

## Windows RUNTIME testing via Wine (Stage 2 hardening — new capability, read carefully)

Stage 1 stated flatly "no Wine" and "untested beyond compiling successfully." That is
no longer accurate: Wine 10.0 was installed in this sandbox during Stage 2 hardening,
and the following was **actually executed** (not merely compiled) via `wine
<exe>.exe`, with full command transcripts preserved in this session's history:

- **The entire GoogleTest suite, cross-compiled as a real Windows PE binary and run
  under Wine, for BOTH architectures**: `QuantumCache.Tests.exe` built with
  `x86_64-w64-mingw32-g++` (PE32+, confirmed via `file`) and separately with
  `i686-w64-mingw32-g++` (PE32, Intel i386, confirmed via `file`), each statically
  linked (`-static-libgcc -static-libstdc++ -static -lwinpthread`) against a
  MinGW-cross-compiled GoogleTest (built from the same `googletest` source package
  used for the native Linux test build). Both executables were run under
  `wine QuantumCache.Tests.exe --gtest_brief=1` and produced **164/164 tests passing,
  exit code 0**, repeated 3 times each with identical results (no flakiness observed).
  This is the first time in this project's history that the Win32File-backed code
  paths (as opposed to PortableFile, the Linux-only reference implementation) were
  ever actually exercised by any test, on either architecture.
  - **This IS real evidence that**: the Win32 file I/O paths (CreateFileW, WriteFile,
    ReadFile, SetFilePointerEx, SetEndOfFile, FlushFileBuffers), the SessionMarker/
    WriteAheadJournal/FileBackingStore logic running against them, and all CacheEngine
    concurrency/durability logic function correctly against Wine's Win32 API
    reimplementation, on both 32-bit and 64-bit code, including every regression test
    added during Stage 2 hardening (the shutdown/Put() race fix, the corrupted-
    huge-length-field fixes, the SessionMarker CRC fixes, etc).
  - **This is NOT evidence that**: real Windows NTFS provides the same guarantees Wine
    happens to provide on top of a Linux ext4/overlay filesystem (Wine's FlushFileBuffers
    ultimately calls Linux `fsync()`/`fdatasync()` on the host, which is a reasonable
    approximation but not proof of real NTFS/real-disk-controller behavior); that the
    real Windows Service Control Manager integration works (Wine has no real SCM — see
    below); that named-pipe IPC between two real Windows processes/privilege contexts
    works (Wine's named pipe implementation exists but was not exercised end-to-end
    here); or that any of this holds on real physical hardware experiencing an actual
    power interruption. Wine execution catches a large, real class of bugs (anything
    that would crash, hang, or misbehave due to actual Win32 API usage) that pure
    cross-compilation cannot, but it is a strictly weaker guarantee than real Windows.
  - **One real, Wine-execution-only bug class was actually found and fixed this way**:
    several newly-added tests initially opened a second `Storage::IFile`/
    `IBackingStore` handle to the same on-disk path while an earlier handle from the
    same test was still alive (not yet destructed/closed). This was silently tolerated
    by `PortableFile` (the permissive POSIX-backed reference implementation used for
    all native-Linux test execution) but failed with a real Windows sharing violation
    under `Win32File` (which deliberately opens with only `FILE_SHARE_READ`, never
    `FILE_SHARE_WRITE` — correct, intentional single-writer semantics). This was a
    **test-authoring bug, not a product bug** (every production call site already
    scopes/closes handles correctly before reopening — see `CacheEngineTests.cpp`'s
    `BuildRig` pattern), but it could ONLY have been discovered by actually running
    Win32File under something that enforces real Windows file-sharing semantics —
    compiling against MinGW's headers does not check this at all. Fixed in
    `StorageFileTests.cpp`, `FileBackingStoreTests.cpp`, `PowerResilienceTests.cpp`,
    and `CacheEngineShutdownRaceTests.cpp` by properly scoping handles.
- **The real named-pipe security descriptor SDDL string** (`D:(A;;GRGW;;;AU)`, from
  `Win32NamedPipeTransport.cpp`) was fed through the REAL Win32 security APIs
  (`ConvertStringSecurityDescriptorToSecurityDescriptorW`, `IsValidSecurityDescriptor`,
  `GetSecurityDescriptorDacl`, `ConvertSecurityDescriptorToStringSecurityDescriptorW`)
  under Wine, both as a standalone throwaway program and as permanent GoogleTest
  regression coverage (`tests/Win32NamedPipeSecurityTests.cpp`, compiled only on
  Windows targets, 4 tests, all passing under Wine on both x64 and x86). This confirms
  the SDDL string is well-formed, parses to a real (non-NULL) DACL restricted to
  Authenticated Users with Generic Read/Write (not Everyone, not Generic-All/full
  control). It does NOT confirm real-world enforcement behavior (e.g. that a genuinely
  lower-privilege Windows process is actually denied access) — that requires two
  distinct real Windows user accounts/sessions, which cannot be constructed here.
- **`QuantumCacheService.exe` (statically linked, real x64 PE32+) was launched directly
  under Wine** (`wine QuantumCacheService.exe`, not through any service infrastructure).
  It exited promptly (exit code 1, no crash, no hang, no unhandled exception) because
  Wine has no real Service Control Manager for `StartServiceCtrlDispatcher` to attach
  to — this is the EXPECTED and CORRECT failure mode for a real Windows service binary
  launched outside the SCM, and confirms the binary itself is not immediately broken,
  but it is explicitly **not** a test of actual service start/stop/PRESHUTDOWN
  behavior, which requires a real SCM (see below).

## What was NOT and could NOT be verified here (be skeptical of this code until you test it)

- **Nothing has been run on an actual Windows kernel, and Wine is not a substitute for
  one.** `winsvc.h`/`advapi32` Service Control Manager calls (`CreateService`,
  `StartServiceCtrlDispatcher`, `RegisterServiceCtrlHandlerEx`, `SERVICE_CONTROL_STOP` /
  `SERVICE_CONTROL_SHUTDOWN` / `SERVICE_CONTROL_PRESHUTDOWN` handling) have compiled
  and linked successfully under MinGW and (for the process-launch path only, not real
  SCM dispatch) been observed to fail gracefully under Wine, but **real SCM-driven
  install/start/stop/PRESHUTDOWN lifecycle has never been exercised** and must be
  tested on real Windows before being trusted. Likewise, real end-to-end named-pipe
  IPC between two separate real Windows processes (a GUI process and a LocalSystem
  service process, crossing an actual privilege boundary) has never been exercised —
  only the security descriptor's structural well-formedness (see above).
- **`FlushFileBuffers`/real NTFS/real-disk-controller durability semantics.** Wine's
  `FlushFileBuffers` implementation calls into the host Linux kernel's own fsync-family
  syscalls; this is a reasonable functional approximation for catching logic bugs (and
  did catch one — see above) but is not evidence about real NTFS journaling behavior,
  real SSD/HDD write-cache/power-loss-protection behavior, or any Windows-version-
  specific filesystem quirk. This must be tested on real Windows/NTFS hardware.
- **Actual physical power-loss testing has not been performed and cannot be performed
  from any sandbox, virtualized or emulated.** Nothing in this project claims otherwise;
  see the dedicated paragraph on this later in this document and in the final Stage 2
  hardening report.
- **WinUI 3 GUI project is unbuilt and unverifiable here.** There is no C++/WinRT, XAML
  compiler, MIDL3, Windows App SDK, or MSIX packaging tool available in this sandbox.
  The `gui/QuantumCacheGui` project contains real, hand-written XAML/C++ source and a
  real `.vcxproj`, but it has **never been compiled** by any tool. It requires Visual
  Studio 2022 (17.x) with the "Desktop development with C++" and "Windows App SDK C++"
  workloads on an actual Windows machine to build for the first time.
- Real power-loss behavior (actual sudden power cut on real hardware/NTFS) has obviously
  not been tested — only the *logical* state machine and journal replay algorithm have
  unit tests, exercised via a simulated crash (killing the process mid-journal-write in
  a test) on the portable file-system abstraction. Stage 2 extends this same style of
  simulated-crash testing to actual cache data (see
  `CacheEngineTest.JournalReplay_RecoversDirtyEntryAfterSimulatedCrash` and related tests
  in `docs/STAGE2_ARCHITECTURE.md`), but it is still a simulation on a Linux filesystem,
  not a real power cut on real Windows/NTFS hardware.
- Stage 2's Windows Service wiring (`main_service.cpp`) now also constructs and drives
  the cache engine through the required start/recovery/ready and
  stop/flush/persist/clean-shutdown sequences. This compiles and links via MinGW-w64 into
  a genuine PE32+ executable (verified with `file`/`objdump`), but — like the rest of the
  Service component — has never been run against a live Service Control Manager.
- `FlushPolicyKind::PeriodicBackground` (Stage 2B): the background flush thread is now
  genuinely implemented (a real `std::thread` inside `CacheEngine`, started only after
  recovery completes, calling the real `FlushAll()` on a timer) and covered by real,
  wall-clock-waiting tests (`CacheEngineTest.PeriodicBackgroundFlush_*`) executed in this
  sandbox — not a simulation. See `docs/STAGE2_ARCHITECTURE.md` for the design.

## Why the GUI is a separate build system from the rest

WinUI 3 C++ desktop apps are not meaningfully buildable through CMake in current,
supported Microsoft tooling: the officially supported flow packages the app via MSIX
and depends on the XAML compiler, MIDL3 IDL compilation (`.idl` → generated headers),
and Windows App SDK NuGet restore, all orchestrated by MSBuild `.vcxproj`/`.sln`
targets that CMake does not (and, per Microsoft's own guidance, is not expected to)
reproduce. Community CMake+WinUI3 setups exist but are unofficial, fragile, and would
constitute exactly the kind of "faked" tooling this project is explicitly avoiding.
Instead:

- The backend (Core Engine, Storage, Configuration, Power Resilience, Logging, IPC,
  Windows Service, Tests) is one CMake project, buildable with the Visual Studio 17
  2022 generator on Windows, or cross-compiled with MinGW-w64 (as verified above).
- The WinUI 3 GUI is a standard MSBuild `.vcxproj`/`.sln` C++/WinRT project that talks
  to the backend service only through the IPC contract defined in `src/Ipc`. It never
  links the backend statically, so the two build systems stay decoupled by design.
