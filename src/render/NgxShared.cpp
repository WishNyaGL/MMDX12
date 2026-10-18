// Shared NGX SDK lifetime (see NgxShared.h).
#include "render/NgxShared.h"
#include "render/Dx12Context.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <nvsdk_ngx.h>
#include <filesystem>

namespace mmdx {

namespace {
int refCount_ = 0;
bool initOk_ = false;
ID3D12Device* initDevice_ = nullptr;
} // namespace

bool NgxSharedInit(Dx12Context& ctx) {
    if (refCount_ > 0) {
        // A second caller after a successful init just takes a reference; after a failed one it keeps failing.
        if (initOk_) {
            ++refCount_;
            return true;
        }
        return false;
    }
    const std::filesystem::path exeDir = ExecutableDir();
    const std::wstring exeDirString = exeDir.wstring();
    NVSDK_NGX_FeatureCommonInfo info{};
    const wchar_t* paths[] = {exeDirString.c_str()};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(
        "a3b1c9d2-4e5f-4a6b-8c7d-9e0f1a2b3c4d", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", exeDirString.c_str(),
        ctx.Device(), &info, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r)) {
        LOG_WARN("NGX: init failed (0x%08X)", (unsigned)r);
        return false;
    }
    initOk_ = true;
    initDevice_ = ctx.Device();
    ++refCount_;
    return true;
}

void NgxSharedRelease() {
    if (refCount_ <= 0) return;
    if (--refCount_ > 0) return;
    if (initOk_ && initDevice_) NVSDK_NGX_D3D12_Shutdown1(initDevice_);
    initOk_ = false;
    initDevice_ = nullptr;
}

} // namespace mmdx
