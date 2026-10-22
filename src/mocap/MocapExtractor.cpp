#include "MocapExtractor.h"
#include "OrtRuntime.h"
#include "asset/VideoReader.h"
#include "core/Log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace mmdx::mocap {

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.f * kPi;

constexpr int kBlendshapeInputLandmarks = 146;
// The 146 face mesh landmarks the blendshape model reads (MediaPipe face_blendshapes graph; the table as in
// QtMeshEditor src/Mocap/FaceCapGeom.cpp, MIT, Copyright (c) 2012-2023 QtMeshEditor).
constexpr std::array<int, kBlendshapeInputLandmarks> kBlendshapeLandmarkSubset = {
    0, 1, 4, 5, 6, 7, 8, 10, 13, 14, 17, 21, 33, 37, 39,
    40, 46, 52, 53, 54, 55, 58, 61, 63, 65, 66, 67, 70, 78, 80,
    81, 82, 84, 87, 88, 91, 93, 95, 103, 105, 107, 109, 127, 132, 133,
    136, 144, 145, 146, 148, 149, 150, 152, 153, 154, 155, 157, 158, 159, 160,
    161, 162, 163, 168, 172, 173, 176, 178, 181, 185, 191, 195, 197, 234, 246,
    249, 251, 263, 267, 269, 270, 276, 282, 283, 284, 285, 288, 291, 293, 295,
    296, 297, 300, 308, 310, 311, 312, 314, 317, 318, 321, 323, 324, 332, 334,
    336, 338, 356, 361, 362, 365, 373, 374, 375, 377, 378, 379, 380, 381, 382,
    384, 385, 386, 387, 388, 389, 390, 397, 398, 400, 402, 405, 409, 415, 454,
    466, 468, 469, 470, 471, 472, 473, 474, 475, 476, 477,
};

struct RotatedRect {
    float cx = 0.f;
    float cy = 0.f;
    float w = 0.f;
    float h = 0.f;
    float angle = 0.f; // radians
};

struct Detection {
    float score = 0.f;
    std::array<float, 4> box{}; // cx - w/2, cy - h/2, w, h
    std::vector<std::array<float, 2>> keypoints;
};

struct Letterbox {
    float padX = 0.f;
    float padY = 0.f;
    float fracX = 1.f;
    float fracY = 1.f;
};

inline float Sigmoid(float x) {
    x = std::clamp(x, -50.f, 50.f);
    return x >= 0.f ? 1.f / (1.f + std::exp(-x))
                    : std::exp(x) / (1.f + std::exp(x));
}

inline float NormalizeAngle(float a) {
    return a - kTwoPi * std::floor((a + kPi) / kTwoPi);
}

inline float IoU(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    const float x1 = std::max(a[0], b[0]);
    const float y1 = std::max(a[1], b[1]);
    const float x2 = std::min(a[0] + a[2], b[0] + b[2]);
    const float y2 = std::min(a[1] + a[3], b[1] + b[3]);
    const float inter = std::max(0.f, x2 - x1) * std::max(0.f, y2 - y1);
    const float uni = a[2] * a[3] + b[2] * b[3] - inter;
    return uni > 0.f ? inter / uni : 0.f;
}

inline void SampleBilinearBGRA(const uint8_t* bgra, int w, int h, float x, float y, float rgb[3]) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(x0);
    const float fy = y - static_cast<float>(y0);
    float acc[3] = { 0.f, 0.f, 0.f };
    for (int dy = 0; dy < 2; ++dy) {
        const int yy = y0 + dy;
        if (yy < 0 || yy >= h) continue;
        const float wy = dy ? fy : (1.f - fy);
        const uint8_t* row = bgra + yy * (w * 4);
        for (int dx = 0; dx < 2; ++dx) {
            const int xx = x0 + dx;
            if (xx < 0 || xx >= w) continue;
            const float wgt = wy * (dx ? fx : (1.f - fx));
            const uint8_t* px = row + xx * 4;
            // BGRA in memory: px[0] = Blue, px[1] = Green, px[2] = Red
            acc[0] += wgt * px[2]; // R
            acc[1] += wgt * px[1]; // G
            acc[2] += wgt * px[0]; // B
        }
    }
    rgb[0] = acc[0];
    rgb[1] = acc[1];
    rgb[2] = acc[2];
}

Letterbox LetterboxToTensor(const uint8_t* bgra, int w, int h, int size,
                            float lo, float hi, float* out, bool nchw)
{
    Letterbox lb;
    if (w <= 0 || h <= 0) return lb;
    const float s = static_cast<float>(size) / std::max(w, h);
    const float ox = (size - w * s) * 0.5f;
    const float oy = (size - h * s) * 0.5f;
    lb.padX = ox / size;
    lb.padY = oy / size;
    lb.fracX = (w * s) / size;
    lb.fracY = (h * s) / size;
    const float scale = (hi - lo) / 255.f;
    const float invS = 1.0f / s;
    const float originX = -ox * invS;
    const float originY = -oy * invS;

    if (!nchw) {
        float* dst = out;
        for (int v = 0; v < size; ++v) {
            for (int u = 0; u < size; ++u) {
                const float sx = originX + u * invS;
                const float sy = originY + v * invS;
                float rgb[3];
                SampleBilinearBGRA(bgra, w, h, sx, sy, rgb);
                *dst++ = rgb[0] * scale + lo;
                *dst++ = rgb[1] * scale + lo;
                *dst++ = rgb[2] * scale + lo;
            }
        }
    } else {
        const int plane = size * size;
        for (int v = 0; v < size; ++v) {
            for (int u = 0; u < size; ++u) {
                const float sx = originX + u * invS;
                const float sy = originY + v * invS;
                float rgb[3];
                SampleBilinearBGRA(bgra, w, h, sx, sy, rgb);
                const int idx = v * size + u;
                out[idx] = rgb[0] * scale + lo;
                out[plane + idx] = rgb[1] * scale + lo;
                out[plane * 2 + idx] = rgb[2] * scale + lo;
            }
        }
    }
    return lb;
}

void CropRotatedRectToTensor(const uint8_t* bgra, int w, int h, const RotatedRect& rect,
                             int size, float lo, float hi, float* out)
{
    const float ca = std::cos(rect.angle);
    const float sa = std::sin(rect.angle);
    const float sx = rect.w / static_cast<float>(size);
    const float sy = rect.h / static_cast<float>(size);
    const float originX = rect.cx + ca * (-rect.w * 0.5f) - sa * (-rect.h * 0.5f);
    const float originY = rect.cy + sa * (-rect.w * 0.5f) + ca * (-rect.h * 0.5f);
    const float dux = ca * sx;
    const float duy = sa * sx;
    const float dvx = -sa * sy;
    const float dvy = ca * sy;
    const float scale = (hi - lo) / 255.f;

    float* dst = out;
    for (int v = 0; v < size; ++v) {
        for (int u = 0; u < size; ++u) {
            const float sx_val = originX + u * dux + v * dvx;
            const float sy_val = originY + u * duy + v * dvy;
            float rgb[3];
            SampleBilinearBGRA(bgra, w, h, sx_val, sy_val, rgb);
            *dst++ = rgb[0] * scale + lo;
            *dst++ = rgb[1] * scale + lo;
            *dst++ = rgb[2] * scale + lo;
        }
    }
}

inline void Unletterbox(const Letterbox& lb, float& x, float& y) {
    x = (x - lb.padX) / lb.fracX;
    y = (y - lb.padY) / lb.fracY;
}

inline void ProjectLandmarks(float* pts, int count, int stride, const RotatedRect& rect) {
    const float ca = std::cos(rect.angle);
    const float sa = std::sin(rect.angle);
    for (int i = 0; i < count; ++i) {
        float* p = pts + i * stride;
        const float x = p[0] - 0.5f;
        const float y = p[1] - 0.5f;
        p[0] = rect.cx + x * rect.w * ca - y * rect.h * sa;
        p[1] = rect.cy + x * rect.w * sa + y * rect.h * ca;
        if (stride > 2) {
            p[2] *= rect.w;
        }
    }
}

std::vector<std::array<float, 2>> GenSsdAnchors(int inputSize, const std::vector<int>& strides) {
    std::vector<std::array<float, 2>> anchors;
    const int n = static_cast<int>(strides.size());
    int layer = 0;
    while (layer < n) {
        int last = layer;
        int scalesInLayer = 0;
        while (last < n && strides[last] == strides[layer]) {
            scalesInLayer += 2;
            ++last;
        }
        const int stride = strides[layer];
        const int fm = (inputSize + stride - 1) / stride;
        for (int y = 0; y < fm; ++y) {
            for (int x = 0; x < fm; ++x) {
                for (int k = 0; k < scalesInLayer; ++k) {
                    anchors.push_back({ (x + 0.5f) / static_cast<float>(fm),
                                       (y + 0.5f) / static_cast<float>(fm) });
                }
            }
        }
        layer = last;
    }
    return anchors;
}

std::vector<Detection> DecodeDetections(
    const float* rawBoxes, const float* rawScores,
    const std::vector<std::array<float, 2>>& anchors, int inputSize,
    int numKeypoints, float minScore, float iouThreshold,
    bool reverseOutputOrder = false)
{
    const int boxDim = 4 + 2 * numKeypoints;
    std::vector<Detection> candidates;
    for (size_t i = 0; i < anchors.size(); ++i) {
        const float score = Sigmoid(rawScores[i]);
        if (score < minScore) continue;
        const float* box = rawBoxes + i * boxDim;
        Detection d;
        d.score = score;
        const float cx = (reverseOutputOrder ? box[1] : box[0]) / static_cast<float>(inputSize) + anchors[i][0];
        const float cy = (reverseOutputOrder ? box[0] : box[1]) / static_cast<float>(inputSize) + anchors[i][1];
        const float w = (reverseOutputOrder ? box[3] : box[2]) / static_cast<float>(inputSize);
        const float h = (reverseOutputOrder ? box[2] : box[3]) / static_cast<float>(inputSize);
        d.box = { cx - w * 0.5f, cy - h * 0.5f, w, h };
        d.keypoints.reserve(numKeypoints);
        for (int k = 0; k < numKeypoints; ++k) {
            const float kx = (reverseOutputOrder ? box[5 + 2 * k] : box[4 + 2 * k]) / static_cast<float>(inputSize) + anchors[i][0];
            const float ky = (reverseOutputOrder ? box[4 + 2 * k] : box[5 + 2 * k]) / static_cast<float>(inputSize) + anchors[i][1];
            d.keypoints.push_back({ kx, ky });
        }
        candidates.push_back(std::move(d));
    }

    std::sort(candidates.begin(), candidates.end(), [](const Detection& a, const Detection& b) {
        return a.score > b.score;
    });

    std::vector<Detection> out;
    std::vector<bool> used(candidates.size(), false);
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (used[i]) continue;
        std::vector<size_t> cluster{ i };
        for (size_t j = i + 1; j < candidates.size(); ++j) {
            if (!used[j] && IoU(candidates[i].box, candidates[j].box) > iouThreshold) {
                cluster.push_back(j);
            }
        }
        float total = 0.f;
        Detection merged = candidates[i];
        std::fill(merged.box.begin(), merged.box.end(), 0.f);
        for (auto& kp : merged.keypoints) kp = { 0.f, 0.f };
        for (size_t j : cluster) {
            used[j] = true;
            total += candidates[j].score;
        }
        for (size_t j : cluster) {
            const float weight = candidates[j].score / total;
            for (int k = 0; k < 4; ++k) merged.box[k] += weight * candidates[j].box[k];
            for (size_t k = 0; k < merged.keypoints.size(); ++k) {
                merged.keypoints[k][0] += weight * candidates[j].keypoints[k][0];
                merged.keypoints[k][1] += weight * candidates[j].keypoints[k][1];
            }
        }
        out.push_back(std::move(merged));
    }
    return out;
}

RotatedRect RectFromPoseDetection(const Detection& det, int imgW, int imgH) {
    RotatedRect r;
    r.cx = det.keypoints[0][0] * imgW;
    r.cy = det.keypoints[0][1] * imgH;
    const float dx = det.keypoints[1][0] * imgW - r.cx;
    const float dy = det.keypoints[1][1] * imgH - r.cy;
    const float radius = std::sqrt(dx * dx + dy * dy);
    r.angle = NormalizeAngle(kPi * 0.5f - std::atan2(-dy, dx));
    r.w = r.h = 2.f * radius * 1.25f;
    return r;
}

[[maybe_unused]] RotatedRect RectFromPoseHand(float wristX, float wristY, float indexX, float indexY, float pinkyX, float pinkyY) {
    RotatedRect r;
    const float mx = 0.5f * (indexX + pinkyX);
    const float my = 0.5f * (indexY + pinkyY);
    const float dx = mx - wristX;
    const float dy = my - wristY;
    const float palm = std::sqrt(dx * dx + dy * dy);
    if (palm < 1.f) return r;
    r.cx = wristX + dx * 0.5f;
    r.cy = wristY + dy * 0.5f;
    r.angle = NormalizeAngle(kPi * 0.5f - std::atan2(-dy, dx));
    r.w = r.h = std::max(palm * 3.0f, 24.f);
    return r;
}

// MediaPipe HandLandmarksToRect + RectTransformation(scale 2.0, shift_y -0.1, square long): rotation from the wrist
// to the middle finger MCP (index / ring MCPs averaged in), box over the palm and lower finger joints.
RotatedRect RectFromHandLandmarks(const float* pts /* 21 x (x, y) pixels */) {
    RotatedRect r;
    const float x0 = pts[0], y0 = pts[1];
    const float x1 = (pts[5 * 2] + pts[13 * 2]) * 0.25f + pts[9 * 2] * 0.5f;
    const float y1 = (pts[5 * 2 + 1] + pts[13 * 2 + 1]) * 0.25f + pts[9 * 2 + 1] * 0.5f;
    r.angle = NormalizeAngle(kPi * 0.5f - std::atan2(-(y1 - y0), x1 - x0));
    const float ca = std::cos(r.angle), sa = std::sin(r.angle);
    static const int kIdx[] = { 0, 1, 2, 3, 5, 6, 9, 10, 13, 14, 17, 18 };
    // box in the crop frame (u = crop x axis (ca, sa), v = crop y axis (-sa, ca))
    float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;
    for (int i : kIdx) {
        const float u = pts[i * 2] * ca + pts[i * 2 + 1] * sa;
        const float v = -pts[i * 2] * sa + pts[i * 2 + 1] * ca;
        minU = std::min(minU, u); maxU = std::max(maxU, u);
        minV = std::min(minV, v); maxV = std::max(maxV, v);
    }
    const float side = std::max(maxU - minU, maxV - minV) * 2.0f;
    const float cu = 0.5f * (minU + maxU);
    const float cv = 0.5f * (minV + maxV) - 0.1f * side;   // shift_y -0.1 (towards the fingers)
    r.cx = cu * ca - cv * sa;
    r.cy = cu * sa + cv * ca;
    r.w = r.h = std::max(side, 24.f);
    return r;
}

RotatedRect RectFromPalmDetection(const Detection& det, int imgW, int imgH) {
    RotatedRect r;
    if (det.keypoints.size() < 3 || imgW <= 0 || imgH <= 0) return r;
    r.cx = (det.box[0] + det.box[2] * 0.5f) * imgW;
    r.cy = (det.box[1] + det.box[3] * 0.5f) * imgH;
    const float dx = (det.keypoints[2][0] - det.keypoints[0][0]) * imgW;
    const float dy = (det.keypoints[2][1] - det.keypoints[0][1]) * imgH;
    r.angle = NormalizeAngle(kPi * 0.5f - std::atan2(-dy, dx));
    const float side = std::max(det.box[2] * imgW, det.box[3] * imgH) * 2.6f;
    r.w = r.h = std::max(side, 24.f);
    const float ca = std::cos(r.angle);
    const float sa = std::sin(r.angle);
    r.cx += 0.5f * r.h * sa;
    r.cy += -0.5f * r.h * ca;
    return r;
}

RotatedRect RectFromFaceLandmarks(const float* landmarksXyz, int count, int imgW, int imgH) {
    RotatedRect r;
    if (count < 264) return r;
    float minX = landmarksXyz[0], maxX = landmarksXyz[0];
    float minY = landmarksXyz[1], maxY = landmarksXyz[1];
    for (int i = 1; i < count; ++i) {
        minX = std::min(minX, landmarksXyz[i * 3 + 0]);
        maxX = std::max(maxX, landmarksXyz[i * 3 + 0]);
        minY = std::min(minY, landmarksXyz[i * 3 + 1]);
        maxY = std::max(maxY, landmarksXyz[i * 3 + 1]);
    }
    r.cx = (minX + maxX) * 0.5f;
    r.cy = (minY + maxY) * 0.5f;
    // Eye outer corners: 33 (right) and 263 (left)
    const float dx = landmarksXyz[263 * 3 + 0] - landmarksXyz[33 * 3 + 0];
    const float dy = landmarksXyz[263 * 3 + 1] - landmarksXyz[33 * 3 + 1];
    r.angle = NormalizeAngle(-std::atan2(-dy, dx));
    const float side = std::max(maxX - minX, maxY - minY) * 1.5f;
    r.w = r.h = std::max(side, 32.f);
    (void)imgW; (void)imgH;
    return r;
}

RotatedRect RectFromPoseFace(const DirectX::XMFLOAT2* poseImage, int imgW, int imgH) {
    RotatedRect r;
    const float lx = poseImage[PoseLm::LeftEyeOuter].x * imgW;
    const float ly = poseImage[PoseLm::LeftEyeOuter].y * imgH;
    const float rx = poseImage[PoseLm::RightEyeOuter].x * imgW;
    const float ry = poseImage[PoseLm::RightEyeOuter].y * imgH;
    const float dx = lx - rx;
    const float dy = ly - ry;
    const float eyeDist = std::sqrt(dx * dx + dy * dy);
    r.angle = NormalizeAngle(-std::atan2(-dy, dx));
    const float nx = poseImage[PoseLm::Nose].x * imgW;
    const float ny = poseImage[PoseLm::Nose].y * imgH;
    const float my = (ly + ry) * 0.5f;
    r.cx = nx;
    r.cy = (ny + my) * 0.5f;
    r.w = r.h = std::max(eyeDist * 3.5f, 48.f);
    return r;
}

std::vector<Ort::Value> RunOrtSession(Ort::Session& session, Ort::Value input, Ort::AllocatorWithDefaultOptions& alloc) {
    Ort::AllocatedStringPtr inName = session.GetInputNameAllocated(0, alloc);
    const size_t nOut = session.GetOutputCount();
    std::vector<Ort::AllocatedStringPtr> outHolders;
    std::vector<const char*> outNames;
    outHolders.reserve(nOut);
    outNames.reserve(nOut);
    for (size_t i = 0; i < nOut; ++i) {
        outHolders.push_back(session.GetOutputNameAllocated(i, alloc));
        outNames.push_back(outHolders.back().get());
    }
    const char* in = inName.get();
    return session.Run(Ort::RunOptions{ nullptr }, &in, &input, 1, outNames.data(), outNames.size());
}

} // namespace

bool ExtractMocap(const std::filesystem::path& video, const ExtractOptions& opts, MocapClip& out,
                  const ExtractProgress& progress, std::string* error)
{
    using clock = std::chrono::steady_clock;
    const auto startTime = clock::now();

    VideoReader reader;
    if (!reader.Open(video, error)) {
        return false;
    }

    std::string ortErr;
    if (!OrtAvailable(&ortErr)) {
        if (error) *error = "ONNX Runtime initialization failed: " + ortErr;
        return false;
    }

    // Load models
    std::unique_ptr<Ort::Session> personDetSession = CreateOrtSession(GetMocapModelPath("person_det.onnx"), error);
    if (!personDetSession) return false;

    std::unique_ptr<Ort::Session> poseLmkSession = CreateOrtSession(GetMocapModelPath("pose_landmark.onnx"), error);
    if (!poseLmkSession) return false;

    std::unique_ptr<Ort::Session> handLmkSession;
    std::unique_ptr<Ort::Session> palmDetSession;
    if (opts.hands) {
        handLmkSession = CreateOrtSession(GetMocapModelPath("hand_landmark.onnx"), error);
        if (!handLmkSession) return false;
        // palm_det is fallback; if file exists, open it
        const auto palmPath = GetMocapModelPath("palm_det.onnx");
        if (std::filesystem::exists(palmPath)) {
            palmDetSession = CreateOrtSession(palmPath, nullptr);
        }
    }

    std::unique_ptr<Ort::Session> faceLmkSession;
    std::unique_ptr<Ort::Session> faceBsSession;
    if (opts.face) {
        faceLmkSession = CreateOrtSession(GetMocapModelPath("face_landmark.onnx"), error);
        if (!faceLmkSession) return false;
        faceBsSession = CreateOrtSession(GetMocapModelPath("face_blendshapes.onnx"), error);
        if (!faceBsSession) return false;
    }

    const uint32_t W = reader.Width();
    const uint32_t H = reader.Height();
    const double videoDuration = reader.Duration();

    out.videoWidth = W;
    out.videoHeight = H;
    out.fps = opts.fps > 0 ? opts.fps : 30.0;
    out.frames.clear();

    const double startSec = std::max(0.0, opts.startSeconds);
    const double endSec = (opts.endSeconds >= 0.0) ? std::min(opts.endSeconds, videoDuration) : videoDuration;
    const double duration = std::max(0.0, endSec - startSec);
    const size_t totalFrames = static_cast<size_t>(std::floor(duration * out.fps + 1.0001));
    out.frames.reserve(totalFrames);

    // Buffers and anchors
    const auto personDetAnchors = GenSsdAnchors(224, { 8, 16, 32, 32, 32 });
    std::vector<float> personDetInput(1 * 3 * 224 * 224);
    std::vector<float> poseInput(1 * 256 * 256 * 3);
    std::vector<float> handInput(opts.hands ? (1 * 224 * 224 * 3) : 0);
    std::vector<float> palmDetInput(palmDetSession ? (1 * 192 * 192 * 3) : 0);
    const auto palmDetAnchors = palmDetSession ? GenSsdAnchors(192, { 8, 16, 16, 16 }) : std::vector<std::array<float, 2>>{};
    std::vector<float> faceInput(opts.face ? (1 * 256 * 256 * 3) : 0);
    std::vector<float> bsInput(opts.face ? (1 * kBlendshapeInputLandmarks * 2) : 0);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::AllocatorWithDefaultOptions alloc;

    bool poseTracking = false;
    RotatedRect trackedPoseRect;

    bool faceTracking = false;
    std::vector<float> prevFacePts; // 478 * 3 pixels
    std::vector<float> prevHandScreen[2]; // 21 * 2 pixels of the last committed hand (tracking ROI)

    size_t validPoseFrames = 0;
    size_t validHandFrames[2] = { 0, 0 };
    size_t validFaceFrames = 0;

    std::vector<uint8_t> bgra;

    for (size_t frameIdx = 0; frameIdx < totalFrames; ++frameIdx) {
        if (progress) {
            const float frac = totalFrames > 1 ? static_cast<float>(frameIdx) / (totalFrames - 1) : 1.f;
            if (!progress(frac)) {
                if (error) *error = "Mocap extraction cancelled";
                return false;
            }
        }

        const double t = startSec + frameIdx / out.fps;
        MocapFrame frame;
        frame.time = t;
        DebugFrameData debugData;

        if (!reader.ReadFrame(t, bgra) || bgra.size() < static_cast<size_t>(W * H * 4)) {
            out.frames.push_back(std::move(frame));
            continue;
        }

        // -------------------------------------------------------------------
        // 1. POSE
        // -------------------------------------------------------------------
        if (!poseTracking) {
            const Letterbox lb = LetterboxToTensor(bgra.data(), W, H, 224, -1.f, 1.f, personDetInput.data(), /*nchw=*/true);
            const std::array<int64_t, 4> shape{ 1, 3, 224, 224 };
            Ort::Value inputVal = Ort::Value::CreateTensor<float>(mem, personDetInput.data(), personDetInput.size(), shape.data(), shape.size());
            auto outs = RunOrtSession(*personDetSession, std::move(inputVal), alloc);

            const float* rawBoxes = nullptr;
            const float* rawScores = nullptr;
            for (auto& o : outs) {
                const auto s = o.GetTensorTypeAndShapeInfo().GetShape();
                if (s.size() == 3 && s[2] > 1) {
                    rawBoxes = o.GetTensorData<float>();
                } else if (s.size() == 3 && s[2] == 1) {
                    rawScores = o.GetTensorData<float>();
                }
            }

            if (rawBoxes && rawScores) {
                auto dets = DecodeDetections(rawBoxes, rawScores, personDetAnchors, 224, /*numKeypoints=*/4, 0.5f, 0.3f);
                if (!dets.empty()) {
                    Detection& best = dets.front();
                    for (auto& kp : best.keypoints) {
                        Unletterbox(lb, kp[0], kp[1]);
                    }
                    trackedPoseRect = RectFromPoseDetection(best, W, H);
                    poseTracking = (trackedPoseRect.w > 8.f && trackedPoseRect.h > 8.f);
                }
            }
        }

        if (poseTracking) {
            CropRotatedRectToTensor(bgra.data(), W, H, trackedPoseRect, 256, 0.f, 1.f, poseInput.data());
            const std::array<int64_t, 4> shape{ 1, 256, 256, 3 };
            Ort::Value inputVal = Ort::Value::CreateTensor<float>(mem, poseInput.data(), poseInput.size(), shape.data(), shape.size());
            auto outs = RunOrtSession(*poseLmkSession, std::move(inputVal), alloc);

            const float* rawScreen = nullptr;
            const float* rawWorld = nullptr;
            float presence = 0.f;
            for (auto& o : outs) {
                const size_t count = o.GetTensorTypeAndShapeInfo().GetElementCount();
                if (count == 39 * 5) {
                    rawScreen = o.GetTensorData<float>();
                } else if (count == 39 * 3) {
                    rawWorld = o.GetTensorData<float>();
                } else if (count == 1) {
                    presence = o.GetTensorData<float>()[0];
                }
            }

            if (rawScreen && rawWorld && presence >= 0.5f) {
                frame.poseValid = true;
                ++validPoseFrames;

                // World landmarks (metres -> metric MMD coordinates, ROI rotation undone)
                const float ca = std::cos(trackedPoseRect.angle);
                const float sa = std::sin(trackedPoseRect.angle);
                for (int i = 0; i < kPoseLmCount; ++i) {
                    const float wx = rawWorld[i * 3 + 0];
                    const float wy = rawWorld[i * 3 + 1];
                    const float wz = rawWorld[i * 3 + 2];
                    const float rx = wx * ca - wy * sa;
                    const float ry = wx * sa + wy * ca;
                    const float rz = wz;

                    frame.pose[i].x = rx * kMetresToMmd;
                    frame.pose[i].y = -ry * kMetresToMmd; // MediaPipe y-down -> +Y up
                    frame.pose[i].z = rz * kMetresToMmd; // MediaPipe z -> +Z away from camera
                    frame.poseVisibility[i] = Sigmoid(rawScreen[i * 5 + 3]);
                }

                // Screen landmarks (normalized y-down 0..1 for image space)
                float screenPts[39 * 3];
                for (int i = 0; i < 39; ++i) {
                    screenPts[i * 3 + 0] = rawScreen[i * 5 + 0] / 256.f;
                    screenPts[i * 3 + 1] = rawScreen[i * 5 + 1] / 256.f;
                    screenPts[i * 3 + 2] = rawScreen[i * 5 + 2] / 256.f;
                }
                ProjectLandmarks(screenPts, 39, 3, trackedPoseRect);
                for (int i = 0; i < kPoseLmCount; ++i) {
                    frame.poseImage[i].x = screenPts[i * 3 + 0] / static_cast<float>(W);
                    frame.poseImage[i].y = screenPts[i * 3 + 1] / static_cast<float>(H);
                }

                // Next-frame ROI from auxiliary alignment landmarks (indices 33 and 34)
                const Detection synth{
                    1.f,
                    { 0.f, 0.f, 0.f, 0.f },
                    { { screenPts[33 * 3 + 0] / W, screenPts[33 * 3 + 1] / H },
                      { screenPts[34 * 3 + 0] / W, screenPts[34 * 3 + 1] / H } }
                };
                trackedPoseRect = RectFromPoseDetection(synth, W, H);
                poseTracking = (trackedPoseRect.w > 8.f && trackedPoseRect.h > 8.f);
            } else {
                poseTracking = false;
                frame.poseValid = false;
            }
        }

        // -------------------------------------------------------------------
        // 2. HANDS
        // -------------------------------------------------------------------
        if (opts.hands && handLmkSession) {
            // One hand model run on `rect`: presence (0 when the crop is degenerate) + landmarks kept in handScreen /
            // handWorld (image pixels / metres, crop rotation undone).
            float handScreen[21 * 2];
            float handWorld[21 * 3];
            auto evalHand = [&](const RotatedRect& rect) -> float {
                if (rect.w < 16.f || rect.h < 16.f) return 0.f;
                CropRotatedRectToTensor(bgra.data(), W, H, rect, 224, 0.f, 1.f, handInput.data());
                const std::array<int64_t, 4> shape{ 1, 224, 224, 3 };
                Ort::Value inputVal = Ort::Value::CreateTensor<float>(mem, handInput.data(), handInput.size(), shape.data(), shape.size());
                auto outs = RunOrtSession(*handLmkSession, std::move(inputVal), alloc);

                // outputs in order: screen landmarks, presence, handedness, world landmarks (mp_handpose.py);
                // presence and handedness are both [1,1]: the FIRST scalar is the presence
                const float* rawScreen = nullptr;
                const float* rawWorld = nullptr;
                float presence = 0.f;
                bool presenceFound = false;
                for (auto& o : outs) {
                    const size_t count = o.GetTensorTypeAndShapeInfo().GetElementCount();
                    if (count == 21 * 3) {
                        if (!rawScreen) rawScreen = o.GetTensorData<float>();
                        else if (!rawWorld) rawWorld = o.GetTensorData<float>();
                    } else if (count == 1 && !presenceFound) {
                        presence = o.GetTensorData<float>()[0];
                        presenceFound = true;
                    }
                }
                if (!rawScreen || !rawWorld) return 0.f;

                const float ca = std::cos(rect.angle);
                const float sa = std::sin(rect.angle);
                for (int k = 0; k < kHandLmCount; ++k) {
                    const float wx = rawWorld[k * 3 + 0];
                    const float wy = rawWorld[k * 3 + 1];
                    handWorld[k * 3 + 0] = wx * ca - wy * sa;
                    handWorld[k * 3 + 1] = wx * sa + wy * ca;
                    handWorld[k * 3 + 2] = rawWorld[k * 3 + 2];
                }
                float screenPts[21 * 3];
                for (int k = 0; k < 21 * 3; ++k) screenPts[k] = rawScreen[k] / 224.f;
                ProjectLandmarks(screenPts, 21, 3, rect);
                for (int k = 0; k < 21; ++k) {
                    handScreen[k * 2 + 0] = screenPts[k * 3 + 0];
                    handScreen[k * 2 + 1] = screenPts[k * 3 + 1];
                }
                return presence;
            };
            auto commitHand = [&](int handIdx) {
                frame.handValid[handIdx] = true;
                ++validHandFrames[handIdx];
                for (int k = 0; k < kHandLmCount; ++k) {
                    frame.hand[handIdx][k].x = handWorld[k * 3 + 0] * kMetresToMmd;
                    frame.hand[handIdx][k].y = -handWorld[k * 3 + 1] * kMetresToMmd;   // y-down -> +Y up
                    frame.hand[handIdx][k].z = handWorld[k * 3 + 2] * kMetresToMmd;    // +Z away from the camera
                }
                debugData.handImage[handIdx].resize(21);
                for (int k = 0; k < 21; ++k) debugData.handImage[handIdx][k] = { handScreen[k * 2], handScreen[k * 2 + 1] };
                prevHandScreen[handIdx].assign(handScreen, handScreen + 21 * 2);
            };
            // Best of several crops, then one re-crop from the found landmarks (MediaPipe's hand tracking ROI), kept
            // when it scores higher. True = committed.
            auto runHandCandidates = [&](const std::vector<RotatedRect>& rects, int handIdx) -> bool {
                float best = 0.f;
                RotatedRect bestRect;
                for (const RotatedRect& r : rects) {
                    const float pr = evalHand(r);
                    if (pr > best) { best = pr; bestRect = r; }
                    if (pr >= 0.9f) break;
                }
                if (best < 0.5f) return false;
                evalHand(bestRect);   // restore the winner's landmarks
                float keepScreen[21 * 2];
                float keepWorld[21 * 3];
                std::copy(handScreen, handScreen + 21 * 2, keepScreen);
                std::copy(handWorld, handWorld + 21 * 3, keepWorld);
                const float p2 = evalHand(RectFromHandLandmarks(keepScreen));
                if (p2 < best) {
                    std::copy(keepScreen, keepScreen + 21 * 2, handScreen);
                    std::copy(keepWorld, keepWorld + 21 * 3, handWorld);
                }
                commitHand(handIdx);
                return true;
            };

            // Hands: tracked from the previous frame's landmarks, else crops along the pose's forearm / hand points
            if (frame.poseValid) {
                struct HandPoseSpec {
                    int handIdx; // 0 = Left, 1 = Right
                    int elbow, wrist, index, pinky;
                };
                const HandPoseSpec specs[2] = {
                    { 0, PoseLm::LeftElbow, PoseLm::LeftWrist, PoseLm::LeftIndex, PoseLm::LeftPinky },
                    { 1, PoseLm::RightElbow, PoseLm::RightWrist, PoseLm::RightIndex, PoseLm::RightPinky }
                };

                for (const auto& spec : specs) {
                    std::vector<RotatedRect> rects;
                    if (prevHandScreen[spec.handIdx].size() == 21 * 2)
                        rects.push_back(RectFromHandLandmarks(prevHandScreen[spec.handIdx].data()));
                    if (frame.poseVisibility[spec.wrist] >= 0.15f) {
                        auto px = [&](int i) { return frame.poseImage[i].x * W; };
                        auto py = [&](int i) { return frame.poseImage[i].y * H; };
                        const float wx = px(spec.wrist), wy = py(spec.wrist);
                        const float kx = 0.5f * (px(spec.index) + px(spec.pinky)) - wx;
                        const float ky = 0.5f * (py(spec.index) + py(spec.pinky)) - wy;
                        const float fx = wx - px(spec.elbow), fy = wy - py(spec.elbow);
                        const float palm = std::sqrt(kx * kx + ky * ky);
                        const float forearm = std::sqrt(fx * fx + fy * fy);
                        // hand length ~ 0.75 forearm; the pose's knuckle points give the direction when they are apart
                        const float handLen = std::max(forearm * 0.75f, palm * 2.2f);
                        float dx = fx, dy = fy;
                        if (palm > 0.25f * handLen) { dx = kx; dy = ky; }
                        const float dl = std::sqrt(dx * dx + dy * dy);
                        if (dl > 1e-3f && handLen > 8.f) {
                            dx /= dl;
                            dy /= dl;
                            for (float scale : { 1.6f, 2.2f, 1.2f }) {
                                for (float along : { 0.5f, 0.3f }) {
                                    RotatedRect r;
                                    r.cx = wx + dx * handLen * along;
                                    r.cy = wy + dy * handLen * along;
                                    r.angle = NormalizeAngle(kPi * 0.5f - std::atan2(-dy, dx));
                                    r.w = r.h = std::max(handLen * scale, 24.f);
                                    rects.push_back(r);
                                }
                            }
                        }
                    }
                    if (!rects.empty()) runHandCandidates(rects, spec.handIdx);
                }
            }

            // Fallback: Palm detector for hands that were not detected
            if (palmDetSession && (!frame.handValid[0] || !frame.handValid[1])) {
                const Letterbox lb = LetterboxToTensor(bgra.data(), W, H, 192, 0.f, 1.f, palmDetInput.data(), /*nchw=*/false);
                const std::array<int64_t, 4> shape{ 1, 192, 192, 3 };
                Ort::Value inputVal = Ort::Value::CreateTensor<float>(mem, palmDetInput.data(), palmDetInput.size(), shape.data(), shape.size());
                auto outs = RunOrtSession(*palmDetSession, std::move(inputVal), alloc);

                const float* rawBoxes = nullptr;
                const float* rawScores = nullptr;
                for (auto& o : outs) {
                    const auto s = o.GetTensorTypeAndShapeInfo().GetShape();
                    if (s.size() == 3 && s[2] > 1) rawBoxes = o.GetTensorData<float>();
                    else if (s.size() == 3 && s[2] == 1) rawScores = o.GetTensorData<float>();
                }

                if (rawBoxes && rawScores) {
                    auto dets = DecodeDetections(rawBoxes, rawScores, palmDetAnchors, 192, 7, 0.5f, 0.3f, /*reverseOutputOrder=*/true);
                    if (dets.empty()) {
                        dets = DecodeDetections(rawBoxes, rawScores, palmDetAnchors, 192, 7, 0.5f, 0.3f, /*reverseOutputOrder=*/false);
                    }
                    for (auto& det : dets) {
                        Unletterbox(lb, det.box[0], det.box[1]);
                        det.box[2] /= lb.fracX;
                        det.box[3] /= lb.fracY;
                        for (auto& kp : det.keypoints) {
                            Unletterbox(lb, kp[0], kp[1]);
                        }
                        const RotatedRect palmRect = RectFromPalmDetection(det, W, H);
                        // Assign left/right from nearer pose wrist
                        int targetHand = 0;
                        if (frame.poseValid) {
                            const float pcx = palmRect.cx;
                            const float pcy = palmRect.cy;
                            const float lx = frame.poseImage[PoseLm::LeftWrist].x * W;
                            const float ly = frame.poseImage[PoseLm::LeftWrist].y * H;
                            const float rx = frame.poseImage[PoseLm::RightWrist].x * W;
                            const float ry = frame.poseImage[PoseLm::RightWrist].y * H;
                            const float dl = (pcx - lx) * (pcx - lx) + (pcy - ly) * (pcy - ly);
                            const float dr = (pcx - rx) * (pcx - rx) + (pcy - ry) * (pcy - ry);
                            targetHand = (dl < dr) ? 0 : 1;
                        }
                        if (!frame.handValid[targetHand]) {
                            runHandCandidates({ palmRect }, targetHand);
                        }
                    }
                }
            }
        }

        for (int h = 0; h < 2; ++h)
            if (!frame.handValid[h]) prevHandScreen[h].clear();

        // -------------------------------------------------------------------
        // 3. FACE
        // -------------------------------------------------------------------
        if (opts.face && faceLmkSession && faceBsSession) {
            RotatedRect faceRect;
            bool haveRect = false;

            if (faceTracking && prevFacePts.size() == kFaceLmCount * 3) {
                faceRect = RectFromFaceLandmarks(prevFacePts.data(), kFaceLmCount, W, H);
                haveRect = (faceRect.w >= 16.f && faceRect.h >= 16.f);
            }
            if (!haveRect && frame.poseValid) {
                faceRect = RectFromPoseFace(frame.poseImage.data(), W, H);
                haveRect = (faceRect.w >= 16.f && faceRect.h >= 16.f);
            }

            if (haveRect) {
                CropRotatedRectToTensor(bgra.data(), W, H, faceRect, 256, 0.f, 1.f, faceInput.data());
                const std::array<int64_t, 4> shape{ 1, 256, 256, 3 };
                Ort::Value inputVal = Ort::Value::CreateTensor<float>(mem, faceInput.data(), faceInput.size(), shape.data(), shape.size());
                auto outs = RunOrtSession(*faceLmkSession, std::move(inputVal), alloc);

                const float* rawLmk = nullptr;
                float presenceLogit = 0.f;
                bool presenceFound = false;
                for (auto& o : outs) {
                    const auto info = o.GetTensorTypeAndShapeInfo();
                    const size_t count = info.GetElementCount();
                    if (count >= 468 * 3) {
                        rawLmk = o.GetTensorData<float>();
                    } else if (count == 1 && !presenceFound) {
                        presenceLogit = o.GetTensorData<float>()[0];
                        presenceFound = true;
                    }
                }

                const float presence = Sigmoid(presenceLogit);
                if (rawLmk && presence >= 0.5f) {
                    frame.faceValid = true;
                    ++validFaceFrames;
                    faceTracking = true;

                    std::vector<float> pts(kFaceLmCount * 3);
                    for (int i = 0; i < kFaceLmCount * 3; ++i) {
                        pts[i] = rawLmk[i] / 256.f;
                    }
                    ProjectLandmarks(pts.data(), kFaceLmCount, 3, faceRect);
                    prevFacePts = pts;

                    frame.faceLandmarks.resize(kFaceLmCount);
                    debugData.faceImage.resize(kFaceLmCount);
                    for (int i = 0; i < kFaceLmCount; ++i) {
                        frame.faceLandmarks[i].x = pts[i * 3 + 0] / static_cast<float>(W);
                        frame.faceLandmarks[i].y = pts[i * 3 + 1] / static_cast<float>(H);
                        frame.faceLandmarks[i].z = pts[i * 3 + 2] / static_cast<float>(W);
                        debugData.faceImage[i].x = pts[i * 3 + 0];
                        debugData.faceImage[i].y = pts[i * 3 + 1];
                    }

                    // Blendshapes
                    for (int i = 0; i < kBlendshapeInputLandmarks; ++i) {
                        const int id = kBlendshapeLandmarkSubset[i];
                        bsInput[i * 2 + 0] = pts[id * 3 + 0];
                        bsInput[i * 2 + 1] = pts[id * 3 + 1];
                    }
                    const std::array<int64_t, 3> bsShape{ 1, kBlendshapeInputLandmarks, 2 };
                    Ort::Value bsInputVal = Ort::Value::CreateTensor<float>(mem, bsInput.data(), bsInput.size(), bsShape.data(), bsShape.size());
                    auto bsOuts = RunOrtSession(*faceBsSession, std::move(bsInputVal), alloc);
                    if (!bsOuts.empty()) {
                        const float* scores = bsOuts.front().GetTensorData<float>();
                        const size_t scoreCount = bsOuts.front().GetTensorTypeAndShapeInfo().GetElementCount();
                        for (size_t i = 0; i < kBlendshapeCount && i < scoreCount; ++i) {
                            frame.blendshapes[i] = std::clamp(scores[i], 0.f, 1.f);
                        }
                    }
                } else {
                    faceTracking = false;
                    frame.faceValid = false;
                }
            } else {
                faceTracking = false;
                frame.faceValid = false;
            }
        }

        if (opts.debugCallback) {
            opts.debugCallback(frameIdx, t, W, H, bgra.data(), frame, debugData);
        }

        out.frames.push_back(std::move(frame));
    }

    const double totalMs = std::chrono::duration<double, std::milli>(clock::now() - startTime).count();
    const double msPerFrame = totalFrames > 0 ? (totalMs / totalFrames) : 0.0;
    const double posePct = totalFrames > 0 ? (100.0 * validPoseFrames / totalFrames) : 0.0;
    const double handLPct = totalFrames > 0 ? (100.0 * validHandFrames[0] / totalFrames) : 0.0;
    const double handRPct = totalFrames > 0 ? (100.0 * validHandFrames[1] / totalFrames) : 0.0;
    const double facePct = totalFrames > 0 ? (100.0 * validFaceFrames / totalFrames) : 0.0;

    LOG_INFO("MOCAP extract: %zu frames, pose %.1f%%, hands L %.1f%% R %.1f%%, face %.1f%%, %.1f ms/frame",
             totalFrames, posePct, handLPct, handRPct, facePct, msPerFrame);

    return true;
}

} // namespace mmdx::mocap
