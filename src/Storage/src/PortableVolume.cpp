// Portable reference IVolume used only for host-side (non-Windows) unit
// testing of components that depend on IVolume (e.g. configuration
// validation of a configured cache path). Not part of the Windows product
// build.
#include "QuantumCache/Storage/IVolume.h"
#include <filesystem>

namespace QuantumCache::Storage {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

std::string NarrowPath(const std::wstring& wide) {
    std::string narrow;
    narrow.reserve(wide.size());
    for (wchar_t c : wide) narrow.push_back(static_cast<char>(c));
    return narrow;
}

class PortableVolume final : public IVolume {
public:
    explicit PortableVolume(std::wstring rootPath) : rootPath_(std::move(rootPath)) {}

    Result<VolumeInfo> QueryInfo() const override {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto space = fs::space(NarrowPath(rootPath_), ec);
        if (ec) {
            return Result<VolumeInfo>::Failure(
                Error{ErrorCode::IoError, "std::filesystem::space failed: " + ec.message(), 0});
        }
        VolumeInfo info;
        info.rootPath = rootPath_;
        info.totalBytes = space.capacity;
        info.freeBytes = space.available;
        info.isFixedDrive = true;
        info.isRemovable = false;
        return Result<VolumeInfo>::Success(info);
    }

    bool IsAvailable() const noexcept override {
        std::error_code ec;
        return std::filesystem::exists(NarrowPath(rootPath_), ec) && !ec;
    }

    const std::wstring& RootPath() const noexcept override { return rootPath_; }

private:
    std::wstring rootPath_;
};

} // namespace

Result<std::unique_ptr<IVolume>> OpenVolume(const std::wstring& rootPath) {
    if (rootPath.empty()) {
        return Result<std::unique_ptr<IVolume>>::Failure(
            Error{ErrorCode::InvalidArgument, "rootPath must not be empty", 0});
    }
    return Result<std::unique_ptr<IVolume>>::Success(
        std::make_unique<PortableVolume>(rootPath));
}

} // namespace QuantumCache::Storage
