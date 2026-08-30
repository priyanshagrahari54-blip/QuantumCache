// Real JSON-backed config store using nlohmann::json (header-only, MIT
// licensed, available both as a system package on this Linux sandbox and
// as a vcpkg/NuGet-style dependency on Windows — see cmake/Dependencies.cmake).
//
// AUDITED BUG (fixed): Save() used to open the DESTINATION file directly
// with std::ios::trunc and write straight into it. A power cut mid-write
// left the config file truncated/partially-written garbage, with the
// last known-good configuration already destroyed by the truncate — the
// exact opposite of "preserve the last known-good configuration if
// anything fails." Save() now performs the required conceptual sequence:
//   1. write the COMPLETE new configuration to a temporary file
//      (never touching the real path);
//   2. durably flush that temporary file (platform durability primitive:
//      fsync on POSIX, FlushFileBuffers on Windows — never merely a
//      language-level stream flush);
//   3. atomically replace the original file with the temporary file
//      (rename() on POSIX is atomic when source/destination are on the
//      same filesystem; ReplaceFileW/MoveFileExW with
//      MOVEFILE_REPLACE_EXISTING on Windows);
//   4. durably flush the directory entry too, where the platform exposes
//      a way to do so (POSIX: fsync the containing directory fd, so the
//      rename itself — not just the file's new content — survives a
//      power cut; Windows does not expose an equivalent directory-fsync
//      primitive through the Win32 API, so this step is POSIX-only and
//      documented as such rather than silently skipped).
// If ANY step fails, the original file is left completely untouched
// (steps 1-2 never touch it; if step 3 fails, the destination still has
// its pre-existing content), which is what "preserve the last known-good
// configuration" requires.
#include "QuantumCache/Configuration/IConfigStore.h"
#include <nlohmann/json.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <random>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace QuantumCache::Configuration {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;
using nlohmann::json;

std::string NarrowFromWide(const std::wstring& wide) {
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t c : wide) narrow.push_back(static_cast<char>(c));
    return narrow;
}

std::wstring WideFromNarrow(const std::string& narrow) {
    std::wstring wide;
    wide.reserve(narrow.size());
    for (char c : narrow) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return wide;
}

#if defined(_WIN32)
std::wstring NarrowPathToWide(const std::string& narrow) {
    // The config path is a UTF-8/narrow filesystem path per this file's
    // IConfigStore contract; Win32 wide-character APIs are used for the
    // actual file operations. A full UTF-8->UTF-16 conversion (not the
    // simplistic widen-each-byte used elsewhere in this file purely for
    // AppConfig's own internal wstring fields) is used here since this
    // path is handed directly to real Win32 file APIs and must be a
    // correct Windows path.
    if (narrow.empty()) return std::wstring();
    int required = ::MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, nullptr, 0);
    if (required <= 0) return std::wstring();
    std::wstring wide(static_cast<std::size_t>(required - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, narrow.c_str(), -1, wide.data(), required);
    return wide;
}
#endif

// Real, working "temp file -> durable flush -> atomic rename" primitive
// shared by JsonConfigStore::Save(). Not folded into Storage::IFile
// because atomic rename-into-place is a filesystem-namespace operation,
// not a single-file durability operation, and IFile intentionally models
// only the latter (see IFile.h). `content` is written verbatim (this
// function does not know or care that it is JSON).
Result<void> AtomicallyReplaceFileContents(const std::string& targetPath, const std::string& content) {
#if defined(_WIN32)
    std::wstring targetWide = NarrowPathToWide(targetPath);
    // Temp file lives NEXT TO the target (same directory / same volume)
    // so the final rename is a same-volume, atomic operation rather than
    // a cross-volume copy+delete. A random suffix avoids collisions
    // between concurrent writers (e.g. a future multi-writer scenario)
    // without requiring a real UUID dependency.
    std::random_device rd;
    std::wstring tempWide = targetWide + L".tmp" + std::to_wstring(rd());

    HANDLE tempHandle = ::CreateFileW(
        tempWide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (tempHandle == INVALID_HANDLE_VALUE) {
        return Result<void>::Failure(
            Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: CreateFileW (temp) failed", ::GetLastError()});
    }

    DWORD written = 0;
    BOOL writeOk = ::WriteFile(tempHandle, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    bool sizeOk = writeOk && written == content.size();
    if (!sizeOk) {
        DWORD err = ::GetLastError();
        ::CloseHandle(tempHandle);
        ::DeleteFileW(tempWide.c_str());
        return Result<void>::Failure(
            Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: WriteFile (temp) failed", err});
    }

    // Step 2: durable flush of the TEMP file's content — a real OS
    // durability request (FlushFileBuffers), not merely a stream flush.
    if (!::FlushFileBuffers(tempHandle)) {
        DWORD err = ::GetLastError();
        ::CloseHandle(tempHandle);
        ::DeleteFileW(tempWide.c_str());
        return Result<void>::Failure(
            Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: FlushFileBuffers (temp) failed", err});
    }
    ::CloseHandle(tempHandle);

    // Step 3: atomic replace. MOVEFILE_REPLACE_EXISTING makes this
    // overwrite the destination if present; MOVEFILE_WRITE_THROUGH asks
    // Windows not to return until the rename itself is flushed through to
    // the storage device, which is the closest Win32 equivalent to POSIX's
    // "fsync the containing directory" step (Windows does not expose a
    // separate directory-handle-fsync primitive through documented Win32
    // APIs).
    if (!::MoveFileExW(tempWide.c_str(), targetWide.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD err = ::GetLastError();
        ::DeleteFileW(tempWide.c_str());
        return Result<void>::Failure(
            Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: MoveFileExW failed", err});
    }

    return Result<void>::Success();
#else
    // POSIX path (used on this Linux development sandbox; also the
    // portable reference behavior for any future non-Windows target).
    std::string tempPath = targetPath + ".tmp";

    int fd = ::open(tempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd < 0) {
        return Result<void>::Failure(Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: open (temp) failed", 0});
    }

    std::size_t totalWritten = 0;
    while (totalWritten < content.size()) {
        ssize_t n = ::write(fd, content.data() + totalWritten, content.size() - totalWritten);
        if (n < 0) {
            ::close(fd);
            ::unlink(tempPath.c_str());
            return Result<void>::Failure(Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: write (temp) failed", 0});
        }
        totalWritten += static_cast<std::size_t>(n);
    }

    // Step 2: durable flush of the TEMP file's content (real OS
    // durability request, fsync — not merely closing the fd).
    if (::fsync(fd) != 0) {
        ::close(fd);
        ::unlink(tempPath.c_str());
        return Result<void>::Failure(Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: fsync (temp) failed", 0});
    }
    ::close(fd);

    // Step 3: atomic replace. POSIX guarantees rename() is atomic with
    // respect to other processes observing the target path when source
    // and destination are on the same filesystem (the common case here,
    // since the temp file was created right next to the target).
    if (::rename(tempPath.c_str(), targetPath.c_str()) != 0) {
        ::unlink(tempPath.c_str());
        return Result<void>::Failure(Error{ErrorCode::IoError, "AtomicallyReplaceFileContents: rename failed", 0});
    }

    // Step 4: durably flush the DIRECTORY ENTRY, not just the file's
    // content. Without this, a power cut immediately after a successful
    // rename() can — on some filesystems/mount options — lose the
    // directory-entry update itself, potentially reverting to the OLD
    // file or leaving the directory entry in an inconsistent state, even
    // though the file's own bytes were fsync'd in step 2. This is a real,
    // commonly-overlooked durability gap in naive "write temp + rename"
    // implementations. Best-effort: if the directory cannot be opened or
    // fsync'd (e.g. an unusual filesystem that does not support
    // directory fsync), the rename has still happened and the content is
    // still durable; only the microscopic window around the rename's own
    // metadata durability is not covered, which is why this is
    // documented rather than treated as a hard failure of the whole
    // Save() call.
    auto lastSlash = targetPath.find_last_of('/');
    std::string dirPath = (lastSlash == std::string::npos) ? std::string(".") : targetPath.substr(0, lastSlash);
    int dirFd = ::open(dirPath.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        ::fsync(dirFd);
        ::close(dirFd);
    }

    return Result<void>::Success();
#endif
}

json ToJson(const AppConfig& config) {
    json j;
    j["schemaVersion"] = config.schemaVersion;
    j["stateDirectory"] = NarrowFromWide(config.stateDirectory);
    j["logDirectory"] = NarrowFromWide(config.logDirectory);
    j["minimumLogLevel"] = config.minimumLogLevel;
    j["ipcPipeName"] = NarrowFromWide(config.ipcPipeName);

    j["cacheEnabled"] = config.cacheEnabled;
    j["backingStoreDataFile"] = NarrowFromWide(config.backingStoreDataFile);
    j["cacheCapacityBytes"] = config.cacheCapacityBytes;
    j["cacheMaxEntryCount"] = config.cacheMaxEntryCount;
    j["cacheShardCount"] = config.cacheShardCount;
    j["evictionPolicy"] = config.evictionPolicy;
    j["writePolicy"] = config.writePolicy;
    j["flushPolicy"] = config.flushPolicy;
    j["flushIntervalSeconds"] = config.flushIntervalSeconds;
    return j;
}

Result<AppConfig> FromJson(const json& j) {
    AppConfig config;
    try {
        config.schemaVersion = j.at("schemaVersion").get<std::uint32_t>();
        config.stateDirectory = WideFromNarrow(j.at("stateDirectory").get<std::string>());
        config.logDirectory = WideFromNarrow(j.at("logDirectory").get<std::string>());
        config.minimumLogLevel = j.at("minimumLogLevel").get<int>();
        config.ipcPipeName = WideFromNarrow(j.at("ipcPipeName").get<std::string>());

        // Stage 2 fields: absent entirely in a Stage 1 (schemaVersion==1)
        // config file, in which case AppConfig's own default member
        // initializers (already applied above via `AppConfig config;`)
        // are kept rather than treating their absence as malformed JSON.
        // This is the actual schema migration path this field's doc
        // comment promises: old configs load successfully and pick up
        // sane Stage 2 defaults instead of being rejected outright.
        if (j.contains("cacheEnabled")) config.cacheEnabled = j.at("cacheEnabled").get<bool>();
        if (j.contains("backingStoreDataFile"))
            config.backingStoreDataFile = WideFromNarrow(j.at("backingStoreDataFile").get<std::string>());
        if (j.contains("cacheCapacityBytes"))
            config.cacheCapacityBytes = j.at("cacheCapacityBytes").get<std::uint64_t>();
        if (j.contains("cacheMaxEntryCount"))
            config.cacheMaxEntryCount = j.at("cacheMaxEntryCount").get<std::uint64_t>();
        if (j.contains("cacheShardCount"))
            config.cacheShardCount = j.at("cacheShardCount").get<std::uint32_t>();
        if (j.contains("evictionPolicy"))
            config.evictionPolicy = j.at("evictionPolicy").get<std::string>();
        if (j.contains("writePolicy"))
            config.writePolicy = j.at("writePolicy").get<std::string>();
        if (j.contains("flushPolicy"))
            config.flushPolicy = j.at("flushPolicy").get<std::string>();
        if (j.contains("flushIntervalSeconds"))
            config.flushIntervalSeconds = j.at("flushIntervalSeconds").get<std::uint32_t>();
    } catch (const json::exception& ex) {
        return Result<AppConfig>::Failure(
            Error{ErrorCode::CorruptData, std::string("config JSON malformed: ") + ex.what(), 0});
    }
    return Result<AppConfig>::Success(config);
}

class JsonConfigStore final : public IConfigStore {
public:
    explicit JsonConfigStore(std::string path) : path_(std::move(path)) {}

    Result<AppConfig> Load() override {
        std::ifstream in(path_, std::ios::in | std::ios::binary);
        if (!in.is_open()) {
            return Result<AppConfig>::Failure(
                Error{ErrorCode::NotFound, "config file not found: " + path_, 0});
        }

        json parsed;
        try {
            in >> parsed;
        } catch (const json::parse_error& ex) {
            return Result<AppConfig>::Failure(
                Error{ErrorCode::CorruptData, std::string("config JSON parse error: ") + ex.what(), 0});
        }

        auto result = FromJson(parsed);
        if (!result) return result;

        auto validation = Validate(result.Value());
        if (!validation) {
            return Result<AppConfig>::Failure(validation.Err());
        }

        return result;
    }

    Result<void> Save(const AppConfig& config) override {
        auto validation = Validate(config);
        if (!validation) return validation;

        // AUDITED BUG (fixed): see the file-header comment on
        // AtomicallyReplaceFileContents for the full rationale. The
        // complete new configuration is serialized in memory FIRST, then
        // handed to the crash-safe temp-file+durable-flush+atomic-rename
        // primitive — the original file at path_ is never opened for
        // writing directly, so a failure or interruption at any point
        // leaves it completely untouched.
        std::string serialized = ToJson(config).dump(2);
        return AtomicallyReplaceFileContents(path_, serialized);
    }

    Result<void> Validate(const AppConfig& config) const override {
        if (config.schemaVersion == 0 || config.schemaVersion > 2) {
            return Result<void>::Failure(
                Error{ErrorCode::VersionMismatch,
                      "unsupported config schemaVersion (this build supports versions 1-2)", 0});
        }
        if (config.stateDirectory.empty()) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "stateDirectory must not be empty", 0});
        }
        if (config.logDirectory.empty()) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "logDirectory must not be empty", 0});
        }
        if (config.minimumLogLevel < 0 || config.minimumLogLevel > 5) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "minimumLogLevel out of range [0,5]", 0});
        }
        if (config.ipcPipeName.empty() || config.ipcPipeName.rfind(L"\\\\.\\pipe\\", 0) != 0) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument,
                      "ipcPipeName must be a well-formed local named pipe path", 0});
        }

        // ---------------------------------------------------------
        // Stage 2: cache engine configuration validation. Every setting
        // is checked against a real, enforced constraint — none of these
        // are decorative.
        // ---------------------------------------------------------
        if (config.backingStoreDataFile.empty()) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "backingStoreDataFile must not be empty", 0});
        }
        if (config.cacheCapacityBytes == 0) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "cacheCapacityBytes must be > 0", 0});
        }
        // Reject implausibly large capacity requests: this project never
        // silently accepts a value it cannot possibly honor (e.g. a typo
        // adding a few extra zeros). 1 TiB is far beyond any realistic
        // Stage 2 RAM cache while still not being an arbitrary tiny cap.
        constexpr std::uint64_t kMaxSaneCapacityBytes = 1024ull * 1024 * 1024 * 1024;
        if (config.cacheCapacityBytes > kMaxSaneCapacityBytes) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument,
                      "cacheCapacityBytes exceeds the maximum sane value (1 TiB)", 0});
        }
        if (config.cacheMaxEntryCount == 0) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "cacheMaxEntryCount must be > 0", 0});
        }
        if (config.cacheShardCount == 0 ||
            (config.cacheShardCount & (config.cacheShardCount - 1)) != 0) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "cacheShardCount must be a power of two >= 1", 0});
        }
        if (config.cacheShardCount > 4096) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument, "cacheShardCount exceeds the maximum sane value (4096)", 0});
        }
        if (config.evictionPolicy != "LeastRecentlyUsed") {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument,
                      "evictionPolicy must be 'LeastRecentlyUsed' (the only policy implemented)", 0});
        }
        if (config.writePolicy != "WriteThrough" && config.writePolicy != "WriteBackDeferred") {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument,
                      "writePolicy must be 'WriteThrough' or 'WriteBackDeferred'", 0});
        }
        if (config.flushPolicy != "Manual" && config.flushPolicy != "PeriodicBackground") {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument,
                      "flushPolicy must be 'Manual' or 'PeriodicBackground'", 0});
        }
        if (config.flushPolicy == "PeriodicBackground" && config.flushIntervalSeconds == 0) {
            return Result<void>::Failure(
                Error{ErrorCode::InvalidArgument,
                      "flushIntervalSeconds must be >= 1 when flushPolicy is 'PeriodicBackground'", 0});
        }

        return Result<void>::Success();
    }

private:
    std::string path_;
};

} // namespace

Result<std::unique_ptr<IConfigStore>> CreateJsonConfigStore(const std::string& path) {
    if (path.empty()) {
        return Result<std::unique_ptr<IConfigStore>>::Failure(
            Error{ErrorCode::InvalidArgument, "path must not be empty", 0});
    }
    return Result<std::unique_ptr<IConfigStore>>::Success(
        std::make_unique<JsonConfigStore>(path));
}

} // namespace QuantumCache::Configuration
