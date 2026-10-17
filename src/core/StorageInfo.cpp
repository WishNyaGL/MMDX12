#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include "core/StorageInfo.h"

#include <string>

namespace mmdx {

namespace {

// Ask the storage device behind `volume` (a \\.\X: or \\?\Volume{...} name, no trailing
// backslash) whether it incurs a seek penalty. Any failure returns false.
bool QuerySeekPenalty(const std::wstring& volume) {
    HANDLE h = CreateFileW(volume.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR desc{};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &desc, sizeof(desc), &bytes, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    return desc.IncursSeekPenalty != FALSE;
}

} // namespace

bool PathIncursSeekPenalty(const std::filesystem::path& path) {
    wchar_t volumePath[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), volumePath, MAX_PATH)) return false;

    std::wstring volume;
    if (volumePath[0] != L'\0' && volumePath[1] == L':' && (volumePath[2] == L'\\' || volumePath[2] == L'\0') &&
        volumePath[3] == L'\0') {
        // Drive-letter root (C:\): open the device as \\.\C:.
        volume = L"\\\\.\\";
        volume += volumePath[0];
        volume += L':';
    } else {
        // Mounted-folder volume: resolve it to \\?\Volume{...}\ and strip the trailing backslash.
        wchar_t volumeName[MAX_PATH];
        if (!GetVolumeNameForVolumeMountPointW(volumePath, volumeName, MAX_PATH)) return false;
        volume = volumeName;
        while (!volume.empty() && volume.back() == L'\\') volume.pop_back();
    }
    if (volume.empty()) return false;
    return QuerySeekPenalty(volume);
}

} // namespace mmdx
