#pragma once
// S5 stage screens (docs/stage_format.md `screens`): one renderer-owned texture per LED / video surface, shown as
// emission on every stage material the screen claims (GpuModel::BindMaterialTexture + MatFlag_Screen).
//
// Sources:
//   none    - black (the LED is off).
//   image   - decoded once, uploaded into the screen texture (capped at 2048).
//   video   - one decoder per video screen (render/VideoTexture.h), looped by the clip's duration; an offline render
//             decodes the exact frame for the rendered time (FrameView::videoExact).
//   camera  - a FEED: the renderer rasterizes the scene from that camera into the screen texture before the main scene
//             (the callback below); a screen shown inside a feed keeps its previous content (no recursion). At most
//             kMaxFeedsPerFrame feeds re-render per frame (round robin beyond that).
//   program - S6: a bundled screen pack (pack.json "type": "screen", screen.hlsl -> PackScreen) draws the content into
//             the screen texture with a fullscreen PSO (DXC ps_6_0, MMDX_PACK = the pack's screen.hlsl), once per frame
//             before the scene passes. The pack reads the song / timeline time, so offline renders are exact. The
//             pack's parameters are the manifest's `packs[].params` with the packs.<index>.params.<key> targets.
//
// Zero screens = zero cost: nothing is created and nothing is bound. Textures are created / freed on change only.
#include "render/RenderPass.h"
#include "render/RenderTypes.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

struct ID3D12GraphicsCommandList;

namespace mmdx {

class Dx12Context;

// What the screen update needs from the frame it runs in (the copy pass binds pc.sceneConstants as b0).
class StageScreens {
public:
    // Feeds re-rendered per frame (round robin beyond that).
    static constexpr uint32_t kMaxFeedsPerFrame = 2;
    // Renders one camera feed into `target` (the screen's own RGBA8 texture, ALLOW_RENDER_TARGET) and returns true when
    // it drew something; false keeps the screen's previous content. The renderer owns the scene passes and the
    // constant-buffer / light-buffer slots of the feed (never RenderToImage's kOffscreenSlot).
    using FeedRender = std::function<bool(uint32_t feedIndex, const ScreenView& screen, Texture& target)>;

    StageScreens();
    ~StageScreens();
    StageScreens(const StageScreens&) = delete;
    StageScreens& operator=(const StageScreens&) = delete;

    // The stage screen shaders live in `shaderDir` (nothing is compiled until a camera feed first runs).
    void Init(const std::filesystem::path& shaderDir) { shaderDir_ = shaderDir; }

    // One frame, before the scene passes: reconciles the textures with `view.screens` (create / free / decode /
    // upload), renders the camera feeds through `renderFeed`, then binds every screen's texture to its materials.
    void Render(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, const FrameView& view, const FeedRender& renderFeed);
    // Scene end / shutdown: frees every texture and video decoder.
    void Clear(Dx12Context& ctx);
    // The model left the drawn set: drop its bind state so a recreated model rebinds cleanly.
    void ForgetModel(uint64_t uid);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::filesystem::path shaderDir_;
};

} // namespace mmdx
