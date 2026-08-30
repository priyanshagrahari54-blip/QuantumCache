#pragma once
#include "QuantumCache/Common/Result.h"
#include <cstdint>
#include <memory>
#include <string>

namespace QuantumCache::Storage {

// Identifies and reports basic facts about a storage volume that
// QuantumCache is configured to use (e.g. the SSD path acting as cache, or
// the HDD/network path being cached). Stage 1 intentionally implements
// only enumeration/identification/free-space queries — NOT the cache
// engine, NOT block-level interception, NOT the deferred-write engine.
struct VolumeInfo {
    std::wstring rootPath;       // e.g. L"D:\\" or L"D:\\QuantumCacheStore\\"
    std::wstring volumeGuidPath; // Win32 \\?\Volume{GUID}\ form, when resolvable
    std::uint64_t totalBytes{0};
    std::uint64_t freeBytes{0};
    bool isFixedDrive{false};
    bool isRemovable{false};
};

class IVolume {
public:
    virtual ~IVolume() = default;

    [[nodiscard]] virtual Common::Result<VolumeInfo> QueryInfo() const = 0;

    // Returns true if the underlying volume is currently reachable/mounted.
    // Meaningful for cache targets that may live on removable or network
    // storage; a false result is a real-world condition the power/crash
    // resilience layer must be aware could co-occur with an unclean
    // shutdown, not just an isolated I/O error.
    [[nodiscard]] virtual bool IsAvailable() const noexcept = 0;

    [[nodiscard]] virtual const std::wstring& RootPath() const noexcept = 0;
};

[[nodiscard]] Common::Result<std::unique_ptr<IVolume>> OpenVolume(const std::wstring& rootPath);

} // namespace QuantumCache::Storage
