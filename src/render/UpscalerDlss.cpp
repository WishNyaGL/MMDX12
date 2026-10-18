// NVIDIA DLSS via the NGX SDK 310.x (static loader, feature DLLs next to the exe).
// d3dx12.h first so <d3d12.h> (Upscaler.h) resolves to the vendored DirectX-Headers, whose
// D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT d3dx12_core.h requires.
#include <directx/d3dx12.h>
#include "render/Upscaler.h"
#include "render/Dx12Context.h"
#include "render/NgxShared.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_d3d.h>
#include <nvsdk_ngx_helpers_dlssd_d3d.h>
#include <nvsdk_ngx_defs_dlssg.h>   // FrameGeneration.Available capability bit
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

namespace {

// Reads the file version of a DLL next to the exe (e.g. "310.9.16.0"); empty without version info.
std::string DllFileVersion(const std::filesystem::path& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), handle, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (!VerQueryValueW(data.data(), L"\\", (void**)&info, &infoSize) || !info) return {};
    return std::to_string(HIWORD(info->dwFileVersionMS)) + "." + std::to_string(LOWORD(info->dwFileVersionMS)) + "." +
           std::to_string(HIWORD(info->dwFileVersionLS)) + "." + std::to_string(LOWORD(info->dwFileVersionLS));
}

NVSDK_NGX_DLSS_Hint_Render_Preset SrPreset(DlssModel model, UpscalerQuality quality) {
    switch (model) {
    case DlssModel::Cnn:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_E;
    case DlssModel::Transformer:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_K;
    case DlssModel::Transformer2:
        // DLSS 4.5: preset M everywhere, preset L for Ultra Performance (its documented default).
        return quality == UpscalerQuality::UltraPerformance ? NVSDK_NGX_DLSS_Hint_Render_Preset_L
                                                            : NVSDK_NGX_DLSS_Hint_Render_Preset_M;
    case DlssModel::Auto:
    default:
        return NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
    }
}

class DlssUpscaler final : public IUpscaler {
public:
    UpscalerKind Kind() const override { return UpscalerKind::DLSS; }
    const char* DisplayName() const override { return "NVIDIA DLSS"; }
    bool IsAvailable() const override { return available_; }
    bool RayReconstructionAvailable() const override { return rrAvailable_; }

    bool Initialize(Dx12Context& ctx) override {
        if (ctx.Caps().vendorId != 0x10DE) {
            LOG_INFO("DLSS: not an NVIDIA GPU");
            return false;
        }
        std::filesystem::path exeDir = ExecutableDir();
        const std::filesystem::path srDll = exeDir / L"nvngx_dlss.dll";
        const std::filesystem::path rrDll = exeDir / L"nvngx_dlssd.dll";
        if (!std::filesystem::exists(srDll)) {
            LOG_WARN("DLSS: nvngx_dlss.dll not found next to the exe");
            return false;
        }
        std::wstring exeDirString = exeDir.wstring();
        // NGX is initialised once per device and shared with DLSS Frame Generation (render/NgxShared.h).
        if (!NgxSharedInit(ctx)) {
            LOG_WARN("DLSS: NGX init failed");
            return false;
        }
        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params_);
        if (NVSDK_NGX_FAILED(r)) {
            LOG_WARN("DLSS: GetCapabilityParameters failed (0x%08X)", (unsigned)r);
            NgxSharedRelease();
            return false;
        }
        int srAvailable = 0, rrAvailable = 0, fgAvailable = 0;
        params_->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &srAvailable);
        params_->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &rrAvailable);
        params_->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &fgAvailable);
        // RR needs its own feature DLL; without it the capability bit alone is not usable.
        rrAvailable_ = rrAvailable != 0 && std::filesystem::exists(rrDll);
        const std::string srVersion = DllFileVersion(srDll);
        const std::string rrVersion = std::filesystem::exists(rrDll) ? DllFileVersion(rrDll) : std::string();
        LOG_INFO("DLSS: SR %s (dll %s), RR %s (dll %s), FG %s (dll version %s)", srAvailable ? "available" : "unavailable",
                 srVersion.empty() ? "unknown" : srVersion.c_str(), rrAvailable_ ? "available" : "unavailable",
                 rrVersion.empty() ? "missing" : rrVersion.c_str(), fgAvailable ? "available" : "unavailable",
                 srVersion.empty() ? "unknown" : srVersion.c_str());
        if (!srAvailable) {
            LOG_WARN("DLSS: SuperSampling not available (driver/snippet missing)");
            device_ = ctx.Device();
            Shutdown();
            return false;
        }
        device_ = ctx.Device();
        available_ = true;
        return true;
    }

    bool Evaluate(Dx12Context& ctx, const UpscaleInputs& in) override {
        if (!available_ || !params_) return false;
        const bool wantRr = in.rayReconstruction && rrAvailable_ && in.diffuseAlbedo && in.specularAlbedo &&
                            in.roughness && in.normals;
        if (wantRr != rrMode_) {
            ctx.WaitForGpu();
            ReleaseFeatures();
            rrMode_ = wantRr;
        }
        if (feature_ && (in.renderWidth != curRenderW_ || in.renderHeight != curRenderH_ ||
                         in.outputWidth != curOutW_ || in.outputHeight != curOutH_ ||
                         in.quality != curQuality_ || in.dlssModel != curModel_)) {
            ctx.WaitForGpu();
            ReleaseFeatures();
        }
        if (!feature_) {
            if (wantRr) {
                if (!CreateRr(in)) return false;
            } else {
                if (!CreateSr(in)) return false;
            }
            curRenderW_ = in.renderWidth;
            curRenderH_ = in.renderHeight;
            curOutW_ = in.outputWidth;
            curOutH_ = in.outputHeight;
            curQuality_ = in.quality;
            curModel_ = in.dlssModel;
        }
        return rrMode_ ? EvaluateRr(in) : EvaluateSr(in);
    }

    void Shutdown() override {
        if (params_ && device_) {
            ReleaseFeatures();   // the GPU must be idle: callers wait before Shutdown
        }
        if (params_) {
            NVSDK_NGX_D3D12_DestroyParameters(params_);
            params_ = nullptr;
        }
        if (device_) {
            NgxSharedRelease();
            device_ = nullptr;
        }
        curRenderW_ = curRenderH_ = curOutW_ = curOutH_ = 0;
        loggedFailure_ = false;
        available_ = false;
        rrAvailable_ = false;
        rrMode_ = false;
    }

private:
    static NVSDK_NGX_PerfQuality_Value PerfQuality(UpscalerQuality quality) {
        switch (quality) {
        case UpscalerQuality::NativeAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
        case UpscalerQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        case UpscalerQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case UpscalerQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case UpscalerQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        }
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }

    void ReleaseFeatures() {
        if (feature_) {
            NVSDK_NGX_D3D12_ReleaseFeature(feature_);
            feature_ = nullptr;
        }
    }

    void SetSrPresets(DlssModel model) {
        const NVSDK_NGX_DLSS_Hint_Render_Preset preset = SrPreset(model, UpscalerQuality::Quality);
        const NVSDK_NGX_DLSS_Hint_Render_Preset ultraPreset = SrPreset(model, UpscalerQuality::UltraPerformance);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, preset);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, preset);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, preset);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, preset);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, ultraPreset);
    }

    bool CreateSr(const UpscaleInputs& in) {
        NVSDK_NGX_DLSS_Create_Params cp{};
        cp.Feature.InWidth = in.renderWidth;
        cp.Feature.InHeight = in.renderHeight;
        cp.Feature.InTargetWidth = in.outputWidth;
        cp.Feature.InTargetHeight = in.outputHeight;
        cp.Feature.InPerfQualityValue = PerfQuality(in.quality);
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        SetSrPresets(in.dlssModel);
        NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(in.cmd, 1, 1, &feature_, params_, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            LOG_ERROR("DLSS: feature creation failed (0x%08X)", (unsigned)r);
            feature_ = nullptr;
            LogOnce("DLSS: evaluate unavailable, will not retry until the parameters change");
            return false;
        }
        loggedFailure_ = false;
        LOG_INFO("DLSS: SR feature created (%ux%u -> %ux%u, model %s)", in.renderWidth, in.renderHeight,
                 in.outputWidth, in.outputHeight, DlssModelId(in.dlssModel));
        return true;
    }

    bool CreateRr(const UpscaleInputs& in) {
        NVSDK_NGX_DLSSD_Create_Params cp{};
        cp.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;   // roughness in normals.w
        cp.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        cp.InWidth = in.renderWidth;
        cp.InHeight = in.renderHeight;
        cp.InTargetWidth = in.outputWidth;
        cp.InTargetHeight = in.outputHeight;
        cp.InPerfQualityValue = PerfQuality(in.quality);
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        // RR model: driver default (preset Default for every quality key).
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                                  NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                                  NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                                  NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                                  NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
        NVSDK_NGX_Parameter_SetUI(params_, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
                                  NVSDK_NGX_RayReconstruction_Hint_Render_Preset_Default);
        NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSD_EXT(in.cmd, 1, 1, &feature_, params_, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            LOG_ERROR("DLSS: RR feature creation failed (0x%08X)", (unsigned)r);
            feature_ = nullptr;
            LogOnce("DLSS: RR unavailable, falling back to SR after this frame");
            rrAvailable_ = false;   // do not retry RR until the next run
            rrMode_ = false;
            return false;
        }
        loggedFailure_ = false;
        LOG_INFO("DLSS: RR feature created (%ux%u -> %ux%u)", in.renderWidth, in.renderHeight, in.outputWidth,
                 in.outputHeight);
        return true;
    }

    bool EvaluateSr(const UpscaleInputs& in) {
        NVSDK_NGX_D3D12_DLSS_Eval_Params ep{};
        ep.Feature.pInColor = in.color;
        ep.Feature.pInOutput = in.output;
        ep.pInDepth = in.depth;
        ep.pInMotionVectors = in.velocity;
        ep.InJitterOffsetX = in.jitterX;
        ep.InJitterOffsetY = in.jitterY;
        ep.InRenderSubrectDimensions = {in.renderWidth, in.renderHeight};
        ep.InReset = in.reset ? 1 : 0;
        ep.InMVScaleX = -(float)in.renderWidth;
        ep.InMVScaleY = -(float)in.renderHeight;
        ep.InFrameTimeDeltaInMsec = in.frameTimeMs;
        NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSS_EXT(in.cmd, feature_, params_, &ep);
        if (NVSDK_NGX_FAILED(r)) {
            LogOnce("DLSS: evaluate failed");
            return false;
        }
        loggedFailure_ = false;
        return true;
    }

    bool EvaluateRr(const UpscaleInputs& in) {
        NVSDK_NGX_D3D12_DLSSD_Eval_Params ep{};
        ep.pInColor = in.color;
        ep.pInOutput = in.output;
        ep.pInDepth = in.depth;
        ep.pInMotionVectors = in.velocity;
        ep.pInDiffuseAlbedo = in.diffuseAlbedo;
        ep.pInSpecularAlbedo = in.specularAlbedo;
        ep.pInRoughness = in.roughness;
        ep.pInNormals = in.normals;
        ep.InJitterOffsetX = in.jitterX;
        ep.InJitterOffsetY = in.jitterY;
        ep.InRenderSubrectDimensions = {in.renderWidth, in.renderHeight};
        ep.InReset = in.reset ? 1 : 0;
        ep.InMVScaleX = -(float)in.renderWidth;
        ep.InMVScaleY = -(float)in.renderHeight;
        ep.InFrameTimeDeltaInMsec = in.frameTimeMs;
        ep.InPreExposure = 1.0f;
        ep.InExposureScale = 1.0f;
        NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSD_EXT(in.cmd, feature_, params_, &ep);
        if (NVSDK_NGX_FAILED(r)) {
            LogOnce("DLSS: RR evaluate failed");
            return false;
        }
        loggedFailure_ = false;
        return true;
    }

    void LogOnce(const char* message) {
        if (loggedFailure_) return;
        LOG_WARN("%s", message);
        loggedFailure_ = true;
    }

    ID3D12Device* device_ = nullptr;
    NVSDK_NGX_Parameter* params_ = nullptr;
    NVSDK_NGX_Handle* feature_ = nullptr;
    uint32_t curRenderW_ = 0, curRenderH_ = 0, curOutW_ = 0, curOutH_ = 0;
    UpscalerQuality curQuality_ = UpscalerQuality::Quality;
    DlssModel curModel_ = DlssModel::Auto;
    bool available_ = false;
    bool rrAvailable_ = false;
    bool rrMode_ = false;
    bool loggedFailure_ = false;
};

} // namespace

std::unique_ptr<IUpscaler> CreateDlssUpscaler() {
    return std::make_unique<DlssUpscaler>();
}

} // namespace mmdx
