// Real Win32 implementation of IVolume using GetDiskFreeSpaceExW /
// GetDriveTypeW. Compiled only for Windows targets. Verified in this
// sandbox only to the extent that it compiles/links via MinGW-w64 into a
// PE object; actual behavior against a live Windows volume (including
// network/removable drives) has not been exercised.
#include "QuantumCache/Storage/IVolume.h"
#include <windows.h>

namespace QuantumCache::Storage {
namespace {

using Common::Error;
using Common::ErrorCode;
using Common::Result;

class Win32Volume final : public IVolume {
public:
    explicit Win32Volume(std::wstring rootPath) : rootPath_(std::move(rootPath)) {}

    Result<VolumeInfo> QueryInfo() const override {
        VolumeInfo info;
        info.rootPath = rootPath_;

        ULARGE_INTEGER freeBytesAvailable{};
        ULARGE_INTEGER totalBytes{};
        ULARGE_INTEGER totalFreeBytes{};
        BOOL ok = ::GetDiskFreeSpaceExW(
            rootPath_.c_str(), &freeBytesAvailable, &totalBytes, &totalFreeBytes);
        if (!ok) {
            Error err;
            err.code = ErrorCode::Win32ApiFailure;
            err.platformErrorValue = ::GetLastError();
            err.message = "GetDiskFreeSpaceExW failed";
            return Result<VolumeInfo>::Failure(err);
        }

        info.totalBytes = totalBytes.QuadPart;
        info.freeBytes = totalFreeBytes.QuadPart;

        UINT driveType = ::GetDriveTypeW(rootPath_.c_str());
        info.isFixedDrive = (driveType == DRIVE_FIXED);
        info.isRemovable = (driveType == DRIVE_REMOVABLE);

        wchar_t volumeName[MAX_PATH] = {};
        if (::GetVolumeNameForVolumeMountPointW(
                rootPath_.c_str(), volumeName, MAX_PATH)) {
            info.volumeGuidPath = volumeName;
        }

        return Result<VolumeInfo>::Success(info);
    }

    bool IsAvailable() const noexcept override {
        UINT driveType = ::GetDriveTypeW(rootPath_.c_str());
        return driveType != DRIVE_NO_ROOT_DIR && driveType != DRIVE_UNKNOWN;
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
        std::make_unique<Win32Volume>(rootPath));
}

} // namespace QuantumCache::Storage
