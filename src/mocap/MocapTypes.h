#pragma once
// Video motion capture ("영상에서 모션 만들기"): the data handed from the landmark extractor (MocapExtractor, ONNX
// MediaPipe models) to the MMD retarget solver (MocapSolver). Every 3D point is in MMD space: left-handed, +Y up,
// +X = the image's right, +Z = away from the camera, MMD units (1 unit = 8 cm, so metres x 12.5). A performer facing
// the camera therefore faces -Z like an MMD model in its default view; nothing is mirrored (their left hand is +X).
#include <DirectXMath.h>
#include <array>
#include <cstdint>
#include <vector>

namespace mmdx::mocap {

using DirectX::XMFLOAT2;
using DirectX::XMFLOAT3;

constexpr float kMetresToMmd = 12.5f;

// BlazePose 33-point topology (MediaPipe Pose Landmarker order).
enum PoseLm : int {
    Nose = 0, LeftEyeInner, LeftEye, LeftEyeOuter, RightEyeInner, RightEye, RightEyeOuter, LeftEar, RightEar,
    MouthLeft, MouthRight, LeftShoulder, RightShoulder, LeftElbow, RightElbow, LeftWrist, RightWrist,
    LeftPinky, RightPinky, LeftIndex, RightIndex, LeftThumb, RightThumb, LeftHip, RightHip, LeftKnee, RightKnee,
    LeftAnkle, RightAnkle, LeftHeel, RightHeel, LeftFootIndex, RightFootIndex,
    kPoseLmCount = 33
};

// MediaPipe hand 21-point topology.
enum HandLm : int {
    Wrist = 0, ThumbCmc, ThumbMcp, ThumbIp, ThumbTip, IndexMcp, IndexPip, IndexDip, IndexTip,
    MiddleMcp, MiddlePip, MiddleDip, MiddleTip, RingMcp, RingPip, RingDip, RingTip,
    PinkyMcp, PinkyPip, PinkyDip, PinkyTip,
    kHandLmCount = 21
};

constexpr int kBlendshapeCount = 52;   // ARKit order as output by MediaPipe face_blendshapes (index 0 = _neutral)
constexpr int kFaceLmCount = 478;      // Face Mesh V2 with irises

struct MocapFrame {
    double time = 0;                   // seconds into the source video

    // Body. world = hip-centred metric 3D (pose world landmarks, converted to MMD space).
    bool poseValid = false;
    std::array<XMFLOAT3, kPoseLmCount> pose{};
    std::array<float, kPoseLmCount> poseVisibility{};   // 0..1
    // Same landmarks in normalised image coordinates (x right 0..1, y DOWN 0..1) for root translation.
    std::array<XMFLOAT2, kPoseLmCount> poseImage{};

    // Hands, index 0 = the performer's LEFT hand, 1 = RIGHT (assigned from the pose wrists, not from the
    // model's handedness guess). Metric 3D, MMD space, camera-aligned (the crop rotation undone), any origin.
    std::array<bool, 2> handValid{};
    std::array<std::array<XMFLOAT3, kHandLmCount>, 2> hand{};

    // Face. blendshapes in ARKit order (0..1). faceLandmarks: normalised image x/y (y down) and z in the same
    // scale as x (MediaPipe convention), for head orientation.
    bool faceValid = false;
    std::array<float, kBlendshapeCount> blendshapes{};
    std::vector<XMFLOAT3> faceLandmarks;   // kFaceLmCount when faceValid, else empty
};

struct MocapClip {
    double fps = 30;                   // sampling rate of frames[] (frames[i].time = start + i / fps)
    uint32_t videoWidth = 0, videoHeight = 0;
    std::vector<MocapFrame> frames;
};

} // namespace mmdx::mocap
