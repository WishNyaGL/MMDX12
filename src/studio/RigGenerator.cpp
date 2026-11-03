#include "studio/RigGenerator.h"

#include "asset/PmxModel.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

namespace mmdx::studio {

namespace {

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

std::string WithoutSpaces(const std::string& s);

struct RoleAlias {
    RigRole role;
    const char* jp;   // Japanese / MMD token (substring)
    const char* en;   // English / common token (substring)
    bool exact = false;  // when true, the whole (compact) name must equal the token
};

// In MMD skeletons the IK/helper bones (足IK親, つま先IK, etc.) are not editable body parts: exclude them so they
// never take a body role.
bool ShouldExcludeBone(const PmxBone& b) {
    const std::string n = WithoutSpaces(b.name) + " " + WithoutSpaces(b.nameEn);
    const char* markers[] = {"ik", "ＩＫ", "親", "dummy", "捩", "先端", "補助", "补助", "helper", "aux"};
    for (const char* m : markers)
        if (n.find(m) != std::string::npos) return true;
    // Decorative / clothing bones must never take a body role: they often contain
    // body-part substrings (e.g. "wrist_ribbon", "waist ribbon") and hijack the role.
    const char* decor[] = {
        "ribbon", "リボン", "bow", "ban", "hat", "帽子", "ヘッド", "headwear", "hair", "髪", "おさげ",
        "skirt", "スカート", "apron", "エプロン", "collar", "襟", "cape", "マント", "tie", "ネクタイ",
        "sash", "带", "帶", "pompom", "ポンポン", "frill", "フリル", "リボン", "バンド",
        "boot", "ブーツ", "shoe", "靴", "socks", "ソックス", "stocking", "ストッキング",
    };
    for (const char* m : decor)
        if (n.find(m) != std::string::npos) return true;
    return false;
}

// Primary Japanese + English tokens per role. Longer/more specific tokens score higher. English tokens are stored
// compact (no spaces) so both "LeftArm" and "left arm" match; Japanese tokens stay as-is. `exact` roles require the
// whole compact name to equal the token (used where a token is a substring of a more specific bone).
const RoleAlias kAliases[] = {
    {RigRole::Pelvis,       "センター",   "pelvis", true},
    {RigRole::Pelvis,       "",           "center", true},
    {RigRole::Pelvis,       "",           "root", true},
    {RigRole::Groove,       "グルーブ",   "groove", true},
    {RigRole::Spine,        "上半身",     "spine"},
    {RigRole::Spine,        "",           "torso"},
    {RigRole::Spine,        "",           "upperbody"},
    {RigRole::Chest,        "上半身2",    "chest"},
    {RigRole::Chest,        "",           "upperbody2"},
    {RigRole::Waist,        "腰",         "waist"},
    {RigRole::Waist,        "ウェスト",   "waist"},
    {RigRole::Waist,        "下半身",     "lowerbody"},
    {RigRole::Waist,        "",           "hips"},
    {RigRole::Neck,         "首",         "neck", true},
    {RigRole::Head,         "頭",         "head"},
    {RigRole::LeftShoulder, "左肩",       "shoulder_l"},
    {RigRole::LeftShoulder, "",           "leftshoulder"},
    {RigRole::LeftShoulder, "",           "shoulder_left"},
    {RigRole::RightShoulder,"右肩",       "shoulder_r"},
    {RigRole::RightShoulder,"",           "rightshoulder"},
    {RigRole::RightShoulder,"",           "shoulder_right"},
    {RigRole::LeftUpperArm, "左上腕",     "upper_arm_l"},
    {RigRole::LeftUpperArm, "左腕",       "leftarm"},
    {RigRole::LeftUpperArm, "",           "arm_l", true},
    {RigRole::LeftUpperArm, "",           "left_arm"},
    {RigRole::LeftUpperArm, "",           "upperarm_l"},
    {RigRole::RightUpperArm,"右上腕",     "upper_arm_r"},
    {RigRole::RightUpperArm,"右腕",       "rightarm"},
    {RigRole::RightUpperArm,"",           "arm_r", true},
    {RigRole::RightUpperArm,"",           "right_arm"},
    {RigRole::RightUpperArm,"",           "upperarm_r"},
    {RigRole::LeftForearm,  "左ひじ",     "forearm_l"},
    {RigRole::LeftForearm,  "左前腕",     "leftforearm"},
    {RigRole::LeftForearm,  "",           "elbow_l"},
    {RigRole::LeftForearm,  "",           "lowerarm_l"},
    {RigRole::RightForearm, "右ひじ",     "forearm_r"},
    {RigRole::RightForearm, "右前腕",     "rightforearm"},
    {RigRole::RightForearm, "",           "elbow_r"},
    {RigRole::RightForearm, "",           "lowerarm_r"},
    {RigRole::LeftHand,     "左手首",     "hand_l"},
    {RigRole::LeftHand,     "左手",       "lefthand"},
    {RigRole::LeftHand,     "",           "wrist_l"},
    {RigRole::LeftHand,     "",           "left_hand"},
    {RigRole::RightHand,    "右手首",     "hand_r"},
    {RigRole::RightHand,    "右手",       "righthand"},
    {RigRole::RightHand,    "",           "wrist_r"},
    {RigRole::RightHand,    "",           "right_hand"},
    {RigRole::LeftThigh,    "左足",       "thigh_l", true},
    {RigRole::LeftThigh,    "左もも",     "leftthigh"},
    {RigRole::LeftThigh,    "",           "leg_l", true},
    {RigRole::LeftThigh,    "",           "upperleg_l"},
    {RigRole::LeftThigh,    "",           "left_leg"},
    {RigRole::RightThigh,   "右足",       "thigh_r", true},
    {RigRole::RightThigh,   "右もも",     "rightthigh"},
    {RigRole::RightThigh,   "",           "leg_r", true},
    {RigRole::RightThigh,   "",           "upperleg_r"},
    {RigRole::RightThigh,   "",           "right_leg"},
    {RigRole::LeftShin,     "左すね",     "shin_l"},
    {RigRole::LeftShin,     "左ひざ",     "leftshin"},
    {RigRole::LeftShin,     "",           "knee_l"},
    {RigRole::LeftShin,     "",           "lowerleg_l"},
    {RigRole::RightShin,    "右すね",     "shin_r"},
    {RigRole::RightShin,    "右ひざ",     "rightshin"},
    {RigRole::RightShin,    "",           "knee_r"},
    {RigRole::RightShin,    "",           "lowerleg_r"},
    {RigRole::LeftFoot,     "左足首",     "foot_l", true},
    {RigRole::LeftFoot,     "",           "ankle_l"},
    {RigRole::RightFoot,    "右足首",     "foot_r", true},
    {RigRole::RightFoot,    "",           "ankle_r"},
    {RigRole::LeftToe,      "左つま先",   "toe_l"},
    {RigRole::LeftToe,      "左足つま先", "toe_l"},
    {RigRole::LeftToe,      "",           "toe2_l"},
    {RigRole::LeftToe,      "",           "toes_l"},
    {RigRole::RightToe,     "右つま先",   "toe_r"},
    {RigRole::RightToe,     "右足つま先", "toe_r"},
    {RigRole::RightToe,     "",           "toe2_r"},
    {RigRole::RightToe,     "",           "toes_r"},
};

std::string WithoutSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == ' ' || c == '_' || c == '-') continue;
        // lowercase only ASCII; leave UTF-8 multibyte bytes intact
        const unsigned char u = (unsigned char)c;
        out += (char)(u < 0x80 ? std::tolower(u) : u);
    }
    return out;
}

// Side test on Japan-based tokens + compact-English tokens. Japanese names are the
// authoritative source; English is only a fallback (translators often botch it,
// e.g. labelling a right toe "L toe").
bool HasSide(const std::string& name, const std::string& nameEn, bool right) {
    const std::string lo = WithoutSpaces(name);
    const std::string en = WithoutSpaces(nameEn);
    const bool jpLeft = lo.find("左") != std::string::npos;
    const bool jpRight = lo.find("右") != std::string::npos;
    // Japanese side markers win over English
    if (jpLeft || jpRight) {
        if (right) return jpRight;
        return jpLeft;
    }
    if (right) {
        if (en.find("right") != std::string::npos) return true;
        return !(en.find("left") != std::string::npos);
    }
    if (en.find("left") != std::string::npos) return true;
    return !(en.find("right") != std::string::npos);
}

int AliasScore(const std::string& name, const std::string& nameEn, const RoleAlias& a, bool needSide) {
    const std::string lo = WithoutSpaces(name);
    const std::string en = WithoutSpaces(nameEn);
    // Compact the EN alias token exactly like bone names, so separator variants match:
    // a "shoulder_l" token now matches "Shoulder_L", "shoulder.l", "shoulder left", etc.
    const std::string enTok = (a.en && *a.en) ? WithoutSpaces(a.en) : std::string();
    int best = 0;
    if (a.exact) {
        // exact roles: the whole compact name must equal the token (jp or en)
        const bool jpEq = a.jp && *a.jp && (lo == a.jp || en == a.jp);
        const bool enEq = !enTok.empty() && (lo == enTok || en == enTok);
        best = (jpEq || enEq) ? 10000 : 0;
    } else {
        const size_t jpPos = a.jp && *a.jp ? lo.find(a.jp) : std::string::npos;
        const size_t jpPosE = a.jp && *a.jp ? en.find(a.jp) : std::string::npos;
        const size_t enPos = !enTok.empty() ? lo.find(enTok) : std::string::npos;
        const size_t enPosE = !enTok.empty() ? en.find(enTok) : std::string::npos;
        if (jpPos != std::string::npos || jpPosE != std::string::npos)
            best = std::max(best, a.jp ? (int)std::strlen(a.jp) : 0);
        if (enPos != std::string::npos || enPosE != std::string::npos)
            best = std::max(best, (int)enTok.size());
    }
    if (best == 0) return 0;
    if (needSide && !HasSide(name, nameEn, RigRoleIsRight(a.role))) return 0;
    return best;
}

// Structural parent roles used as a fallback when names are not recognised.
RigRole ParentRoleFor(RigRole role) {
    switch (role) {
        case RigRole::Spine: return RigRole::Pelvis;
        case RigRole::Chest: return RigRole::Spine;
        case RigRole::Waist: return RigRole::Chest;
        case RigRole::Neck: return RigRole::Chest;
        case RigRole::Head: return RigRole::Neck;
        case RigRole::LeftUpperArm: return RigRole::LeftShoulder;
        case RigRole::LeftForearm: return RigRole::LeftUpperArm;
        case RigRole::LeftHand: return RigRole::LeftForearm;
        case RigRole::LeftShin: return RigRole::LeftThigh;
        case RigRole::LeftFoot: return RigRole::LeftShin;
        case RigRole::RightUpperArm: return RigRole::RightShoulder;
        case RigRole::RightForearm: return RigRole::RightUpperArm;
        case RigRole::RightHand: return RigRole::RightForearm;
        case RigRole::RightShin: return RigRole::RightThigh;
        case RigRole::RightFoot: return RigRole::RightShin;
        default: return RigRole::Count;
    }
}

} // namespace

RigGenerateResult GenerateRig(const PmxModel& model) {
    RigGenerateResult result;
    RigDefinition& rig = result.rig;
    RigDiagnostics& diag = result.diag;
    rig.Clear();
    rig.sourceModelName = model.name;

    const int n = (int)model.bones.size();
    std::vector<char> used((size_t)n, 0);
    std::vector<int> assigned((size_t)RigRole::Count, -1);

    // --- pass 1: dictionary matching (best unused bone per role, greedy by fixed role order)
    for (int r = 0; r < (int)RigRole::Count; ++r) {
        const RigRole role = (RigRole)r;
        const bool needSide = RigRoleIsLeft(role) || RigRoleIsRight(role);
        int bestBone = -1, bestScore = 0;
        for (int b = 0; b < n; ++b) {
            if (used[(size_t)b]) continue;
            if (ShouldExcludeBone(model.bones[(size_t)b])) continue;
            int score = 0;
            for (const RoleAlias& a : kAliases) {
                if (a.role != role) continue;
                score = std::max(score, AliasScore(model.bones[(size_t)b].name, model.bones[(size_t)b].nameEn, a, needSide));
            }
            if (score > bestScore) { bestScore = score; bestBone = b; }
        }
        if (bestBone >= 0 && bestScore > 0) {
            used[(size_t)bestBone] = 1;
            assigned[(size_t)r] = bestBone;
        }
    }

    // --- pass 2: structural fallback for chains whose parent was resolved (child of the parent bone)
    for (int r = 0; r < (int)RigRole::Count; ++r) {
        if (assigned[(size_t)r] >= 0) continue;
        const RigRole pRole = ParentRoleFor((RigRole)r);
        if (pRole == RigRole::Count) continue;
        const int parentBone = assigned[(size_t)pRole];
        if (parentBone < 0) continue;
        for (int b = 0; b < n; ++b) {
            if (used[(size_t)b]) continue;
            if (model.bones[(size_t)b].parentIndex != parentBone) continue;
            const uint16_t flags = model.bones[(size_t)b].flags;
            if (!(flags & PmxBone_Rotatable)) continue;
            used[(size_t)b] = 1;
            assigned[(size_t)r] = b;
            break;
        }
    }

    // --- build the definition + diagnostics
    for (int r = 0; r < (int)RigRole::Count; ++r) {
        RigBoneBinding& bind = rig.bindings[(size_t)r];
        bind.role = (RigRole)r;
        bind.boneIndex = assigned[(size_t)r];
        if (bind.boneIndex >= 0) {
            bind.boneName = model.bones[(size_t)bind.boneIndex].name;
            bind.confidence = 1.0f;  // dictionary/structural match; refined later
        } else {
            diag.warnings.push_back(std::string("unresolved ") + RigRoleName(bind.role));
        }
    }

    // Ready-made model IK bones (arm_IK / leg_IKP / toe_IK): find bones whose IK target is one of our end roles.
    // Controllers can then move the IK bone itself as an object (the engine solves the chain).
    // For feet, prefer the IK bone's PARENT (leg_IKP, the pivot under the heel) when it exists; else the IK bone.
    {
        static const RigRole kIkEnds[] = {
            RigRole::LeftHand, RigRole::RightHand,
            RigRole::LeftFoot, RigRole::RightFoot,
            RigRole::LeftToe, RigRole::RightToe,
        };
        for (const RigRole role : kIkEnds) {
            const int target = rig.BoneFor(role);
            if (target < 0) continue;
            for (int bi = 0; bi < n; ++bi) {
                const PmxBone& b = model.bones[(size_t)bi];
                if (!(b.flags & PmxBone_IK) || b.ikTargetIndex != target) continue;
                int drive = bi;
                // feet: prefer the parent (leg_IKP) as the control handle, UNLESS the parent is
                // the skeleton root / master bone (全ての親) — many authors parent the leg IK directly
                // to the master, which would pull the foot controller onto the master.
                if (role == RigRole::LeftFoot || role == RigRole::RightFoot) {
                    const int par = b.parentIndex;
                    if (par > 0 && par < n) {
                        const PmxBone& pb = model.bones[(size_t)par];
                        const std::string parName = WithoutSpaces(pb.name) + " " + WithoutSpaces(pb.nameEn);
                        const bool isRoot = parName.find("master") != std::string::npos ||
                                            parName.find("root") != std::string::npos ||
                                            parName.find("親") != std::string::npos;
                        if (!isRoot && !(pb.flags & PmxBone_IK)) drive = par;  // leg_IKP
                    }
                }
                rig.ikBones.push_back({role, drive, model.bones[(size_t)drive].name});
            }
        }
    }

    // Build the controller entities (stage 1 data). Order matches the prototype's
    // array indices: 0..5 chains, 6..12 body, 13..18 FK joints. This list is the
    // single source of truth for the viewport and the timeline.
    rig.controllers.clear();
    // chains: hands / feet / toes
    static const RigRole kChainRoles[6][3] = {
        {RigRole::LeftUpperArm, RigRole::LeftForearm, RigRole::LeftHand},
        {RigRole::RightUpperArm, RigRole::RightForearm, RigRole::RightHand},
        {RigRole::LeftThigh, RigRole::LeftShin, RigRole::LeftFoot},
        {RigRole::RightThigh, RigRole::RightShin, RigRole::RightFoot},
        {RigRole::LeftFoot, RigRole::LeftToe, RigRole::LeftToe},
        {RigRole::RightFoot, RigRole::RightToe, RigRole::RightToe},
    };
    static const char* kChainIds[6] = {"hand_l", "hand_r", "foot_l", "foot_r", "toe_l", "toe_r"};
    for (int i = 0; i < 6; ++i) {
        RigController c;
        c.id = kChainIds[i];
        c.kind = RigControllerKind::Chain;
        c.shape = RigControllerShape::Ring;
        c.canTranslate = true;
        c.canRotate = true;
        c.solverType = RigSolverType::IK;
        c.ikMode = RigIkMode::NativePMX;
        c.roles.assign(&kChainRoles[i][0], &kChainRoles[i][0] + 3);
        const RigRole endRole = kChainRoles[i][2];
        const int targetIdx = rig.BoneFor(endRole);
        const int ikBone = targetIdx >= 0 ? rig.IkBoneFor(endRole) : -1;
        if (ikBone >= 0) {
            // native PMX IK: move the model's own IK bone
            c.ik.ikBone = ikBone;
            c.ik.targetBone = targetIdx;
            c.ik.effectorBone = targetIdx;
        } else if (targetIdx >= 0) {
            // no native IK: use a virtual chain (root/mid/end); limits are filled later
            c.ikMode = RigIkMode::Virtual;
            c.ik.targetBone = targetIdx;
            c.ik.effectorBone = targetIdx;
        }
        rig.controllers.push_back(std::move(c));
    }
    // body rings: center / groove / spine / chest / neck / head / waist
    static const RigRole kBodyRoles[7] = {
        RigRole::Pelvis, RigRole::Groove, RigRole::Spine, RigRole::Chest,
        RigRole::Neck, RigRole::Head, RigRole::Waist,
    };
    static const char* kBodyIds[7] = {"center", "groove", "spine", "chest", "neck", "head", "waist"};
    for (int i = 0; i < 7; ++i) {
        RigController c;
        c.id = kBodyIds[i];
        c.kind = RigControllerKind::Body;
        c.shape = RigControllerShape::Ring;
        c.draggable = (i == 0 || i == 1);
        c.canTranslate = (i == 0 || i == 1);   // center/groove move; the rest rotate
        c.canRotate = !(i == 0 || i == 1);
        c.roles.push_back(kBodyRoles[i]);
        rig.controllers.push_back(std::move(c));
    }
    // FK joints: shoulders / upper arms / thighs
    static const RigRole kJointRoles[6] = {
        RigRole::LeftShoulder, RigRole::RightShoulder,
        RigRole::LeftUpperArm, RigRole::RightUpperArm,
        RigRole::LeftThigh, RigRole::RightThigh,
    };
    static const char* kJointIds[6] = {"shoulder_l", "shoulder_r", "upper_arm_l", "upper_arm_r", "thigh_l", "thigh_r"};
    for (int i = 0; i < 6; ++i) {
        RigController c;
        c.id = kJointIds[i];
        c.kind = RigControllerKind::Joint;
        c.shape = RigControllerShape::Diamond;
        c.rotate = true;
        c.canTranslate = false;
        c.canRotate = true;
        c.roles.push_back(kJointRoles[i]);
        rig.controllers.push_back(std::move(c));
    }

    return result;
}

} // namespace mmdx::studio
