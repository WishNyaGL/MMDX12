// restpose_retarget_test: numeric check of the studio's arm rest-pose correction (studio/RestPoseRetarget).
// A/B: a random motion evaluated on a synthetic T-pose twin must land where the retargeted motion lands on the
// A-pose model. C: an un-keyed arm gets a frame-0 key. D: the T-pose detection threshold. E: real library files
// (SKIP when absent). Exit code 1 on any FAIL.
#include "asset/ModelImport.h"
#include "asset/RestPose.h"
#include "asset/VmdMotion.h"
#include "studio/RestPoseRetarget.h"
#include "studio/StudioMotion.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace mmdx;
using namespace mmdx::studio;
using namespace DirectX;
namespace fs = std::filesystem;

namespace {

int g_fails = 0;
void Expect(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fails;
}

constexpr float kPi = 3.14159265358979f;
const char* const kUpperBody = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB";  // 上半身
const char* const kLeftArm = "\xE5\xB7\xA6\xE8\x85\x95";                 // 左腕
const char* const kLeftElbow = "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x98";   // 左ひじ
const char* const kLeftWrist = "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96";   // 左手首
const char* const kLeftFinger = "\xE5\xB7\xA6\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x91";  // 左人指１
const char* const kRightArm = "\xE5\x8F\xB3\xE8\x85\x95";                             // 右腕
const char* const kRightElbow = "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x98";                // 右ひじ
const char* const kRightWrist = "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96";                // 右手首
const char* const kRightFinger = "\xE5\x8F\xB3\xE4\xBA\xBA\xE6\x8C\x87\xEF\xBC\x91";  // 右人指１

// Synthetic rig: 上半身 at (0,10,0); each arm chain is `armDeg` below horizontal (T twin: 0).
PmxModel BuildModel(float armDeg) {
    PmxModel m;
    const float a = armDeg * kPi / 180.0f;
    const XMFLOAT3 dir{std::cos(a), -std::sin(a), 0.0f};
    auto bone = [&](const char* name, XMFLOAT3 pos, int parent) {
        PmxBone b;
        b.name = name;
        b.position = pos;
        b.parentIndex = parent;
        m.bones.push_back(b);
        return (int)m.bones.size() - 1;
    };
    bone(kUpperBody, {0, 10, 0}, -1);
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? 1.0f : -1.0f;
        const XMFLOAT3 arm = {s * 1.0f, 15.0f, 0.0f};
        const XMFLOAT3 elbow = {arm.x + s * 3.0f * dir.x, arm.y + 3.0f * dir.y, 0.0f};
        const XMFLOAT3 wrist = {elbow.x + s * 2.5f * dir.x, elbow.y + 2.5f * dir.y, 0.0f};
        const XMFLOAT3 finger = {wrist.x + s * 0.6f * dir.x, wrist.y + 0.6f * dir.y, 0.0f};
        const int a0 = bone(side == 0 ? kLeftArm : kRightArm, arm, 0);
        const int e0 = bone(side == 0 ? kLeftElbow : kRightElbow, elbow, a0);
        const int w0 = bone(side == 0 ? kLeftWrist : kRightWrist, wrist, e0);
        bone(side == 0 ? kLeftFinger : kRightFinger, finger, w0);
    }
    return m;
}

const char* const kChain[2][5] = {{kLeftArm, kLeftElbow, kLeftWrist, kLeftFinger, kUpperBody},
                                  {kRightArm, kRightElbow, kRightWrist, kRightFinger, kUpperBody}};

// Forward kinematics with the engine's conventions ("rotate by the local, then by the parent's world"):
// world rotation = XMQuaternionMultiply(local, parentWorld);
// world position = parent position + XMVector3Rotate((pos[bone] + t) - pos[parent], parentWorldRot).
struct WorldPose {
    std::vector<XMFLOAT4> rot;
    std::vector<XMFLOAT3> pos;
};

WorldPose EvalWorld(const PmxModel& m, const MotionData& mo, int frame) {
    WorldPose w;
    w.rot.assign(m.bones.size(), XMFLOAT4{0, 0, 0, 1});
    w.pos.assign(m.bones.size(), XMFLOAT3{0, 0, 0});
    std::vector<char> done(m.bones.size(), 0);
    // Bones are in parent-before-child order in the synthetic rigs; resolve recursively anyway.
    std::function<void(int)> visit = [&](int b) {
        if (done[(size_t)b]) return;
        const PmxBone& bone = m.bones[(size_t)b];
        BoneKf k;
        if (const auto it = mo.bones.find(bone.name); it != mo.bones.end() && !it->second.empty())
            k = SampleBone(it->second, frame);
        XMVECTOR rot = XMLoadFloat4(&k.r);
        XMVECTOR pos = XMLoadFloat3(&bone.position) + XMLoadFloat3(&k.t);
        if (bone.parentIndex >= 0 && bone.parentIndex < (int)m.bones.size()) {
            visit(bone.parentIndex);
            const PmxBone& parent = m.bones[(size_t)bone.parentIndex];
            const XMVECTOR pr = XMLoadFloat4(&w.rot[(size_t)bone.parentIndex]);
            const XMVECTOR pp = XMLoadFloat3(&w.pos[(size_t)bone.parentIndex]);
            pos = pp + XMVector3Rotate(pos - XMLoadFloat3(&parent.position), pr);
            rot = XMQuaternionMultiply(rot, pr);
        }
        XMStoreFloat4(&w.rot[(size_t)b], XMQuaternionNormalize(rot));
        XMStoreFloat3(&w.pos[(size_t)b], pos);
        done[(size_t)b] = 1;
    };
    for (int b = 0; b < (int)m.bones.size(); ++b) visit(b);
    return w;
}

int IndexOf(const PmxModel& m, const char* name) { return m.FindBone(name); }

float Dist(const XMFLOAT3& a, const XMFLOAT3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

XMFLOAT4 RandomRotation(std::mt19937& rng) {
    std::uniform_real_distribution<float> u(-1.0f, 1.0f), ang(0.0f, 90.0f);
    XMVECTOR axis;
    do axis = XMVectorSet(u(rng), u(rng), u(rng), 0.0f);
    while (XMVectorGetX(XMVector3Length(axis)) < 0.1f);
    XMFLOAT4 q;
    XMStoreFloat4(&q, XMQuaternionRotationAxis(XMVector3Normalize(axis), ang(rng) * kPi / 180.0f));
    return q;
}

// --- A / B: random motion, exactness against the T-pose twin ------------------------------------------------
void CaseAB() {
    const PmxModel aModel = BuildModel(kMmdArmAngleDeg);
    const PmxModel tModel = BuildModel(0.0f);
    MotionData motion;
    std::mt19937 rng(12345);
    std::uniform_real_distribution<float> tr(-1.0f, 1.0f);
    const int frames[6] = {0, 7, 13, 29, 41, 60};
    for (int side = 0; side < 2; ++side) {
        for (int b = 0; b < 4; ++b) {
            const char* name = kChain[side][b];
            for (int f : frames) {
                BoneKf k;
                k.frame = f;
                k.r = RandomRotation(rng);
                if (b == 1) k.t = {tr(rng), tr(rng), tr(rng)};  // a random translation on ひじ
                UpsertKey(motion.bones[name], k);
            }
        }
    }
    for (int f : frames) {
        BoneKf k;
        k.frame = f;
        k.r = RandomRotation(rng);
        UpsertKey(motion.bones[kUpperBody], k);
    }

    MotionData retargeted = motion;
    ArmRetargetOptions opt;  // T-pose authored: sourceArmDeg = 0, both sides
    const ArmRetargetResult res = RetargetArmRest(aModel, retargeted, opt);
    Expect(res.applied, "A: retarget applied");
    Expect(res.rotationDeg[0] > 1.0f && res.rotationDeg[1] > 1.0f, "A: a correction angle per side");

    // The invariant the formulas rest on: rotating the A rest direction by R gives the T rest direction.
    for (int side = 0; side < 2; ++side) {
        const float s = side == 0 ? 1.0f : -1.0f;
        const int arm = IndexOf(aModel, side == 0 ? kLeftArm : kRightArm);
        const int elbow = IndexOf(aModel, side == 0 ? kLeftElbow : kRightElbow);
        const XMFLOAT3 pa = aModel.bones[(size_t)arm].position, pe = aModel.bones[(size_t)elbow].position;
        const XMVECTOR dA = XMVector3Normalize(XMVectorSet(pe.x - pa.x, pe.y - pa.y, pe.z - pa.z, 0.0f));
        const XMVECTOR hA = XMVector3Normalize(XMVectorSet(XMVectorGetX(dA), 0.0f, XMVectorGetZ(dA), 0.0f));
        const XMVECTOR dS = hA;  // sourceArmDeg = 0
        const XMVECTOR axis = XMVector3Cross(dA, dS);
        const float angle = std::acos(std::clamp(XMVectorGetX(XMVector3Dot(dA, dS)), -1.0f, 1.0f));
        const XMVECTOR R = XMQuaternionRotationAxis(XMVector3Normalize(axis), angle);
        const float err = XMVectorGetX(XMVector3Length(XMVector3Rotate(dA, R) - dS));
        char msg[96];
        std::snprintf(msg, sizeof(msg), "A: side %d invariant XMVector3Rotate(dA, R) == dS (err %.6f)", side, err);
        Expect(err < 1e-4f, msg);
        std::snprintf(msg, sizeof(msg), "A: side %d correction angle %.2f deg", side, res.rotationDeg[side]);
        Expect(std::fabs(res.rotationDeg[side] - 38.0f) < 0.5f, msg);
        (void)s;
    }

    float worst = 0.0f;
    for (int f : frames) {
        const WorldPose wt = EvalWorld(tModel, motion, f);
        const WorldPose wa = EvalWorld(aModel, retargeted, f);
        for (int side = 0; side < 2; ++side) {
            for (int b = 1; b <= 3; ++b) {  // elbow, wrist, fingertip
                const int ti = IndexOf(tModel, kChain[side][b]);
                const int ai = IndexOf(aModel, kChain[side][b]);
                worst = std::max(worst, Dist(wt.pos[(size_t)ti], wa.pos[(size_t)ai]));
            }
        }
    }
    char msg[128];
    std::snprintf(msg, sizeof(msg), "B: every key frame matches the T twin (worst %.6f)", worst);
    Expect(worst < 1e-3f, msg);
    printf("  retarget: bones %d keys %d, angles %.2f / %.2f\n", res.bonesChanged, res.keysChanged, res.rotationDeg[0],
           res.rotationDeg[1]);
}

// --- C: no arm keys -> a frame-0 key, the A model reproduces the T twin's rest -------------------------------
void CaseC() {
    const PmxModel aModel = BuildModel(kMmdArmAngleDeg);
    const PmxModel tModel = BuildModel(0.0f);
    MotionData motion;  // empty: everything at rest
    ArmRetargetOptions opt;
    const ArmRetargetResult res = RetargetArmRest(aModel, motion, opt);
    Expect(res.applied, "C: applied on an un-keyed motion");
    for (int side = 0; side < 2; ++side) {
        const auto it = motion.bones.find(kChain[side][0]);
        char msg[96];
        std::snprintf(msg, sizeof(msg), "C: side %d got one 腕 key at frame 0", side);
        Expect(it != motion.bones.end() && it->second.size() == 1 && it->second.front().frame == 0, msg);
    }
    const WorldPose wt = EvalWorld(tModel, MotionData{}, 0);
    const WorldPose wa = EvalWorld(aModel, motion, 0);
    float worst = 0.0f;
    for (int side = 0; side < 2; ++side)
        for (int b = 1; b <= 3; ++b) worst = std::max(worst, Dist(wt.pos[(size_t)IndexOf(tModel, kChain[side][b])],
                                                                wa.pos[(size_t)IndexOf(aModel, kChain[side][b])]));
    char msg[128];
    std::snprintf(msg, sizeof(msg), "C: the A model's rest matches the T twin (worst %.6f)", worst);
    Expect(worst < 1e-3f, msg);
}

// --- D: detection ------------------------------------------------------------------------------------------
void CaseD() {
    const PmxModel aModel = BuildModel(kMmdArmAngleDeg);
    const float targets[2] = {50.0f, 110.0f};
    for (int t = 0; t < 2; ++t) {
        MotionData motion;
        for (int side = 0; side < 2; ++side) {
            const float s = side == 0 ? 1.0f : -1.0f;
            const float a = kMmdArmAngleDeg * kPi / 180.0f;
            const XMVECTOR dA = XMVectorSet(s * std::cos(a), -std::sin(a), 0.0f, 0.0f);
            const XMVECTOR hA = XMVectorSet(s, 0.0f, 0.0f, 0.0f);
            const XMVECTOR dT = XMVector3Normalize(hA * std::cos(targets[t] * kPi / 180.0f) +
                                                   XMVectorSet(0.0f, -std::sin(targets[t] * kPi / 180.0f), 0.0f, 0.0f));
            const XMVECTOR axis = XMVector3Cross(dA, dT);
            const float angle = std::acos(std::clamp(XMVectorGetX(XMVector3Dot(dA, dT)), -1.0f, 1.0f));
            XMFLOAT4 r;
            XMStoreFloat4(&r, XMQuaternionRotationAxis(XMVector3Normalize(axis), angle));
            for (int f = 0; f < 10; ++f) {
                BoneKf k;
                k.frame = f * 3;
                k.r = r;
                UpsertKey(motion.bones[kChain[side][0]], k);
            }
        }
        const ArmRestGuess g = GuessTPoseMotion(motion);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "D: %.0f deg reads likely=%s (median %.1f, %d samples)", targets[t],
                      g.likely ? "true" : "false", g.medianDeg, g.samples);
        Expect(g.samples == 20 && std::fabs(g.medianDeg - targets[t]) < 1.0f && g.likely == (targets[t] > 78.0f), msg);
    }
    (void)aModel;
}

// --- E: real data (SKIP when absent) -----------------------------------------------------------------------
fs::path FindInLibrary(const fs::path& rel) {
    std::error_code ec;
    for (fs::path dir = fs::current_path(ec); !dir.empty(); dir = dir.parent_path()) {
        const fs::path p = dir / "library" / rel;
        if (fs::is_regular_file(p, ec)) return p;
        if (dir == dir.parent_path()) break;
    }
    const fs::path bySource = fs::path(__FILE__).parent_path().parent_path() / "library" / rel;
    if (fs::is_regular_file(bySource, ec)) return bySource;
    return {};
}

void CaseE() {
    const fs::path romeo = FindInLibrary(fs::path(L"motion") / L"Romeo And Cinderella" / L"Miku.vmd");
    if (romeo.empty()) {
        printf("SKIP Romeo And Cinderella/Miku.vmd (no library)\n");
    } else {
        VmdMotion vmd;
        std::string err;
        if (!LoadVmd(romeo, vmd, &err)) {
            Expect(false, ("E: Romeo And Cinderella/Miku.vmd loads (" + err + ")").c_str());
        } else {
            const ArmRestGuess g = GuessTPoseMotion(MotionData::FromVmd(vmd));
            printf("  Romeo And Cinderella/Miku.vmd: median %.1f deg, %d samples\n", g.medianDeg, g.samples);
            Expect(g.likely, "E: Romeo And Cinderella/Miku.vmd reads T-pose authored");
        }
    }
    const fs::path evergreen = FindInLibrary(fs::path(L"motion") / L"Evergreen" / L"ch0208_so2009.vmd");
    if (evergreen.empty()) {
        printf("SKIP Evergreen/ch0208_so2009.vmd (no library)\n");
    } else {
        VmdMotion vmd;
        std::string err;
        if (!LoadVmd(evergreen, vmd, &err)) {
            Expect(false, ("E: Evergreen/ch0208_so2009.vmd loads (" + err + ")").c_str());
        } else {
            const ArmRestGuess g = GuessTPoseMotion(MotionData::FromVmd(vmd));
            printf("  Evergreen/ch0208_so2009.vmd: median %.1f deg, %d samples\n", g.medianDeg, g.samples);
            Expect(!g.likely, "E: Evergreen/ch0208_so2009.vmd reads A-pose authored");
        }
    }
    const fs::path white = FindInLibrary(fs::path(L"model") / L"sour_miku" / L"White.pmx");
    if (white.empty()) {
        printf("SKIP sour_miku/White.pmx (no library)\n");
    } else {
        PmxModel m;
        std::string err;
        if (!LoadModelFile(white, ModelRole::Character, m, &err)) {
            Expect(false, ("E: sour_miku/White.pmx loads (" + err + ")").c_str());
        } else {
            const int arm = m.FindBone(kLeftArm);
            Expect(arm >= 0 && m.bones[(size_t)arm].position.x > 0.0f, "E: White.pmx 左腕 sits on +x");
            const ArmRest rest = MeasureArmRest(m);
            printf("  sour_miku/White.pmx: arm rest L %.1f R %.1f deg (valid %d)\n", rest.deg[0], rest.deg[1],
                   (int)rest.valid);
            Expect(rest.valid, "E: White.pmx arm rest measures");
        }
    }
}

} // namespace

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    CaseAB();
    CaseC();
    CaseD();
    CaseE();
    printf("restpose_retarget_test: %s (%d failures)\n", g_fails ? "FAILED" : "all PASS", g_fails);
    return g_fails ? 1 : 0;
}
