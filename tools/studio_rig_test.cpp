// Rig generator self-test: builds synthetic skeletons (typical MMD-JP and English names) and asserts the core roles
// resolve. Optionally analyzes a real PMX: studio_rig_test [model.pmx]
#include "studio/RigGenerator.h"
#include "studio/RigPersistence.h"

#include "asset/PmxModel.h"
#include "core/TextUtil.h"

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

// Prints a compact one-line resolution summary for a model: resolved/19 roles + the set of unresolved roles.
int Summarize(const PmxModel& model, const char* label) {
    const RigGenerateResult res = GenerateRig(model);
    int resolved = 0;
    std::string unresolved;
    for (int r = 0; r < (int)RigRole::Count; ++r) {
        const RigBoneBinding& b = res.rig.bindings[(size_t)r];
        if (b.boneIndex >= 0) ++resolved;
        else { if (!unresolved.empty()) unresolved += ","; unresolved += RigRoleName(b.role); }
    }
    std::printf("%-46s %2d/19  %s\n", label, resolved, unresolved.c_str());
    return resolved == (int)RigRole::Count ? 0 : 1;
}

int main(int argc, char** argv) {
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
                const std::string pathStr = f.string();
                if (!LoadPmx(f, model, &err)) {
                    std::printf("%-46s LOAD-FAIL (%s)\n", pathStr.c_str(), err.c_str());
                    std::fflush(stdout);
                    continue;
                }
                ++total;
                failed += Summarize(model, pathStr.c_str());
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
            for (size_t i = 0; i < model.bones.size(); ++i)
                std::printf("%3zu '%s' / '%s' parent=%d\n", i, model.bones[i].name.c_str(),
                            model.bones[i].nameEn.c_str(), model.bones[i].parentIndex);
            return 0;
        }
        const RigGenerateResult res = GenerateRig(model);
        std::printf("=== %s ===\n", arg.c_str());
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
    }

    // --- synthetic English-only skeleton (names unknown to JP dictionary)
    {
        PmxModel m;
        const int center = AddBone(m, "Root", "", -1);
        const int spine = AddBone(m, "Spine", "", center);
        const int chest = AddBone(m, "Chest", "", spine);
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
        failed += CheckRole(res.rig, RigRole::Neck, "neck");
        failed += CheckRole(res.rig, RigRole::Head, "head");
        failed += CheckRole(res.rig, RigRole::LeftUpperArm, "upper_arm_l");
        failed += CheckRole(res.rig, RigRole::LeftForearm, "forearm_l");
        failed += CheckRole(res.rig, RigRole::LeftHand, "hand_l");
        failed += CheckRole(res.rig, RigRole::RightFoot, "foot_r");
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
        const std::filesystem::path rf = std::filesystem::temp_directory_path() / "rig_roundtrip_test.mmdxrig";
        std::string err;
        if (!SaveRig(rf, g.rig, &err)) {
            std::printf("  FAIL rig save: %s\n", err.c_str());
            ++failed;
        } else {
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
