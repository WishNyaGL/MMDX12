// The "lights" JSON schema (see SceneLightJson.h): project editor.lights and stage.json "lights" share it.
#include "asset/SceneLightJson.h"
#include <algorithm>

namespace mmdx::studio {

namespace {

// ---- guarded nlohmann reads: a wrong-typed key keeps the default, nothing throws (as StudioProject.cpp) ----
int ReadInt(const nlohmann::json& j, const char* key, int def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<int>();
}
double ReadDouble(const nlohmann::json& j, const char* key, double def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return def;
    return it->get<double>();
}
bool ReadBool(const nlohmann::json& j, const char* key, bool def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_boolean()) return def;
    return it->get<bool>();
}
std::string ReadString(const nlohmann::json& j, const char* key, const std::string& def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return def;
    return it->get<std::string>();
}
DirectX::XMFLOAT2 ReadVec2(const nlohmann::json& j, const char* key, DirectX::XMFLOAT2 def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 2) return def;
    for (int i = 0; i < 2; ++i)
        if (!(*it)[i].is_number()) return def;
    return {(float)(*it)[0].get<double>(), (float)(*it)[1].get<double>()};
}
DirectX::XMFLOAT3 ReadVec3(const nlohmann::json& j, const char* key, DirectX::XMFLOAT3 def) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return def;
    for (int i = 0; i < 3; ++i)
        if (!(*it)[i].is_number()) return def;
    return {(float)(*it)[0].get<double>(), (float)(*it)[1].get<double>(), (float)(*it)[2].get<double>()};
}
// The member `key` when it is an object, else nullptr.
const nlohmann::json* ObjectAt(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_object() ? &*it : nullptr;
}

nlohmann::json Vec3Json(const DirectX::XMFLOAT3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }

nlohmann::json LightValuesJson(const LightValues& v) {
    return {
        {"position", Vec3Json(v.position)}, {"aim", Vec3Json(v.aim)},     {"direction", Vec3Json(v.direction)},
        {"color", Vec3Json(v.color)},       {"intensity", v.intensity},   {"range", v.range},
        {"coneOuter", v.coneOuter},         {"coneInner", v.coneInner},   {"size", {v.size.x, v.size.y}},
    };
}

void ReadLightValues(const nlohmann::json& j, LightValues& v) {
    v.position = ReadVec3(j, "position", v.position);
    v.aim = ReadVec3(j, "aim", v.aim);
    v.direction = ReadVec3(j, "direction", v.direction);
    v.color = ReadVec3(j, "color", v.color);
    v.intensity = (float)ReadDouble(j, "intensity", v.intensity);
    v.range = (float)ReadDouble(j, "range", v.range);
    v.coneOuter = (float)ReadDouble(j, "coneOuter", v.coneOuter);
    v.coneInner = (float)ReadDouble(j, "coneInner", v.coneInner);
    v.size = ReadVec2(j, "size", v.size);
}

} // namespace

nlohmann::json SceneLightToJson(const SceneLight& l) {
    nlohmann::json j;
    j["uid"] = l.uid;
    j["kind"] = LightKindName(l.kind);
    j["name"] = l.name;
    j["enabled"] = l.enabled;
    j["values"] = LightValuesJson(l.v);
    j["sun"] = {{"vmdLink", l.vmdLink}, {"rimStrength", l.rimStrength}, {"rimColor", Vec3Json(l.rimColor)}};
    j["spot"] = {{"aim", AimModeName(l.aimMode)},
                 {"target", l.targetUid},
                 {"part", TargetPartName(l.targetPart)},
                 {"swayPhase", l.swayPhase}};
    j["sky"] = {{"zenith", Vec3Json(l.skyZenith)}, {"horizon", Vec3Json(l.skyHorizon)}, {"ground", Vec3Json(l.groundColor)}};
    j["shadow"] = {{"type", ShadowTypeName(l.shadow)},
                   {"softness", l.shadowSoftness},
                   {"density", l.shadowDensity},
                   {"color", Vec3Json(l.shadowColor)}};
    j["falloff"] = FalloffTypeName(l.falloff);
    j["affectDiffuse"] = l.affectDiffuse;
    j["affectSpecular"] = l.affectSpecular;
    j["viewportVisible"] = l.viewportVisible;
    // S3: studio only (a stage.json light has neither): the stage the light came from and its manifest name.
    if (l.stageUid != 0) j["stageUid"] = l.stageUid;
    if (!l.stageLightName.empty()) j["stageLight"] = l.stageLightName;
    nlohmann::json keys = nlohmann::json::array();
    for (const LightKey& k : l.keys) keys.push_back({{"frame", k.frame}, {"values", LightValuesJson(k.v)}});
    j["keys"] = std::move(keys);
    return j;
}

bool SceneLightFromJson(const nlohmann::json& jl, SceneLight& l, std::vector<std::string>* warnings) {
    const std::string kindStr = ReadString(jl, "kind", "");
    if (!ParseLightKind(kindStr, l.kind)) {
        if (warnings) warnings->push_back("unknown light kind: " + kindStr);
        return false;
    }
    l.uid = (uint32_t)std::max(0, ReadInt(jl, "uid", 0));
    l.name = ReadString(jl, "name", "");
    l.enabled = ReadBool(jl, "enabled", l.enabled);
    if (const nlohmann::json* v = ObjectAt(jl, "values")) ReadLightValues(*v, l.v);
    if (const nlohmann::json* s = ObjectAt(jl, "sun")) {
        l.vmdLink = ReadBool(*s, "vmdLink", l.vmdLink);
        l.rimStrength = (float)ReadDouble(*s, "rimStrength", l.rimStrength);
        l.rimColor = ReadVec3(*s, "rimColor", l.rimColor);
    }
    if (const nlohmann::json* s = ObjectAt(jl, "spot")) {
        ParseAimMode(ReadString(*s, "aim", ""), l.aimMode);
        l.targetUid = (uint32_t)std::max(0, ReadInt(*s, "target", 0));
        ParseTargetPart(ReadString(*s, "part", ""), l.targetPart);
        l.swayPhase = (float)ReadDouble(*s, "swayPhase", l.swayPhase);
    }
    if (const nlohmann::json* s = ObjectAt(jl, "sky")) {
        l.skyZenith = ReadVec3(*s, "zenith", l.skyZenith);
        l.skyHorizon = ReadVec3(*s, "horizon", l.skyHorizon);
        l.groundColor = ReadVec3(*s, "ground", l.groundColor);
    }
    if (const nlohmann::json* s = ObjectAt(jl, "shadow")) {
        ParseShadowType(ReadString(*s, "type", ""), l.shadow);
        l.shadowSoftness = (float)ReadDouble(*s, "softness", l.shadowSoftness);
        l.shadowDensity = (float)ReadDouble(*s, "density", l.shadowDensity);
        l.shadowColor = ReadVec3(*s, "color", l.shadowColor);
    }
    ParseFalloffType(ReadString(jl, "falloff", ""), l.falloff);
    l.affectDiffuse = ReadBool(jl, "affectDiffuse", l.affectDiffuse);
    l.affectSpecular = ReadBool(jl, "affectSpecular", l.affectSpecular);
    l.viewportVisible = ReadBool(jl, "viewportVisible", l.viewportVisible);
    l.stageUid = (uint32_t)std::max(0, ReadInt(jl, "stageUid", 0));
    l.stageLightName = ReadString(jl, "stageLight", "");
    const auto keysIt = jl.find("keys");
    if (keysIt != jl.end() && keysIt->is_array()) {
        for (const nlohmann::json& jk : *keysIt) {
            if (!jk.is_object()) continue;
            LightKey k;
            k.frame = std::max(0, ReadInt(jk, "frame", 0));
            k.v = l.v;
            if (const nlohmann::json* v = ObjectAt(jk, "values")) ReadLightValues(*v, k.v);
            // sorted by frame; a repeated frame keeps the last key
            const auto at = std::lower_bound(l.keys.begin(), l.keys.end(), k.frame,
                                            [](const LightKey& a, int f) { return a.frame < f; });
            if (at != l.keys.end() && at->frame == k.frame) *at = k;
            else l.keys.insert(at, k);
        }
    }
    return true;
}

} // namespace mmdx::studio
