#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "mocap/MocapSolver.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

using namespace mmdx;
using namespace mmdx::mocap;
using namespace DirectX;

namespace {

inline XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XMFLOAT3 Mul(const XMFLOAT3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const XMFLOAT3& a) { return std::sqrt(Dot(a, a)); }

inline XMFLOAT3 Normalize(const XMFLOAT3& a, const XMFLOAT3& fallback = {0, 1, 0}) {
    float len = Length(a);
    if (len < 1e-6f) return fallback;
    return {a.x / len, a.y / len, a.z / len};
}

inline XMFLOAT3 TransformPoint(const XMFLOAT3& p, const XMFLOAT4X4& m) {
    XMFLOAT3 r;
    XMVECTOR v = XMLoadFloat3(&p);
    XMMATRIX mat = XMLoadFloat4x4(&m);
    XMStoreFloat3(&r, XMVector3TransformCoord(v, mat));
    return r;
}

inline XMFLOAT3 TransformNormal(const XMFLOAT3& n, const XMFLOAT4X4& m) {
    XMFLOAT3 r;
    XMVECTOR v = XMLoadFloat3(&n);
    XMMATRIX mat = XMLoadFloat4x4(&m);
    XMStoreFloat3(&r, XMVector3TransformNormal(v, mat));
    return r;
}

int FindBone(const PmxModel& pmx, const std::vector<std::string>& candidates) {
    for (const auto& c : candidates) {
        for (size_t i = 0; i < pmx.bones.size(); ++i) {
            if (pmx.bones[i].name == c) return (int)i;
        }
    }
    return -1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::filesystem::path pmxPath = (argc >= 2) ? argv[1]
        : L"library\\model\\Sekai Model Pack v4\\LeoNeed\\Ichika\\01_9001.pmx";
    std::filesystem::path vmdPath = (argc >= 3) ? argv[2]
        : L"library\\motion\\Evergreen\\ch0208_so2009.vmd";
    const int testFrames = (argc >= 4) ? _wtoi(argv[3]) : 300;

    if (!std::filesystem::exists(pmxPath) || !std::filesystem::exists(vmdPath)) {
        std::printf("SKIP: Required files not found (%ls, %ls)\n", pmxPath.c_str(), vmdPath.c_str());
        return 0;
    }

    auto pmx = std::make_shared<PmxModel>();
    std::string err;
    if (!LoadModelFile(pmxPath, ModelRole::Character, *pmx, &err)) {
        std::printf("FAIL: Failed to load PMX %ls: %s\n", pmxPath.c_str(), err.c_str());
        return 1;
    }

    VmdMotion srcVmd;
    if (!LoadVmd(vmdPath, srcVmd, &err)) {
        std::printf("FAIL: Failed to load VMD %ls: %s\n", vmdPath.c_str(), err.c_str());
        return 1;
    }

    auto srcMotion = BoundMotion::Bind(*pmx, {&srcVmd});
    std::printf("=================================================================\n");
    std::printf(" Mocap Solver Round-Trip Test\n");
    std::printf(" Model: %s (%zu bones)\n", pmx->name.c_str(), pmx->bones.size());
    std::printf(" Frames to test: %d\n", testFrames);
    std::printf("=================================================================\n");

    // Bone mapping
    int bLeftArm = FindBone(*pmx, {"左腕"});
    int bRightArm = FindBone(*pmx, {"右腕"});
    int bLeftElbow = FindBone(*pmx, {"左ひじ"});
    int bRightElbow = FindBone(*pmx, {"右ひじ"});
    int bLeftWrist = FindBone(*pmx, {"左手首"});
    int bRightWrist = FindBone(*pmx, {"右手首"});

    int bLeftLeg = FindBone(*pmx, {"左足"});
    int bRightLeg = FindBone(*pmx, {"右足"});
    int bLeftKnee = FindBone(*pmx, {"左ひざ"});
    int bRightKnee = FindBone(*pmx, {"右ひざ"});
    int bLeftAnkle = FindBone(*pmx, {"左足首"});
    int bRightAnkle = FindBone(*pmx, {"右足首"});
    int bLeftToe = FindBone(*pmx, {"左つま先"});
    int bRightToe = FindBone(*pmx, {"右つま先"});

    int bHead = FindBone(*pmx, {"頭"});

    // Finger bones: [hand: 0=Left, 1=Right][finger: 0=Thumb..4=Pinky][joint: 0..2]
    int fingers[2][5][3]{};
    const char* prefix[2] = {"左", "右"};
    const char* fingerNames[5][3] = {
        {"親指０", "親指１", "親指２"},
        {"人差指１", "人差指２", "人差指３"},
        {"中指１", "中指２", "中指３"},
        {"薬指１", "薬指２", "薬指３"},
        {"小指１", "小指２", "小指３"}
    };
    const char* altFingerNames[5][3] = {
        {"親指0", "親指1", "親指2"},
        {"人指１", "人指２", "人指３"},
        {"中指1", "中指2", "中指3"},
        {"薬指1", "薬指2", "薬指3"},
        {"小指1", "小指2", "小指3"}
    };
    for (int h = 0; h < 2; ++h) {
        for (int f = 0; f < 5; ++f) {
            for (int j = 0; j < 3; ++j) {
                fingers[h][f][j] = FindBone(*pmx, {
                    std::string(prefix[h]) + fingerNames[f][j],
                    std::string(prefix[h]) + altFingerNames[f][j]
                });
            }
        }
    }

    ModelInstance instSrc(pmx);
    instSrc.EnablePhysics(false);

    // 1. Synthesize MocapClip from source VMD
    MocapClip clip;
    clip.fps = 30.0;
    clip.videoWidth = 1920;
    clip.videoHeight = 1080;
    clip.frames.resize(testFrames);

    struct GroundTruth {
        XMFLOAT3 leftElbow, rightElbow, leftWrist, rightWrist;
        XMFLOAT3 leftKnee, rightKnee, leftAnkle, rightAnkle;
        XMFLOAT3 leftToe, rightToe;
        XMFLOAT3 hipCenter;
        XMFLOAT3 headForward;
        XMFLOAT3 fingerTips[2][5];
        XMFLOAT3 wristPos[2];
    };
    std::vector<GroundTruth> gt(testFrames);

    const float kOrthoScale = 25.0f;
    const float kAspect = 1920.0f / 1080.0f;   // the image is 16:9: x is normalised by the width

    for (int f = 0; f < testFrames; ++f) {
        srcMotion->Evaluate((float)f, instSrc);
        instSrc.UpdatePose(0.0f);

        MocapFrame& mf = clip.frames[f];
        mf.time = f / 30.0;
        mf.poseValid = true;

        XMFLOAT3 pLSh = instSrc.BoneWorldPosition(bLeftArm);
        XMFLOAT3 pRSh = instSrc.BoneWorldPosition(bRightArm);
        XMFLOAT3 pLEl = instSrc.BoneWorldPosition(bLeftElbow);
        XMFLOAT3 pREl = instSrc.BoneWorldPosition(bRightElbow);
        XMFLOAT3 pLWr = instSrc.BoneWorldPosition(bLeftWrist);
        XMFLOAT3 pRWr = instSrc.BoneWorldPosition(bRightWrist);

        XMFLOAT3 pLHp = instSrc.BoneWorldPosition(bLeftLeg);
        XMFLOAT3 pRHp = instSrc.BoneWorldPosition(bRightLeg);
        XMFLOAT3 pLKn = instSrc.BoneWorldPosition(bLeftKnee);
        XMFLOAT3 pRKn = instSrc.BoneWorldPosition(bRightKnee);
        XMFLOAT3 pLAn = instSrc.BoneWorldPosition(bLeftAnkle);
        XMFLOAT3 pRAn = instSrc.BoneWorldPosition(bRightAnkle);
        XMFLOAT3 pLTo = (bLeftToe >= 0) ? instSrc.BoneWorldPosition(bLeftToe) : Add(pLAn, {0, -1.0f, -1.5f});
        XMFLOAT3 pRTo = (bRightToe >= 0) ? instSrc.BoneWorldPosition(bRightToe) : Add(pRAn, {0, -1.0f, -1.5f});

        XMFLOAT3 pLHeel = Add(pLAn, {0, 0, 0.4f}); // ankle - small forward (-Z)
        XMFLOAT3 pRHeel = Add(pRAn, {0, 0, 0.4f});

        // Face points from head bone world transform
        const XMFLOAT4X4& mHead = instSrc.BoneWorld(bHead);
        XMFLOAT3 pNose = TransformPoint({0, 0.2f, -1.2f}, mHead);
        XMFLOAT3 pLEye = TransformPoint({0.35f, 0.4f, -0.8f}, mHead);
        XMFLOAT3 pREye = TransformPoint({-0.35f, 0.4f, -0.8f}, mHead);
        XMFLOAT3 pLEar = TransformPoint({0.8f, 0.2f, 0.0f}, mHead);
        XMFLOAT3 pREar = TransformPoint({-0.8f, 0.2f, 0.0f}, mHead);

        // Record ground truth
        gt[f].leftElbow = pLEl; gt[f].rightElbow = pREl;
        gt[f].leftWrist = pLWr; gt[f].rightWrist = pRWr;
        gt[f].leftKnee = pLKn; gt[f].rightKnee = pRKn;
        gt[f].leftAnkle = pLAn; gt[f].rightAnkle = pRAn;
        gt[f].leftToe = pLTo; gt[f].rightToe = pRTo;
        gt[f].hipCenter = Mul(Add(pLHp, pRHp), 0.5f);
        gt[f].headForward = TransformNormal({0, 0, -1.0f}, mHead);
        gt[f].wristPos[0] = pLWr; gt[f].wristPos[1] = pRWr;

        // Populate raw world landmarks
        std::array<XMFLOAT3, kPoseLmCount> rawWorld{};
        rawWorld[Nose] = pNose;
        rawWorld[LeftEye] = pLEye; rawWorld[RightEye] = pREye;
        rawWorld[LeftEar] = pLEar; rawWorld[RightEar] = pREar;
        rawWorld[LeftShoulder] = pLSh; rawWorld[RightShoulder] = pRSh;
        rawWorld[LeftElbow] = pLEl; rawWorld[RightElbow] = pREl;
        rawWorld[LeftWrist] = pLWr; rawWorld[RightWrist] = pRWr;
        rawWorld[LeftHip] = pLHp; rawWorld[RightHip] = pRHp;
        rawWorld[LeftKnee] = pLKn; rawWorld[RightKnee] = pRKn;
        rawWorld[LeftAnkle] = pLAn; rawWorld[RightAnkle] = pRAn;
        rawWorld[LeftHeel] = pLHeel; rawWorld[RightHeel] = pRHeel;
        rawWorld[LeftFootIndex] = pLTo; rawWorld[RightFootIndex] = pRTo;

        // Hip-centred pose landmarks & orthographic image coords
        for (int i = 0; i < kPoseLmCount; ++i) {
            mf.pose[i] = Sub(rawWorld[i], gt[f].hipCenter);
            mf.poseVisibility[i] = 1.0f;
            mf.poseImage[i].x = 0.5f + rawWorld[i].x / (kOrthoScale * kAspect);
            mf.poseImage[i].y = 0.5f - rawWorld[i].y / kOrthoScale;
        }

        // Face landmarks (only the points the solver reads), every other frame so both head paths run
        if ((f & 1) == 0) {
            mf.faceValid = true;
            mf.faceLandmarks.assign(kFaceLmCount, XMFLOAT3{});
            auto putFace = [&](int i, XMFLOAT3 local) {
                const XMFLOAT3 w = TransformPoint(local, mHead);
                mf.faceLandmarks[i] = {0.5f + w.x / (kOrthoScale * kAspect), 0.5f - w.y / kOrthoScale,
                                       w.z / (kOrthoScale * kAspect)};
            };
            putFace(263, {0.6f, 0.6f, -0.5f});   // performer's left eye outer corner (+X)
            putFace(33, {-0.6f, 0.6f, -0.5f});
            putFace(10, {0.0f, 1.5f, -0.4f});    // forehead top
            putFace(152, {0.0f, -0.5f, -0.6f});  // chin
        }

        // Hands
        for (int h = 0; h < 2; ++h) {
            mf.handValid[h] = true;
            mf.hand[h][Wrist] = (h == 0) ? pLWr : pRWr;

            // Thumb
            int bTh0 = fingers[h][0][0];
            int bTh1 = fingers[h][0][1];
            int bTh2 = fingers[h][0][2];
            XMFLOAT3 pTh0 = (bTh0 >= 0) ? instSrc.BoneWorldPosition(bTh0) : Add(mf.hand[h][Wrist], {0.2f, 0, 0});
            XMFLOAT3 pTh1 = (bTh1 >= 0) ? instSrc.BoneWorldPosition(bTh1) : Add(pTh0, {0.2f, 0, 0});
            XMFLOAT3 pTh2 = (bTh2 >= 0) ? instSrc.BoneWorldPosition(bTh2) : Add(pTh1, {0.2f, 0, 0});
            XMFLOAT3 pThTip = Add(pTh2, Mul(Sub(pTh2, pTh1), 0.7f));

            mf.hand[h][ThumbCmc] = pTh0;
            mf.hand[h][ThumbMcp] = pTh1;
            mf.hand[h][ThumbIp]  = pTh2;
            mf.hand[h][ThumbTip] = pThTip;
            gt[f].fingerTips[h][0] = pThTip;

            // 4 fingers: Index, Middle, Ring, Pinky
            int baseLm[4] = {IndexMcp, MiddleMcp, RingMcp, PinkyMcp};
            for (int fi = 0; fi < 4; ++fi) {
                int finger = fi + 1;
                int b1 = fingers[h][finger][0];
                int b2 = fingers[h][finger][1];
                int b3 = fingers[h][finger][2];

                XMFLOAT3 p1 = (b1 >= 0) ? instSrc.BoneWorldPosition(b1) : Add(mf.hand[h][Wrist], {0.3f, 0, 0});
                XMFLOAT3 p2 = (b2 >= 0) ? instSrc.BoneWorldPosition(b2) : Add(p1, {0.3f, 0, 0});
                XMFLOAT3 p3 = (b3 >= 0) ? instSrc.BoneWorldPosition(b3) : Add(p2, {0.2f, 0, 0});
                XMFLOAT3 tip = Add(p3, Mul(Sub(p3, p2), 0.7f));

                mf.hand[h][baseLm[fi] + 0] = p1; // Mcp
                mf.hand[h][baseLm[fi] + 1] = p2; // Pip
                mf.hand[h][baseLm[fi] + 2] = p3; // Dip
                mf.hand[h][baseLm[fi] + 3] = tip;// Tip

                gt[f].fingerTips[h][finger] = tip;
            }
        }
    }

    // 2. Solve MocapClip -> VMD
    SolveOptions opts;
    opts.smoothing = 0.0f; // raw solve for purest round-trip fidelity
    VmdMotion solvedVmd;
    SolveReport report;
    if (!SolveMotion(clip, *pmx, opts, solvedVmd, &report, &err)) {
        std::printf("FAIL: SolveMotion failed: %s\n", err.c_str());
        return 1;
    }

    std::printf("Solve succeeded: %d bone keys, %d morph keys across %d frames\n",
                report.boneKeys, report.morphKeys, report.frames);

    // 3. Pose model with solved VMD and evaluate
    auto solvedMotion = BoundMotion::Bind(*pmx, {&solvedVmd});
    ModelInstance instSolved(pmx);
    instSolved.EnablePhysics(false);

    float sumLimbErr = 0.0f;
    float maxLimbErr = 0.0f;
    int limbCount = 0;

    float sumFingerErr = 0.0f;
    int fingerCount = 0;

    float minFootY = 999.0f;
    float sumFootContactErr = 0.0f;
    float minSrcFootY = 999.0f;
    float sumHeadAngle = 0.0f;

    for (int f = 0; f < testFrames; ++f) {
        solvedMotion->Evaluate((float)f, instSolved);
        instSolved.UpdatePose(0.0f);

        // Align hip centre
        XMFLOAT3 pLHp = instSolved.BoneWorldPosition(bLeftLeg);
        XMFLOAT3 pRHp = instSolved.BoneWorldPosition(bRightLeg);
        XMFLOAT3 solHip = Mul(Add(pLHp, pRHp), 0.5f);
        XMFLOAT3 deltaH = Sub(gt[f].hipCenter, solHip);

        auto checkLimb = [&](int boneIdx, const XMFLOAT3& target) {
            if (boneIdx < 0) return;
            XMFLOAT3 pSol = Add(instSolved.BoneWorldPosition(boneIdx), deltaH);
            float err = Length(Sub(pSol, target));
            sumLimbErr += err;
            if (err > maxLimbErr) maxLimbErr = err;
            ++limbCount;
        };

        checkLimb(bLeftElbow, gt[f].leftElbow);
        checkLimb(bRightElbow, gt[f].rightElbow);
        checkLimb(bLeftWrist, gt[f].leftWrist);
        checkLimb(bRightWrist, gt[f].rightWrist);
        checkLimb(bLeftKnee, gt[f].leftKnee);
        checkLimb(bRightKnee, gt[f].rightKnee);
        checkLimb(bLeftAnkle, gt[f].leftAnkle);
        checkLimb(bRightAnkle, gt[f].rightAnkle);

        // Feet vertical position check
        XMFLOAT3 solLAnk = instSolved.BoneWorldPosition(bLeftAnkle);
        XMFLOAT3 solRAnk = instSolved.BoneWorldPosition(bRightAnkle);
        XMFLOAT3 solLToe = (bLeftToe >= 0) ? instSolved.BoneWorldPosition(bLeftToe) : solLAnk;
        XMFLOAT3 solRToe = (bRightToe >= 0) ? instSolved.BoneWorldPosition(bRightToe) : solRAnk;
        minFootY = std::min({minFootY, solLAnk.y, solRAnk.y, solLToe.y, solRToe.y});
        const float solLowest = std::min({solLAnk.y, solRAnk.y, solLToe.y, solRToe.y});
        const float srcLowest = std::min({gt[f].leftAnkle.y, gt[f].rightAnkle.y, gt[f].leftToe.y, gt[f].rightToe.y});
        sumFootContactErr += std::fabs(solLowest - srcLowest);
        minSrcFootY = std::min(minSrcFootY, srcLowest);
        if (f % 30 == 0) std::printf("  foot f%3d: src %.3f solved %.3f\n", f, srcLowest, solLowest);

        // Head direction check
        const XMFLOAT4X4& mHead = instSolved.BoneWorld(bHead);
        XMFLOAT3 solHeadFwd = TransformNormal({0, 0, -1.0f}, mHead);
        float cosA = std::clamp(Dot(Normalize(solHeadFwd), Normalize(gt[f].headForward)), -1.0f, 1.0f);
        sumHeadAngle += std::acos(cosA) * (180.0f / 3.14159265f);

        // Finger tips error: align wrist first
        float fErr[2][5]{};
        for (int h = 0; h < 2; ++h) {
            int bWr = (h == 0) ? bLeftWrist : bRightWrist;
            XMFLOAT3 solWr = instSolved.BoneWorldPosition(bWr);
            XMFLOAT3 srcWr = gt[f].wristPos[h];
            XMFLOAT3 deltaWr = Sub(srcWr, solWr);

            for (int finger = 0; finger < 5; ++finger) {
                int tipBone = (finger == 0) ? fingers[h][0][2] : fingers[h][finger][2];
                int pBone = (finger == 0) ? fingers[h][0][1] : fingers[h][finger][1];
                if (tipBone < 0 || pBone < 0) continue;

                XMFLOAT3 pT = instSolved.BoneWorldPosition(tipBone);
                XMFLOAT3 pP = instSolved.BoneWorldPosition(pBone);
                XMFLOAT3 pTip = Add(pT, Mul(Sub(pT, pP), 0.7f));

                // Position error after aligning wrist
                float err = Length(Sub(Add(pTip, deltaWr), gt[f].fingerTips[h][finger]));
                fErr[h][finger] = err;
                sumFingerErr += err;
                ++fingerCount;
            }
        }
        if (f == 0) {
            std::printf("Frame 0 Fingers: LTh=%.3f, LIdx=%.3f, LMid=%.3f, LRng=%.3f, LPnk=%.3f | RTh=%.3f, RIdx=%.3f, RMid=%.3f, RRng=%.3f, RPnk=%.3f\n",
                        fErr[0][0], fErr[0][1], fErr[0][2], fErr[0][3], fErr[0][4],
                        fErr[1][0], fErr[1][1], fErr[1][2], fErr[1][3], fErr[1][4]);
        }
    }

    float meanLimbErr = (limbCount > 0) ? (sumLimbErr / limbCount) : 0.0f;
    float meanFingerErr = (fingerCount > 0) ? (sumFingerErr / fingerCount) : 0.0f;
    float meanHeadAngle = sumHeadAngle / testFrames;

    // 4. Static Test: same frame repeated 60x with +-small noise -> <= 3 keys per bone
    std::printf("\n--- Running Static Test (60 frames with noise) ---\n");
    MocapClip staticClip;
    staticClip.fps = 30.0;
    staticClip.frames.resize(60);
    const MocapFrame& refFrame = clip.frames[0];
    for (int i = 0; i < 60; ++i) {
        staticClip.frames[i] = refFrame;
        staticClip.frames[i].time = i / 30.0;
        float noise = 0.001f * std::sin((float)i * 1.5f);
        for (auto& p : staticClip.frames[i].pose) {
            p.x += noise; p.y -= noise; p.z += noise;
        }
    }
    SolveOptions staticOpts;
    staticOpts.smoothing = 0.5f; // smoothing on to filter jitter
    staticOpts.rotationTolerance = 0.5f;
    staticOpts.translationTolerance = 0.05f;

    VmdMotion staticVmd;
    SolveReport staticRep;
    SolveMotion(staticClip, *pmx, staticOpts, staticVmd, &staticRep, &err);

    std::unordered_map<std::string, int> keysPerBone;
    for (const auto& k : staticVmd.boneKeys) keysPerBone[k.boneName]++;

    int maxKeysOnBone = 0;
    std::string worstBoneName;
    for (const auto& [bName, count] : keysPerBone) {
        if (count > maxKeysOnBone) {
            maxKeysOnBone = count;
            worstBoneName = bName;
        }
    }
    std::printf("Static test max keys per bone: %d ('%s') (target <= 3)\n", maxKeysOnBone, worstBoneName.c_str());

    // 5. Face Table Test: synthetic blendshapes
    std::printf("\n--- Running Face Table Test ---\n");
    struct FaceTestCase {
        const char* desc;
        int blendIdx1, blendIdx2;
        float val1, val2;
        const char* expectedMorph;
        float expectedWeight;
    };
    FaceTestCase faceTests[] = {
        {"jawOpen 1.0 -> あ ≈ 1.0", 25, -1, 1.0f, 0.0f, "あ", 1.0f},
        {"eyeBlinkLeft+Right 1.0 -> まばたき ≈ 1.0", 9, 10, 1.0f, 1.0f, "まばたき", 1.0f},
        {"eyeBlinkLeft 1.0 -> ウィンク ≈ 1.0", 9, -1, 1.0f, 0.0f, "ウィンク", 1.0f},
        {"eyeBlinkRight 1.0 -> ウィンク右 ≈ 1.0", 10, -1, 1.0f, 0.0f, "ウィンク右", 1.0f},
        {"mouthPucker 1.0 -> う ≈ 1.0", 38, -1, 1.0f, 0.0f, "う", 1.0f},
        {"mouthStretch 1.0 -> い ≈ 1.0", 46, 47, 1.0f, 1.0f, "い", 1.0f},
        {"browInnerUp 1.0 -> 困る ≈ 1.0", 3, -1, 1.0f, 0.0f, "困る", 1.0f},
        {"browOuterUp 1.0 -> 上 ≈ 1.0", 4, 5, 1.0f, 1.0f, "上", 1.0f},
        {"cheekSquint 1.0 -> 笑い ≈ 1.0", 7, 8, 1.0f, 1.0f, "笑い", 1.0f}
    };

    bool faceTestsPass = true;
    for (const auto& tc : faceTests) {
        MocapClip singleClip;
        singleClip.fps = 30.0;
        MocapFrame mf;
        mf.time = 0;
        mf.poseValid = true;
        mf.faceValid = true;
        if (tc.blendIdx1 >= 0) mf.blendshapes[tc.blendIdx1] = tc.val1;
        if (tc.blendIdx2 >= 0) mf.blendshapes[tc.blendIdx2] = tc.val2;
        singleClip.frames.push_back(mf);

        VmdMotion fVmd;
        SolveOptions fOpts;
        fOpts.smoothing = 0.0f;
        SolveMotion(singleClip, *pmx, fOpts, fVmd);

        float foundWeight = 0.0f;
        bool found = false;
        for (const auto& mk : fVmd.morphKeys) {
            if (mk.morphName == tc.expectedMorph) {
                foundWeight = mk.weight;
                found = true;
                break;
            }
        }
        float diff = std::abs(foundWeight - tc.expectedWeight);
        bool ok = found && (diff <= 0.2f);
        if (!ok) faceTestsPass = false;
        std::printf("  [%s] %s: got %.3f (expected %.3f)\n", ok ? "PASS" : "FAIL", tc.desc, foundWeight, tc.expectedWeight);
    }

    // 6. Summary Table & Pass Criteria
    std::printf("\n=================================================================\n");
    std::printf(" SUMMARY RESULTS\n");
    std::printf("=================================================================\n");
    std::printf(" Joint Group             | Metric            | Target     | Value      | Result\n");
    std::printf(" ----------------------- | ----------------- | ---------- | ---------- | ------\n");
    std::printf(" Limbs (Elbow/Wrist/Leg) | Mean Position Err | < 0.350    | %-10.4f | %s\n",
                meanLimbErr, (meanLimbErr < 0.35f) ? "PASS" : "FAIL");
    std::printf(" Limbs (Elbow/Wrist/Leg) | Max Position Err  | Report     | %-10.4f | INFO\n",
                maxLimbErr);
    std::printf(" Finger Tips             | Mean Position Err | < 0.150    | %-10.4f | %s\n",
                meanFingerErr, (meanFingerErr < 0.15f) ? "PASS" : "FAIL");
    std::printf(" Center Floor Constraint | Min Foot Y        | >= -0.050  | %-10.4f | %s\n",
                minFootY, (minFootY >= -0.05f) ? "PASS" : "FAIL");
    std::printf(" (source min foot y %.3f)\n", minSrcFootY);
    const float meanFootContact = testFrames > 0 ? sumFootContactErr / testFrames : 0.0f;
    std::printf(" Foot Contact            | Mean |dLowest|   | < 0.300    | %-10.4f | %s\n",
                meanFootContact, (meanFootContact < 0.3f) ? "PASS" : "FAIL");
    std::printf(" Head Orientation        | Mean Angle Error  | < 10.0 deg | %-10.2f | %s\n",
                meanHeadAngle, (meanHeadAngle < 10.0f) ? "PASS" : "FAIL");
    std::printf(" Static Key Reduction    | Max Keys Per Bone | <= 3 keys  | %-10d | %s\n",
                maxKeysOnBone, (maxKeysOnBone <= 3) ? "PASS" : "FAIL");
    std::printf(" Face Morph Mapping      | Blendshape Tests  | All match  | %-10s | %s\n",
                faceTestsPass ? "9/9 match" : "mismatches", faceTestsPass ? "PASS" : "FAIL");
    std::printf("=================================================================\n");

    bool allPass = (meanLimbErr < 0.35f) &&
                   (meanFingerErr < 0.15f) &&
                   (minFootY >= -0.05f) &&
                   (meanFootContact < 0.3f) &&
                   (meanHeadAngle < 10.0f) &&
                   (maxKeysOnBone <= 3) &&
                   faceTestsPass;

    std::printf("OVERALL: %s\n", allPass ? "PASS" : "FAIL");
    return allPass ? 0 : 1;
}
