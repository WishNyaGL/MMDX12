#include "mocap/MocapSolver.h"
#include "anim/ModelInstance.h"
#include <memory>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

namespace mmdx::mocap {

using namespace DirectX;

// ----------------------------------------------------------------------------
// ARKit 52 Blendshape names in MediaPipe output order (index 0 = "_neutral")
// ----------------------------------------------------------------------------
const char* const kArkitBlendshapeNames[52] = {
    "_neutral",
    "browDownLeft", "browDownRight", "browInnerUp", "browOuterUpLeft", "browOuterUpRight",
    "cheekPuff", "cheekSquintLeft", "cheekSquintRight",
    "eyeBlinkLeft", "eyeBlinkRight",
    "eyeLookDownLeft", "eyeLookDownRight", "eyeLookInLeft", "eyeLookInRight",
    "eyeLookOutLeft", "eyeLookOutRight", "eyeLookUpLeft", "eyeLookUpRight",
    "eyeSquintLeft", "eyeSquintRight", "eyeWideLeft", "eyeWideRight",
    "jawForward", "jawLeft", "jawOpen", "jawRight",
    "mouthClose", "mouthDimpleLeft", "mouthDimpleRight", "mouthFrownLeft", "mouthFrownRight",
    "mouthFunnel", "mouthLeft", "mouthLowerDownLeft", "mouthLowerDownRight",
    "mouthPressLeft", "mouthPressRight", "mouthPucker", "mouthRight",
    "mouthRollLower", "mouthRollUpper", "mouthShrugLower", "mouthShrugUpper",
    "mouthSmileLeft", "mouthSmileRight", "mouthStretchLeft", "mouthStretchRight",
    "mouthUpperUpLeft", "mouthUpperUpRight",
    "noseSneerLeft", "noseSneerRight"
};

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr float kDegToRad = kPi / 180.0f;

// ----------------------------------------------------------------------------
// Vector & Quaternion math helpers
// ----------------------------------------------------------------------------
inline XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline XMFLOAT2 Add(const XMFLOAT2& a, const XMFLOAT2& b) { return {a.x + b.x, a.y + b.y}; }
inline XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline XMFLOAT2 Sub(const XMFLOAT2& a, const XMFLOAT2& b) { return {a.x - b.x, a.y - b.y}; }
inline XMFLOAT3 Mul(const XMFLOAT3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline XMFLOAT2 Mul(const XMFLOAT2& a, float s) { return {a.x * s, a.y * s}; }
inline float Dot(const XMFLOAT3& a, const XMFLOAT3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const XMFLOAT3& a) { return std::sqrt(Dot(a, a)); }

inline XMFLOAT3 Normalize(const XMFLOAT3& a, const XMFLOAT3& fallback = {0, 1, 0}) {
    float len = Length(a);
    if (len < 1e-6f) return fallback;
    return {a.x / len, a.y / len, a.z / len};
}

inline XMFLOAT3 Cross(const XMFLOAT3& a, const XMFLOAT3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline XMFLOAT4 QuatIdentity() { return {0, 0, 0, 1}; }

inline XMFLOAT4 QuatMultiply(const XMFLOAT4& a, const XMFLOAT4& b) {
    XMFLOAT4 r;
    XMStoreFloat4(&r, XMQuaternionMultiply(XMLoadFloat4(&a), XMLoadFloat4(&b)));
    return r;
}

inline XMFLOAT4 QuatInverse(const XMFLOAT4& q) {
    XMFLOAT4 r;
    XMStoreFloat4(&r, XMQuaternionInverse(XMLoadFloat4(&q)));
    return r;
}

inline XMFLOAT4 QuatNormalize(const XMFLOAT4& q) {
    XMFLOAT4 r;
    XMStoreFloat4(&r, XMQuaternionNormalize(XMLoadFloat4(&q)));
    return r;
}

inline XMFLOAT3 RotateVec(const XMFLOAT3& v, const XMFLOAT4& q) {
    XMFLOAT3 r;
    XMStoreFloat3(&r, XMVector3Rotate(XMLoadFloat3(&v), XMLoadFloat4(&q)));
    return r;
}

inline XMFLOAT4 QuatShortestArc(const XMFLOAT3& from, const XMFLOAT3& to) {
    XMFLOAT3 v1 = Normalize(from);
    XMFLOAT3 v2 = Normalize(to);
    float dot = Dot(v1, v2);
    if (dot > 0.999999f) return QuatIdentity();
    if (dot < -0.999999f) {
        XMFLOAT3 alt = (std::abs(v1.x) < 0.9f) ? XMFLOAT3{1, 0, 0} : XMFLOAT3{0, 1, 0};
        XMFLOAT3 axis = Normalize(Cross(v1, alt));
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    XMFLOAT3 c = Cross(v1, v2);
    XMFLOAT4 q{c.x, c.y, c.z, 1.0f + dot};
    return QuatNormalize(q);
}

inline float QuatAngleDeg(const XMFLOAT4& q1, const XMFLOAT4& q2) {
    float dot = std::abs(q1.x * q2.x + q1.y * q2.y + q1.z * q2.z + q1.w * q2.w);
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * std::acos(dot) * kRadToDeg;
}

// Build an orthonormal basis matrix where row 0 is primary, row 2 is normal to (primary x ref), row 1 is row 2 x row 0.
inline XMMATRIX BuildBasis(const XMFLOAT3& primary, const XMFLOAT3& ref) {
    XMFLOAT3 p = Normalize(primary, {1, 0, 0});
    XMFLOAT3 c = Cross(p, ref);
    if (Length(c) < 1e-4f) {
        // Fallback reference if primary and ref are nearly collinear
        XMFLOAT3 alt = (std::abs(p.y) < 0.9f) ? XMFLOAT3{0, 1, 0} : XMFLOAT3{0, 0, 1};
        c = Cross(p, alt);
    }
    XMFLOAT3 z = Normalize(c, {0, 0, 1});
    XMFLOAT3 y = Cross(z, p);
    XMVECTOR v0 = XMLoadFloat3(&p);
    XMVECTOR v1 = XMLoadFloat3(&y);
    XMVECTOR v2 = XMLoadFloat3(&z);
    return XMMATRIX(v0, v1, v2, g_XMIdentityR3);
}

// Compute world rotation taking basis F_rest to F_target
inline XMFLOAT4 RotationBetweenFrames(const XMFLOAT3& p_rest, const XMFLOAT3& ref_rest,
                                      const XMFLOAT3& p_target, const XMFLOAT3& ref_target) {
    XMMATRIX F_rest = BuildBasis(p_rest, ref_rest);
    XMMATRIX F_target = BuildBasis(p_target, ref_target);
    // In row-vector DirectXMath: F_rest * M = F_target -> M = F_rest^T * F_target
    XMMATRIX M = XMMatrixMultiply(XMMatrixTranspose(F_rest), F_target);
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionNormalize(XMQuaternionRotationMatrix(M)));
    return q;
}

// Fill standard VMD linear bezier interpolation block (20, 20, 107, 107)
void FillLinearInterp(uint8_t interp[64]) {
    std::memset(interp, 0, 64);
    uint8_t t[16];
    for (int i = 0; i < 4; ++i) {
        uint8_t val = (i < 2) ? 20 : 107;
        for (int c = 0; c < 4; ++c) t[i * 4 + c] = val;
    }
    for (int row = 0; row < 4; ++row) {
        for (int j = 0; j + row < 16; ++j) {
            interp[row * 16 + j] = t[j + row];
        }
    }
}

// ----------------------------------------------------------------------------
// One-Euro Filter
// ----------------------------------------------------------------------------
class OneEuroFilter1D {
public:
    void Init(float minCutoff = 1.0f, float beta = 0.005f, float dCutoff = 1.0f) {
        minCutoff_ = minCutoff; beta_ = beta; dCutoff_ = dCutoff;
        hasPrev_ = false;
    }
    float Filter(float x, float dt) {
        if (!hasPrev_ || dt <= 0.0f) {
            xPrev_ = x; dxPrev_ = 0.0f; hasPrev_ = true;
            return x;
        }
        float dx = (x - xPrev_) / dt;
        float alphaD = Alpha(dt, dCutoff_);
        float dxHat = alphaD * dx + (1.0f - alphaD) * dxPrev_;
        float cutoff = minCutoff_ + beta_ * std::abs(dxHat);
        float alpha = Alpha(dt, cutoff);
        float xHat = alpha * x + (1.0f - alpha) * xPrev_;
        xPrev_ = xHat; dxPrev_ = dxHat;
        return xHat;
    }
private:
    static float Alpha(float dt, float cutoff) {
        float tau = 1.0f / (2.0f * kPi * cutoff);
        return 1.0f / (1.0f + tau / dt);
    }
    float minCutoff_ = 1.0f, beta_ = 0.005f, dCutoff_ = 1.0f;
    float xPrev_ = 0.0f, dxPrev_ = 0.0f;
    bool hasPrev_ = false;
};

class OneEuroFilter3D {
public:
    void Init(float minCutoff = 1.0f, float beta = 0.005f, float dCutoff = 1.0f) {
        fx_.Init(minCutoff, beta, dCutoff);
        fy_.Init(minCutoff, beta, dCutoff);
        fz_.Init(minCutoff, beta, dCutoff);
    }
    XMFLOAT3 Filter(const XMFLOAT3& v, float dt) {
        return {fx_.Filter(v.x, dt), fy_.Filter(v.y, dt), fz_.Filter(v.z, dt)};
    }
private:
    OneEuroFilter1D fx_, fy_, fz_;
};

class OneEuroFilterQuat {
public:
    void Init(float minCutoff = 1.0f, float beta = 0.005f, float dCutoff = 1.0f) {
        fx_.Init(minCutoff, beta, dCutoff);
        fy_.Init(minCutoff, beta, dCutoff);
        fz_.Init(minCutoff, beta, dCutoff);
        fw_.Init(minCutoff, beta, dCutoff);
        hasPrev_ = false;
    }
    XMFLOAT4 Filter(XMFLOAT4 q, float dt) {
        if (hasPrev_) {
            // Quaternion sign continuity
            float dot = q.x * prevQ_.x + q.y * prevQ_.y + q.z * prevQ_.z + q.w * prevQ_.w;
            if (dot < 0.0f) {
                q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w;
            }
        }
        XMFLOAT4 res{
            fx_.Filter(q.x, dt),
            fy_.Filter(q.y, dt),
            fz_.Filter(q.z, dt),
            fw_.Filter(q.w, dt)
        };
        res = QuatNormalize(res);
        prevQ_ = res;
        hasPrev_ = true;
        return res;
    }
private:
    OneEuroFilter1D fx_, fy_, fz_, fw_;
    XMFLOAT4 prevQ_{0, 0, 0, 1};
    bool hasPrev_ = false;
};

// ----------------------------------------------------------------------------
// Model Bone Finder
// ----------------------------------------------------------------------------
struct SkeletonCalibration {
    int bRoot = -1;      // 全ての親
    int bCenter = -1;    // センター
    int bLower = -1;     // 下半身
    int bUpper = -1;     // 上半身
    int bUpper2 = -1;    // 上半身2 (optional)
    int bNeck = -1;      // 首
    int bHead = -1;      // 頭
    int bEyes = -1;      // 両目

    // Left arm
    int bLeftShoulder = -1, bLeftArm = -1, bLeftElbow = -1, bLeftWrist = -1;
    // Right arm
    int bRightShoulder = -1, bRightArm = -1, bRightElbow = -1, bRightWrist = -1;

    // Left leg
    int bLeftLeg = -1, bLeftKnee = -1, bLeftAnkle = -1, bLeftToe = -1;
    // Right leg
    int bRightLeg = -1, bRightKnee = -1, bRightAnkle = -1, bRightToe = -1;

    // IK bones
    int bLeftLegIk = -1, bRightLegIk = -1, bLeftToeIk = -1, bRightToeIk = -1;

    // Fingers: [hand: 0=Left, 1=Right][finger: 0=Thumb, 1=Index, 2=Middle, 3=Ring, 4=Pinky][joint: 0..2]
    int fingers[2][5][3]{};

    // Rest vectors & references
    XMFLOAT3 restArm[2]{};        // upper arm segment
    XMFLOAT3 restForearm[2]{};    // forearm segment
    XMFLOAT3 restElbowRef[2]{};   // elbow hinge reference
    XMFLOAT3 restPalm[2]{};       // wrist to middle MCP
    XMFLOAT3 restAcross[2]{};     // index MCP to pinky MCP

    XMFLOAT3 restLeg[2]{};        // thigh segment
    XMFLOAT3 restKnee[2]{};       // calf segment
    XMFLOAT3 restKneeRef[2]{};    // knee hinge reference
    XMFLOAT3 restFoot[2]{};       // ankle to toe segment

    XMFLOAT3 restHipSpan{};       // RightHip -> LeftHip
    XMFLOAT3 restSpine{};         // Hips -> Shoulders
    XMFLOAT3 restShoulderSpan{};  // RightShoulder -> LeftShoulder

    // Finger curl axes & rest angles: [hand][finger][joint]
    XMFLOAT3 fingerCurlAxis[2][5][3]{};
    float fingerRestAngle[2][5][3]{};
};

int FindBone(const PmxModel& model, const std::vector<std::string>& candidates) {
    for (const auto& name : candidates) {
        for (size_t i = 0; i < model.bones.size(); ++i) {
            if (model.bones[i].name == name) return (int)i;
        }
    }
    return -1;
}

int FindMorph(const PmxModel& model, const std::vector<std::string>& candidates) {
    for (const auto& name : candidates) {
        for (size_t i = 0; i < model.morphs.size(); ++i) {
            if (model.morphs[i].name == name) return (int)i;
        }
    }
    return -1;
}

void CalibrateSkeleton(const PmxModel& model, SkeletonCalibration& skel, SolveReport* report) {
    auto trackBone = [&](int idx, const std::string& name) {
        if (idx < 0 && report) report->missingBones.push_back(name);
        return idx;
    };

    skel.bRoot = FindBone(model, {"全ての親"});
    skel.bCenter = trackBone(FindBone(model, {"センター"}), "センター");
    skel.bLower = trackBone(FindBone(model, {"下半身"}), "下半身");
    skel.bUpper = trackBone(FindBone(model, {"上半身"}), "上半身");
    skel.bUpper2 = FindBone(model, {"上半身2"}); // optional
    skel.bNeck = trackBone(FindBone(model, {"首"}), "首");
    skel.bHead = trackBone(FindBone(model, {"頭"}), "頭");
    skel.bEyes = FindBone(model, {"両目"}); // optional

    skel.bLeftShoulder = FindBone(model, {"左肩"});
    skel.bLeftArm = trackBone(FindBone(model, {"左腕"}), "左腕");
    skel.bLeftElbow = trackBone(FindBone(model, {"左ひじ"}), "左ひじ");
    skel.bLeftWrist = trackBone(FindBone(model, {"左手首"}), "左手首");

    skel.bRightShoulder = FindBone(model, {"右肩"});
    skel.bRightArm = trackBone(FindBone(model, {"右腕"}), "右腕");
    skel.bRightElbow = trackBone(FindBone(model, {"右ひじ"}), "右ひじ");
    skel.bRightWrist = trackBone(FindBone(model, {"右手首"}), "右手首");

    skel.bLeftLeg = trackBone(FindBone(model, {"左足"}), "左足");
    skel.bLeftKnee = trackBone(FindBone(model, {"左ひざ"}), "左ひざ");
    skel.bLeftAnkle = trackBone(FindBone(model, {"左足首"}), "左足首");
    skel.bLeftToe = FindBone(model, {"左つま先"});

    skel.bRightLeg = trackBone(FindBone(model, {"右足"}), "右足");
    skel.bRightKnee = trackBone(FindBone(model, {"右ひざ"}), "右ひざ");
    skel.bRightAnkle = trackBone(FindBone(model, {"右足首"}), "右足首");
    skel.bRightToe = FindBone(model, {"右つま先"});

    skel.bLeftLegIk = FindBone(model, {"左足ＩＫ", "左足IK"});
    skel.bRightLegIk = FindBone(model, {"右足ＩＫ", "右足IK"});
    skel.bLeftToeIk = FindBone(model, {"左つま先ＩＫ", "左つま先IK"});
    skel.bRightToeIk = FindBone(model, {"右つま先ＩＫ", "右つま先IK"});

    // Finger bones:
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
                std::string n1 = std::string(prefix[h]) + fingerNames[f][j];
                std::string n2 = std::string(prefix[h]) + altFingerNames[f][j];
                skel.fingers[h][f][j] = FindBone(model, {n1, n2});
            }
        }
    }

    // Compute rest vectors
    auto getRest = [&](int idx, const XMFLOAT3& def = {0, 0, 0}) -> XMFLOAT3 {
        if (idx >= 0 && idx < (int)model.bones.size()) return model.bones[idx].position;
        return def;
    };

    // Torso rest
    XMFLOAT3 lHipRest = getRest(skel.bLeftLeg, {0.8f, 12.0f, 0});
    XMFLOAT3 rHipRest = getRest(skel.bRightLeg, {-0.8f, 12.0f, 0});
    skel.restHipSpan = Sub(lHipRest, rHipRest); // Right -> Left (+X)

    XMFLOAT3 lArmRest = getRest(skel.bLeftArm, {1.2f, 16.8f, 0});
    XMFLOAT3 rArmRest = getRest(skel.bRightArm, {-1.2f, 16.8f, 0});
    skel.restShoulderSpan = Sub(lArmRest, rArmRest);

    XMFLOAT3 midHips = Mul(Add(lHipRest, rHipRest), 0.5f);
    XMFLOAT3 midShoulders = Mul(Add(lArmRest, rArmRest), 0.5f);
    skel.restSpine = Sub(midShoulders, midHips);

    // Arms rest
    int armBones[2][3] = {
        {skel.bLeftArm, skel.bLeftElbow, skel.bLeftWrist},
        {skel.bRightArm, skel.bRightElbow, skel.bRightWrist}
    };
    for (int h = 0; h < 2; ++h) {
        XMFLOAT3 pArm = getRest(armBones[h][0]);
        XMFLOAT3 pElbow = getRest(armBones[h][1]);
        XMFLOAT3 pWrist = getRest(armBones[h][2]);
        skel.restArm[h] = Sub(pElbow, pArm);
        skel.restForearm[h] = Sub(pWrist, pElbow);

        // Elbow bend reference
        XMFLOAT3 c = Cross(skel.restArm[h], skel.restForearm[h]);
        if (Length(c) < 0.05f) {
            // Straight arm: natural bend is forward (-Z)
            skel.restElbowRef[h] = {0, 0, -1.0f};
        } else {
            skel.restElbowRef[h] = Normalize(c);
        }

        // Palm rest: wrist to middle MCP
        int bMid = skel.fingers[h][2][0];
        int bIdx = skel.fingers[h][1][0];
        int bPnk = skel.fingers[h][4][0];
        if (bMid >= 0 && bIdx >= 0 && bPnk >= 0) {
            skel.restPalm[h] = Sub(getRest(bMid), pWrist);
            skel.restAcross[h] = Sub(getRest(bIdx), getRest(bPnk));
        } else {
            skel.restPalm[h] = skel.restForearm[h];
            skel.restAcross[h] = (h == 0) ? XMFLOAT3{0, 1, 0} : XMFLOAT3{0, -1, 0};
        }

        // Finger curl axes & rest angles
        for (int f = 0; f < 5; ++f) {
            int b0 = skel.fingers[h][f][0];
            int b1 = skel.fingers[h][f][1];
            int b2 = skel.fingers[h][f][2];

            XMFLOAT3 p0 = (b0 >= 0) ? getRest(b0) : pWrist;
            XMFLOAT3 p1 = (b1 >= 0) ? getRest(b1) : Add(p0, {0.2f, 0, 0});
            XMFLOAT3 p2 = (b2 >= 0) ? getRest(b2) : Add(p1, {0.2f, 0, 0});
            XMFLOAT3 p3 = Add(p2, Mul(Sub(p2, p1), 0.7f));

            XMFLOAT3 s_wrist = Sub(p0, pWrist);
            XMFLOAT3 s0 = Sub(p1, p0);
            XMFLOAT3 s1 = Sub(p2, p1);
            XMFLOAT3 s2 = Sub(p3, p2);

            skel.fingerRestAngle[h][f][0] = std::acos(std::clamp(Dot(Normalize(s_wrist), Normalize(s0)), -1.0f, 1.0f));
            skel.fingerRestAngle[h][f][1] = std::acos(std::clamp(Dot(Normalize(s0), Normalize(s1)), -1.0f, 1.0f));
            skel.fingerRestAngle[h][f][2] = 0.0f;

            for (int j = 0; j < 3; ++j) {
                int bCurr = skel.fingers[h][f][j];
                if (bCurr < 0) continue;
                XMFLOAT3 seg = (j == 0) ? s0 : ((j == 1) ? s1 : s2);
                XMFLOAT3 segDir = Normalize(seg);
                if (f == 0) {
                    XMFLOAT3 midPos = (bMid >= 0) ? getRest(bMid) : Add(pWrist, {0, 0, 0.5f});
                    XMFLOAT3 thumbToMid = Normalize(Sub(midPos, getRest(bCurr)));
                    skel.fingerCurlAxis[h][f][j] = Normalize(Cross(segDir, thumbToMid));
                } else {
                    skel.fingerCurlAxis[h][f][j] = (h == 0) ? XMFLOAT3{0, 0, -1.0f} : XMFLOAT3{0, 0, 1.0f};
                }
            }
        }
    }

    // Legs rest
    int legBones[2][4] = {
        {skel.bLeftLeg, skel.bLeftKnee, skel.bLeftAnkle, skel.bLeftToe},
        {skel.bRightLeg, skel.bRightKnee, skel.bRightAnkle, skel.bRightToe}
    };
    for (int h = 0; h < 2; ++h) {
        XMFLOAT3 pLeg = getRest(legBones[h][0]);
        XMFLOAT3 pKnee = getRest(legBones[h][1]);
        XMFLOAT3 pAnkle = getRest(legBones[h][2]);
        XMFLOAT3 pToe = (legBones[h][3] >= 0) ? getRest(legBones[h][3]) : Add(pAnkle, {0, -1.0f, -1.5f});

        skel.restLeg[h] = Sub(pKnee, pLeg);
        skel.restKnee[h] = Sub(pAnkle, pKnee);
        skel.restFoot[h] = Sub(pToe, pAnkle);

        XMFLOAT3 c = Cross(skel.restLeg[h], skel.restKnee[h]);
        if (Length(c) < 0.05f) {
            // Straight leg: knee bends backwards, so axis is lateral
            skel.restKneeRef[h] = (h == 0) ? XMFLOAT3{1, 0, 0} : XMFLOAT3{-1, 0, 0};
        } else {
            skel.restKneeRef[h] = Normalize(c);
        }
    }
}

// ----------------------------------------------------------------------------
// Morph Table & Matcher
// ----------------------------------------------------------------------------
struct MorphMapping {
    int mBlink = -1;      // まばたき
    int mWink = -1;       // ウィンク (Left eye closes)
    int mWinkR = -1;      // ウィンク右 (Right eye closes)
    int mA = -1;          // あ
    int mI = -1;          // い
    int mU = -1;          // う
    int mE = -1;          // え
    int mO = -1;          // お
    int mSmile = -1;      // 笑い
    int mBrowUp = -1;     // 上
    int mBrowTrouble = -1;// 困る
    int mBrowAnger = -1;  // 怒り
};

void CalibrateMorphs(const PmxModel& model, MorphMapping& morphs, SolveReport* report) {
    auto trackMorph = [&](int idx, const std::string& name) {
        if (idx < 0 && report) report->missingMorphs.push_back(name);
        return idx;
    };

    morphs.mBlink = trackMorph(FindMorph(model, {"まばたき"}), "まばたき");
    morphs.mWink = trackMorph(FindMorph(model, {"ウィンク"}), "ウィンク");
    morphs.mWinkR = trackMorph(FindMorph(model, {"ウィンク右"}), "ウィンク右");

    morphs.mA = trackMorph(FindMorph(model, {"あ", "あ2"}), "あ");
    morphs.mI = trackMorph(FindMorph(model, {"い", "い2"}), "い");
    morphs.mU = trackMorph(FindMorph(model, {"う", "う2"}), "う");
    morphs.mE = trackMorph(FindMorph(model, {"え", "え2"}), "え");
    morphs.mO = trackMorph(FindMorph(model, {"お", "お2"}), "お");

    morphs.mSmile = trackMorph(FindMorph(model, {"笑い"}), "笑い");
    morphs.mBrowUp = trackMorph(FindMorph(model, {"上"}), "上");
    morphs.mBrowTrouble = trackMorph(FindMorph(model, {"困る"}), "困る");
    morphs.mBrowAnger = FindMorph(model, {"怒り"}); // optional, Ichika doesn't have it
    if (morphs.mBrowAnger < 0 && report) report->missingMorphs.push_back("怒り");
}

// ----------------------------------------------------------------------------
// Key Decimation (Ramer-Douglas-Peucker)
// ----------------------------------------------------------------------------
void DecimateBoneKeys(const std::vector<VmdBoneKey>& inKeys, float rotTolDeg, float transTol,
                      std::vector<VmdBoneKey>& outKeys) {
    if (inKeys.size() <= 2) {
        outKeys = inKeys;
        return;
    }

    std::vector<bool> keep(inKeys.size(), false);
    keep.front() = true;
    keep.back() = true;

    auto rdp = [&](auto& self, size_t first, size_t last) -> void {
        if (last <= first + 1) return;

        float maxErr = 0.0f;
        size_t maxIdx = first;

        float f0 = (float)inKeys[first].frame;
        float f1 = (float)inKeys[last].frame;
        float fSpan = f1 - f0;
        if (fSpan <= 0.0f) return;

        for (size_t i = first + 1; i < last; ++i) {
            float t = ((float)inKeys[i].frame - f0) / fSpan;

            // Interpolate translation
            XMFLOAT3 tInterp{
                (1.0f - t) * inKeys[first].translation.x + t * inKeys[last].translation.x,
                (1.0f - t) * inKeys[first].translation.y + t * inKeys[last].translation.y,
                (1.0f - t) * inKeys[first].translation.z + t * inKeys[last].translation.z
            };
            float tErr = Length(Sub(inKeys[i].translation, tInterp));

            // Interpolate rotation (Slerp)
            XMVECTOR q0 = XMLoadFloat4(&inKeys[first].rotation);
            XMVECTOR q1 = XMLoadFloat4(&inKeys[last].rotation);
            XMVECTOR qInterp = XMQuaternionSlerp(q0, q1, t);
            XMFLOAT4 qI;
            XMStoreFloat4(&qI, qInterp);
            float rErr = QuatAngleDeg(inKeys[i].rotation, qI);

            // Normalized error relative to tolerances
            float errNorm = std::max(rErr / rotTolDeg, tErr / transTol);
            if (errNorm > maxErr) {
                maxErr = errNorm;
                maxIdx = i;
            }
        }

        if (maxErr > 1.0f) {
            keep[maxIdx] = true;
            self(self, first, maxIdx);
            self(self, maxIdx, last);
        }
    };

    rdp(rdp, 0, inKeys.size() - 1);

    for (size_t i = 0; i < inKeys.size(); ++i) {
        if (keep[i]) outKeys.push_back(inKeys[i]);
    }
}

void DecimateMorphKeys(const std::vector<VmdMorphKey>& inKeys, float tol,
                       std::vector<VmdMorphKey>& outKeys) {
    if (inKeys.size() <= 2) {
        outKeys = inKeys;
        return;
    }

    std::vector<bool> keep(inKeys.size(), false);
    keep.front() = true;
    keep.back() = true;

    auto rdp = [&](auto& self, size_t first, size_t last) -> void {
        if (last <= first + 1) return;

        float maxErr = 0.0f;
        size_t maxIdx = first;

        float f0 = (float)inKeys[first].frame;
        float f1 = (float)inKeys[last].frame;
        float fSpan = f1 - f0;
        if (fSpan <= 0.0f) return;

        for (size_t i = first + 1; i < last; ++i) {
            float t = ((float)inKeys[i].frame - f0) / fSpan;
            float wInterp = (1.0f - t) * inKeys[first].weight + t * inKeys[last].weight;
            float err = std::abs(inKeys[i].weight - wInterp);
            if (err > maxErr) {
                maxErr = err;
                maxIdx = i;
            }
        }

        if (maxErr > tol) {
            keep[maxIdx] = true;
            self(self, first, maxIdx);
            self(self, maxIdx, last);
        }
    };

    rdp(rdp, 0, inKeys.size() - 1);

    for (size_t i = 0; i < inKeys.size(); ++i) {
        if (keep[i]) outKeys.push_back(inKeys[i]);
    }
}

} // namespace

// ----------------------------------------------------------------------------
// SolveMotion implementation
// ----------------------------------------------------------------------------
bool SolveMotion(const MocapClip& clip, const PmxModel& model, const SolveOptions& opts, VmdMotion& out,
                 SolveReport* report, std::string* error) {
    if (clip.frames.empty()) {
        if (error) *error = "Empty mocap clip";
        return false;
    }

    SolveReport rep;
    SkeletonCalibration skel;
    CalibrateSkeleton(model, skel, &rep);

    MorphMapping morphs;
    CalibrateMorphs(model, morphs, &rep);

    out.modelName = model.name;
    out.boneKeys.clear();
    out.morphKeys.clear();
    out.cameraKeys.clear();
    out.lightKeys.clear();
    out.shadowKeys.clear();
    out.ikKeys.clear();

    // Foot IK off key at frame 0
    VmdIkKey ik0;
    ik0.frame = 0;
    ik0.visible = true;
    ik0.ikStates.push_back({"左足ＩＫ", false});
    ik0.ikStates.push_back({"右足ＩＫ", false});
    if (skel.bLeftToeIk >= 0) ik0.ikStates.push_back({"左つま先ＩＫ", false});
    if (skel.bRightToeIk >= 0) ik0.ikStates.push_back({"右つま先ＩＫ", false});
    out.ikKeys.push_back(ik0);

    // Find first valid frame
    int firstValidIdx = -1;
    for (size_t i = 0; i < clip.frames.size(); ++i) {
        if (clip.frames[i].poseValid) {
            firstValidIdx = (int)i;
            break;
        }
    }
    if (firstValidIdx < 0) {
        if (error) *error = "No valid pose frames in clip";
        return false;
    }

    const double t0 = clip.frames[0].time;
    const double firstValidTime = clip.frames[firstValidIdx].time;

    // Filters for smoothing
    float minCutoff = (opts.smoothing > 0.0f) ? (1.0f / (1.0f + 8.0f * opts.smoothing)) : 100.0f;
    float beta = (opts.smoothing > 0.0f) ? (0.01f / (1.0f + 4.0f * opts.smoothing)) : 0.0f;
    float dt = 1.0f / 30.0f;

    std::unordered_map<std::string, OneEuroFilterQuat> boneQuatFilters;
    OneEuroFilter3D rootPosFilter;
    rootPosFilter.Init(minCutoff, beta);

    // Tracks to collect unreduced keys
    std::unordered_map<std::string, std::vector<VmdBoneKey>> rawBoneTracks;
    std::unordered_map<std::string, std::vector<VmdMorphKey>> rawMorphTracks;

    // Root scale calibration. Image coordinates are normalised per axis: work in pixels.
    const float imgW = clip.videoWidth > 0 ? (float)clip.videoWidth : 1.0f;
    const float imgH = clip.videoHeight > 0 ? (float)clip.videoHeight : 1.0f;
    auto toPx = [&](const XMFLOAT2& q) { return XMFLOAT2{q.x * imgW, q.y * imgH}; };
    const MocapFrame& f0Ref = clip.frames[firstValidIdx];
    XMFLOAT2 hipImg0 = toPx(Mul(Add(f0Ref.poseImage[LeftHip], f0Ref.poseImage[RightHip]), 0.5f));
    XMFLOAT2 shImg0 = toPx(Mul(Add(f0Ref.poseImage[LeftShoulder], f0Ref.poseImage[RightShoulder]), 0.5f));
    float lImg0 = std::max(0.01f, std::sqrt((shImg0.x - hipImg0.x) * (shImg0.x - hipImg0.x) +
                                            (shImg0.y - hipImg0.y) * (shImg0.y - hipImg0.y)));
    float lWorld = Length(skel.restSpine);
    if (lWorld < 0.1f) lWorld = 3.5f;
    float imgScale = lWorld / lImg0;

    // Helper to evaluate a frame's pose
    auto solveFramePose = [&](const MocapFrame& mf, uint32_t vmdFrame) {
        // 1. Torso: 下半身 & 上半身
        XMFLOAT3 lHip = mf.pose[LeftHip];
        XMFLOAT3 rHip = mf.pose[RightHip];
        XMFLOAT3 hipSpan = Sub(lHip, rHip);
        XMFLOAT3 hipMid = Mul(Add(lHip, rHip), 0.5f);

        XMFLOAT3 lSh = mf.pose[LeftShoulder];
        XMFLOAT3 rSh = mf.pose[RightShoulder];
        XMFLOAT3 shSpan = Sub(lSh, rSh);
        XMFLOAT3 shMid = Mul(Add(lSh, rSh), 0.5f);

        XMFLOAT3 spine = Sub(shMid, hipMid);
        XMFLOAT3 forwardHip = Cross(spine, hipSpan);
        XMFLOAT3 forwardSh = Cross(spine, shSpan);

        XMFLOAT4 qLowerWorld = RotationBetweenFrames(skel.restHipSpan, {0, 0, -1.0f}, hipSpan, forwardHip);
        XMFLOAT4 qChestWorld = RotationBetweenFrames(skel.restShoulderSpan, {0, 0, -1.0f}, shSpan, forwardSh);

        // Lower body key
        XMFLOAT4 qLowerLocal = qLowerWorld; // parent is groove/center/waist at identity
        if (opts.smoothing > 0.0f) qLowerLocal = boneQuatFilters["下半身"].Filter(qLowerLocal, dt);

        VmdBoneKey kLower;
        kLower.boneName = "下半身";
        kLower.frame = vmdFrame;
        kLower.rotation = qLowerLocal;
        FillLinearInterp(kLower.interp);
        rawBoneTracks["下半身"].push_back(kLower);

        // Upper body bend sharing
        int upperParent = (skel.bUpper >= 0) ? model.bones[skel.bUpper].parentIndex : -1;
        bool upperChildOfLower = (upperParent == skel.bLower);
        XMFLOAT4 qUpperTotal = upperChildOfLower ? QuatMultiply(qChestWorld, QuatInverse(qLowerWorld)) : qChestWorld;

        if (skel.bUpper2 >= 0) {
            // Split 50/50
            XMFLOAT4 qHalf;
            XMStoreFloat4(&qHalf, XMQuaternionSlerp(XMQuaternionIdentity(), XMLoadFloat4(&qUpperTotal), 0.5f));
            XMFLOAT4 qUpper1 = qHalf;
            XMFLOAT4 qUpper2 = QuatMultiply(qUpperTotal, QuatInverse(qHalf));

            if (opts.smoothing > 0.0f) {
                qUpper1 = boneQuatFilters["上半身"].Filter(qUpper1, dt);
                qUpper2 = boneQuatFilters["上半身2"].Filter(qUpper2, dt);
            }

            VmdBoneKey kU1;
            kU1.boneName = "上半身";
            kU1.frame = vmdFrame;
            kU1.rotation = qUpper1;
            FillLinearInterp(kU1.interp);
            rawBoneTracks["上半身"].push_back(kU1);

            VmdBoneKey kU2;
            kU2.boneName = "上半身2";
            kU2.frame = vmdFrame;
            kU2.rotation = qUpper2;
            FillLinearInterp(kU2.interp);
            rawBoneTracks["上半身2"].push_back(kU2);
        } else {
            XMFLOAT4 qUpper = qUpperTotal;
            if (opts.smoothing > 0.0f) qUpper = boneQuatFilters["上半身"].Filter(qUpper, dt);

            VmdBoneKey kU;
            kU.boneName = "上半身";
            kU.frame = vmdFrame;
            kU.rotation = qUpper;
            FillLinearInterp(kU.interp);
            rawBoneTracks["上半身"].push_back(kU);
        }

        // 2. Head & Neck
        if (opts.head) {
            XMFLOAT3 headSpan, headForward;
            if (mf.faceValid && mf.faceLandmarks.size() >= 300) {
                // Face landmarks (MocapTypes.h: normalised image x/y with y DOWN, z in x's scale): to isotropic
                // pixel units with +Y up / +Z away from the camera. 263 / 33 = the performer's left / right eye
                // outer corner, 10 = forehead top, 152 = chin.
                auto facePt = [&](int i) {
                    const XMFLOAT3& q = mf.faceLandmarks[i];
                    return XMFLOAT3{q.x * imgW, -q.y * imgH, q.z * imgW};
                };
                headSpan = Sub(facePt(263), facePt(33));
                XMFLOAT3 upF = Sub(facePt(10), facePt(152));
                headForward = Cross(upF, headSpan);
            } else {
                // Pose landmarks: LeftEar (7), RightEar (8), Nose (0)
                headSpan = Sub(mf.pose[LeftEar], mf.pose[RightEar]);
                XMFLOAT3 earMid = Mul(Add(mf.pose[LeftEar], mf.pose[RightEar]), 0.5f);
                headForward = Sub(mf.pose[Nose], earMid);
            }

            XMFLOAT4 qHeadWorld = RotationBetweenFrames({1, 0, 0}, {0, 0, -1}, headSpan, headForward);
            XMFLOAT4 qHeadRel = QuatMultiply(qHeadWorld, QuatInverse(qChestWorld));

            // Distribute 25% to Neck, 75% to Head
            XMFLOAT4 qNeckLocal, qHeadLocal;
            XMStoreFloat4(&qNeckLocal, XMQuaternionSlerp(XMQuaternionIdentity(), XMLoadFloat4(&qHeadRel), 0.25f));
            qHeadLocal = QuatMultiply(qHeadRel, QuatInverse(qNeckLocal));

            if (opts.smoothing > 0.0f) {
                qNeckLocal = boneQuatFilters["首"].Filter(qNeckLocal, dt);
                qHeadLocal = boneQuatFilters["頭"].Filter(qHeadLocal, dt);
            }

            VmdBoneKey kNeck;
            kNeck.boneName = "首";
            kNeck.frame = vmdFrame;
            kNeck.rotation = qNeckLocal;
            FillLinearInterp(kNeck.interp);
            rawBoneTracks["首"].push_back(kNeck);

            VmdBoneKey kHead;
            kHead.boneName = "頭";
            kHead.frame = vmdFrame;
            kHead.rotation = qHeadLocal;
            FillLinearInterp(kHead.interp);
            rawBoneTracks["頭"].push_back(kHead);
        }

        // 3. Arms: 腕, ひじ, 手首
        XMFLOAT4 wristWorldHand[2] = {QuatIdentity(), QuatIdentity()};
        if (opts.body) {
            const char* armNames[2] = {"左腕", "右腕"};
            const char* elbowNames[2] = {"左ひじ", "右ひじ"};
            const char* wristNames[2] = {"左手首", "右手首"};
            int poseSh[2] = {LeftShoulder, RightShoulder};
            int poseEl[2] = {LeftElbow, RightElbow};
            int poseWr[2] = {LeftWrist, RightWrist};

            for (int h = 0; h < 2; ++h) {
                XMFLOAT3 pSh = mf.pose[poseSh[h]];
                XMFLOAT3 pEl = mf.pose[poseEl[h]];
                XMFLOAT3 pWr = mf.pose[poseWr[h]];

                XMFLOAT3 armSeg = Sub(pEl, pSh);
                XMFLOAT3 elSeg = Sub(pWr, pEl);

                XMFLOAT3 elBend = Cross(armSeg, elSeg);
                if (Length(elBend) < 0.05f) {
                    elBend = forwardSh; // straight arm fallback to chest forward
                }

                // Upper arm
                XMFLOAT4 qArmWorld = RotationBetweenFrames(skel.restArm[h], skel.restElbowRef[h], armSeg, elBend);
                XMFLOAT4 qArmLocal = QuatMultiply(qArmWorld, QuatInverse(qChestWorld));
                if (opts.smoothing > 0.0f) qArmLocal = boneQuatFilters[armNames[h]].Filter(qArmLocal, dt);

                VmdBoneKey kArm;
                kArm.boneName = armNames[h];
                kArm.frame = vmdFrame;
                kArm.rotation = qArmLocal;
                FillLinearInterp(kArm.interp);
                rawBoneTracks[armNames[h]].push_back(kArm);

                // Elbow / forearm
                XMFLOAT4 qElbowWorld = RotationBetweenFrames(skel.restForearm[h], skel.restElbowRef[h], elSeg, elBend);
                XMFLOAT4 qElbowLocal = QuatMultiply(qElbowWorld, QuatInverse(qArmWorld));
                if (opts.smoothing > 0.0f) qElbowLocal = boneQuatFilters[elbowNames[h]].Filter(qElbowLocal, dt);

                VmdBoneKey kElbow;
                kElbow.boneName = elbowNames[h];
                kElbow.frame = vmdFrame;
                kElbow.rotation = qElbowLocal;
                FillLinearInterp(kElbow.interp);
                rawBoneTracks[elbowNames[h]].push_back(kElbow);

                // Wrist
                XMFLOAT4 qWristWorld = qElbowWorld; // fallback
                if (mf.handValid[h]) {
                    const auto& lm = mf.hand[h];
                    XMFLOAT3 palmVec = Sub(lm[MiddleMcp], lm[Wrist]);
                    XMFLOAT3 acrossVec = Sub(lm[IndexMcp], lm[PinkyMcp]);
                    XMFLOAT3 palmNormal = Cross(palmVec, acrossVec);
                    qWristWorld = RotationBetweenFrames(skel.restPalm[h], skel.restAcross[h], palmVec, acrossVec);
                }
                wristWorldHand[h] = qWristWorld;
                XMFLOAT4 qWristLocal = QuatMultiply(qWristWorld, QuatInverse(qElbowWorld));
                if (opts.smoothing > 0.0f) qWristLocal = boneQuatFilters[wristNames[h]].Filter(qWristLocal, dt);

                VmdBoneKey kWrist;
                kWrist.boneName = wristNames[h];
                kWrist.frame = vmdFrame;
                kWrist.rotation = qWristLocal;
                FillLinearInterp(kWrist.interp);
                rawBoneTracks[wristNames[h]].push_back(kWrist);
            }
        }

        // 4. Legs: 足, ひざ, 足首
        if (opts.body) {
            const char* legNames[2] = {"左足", "右足"};
            const char* kneeNames[2] = {"左ひざ", "右ひざ"};
            const char* ankleNames[2] = {"左足首", "右足首"};
            int poseHip[2] = {LeftHip, RightHip};
            int poseKn[2] = {LeftKnee, RightKnee};
            int poseAnk[2] = {LeftAnkle, RightAnkle};
            int poseToe[2] = {LeftFootIndex, RightFootIndex};

            for (int h = 0; h < 2; ++h) {
                XMFLOAT3 pHp = mf.pose[poseHip[h]];
                XMFLOAT3 pKn = mf.pose[poseKn[h]];
                XMFLOAT3 pAn = mf.pose[poseAnk[h]];
                XMFLOAT3 pTo = mf.pose[poseToe[h]];

                XMFLOAT3 legSeg = Sub(pKn, pHp);
                XMFLOAT3 kneeSeg = Sub(pAn, pKn);
                XMFLOAT3 footSeg = Sub(pTo, pAn);

                XMFLOAT3 kneeBend = Cross(legSeg, kneeSeg);
                if (Length(kneeBend) < 0.05f) {
                    kneeBend = (h == 0) ? XMFLOAT3{1, 0, 0} : XMFLOAT3{-1, 0, 0};
                }

                // Thigh
                XMFLOAT4 qLegWorld = RotationBetweenFrames(skel.restLeg[h], skel.restKneeRef[h], legSeg, kneeBend);
                XMFLOAT4 qLegLocal = QuatMultiply(qLegWorld, QuatInverse(qLowerWorld));
                if (opts.smoothing > 0.0f) qLegLocal = boneQuatFilters[legNames[h]].Filter(qLegLocal, dt);

                VmdBoneKey kLeg;
                kLeg.boneName = legNames[h];
                kLeg.frame = vmdFrame;
                kLeg.rotation = qLegLocal;
                FillLinearInterp(kLeg.interp);
                rawBoneTracks[legNames[h]].push_back(kLeg);

                // Knee / calf
                XMFLOAT4 qKneeWorld = RotationBetweenFrames(skel.restKnee[h], skel.restKneeRef[h], kneeSeg, kneeBend);
                XMFLOAT4 qKneeLocal = QuatMultiply(qKneeWorld, QuatInverse(qLegWorld));
                if (opts.smoothing > 0.0f) qKneeLocal = boneQuatFilters[kneeNames[h]].Filter(qKneeLocal, dt);

                VmdBoneKey kKnee;
                kKnee.boneName = kneeNames[h];
                kKnee.frame = vmdFrame;
                kKnee.rotation = qKneeLocal;
                FillLinearInterp(kKnee.interp);
                rawBoneTracks[kneeNames[h]].push_back(kKnee);

                // Ankle / foot
                XMFLOAT4 qAnkleWorld = RotationBetweenFrames(skel.restFoot[h], {0, 1, 0}, footSeg, kneeBend);
                XMFLOAT4 qAnkleLocal = QuatMultiply(qAnkleWorld, QuatInverse(qKneeWorld));
                if (opts.smoothing > 0.0f) qAnkleLocal = boneQuatFilters[ankleNames[h]].Filter(qAnkleLocal, dt);

                VmdBoneKey kAnkle;
                kAnkle.boneName = ankleNames[h];
                kAnkle.frame = vmdFrame;
                kAnkle.rotation = qAnkleLocal;
                FillLinearInterp(kAnkle.interp);
                rawBoneTracks[ankleNames[h]].push_back(kAnkle);
            }
        }

        // 5. Fingers
        if (opts.fingers) {
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
                if (!mf.handValid[h]) continue;
                const auto& lm = mf.hand[h];

                // Landmarks per finger: 4 points [MCP, PIP, DIP, TIP]
                int lmIndices[5][4] = {
                    {ThumbCmc, ThumbMcp, ThumbIp, ThumbTip},
                    {IndexMcp, IndexPip, IndexDip, IndexTip},
                    {MiddleMcp, MiddlePip, MiddleDip, MiddleTip},
                    {RingMcp, RingPip, RingDip, RingTip},
                    {PinkyMcp, PinkyPip, PinkyDip, PinkyTip}
                };

                for (int f = 0; f < 5; ++f) {
                    XMFLOAT3 s_prev = (f == 0)
                        ? Sub(lm[lmIndices[f][0]], lm[Wrist])
                        : Sub(lm[lmIndices[f][0]], lm[Wrist]);

                    XMFLOAT4 qParentWorld = wristWorldHand[h];

                    for (int j = 0; j < 3; ++j) {
                        int bIdx = skel.fingers[h][f][j];
                        if (bIdx < 0) continue;

                        XMFLOAT3 s_curr = Sub(lm[lmIndices[f][j + 1]], lm[lmIndices[f][j]]);

                        XMFLOAT4 qRot;
                        if (j == 0 || (f == 0 && j == 1)) {
                            // MCP joint (and Thumb MCP): direction solve to capture natural spread & curl
                            int bChild = (j < 2) ? skel.fingers[h][f][j + 1] : -1;
                            XMFLOAT3 restSeg = (bChild >= 0)
                                ? Sub(model.bones[bChild].position, model.bones[bIdx].position)
                                : ((j > 0 && skel.fingers[h][f][j - 1] >= 0)
                                    ? Sub(model.bones[bIdx].position, model.bones[skel.fingers[h][f][j - 1]].position)
                                    : XMFLOAT3{0.3f, 0, 0});
                            XMFLOAT3 targetLocal = RotateVec(s_curr, QuatInverse(qParentWorld));
                            qRot = QuatShortestArc(restSeg, targetLocal);
                        } else {
                            // PIP and DIP: 1D anatomical hinge curl
                            float cosA = Dot(Normalize(s_prev), Normalize(s_curr));
                            cosA = std::clamp(cosA, -1.0f, 1.0f);
                            float rawAngle = std::acos(cosA);
                            float curl = rawAngle - skel.fingerRestAngle[h][f][j];
                            curl = std::clamp(curl, -10.0f * kDegToRad, 100.0f * kDegToRad);

                            XMVECTOR axis = XMLoadFloat3(&skel.fingerCurlAxis[h][f][j]);
                            XMStoreFloat4(&qRot, XMQuaternionRotationAxis(axis, curl));
                        }

                        std::string bName = model.bones[bIdx].name;
                        if (opts.smoothing > 0.0f) qRot = boneQuatFilters[bName].Filter(qRot, dt);

                        VmdBoneKey kFinger;
                        kFinger.boneName = bName;
                        kFinger.frame = vmdFrame;
                        kFinger.rotation = qRot;
                        FillLinearInterp(kFinger.interp);
                        rawBoneTracks[bName].push_back(kFinger);

                        qParentWorld = QuatMultiply(qRot, qParentWorld);
                        s_prev = s_curr;
                    }
                }
            }
        }

        // 6. Root Motion (センター)
        if (opts.rootMotion) {
            XMFLOAT2 hipImg = toPx(Mul(Add(mf.poseImage[LeftHip], mf.poseImage[RightHip]), 0.5f));
            float dx = (hipImg.x - hipImg0.x) * imgScale;

            // Change of apparent scale for depth Z (damped: the weakest cue)
            XMFLOAT2 shImg = toPx(Mul(Add(mf.poseImage[LeftShoulder], mf.poseImage[RightShoulder]), 0.5f));
            float lImgCurr = std::max(0.01f, std::sqrt((shImg.x - hipImg.x) * (shImg.x - hipImg.x) +
                                                       (shImg.y - hipImg.y) * (shImg.y - hipImg.y)));
            float dz = (lImg0 / lImgCurr - 1.0f) * lWorld * 0.4f;

            // Vertical: set by the ground-contact pass after the rotations are known (FK on the model itself).
            const float yFinal = 0.0f;

            XMFLOAT3 rootPos{dx, yFinal, dz};
            if (opts.smoothing > 0.0f) rootPos = rootPosFilter.Filter(rootPos, dt);

            VmdBoneKey kCenter;
            kCenter.boneName = "センター";
            kCenter.frame = vmdFrame;
            kCenter.translation = rootPos;
            kCenter.rotation = QuatIdentity();
            FillLinearInterp(kCenter.interp);
            rawBoneTracks["センター"].push_back(kCenter);
        }

        // 7. Face morphs & Eye gaze
        if (opts.face && mf.faceValid) {
            auto blend = mf.blendshapes;
            if (opts.mirrorBlendshapeSides) {
                // Swap Left/Right shapes
                std::swap(blend[1], blend[2]);   // browDown
                std::swap(blend[4], blend[5]);   // browOuterUp
                std::swap(blend[7], blend[8]);   // cheekSquint
                std::swap(blend[9], blend[10]);  // eyeBlink
                std::swap(blend[11], blend[12]); // eyeLookDown
                std::swap(blend[13], blend[14]); // eyeLookIn
                std::swap(blend[15], blend[16]); // eyeLookOut
                std::swap(blend[17], blend[18]); // eyeLookUp
                std::swap(blend[19], blend[20]); // eyeSquint
                std::swap(blend[21], blend[22]); // eyeWide
                std::swap(blend[28], blend[29]); // mouthDimple
                std::swap(blend[30], blend[31]); // mouthFrown
                std::swap(blend[34], blend[35]); // mouthLowerDown
                std::swap(blend[36], blend[37]); // mouthPress
                std::swap(blend[44], blend[45]); // mouthSmile
                std::swap(blend[46], blend[47]); // mouthStretch
                std::swap(blend[48], blend[49]); // mouthUpperUp
            }

            // Blinks
            float blinkL = blend[9];
            float blinkR = blend[10];
            float bothBlink = std::min(blinkL, blinkR);
            float winkL = std::max(0.0f, blinkL - blinkR); // character's Left eye closes
            float winkR = std::max(0.0f, blinkR - blinkL); // character's Right eye closes

            auto addMorphKey = [&](int morphIdx, float val) {
                if (morphIdx < 0) return;
                val = std::clamp(val, 0.0f, 1.0f);
                const std::string& mName = model.morphs[morphIdx].name;
                VmdMorphKey mk;
                mk.morphName = mName;
                mk.frame = vmdFrame;
                mk.weight = val;
                rawMorphTracks[mName].push_back(mk);
            };

            addMorphKey(morphs.mBlink, bothBlink);
            addMorphKey(morphs.mWink, winkL);
            addMorphKey(morphs.mWinkR, winkR);

            // Mouth vowels: あ, い, う, え, お
            float jaw = blend[25]; // jawOpen
            float funnel = blend[32]; // mouthFunnel
            float pucker = blend[38]; // mouthPucker
            float smile = (blend[44] + blend[45]) * 0.5f;
            float stretch = (blend[46] + blend[47]) * 0.5f;

            float wA = jaw * (1.0f - funnel * 0.8f);
            float wI = std::max(stretch, smile) * (1.0f - jaw * 0.5f);
            float wU = std::max(pucker, funnel * 0.8f) * (1.0f - jaw * 0.5f);
            float wO = funnel * jaw;
            float wE = std::max(0.0f, std::min(stretch, jaw) * 1.5f);

            // Normalize vowels so sum <= 1
            float sumV = wA + wI + wU + wE + wO;
            if (sumV > 1.0f) {
                wA /= sumV; wI /= sumV; wU /= sumV; wE /= sumV; wO /= sumV;
            }

            addMorphKey(morphs.mA, wA);
            addMorphKey(morphs.mI, wI);
            addMorphKey(morphs.mU, wU);
            addMorphKey(morphs.mE, wE);
            addMorphKey(morphs.mO, wO);

            // Brows & cheeks
            float browUp = (blend[4] + blend[5]) * 0.5f;
            float browDown = (blend[1] + blend[2]) * 0.5f;
            float browTrouble = blend[3]; // browInnerUp
            float squint = std::max((blend[7] + blend[8]) * 0.5f, (blend[19] + blend[20]) * 0.5f);

            addMorphKey(morphs.mBrowUp, browUp);
            addMorphKey(morphs.mBrowTrouble, browTrouble);
            addMorphKey(morphs.mBrowAnger, browDown);
            addMorphKey(morphs.mSmile, squint);

            // Eye gaze -> 両目 bone
            if (skel.bEyes >= 0) {
                float up = (blend[17] + blend[18]) * 0.5f;
                float down = (blend[11] + blend[12]) * 0.5f;
                float lookLeft = (blend[15] + blend[14]) * 0.5f;  // outL + inR
                float lookRight = (blend[13] + blend[16]) * 0.5f; // inL + outR

                float pitch = std::clamp((up - down) * (15.0f * kDegToRad), -15.0f * kDegToRad, 15.0f * kDegToRad);
                float yaw = std::clamp((lookLeft - lookRight) * (15.0f * kDegToRad), -15.0f * kDegToRad, 15.0f * kDegToRad);

                XMFLOAT4 qEyes;
                XMStoreFloat4(&qEyes, XMQuaternionRotationRollPitchYaw(pitch, yaw, 0.0f));
                if (opts.smoothing > 0.0f) qEyes = boneQuatFilters["両目"].Filter(qEyes, dt);

                VmdBoneKey kEyes;
                kEyes.boneName = "両目";
                kEyes.frame = vmdFrame;
                kEyes.rotation = qEyes;
                FillLinearInterp(kEyes.interp);
                rawBoneTracks["両目"].push_back(kEyes);
            }
        }
    };

    // If gap at start > 1.0s, bridge frame 0 with first valid pose
    if (firstValidTime - t0 > 1.0) {
        solveFramePose(clip.frames[firstValidIdx], 0);
    }

    // Process all frames
    for (size_t i = 0; i < clip.frames.size(); ++i) {
        const MocapFrame& mf = clip.frames[i];
        if (!mf.poseValid) continue; // skip invalid frames

        uint32_t vmdFrame = (uint32_t)std::max(0.0, std::round((mf.time - t0) * 30.0));
        solveFramePose(mf, vmdFrame);
    }

    // Ground contact: pose the model with each frame's solved rotations (センター at 0, leg IK off as in the output)
    // and lift / lower センター so the lowest foot point (ankle / toe bones) sits on the floor as it does at rest.
    // Exact for any proportions, crouches included; jumps are not recovered (the feet stay planted).
    if (opts.rootMotion && opts.body) {
        auto centreIt = rawBoneTracks.find("センター");
        int feet[4] = {skel.bLeftAnkle, skel.bRightAnkle, skel.bLeftToe, skel.bRightToe};
        if (centreIt != rawBoneTracks.end() && (feet[0] >= 0 || feet[1] >= 0)) {
            float restLowest = std::numeric_limits<float>::max();
            for (int b : feet)
                if (b >= 0) restLowest = std::min(restLowest, model.bones[b].position.y);
            std::vector<int> ikBones;
            for (int b : {skel.bLeftLegIk, skel.bRightLegIk, skel.bLeftToeIk, skel.bRightToeIk})
                if (b >= 0) ikBones.push_back(b);
            std::unordered_map<std::string, int> boneIndex;
            for (size_t i = 0; i < model.bones.size(); ++i) boneIndex.emplace(model.bones[i].name, (int)i);
            // the caller owns `model` for the duration of the call: a non-owning shared_ptr is enough
            ModelInstance inst(std::shared_ptr<const PmxModel>(&model, [](const PmxModel*) {}));
            inst.EnablePhysics(false);
            struct Track { int bone; const std::vector<VmdBoneKey>* keys; size_t cursor; };
            std::vector<Track> tracks;
            for (const auto& [name, keys] : rawBoneTracks) {
                auto it = boneIndex.find(name);
                if (it != boneIndex.end() && name != "センター") tracks.push_back({it->second, &keys, 0});
            }
            for (VmdBoneKey& ck : centreIt->second) {
                inst.ResetPose();
                for (int b : ikBones) inst.SetIkEnabled(b, false);
                for (Track& t : tracks) {   // every track is keyed on the same solved frames, in frame order
                    const auto& keys = *t.keys;
                    while (t.cursor + 1 < keys.size() && keys[t.cursor + 1].frame <= ck.frame) ++t.cursor;
                    if (!keys.empty() && keys[t.cursor].frame <= ck.frame)
                        inst.SetBoneAnim(t.bone, keys[t.cursor].translation, keys[t.cursor].rotation);
                }
                inst.UpdatePose(0.0f);
                float lowest = std::numeric_limits<float>::max();
                for (int b : feet)
                    if (b >= 0) lowest = std::min(lowest, inst.BoneWorldPosition(b).y);
                ck.translation.y = restLowest - lowest;
            }
        }
    }

    // Apply keyframe decimation
    int totalBoneKeys = 0;
    for (auto& [bName, keys] : rawBoneTracks) {
        if (keys.empty()) continue;
        std::vector<VmdBoneKey> reduced;
        DecimateBoneKeys(keys, opts.rotationTolerance, opts.translationTolerance, reduced);
        out.boneKeys.insert(out.boneKeys.end(), reduced.begin(), reduced.end());
        totalBoneKeys += (int)reduced.size();
    }

    int totalMorphKeys = 0;
    for (auto& [mName, keys] : rawMorphTracks) {
        if (keys.empty()) continue;
        std::vector<VmdMorphKey> reduced;
        DecimateMorphKeys(keys, 0.01f, reduced);
        out.morphKeys.insert(out.morphKeys.end(), reduced.begin(), reduced.end());
        totalMorphKeys += (int)reduced.size();
    }

    // Sort keys by frame
    std::sort(out.boneKeys.begin(), out.boneKeys.end(), [](const VmdBoneKey& a, const VmdBoneKey& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return a.boneName < b.boneName;
    });
    std::sort(out.morphKeys.begin(), out.morphKeys.end(), [](const VmdMorphKey& a, const VmdMorphKey& b) {
        if (a.frame != b.frame) return a.frame < b.frame;
        return a.morphName < b.morphName;
    });

    uint32_t maxF = 0;
    for (const auto& k : out.boneKeys) if (k.frame > maxF) maxF = k.frame;
    for (const auto& k : out.morphKeys) if (k.frame > maxF) maxF = k.frame;
    out.maxFrame = maxF;

    rep.frames = (int)clip.frames.size();
    rep.boneKeys = totalBoneKeys;
    rep.morphKeys = totalMorphKeys;

    if (report) *report = rep;
    return true;
}

} // namespace mmdx::mocap
