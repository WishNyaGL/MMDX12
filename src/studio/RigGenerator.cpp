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
    const char* markers[] = {"ik", "ＩＫ", "親", "dummy", "捩", "先端"};
    for (const char* m : markers)
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
    {RigRole::Spine,        "上半身",     "spine"},
    {RigRole::Spine,        "",           "torso"},
    {RigRole::Chest,        "上半身2",    "chest"},
    {RigRole::Neck,         "首",         "neck", true},
    {RigRole::Head,         "頭",         "head"},
    {RigRole::LeftShoulder, "左肩",       "shoulder_l"},
    {RigRole::LeftUpperArm, "左上腕",     "upper_arm_l"},
    {RigRole::LeftUpperArm, "左腕",       "leftarm"},
    {RigRole::LeftForearm,  "左ひじ",     "forearm_l"},
    {RigRole::LeftForearm,  "左前腕",     "leftforearm"},
    {RigRole::LeftHand,     "左手首",     "hand_l"},
    {RigRole::LeftHand,     "左手",       "lefthand"},
    {RigRole::RightShoulder,"右肩",       "shoulder_r"},
    {RigRole::RightUpperArm,"右上腕",     "upper_arm_r"},
    {RigRole::RightUpperArm,"右腕",       "rightarm"},
    {RigRole::RightForearm, "右ひじ",     "forearm_r"},
    {RigRole::RightForearm, "右前腕",     "rightforearm"},
    {RigRole::RightHand,    "右手首",     "hand_r"},
    {RigRole::RightHand,    "右手",       "righthand"},
    {RigRole::LeftThigh,    "左足",       "thigh_l", true},
    {RigRole::LeftThigh,    "左もも",     "leftthigh"},
    {RigRole::LeftShin,     "左すね",     "shin_l"},
    {RigRole::LeftShin,     "左ひざ",     "leftshin"},
    {RigRole::LeftFoot,     "左足首",     "foot_l", true},
    {RigRole::RightThigh,   "右足",       "thigh_r", true},
    {RigRole::RightThigh,   "右もも",     "rightthigh"},
    {RigRole::RightShin,    "右すね",     "shin_r"},
    {RigRole::RightShin,    "右ひざ",     "rightshin"},
    {RigRole::RightFoot,    "右足首",     "foot_r", true},
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

// Side test on Japan-based tokens + compact-English tokens.
bool HasSide(const std::string& name, const std::string& nameEn, bool right) {
    const std::string lo = WithoutSpaces(name);
    const std::string en = WithoutSpaces(nameEn);
    if (right) {
        if (lo.find("右") != std::string::npos || lo.find("right") != std::string::npos ||
            en.find("right") != std::string::npos)
            return true;
        // must not be a left one
        return !(lo.find("左") != std::string::npos || lo.find("left") != std::string::npos ||
                 en.find("left") != std::string::npos);
    }
    if (lo.find("左") != std::string::npos || lo.find("left") != std::string::npos ||
        en.find("left") != std::string::npos)
        return true;
    // must not be a right one
    return !(lo.find("右") != std::string::npos || lo.find("right") != std::string::npos ||
             en.find("right") != std::string::npos);
}

int AliasScore(const std::string& name, const std::string& nameEn, const RoleAlias& a, bool needSide) {
    const std::string lo = WithoutSpaces(name);
    const std::string en = WithoutSpaces(nameEn);
    int best = 0;
    if (a.exact) {
        // exact roles: the whole compact name must equal the token (jp or en)
        const bool jpEq = a.jp && *a.jp && (lo == a.jp || en == a.jp);
        const bool enEq = a.en && *a.en && (lo == a.en || en == a.en);
        best = (jpEq || enEq) ? 10000 : 0;
    } else {
        const size_t jpPos = a.jp && *a.jp ? lo.find(a.jp) : std::string::npos;
        const size_t jpPosE = a.jp && *a.jp ? en.find(a.jp) : std::string::npos;
        const size_t enPos = a.en && *a.en ? lo.find(a.en) : std::string::npos;
        const size_t enPosE = a.en && *a.en ? en.find(a.en) : std::string::npos;
        if (jpPos != std::string::npos || jpPosE != std::string::npos)
            best = std::max(best, a.jp ? (int)std::strlen(a.jp) : 0);
        if (enPos != std::string::npos || enPosE != std::string::npos)
            best = std::max(best, a.en ? (int)std::strlen(a.en) : 0);
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

    return result;
}

} // namespace mmdx::studio
