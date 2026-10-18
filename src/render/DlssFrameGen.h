#pragma once
// DLSS Frame Generation (DLSS-G / DLSS-MFG) through the raw NGX DLSS-G feature (nvngx_dlssg.dll).
// The renderer supplies the final (UI) and hudless back-buffer copies, depth, motion vectors and camera data;
// this class owns the feature handle, the per-frame input/output textures and the evaluation. Presentation
// pacing stays with Dx12Context::EndFrame (see FrameGenPresentFrames there).
#include <directx/d3d12.h>
#include <wrl/client.h>
#include <cstdint>

namespace mmdx {

class Dx12Context;
using Microsoft::WRL::ComPtr;

// Everything DLSS-G needs for one rendered frame, already converted to the SDK's conventions.
struct FrameGenInputs {
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* backbuffer = nullptr;   // final frame with UI (window size, R8G8B8A8_UNORM)
    ID3D12Resource* hudless = nullptr;      // the same frame without UI
    ID3D12Resource* depth = nullptr;        // render resolution, raw device depth (0 near .. 1 far)
    ID3D12Resource* velocity = nullptr;     // render resolution, uv(cur) - uv(prev)
    D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES velocityState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t width = 0, height = 0;         // output / feature size
    uint32_t renderWidth = 0, renderHeight = 0;
    int multiplier = 0;                     // 2 / 3 / 4 (1 generated frame .. 3 generated frames)
    bool reset = false;                     // camera cut / seek / scene change
    float frameTimeMs = 16.7f;
    float jitterX = 0, jitterY = 0;         // jitter of this frame in render pixels (the DLSS-SR convention)
    // Row-major, post-multiplication (DirectX): v_clip = v * matrix.
    float cameraViewToClip[4][4] = {};
    float clipToCameraView[4][4] = {};
    float clipToPrevClip[4][4] = {};
    float prevClipToClip[4][4] = {};
    float cameraPos[3] = {}, cameraUp[3] = {}, cameraRight[3] = {}, cameraFwd[3] = {};
    float nearZ = 0.5f, farZ = 3000.0f, fov = 0.5f, aspect = 1.0f;
};

class DlssFrameGen {
public:
    DlssFrameGen() = default;
    ~DlssFrameGen();
    DlssFrameGen(const DlssFrameGen&) = delete;
    DlssFrameGen& operator=(const DlssFrameGen&) = delete;

    // NGX init (shared with the DLSS upscaler) + capability check. Never throws; a missing DLL or an
    // unsupported GPU just leaves it unavailable with a reason string.
    bool Initialize(Dx12Context& ctx);
    // Releases feature + textures. The GPU must be idle (Renderer waits).
    void Shutdown();

    bool Available() const { return available_; }
    int MaxMultiplier() const { return maxMultiplier_; }
    const char* Reason() const { return reason_; }

    // Copies the current back buffer (hudless: before ImGui; final: after ImGui) into this frame's slot.
    bool CopyHudless(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer);
    bool CopyFinal(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, ID3D12Resource* backbuffer);

    // Creates/recreates the feature as needed, then records one Evaluate per generated frame.
    bool Evaluate(Dx12Context& ctx, const FrameGenInputs& in);

    // True once the feature instance / its textures are allocated (so turning frame generation off can free them).
    bool HasResources() const { return feature_ != nullptr || width_ != 0; }
    // Releases the feature and its textures when frame generation is off (waits for the GPU). Cheap no-op otherwise.
    void SetActive(Dx12Context& ctx, bool active);

    int GeneratedCount() const { return generatedCount_; }
    ID3D12Resource* GeneratedFrame(int i) const;
    ID3D12Resource* RealFrame() const;

private:
    struct Slot {
        ComPtr<ID3D12Resource> finalFrame;
        ComPtr<ID3D12Resource> hudless;
        ComPtr<ID3D12Resource> generated[3];
        ComPtr<ID3D12Resource> real;
        D3D12_RESOURCE_STATES finalState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_COMMON;
        D3D12_RESOURCE_STATES generatedState[3] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON,
                                                   D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES realState = D3D12_RESOURCE_STATE_COMMON;
    };
    bool EnsureResources(Dx12Context& ctx, uint32_t width, uint32_t height);
    void ReleaseResources();
    void ReleaseFeature(Dx12Context& ctx);
    ID3D12Device* device_ = nullptr;
    void* params_ = nullptr;    // NVSDK_NGX_Parameter*
    void* feature_ = nullptr;   // NVSDK_NGX_Handle*
    uint32_t featureW_ = 0, featureH_ = 0, featureRW_ = 0, featureRH_ = 0;
    uint32_t width_ = 0, height_ = 0;
    Slot slots_[2];
    int currentSlot_ = 0;
    int generatedCount_ = 0;
    bool available_ = false;
    bool initialized_ = false;
    int maxMultiplier_ = 0;
    bool loggedFailure_ = false;
    const char* reason_ = "";
};

} // namespace mmdx
