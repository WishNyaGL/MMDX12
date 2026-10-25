// Layered motion editing (studio_motion_edit): small, additive ops on a character's bone / morph tracks, applied in
// order as ONE undo step. Baked layers write keys at `step` resolution from the current track, scaled by a
// smoothstep attack / release envelope so the motion outside [start, end] is untouched.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/McpKeyUtil.h"
#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"
#include "studio/StudioPose.h"

namespace mmdx {

using namespace studio;

namespace {

constexpr size_t kMaxWrittenKeys = 100000;
const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";       // センター
const char* const kAllParentBone = "\xE5\x85\xA8\xE3\x81\xA6\xE3\x81\xAE\xE8\xA6\xAA";    // 全ての親
const char* const kNeckBone = "\xE9\xA6\x96";                                            // 首
const char* const kHeadBone = "\xE9\xA0\xAD";                                            // 頭
const char* const kUpperBodyBone = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB";               // 上半身
const char* const kUpperBody2Bone = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB\x32";          // 上半身2
const char* const kGrooveBone = "\xE3\x82\xB0\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x96";       // グルーブ

// Default follow-through chains: torso, left arm, right arm (parent -> child).
const char* const kDefaultChains[3][3] = {
    {"\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB\x32", "\xE9\xA6\x96", "\xE9\xA0\xAD"},                 // 上半身2, 首, 頭
    {"\xE5\xB7\xA6\xE8\x85\x95", "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x98", "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96"},  // 左腕, 左ひじ, 左手首
    {"\xE5\x8F\xB3\xE8\x85\x95", "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x98", "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96"},  // 右腕, 右ひじ, 右手首
};

enum class OpKind { Rotate, Translate, LookAt, Retime, Copy, Mirror, Smooth, Morph, KeyPose, Ease, Overlap, Spring, Breathe };

const char* OpName(OpKind k) {
    switch (k) {
        case OpKind::Rotate: return "rotate";
        case OpKind::Translate: return "translate";
        case OpKind::LookAt: return "look_at";
        case OpKind::Retime: return "retime";
        case OpKind::Copy: return "copy";
        case OpKind::Mirror: return "mirror";
        case OpKind::Smooth: return "smooth";
        case OpKind::Morph: return "morph";
        case OpKind::KeyPose: return "key_pose";
        case OpKind::Ease: return "ease";
        case OpKind::Overlap: return "overlap";
        case OpKind::Spring: return "spring";
        case OpKind::Breathe: return "breathe";
    }
    return "";
}

struct KeyBoneEdit {
    std::string name;
    bool hasRot = false, hasTranslate = false;
    float rotate[3] = {0, 0, 0};
    float translate[3] = {0, 0, 0};
};

struct Op {
    OpKind kind = OpKind::Rotate;
    int start = 0, end = 0;
    int attack = 6, release = 6, step = 2;
    nlohmann::json curve;  // null = linear
    std::vector<std::string> bones;
    bool hasBones = false;
    // rotate / translate
    float rotate[3] = {0, 0, 0};
    float offset[3] = {0, 0, 0};
    bool world = false;
    // look_at
    std::string targetKind;  // "camera", "point", "bone"
    float targetPoint[3] = {0, 0, 0};
    int targetModel = -1, targetBone = -1;
    std::vector<float> weights;
    float maxDeg = 60.0f;
    // retime
    int retimeOffset = 0;
    float scale = 1.0f;
    // any op with a "bones" filter (retime / copy / mirror)
    bool hasBonesFilter = false;
    // copy
    int to = 0;
    bool mirrorCopy = false;
    // smooth
    int window = 5;
    // morph
    std::string morphName;
    std::vector<int> morphFrames;
    int every = 0;
    float peak = 1.0f;
    int morphAttack = 2, hold = 2, morphRelease = 4;
    // key_pose
    int keyFrame = 0;
    int ease = 8;
    std::vector<KeyBoneEdit> keyBones;
    // ease
    bool onlyLinear = true;
    std::vector<int> channels{0, 1, 2, 3};
    // overlap
    std::vector<std::vector<std::string>> chains;
    int lag = 2;
    // spring
    float frequency = 3.0f;
    float damping = 0.45f;
    float mix = 1.0f;
    // breathe
    int period = 105;
    float amountDeg = 1.2f;
    bool onlyHolds = true;
    float headDriftDeg = 0.8f;
};

bool ReadInt(const nlohmann::json& j, const char* name, int& out, bool required, std::string& error) {
    if (!j.contains(name)) {
        if (required) error = std::string("op requires '") + name + "'";
        return !required;
    }
    if (!j[name].is_number_integer()) {
        error = std::string(name) + " must be an integer";
        return false;
    }
    out = j[name].get<int>();
    return true;
}

bool ReadNumber(const nlohmann::json& j, const char* name, float& out, std::string& error) {
    if (!j.contains(name)) return true;
    if (!j[name].is_number()) {
        error = std::string(name) + " must be a number";
        return false;
    }
    out = j[name].get<float>();
    return true;
}

bool ReadVec3(const nlohmann::json& j, float out[3]) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!j[i].is_number()) return false;
    for (int i = 0; i < 3; ++i) out[i] = j[i].get<float>();
    return true;
}

bool ReadStringList(const nlohmann::json& j, std::vector<std::string>& out, std::string& error) {
    if (!j.is_array()) {
        error = "bones must be an array of PMX names";
        return false;
    }
    for (const nlohmann::json& b : j) {
        if (!b.is_string()) {
            error = "bones must be an array of PMX names";
            return false;
        }
        out.push_back(b.get<std::string>());
    }
    return true;
}

float SmoothStep(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

float Envelope(int frame, int start, int end, int attack, int release) {
    if (frame < start || frame > end) return 0.0f;
    float v = 1.0f;
    if (attack > 0 && frame < start + attack) v = std::min(v, SmoothStep((float)(frame - start) / (float)attack));
    if (release > 0 && frame > end - release) v = std::min(v, SmoothStep((float)(end - frame) / (float)release));
    return v;
}

DirectX::XMFLOAT4 QIdentity() { return {0, 0, 0, 1}; }

DirectX::XMFLOAT4 QNormalize(const DirectX::XMFLOAT4& q) {
    using namespace DirectX;
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionNormalize(XMLoadFloat4(&q)));
    return out;
}

DirectX::XMFLOAT4 QMul(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b) {
    using namespace DirectX;
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionMultiply(XMLoadFloat4(&a), XMLoadFloat4(&b)));
    return out;
}

DirectX::XMFLOAT4 QConj(const DirectX::XMFLOAT4& q) { return {-q.x, -q.y, -q.z, q.w}; }

DirectX::XMFLOAT4 QSlerp(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b, float t) {
    using namespace DirectX;
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionSlerp(XMLoadFloat4(&a), XMLoadFloat4(&b), t));
    return out;
}

float QAngleDeg(const DirectX::XMFLOAT4& q) {
    const float w = std::clamp(std::fabs(q.w), 0.0f, 1.0f);
    return DirectX::XMConvertToDegrees(2.0f * std::acos(w));
}

// The rotation part of a world matrix (scale removed) as a quaternion.
DirectX::XMFLOAT4 QFromMatrix(const DirectX::XMFLOAT4X4& m) {
    using namespace DirectX;
    XMMATRIX r = XMLoadFloat4x4(&m);
    r.r[0] = XMVector3Normalize(XMVectorSetW(r.r[0], 0));
    r.r[1] = XMVector3Normalize(XMVectorSetW(r.r[1], 0));
    r.r[2] = XMVector3Normalize(XMVectorSetW(r.r[2], 0));
    r.r[3] = XMVectorSet(0, 0, 0, 1);
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionRotationMatrix(r));
    return QNormalize(out);
}

DirectX::XMFLOAT3 QRotate(const DirectX::XMFLOAT4& q, const DirectX::XMFLOAT3& v) {
    using namespace DirectX;
    XMFLOAT3 out;
    XMStoreFloat3(&out, XMVector3Rotate(XMLoadFloat3(&v), XMLoadFloat4(&q)));
    return out;
}

// The shortest rotation turning unit vector `from` into unit vector `to`.
DirectX::XMFLOAT4 QFromTo(const DirectX::XMFLOAT3& from, const DirectX::XMFLOAT3& to) {
    using namespace DirectX;
    const XMVECTOR a = XMVector3Normalize(XMLoadFloat3(&from));
    const XMVECTOR b = XMVector3Normalize(XMLoadFloat3(&to));
    const float d = std::clamp(XMVectorGetX(XMVector3Dot(a, b)), -1.0f, 1.0f);
    XMVECTOR axis = XMVector3Cross(a, b);
    XMFLOAT4 out;
    if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-10f) {
        if (d >= 0.0f) return QIdentity();  // already aligned
        axis = XMVector3Cross(XMVectorSet(1, 0, 0, 0), a);
        if (XMVectorGetX(XMVector3LengthSq(axis)) < 1e-6f) axis = XMVector3Cross(XMVectorSet(0, 1, 0, 0), a);
        XMStoreFloat4(&out, XMQuaternionRotationAxis(XMVector3Normalize(axis), XM_PI));
        return QNormalize(out);
    }
    XMStoreFloat4(&out, XMQuaternionRotationAxis(XMVector3Normalize(axis), std::acos(d)));
    return QNormalize(out);
}

DirectX::XMFLOAT3 Sub3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
DirectX::XMFLOAT3 Add3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
DirectX::XMFLOAT3 Scale3(const DirectX::XMFLOAT3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Len3(const DirectX::XMFLOAT3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
DirectX::XMFLOAT3 Norm3(const DirectX::XMFLOAT3& a) {
    const float l = Len3(a);
    return l > 1e-8f ? Scale3(a, 1.0f / l) : DirectX::XMFLOAT3{0, 0, -1};
}
DirectX::XMFLOAT3 Lerp3(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

float Round3(double v) { return (float)(std::round(v * 1000.0) / 1000.0); }

struct TrackChange {
    bool morph = false;
    std::string name;
    int added = 0;
    int modified = 0;
};

struct Ctx {
    StudioDoc* doc = nullptr;
    StudioModel* model = nullptr;
    int modelIndex = -1;
    size_t written = 0;
    bool capHit = false;
    std::vector<TrackState> before;
    std::set<std::pair<int, std::string>> touched;  // (0 bone, 1 morph, name)

    void TouchBone(const std::string& name) {
        if (touched.insert({0, name}).second) before.push_back(CaptureTrack(*doc, modelIndex, RowKind::Bone, name));
    }
    void TouchMorph(const std::string& name) {
        if (touched.insert({1, name}).second) before.push_back(CaptureTrack(*doc, modelIndex, RowKind::Morph, name));
    }
    void WriteBone(const std::string& name, const BoneKf& k, TrackChange& tc) {
        TouchBone(name);
        const bool existed = FindKey(model->motion.bones[name], k.frame) != nullptr;
        UpsertKey(model->motion.bones[name], k);
        ++written;
        if (existed) ++tc.modified;
        else ++tc.added;
        if (written > kMaxWrittenKeys) capHit = true;
    }
    void WriteMorph(const std::string& name, int frame, float weight, TrackChange& tc) {
        TouchMorph(name);
        const bool existed = FindKey(model->motion.morphs[name], frame) != nullptr;
        UpsertKey(model->motion.morphs[name], MorphKf{frame, weight});
        ++written;
        if (existed) ++tc.modified;
        else ++tc.added;
        if (written > kMaxWrittenKeys) capHit = true;
    }
};

std::vector<int> LayerFrames(int start, int end, int step, const std::vector<BoneKf>& base) {
    std::set<int> frames;
    if (start > 0) frames.insert(start - 1);
    frames.insert(end + 1);
    if (step < 1) step = 1;
    for (int f = start; f <= end; f += step) frames.insert(f);
    for (const BoneKf& k : base)
        if (k.frame >= start && k.frame <= end) frames.insert(k.frame);
    return std::vector<int>(frames.begin(), frames.end());
}

TrackChange* FindChange(std::vector<TrackChange>& list, bool morph, const std::string& name) {
    for (TrackChange& c : list)
        if (c.morph == morph && c.name == name) return &c;
    list.push_back(TrackChange{morph, name, 0, 0});
    return &list.back();
}

nlohmann::json ChangesJson(const std::vector<TrackChange>& list) {
    nlohmann::json arr = nlohmann::json::array();
    for (const TrackChange& c : list)
        arr.push_back({{"kind", c.morph ? "morph" : "bone"}, {"name", c.name}, {"added", c.added},
                       {"modified", c.modified}});
    return arr;
}

nlohmann::json TracksJson(const std::vector<TrackChange>& list) {
    nlohmann::json arr = nlohmann::json::array();
    for (const TrackChange& c : list) arr.push_back(c.name);
    return arr;
}

bool ParseOp(const nlohmann::json& j, const StudioModel& m, const StudioDoc& d, Op& op, std::string& error) {
    if (!j.is_object()) {
        error = "every op must be an object";
        return false;
    }
    if (!j.contains("op") || !j["op"].is_string()) {
        error = "every op requires a string 'op'";
        return false;
    }
    const std::string name = j["op"].get<std::string>();
    if (name == "rotate") op.kind = OpKind::Rotate;
    else if (name == "translate") op.kind = OpKind::Translate;
    else if (name == "look_at") op.kind = OpKind::LookAt;
    else if (name == "retime") op.kind = OpKind::Retime;
    else if (name == "copy") op.kind = OpKind::Copy;
    else if (name == "mirror") op.kind = OpKind::Mirror;
    else if (name == "smooth") op.kind = OpKind::Smooth;
    else if (name == "morph") op.kind = OpKind::Morph;
    else if (name == "key_pose") op.kind = OpKind::KeyPose;
    else if (name == "ease") op.kind = OpKind::Ease;
    else if (name == "overlap") op.kind = OpKind::Overlap;
    else if (name == "spring") op.kind = OpKind::Spring;
    else if (name == "breathe") op.kind = OpKind::Breathe;
    else {
        error = "unknown op: " + name;
        return false;
    }

    // common "bones" list (validated against the PMX)
    if (j.contains("bones") && j["bones"].is_array()) {
        if (!ReadStringList(j["bones"], op.bones, error)) return false;
        op.hasBones = true;
        for (const std::string& b : op.bones)
            if (m.pmx->FindBone(b) < 0) {
                error = "Bone not found: " + b;
                return false;
            }
    }

    if (op.kind == OpKind::Morph) {
        if (!j.contains("morph") || !j["morph"].is_string()) {
            error = "morph op requires a string 'morph'";
            return false;
        }
        op.morphName = j["morph"].get<std::string>();
        if (m.pmx->FindMorph(op.morphName) < 0) {
            error = "Morph not found: " + op.morphName;
            return false;
        }
        if (!ReadNumber(j, "peak", op.peak, error)) return false;
        if (op.peak < 0.0f || op.peak > 1.0f) {
            error = "peak must be within 0..1";
            return false;
        }
        if (j.contains("attack")) {
            if (!ReadInt(j, "attack", op.morphAttack, true, error)) return false;
            if (op.morphAttack < 0) { error = "attack must be >= 0"; return false; }
        }
        if (j.contains("hold")) {
            if (!ReadInt(j, "hold", op.hold, true, error)) return false;
            if (op.hold < 0) { error = "hold must be >= 0"; return false; }
        }
        if (j.contains("release")) {
            if (!ReadInt(j, "release", op.morphRelease, true, error)) return false;
            if (op.morphRelease < 0) { error = "release must be >= 0"; return false; }
        }
        if (j.contains("frames")) {
            if (!j["frames"].is_array()) { error = "frames must be an array of integers"; return false; }
            for (const nlohmann::json& f : j["frames"]) {
                if (!f.is_number_integer()) { error = "frames must be an array of integers"; return false; }
                const int frame = f.get<int>();
                if (frame < 0) { error = "frames must be >= 0"; return false; }
                op.morphFrames.push_back(frame);
            }
            if (op.morphFrames.empty()) { error = "frames must not be empty"; return false; }
        } else {
            if (!ReadInt(j, "start", op.start, true, error) || !ReadInt(j, "end", op.end, true, error)) return false;
            if (!j.contains("every") || !j["every"].is_number_integer()) {
                error = "morph op needs 'frames' or 'every'";
                return false;
            }
            op.every = j["every"].get<int>();
            if (op.every < 1) { error = "every must be >= 1"; return false; }
            if (op.start > op.end) std::swap(op.start, op.end);
        }
        return true;
    }

    if (op.kind == OpKind::KeyPose) {
        if (!ReadInt(j, "frame", op.keyFrame, true, error)) return false;
        if (op.keyFrame < 0) { error = "frame must be >= 0"; return false; }
        op.start = op.end = op.keyFrame;
        if (j.contains("ease")) {
            if (!ReadInt(j, "ease", op.ease, true, error)) return false;
            if (op.ease < 1) { error = "ease must be >= 1"; return false; }
        }
    } else {
        if (!ReadInt(j, "start", op.start, true, error) || !ReadInt(j, "end", op.end, true, error)) return false;
        if (op.start > op.end) std::swap(op.start, op.end);
        if (op.start < 0) { error = "start must be >= 0"; return false; }
    }

    if (!ReadInt(j, "attack", op.attack, false, error)) return false;
    if (!ReadInt(j, "release", op.release, false, error)) return false;
    if (op.attack < 0 || op.release < 0) { error = "attack / release must be >= 0"; return false; }
    if (!ReadInt(j, "step", op.step, false, error)) return false;
    if (op.step < 1) { error = "step must be >= 1"; return false; }
    if (j.contains("curve")) {
        uint8_t tmp[64];
        if (!ApplyBoneCurve(j["curve"], tmp, error)) return false;
        op.curve = j["curve"];
    }

    switch (op.kind) {
        case OpKind::Rotate: {
            if (!op.hasBones || op.bones.empty()) { error = "rotate requires 'bones'"; return false; }
            if (!j.contains("rotate_deg") || !ReadVec3(j["rotate_deg"], op.rotate)) {
                error = "rotate requires rotate_deg [x,y,z]";
                return false;
            }
            if (j.contains("space")) {
                if (!j["space"].is_string()) { error = "space must be 'local' or 'world'"; return false; }
                const std::string s = j["space"].get<std::string>();
                if (s != "local" && s != "world") { error = "space must be 'local' or 'world'"; return false; }
                op.world = s == "world";
            }
            break;
        }
        case OpKind::Translate: {
            if (!op.hasBones) op.bones = {kCenterBone};
            for (const std::string& b : op.bones)
                if (m.pmx->FindBone(b) < 0) { error = "Bone not found: " + b; return false; }
            if (!j.contains("offset") || !ReadVec3(j["offset"], op.offset)) {
                error = "translate requires offset [x,y,z]";
                return false;
            }
            break;
        }
        case OpKind::LookAt: {
            if (!j.contains("target")) { error = "look_at requires 'target'"; return false; }
            const nlohmann::json& t = j["target"];
            if (t.is_string()) {
                if (t.get<std::string>() != "camera") {
                    error = "target must be 'camera', [x,y,z] or {model, bone}";
                    return false;
                }
                op.targetKind = "camera";
            } else if (t.is_array()) {
                if (!ReadVec3(t, op.targetPoint)) { error = "target point must be [x,y,z]"; return false; }
                op.targetKind = "point";
            } else if (t.is_object()) {
                if (!t.contains("model") || !t["model"].is_number_integer() || !t.contains("bone") ||
                    !t["bone"].is_string()) {
                    error = "target must be {model, bone}";
                    return false;
                }
                op.targetModel = t["model"].get<int>();
                if (op.targetModel < 0 || op.targetModel >= (int)d.models.size()) {
                    error = "target model index out of range: " + std::to_string(op.targetModel);
                    return false;
                }
                const studio::StudioModel& tm = *d.models[(size_t)op.targetModel];
                if (!tm.pmx) { error = "target model has no PMX data"; return false; }
                op.targetBone = tm.pmx->FindBone(t["bone"].get<std::string>());
                if (op.targetBone < 0) {
                    error = "target bone not found: " + t["bone"].get<std::string>();
                    return false;
                }
                op.targetKind = "bone";
            } else {
                error = "target must be 'camera', [x,y,z] or {model, bone}";
                return false;
            }
            if (!op.hasBones) op.bones = {kNeckBone, kHeadBone};
            if (op.bones.empty()) { error = "look_at requires at least one bone"; return false; }
            for (const std::string& b : op.bones)
                if (m.pmx->FindBone(b) < 0) { error = "Bone not found: " + b; return false; }
            if (j.contains("weights")) {
                if (!j["weights"].is_array() || j["weights"].size() != op.bones.size()) {
                    error = "weights must have one number per bone";
                    return false;
                }
                for (const nlohmann::json& w : j["weights"]) {
                    if (!w.is_number()) { error = "weights must be numbers"; return false; }
                    op.weights.push_back(w.get<float>());
                }
            } else {
                op.weights.assign(op.bones.size(), 1.0f / (float)op.bones.size());
            }
            float sum = 0;
            for (float w : op.weights) sum += std::max(0.0f, w);
            if (sum <= 1e-6f) { error = "weights must not all be zero"; return false; }
            if (!ReadNumber(j, "max_deg", op.maxDeg, error)) return false;
            if (op.maxDeg <= 0.0f) { error = "max_deg must be > 0"; return false; }
            break;
        }
        case OpKind::Retime: {
            if (!j.contains("offset") || !j["offset"].is_number_integer()) {
                error = "retime requires an integer 'offset'";
                return false;
            }
            op.retimeOffset = j["offset"].get<int>();
            if (!ReadNumber(j, "scale", op.scale, error)) return false;
            if (op.scale <= 0.0f) { error = "retime scale must be > 0"; return false; }
            op.hasBonesFilter = op.hasBones;
            break;
        }
        case OpKind::Copy: {
            if (!j.contains("to") || !j["to"].is_number_integer()) {
                error = "copy requires an integer 'to'";
                return false;
            }
            op.to = j["to"].get<int>();
            if (op.to < 0) { error = "to must be >= 0"; return false; }
            if (j.contains("mirror")) {
                if (!j["mirror"].is_boolean()) { error = "mirror must be a boolean"; return false; }
                op.mirrorCopy = j["mirror"].get<bool>();
            }
            op.hasBonesFilter = op.hasBones;
            break;
        }
        case OpKind::Mirror: {
            op.hasBonesFilter = op.hasBones;
            break;
        }
        case OpKind::Smooth: {
            if (!op.hasBones || op.bones.empty()) { error = "smooth requires 'bones'"; return false; }
            if (j.contains("window")) {
                if (!ReadInt(j, "window", op.window, true, error)) return false;
                if (op.window < 1) { error = "window must be >= 1"; return false; }
            }
            break;
        }
        case OpKind::KeyPose: {
            if (!j.contains("bones") || !j["bones"].is_object() || j["bones"].empty()) {
                error = "key_pose requires a non-empty 'bones' object";
                return false;
            }
            for (auto it = j["bones"].begin(); it != j["bones"].end(); ++it) {
                if (m.pmx->FindBone(it.key()) < 0) { error = "Bone not found: " + it.key(); return false; }
                if (!it.value().is_object()) { error = "key_pose bones must be {name: {...}}"; return false; }
                KeyBoneEdit e;
                e.name = it.key();
                if (it.value().contains("rotate_deg")) {
                    if (!ReadVec3(it.value()["rotate_deg"], e.rotate)) {
                        error = "rotate_deg must be [x,y,z]";
                        return false;
                    }
                    e.hasRot = true;
                }
                if (it.value().contains("translate")) {
                    if (!ReadVec3(it.value()["translate"], e.translate)) {
                        error = "translate must be [x,y,z]";
                        return false;
                    }
                    e.hasTranslate = true;
                }
                if (!e.hasRot && !e.hasTranslate) {
                    error = "key_pose bone '" + e.name + "' needs rotate_deg or translate";
                    return false;
                }
                op.keyBones.push_back(std::move(e));
            }
            break;
        }
        case OpKind::Morph:
            break;  // handled above
        case OpKind::Ease: {
            if (!j.contains("curve")) op.curve = "smooth";
            if (j.contains("only_linear")) {
                if (!j["only_linear"].is_boolean()) { error = "only_linear must be a boolean"; return false; }
                op.onlyLinear = j["only_linear"].get<bool>();
            }
            if (j.contains("channels")) {
                if (!j["channels"].is_array() || j["channels"].empty()) {
                    error = "channels must be a non-empty array of x / y / z / rotation";
                    return false;
                }
                op.channels.clear();
                for (const nlohmann::json& c : j["channels"]) {
                    if (c.is_number_integer()) {
                        const int ch = c.get<int>();
                        if (ch < 0 || ch > 3) { error = "channels must be 0..3 or x / y / z / rotation"; return false; }
                        op.channels.push_back(ch);
                    } else if (c.is_string()) {
                        const std::string s = c.get<std::string>();
                        int ch = -1;
                        for (int i = 0; i < 4; ++i)
                            if (s == kBoneCurveNames[i]) ch = i;
                        if (ch < 0) { error = "unknown bone curve channel: " + s; return false; }
                        op.channels.push_back(ch);
                    } else {
                        error = "channels must be 0..3 or x / y / z / rotation";
                        return false;
                    }
                }
            }
            break;
        }
        case OpKind::Overlap: {
            if (!j.contains("step")) op.step = 1;
            if (j.contains("chains")) {
                if (!j["chains"].is_array() || j["chains"].empty()) {
                    error = "chains must be a non-empty array of bone-name arrays";
                    return false;
                }
                for (const nlohmann::json& c : j["chains"]) {
                    std::vector<std::string> chain;
                    if (!c.is_array()) { error = "chains must be arrays of bone names"; return false; }
                    if (!ReadStringList(c, chain, error)) return false;
                    if (chain.size() < 2) { error = "each chain needs at least two bones"; return false; }
                    for (const std::string& b : chain)
                        if (m.pmx->FindBone(b) < 0) { error = "Bone not found: " + b; return false; }
                    op.chains.push_back(std::move(chain));
                }
            }
            if (j.contains("lag")) {
                if (!ReadInt(j, "lag", op.lag, true, error)) return false;
            }
            if (op.lag < 0 || op.lag > 12) { error = "lag must be within 0..12"; return false; }
            break;
        }
        case OpKind::Spring: {
            if (!j.contains("step")) op.step = 1;
            if (!op.hasBones || op.bones.empty()) { error = "spring requires 'bones'"; return false; }
            if (!ReadNumber(j, "frequency", op.frequency, error)) return false;
            if (!ReadNumber(j, "damping", op.damping, error)) return false;
            if (!ReadNumber(j, "mix", op.mix, error)) return false;
            if (op.frequency <= 0.0f) { error = "frequency must be > 0"; return false; }
            if (op.damping <= 0.0f) { error = "damping must be > 0"; return false; }
            if (op.mix < 0.0f || op.mix > 1.0f) { error = "mix must be within 0..1"; return false; }
            break;
        }
        case OpKind::Breathe: {
            if (j.contains("period")) {
                if (!ReadInt(j, "period", op.period, true, error)) return false;
            }
            if (!ReadNumber(j, "amount_deg", op.amountDeg, error)) return false;
            if (!ReadNumber(j, "head_drift_deg", op.headDriftDeg, error)) return false;
            if (j.contains("only_holds")) {
                if (!j["only_holds"].is_boolean()) { error = "only_holds must be a boolean"; return false; }
                op.onlyHolds = j["only_holds"].get<bool>();
            }
            if (op.period < 20) { error = "period must be >= 20"; return false; }
            break;
        }
    }
    return true;
}

// Parent world rotation of `bone` at `frame`, from a sampled world table (`indices` maps bone index -> column).
DirectX::XMFLOAT4 ParentWorldQ(const std::vector<DirectX::XMFLOAT4X4>& row, const std::vector<int>& indices,
                              int parent, const DirectX::XMFLOAT4& rootQ) {
    if (parent < 0) return rootQ;
    const auto it = std::lower_bound(indices.begin(), indices.end(), parent);
    if (it == indices.end() || *it != parent) return rootQ;
    return QFromMatrix(row[(size_t)(it - indices.begin())]);
}

// Rotation vector (axis * angle) of a unit quaternion, and its inverse.
DirectX::XMFLOAT3 QLog(const DirectX::XMFLOAT4& qIn) {
    DirectX::XMFLOAT4 q = QNormalize(qIn);
    if (q.w < 0) q = {-q.x, -q.y, -q.z, -q.w};
    const DirectX::XMFLOAT3 v = {q.x, q.y, q.z};
    const float s = Len3(v);
    if (s < 1e-8f) return {0, 0, 0};
    const float angle = 2.0f * std::atan2(s, std::clamp(q.w, -1.0f, 1.0f));
    return Scale3(v, angle / s);
}

DirectX::XMFLOAT4 QExp(const DirectX::XMFLOAT3& rv) {
    const float angle = Len3(rv);
    if (angle < 1e-8f) return QIdentity();
    using namespace DirectX;
    const XMVECTOR axis = XMVector3Normalize(XMLoadFloat3(&rv));
    XMFLOAT4 out;
    XMStoreFloat4(&out, XMQuaternionRotationAxis(axis, angle));
    return QNormalize(out);
}

bool IsLinearCurve(const uint8_t c[4]) {
    return std::abs((int)c[0] - (int)c[1]) <= 2 && std::abs((int)c[2] - (int)c[3]) <= 2;
}

bool BoneHasKeysInRange(const std::vector<BoneKf>& keys, int start, int end) {
    for (const BoneKf& k : keys)
        if (k.frame >= start && k.frame <= end) return true;
    return false;
}

}  // namespace

bool App::ExecuteMcpMotion(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise) {
    if (tool != "studio_motion_edit") return false;
    if (!promise) return false;
    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;
    if (!args.contains("model") || !args["model"].is_number_integer()) {
        promise->Reject("studio_motion_edit requires an integer 'model'");
        return true;
    }
    const int modelIndex = args["model"].get<int>();
    if (modelIndex < 0 || modelIndex >= (int)d.models.size()) {
        promise->Reject("Model index out of range: " + std::to_string(modelIndex));
        return true;
    }
    StudioModel& m = *d.models[(size_t)modelIndex];
    if (m.kind != ModelKind::Character) {
        promise->Reject("studio_motion_edit requires a character model");
        return true;
    }
    if (!m.pmx || !m.inst) {
        promise->Reject("The model has no PMX data or instance");
        return true;
    }
    if (!args.contains("ops") || !args["ops"].is_array() || args["ops"].empty()) {
        promise->Reject("ops must be a non-empty array");
        return true;
    }
    const bool dryRun = args.value("dry_run", false);

    std::vector<Op> ops;
    ops.reserve(args["ops"].size());
    for (const nlohmann::json& j : args["ops"]) {
        Op op;
        std::string error;
        if (!ParseOp(j, m, d, op, error)) {
            promise->Reject(error);
            return true;
        }
        ops.push_back(std::move(op));
    }
    for (const Op& op : ops) {
        if (op.kind == OpKind::LookAt && op.targetKind == "camera" && d.SelectedCameraTrack().camera.empty()) {
            promise->Reject("look_at target 'camera' needs a motion camera");
            return true;
        }
    }

    Ctx ctx;
    ctx.doc = &d;
    ctx.model = &m;
    ctx.modelIndex = modelIndex;

    const std::map<std::string, std::vector<BoneKf>> bonesBefore = m.motion.bones;
    const std::map<std::string, std::vector<MorphKf>> morphsBefore = m.motion.morphs;
    const DirectX::XMFLOAT4 rootQ = EulerToQuat(m.place.rotationDeg.x, m.place.rotationDeg.y, m.place.rotationDeg.z);

    nlohmann::json opResults = nlohmann::json::array();
    std::string error;

    for (const Op& op : ops) {
        const size_t beforeCount = ctx.written;
        std::vector<TrackChange> changes;
        nlohmann::json extra = nlohmann::json::object();

        switch (op.kind) {
            // ---- rotate / translate: additive baked layer ------------------
            case OpKind::Rotate:
            case OpKind::Translate: {
                const bool isRotate = op.kind == OpKind::Rotate;
                const bool worldRotate = isRotate && op.world;
                bool anyCentered = false;
                struct Base {
                    std::string name;
                    int index = -1, parent = -1;
                    std::vector<BoneKf> keys;
                    DirectX::XMFLOAT4 offset = {0, 0, 0, 1};
                };
                std::vector<Base> bases;
                bases.reserve(op.bones.size());
                for (const std::string& name : op.bones) {
                    Base b;
                    b.name = name;
                    b.index = m.pmx->FindBone(name);
                    b.parent = m.pmx->bones[(size_t)b.index].parentIndex;
                    const auto it = m.motion.bones.find(name);
                    if (it != m.motion.bones.end()) b.keys = it->second;
                    if (isRotate) {
                        b.offset = EulerToQuat(op.rotate[0], op.rotate[1], op.rotate[2]);
                    } else {
                        const bool centered = name == kCenterBone || name == kAllParentBone;
                        anyCentered = anyCentered || centered;
                        b.offset = QIdentity();
                        b.offset.x = op.offset[0];
                        b.offset.y = op.offset[1];
                        b.offset.z = op.offset[2];
                    }
                    bases.push_back(std::move(b));
                }

                std::vector<int> indices;
                for (const Base& b : bases) {
                    indices.push_back(b.index);
                    if (b.parent >= 0) indices.push_back(b.parent);
                }
                std::sort(indices.begin(), indices.end());
                indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

                std::vector<std::vector<int>> frameLists;
                std::set<int> allFrames;
                for (const Base& b : bases) {
                    frameLists.push_back(LayerFrames(op.start, op.end, op.step, b.keys));
                    for (int f : frameLists.back()) allFrames.insert(f);
                }
                std::vector<int> frames(allFrames.begin(), allFrames.end());

                const bool needWorld = worldRotate || (!isRotate && anyCentered);
                std::vector<std::vector<DirectX::XMFLOAT4X4>> worlds;
                if (needWorld) worlds = StudioSampleBoneWorlds(m, frames, indices);

                bool failed = false;
                for (size_t bi = 0; bi < bases.size() && !failed; ++bi) {
                    Base& b = bases[bi];
                    const bool centered = b.name == kCenterBone || b.name == kAllParentBone;
                    for (int f : frameLists[bi]) {
                        const float env = Envelope(f, op.start, op.end, op.attack, op.release);
                        BoneKf base = b.keys.empty() ? BoneKf{} : SampleBone(b.keys, f);
                        base.frame = f;
                        BoneKf k = base;
                        if (!ApplyBoneCurve(op.curve, k.interp, error)) { failed = true; break; }
                        const size_t fi = (size_t)(std::lower_bound(frames.begin(), frames.end(), f) - frames.begin());
                        const std::vector<DirectX::XMFLOAT4X4>* row =
                            (needWorld && fi < worlds.size()) ? &worlds[fi] : nullptr;
                        if (isRotate) {
                            // QMul(a, b) = a then b (DirectXMath). The offset turns the bone about its PARENT's
                            // axes after the motion's own rotation: local' = local then offset. A world-space offset D
                            // becomes P then D then P^-1 in the parent frame (P = parent world rotation), so that
                            // local' then P = local then P then D.
                            DirectX::XMFLOAT4 local = QSlerp(QIdentity(), b.offset, env);
                            if (worldRotate && row) {
                                const DirectX::XMFLOAT4 parentQ = ParentWorldQ(*row, indices, b.parent, rootQ);
                                local = QMul(QMul(parentQ, local), QConj(parentQ));
                            }
                            k.r = QNormalize(QMul(base.r, local));
                        } else {
                            DirectX::XMFLOAT3 delta = Scale3({b.offset.x, b.offset.y, b.offset.z}, env);
                            if (centered && row) {
                                const DirectX::XMFLOAT4 parentQ = ParentWorldQ(*row, indices, b.parent, rootQ);
                                delta = QRotate(QConj(parentQ), delta);
                            }
                            k.t = Add3(base.t, delta);
                        }
                        TrackChange* tc = FindChange(changes, false, b.name);
                        ctx.WriteBone(b.name, k, *tc);
                        if (ctx.capHit) { failed = true; break; }
                    }
                }
                if (failed) goto done;
                break;
            }

            // ---- look_at ---------------------------------------------------
            case OpKind::LookAt: {
                struct LookBase {
                    std::string name;
                    int index = -1, parent = -1;
                    std::vector<BoneKf> keys;
                    float weight = 0;
                };
                std::vector<LookBase> bases;
                float wsum = 0;
                for (float w : op.weights) wsum += std::max(0.0f, w);
                for (size_t i = 0; i < op.bones.size(); ++i) {
                    LookBase b;
                    b.name = op.bones[i];
                    b.index = m.pmx->FindBone(b.name);
                    b.parent = m.pmx->bones[(size_t)b.index].parentIndex;
                    const auto it = m.motion.bones.find(b.name);
                    if (it != m.motion.bones.end()) b.keys = it->second;
                    b.weight = std::max(0.0f, op.weights[i]) / wsum;
                    bases.push_back(std::move(b));
                }
                const int headBone = bases.back().index;

                std::vector<int> indices;
                for (const LookBase& b : bases) {
                    indices.push_back(b.index);
                    if (b.parent >= 0) indices.push_back(b.parent);
                }
                std::sort(indices.begin(), indices.end());
                indices.erase(std::unique(indices.begin(), indices.end()), indices.end());

                std::vector<std::vector<int>> frameLists;
                std::set<int> allFrames;
                for (const LookBase& b : bases) {
                    frameLists.push_back(LayerFrames(op.start, op.end, op.step, b.keys));
                    for (int f : frameLists.back()) allFrames.insert(f);
                }
                std::vector<int> frames(allFrames.begin(), allFrames.end());
                const std::vector<std::vector<DirectX::XMFLOAT4X4>> worlds =
                    StudioSampleBoneWorlds(m, frames, indices);

                // Target positions, one per frame.
                std::vector<DirectX::XMFLOAT3> targets;
                targets.reserve(frames.size());
                if (op.targetKind == "bone") {
                    StudioModel& tm = *d.models[(size_t)op.targetModel];
                    const std::vector<std::vector<DirectX::XMFLOAT3>> pos =
                        StudioSampleBones(tm, frames, {op.targetBone});
                    for (const auto& r : pos) targets.push_back(r.empty() ? DirectX::XMFLOAT3{0, 0, 0} : r[0]);
                } else if (op.targetKind == "camera") {
                    d.RefreshCameraEvals();   // S4: one evaluator per camera
                    for (int f : frames) {
                        const std::shared_ptr<CameraMotion>& eval = d.ActiveCameraEval(f);
                        CameraPose pose = eval ? eval->Evaluate((float)f) : CameraPose{};
                        DirectX::XMFLOAT3 eye;
                        CameraMotion::ToView(pose, nullptr, &eye);
                        targets.push_back(eye);
                    }
                } else {
                    targets.assign(frames.size(), {op.targetPoint[0], op.targetPoint[1], op.targetPoint[2]});
                }

                std::vector<int> measureFrames;
                bool failed = false;
                for (size_t bi = 0; bi < bases.size() && !failed; ++bi) {
                    const LookBase& b = bases[bi];
                    for (int f : frameLists[bi]) {
                        const size_t fi = (size_t)(std::lower_bound(frames.begin(), frames.end(), f) - frames.begin());
                        const float env = Envelope(f, op.start, op.end, op.attack, op.release);
                        BoneKf base = b.keys.empty() ? BoneKf{} : SampleBone(b.keys, f);
                        base.frame = f;
                        BoneKf k = base;
                        if (!ApplyBoneCurve(op.curve, k.interp, error)) { failed = true; break; }
                        if (env > 0.0f && fi < worlds.size()) {
                            const auto hit = std::lower_bound(indices.begin(), indices.end(), headBone);
                            if (hit != indices.end() && *hit == headBone) {
                                const DirectX::XMFLOAT4X4& headWorld = worlds[fi][(size_t)(hit - indices.begin())];
                                const DirectX::XMFLOAT4 headQ = QFromMatrix(headWorld);
                                const DirectX::XMFLOAT3 fwd = QRotate(headQ, {0, 0, -1});  // MMD faces -Z
                                const DirectX::XMFLOAT3 pos = {headWorld._41, headWorld._42, headWorld._43};
                                const DirectX::XMFLOAT3 dir = Norm3(Sub3(targets[fi], pos));
                                DirectX::XMFLOAT4 delta = QFromTo(fwd, dir);
                                const float angle = QAngleDeg(delta);
                                if (angle > op.maxDeg && angle > 1e-6f) delta = QSlerp(QIdentity(), delta, op.maxDeg / angle);
                                if (b.weight > 0.0f) {
                                    // split the world offset across the chain by weight
                                    const DirectX::XMFLOAT4 worldDelta = QSlerp(QIdentity(), delta, b.weight * env);
                                    const DirectX::XMFLOAT4 parentQ = ParentWorldQ(worlds[fi], indices, b.parent, rootQ);
                                    // world delta -> parent frame, applied after the motion (see rotate above)
                                    const DirectX::XMFLOAT4 local = QMul(QMul(parentQ, worldDelta), QConj(parentQ));
                                    k.r = QNormalize(QMul(base.r, local));
                                }
                            }
                        }
                        TrackChange* tc = FindChange(changes, false, b.name);
                        ctx.WriteBone(b.name, k, *tc);
                        if (f >= op.start && f <= op.end && env > 0.0f) measureFrames.push_back(f);
                        if (ctx.capHit) { failed = true; break; }
                    }
                }
                if (failed) goto done;

                // Re-sample (writes and measures only outside dry runs) and report the mean remaining angle error.
                m.motionVersion++;
                {
                    std::sort(measureFrames.begin(), measureFrames.end());
                    measureFrames.erase(std::unique(measureFrames.begin(), measureFrames.end()), measureFrames.end());
                    if (!measureFrames.empty()) {
                        std::vector<DirectX::XMFLOAT3> mt;
                        if (op.targetKind == "bone") {
                            StudioModel& tm = *d.models[(size_t)op.targetModel];
                            const std::vector<std::vector<DirectX::XMFLOAT3>> pos =
                                StudioSampleBones(tm, measureFrames, {op.targetBone});
                            for (const auto& r : pos) mt.push_back(r.empty() ? DirectX::XMFLOAT3{0, 0, 0} : r[0]);
                        } else if (op.targetKind == "camera") {
                            d.RefreshCameraEvals();
                            for (int f : measureFrames) {
                                const std::shared_ptr<CameraMotion>& eval = d.ActiveCameraEval(f);
                                CameraPose pose = eval ? eval->Evaluate((float)f) : CameraPose{};
                                DirectX::XMFLOAT3 eye;
                                CameraMotion::ToView(pose, nullptr, &eye);
                                mt.push_back(eye);
                            }
                        } else {
                            mt.assign(measureFrames.size(), {op.targetPoint[0], op.targetPoint[1], op.targetPoint[2]});
                        }
                        const std::vector<std::vector<DirectX::XMFLOAT4X4>> mw =
                            StudioSampleBoneWorlds(m, measureFrames, {headBone});
                        double sum = 0;
                        size_t n = 0;
                        for (size_t i = 0; i < measureFrames.size() && i < mw.size(); ++i) {
                            if (mw[i].empty()) continue;
                            const DirectX::XMFLOAT4 q = QFromMatrix(mw[i][0]);
                            const DirectX::XMFLOAT3 pos = {mw[i][0]._41, mw[i][0]._42, mw[i][0]._43};
                            const DirectX::XMFLOAT3 fwd = QRotate(q, {0, 0, -1});
                            const DirectX::XMFLOAT3 dir = Norm3(Sub3(i < mt.size() ? mt[i] : DirectX::XMFLOAT3{0, 0, 0}, pos));
                            sum += QAngleDeg(QFromTo(fwd, dir));
                            ++n;
                        }
                        if (n) extra["mean_error_deg"] = Round3(sum / (double)n);
                    }
                }
                break;
            }

            // ---- retime ----------------------------------------------------
            case OpKind::Retime: {
                std::vector<std::string> boneNames;
                if (op.hasBonesFilter) boneNames = op.bones;
                else
                    for (const auto& [name, keys] : m.motion.bones) boneNames.push_back(name);
                std::vector<std::string> morphNames;
                if (!op.hasBonesFilter)
                    for (const auto& [name, keys] : m.motion.morphs) morphNames.push_back(name);

                bool failed = false;
                for (const std::string& name : boneNames) {
                    const auto it = m.motion.bones.find(name);
                    if (it == m.motion.bones.end()) continue;
                    ctx.TouchBone(name);  // snapshot before the source range is erased
                    std::vector<BoneKf> moved;
                    for (const BoneKf& k : it->second)
                        if (k.frame >= op.start && k.frame <= op.end) {
                            BoneKf mk = k;
                            mk.frame = std::max(0, (int)std::lround((float)op.start + (float)(k.frame - op.start) * op.scale) +
                                                       op.retimeOffset);
                            moved.push_back(mk);
                        }
                    if (moved.empty()) continue;
                    std::sort(moved.begin(), moved.end(), [](const BoneKf& a, const BoneKf& b) { return a.frame < b.frame; });
                    // erase the source range first, then write the moved keys (replacing keys outside it)
                    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                                    [&](const BoneKf& k) { return k.frame >= op.start && k.frame <= op.end; }),
                                     it->second.end());
                    TrackChange* tc = FindChange(changes, false, name);
                    for (const BoneKf& k : moved) ctx.WriteBone(name, k, *tc);
                    if (ctx.capHit) { failed = true; break; }
                }
                for (const std::string& name : morphNames) {
                    if (failed) break;
                    const auto it = m.motion.morphs.find(name);
                    if (it == m.motion.morphs.end()) continue;
                    ctx.TouchMorph(name);  // snapshot before the source range is erased
                    std::vector<MorphKf> moved;
                    for (const MorphKf& k : it->second)
                        if (k.frame >= op.start && k.frame <= op.end) {
                            MorphKf mk = k;
                            mk.frame = std::max(0, (int)std::lround((float)op.start + (float)(k.frame - op.start) * op.scale) +
                                                       op.retimeOffset);
                            moved.push_back(mk);
                        }
                    if (moved.empty()) continue;
                    std::sort(moved.begin(), moved.end(), [](const MorphKf& a, const MorphKf& b) { return a.frame < b.frame; });
                    it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                                    [&](const MorphKf& k) { return k.frame >= op.start && k.frame <= op.end; }),
                                     it->second.end());
                    TrackChange* tc = FindChange(changes, true, name);
                    for (const MorphKf& k : moved) ctx.WriteMorph(name, k.frame, k.weight, *tc);
                    if (ctx.capHit) { failed = true; break; }
                }
                if (failed) goto done;
                break;
            }

            // ---- copy ------------------------------------------------------
            case OpKind::Copy: {
                bool failed = false;
                if (op.mirrorCopy) {
                    std::vector<std::string> source;
                    if (op.hasBonesFilter) source = op.bones;
                    else
                        for (const auto& [name, keys] : m.motion.bones) source.push_back(name);
                    for (const std::string& name : source) {
                        const auto it = m.motion.bones.find(name);
                        if (it == m.motion.bones.end()) continue;
                        const std::vector<BoneKf> snap = it->second;
                        const int idx = m.pmx->FindBone(name);
                        const int mirrorIdx = idx >= 0 ? MirrorBoneIndex(*m.pmx, idx) : idx;
                        if (mirrorIdx < 0) continue;
                        const std::string target = m.pmx->bones[(size_t)mirrorIdx].name;
                        TrackChange* tc = FindChange(changes, false, target);
                        for (const BoneKf& k : snap) {
                            if (k.frame < op.start || k.frame > op.end) continue;
                            const int dest = op.to + (k.frame - op.start);
                            if (dest < 0) continue;
                            BoneKf mk = k;
                            mk.frame = dest;
                            const PoseBone p = MirrorPose(PoseBone{k.t, k.r});
                            mk.t = p.t;
                            mk.r = p.r;
                            ctx.WriteBone(target, mk, *tc);
                        }
                        if (ctx.capHit) { failed = true; break; }
                    }
                } else {
                    std::vector<std::string> boneNames;
                    if (op.hasBonesFilter) boneNames = op.bones;
                    else
                        for (const auto& [name, keys] : m.motion.bones) boneNames.push_back(name);
                    // snapshot all sources first (a later write must not feed an earlier copy)
                    std::map<std::string, std::vector<BoneKf>> boneSnap;
                    for (const std::string& name : boneNames) {
                        const auto it = m.motion.bones.find(name);
                        if (it != m.motion.bones.end()) boneSnap[name] = it->second;
                    }
                    std::map<std::string, std::vector<MorphKf>> morphSnap;
                    if (!op.hasBonesFilter)
                        for (const auto& [name, keys] : m.motion.morphs) morphSnap[name] = keys;
                    for (const auto& [name, snap] : boneSnap) {
                        TrackChange* tc = FindChange(changes, false, name);
                        for (const BoneKf& k : snap) {
                            if (k.frame < op.start || k.frame > op.end) continue;
                            const int dest = op.to + (k.frame - op.start);
                            if (dest < 0) continue;
                            BoneKf mk = k;
                            mk.frame = dest;
                            ctx.WriteBone(name, mk, *tc);
                        }
                        if (ctx.capHit) { failed = true; break; }
                    }
                    for (const auto& [name, snap] : morphSnap) {
                        if (failed) break;
                        TrackChange* tc = FindChange(changes, true, name);
                        for (const MorphKf& k : snap) {
                            if (k.frame < op.start || k.frame > op.end) continue;
                            const int dest = op.to + (k.frame - op.start);
                            if (dest < 0) continue;
                            ctx.WriteMorph(name, dest, k.weight, *tc);
                        }
                        if (ctx.capHit) { failed = true; break; }
                    }
                }
                if (failed) goto done;
                break;
            }

            // ---- mirror ----------------------------------------------------
            case OpKind::Mirror: {
                std::vector<std::string> source;
                if (op.hasBonesFilter) source = op.bones;
                else
                    for (const auto& [name, keys] : m.motion.bones) source.push_back(name);
                // snapshot every source track before writing any mirror (both sides read the original)
                std::map<std::string, std::vector<BoneKf>> snap;
                for (const std::string& name : source) {
                    const auto it = m.motion.bones.find(name);
                    if (it != m.motion.bones.end()) snap[name] = it->second;
                }
                bool failed = false;
                for (const auto& [name, keys] : snap) {
                    const int idx = m.pmx->FindBone(name);
                    const int mirrorIdx = idx >= 0 ? MirrorBoneIndex(*m.pmx, idx) : idx;
                    if (mirrorIdx < 0) continue;
                    const std::string target = m.pmx->bones[(size_t)mirrorIdx].name;
                    TrackChange* tc = FindChange(changes, false, target);
                    for (const BoneKf& k : keys) {
                        if (k.frame < op.start || k.frame > op.end) continue;
                        BoneKf mk = k;
                        const PoseBone p = MirrorPose(PoseBone{k.t, k.r});
                        mk.t = p.t;
                        mk.r = p.r;
                        ctx.WriteBone(target, mk, *tc);
                    }
                    if (ctx.capHit) { failed = true; break; }
                }
                if (failed) goto done;
                break;
            }

            // ---- smooth ----------------------------------------------------
            case OpKind::Smooth: {
                float maxChange = 0;
                bool failed = false;
                for (const std::string& name : op.bones) {
                    const auto it = m.motion.bones.find(name);
                    std::vector<BoneKf> base;
                    if (it != m.motion.bones.end()) base = it->second;
                    std::vector<int> frames;
                    for (int f = op.start; f <= op.end; f += op.step) frames.push_back(f);
                    if (frames.empty()) continue;
                    std::vector<BoneKf> samples;
                    for (int f : frames) {
                        BoneKf k = base.empty() ? BoneKf{} : SampleBone(base, f);
                        k.frame = f;
                        samples.push_back(k);
                    }
                    TrackChange* tc = FindChange(changes, false, name);
                    const int half = op.window / 2;
                    for (size_t i = 0; i < samples.size(); ++i) {
                        DirectX::XMFLOAT3 t{0, 0, 0};
                        const DirectX::XMFLOAT4 ref = samples[i].r;
                        DirectX::XMFLOAT4 q{0, 0, 0, 0};
                        int n = 0;
                        for (size_t j = 0; j < samples.size(); ++j) {
                            if (std::abs((int)j - (int)i) > half) continue;
                            t = Add3(t, samples[j].t);
                            DirectX::XMFLOAT4 qj = samples[j].r;
                            if (qj.x * ref.x + qj.y * ref.y + qj.z * ref.z + qj.w * ref.w < 0)
                                qj = {-qj.x, -qj.y, -qj.z, -qj.w};  // hemisphere alignment
                            q.x += qj.x; q.y += qj.y; q.z += qj.z; q.w += qj.w;
                            ++n;
                        }
                        if (n == 0) continue;
                        t = Scale3(t, 1.0f / (float)n);
                        q = QNormalize(q);
                        const int f = frames[i];
                        const float env = Envelope(f, op.start, op.end, op.attack, op.release);
                        BoneKf out = samples[i];
                        out.t = Lerp3(samples[i].t, t, env);
                        out.r = QNormalize(QSlerp(samples[i].r, q, env));
                        if (!ApplyBoneCurve(op.curve, out.interp, error)) { failed = true; break; }
                        maxChange = std::max(maxChange, QAngleDeg(QMul(QConj(samples[i].r), out.r)));
                        ctx.WriteBone(name, out, *tc);
                        if (ctx.capHit) { failed = true; break; }
                    }
                    if (failed) break;
                }
                if (failed) goto done;
                extra["max_change_deg"] = Round3(maxChange);
                break;
            }

            // ---- ease: rewrite interpolation curves of existing keys ---------
            case OpKind::Ease: {
                std::vector<std::string> names;
                if (op.hasBones) names = op.bones;
                else
                    for (const auto& [name, keys] : m.motion.bones)
                        if (BoneHasKeysInRange(keys, op.start, op.end)) names.push_back(name);
                uint8_t target[64];
                if (!ApplyBoneCurve(op.curve, target, error)) goto done;
                int curvesChanged = 0;
                bool failed = false;
                for (const std::string& name : names) {
                    const auto it = m.motion.bones.find(name);
                    if (it == m.motion.bones.end()) continue;
                    TrackChange* tc = FindChange(changes, false, name);
                    for (const BoneKf& base : it->second) {
                        if (base.frame < op.start || base.frame > op.end) continue;
                        BoneKf k = base;
                        int changed = 0;
                        for (int ch : op.channels) {
                            uint8_t old[4], want[4];
                            GetBoneCurve(k.interp, ch, old);
                            GetBoneCurve(target, ch, want);
                            if (op.onlyLinear && !IsLinearCurve(old)) continue;
                            if (std::equal(old, old + 4, want)) continue;
                            SetBoneCurve(k.interp, ch, want);
                            ++changed;
                        }
                        if (changed == 0) continue;
                        curvesChanged += changed;
                        ctx.WriteBone(name, k, *tc);
                        if (ctx.capHit) { failed = true; break; }
                    }
                    if (failed) break;
                }
                if (failed) goto done;
                extra["curves_changed"] = curvesChanged;
                break;
            }

            // ---- overlap: per-chain follow-through --------------------------
            case OpKind::Overlap: {
                std::vector<std::vector<std::string>> chains = op.chains;
                if (chains.empty())
                    for (const auto& c : kDefaultChains) chains.push_back({c[0], c[1], c[2]});
                bool failed = false;
                for (const std::vector<std::string>& chain : chains) {
                    for (size_t k = 1; k < chain.size() && !failed; ++k) {
                        const std::string& name = chain[k];
                        const auto it = m.motion.bones.find(name);
                        if (it == m.motion.bones.end()) continue;
                        const std::vector<BoneKf> orig = it->second;
                        std::vector<int> frames = LayerFrames(op.start, op.end, op.step, orig);
                        TrackChange* tc = FindChange(changes, false, name);
                        const int shift = (int)k * op.lag;
                        for (int f : frames) {
                            BoneKf out = SampleBone(orig, f);
                            out.frame = f;
                            const float env = Envelope(f, op.start, op.end, op.attack, op.release);
                            if (env > 0.0f) {
                                const BoneKf shifted = SampleBone(orig, f - shift);
                                out.t = Lerp3(out.t, shifted.t, env);
                                out.r = QNormalize(QSlerp(out.r, shifted.r, env));
                            }
                            ctx.WriteBone(name, out, *tc);
                            if (ctx.capHit) { failed = true; break; }
                        }
                    }
                }
                if (failed) goto done;
                break;
            }

            // ---- spring: second-order damped secondary motion ---------------
            case OpKind::Spring: {
                constexpr float kDt = 1.0f / (30.0f * 4.0f);  // 30 fps, 4 substeps
                const float omega = 2.0f * DirectX::XM_PI * op.frequency;
                const float c = 2.0f * op.damping * omega;
                const float kSpring = omega * omega;
                const int simStart = std::max(0, op.start - op.attack);
                float maxDev = 0.0f;
                bool failed = false;
                for (const std::string& name : op.bones) {
                    const auto it = m.motion.bones.find(name);
                    std::vector<BoneKf> base;
                    if (it != m.motion.bones.end()) base = it->second;
                    const bool isTranslate = name == kCenterBone || name == kGrooveBone;
                    BoneKf startKey = base.empty() ? BoneKf{} : SampleBone(base, simStart);
                    DirectX::XMFLOAT4 qs = startKey.r;
                    DirectX::XMFLOAT3 w{0, 0, 0};
                    DirectX::XMFLOAT3 xs = isTranslate ? startKey.t : DirectX::XMFLOAT3{0, 0, 0};
                    DirectX::XMFLOAT3 v{0, 0, 0};
                    std::map<int, BoneKf> baked;
                    for (int f = simStart; f <= op.end; ++f) {
                        BoneKf orig = base.empty() ? BoneKf{} : SampleBone(base, f);
                        orig.frame = f;
                        for (int sub = 0; sub < 4; ++sub) {
                            if (isTranslate) {
                                const DirectX::XMFLOAT3 err = Sub3(orig.t, xs);
                                const DirectX::XMFLOAT3 a = Add3(Scale3(err, kSpring), Scale3(v, -c));
                                v = Add3(v, Scale3(a, kDt));
                                xs = Add3(xs, Scale3(v, kDt));
                            } else {
                                const DirectX::XMFLOAT3 err = QLog(QMul(QConj(qs), orig.r));
                                const DirectX::XMFLOAT3 a = Add3(Scale3(err, kSpring), Scale3(w, -c));
                                w = Add3(w, Scale3(a, kDt));
                                qs = QNormalize(QMul(qs, QExp(Scale3(w, kDt))));
                            }
                        }
                        if (f >= op.start) baked[f] = orig;
                        if (!isTranslate) {
                            baked[f].r = qs;
                            baked[f].t = orig.t;
                        } else {
                            baked[f].t = xs;
                            baked[f].r = orig.r;
                        }
                        if (f >= op.start) {
                            const float dev = isTranslate ? Len3(Sub3(xs, orig.t))
                                                          : QAngleDeg(QMul(QConj(orig.r), qs));
                            maxDev = std::max(maxDev, dev);
                        }
                    }
                    std::vector<int> frames = LayerFrames(op.start, op.end, op.step, base);
                    TrackChange* tc = FindChange(changes, false, name);
                    for (int f : frames) {
                        const BoneKf orig = base.empty() ? BoneKf{} : SampleBone(base, f);
                        const auto hit = baked.find(f);
                        BoneKf out = orig;
                        out.frame = f;
                        if (hit != baked.end()) {
                            const float env = Envelope(f, op.start, op.end, op.attack, op.release) * op.mix;
                            if (isTranslate) out.t = Lerp3(orig.t, hit->second.t, env);
                            else out.r = QNormalize(QSlerp(orig.r, hit->second.r, env));
                        }
                        ctx.WriteBone(name, out, *tc);
                        if (ctx.capHit) { failed = true; break; }
                    }
                    if (failed) break;
                }
                if (failed) goto done;
                extra["max_deviation_deg"] = Round3(maxDev);
                break;
            }

            // ---- breathe: additive idle life --------------------------------
            case OpKind::Breathe: {
                std::vector<std::string> names = op.bones;
                if (!op.hasBones) {
                    if (m.pmx->FindBone(kUpperBody2Bone) >= 0) names = {kUpperBody2Bone};
                    else if (m.pmx->FindBone(kUpperBodyBone) >= 0) names = {kUpperBodyBone};
                }
                const int headIndex = m.pmx->FindBone(kHeadBone);
                if (headIndex >= 0 && op.headDriftDeg > 0.0f &&
                    std::find(names.begin(), names.end(), kHeadBone) == names.end())
                    names.push_back(kHeadBone);

                // Hold ranges of the torso group (same rule as studio_motion_quality).
                std::vector<std::pair<int, int>> holds;
                if (op.onlyHolds) {
                    std::vector<std::string> torso;
                    for (const char* n : {kUpperBodyBone, kUpperBody2Bone, "\xE4\xB8\x8B\xE5\x8D\x8A\xE8\xBA\xAB"})
                        if (m.pmx->FindBone(n) >= 0) torso.push_back(n);
                    std::vector<float> speed((size_t)(op.end - op.start + 1), 0.0f);
                    bool haveTorso = false;
                    for (const std::string& name : torso) {
                        const auto it = m.motion.bones.find(name);
                        if (it == m.motion.bones.end()) continue;
                        haveTorso = true;
                        for (int f = op.start; f <= op.end; ++f) {
                            BoneKf a = SampleBone(it->second, f);
                            BoneKf b = SampleBone(it->second, f + 1);
                            const float deg = QAngleDeg(QMul(QConj(a.r), b.r));
                            const float dist = Len3(Sub3(a.t, b.t));
                            const size_t i = (size_t)(f - op.start);
                            speed[i] = std::max(speed[i], std::max(deg / 0.15f, dist / 0.01f));
                        }
                    }
                    if (haveTorso) {
                        int runStart = -1;
                        for (int f = op.start; f <= op.end + 1; ++f) {
                            const bool still = f <= op.end && speed[(size_t)(f - op.start)] < 1.0f;
                            if (still && runStart < 0) runStart = f;
                            if (!still && runStart >= 0) {
                                if (f - runStart >= 15) holds.push_back({runStart, f - 1});
                                runStart = -1;
                            }
                        }
                    }
                }

                auto applyLayer = [&](const std::string& name, int f, float env, bool head) {
                    const auto it = m.motion.bones.find(name);
                    std::vector<BoneKf> base;
                    if (it != m.motion.bones.end()) base = it->second;
                    BoneKf out = base.empty() ? BoneKf{} : SampleBone(base, f);
                    out.frame = f;
                    const float phase = (float)(f - op.start) / (float)op.period * 2.0f * DirectX::XM_PI;
                    float pitch = std::sin(phase) * op.amountDeg;
                    if (head) {
                        // deterministic smooth low-frequency drift, ~2.5 s period (~75 frames)
                        const float t = (float)f;
                        const float drift = op.headDriftDeg *
                                            (0.6f * std::sin(t * 2.0f * DirectX::XM_PI / 75.0f + 1.7f) +
                                             0.4f * std::sin(t * 2.0f * DirectX::XM_PI / 131.0f + 0.3f));
                        pitch = drift;
                    }
                    out.r = QNormalize(QMul(out.r, EulerToQuat(pitch * env, 0, 0)));
                    return out;
                };

                bool failed = false;
                for (const std::string& name : names) {
                    if (m.pmx->FindBone(name) < 0) continue;
                    const bool head = name == kHeadBone;
                    const auto it = m.motion.bones.find(name);
                    std::vector<BoneKf> base;
                    if (it != m.motion.bones.end()) base = it->second;
                    std::vector<int> frames = LayerFrames(op.start, op.end, op.step, base);
                    TrackChange* tc = FindChange(changes, false, name);
                    for (int f : frames) {
                        float env = Envelope(f, op.start, op.end, op.attack, op.release);
                        if (op.onlyHolds) {
                            float holdEnv = 0.0f;
                            for (const auto& h : holds) {
                                if (f >= h.first && f <= h.second) { holdEnv = 1.0f; break; }
                                if (f >= h.first - 8 && f < h.first)
                                    holdEnv = std::max(holdEnv, SmoothStep((float)(f - (h.first - 8)) / 8.0f));
                                if (f > h.second && f <= h.second + 8)
                                    holdEnv = std::max(holdEnv, SmoothStep((float)((h.second + 8) - f) / 8.0f));
                            }
                            env = std::min(env, holdEnv);
                        }
                        if (env <= 0.0f) continue;
                        BoneKf out = applyLayer(name, f, env, head);
                        ctx.WriteBone(name, out, *tc);
                        if (ctx.capHit) { failed = true; break; }
                    }
                    if (failed) break;
                }
                if (failed) goto done;
                break;
            }

            // ---- morph -----------------------------------------------------
            case OpKind::Morph: {
                std::vector<int> frames = op.morphFrames;
                if (frames.empty())
                    for (int f = op.start; f <= op.end; f += op.every) frames.push_back(f);
                const auto it = m.motion.morphs.find(op.morphName);
                std::vector<MorphKf> baseTrack;
                if (it != m.motion.morphs.end()) baseTrack = it->second;
                std::map<int, float> weights;  // frame -> max weight (overlapping envelopes merge by max)
                for (int f : frames) {
                    const float baseValue = baseTrack.empty() ? 0.0f : SampleMorph(baseTrack, f);
                    const struct {
                        int frame;
                        float weight;
                    } keys[] = {{f - op.morphAttack, baseValue},
                                {f, op.peak},
                                {f + op.hold, op.peak},
                                {f + op.hold + op.morphRelease, baseValue}};
                    for (const auto& k : keys) {
                        if (k.frame < 0) continue;
                        auto wi = weights.find(k.frame);
                        if (wi == weights.end()) weights[k.frame] = k.weight;
                        else wi->second = std::max(wi->second, k.weight);
                    }
                }
                TrackChange* tc = FindChange(changes, true, op.morphName);
                for (const auto& [f, w] : weights) ctx.WriteMorph(op.morphName, f, w, *tc);
                if (ctx.capHit) goto done;
                break;
            }

            // ---- key_pose --------------------------------------------------
            case OpKind::KeyPose: {
                const int lo = std::max(0, op.keyFrame - op.ease);
                const int hi = op.keyFrame + op.ease;
                std::set<int> frameSet;
                for (int f = lo; f <= hi; f += op.step) frameSet.insert(f);
                frameSet.insert(op.keyFrame);
                frameSet.insert(lo);
                frameSet.insert(hi);
                std::vector<int> frames(frameSet.begin(), frameSet.end());
                bool failed = false;
                for (const KeyBoneEdit& e : op.keyBones) {
                    const auto it = m.motion.bones.find(e.name);
                    std::vector<BoneKf> base;
                    if (it != m.motion.bones.end()) base = it->second;
                    BoneKf target = base.empty() ? BoneKf{} : SampleBone(base, op.keyFrame);
                    target.frame = op.keyFrame;
                    if (e.hasRot) target.r = EulerToQuat(e.rotate[0], e.rotate[1], e.rotate[2]);
                    if (e.hasTranslate) target.t = {e.translate[0], e.translate[1], e.translate[2]};
                    TrackChange* tc = FindChange(changes, false, e.name);
                    for (int f : frames) {
                        const int d = std::abs(f - op.keyFrame);
                        const float u = op.ease > 0 ? 1.0f - (float)d / (float)op.ease : (f == op.keyFrame ? 1.0f : 0.0f);
                        const float blend = SmoothStep(u);
                        BoneKf baseKey = base.empty() ? BoneKf{} : SampleBone(base, f);
                        baseKey.frame = f;
                        BoneKf k = baseKey;
                        k.t = Lerp3(baseKey.t, target.t, blend);
                        k.r = QNormalize(QSlerp(baseKey.r, target.r, blend));
                        if (!ApplyBoneCurve(op.curve, k.interp, error)) { failed = true; break; }
                        ctx.WriteBone(e.name, k, *tc);
                        if (ctx.capHit) { failed = true; break; }
                    }
                    if (failed) break;
                }
                if (failed) goto done;
                break;
            }
        }

        {
            nlohmann::json r;
            r["op"] = OpName(op.kind);
            r["tracks"] = TracksJson(changes);
            r["keys_written"] = (int)(ctx.written - beforeCount);
            if (dryRun) r["track_changes"] = ChangesJson(changes);
            for (auto& [key, value] : extra.items()) r[key] = value;
            opResults.push_back(std::move(r));
        }
        // Later ops must sample the output of earlier ops, not a stale bound motion.
        if (ctx.written != beforeCount) m.motionVersion++;
    }

done:
    if (ctx.capHit || !error.empty()) {
        // Roll everything back: nothing was committed.
        m.motion.bones = bonesBefore;
        m.motion.morphs = morphsBefore;
        m.motionVersion++;
        if (ctx.capHit) promise->Reject("Too many keys written (max 100000 per call)");
        else promise->Reject(error.empty() ? "studio_motion_edit failed" : error);
        return true;
    }

    if (dryRun) {
        m.motion.bones = bonesBefore;
        m.motion.morphs = morphsBefore;
        m.motionVersion++;
        promise->Resolve({{"ops", opResults},
                          {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()},
                          {"dry_run", true}});
        return true;
    }

    if (ctx.written > 0) {
        m.motionVersion++;
        StudioPushTrackEdit("AI: studio_motion_edit", ctx.before);
    }
    promise->Resolve({{"ops", opResults},
                      {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()},
                      {"keys_written", (int)ctx.written}});
    return true;
}

}  // namespace mmdx
