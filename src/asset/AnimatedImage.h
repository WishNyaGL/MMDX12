#pragma once
// M7 fix 4: animated image decoder for MME's ANIMATEDTEXTURE semantic (spec draft §4.11). Decodes an animated
// GIF or an APNG (a PNG extension: acTL / fcTL / fdAT chunks on top of the base PNG) into frames + per-frame
// delays. Self-contained (no WIC COM): a small zlib/DEFLATE inflater drives the PNG path, an LZW decoder the GIF
// path, so the decoder is unit-testable without a GPU or a window.
//
// The runtime picks the frame for a time with FrameAt(): ANIMATEDTEXTURE chooses by TIME (scene clock) + Offset /
// Speed; SeekVariable drives the seek directly (the caller maps the variable to seconds).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mmdx {

// One decoded frame, tightly packed RGBA8 at the animation's full canvas size.
struct AnimImageFrame {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;   // width * height * 4
    float delay = 0.1f;          // seconds this frame is shown (>= 0)
};

class AnimatedImage {
public:
    bool Load(const std::filesystem::path& path, std::string* error = nullptr);
    bool LoadFromMemory(const uint8_t* bytes, size_t size, std::string* error = nullptr);

    bool Empty() const { return frames_.empty(); }
    size_t FrameCount() const { return frames_.size(); }
    const AnimImageFrame& Frame(size_t i) const { return frames_[i < frames_.size() ? i : 0]; }
    uint32_t Width() const { return frames_.empty() ? 0 : frames_[0].width; }
    uint32_t Height() const { return frames_.empty() ? 0 : frames_[0].height; }
    float TotalDuration() const;   // sum of delays (0 when empty)

    // The frame shown at `timeSeconds` (wraps around TotalDuration; a single frame always returns frame 0).
    size_t FrameAt(float timeSeconds) const;

    void Clear() { frames_.clear(); }

private:
    std::vector<AnimImageFrame> frames_;
};

// M7 fix 5: packs up to `maxTexels` mip-0 texels of a tightly packed RGBA8 image into a float4 array (each
// component / 255, alpha kept). Returns the number of float4s written. Shared by the MME TEXTUREVALUE binder and
// its self test (a plain CPU helper, no GPU).
uint32_t PackTexelsToFloat4(const uint8_t* rgba, uint32_t width, uint32_t height, float* out, uint32_t maxTexels);

} // namespace mmdx
