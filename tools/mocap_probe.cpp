#include "mocap/MocapExtractor.h"
#include "mocap/MocapSolver.h"
#include "asset/ModelImport.h"
#include "asset/ImageLoader.h"
#include "asset/VideoReader.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace mmdx;
using namespace mmdx::mocap;

namespace {

struct ColorRGBA {
    uint8_t r, g, b, a;
};

void DrawPixel(std::vector<uint8_t>& rgba, int w, int h, int x, int y, ColorRGBA color) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
    rgba[idx + 0] = color.r;
    rgba[idx + 1] = color.g;
    rgba[idx + 2] = color.b;
    rgba[idx + 3] = color.a;
}

void DrawCircle(std::vector<uint8_t>& rgba, int w, int h, int cx, int cy, int radius, ColorRGBA color) {
    for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (dx * dx + dy * dy <= radius * radius) {
                DrawPixel(rgba, w, h, cx + dx, cy + dy, color);
            }
        }
    }
}

void DrawLine(std::vector<uint8_t>& rgba, int w, int h, int x0, int y0, int x1, int y1, ColorRGBA color, int thickness = 2) {
    const int dx = std::abs(x1 - x0);
    const int dy = std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    while (true) {
        if (thickness <= 1) {
            DrawPixel(rgba, w, h, x0, y0, color);
        } else {
            DrawCircle(rgba, w, h, x0, y0, thickness / 2, color);
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Usage: mocap_probe <video> [--start s] [--end s] [--fps f] [--no-face] [--no-hands] [--overlay <dir> [--every N]] [--solve <pmx> --vmd <out.vmd>]\n");
        return 1;
    }

    std::filesystem::path videoPath;
    ExtractOptions opts;
    std::filesystem::path overlayDir;
    int everyN = 15;
    std::filesystem::path solvePmx, solveVmd;   // --solve <pmx> --vmd <out>: retarget the capture and write a VMD

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--start" && i + 1 < argc) {
            opts.startSeconds = std::stod(argv[++i]);
        } else if (arg == "--end" && i + 1 < argc) {
            opts.endSeconds = std::stod(argv[++i]);
        } else if (arg == "--fps" && i + 1 < argc) {
            opts.fps = std::stod(argv[++i]);
        } else if (arg == "--no-face") {
            opts.face = false;
        } else if (arg == "--no-hands") {
            opts.hands = false;
        } else if (arg == "--overlay" && i + 1 < argc) {
            overlayDir = argv[++i];
        } else if (arg == "--solve" && i + 1 < argc) {
            solvePmx = argv[++i];
        } else if (arg == "--vmd" && i + 1 < argc) {
            solveVmd = argv[++i];
        } else if (arg == "--every" && i + 1 < argc) {
            everyN = std::max(1, std::stoi(argv[++i]));
        } else if (arg[0] != '-') {
            videoPath = arg;
        }
    }

    if (videoPath.empty()) {
        printf("Error: No video file specified\n");
        return 1;
    }

    if (!overlayDir.empty()) {
        std::filesystem::create_directories(overlayDir);
        opts.debugCallback = [&overlayDir, everyN](size_t frameIndex, double timeSec, uint32_t width, uint32_t height,
                                                  const uint8_t* bgra, const MocapFrame& frame,
                                                  const DebugFrameData& debugData) {
            if (frameIndex % everyN != 0) return;

            // Convert BGRA to RGBA
            std::vector<uint8_t> rgba(width * height * 4);
            for (size_t i = 0; i < width * height; ++i) {
                rgba[i * 4 + 0] = bgra[i * 4 + 2]; // R
                rgba[i * 4 + 1] = bgra[i * 4 + 1]; // G
                rgba[i * 4 + 2] = bgra[i * 4 + 0]; // B
                rgba[i * 4 + 3] = 255;             // A
            }

            const int W = static_cast<int>(width);
            const int H = static_cast<int>(height);

            // 1. Draw Body Skeleton
            if (frame.poseValid) {
                static const std::pair<int, int> poseBones[] = {
                    { PoseLm::Nose, PoseLm::LeftEyeInner }, { PoseLm::LeftEyeInner, PoseLm::LeftEye },
                    { PoseLm::LeftEye, PoseLm::LeftEyeOuter }, { PoseLm::LeftEyeOuter, PoseLm::LeftEar },
                    { PoseLm::Nose, PoseLm::RightEyeInner }, { PoseLm::RightEyeInner, PoseLm::RightEye },
                    { PoseLm::RightEye, PoseLm::RightEyeOuter }, { PoseLm::RightEyeOuter, PoseLm::RightEar },
                    { PoseLm::MouthLeft, PoseLm::MouthRight },
                    { PoseLm::LeftShoulder, PoseLm::RightShoulder },
                    { PoseLm::LeftShoulder, PoseLm::LeftHip }, { PoseLm::RightShoulder, PoseLm::RightHip },
                    { PoseLm::LeftHip, PoseLm::RightHip },
                    { PoseLm::LeftShoulder, PoseLm::LeftElbow }, { PoseLm::LeftElbow, PoseLm::LeftWrist },
                    { PoseLm::LeftWrist, PoseLm::LeftThumb }, { PoseLm::LeftWrist, PoseLm::LeftPinky },
                    { PoseLm::LeftWrist, PoseLm::LeftIndex }, { PoseLm::LeftPinky, PoseLm::LeftIndex },
                    { PoseLm::RightShoulder, PoseLm::RightElbow }, { PoseLm::RightElbow, PoseLm::RightWrist },
                    { PoseLm::RightWrist, PoseLm::RightThumb }, { PoseLm::RightWrist, PoseLm::RightPinky },
                    { PoseLm::RightWrist, PoseLm::RightIndex }, { PoseLm::RightPinky, PoseLm::RightIndex },
                    { PoseLm::LeftHip, PoseLm::LeftKnee }, { PoseLm::LeftKnee, PoseLm::LeftAnkle },
                    { PoseLm::LeftAnkle, PoseLm::LeftHeel }, { PoseLm::LeftAnkle, PoseLm::LeftFootIndex },
                    { PoseLm::LeftHeel, PoseLm::LeftFootIndex },
                    { PoseLm::RightHip, PoseLm::RightKnee }, { PoseLm::RightKnee, PoseLm::RightAnkle },
                    { PoseLm::RightAnkle, PoseLm::RightHeel }, { PoseLm::RightAnkle, PoseLm::RightFootIndex },
                    { PoseLm::RightHeel, PoseLm::RightFootIndex }
                };

                const ColorRGBA boneColor{ 0, 255, 255, 255 }; // Cyan
                for (const auto& bone : poseBones) {
                    if (frame.poseVisibility[bone.first] > 0.1f && frame.poseVisibility[bone.second] > 0.1f) {
                        const int x0 = static_cast<int>(frame.poseImage[bone.first].x * W);
                        const int y0 = static_cast<int>(frame.poseImage[bone.first].y * H);
                        const int x1 = static_cast<int>(frame.poseImage[bone.second].x * W);
                        const int y1 = static_cast<int>(frame.poseImage[bone.second].y * H);
                        DrawLine(rgba, W, H, x0, y0, x1, y1, boneColor, 3);
                    }
                }

                const ColorRGBA jointColor{ 255, 0, 0, 255 }; // Red
                for (int i = 0; i < kPoseLmCount; ++i) {
                    if (frame.poseVisibility[i] > 0.1f) {
                        const int x = static_cast<int>(frame.poseImage[i].x * W);
                        const int y = static_cast<int>(frame.poseImage[i].y * H);
                        DrawCircle(rgba, W, H, x, y, 4, jointColor);
                    }
                }
            }

            // 2. Draw Hands
            static const std::pair<int, int> handBones[] = {
                { 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 4 },
                { 0, 5 }, { 5, 6 }, { 6, 7 }, { 7, 8 },
                { 0, 9 }, { 9, 10 }, { 10, 11 }, { 11, 12 },
                { 0, 13 }, { 13, 14 }, { 14, 15 }, { 15, 16 },
                { 0, 17 }, { 17, 18 }, { 18, 19 }, { 19, 20 }
            };

            for (int hIdx = 0; hIdx < 2; ++hIdx) {
                if (frame.handValid[hIdx] && debugData.handImage[hIdx].size() == kHandLmCount) {
                    const ColorRGBA handColor = (hIdx == 0) ? ColorRGBA{ 255, 128, 0, 255 } : ColorRGBA{ 0, 128, 255, 255 };
                    for (const auto& bone : handBones) {
                        const int x0 = static_cast<int>(debugData.handImage[hIdx][bone.first].x);
                        const int y0 = static_cast<int>(debugData.handImage[hIdx][bone.first].y);
                        const int x1 = static_cast<int>(debugData.handImage[hIdx][bone.second].x);
                        const int y1 = static_cast<int>(debugData.handImage[hIdx][bone.second].y);
                        DrawLine(rgba, W, H, x0, y0, x1, y1, handColor, 2);
                    }
                    for (int k = 0; k < kHandLmCount; ++k) {
                        const int x = static_cast<int>(debugData.handImage[hIdx][k].x);
                        const int y = static_cast<int>(debugData.handImage[hIdx][k].y);
                        DrawCircle(rgba, W, H, x, y, 3, ColorRGBA{ 255, 255, 0, 255 });
                    }
                }
            }

            // 3. Draw Face
            if (frame.faceValid && debugData.faceImage.size() == kFaceLmCount) {
                const ColorRGBA faceColor{ 0, 255, 0, 255 }; // Green
                for (int i = 0; i < kFaceLmCount; ++i) {
                    const int x = static_cast<int>(debugData.faceImage[i].x);
                    const int y = static_cast<int>(debugData.faceImage[i].y);
                    DrawCircle(rgba, W, H, x, y, 1, faceColor);
                }
            }

            char fileName[64];
            snprintf(fileName, sizeof(fileName), "frame_%05zu.png", frameIndex);
            const std::filesystem::path outPath = overlayDir / fileName;
            SavePngRGBA8(outPath, width, height, rgba.data(), width * 4);
        };
    }

    printf("Extracting mocap from: %s\n", mmdx::PathToUtf8(videoPath).c_str());
    {
        VideoReader vr;
        if (vr.Open(videoPath)) {
            printf("Video info: %ux%u, %.2f fps, duration %.3fs, frames %llu\n",
                   vr.Width(), vr.Height(), vr.Fps(), vr.Duration(), vr.FrameCount());
        }
    }
    MocapClip clip;
    std::string error;
    if (!ExtractMocap(videoPath, opts, clip, nullptr, &error)) {
        printf("Extraction failed: %s\n", error.c_str());
        return 1;
    }

    struct SecStats {
        size_t total = 0;
        size_t pose = 0;
        size_t handL = 0;
        size_t handR = 0;
        size_t face = 0;
    };
    std::map<int, SecStats> secStats;
    for (const auto& f : clip.frames) {
        int sec = static_cast<int>(std::floor(f.time));
        auto& s = secStats[sec];
        s.total++;
        if (f.poseValid) s.pose++;
        if (f.handValid[0]) s.handL++;
        if (f.handValid[1]) s.handR++;
        if (f.faceValid) s.face++;
    }

    printf("\nPer-second validity:\n");
    for (const auto& [sec, s] : secStats) {
        const double pPose = s.total ? (100.0 * s.pose / s.total) : 0.0;
        const double pHandL = s.total ? (100.0 * s.handL / s.total) : 0.0;
        const double pHandR = s.total ? (100.0 * s.handR / s.total) : 0.0;
        const double pFace = s.total ? (100.0 * s.face / s.total) : 0.0;
        printf("  sec %3d: pose %2zu/%2zu (%5.1f%%), hands L %2zu/%2zu (%5.1f%%) R %2zu/%2zu (%5.1f%%), face %2zu/%2zu (%5.1f%%)\n",
               sec, s.pose, s.total, pPose, s.handL, s.total, pHandL, s.handR, s.total, pHandR, s.face, s.total, pFace);
    }

    size_t totalPose = 0, totalHandL = 0, totalHandR = 0, totalFace = 0;
    for (const auto& f : clip.frames) {
        if (f.poseValid) totalPose++;
        if (f.handValid[0]) totalHandL++;
        if (f.handValid[1]) totalHandR++;
        if (f.faceValid) totalFace++;
    }
    const size_t total = clip.frames.size();
    printf("\nTotal Summary (%zu frames):\n", total);
    printf("  Pose:  %zu / %zu (%5.1f%%)\n", totalPose, total, total ? (100.0 * totalPose / total) : 0.0);
    printf("  Hands: Left %zu / %zu (%5.1f%%), Right %zu / %zu (%5.1f%%)\n",
           totalHandL, total, total ? (100.0 * totalHandL / total) : 0.0,
           totalHandR, total, total ? (100.0 * totalHandR / total) : 0.0);
    printf("  Face:  %zu / %zu (%5.1f%%)\n", totalFace, total, total ? (100.0 * totalFace / total) : 0.0);

    if (!solvePmx.empty()) {
        PmxModel pmx;
        if (!LoadModelFile(solvePmx, ModelRole::Character, pmx, &error)) {
            printf("model: %s\n", error.c_str());
            return 1;
        }
        VmdMotion vmd;
        SolveReport rep;
        if (!SolveMotion(clip, pmx, SolveOptions{}, vmd, &rep, &error)) {
            printf("solve failed: %s\n", error.c_str());
            return 1;
        }
        printf("Solve: %d frames, %d bone keys, %d morph keys, %zu missing bones, %zu missing morphs\n",
               rep.frames, rep.boneKeys, rep.morphKeys, rep.missingBones.size(), rep.missingMorphs.size());
        if (!solveVmd.empty()) {
            if (!SaveVmd(solveVmd, vmd, &error)) {
                printf("vmd: %s\n", error.c_str());
                return 1;
            }
            printf("VMD written: %s\n", mmdx::PathToUtf8(solveVmd).c_str());
        }
    }

    return 0;
}
