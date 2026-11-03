// Rig generator self-test: builds synthetic skeletons (typical MMD-JP and English names) and asserts the core roles
// resolve. Optionally analyzes a real PMX: studio_rig_test [model.pmx]
#include "studio/RigGenerator.h"
#include "studio/RigPersistence.h"
#include "studio/RigSolver.h"

#include "asset/PmxModel.h"
#include "core/TextUtil.h"

#include <DirectXMath.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace mmdx;
using namespace mmdx::studio;

namespace {

// minimal skeleton builder: appends a bone with an optional parent
int AddBone(PmxModel& m, std::string name, std::string nameEn, int parent, uint16_t flags = PmxBone_Rotatable) {
    PmxBone b;
    b.name = std::move(name);
    b.nameEn = std::move(nameEn);
    b.parentIndex = parent;
    b.flags = flags;
    m.bones.push_back(std::move(b));
    return (int)m.bones.size() - 1;
}

int CheckRole(const RigDefinition& rig, RigRole role, const char* roleName) {
    const int bi = rig.BoneFor(role);
    if (bi < 0) {
        std::printf("  FAIL %s: unresolved\n", roleName);
        return 1;
    }
    std::printf("  ok   %-12s -> bone %d\n", roleName, bi);
    return 0;
}

} // namespace

// Prints a compact one-line resolution summary for a model: resolved/20 roles + the set of unresolved roles.
int Summarize(const PmxModel& model, const char* label) {
    const RigGenerateResult res = GenerateRig(model);
    int resolved = 0;
    std::string unresolved;
    for (int r = 0; r < (int)RigRole::Count; ++r) {
        const RigBoneBinding& b = res.rig.bindings[(size_t)r];
        if (b.boneIndex >= 0) ++resolved;
        else { if (!unresolved.empty()) unresolved += ","; unresolved += RigRoleName(b.role); }
    }
    std::printf("%-46s %2d/%d  %s\n", label, resolved, (int)RigRole::Count, unresolved.c_str());
    return resolved == (int)RigRole::Count ? 0 : 1;
}

// Diagnostic: print the chain controllers with their IK binding for a model.
void PrintControllers(const PmxModel& model) {
    const RigGenerateResult res = GenerateRig(model);
    for (const RigController& c : res.rig.controllers) {
        if (c.kind != RigControllerKind::Chain) continue;
        std::printf("  chain %-10s solver=%d ikMode=%d roles=", c.id.c_str(),
                    (int)c.solverType, (int)c.ikMode);
        for (RigRole r : c.roles) std::printf("%s ", RigRoleName(r));
        std::printf("\n    ik: ikBone=%d target=%d effector=%d chain=[", c.ik.ikBone,
                    c.ik.targetBone, c.ik.effectorBone);
        for (size_t i = 0; i < c.ik.chainBones.size(); ++i) {
            std::printf("%d", c.ik.chainBones[i]);
            if (i + 1 < c.ik.chainBones.size()) std::printf(",");
        }
        std::printf("]\n");
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int failed = 0;

    if (argc > 1) {
        const std::string arg = argv[1];
        // directory mode: recursively scan for .pmx files
        std::error_code ec;
        if (std::filesystem::is_directory(arg, ec)) {
            int total = 0, resolvedAll = 0;
            std::vector<std::filesystem::path> files;
            for (auto it = std::filesystem::recursive_directory_iterator(arg, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!ec && it->is_regular_file() && it->path().extension() == L".pmx")
                    files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            for (const auto& f : files) {
                PmxModel model;
                std::string err;
                // Use u8string (always UTF-8) for display: string() throws on Unicode names that the narrow code page
                // cannot encode (e.g. Korean/Japanese), which would terminate the scan.
                const auto u8path = f.u8string();
                const std::string pathStr((const char*)u8path.data(), u8path.size());
                if (!LoadPmx(f, model, &err)) {
                    std::printf("%-46s LOAD-FAIL (%s)\n", pathStr.c_str(), err.c_str());
                    std::fflush(stdout);
                    continue;
                }
                ++total;
                failed += Summarize(model, pathStr.c_str());
                if (argc > 2 && std::string(argv[2]) == "--controllers") PrintControllers(model);
                std::fflush(stdout);
                resolvedAll += 0;
            }
            std::printf("\n%d models scanned\n", total);
            return failed;
        }
        // single-file mode
        std::string err;
        PmxModel model;
        if (!LoadPmx(Utf8ToPath(arg), model, &err)) {
            std::printf("load failed: %s\n", err.c_str());
            return 2;
        }
        if (argc > 2 && std::string(argv[2]) == "--bones") {
            for (size_t i = 0; i < model.bones.size(); ++i) {
                const PmxBone& b = model.bones[i];
                const int fl = b.flags;
                std::printf("%3zu '%s' / '%s' parent=%d ik=%d rot=%d mov=%d op=%d\n", i, b.name.c_str(),
                            b.nameEn.c_str(), b.parentIndex,
                            (fl & PmxBone_IK) ? b.ikTargetIndex : -1,
                            (fl & PmxBone_Rotatable) ? 1 : 0,
                            (fl & PmxBone_Movable) ? 1 : 0,
                            (fl & PmxBone_Operable) ? 1 : 0);
            }
            return 0;
        }
        if (argc > 2 && std::string(argv[2]) == "--chain") {
            const RigGenerateResult res = GenerateRig(model);
            const char* groups[4][3] = {
                {"shoulder_l", "forearm_l", "hand_l"},
                {"shoulder_r", "forearm_r", "hand_r"},
                {"thigh_l", "shin_l", "foot_l"},
                {"thigh_r", "shin_r", "foot_r"},
            };
            for (int g = 0; g < 4; ++g) {
                RigChain c;
                std::printf("chain %d:\n", g);
                for (int r = 0; r < 3; ++r) {
                    int bi = -1;
                    for (int rr = 0; rr < (int)RigRole::Count; ++rr)
                        if (std::string(RigRoleName((RigRole)rr)) == groups[g][r]) { bi = res.rig.BoneFor((RigRole)rr); break; }
                    if (bi < 0) { std::printf("  %-12s unresolved\n", groups[g][r]); continue; }
                    const PmxBone& b = model.bones[(size_t)bi];
                    std::printf("  %-12s -> %2d '%s' pos=(%.2f,%.2f,%.2f) parent=%d\n", groups[g][r], bi,
                                b.name.c_str(), b.position.x, b.position.y, b.position.z, b.parentIndex);
                }
            }
            return 0;
        }
        const RigGenerateResult res = GenerateRig(model);
        std::printf("=== %s ===\n", arg.c_str());
        std::printf("ikBones: %zu\n", res.rig.ikBones.size());
        for (const RigIkBone& ik : res.rig.ikBones)
            std::printf("  %-10s -> %d '%s'\n", RigRoleName(ik.targetRole), ik.boneIndex, ik.boneName.c_str());
        for (int r = 0; r < (int)RigRole::Count; ++r) {
            const RigBoneBinding& b = res.rig.bindings[(size_t)r];
            if (b.boneIndex >= 0)
                std::printf("  ok   %-12s -> %s\n", RigRoleName(b.role), model.bones[(size_t)b.boneIndex].name.c_str());
            else
                std::printf("  ?    %-12s -> unresolved\n", RigRoleName(b.role));
        }
        for (const std::string& w : res.diag.warnings) std::printf("  warn: %s\n", w.c_str());
        return 0;
    }

    // --- synthetic MMD (Japanese) skeleton
    {
        PmxModel m;
        const int center = AddBone(m, "センター", "Center", -1);
        const int spine = AddBone(m, "上半身", "Spine", center);
        const int chest = AddBone(m, "胸", "Chest", spine);
        const int waist = AddBone(m, "腰", "Waist", chest);
        const int neck = AddBone(m, "首", "Neck", chest);
        const int head = AddBone(m, "頭", "Head", neck);
        const int ls = AddBone(m, "左肩", "Left Shoulder", chest);
        const int la = AddBone(m, "左腕", "Left Arm", ls);
        const int lf = AddBone(m, "左ひじ", "Left Forearm", la);
        const int lh = AddBone(m, "左手首", "Left Hand", lf);
        const int rs = AddBone(m, "右肩", "Right Shoulder", chest);
        const int ra = AddBone(m, "右腕", "Right Arm", rs);
        const int rf = AddBone(m, "右ひじ", "Right Forearm", ra);
        const int rh = AddBone(m, "右手首", "Right Hand", rf);
        const int lt = AddBone(m, "左足", "Left Thigh", center);
        const int lsh = AddBone(m, "左すね", "Left Shin", lt);
        const int lf2 = AddBone(m, "左足首", "Left Foot", lsh);
        const int rt = AddBone(m, "右足", "Right Thigh", center);
        const int rsh = AddBone(m, "右すね", "Right Shin", rt);
        const int rf2 = AddBone(m, "右足首", "Right Foot", rsh);
        (void)head; (void)lf; (void)lh; (void)rf; (void)rh; (void)lf2; (void)rf2;
        (void)la; (void)ra; (void)lt; (void)rt;

        std::printf("=== synthetic MMD-JP ===\n");
        const RigGenerateResult res = GenerateRig(m);
        failed += CheckRole(res.rig, RigRole::Pelvis, "pelvis");
        failed += CheckRole(res.rig, RigRole::Spine, "spine");
        failed += CheckRole(res.rig, RigRole::Chest, "chest");
        failed += CheckRole(res.rig, RigRole::Waist, "waist");
        failed += CheckRole(res.rig, RigRole::Neck, "neck");
        failed += CheckRole(res.rig, RigRole::Head, "head");
        failed += CheckRole(res.rig, RigRole::LeftUpperArm, "upper_arm_l");
        failed += CheckRole(res.rig, RigRole::LeftForearm, "forearm_l");
        failed += CheckRole(res.rig, RigRole::LeftHand, "hand_l");
        failed += CheckRole(res.rig, RigRole::RightUpperArm, "upper_arm_r");
        failed += CheckRole(res.rig, RigRole::RightForearm, "forearm_r");
        failed += CheckRole(res.rig, RigRole::RightHand, "hand_r");
        failed += CheckRole(res.rig, RigRole::LeftThigh, "thigh_l");
        failed += CheckRole(res.rig, RigRole::LeftShin, "shin_l");
        failed += CheckRole(res.rig, RigRole::LeftFoot, "foot_l");
        failed += CheckRole(res.rig, RigRole::RightThigh, "thigh_r");
        failed += CheckRole(res.rig, RigRole::RightShin, "shin_r");
        failed += CheckRole(res.rig, RigRole::RightFoot, "foot_r");
        if (res.rig.controllers.size() != 19) {
            std::printf("  FAIL controllers count = %zu (expected 19)\n", res.rig.controllers.size());
            ++failed;
        }
    }

    // --- synthetic English-only skeleton (names unknown to JP dictionary)
    {
        PmxModel m;
        const int center = AddBone(m, "Root", "", -1);
        const int spine = AddBone(m, "Spine", "", center);
        const int chest = AddBone(m, "Chest", "", spine);
        const int waist = AddBone(m, "Waist", "", chest);
        const int neck = AddBone(m, "Neck", "", chest);
        const int head = AddBone(m, "Head", "", neck);
        const int la = AddBone(m, "LeftArm", "", chest);
        const int lf = AddBone(m, "LeftForearm", "", la);
        const int lh = AddBone(m, "LeftHand", "", lf);
        const int ra = AddBone(m, "RightArm", "", chest);
        const int rf = AddBone(m, "RightForearm", "", ra);
        const int rh = AddBone(m, "RightHand", "", rf);
        const int lt = AddBone(m, "LeftThigh", "", center);
        const int lsh = AddBone(m, "LeftShin", "", lt);
        const int lf2 = AddBone(m, "LeftFoot", "", lsh);
        const int rt = AddBone(m, "RightThigh", "", center);
        const int rsh = AddBone(m, "RightShin", "", rt);
        const int rf2 = AddBone(m, "RightFoot", "", rsh);
        (void)head; (void)lf; (void)lh; (void)rf; (void)rh; (void)lf2; (void)rf2;
        (void)la; (void)ra; (void)lt; (void)rt;

        std::printf("=== synthetic English ===\n");
        const RigGenerateResult res = GenerateRig(m);
        failed += CheckRole(res.rig, RigRole::Pelvis, "pelvis");
        failed += CheckRole(res.rig, RigRole::Spine, "spine");
        failed += CheckRole(res.rig, RigRole::Chest, "chest");
        failed += CheckRole(res.rig, RigRole::Waist, "waist");
        failed += CheckRole(res.rig, RigRole::Neck, "neck");
        failed += CheckRole(res.rig, RigRole::Head, "head");
        failed += CheckRole(res.rig, RigRole::LeftUpperArm, "upper_arm_l");
        failed += CheckRole(res.rig, RigRole::LeftForearm, "forearm_l");
        failed += CheckRole(res.rig, RigRole::LeftHand, "hand_l");
        failed += CheckRole(res.rig, RigRole::RightFoot, "foot_r");
    }

    // --- synthetic EN shorthand skeleton: elbow/wrist/leg/knee/ankle naming ---
    // Many models label limbs with downstream joint words (arm/elbow/wrist for the
    // upper arm/forearm/hand; leg/knee/ankle for thigh/shin/foot). Assert the expanded
    // alias table resolves them.
    {
        PmxModel m;
        const int center = AddBone(m, "Center", "", -1);
        const int spine = AddBone(m, "Spine", "", center);
        const int chest = AddBone(m, "Chest", "", spine);
        const int neck = AddBone(m, "Neck", "", chest);
        const int head = AddBone(m, "Head", "", neck);
        const int ls = AddBone(m, "Shoulder_L", "", chest);
        const int la = AddBone(m, "Arm_L", "", ls);
        const int lf = AddBone(m, "Elbow_L", "", la);
        const int lh = AddBone(m, "Wrist_L", "", lf);
        const int lt = AddBone(m, "Leg_L", "", center);
        const int lsh = AddBone(m, "Knee_L", "", lt);
        const int lf2 = AddBone(m, "Ankle_L", "", lsh);
        (void)head; (void)lf; (void)lh; (void)lf2;
        std::printf("=== synthetic EN shorthand ===\n");
        const RigGenerateResult res = GenerateRig(m);
        failed += CheckRole(res.rig, RigRole::LeftUpperArm, "upper_arm_l");
        failed += CheckRole(res.rig, RigRole::LeftForearm, "forearm_l");
        failed += CheckRole(res.rig, RigRole::LeftHand, "hand_l");
        failed += CheckRole(res.rig, RigRole::LeftThigh, "thigh_l");
        failed += CheckRole(res.rig, RigRole::LeftShin, "shin_l");
        failed += CheckRole(res.rig, RigRole::LeftFoot, "foot_l");
    }

    // --- chain solver math smoke test
    {
        RigChainDrag d;
        d.valid = true;
        d.chain = {0, 1, 2};
        d.rootPos = {0, 0, 0}; d.midPos = {0, 0, -1}; d.endPos = {0, 0, -2};
        d.rootWorld = {0, 0, 0, 1}; d.midWorld = {0, 0, 0, 1}; d.endWorld = {0, 0, 0, 1};
        d.rootParentWorld = {0, 0, 0, 1}; d.midParentWorld = {0, 0, 0, 1}; d.endParentWorld = {0, 0, 0, 1};
        d.bendNormal = {0, 1, 0};
        RigChainResult r;
        // reachable target: bent to the side
        const bool ok = SolveRigChainDrag(d, {0.5f, 0.5f, -1.8f}, &r);
        if (!ok) { std::printf("  FAIL chain solve ok\n"); ++failed; }
        else {
            const auto isFinite = [](const XMFLOAT4& q) {
                return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
            };
            if (!isFinite(r.rootLocal) || !isFinite(r.midLocal) || !isFinite(r.endLocal)) {
                std::printf("  FAIL chain solve non-finite\n"); ++failed;
            }
            // far target: chain straightens but still ok, no NaN
            RigChainResult r2;
            if (!SolveRigChainDrag(d, {0, 0, -50}, &r2) ||
                !isFinite(r2.rootLocal) || !isFinite(r2.midLocal) || !isFinite(r2.endLocal)) {
                std::printf("  FAIL chain solve far target\n"); ++failed;
            }
            std::printf("  chain solve smoke: ok\n");
        }
    }

    // --- chain solver: the hand keeps its own LOCAL rotation (does not chase the forearm) ---
    // The hand (end bone) with a non-identity local rotation must keep that rotation after a
    // move solve, while the elbow (mid) rotates to place the wrist. The old behaviour froze the
    // hand's world orientation, which pinned the hand to the forearm vector and killed rotation.
    {
        RigChainDrag d;
        d.valid = true;
        d.chain = {0, 1, 2};
        d.rootPos = {0, 0, 0}; d.midPos = {0, 0, -1}; d.endPos = {0, 0, -2};
        d.rootWorld = {0, 0, 0, 1}; d.midWorld = {0, 0, 0, 1};
        // a hand twisted 90° about Y (local rotation vs the forearm)
        d.endWorld = {0, 0.7071f, 0, 0.7071f};
        d.rootParentWorld = {0, 0, 0, 1}; d.midParentWorld = {0, 0, 0, 1};
        d.endParentWorld = {0, 0, 0, 1};
        d.bendNormal = {0, 1, 0};
        const XMFLOAT4 wantEnd{0, 0.7071f, 0, 0.7071f};
        RigChainResult r;
        if (!SolveRigChainDrag(d, {0.5f, 0.5f, -1.8f}, &r)) {
            std::printf("  FAIL chain solve (hand-rotation) ok\n"); ++failed;
        } else {
            bool keep = true;
            const float wantEndArr[4] = {wantEnd.x, wantEnd.y, wantEnd.z, wantEnd.w};
            const float endArr[4] = {r.endLocal.x, r.endLocal.y, r.endLocal.z, r.endLocal.w};
            for (int c = 0; c < 4; ++c)
                if (std::fabs(endArr[c] - wantEndArr[c]) > 1e-3f) keep = false;
            // the elbow must actually bend away from identity (hand follows)
            const bool midMoved = std::fabs(r.midLocal.w - 1.0f) > 1e-3f;
            if (!keep) { std::printf("  FAIL chain solve: hand local rotation not preserved\n"); ++failed; }
            else if (!midMoved) { std::printf("  FAIL chain solve: elbow did not move\n"); ++failed; }
            else std::printf("  chain solve hand-rotation: preserved, elbow moves\n");
        }
    }

    // --- FK solver smoke test: identity when no drag, finite with drag
    {
        RigFkDrag fk;
        fk.bone = 0;
        fk.startLocal = {0, 0, 0, 1};
        fk.pitchSign = 1.0f;
        const XMFLOAT3 right{1, 0, 0}, up{0, 1, 0};
        const XMFLOAT4 parentWorld{0, 0, 0, 1};
        const XMFLOAT4 no = SolveRigFkDrag(fk, right, up, parentWorld, 0.0f, 0.0f);
        const XMFLOAT4 rot = SolveRigFkDrag(fk, right, up, parentWorld, 10.0f, -5.0f);
        const auto isFinite = [](const XMFLOAT4& q) {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        };
        bool fkOk = isFinite(no) && isFinite(rot);
        if (std::fabs(no.w - 1.0f) > 1e-5f) fkOk = false;  // identity stays identity
        std::printf("  FK solve smoke: %s\n", fkOk ? "ok" : "FAIL");
        if (!fkOk) ++failed;
    }

    // --- virtual IK solver smoke test ---
    {
        RigVirtualIkDrag d;
        d.chain = {0, 1, 2};
        // startPos holds one point per chain bone + the target (n + 1 total)
        d.startPos = {{0, 0, 0}, {0, 0, -1}, {0, 0, -2}, {0, 0, -3}};
        d.startWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.parentWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.bendNormals = {{0, 1, 0}, {0, 1, 0}};
        d.valid = true;
        d.targetBone = 2;
        std::vector<XMFLOAT4> out;
        const bool ok = SolveRigVirtualIkDrag(d, {0.5f, 0.5f, -1.8f}, out);
        const auto isFinite = [](const XMFLOAT4& q) {
            return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w);
        };
        bool vikOk = ok && out.size() == 3 && isFinite(out[0]) && isFinite(out[1]) && isFinite(out[2]);
        // The hand (end bone) keeps its relative orientation to the forearm: with identity
        // start worlds relEnd is identity, so out[2] should stay ~identity while the chain bends.
        if (vikOk) {
            const float w = out[2].w;
            if (std::fabs(w - 1.0f) > 1e-3f) vikOk = false;
        }
        // Regression: the solver must actually bend the chain — solving toward a different
        // target must change the output (a bug restored identity for every target).
        if (vikOk) {
            std::vector<XMFLOAT4> out2;
            SolveRigVirtualIkDrag(d, {1.0f, 0.0f, -2.5f}, out2);
            bool moved = out2.size() == 3 &&
                (std::fabs(out[0].x - out2[0].x) > 1e-3f ||
                 std::fabs(out[0].y - out2[0].y) > 1e-3f ||
                 std::fabs(out[0].z - out2[0].z) > 1e-3f ||
                 std::fabs(out[0].w - out2[0].w) > 1e-3f);
            if (!moved) {
                std::printf("  FAIL virtual IK no movement\n");
                ++failed;
                vikOk = false;
            }
        }
        std::printf("  virtual IK smoke: %s\n", vikOk ? "ok" : "FAIL");
        if (!vikOk) ++failed;
    }

    // --- virtual IK input-contract regression: undersized startPos must be rejected ---
    // The solver requires one start point per chain bone plus the target (n+1). A
    // capture with only n points used to walk out of bounds (heap corruption); it
    // must now be refused without touching outLocal.
    {
        RigVirtualIkDrag d;
        d.chain = {0, 1, 2};
        d.startPos = {{0, 0, 0}, {0, 0, -1}, {0, 0, -2}};   // n=3 but only 3 points (need 4)
        d.startWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.parentWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.bendNormals = {{0, 1, 0}, {0, 1, 0}};
        d.valid = true;
        d.targetBone = 2;
        std::vector<XMFLOAT4> out;
        const bool ok = SolveRigVirtualIkDrag(d, {0.5f, 0.5f, -1.8f}, out);
        if (ok) {
            std::printf("  FAIL virtual IK contract: undersized startPos accepted\n");
            ++failed;
        } else {
            std::printf("  virtual IK contract (undersized startPos): rejected\n");
        }
    }

    // --- virtual IK 4-bone chain (real arm: twist bones included), target == end bone ---
    // Reproduces the runtime case: chain of 4 bones, targetBone == last chain bone, so the
    // final segment has zero length. The end bone (hand) must NOT get a zero quaternion, and
    // must keep its ANGLE relative to the forearm (not freeze in world space).
    {
        RigVirtualIkDrag d;
        d.chain = {0, 1, 2, 3};
        d.startPos = {{0, 0, 0}, {0.2f, 0, -1}, {0, 0, -1.6f}, {0.1f, 0, -2.0f}, {0.1f, 0, -2.0f}};
        d.startWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.parentWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.bendNormals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
        d.valid = true;
        d.targetBone = 3;
        std::vector<XMFLOAT4> out;
        SolveRigVirtualIkDrag(d, {0.5f, 0.3f, -1.8f}, out);
        bool anyZero = false;
        for (size_t i = 0; i < out.size(); ++i) {
            const float len2 = out[i].x*out[i].x + out[i].y*out[i].y +
                               out[i].z*out[i].z + out[i].w*out[i].w;
            if (len2 < 0.5f) anyZero = true;  // not a valid unit quaternion
            std::printf("    vik4 out[%zu]=(%.3f,%.3f,%.3f,%.3f) len2=%.4f\n", i,
                        out[i].x, out[i].y, out[i].z, out[i].w, len2);
        }
        if (anyZero) { std::printf("  FAIL virtual IK 4-bone: zero/invalid quaternion\n"); ++failed; }
        else if (std::fabs(out[3].w - 1.0f) > 1e-3f) {
            // startWorld is identity, so the end's angle to the forearm is identity
            std::printf("  FAIL virtual IK 4-bone: end angle not preserved (expected identity)\n"); ++failed;
        }
        else std::printf("  virtual IK 4-bone: ok\n");
    }

    // --- virtual IK: end bone keeps its angle to the forearm even when hand is twisted ---
    // Hand is pre-rotated 90° about Y. After solving toward a different target, the hand's
    // LOCAL quaternion (relative to the forearm) must stay that 90° twist in world space.
    {
        RigVirtualIkDrag d;
        d.chain = {0, 1, 2, 3};
        d.startPos = {{0, 0, 0}, {0.2f, 0, -1}, {0, 0, -1.6f}, {0.1f, 0, -2.0f}, {0.1f, 0, -2.0f}};
        // forearm (index 2) identity; hand (index 3) twisted 90° Y
        d.startWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0.7071f, 0, 0.7071f}};
        d.parentWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.bendNormals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}};
        d.valid = true;
        d.targetBone = 3;
        std::vector<XMFLOAT4> out;
        SolveRigVirtualIkDrag(d, {0.6f, 0.2f, -1.7f}, out);
        // expected local hand = relEnd = startWorld[3]*inverse(startWorld[2]) = 90°Y twist
        const XMFLOAT4 want{0, 0.7071f, 0, 0.7071f};
        bool keep = out.size() == 4;
        for (int c = 0; keep && c < 4; ++c) {
            const float a = c==0?out[3].x : c==1?out[3].y : c==2?out[3].z : out[3].w;
            const float b = c==0?want.x : c==1?want.y : c==2?want.z : want.w;
            if (std::fabs(a - b) > 1e-3f) keep = false;
        }
        if (!keep) { std::printf("  FAIL virtual IK end keeps forearm angle\n"); ++failed; }
        else std::printf("  virtual IK end keeps forearm angle: ok\n");
    }

    // --- virtual IK bend-guard: the elbow/knee must never invert to the wrong side ---
    {
        RigVirtualIkDrag d;
        d.chain = {0, 1, 2};
        d.startPos = {{0, 0, 0}, {0.3f, 0, -1}, {0, 0, -2}, {-0.3f, 0, -3}};
        d.startWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        d.parentWorld = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
        // rest bend-plane normals: cross(mid-root, end-mid) = (0, 0.6, 0) -> (0,1,0)
        d.bendNormals = {{0, 1, 0}, {0, 1, 0}};
        d.valid = true;
        d.targetBone = 2;
        const auto sub = [](const XMFLOAT3& a, const XMFLOAT3& b) {
            return XMFLOAT3{a.x - b.x, a.y - b.y, a.z - b.z};
        };
        const auto cross = [](const XMFLOAT3& a, const XMFLOAT3& b) {
            return XMFLOAT3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        };
        const auto dot = [](const XMFLOAT3& a, const XMFLOAT3& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };
        const auto len = [&](const XMFLOAT3& a) { return std::sqrt(dot(a, a)); };
        const auto qmul = [](const XMFLOAT4& a, const XMFLOAT4& b) {
            return XMFLOAT4{
                a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            };
        };
        // rotate v by unit quaternion q=(x,y,z,w): t=2*cross(u,v); v' = v + w*t + cross(u,t)
        const auto rot = [&](const XMFLOAT3& v, const XMFLOAT4& q) {
            const XMFLOAT3 u{q.x, q.y, q.z};
            const XMFLOAT3 cuv = cross(u, v);
            const XMFLOAT3 t{2.0f * cuv.x, 2.0f * cuv.y, 2.0f * cuv.z};
            const XMFLOAT3 ut = cross(u, t);
            return XMFLOAT3{v.x + q.w * t.x + ut.x, v.y + q.w * t.y + ut.y, v.z + q.w * t.z + ut.z};
        };

        const XMFLOAT3 root{0, 0, 0};
        const XMFLOAT3 restMid{0.3f, 0, -1};
        const XMFLOAT3 restEnd{0, 0, -2};
        const XMFLOAT3 nRest = cross(sub(restEnd, root), sub(restMid, root));
        const float L0 = len(sub(restMid, root));
        const float L1 = len(sub(restEnd, restMid));
        const XMFLOAT3 sm0 = sub(restMid, root);
        const XMFLOAT3 sm1 = sub(restEnd, restMid);
        const XMFLOAT3 s0{sm0.x / L0, sm0.y / L0, sm0.z / L0};
        const XMFLOAT3 s1{sm1.x / L1, sm1.y / L1, sm1.z / L1};

        const float targets[][3] = {
            {0.5f, 0.5f, -1.8f},
            {-4.0f, 0.0f, -1.0f},
            {4.0f, 0.0f, -1.0f},
            {0.0f, 3.0f, -1.0f},
            {0.0f, -3.0f, -1.0f},
            {-2.0f, 0.0f, -2.0f},
            {2.0f, 0.0f, -2.0f},
        };
        bool bad = false;
        for (const auto& tg : targets) {
            std::vector<XMFLOAT4> out;
            if (!SolveRigVirtualIkDrag(d, {tg[0], tg[1], tg[2]}, out) || out.size() != 3) { bad = true; break; }
            const XMFLOAT3 r0 = rot(s0, out[0]);
            const XMFLOAT3 elbow{L0 * r0.x, L0 * r0.y, L0 * r0.z};
            const XMFLOAT4 q01 = qmul(out[0], out[1]);
            const XMFLOAT3 r1 = rot(s1, q01);
            const XMFLOAT3 hand{elbow.x + L1 * r1.x, elbow.y + L1 * r1.y, elbow.z + L1 * r1.z};
            const XMFLOAT3 ab = sub(hand, root);
            const XMFLOAT3 ap = sub(elbow, root);
            const float side = dot(cross(ab, ap), nRest);
            if (side < -1e-4f) { bad = true; break; }
        }
        if (bad) { std::printf("  FAIL virtual IK bend-guard: elbow inverted\n"); ++failed; }
        else std::printf("  virtual IK bend-guard: elbow stays on the rest side\n");
    }

    // --- hierarchy chain builder test (twist bones skipped, elbow kept) ---
    {
        PmxModel m;
        const int arm = AddBone(m, "arm", "arm", -1);
        const int twist1 = AddBone(m, "armtwist", "armtwist", arm, PmxBone_Rotatable | PmxBone_FixedAxis);
        const int elbow = AddBone(m, "elbow", "elbow", twist1);
        const int twist2 = AddBone(m, "wristtwist", "wristtwist", elbow, PmxBone_Rotatable | PmxBone_FixedAxis);
        const int wrist = AddBone(m, "wrist", "wrist", twist2);
        const std::vector<int> chain = BuildRigChainByHierarchy(m, arm, wrist);
        const std::vector<int> expected = {arm, elbow, wrist};
        bool hOk = chain == expected;
        std::printf("  hierarchy chain: %s (", hOk ? "ok" : "FAIL");
        for (int b : chain) std::printf("%d,", b);
        std::printf(")\n");
        if (!hOk) ++failed;
    }

    // --- persistence round-trip on the MMD-JP synthetic skeleton
    {
        PmxModel m;
        const int center = AddBone(m, "センター", "Center", -1);
        const int spine = AddBone(m, "上半身", "Spine", center);
        const int chest = AddBone(m, "上半身2", "Chest", spine);
        const int neck = AddBone(m, "首", "Neck", chest);
        const int head = AddBone(m, "頭", "Head", neck);
        const int la = AddBone(m, "左腕", "Left Arm", chest);
        const int lf = AddBone(m, "左ひじ", "Left Forearm", la);
        const int lh = AddBone(m, "左手首", "Left Hand", lf);
        const int lt = AddBone(m, "左足", "Left Thigh", center);
        const int lsh = AddBone(m, "左ひざ", "Left Shin", lt);
        const int lf2 = AddBone(m, "左足首", "Left Foot", lsh);
        (void)head; (void)lh; (void)lf2;

        const RigGenerateResult g = GenerateRig(m);
        std::printf("  rt: after generate\n");
        const std::filesystem::path rf = std::filesystem::temp_directory_path() / "rig_roundtrip_test.mmdxrig";
        std::string err;
        if (!SaveRig(rf, g.rig, &err)) {
            std::printf("  FAIL rig save: %s\n", err.c_str());
            ++failed;
        } else {
            std::printf("  rt: after save\n");
            RigDefinition loaded;
            if (!LoadRig(rf, loaded, &err)) {
                std::printf("  FAIL rig load: %s\n", err.c_str());
                ++failed;
            } else {
                bool ok = loaded.bindings.size() == g.rig.bindings.size();
                for (size_t i = 0; ok && i < loaded.bindings.size(); ++i) {
                    if (loaded.bindings[i].role != g.rig.bindings[i].role ||
                        loaded.bindings[i].boneIndex != g.rig.bindings[i].boneIndex ||
                        loaded.bindings[i].boneName != g.rig.bindings[i].boneName)
                        ok = false;
                }
                std::printf("  rig round-trip: %s\n", ok ? "ok" : "MISMATCH");
                if (!ok) ++failed;
                // controllers survive the round-trip
                if (loaded.controllers.size() != g.rig.controllers.size() || g.rig.controllers.empty()) {
                    std::printf("  FAIL controllers round-trip: %zu vs %zu\n",
                                loaded.controllers.size(), g.rig.controllers.size());
                    ++failed;
                } else {
                    bool cok = true;
                    for (size_t i = 0; i < loaded.controllers.size(); ++i) {
                        const RigController& a = loaded.controllers[i];
                        const RigController& b = g.rig.controllers[i];
                        if (a.id != b.id || a.kind != b.kind || a.shape != b.shape ||
                            a.rotate != b.rotate || a.draggable != b.draggable || a.roles != b.roles)
                            cok = false;
                    }
                    if (!cok) { std::printf("  FAIL controllers mismatch\n"); ++failed; }
                }
                // rebind against the same model keeps every already-resolved binding
                RebindRig(loaded, m);
                int resolved = 0, savedResolved = 0;
                for (const RigBoneBinding& b : loaded.bindings) if (b.boneIndex >= 0) ++resolved;
                for (const RigBoneBinding& b : g.rig.bindings) if (b.boneIndex >= 0) ++savedResolved;
                if (resolved != savedResolved) {
                    std::printf("  FAIL rebind resolved=%d saved=%d\n", resolved, savedResolved);
                    ++failed;
                }
            }
            std::filesystem::remove(rf);
        }
    }

    std::printf(failed == 0 ? "PASS\n" : "FAIL (%d)\n", failed);
    return failed == 0 ? 0 : 1;
}
