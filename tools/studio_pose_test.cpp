#include "studio/AutoIk.h"
#include "studio/StudioPose.h"
#include <DirectXMath.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <string>
#include <set>

using namespace mmdx;
using namespace mmdx::studio;
using namespace DirectX;

static int g_passed = 0, g_failed = 0;

static void Check(bool ok, const char* name, const char* fmt = "", ...) {
    if (ok) {
        ++g_passed;
        std::printf("PASS %s\n", name);
        return;
    }
    ++g_failed;
    std::printf("FAIL %s: ", name);
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

static void TestMirrorBoneName() {
    Check(MirrorBoneName("\xE5\xB7\xA6\xE8\x85\x95") == "\xE5\x8F\xB3\xE8\x85\x95", "MirrorBoneName: \xE5\xB7\xA6\xE8\x85\x95 -> \xE5\x8F\xB3\xE8\x85\x95"); // 左腕 -> 右腕
    Check(MirrorBoneName("\xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB") == "\xE5\xB7\xA6\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB", "MirrorBoneName: \xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB -> \xE5\xB7\xA6\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB"); // 右足ＩＫ -> 左足ＩＫ
    Check(MirrorBoneName("\xE5\xB7\xA6\xE5\x8F\xB3") == "\xE5\x8F\xB3\xE5\xB7\xA6", "MirrorBoneName: \xE5\xB7\xA6\xE5\x8F\xB3 -> \xE5\x8F\xB3\xE5\xB7\xA6"); // 左右 -> 右左
    Check(MirrorBoneName("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC") == "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", "MirrorBoneName: \xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC unchanged"); // センター
    Check(MirrorBoneName("Arm_L") == "Arm_R", "MirrorBoneName: Arm_L -> Arm_R");
    Check(MirrorBoneName("hand.R") == "hand.L", "MirrorBoneName: hand.R -> hand.L");
    Check(MirrorBoneName("LeftFoot") == "RightFoot", "MirrorBoneName: LeftFoot -> RightFoot");
    Check(MirrorBoneName("mixamorig:RightHand") == "mixamorig:LeftHand", "MirrorBoneName: mixamorig:RightHand -> mixamorig:LeftHand");
    Check(MirrorBoneName("Hips") == "Hips", "MirrorBoneName: Hips unchanged");
    Check(MirrorBoneName("_L") == "_L", "MirrorBoneName: _L unchanged");
}

static void TestMirrorPoseDouble() {
    PoseBone p{{1,2,3}, {0,0,0,0}};
    XMVECTOR q = XMVector4Normalize(XMVectorSet(0.1f, 0.2f, 0.3f, 0.9f));
    XMStoreFloat4(&p.r, q);
    PoseBone m = MirrorPose(MirrorPose(p));
    Check(std::abs(m.t.x - p.t.x) < 1e-6 && std::abs(m.t.y - p.t.y) < 1e-6 && std::abs(m.t.z - p.t.z) < 1e-6 &&
          std::abs(m.r.x - p.r.x) < 1e-6 && std::abs(m.r.y - p.r.y) < 1e-6 && std::abs(m.r.z - p.r.z) < 1e-6 && std::abs(m.r.w - p.r.w) < 1e-6,
          "MirrorPose twice returns the original");
}

static void TestMirrorPoseReflection() {
    XMVECTOR v = XMVectorSet(0.3f, -0.7f, 0.5f, 0.0f);
    XMVECTOR q = XMVector4Normalize(XMVectorSet(0.2f, -0.4f, 0.1f, 0.8f));
    PoseBone pb{{}, {}};
    XMStoreFloat4(&pb.r, q);
    PoseBone mp = MirrorPose(pb);
    XMVECTOR mq = XMLoadFloat4(&mp.r);
    
    auto M = [](XMVECTOR a) {
        XMFLOAT4 f; XMStoreFloat4(&f, a);
        return XMVectorSet(-f.x, f.y, f.z, f.w);
    };
    
    XMVECTOR M_v = M(v);
    XMVECTOR rot_Mv = XMVector3Rotate(M_v, mq);
    XMVECTOR rot_v = XMVector3Rotate(v, q);
    XMVECTOR M_rot_v = M(rot_v);
    
    XMFLOAT4 a, b;
    XMStoreFloat4(&a, rot_Mv);
    XMStoreFloat4(&b, M_rot_v);
    
    bool ok = std::abs(a.x - b.x) < 1e-5 && std::abs(a.y - b.y) < 1e-5 && std::abs(a.z - b.z) < 1e-5;
    Check(ok, "MirrorPose is a true reflection");
}

static void TestMirrorPoseInto() {
    PmxModel model;
    model.bones.resize(4);
    model.bones[0].name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";
    model.bones[0].flags = PmxBone_Rotatable | PmxBone_Movable;
    model.bones[1].name = "\xE5\xB7\xA6\xE8\x85\x95";
    model.bones[1].flags = PmxBone_Rotatable;
    model.bones[2].name = "\xE5\x8F\xB3\xE8\x85\x95";
    model.bones[2].flags = PmxBone_Rotatable;
    model.bones[3].name = "\xE9\xA0\xAD";
    model.bones[3].flags = PmxBone_Rotatable;
    
    std::vector<PoseBone> current(4, PoseBone{{0,0,0},{0,0,0,1}});
    XMVECTOR r1 = XMQuaternionRotationRollPitchYaw(0.1f, 0.4f, 0.3f);
    XMStoreFloat4(&current[1].r, r1);
    current[0].t = {2, 0, 1};
    
    {
        PoseLayer out;
        int written = MirrorPoseInto(model, current, nullptr, out);
        Check(written == 3, "Whole-model MirrorPoseInto written == 3");
        PoseBone m1 = MirrorPose(current[1]);
        Check(out.bones[2].r.x == m1.r.x && out.bones[2].r.y == m1.r.y && out.bones[2].r.z == m1.r.z && out.bones[2].r.w == m1.r.w, "Whole-model MirrorPoseInto out.bones[2].r == MirrorPose(current[1]).r");
        Check(out.bones[1].r.x == 0 && out.bones[1].r.y == 0 && out.bones[1].r.z == 0 && out.bones[1].r.w == 1, "Whole-model MirrorPoseInto out.bones[1].r == identity");
        Check(out.bones[0].t.x == -2 && out.bones[0].t.y == 0 && out.bones[0].t.z == 1, "Whole-model MirrorPoseInto out.bones[0].t == (-2, 0, 1)");
    }
    
    {
        PoseLayer out;
        std::set<int> onlyBones = {1};
        int written = MirrorPoseInto(model, current, &onlyBones, out);
        Check(written == 1, "onlyBones = {1} written == 1");
        Check(out.bones.size() == 1 && out.bones.count(2), "onlyBones = {1} only out.bones[2] set");
    }
    
    {
        model.bones[0].flags = PmxBone_Rotatable;
        PoseLayer out;
        std::set<int> onlyBones = {0};
        int written = MirrorPoseInto(model, current, &onlyBones, out);
        Check(written == 0, "Bone 0 not Movable MirrorPoseInto writes nothing");
        model.bones[0].flags = PmxBone_Rotatable | PmxBone_Movable; // restore
    }
    
    {
        PoseLayer out;
        out.bones[3] = {{9,9,9},{0,0,0,1}};
        std::set<int> onlyBones = {1};
        MirrorPoseInto(model, current, &onlyBones, out);
        Check(out.bones.count(3) && out.bones[3].t.x == 9, "Existing unrelated entries in out are kept");
    }
}

static void TestVpd() {
    PmxModel model;
    model.name = "TestModel";
    model.bones.resize(4);
    model.bones[0].name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";
    model.bones[0].flags = PmxBone_Rotatable | PmxBone_Movable;
    model.bones[1].name = "\xE5\xB7\xA6\xE8\x85\x95";
    model.bones[1].flags = PmxBone_Rotatable;
    model.bones[2].name = "\xE5\x8F\xB3\xE8\x85\x95";
    model.bones[2].flags = PmxBone_Rotatable;
    model.bones[3].name = "\xE9\xA0\xAD";
    model.bones[3].flags = PmxBone_Rotatable;
    
    model.morphs.resize(2);
    model.morphs[0].name = "a";
    model.morphs[1].name = "b";
    
    std::vector<PoseBone> current(4, PoseBone{{0,0,0},{0,0,0,1}});
    XMVECTOR r1 = XMQuaternionRotationRollPitchYaw(0.1f, 0.4f, 0.3f);
    XMStoreFloat4(&current[1].r, r1);
    current[0].t = {2, 0, 1};
    
    std::vector<float> morphWeights = {0.0f, 0.5f};
    
    VpdPose vpdAll = MakeVpdPose(model, current, morphWeights, nullptr);
    Check(vpdAll.bones.size() == 2 && vpdAll.bones[0].name == model.bones[0].name && vpdAll.bones[1].name == model.bones[1].name, "MakeVpdPose whole model bones non-identity listed");
    Check(vpdAll.morphs.size() == 1 && vpdAll.morphs[0].name == "b" && vpdAll.morphs[0].weight == 0.5f, "MakeVpdPose whole model morphs listed");
    
    std::set<int> ob = {3};
    VpdPose vpdPart = MakeVpdPose(model, current, morphWeights, &ob);
    Check(vpdPart.bones.size() == 1 && vpdPart.bones[0].name == model.bones[3].name, "MakeVpdPose onlyBones exactly bone 3 listed");
    Check(vpdPart.morphs.empty(), "MakeVpdPose onlyBones no morphs");
    
    auto tempPath = std::filesystem::temp_directory_path() / "studio_pose_test.vpd";
    std::string err;
    bool saved = SaveVpd(tempPath, vpdAll, &err);
    Check(saved, "SaveVpd");
    
    VpdPose loaded;
    bool loadedOk = LoadVpd(tempPath, loaded, &err);
    Check(loadedOk, "LoadVpd");
    
    PoseLayer out;
    int applied = ApplyVpdPose(model, loaded, nullptr, out, nullptr);
    Check(applied == 3, "ApplyVpdPose count == 3"); // 2 bones + 1 morph
    Check(out.bones.count(0) && std::abs(out.bones[0].t.x - 2) < 1e-5, "ApplyVpdPose bone 0 value ok");
    Check(out.bones.count(1) && std::abs(out.bones[1].r.x - current[1].r.x) < 1e-5, "ApplyVpdPose bone 1 value ok");
    Check(out.morphs.count(1) && out.morphs[1] == 0.5f, "ApplyVpdPose morph b == 0.5");
    
    std::filesystem::remove(tempPath);
}

static void TestApplyMissing() {
    PmxModel model;
    model.bones.resize(4);
    model.bones[0].name = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";
    model.bones[1].name = "\xE5\xB7\xA6\xE8\x85\x95";
    model.bones[2].name = "\xE5\x8F\xB3\xE8\x85\x95";
    model.bones[3].name = "\xE9\xA0\xAD";
    
    VpdPose vpd;
    vpd.bones.push_back({"nonexistent", {0,0,0}, {0,0,0,1}});
    vpd.morphs.push_back({"zzz", 1.0f});
    vpd.bones.push_back({"\xE5\x8F\xB3\xE8\x85\x95", {0,0,0}, {0,0,0,1}});
    
    PoseLayer out;
    std::vector<std::string> missing;
    int count = ApplyVpdPose(model, vpd, nullptr, out, &missing);
    Check(count == 1, "ApplyVpdPose missing count excludes them");
    Check(missing.size() == 2 && missing[0] == "nonexistent" && missing[1] == "zzz", "ApplyVpdPose missing reported");
    
    missing.clear();
    out.bones.clear();
    std::set<int> ob = {1};
    count = ApplyVpdPose(model, vpd, &ob, out, &missing);
    Check(count == 0, "ApplyVpdPose onlyBones skips RightArm");
    Check(missing.size() == 1 && missing[0] == "nonexistent", "ApplyVpdPose onlyBones morphs ignored");
}

static void TestSamePoseLayer() {
    PoseLayer a, b;
    a.frame = 10; b.frame = 10;
    a.bones[1] = {{1,2,3}, {0,0,0,1}};
    b.bones[1] = {{1,2,3}, {0,0,0,1}};
    a.morphs[2] = 0.5f;
    b.morphs[2] = 0.5f;
    
    Check(SamePoseLayer(a, b), "SamePoseLayer equal layers true");
    b.frame = 11;
    Check(!SamePoseLayer(a, b), "SamePoseLayer changing frame false");
    b.frame = 10;
    b.bones[1].t.x = 2;
    Check(!SamePoseLayer(a, b), "SamePoseLayer changing one value false");
    b.bones[1].t.x = 1;
    b.morphs[3] = 0.1f;
    Check(!SamePoseLayer(a, b), "SamePoseLayer adding a morph false");
}

// A synthetic right arm: 上半身 -> 右腕 -> 右ひじ -> 右手首, plus a 右腕捩 twist bone between 腕 and ひじ, and a
// leg 右足 driven by 右足ＩＫ. `twist` inserts the twist bone into the wrist's parent chain (default off).
static PmxModel MakeArmModel(bool twist = false) {
    PmxModel m;
    m.bones.resize(7);
    auto bone = [&](int i, const char* name, int parent, DirectX::XMFLOAT3 pos, uint16_t flags) {
        m.bones[i].name = name;
        m.bones[i].parentIndex = parent;
        m.bones[i].position = pos;
        m.bones[i].flags = flags;
    };
    bone(0, "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB", -1, {0, 0, 0}, PmxBone_Rotatable);        // 上半身
    bone(1, "\xE5\x8F\xB3\xE8\x85\x95", 0, {0, 0, 0}, PmxBone_Rotatable);                      // 右腕
    bone(2, "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x98", 1, {1, 0, 0}, PmxBone_Rotatable);             // 右ひじ
    bone(3, "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96", 2, {1, 1, 0}, PmxBone_Rotatable);           // 右手首
    bone(4, "\xE5\x8F\xB3\xE8\x85\x95\xE6\x8D\xA9", 1, {0.5f, 0, 0}, PmxBone_Rotatable | PmxBone_FixedAxis);  // 右腕捩
    bone(5, "\xE5\x8F\xB3\xE8\xB6\xB3", 0, {0, -1, 0}, PmxBone_Rotatable);                      // 右足
    bone(6, "\xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB", -1, {0, -1, 0}, PmxBone_IK);   // 右足ＩＫ
    m.bones[6].ikTargetIndex = 5;
    m.bones[6].ikLinks.push_back(PmxIkLink{2, false, {}, {}});
    if (twist) m.bones[2].parentIndex = 4;  // 腕捩 sits between 腕 and ひじ (wrist still hangs off ひじ)
    return m;
}

static void TestFindAutoIkChain() {
    {
        const PmxModel m = MakeArmModel();
        int root = -1, mid = -1;
        Check(FindAutoIkChain(m, 3, root, mid) && mid == 2 && root == 1, "FindAutoIkChain arm: mid=ひじ root=腕");
    }
    {
        const PmxModel m = MakeArmModel(true);
        int root = -1, mid = -1;
        Check(FindAutoIkChain(m, 3, root, mid) && mid == 2 && root == 1, "FindAutoIkChain skips the twist bone");
    }
    {
        const PmxModel m = MakeArmModel();
        int root = -1, mid = -1;
        Check(!FindAutoIkChain(m, 5, root, mid), "FindAutoIkChain leg under PMX IK returns false");
    }
}

static void TestSolveTwoBone() {
    const XMVECTOR root = XMVectorSet(0, 0, 0, 1);
    const XMVECTOR mid = XMVectorSet(1, 0, 0, 1);
    const XMVECTOR end = XMVectorSet(1, 1, 0, 1);
    const XMVECTOR bend = XMVectorSet(0, 0, 1, 0);
    auto endAfter = [&](FXMVECTOR target, XMVECTOR qRoot, XMVECTOR qMid, XMVECTOR* out) {
        const XMVECTOR mid2 = XMVectorAdd(root, XMVector3Rotate(XMVectorSubtract(mid, root), qRoot));
        const XMVECTOR e2 = XMVectorAdd(mid2, XMVector3Rotate(XMVector3Rotate(XMVectorSubtract(end, mid), qRoot), qMid));
        *out = e2;
    };
    {
        XMVECTOR qRoot, qMid;
        const XMVECTOR target = XMVectorSet(1.2f, 0.8f, 0, 1);
        Check(SolveTwoBone(root, mid, end, target, bend, qRoot, qMid), "SolveTwoBone reachable returns true");
        XMVECTOR got;
        endAfter(target, qRoot, qMid, &got);
        Check(XMVectorGetX(XMVector3Length(XMVectorSubtract(got, target))) < 0.01f,
              "SolveTwoBone reachable within 0.01");
    }
    {
        XMVECTOR qRoot, qMid;
        const XMVECTOR target = XMVectorSet(5, 0, 0, 1);  // beyond lab + lbc = 2
        Check(SolveTwoBone(root, mid, end, target, bend, qRoot, qMid), "SolveTwoBone unreachable returns true");
        XMVECTOR got;
        endAfter(target, qRoot, qMid, &got);
        Check(XMVectorGetX(XMVector3Length(XMVectorSubtract(got, XMVectorSet(2, 0, 0, 1)))) < 0.01f,
              "SolveTwoBone unreachable: chain straight toward the target");
    }
    {
        XMVECTOR qRoot, qMid;
        const XMVECTOR target = XMVectorSet(2, 0, 0, 1);  // exactly full extension
        SolveTwoBone(root, mid, end, target, bend, qRoot, qMid);
        const bool finite = std::isfinite(XMVectorGetX(qRoot)) && std::isfinite(XMVectorGetY(qRoot)) &&
                            std::isfinite(XMVectorGetZ(qRoot)) && std::isfinite(XMVectorGetW(qRoot)) &&
                            std::isfinite(XMVectorGetX(qMid)) && std::isfinite(XMVectorGetY(qMid)) &&
                            std::isfinite(XMVectorGetZ(qMid)) && std::isfinite(XMVectorGetW(qMid));
        Check(finite, "SolveTwoBone no NaN at full extension");
    }
}

static void TestSolveAutoIkDrag() {
    // start pose: chain along +X with a bend, identity parent world rotations, the wrist turned about X
    AutoIkDrag drag;
    drag.valid = true;
    drag.root = 1; drag.mid = 2; drag.end = 3;
    drag.rootPos = {0, 0, 0};
    drag.midPos = {1, 0, 0};
    drag.endPos = {1, 1, 0};
    drag.rootParentWorld = {0, 0, 0, 1};
    drag.midParentWorld = {0, 0, 0, 1};
    drag.endParentWorld = {0, 0, 0, 1};
    drag.rootWorld = {0, 0, 0, 1};
    drag.midWorld = {0, 0, 0, 1};
    XMStoreFloat4(&drag.endWorld, XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0), 0.5f));  // wrist turned
    drag.bendNormal = {0, 0, 1};

    AutoIkResult r;
    const XMFLOAT3 target{1.2f, 0.8f, 0.0f};
    Check(SolveAutoIkDrag(drag, target, &r), "SolveAutoIkDrag ok");

    // recover the solver's world deltas from the returned locals (world' = local' * parentWorld', parentWorld' = parent*q)
    const XMVECTOR rootWorldNew = XMQuaternionMultiply(XMLoadFloat4(&r.rootLocal), XMLoadFloat4(&drag.rootParentWorld));
    const XMVECTOR qRoot = XMQuaternionMultiply(rootWorldNew, XMQuaternionInverse(XMLoadFloat4(&drag.rootWorld)));
    const XMVECTOR midParentNew = XMQuaternionMultiply(XMLoadFloat4(&drag.midParentWorld), qRoot);
    const XMVECTOR midWorldNew = XMQuaternionMultiply(XMLoadFloat4(&r.midLocal), midParentNew);
    const XMVECTOR qMid = XMQuaternionMultiply(XMQuaternionInverse(XMQuaternionMultiply(XMLoadFloat4(&drag.midWorld), qRoot)),
                                               midWorldNew);
    const XMVECTOR midPosNew = XMVectorAdd(XMLoadFloat3(&drag.rootPos),
                                 XMVector3Rotate(XMVectorSubtract(XMLoadFloat3(&drag.midPos), XMLoadFloat3(&drag.rootPos)), qRoot));
    const XMVECTOR endPosNew = XMVectorAdd(midPosNew,
                                 XMVector3Rotate(XMVector3Rotate(XMVectorSubtract(XMLoadFloat3(&drag.endPos), XMLoadFloat3(&drag.midPos)), qRoot), qMid));
    Check(XMVectorGetX(XMVector3Length(XMVectorSubtract(endPosNew, XMLoadFloat3(&target)))) < 0.01f,
          "SolveAutoIkDrag reaches the target");

    const XMVECTOR endParentNew = XMQuaternionMultiply(XMLoadFloat4(&drag.endParentWorld), XMQuaternionMultiply(qRoot, qMid));
    const XMVECTOR endWorldFinal = XMQuaternionMultiply(XMLoadFloat4(&r.endLocal), endParentNew);
    const float dot = std::fabs(XMVectorGetX(XMVector4Dot(XMQuaternionNormalize(endWorldFinal),
                                                          XMQuaternionNormalize(XMLoadFloat4(&drag.endWorld)))));
    Check(dot > 0.9999f, "SolveAutoIkDrag preserves the end bone's world orientation");
}

int main() {
    TestMirrorBoneName();
    TestMirrorPoseDouble();
    TestMirrorPoseReflection();
    TestMirrorPoseInto();
    TestVpd();
    TestApplyMissing();
    TestSamePoseLayer();
    TestFindAutoIkChain();
    TestSolveTwoBone();
    TestSolveAutoIkDrag();
    
    std::printf("studio_pose_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed > 0 ? 1 : 0;
}
