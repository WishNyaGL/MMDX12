#pragma once
// The MMD background AVI (背景AVI) feeding `screen.bmp` materials: one renderer-owned texture holds the current
// video frame, and every model material whose diffuse texture file name is `screen.bmp` shows it (MMD stages put
// video on in-scene monitors / LED walls this way). The background layer with `drawBackground` also draws full
// screen (PassVideo); this class only decodes and binds the texture.
//
// The texture is decoded on a worker into a small CPU ring in real time (like PassVideo); an offline render sets
// FrameView::videoExact so the frame for the rendered time is decoded synchronously and exactly.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_map>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace mmdx {

class Dx12Context;
struct FrameView;

class ScreenVideo {
public:
    ScreenVideo();
    ~ScreenVideo();
    ScreenVideo(const ScreenVideo&) = delete;
    ScreenVideo& operator=(const ScreenVideo&) = delete;

    // Decodes the frame for this view's screen layer (if any) into the shared texture and binds it to every model
    // with screen materials. Must be called on the command list before any pass that draws models.
    void Update(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, const FrameView& view);
    // Releases the decode worker, staging buffers and the texture (scene end / unload).
    void Clear(Dx12Context& ctx);
    // The model left the drawn set: drop its bind state so a recreated model rebinds cleanly.
    void ForgetModel(uint64_t uid);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
