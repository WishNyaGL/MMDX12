// NVIDIA DLSS Frame Generation (DLSS-G / DLSS-MFG) via the raw NGX feature.
#include <directx/d3dx12.h>
#include "render/DlssFrameGen.h"
#include "render/Dx12Context.h"
#include "render/NgxShared.h"
#include "core/Log.h"
#include "core/TextUtil.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssg.h>
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace mmdx {

namespace {
constexpr DXGI_FORMAT kFrameFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to) {
    if (!cmd || !res || from == to) return;
    D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(res, from, to);
    cmd->ResourceBarrier(1, &b);
}
} // namespace

DlssFrameGen::~DlssFrameGen() {
    ReleaseResources();
}

bool DlssFrameGen::Initialize(Dx12Context& ctx) {
    if (initialized_) return available_;
    initialized_ = true;
    device_ = ctx.Device();
    const std::filesystem::path exeDir = ExecutableDir();
    if (ctx.Caps().vendorId != 0x10DE) {
        reason_ = "NVIDIA GPU가 아닙니다";
        LOG_INFO("DLSS-G: not an NVIDIA GPU");
        return false;
    }
    if (!std::filesystem::exists(exeDir / L"nvngx_dlssg.dll")) {
        reason_ = "nvngx_dlssg.dll이 없습니다";
        LOG_WARN("DLSS-G: nvngx_dlssg.dll not found next to the exe");
        return false;
    }
    // NGX must be initialised only once per device / app id; share the upscaler's instance.
    if (!NgxSharedInit(ctx)) {
        reason_ = "NGX 초기화에 실패했습니다";
        LOG_WARN("DLSS-G: NGX init failed");
        return false;
    }
    NVSDK_NGX_Parameter* params = nullptr;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params);
    if (NVSDK_NGX_FAILED(r) || !params) {
        reason_ = "NGX 기능 조회에 실패했습니다";
        LOG_WARN("DLSS-G: GetCapabilityParameters failed (0x%08X)", (unsigned)r);
        NgxSharedRelease();
        return false;
    }
    params_ = params;
    int ok = 0;
    params->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &ok);
    if (!ok) {
        int initResult = 0;
        params->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &initResult);
        reason_ = "이 GPU 또는 드라이버는 프레임 생성을 지원하지 않습니다";
        LOG_INFO("DLSS-G: FrameGeneration not available (init result 0x%08X)", (unsigned)initResult);
        NVSDK_NGX_D3D12_DestroyParameters((NVSDK_NGX_Parameter*)params_);
        params_ = nullptr;
        NgxSharedRelease();
        return false;
    }
    unsigned maxMulti = 1;
    params->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &maxMulti);
    maxMultiplier_ = (int)std::clamp(maxMulti, 1u, 3u) + 1;   // 1 intermediate frame = 2x
    available_ = true;
    LOG_INFO("DLSS-G: available, max multiplier %dx (MultiFrameCountMax=%u)", maxMultiplier_, maxMulti);
    return true;
}

void DlssFrameGen::Shutdown() {
    if (feature_) {
        NVSDK_NGX_D3D12_ReleaseFeature((NVSDK_NGX_Handle*)feature_);
        feature_ = nullptr;
    }
    featureW_ = featureH_ = featureRW_ = featureRH_ = 0;
    ReleaseResources();
    if (params_) {
        NVSDK_NGX_D3D12_DestroyParameters((NVSDK_NGX_Parameter*)params_);
        params_ = nullptr;
    }
    if (initialized_) NgxSharedRelease();
    initialized_ = false;
    available_ = false;
    device_ = nullptr;
    maxMultiplier_ = 0;
}

void DlssFrameGen::SetActive(Dx12Context& ctx, bool active) {
    if (active || !HasResources()) return;
    ctx.WaitForGpu();
    if (feature_) {
        NVSDK_NGX_D3D12_ReleaseFeature((NVSDK_NGX_Handle*)feature_);
        feature_ = nullptr;
    }
    featureW_ = featureH_ = featureRW_ = featureRH_ = 0;
    ReleaseResources();
}

void DlssFrameGen::ReleaseResources() {
    for (Slot& s : slots_) {
        s.finalFrame.Reset();
        s.hudless.Reset();
        for (auto& g : s.generated) g.Reset();
        s.real.Reset();
        s.finalState = D3D12_RESOURCE_STATE_COMMON;
        s.hudlessState = D3D12_RESOURCE_STATE_COMMON;
        s.realState = D3D12_RESOURCE_STATE_COMMON;
        for (auto& st : s.generatedState) st = D3D12_RESOURCE_STATE_COMMON;
    }
    width_ = height_ = 0;
    generatedCount_ = 0;
    currentSlot_ = 0;
}

bool DlssFrameGen::EnsureResources(Dx12Context& ctx, uint32_t width, uint32_t height) {
    if (width_ == width && height_ == height && slots_[0].finalFrame) return true;
    ctx.WaitForGpu();
    ReleaseResources();
    if (!device_ || width == 0 || height == 0) return false;
    D3D12_HEAP_PROPERTIES def{D3D12_HEAP_TYPE_DEFAULT};
    CD3DX12_RESOURCE_DESC td = CD3DX12_RESOURCE_DESC::Tex2D(kFrameFormat, width, height, 1, 1);
    CD3DX12_RESOURCE_DESC tdUav = td;
    tdUav.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto make = [&](const wchar_t* name, bool uav, ComPtr<ID3D12Resource>& out) {
        if (!CheckHr(device_->CreateCommittedResource(
                         &def, D3D12_HEAP_FLAG_NONE, uav ? &tdUav : &td,
                         uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON, nullptr,
                         IID_PPV_ARGS(&out)),
                     "DLSS-G: CreateCommittedResource"))
            return false;
        out->SetName(name);
        return true;
    };
    for (Slot& s : slots_) {
        if (!make(L"framegen.final", false, s.finalFrame)) return false;
        if (!make(L"framegen.hudless", false, s.hudless)) return false;
        if (!make(L"framegen.real", true, s.real)) return false;
        for (int i = 0; i < 3; ++i) {
            wchar_t name[32];
            swprintf_s(name, L"framegen.generated%d", i);
            if (!make(name, true, s.generated[i])) return false;
            s.generatedState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        }
        s.finalState = D3D12_RESOURCE_STATE_COMMON;
        s.hudlessState = D3D12_RESOURCE_STATE_COMMON;
        s.realState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    width_ = width;
    height_ = height;
    return true;
}

bool DlssFrameGen::CopyHudless(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer) {
    if (!available_ || !backbuffer || !cmd || !EnsureResources(ctx, ctx.Width(), ctx.Height())) return false;
    currentSlot_ = (int)(ctx.FrameNumber() % 2);
    Slot& s = slots_[currentSlot_];
    Transition(cmd, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmd, s.hudless.Get(), s.hudlessState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(s.hudless.Get(), backbuffer);
    Transition(cmd, s.hudless.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.hudlessState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(cmd, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

bool DlssFrameGen::CopyFinal(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer) {
    if (!available_ || !backbuffer || !cmd || !EnsureResources(ctx, ctx.Width(), ctx.Height())) return false;
    currentSlot_ = (int)(ctx.FrameNumber() % 2);
    Slot& s = slots_[currentSlot_];
    Transition(cmd, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(cmd, s.finalFrame.Get(), s.finalState, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(s.finalFrame.Get(), backbuffer);
    Transition(cmd, s.finalFrame.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.finalState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    Transition(cmd, backbuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

bool DlssFrameGen::Evaluate(Dx12Context& ctx, const FrameGenInputs& in) {
    generatedCount_ = 0;
    if (!available_ || !params_ || !in.cmd || !in.backbuffer || !in.depth || !in.velocity) return false;
    if (!EnsureResources(ctx, in.width, in.height)) return false;
    const int slotIndex = (int)(ctx.FrameNumber() % 2);
    Slot& s = slots_[slotIndex];
    currentSlot_ = slotIndex;

    const uint32_t multiplier =
        (uint32_t)std::clamp(in.multiplier, 2, std::max(2, maxMultiplier_));
    const uint32_t generated = multiplier - 1;

    if (feature_ && (featureW_ != in.width || featureH_ != in.height || featureRW_ != in.renderWidth ||
                     featureRH_ != in.renderHeight)) {
        ctx.WaitForGpu();
        NVSDK_NGX_D3D12_ReleaseFeature((NVSDK_NGX_Handle*)feature_);
        feature_ = nullptr;
    }
    if (!feature_) {
        NVSDK_NGX_DLSSG_Create_Params cp{};
        cp.Width = in.width;
        cp.Height = in.height;
        cp.NativeBackbufferFormat = (unsigned)kFrameFormat;
        cp.RenderWidth = in.renderWidth;
        cp.RenderHeight = in.renderHeight;
        cp.DynamicResolutionScaling = false;
        NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSG(in.cmd, 1, 1, (NVSDK_NGX_Handle**)&feature_,
                                                    (NVSDK_NGX_Parameter*)params_, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            feature_ = nullptr;
            if (!loggedFailure_) {
                LOG_WARN("DLSS-G: feature creation failed (0x%08X)", (unsigned)r);
                loggedFailure_ = true;
            }
            available_ = false;
            return false;
        }
        featureW_ = in.width;
        featureH_ = in.height;
        featureRW_ = in.renderWidth;
        featureRH_ = in.renderHeight;
        LOG_INFO("DLSS-G: feature created %ux%u (render %ux%u)", in.width, in.height, in.renderWidth,
                 in.renderHeight);
    }

    for (uint32_t i = 0; i < generated; ++i)
        Transition(in.cmd, s.generated[i].Get(), s.generatedState[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Transition(in.cmd, s.real.Get(), s.realState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (uint32_t i = 0; i < generated; ++i) s.generatedState[i] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    s.realState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    // Depth / motion vectors stay in the renderer's shader-readable state between frames; DLSS-G wants all inputs
    // in NON_PIXEL_SHADER_RESOURCE.
    Transition(in.cmd, in.depth, in.depthState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(in.cmd, in.velocity, in.velocityState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep{};
    ep.pBackbuffer = s.finalFrame.Get();
    ep.pHudless = s.hudless.Get();
    ep.pDepth = in.depth;
    ep.pMVecs = in.velocity;
    ep.pOutputRealFrame = s.real.Get();

    for (uint32_t i = 0; i < generated; ++i) {
        ep.pOutputInterpFrame = s.generated[i].Get();
        NVSDK_NGX_DLSSG_Opt_Eval_Params op{};
        op.multiFrameCount = generated;
        op.multiFrameIndex = i + 1;
        std::memcpy(op.cameraViewToClip, in.cameraViewToClip, sizeof(op.cameraViewToClip));
        std::memcpy(op.clipToCameraView, in.clipToCameraView, sizeof(op.clipToCameraView));
        std::memcpy(op.clipToPrevClip, in.clipToPrevClip, sizeof(op.clipToPrevClip));
        std::memcpy(op.prevClipToClip, in.prevClipToClip, sizeof(op.prevClipToClip));
        for (int k = 0; k < 3; ++k) {
            op.cameraPos[k] = in.cameraPos[k];
            op.cameraUp[k] = in.cameraUp[k];
            op.cameraRight[k] = in.cameraRight[k];
            op.cameraFwd[k] = in.cameraFwd[k];
        }
        op.cameraNear = in.nearZ;
        op.cameraFar = in.farZ;
        op.cameraFOV = in.fov;
        op.cameraAspectRatio = in.aspect;
        op.colorBuffersHDR = false;
        op.depthInverted = false;
        op.cameraMotionIncluded = true;
        op.reset = in.reset;
        op.notRenderingGameFrames = false;
        op.orthoProjection = false;
        op.motionVectorsInvalidValue = -1.0f;
        // uv(cur) - uv(prev) -> pixel offsets at the motion-vector resolution (same convention as the DLSS upscaler).
        op.mvecScale[0] = -(float)in.renderWidth;
        op.mvecScale[1] = -(float)in.renderHeight;
        op.jitterOffset[0] = in.jitterX;
        op.jitterOffset[1] = in.jitterY;
        op.menuDetectionEnabled = false;
        NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(in.cmd, (NVSDK_NGX_Handle*)feature_,
                                                      (NVSDK_NGX_Parameter*)params_, &ep, &op);
        if (NVSDK_NGX_FAILED(r)) {
            if (!loggedFailure_) {
                LOG_WARN("DLSS-G: evaluate failed (0x%08X)", (unsigned)r);
                loggedFailure_ = true;
            }
            Transition(in.cmd, in.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.depthState);
            Transition(in.cmd, in.velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.velocityState);
            available_ = false;
            return false;
        }
    }
    loggedFailure_ = false;
    generatedCount_ = (int)generated;
    // Hand the inputs back in the state the renderer tracks (they are read by pixel shaders next frame).
    Transition(in.cmd, in.depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.depthState);
    Transition(in.cmd, in.velocity, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, in.velocityState);
    // The presentation copy (Dx12Context::EndFrameFrameGen) reads the outputs as a copy source.
    for (uint32_t i = 0; i < generated; ++i)
        Transition(in.cmd, s.generated[i].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(in.cmd, s.real.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (uint32_t i = 0; i < generated; ++i) s.generatedState[i] = D3D12_RESOURCE_STATE_COPY_SOURCE;
    s.realState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return true;
}

ID3D12Resource* DlssFrameGen::GeneratedFrame(int i) const {
    if (i < 0 || i >= generatedCount_) return nullptr;
    return slots_[(size_t)currentSlot_].generated[i].Get();
}

ID3D12Resource* DlssFrameGen::RealFrame() const {
    return slots_[(size_t)currentSlot_].real.Get();
}

} // namespace mmdx
