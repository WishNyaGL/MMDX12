#pragma once
#include "MocapTypes.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mmdx::mocap {

struct DebugFrameData {
    // 21 image-space coordinates for left (0) and right (1) hands in pixels
    std::vector<DirectX::XMFLOAT2> handImage[2];
    // 478 image-space coordinates for face in pixels
    std::vector<DirectX::XMFLOAT2> faceImage;
};

using ExtractProgress = std::function<bool(float fraction)>;   // return false = cancel
using FrameDebugCallback = std::function<void(size_t frameIndex, double timeSec, uint32_t width, uint32_t height,
                                             const uint8_t* bgra, const MocapFrame& frame,
                                             const DebugFrameData& debugData)>;

struct ExtractOptions {
    double startSeconds = 0;
    double endSeconds = -1; // -1 = to the end
    double fps = 30;
    bool hands = true;
    bool face = true;
    FrameDebugCallback debugCallback{};
};

bool ExtractMocap(const std::filesystem::path& video, const ExtractOptions& opts, MocapClip& out,
                  const ExtractProgress& progress = {}, std::string* error = nullptr);

} // namespace mmdx::mocap
