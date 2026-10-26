#pragma once
// One video file decoded into one renderer-owned texture (the per-file decoder render/ScreenVideo.h used to own).
//
// The texture is decoded on a worker into a small CPU ring in real time (like PassVideo); an exact update decodes
// the frame for the rendered time synchronously instead (offline renders / video exports). Every video consumer
// (the background AVI's screen.bmp path, an MMDX stage video screen) owns one of these and loops the time by the
// clip's duration, so the callers only hand in the time.
#include <cstdint>
#include <filesystem>
#include <memory>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace mmdx {

class Dx12Context;

class VideoTexture {
public:
    VideoTexture();
    ~VideoTexture();
    VideoTexture(const VideoTexture&) = delete;
    VideoTexture& operator=(const VideoTexture&) = delete;
    VideoTexture(VideoTexture&&) noexcept;
    VideoTexture& operator=(VideoTexture&&) noexcept;

    // Opens `p` (its own texture, staging ring and decode worker). False + one warning line when the file cannot be
    // opened; the current video (texture and bindings) stays untouched. A file already open is a no-op true.
    bool Open(Dx12Context& ctx, const std::filesystem::path& p);
    // Drops the decoder, staging buffers and the texture (scene end / screen switched off).
    void Release(Dx12Context& ctx);

    bool IsOpen() const;
    bool HasTexture() const;                 // a texture exists (a video may be open before its first frame lands)
    ID3D12Resource* TextureResource() const;
    const std::filesystem::path& Path() const;
    uint32_t Width() const;
    uint32_t Height() const;
    double Duration() const;                 // seconds (0 = unknown: the time is not looped)

    // Shows the frame for `t` (seconds): loops it by the duration, then either decodes exactly (`exact`, offline) or
    // uploads the newest ready ring frame. `slot` is the frame-in-flight slot whose staging buffer uploads. False
    // when no texture is available (the caller keeps the previous binding).
    bool Update(Dx12Context& ctx, ID3D12GraphicsCommandList* cmd, uint32_t slot, double t, bool exact, bool seek);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mmdx
