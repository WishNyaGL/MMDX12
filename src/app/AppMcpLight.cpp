// AI lighting-director tools: build a lighting rig in one step (studio_light_rig) and turn the song's beat / section
// grid into keyed light animation (studio_light_cues). Every call that changes the document is one undo step.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio/BeatAnalysis.h"
#include "core/TextUtil.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"

namespace mmdx {

using namespace studio;

namespace {

constexpr int kMaxCueKeys = 50000;

const char* const kCenterName = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター
const char* const kHeadName = "\xE9\xA0\xAD";                                        // 頭
const char* const kFootLName = "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96";               // 左足首
const char* const kFootRName = "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96";               // 右足首

bool ReadVec3(const nlohmann::json& j, DirectX::XMFLOAT3& out) {
    if (!j.is_array() || j.size() != 3) return false;
    for (int i = 0; i < 3; ++i)
        if (!j[i].is_number()) return false;
    out = {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
    return true;
}

nlohmann::json Vec3Json(const DirectX::XMFLOAT3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

struct SubjectInfo {
    int index = -1;
    uint32_t uid = 0;
    DirectX::XMFLOAT3 centre{0, 10, 0};
    DirectX::XMFLOAT3 head{0, 16, 0};
    DirectX::XMFLOAT3 ground{0, 0, 0};
    float height = 10.0f;
};

// Reads the subject's centre / head / feet at the current frame (posed like the viewport, no physics).
using BoneSampler = std::function<std::vector<std::vector<DirectX::XMFLOAT3>>(StudioModel&, const std::vector<int>&)>;
bool SampleSubject(StudioDoc& d, int requested, const BoneSampler& sampleBones, SubjectInfo& out,
                   std::string& error) {
    int index = requested;
    if (index < 0) {
        for (size_t i = 0; i < d.models.size(); ++i) {
            const StudioModel& m = *d.models[i];
            if (m.kind == ModelKind::Character && m.inst && m.pmx) {
                index = (int)i;
                break;
            }
        }
    }
    if (index < 0 || index >= (int)d.models.size()) {
        error = "No character model to use as the light subject";
        return false;
    }
    StudioModel& m = *d.models[(size_t)index];
    if (m.kind != ModelKind::Character || !m.inst || !m.pmx) {
        error = "subject must be a loaded character model";
        return false;
    }
    const int centerBone = m.pmx->FindBone(kCenterName);
    const int headBone = m.pmx->FindBone(kHeadName);
    const int footL = m.pmx->FindBone(kFootLName);
    const int footR = m.pmx->FindBone(kFootRName);
    std::vector<int> bones;
    std::vector<int> slots(4, -1);
    const int wanted[4] = {centerBone, headBone, footL, footR};
    for (int k = 0; k < 4; ++k) {
        if (wanted[k] >= 0) {
            slots[k] = (int)bones.size();
            bones.push_back(wanted[k]);
        }
    }
    const int frame = d.Frame();
    const auto pos = sampleBones(m, {frame});
    out.index = index;
    out.uid = m.uid;
    if (pos.empty()) {
        out.centre = {0, 10, 0};
        out.head = {0, 20, 0};
        out.ground = {0, 0, 0};
        out.height = 20.0f;
        return true;
    }
    const auto& row = pos[0];
    auto at = [&](int k) { return slots[k] >= 0 ? row[(size_t)slots[k]] : DirectX::XMFLOAT3{0, 0, 0}; };
    const DirectX::XMFLOAT3 center = at(0);
    const DirectX::XMFLOAT3 head = at(2);
    DirectX::XMFLOAT3 ground = {center.x, head.y - 1.0f, center.z};
    bool anyFoot = false;
    for (int k = 2; k < 4; ++k) {
        if (slots[k] < 0) continue;
        const DirectX::XMFLOAT3 f = row[(size_t)slots[k]];
        if (!anyFoot) {
            ground = f;
            anyFoot = true;
        } else {
            ground.y = std::min(ground.y, f.y);
        }
    }
    float height = head.y - ground.y;
    if (!(height > 0.01f)) height = 10.0f;
    out.centre = slots[0] >= 0 ? center : DirectX::XMFLOAT3{ground.x, ground.y + height * 0.55f, ground.z};
    out.head = slots[1] >= 0 ? head : DirectX::XMFLOAT3{out.centre.x, ground.y + height * 0.95f, out.centre.z};
    out.ground = ground;
    out.height = height;
    return true;
}

DirectX::XMFLOAT3 ResolveTarget(const SubjectInfo& s, const std::string& name, bool& ok) {
    ok = true;
    if (name == "head") return s.head;
    if (name == "center" || name == "centre") return s.centre;
    if (name == "left") return {s.centre.x - s.height * 0.6f, s.centre.y, s.centre.z};
    if (name == "right") return {s.centre.x + s.height * 0.6f, s.centre.y, s.centre.z};
    if (name == "audience") return {s.centre.x, s.centre.y, s.centre.z - s.height * 3.0f};
    ok = false;
    return {};
}

const char* const kRigs[] = {"key_fill_rim", "front_wash", "top_spots", "side_bars", "back_beams"};

bool IsRig(const std::string& s) {
    for (const char* r : kRigs)
        if (s == r) return true;
    return false;
}

DirectX::XMFLOAT3 LightAimForOutput(const SceneLight& l, const LightValues& v) {
    if (l.kind == LightKind::Spot || l.kind == LightKind::Area) return v.aim;
    if (l.kind == LightKind::Sun) return v.direction;
    return v.aim;
}

bool ReadFrame(const nlohmann::json& j, int& frame, std::string& error) {
    if (!j.is_number_integer()) {
        error = "frame must be an integer";
        return false;
    }
    frame = j.get<int>();
    if (frame < 0) {
        error = "frame must be >= 0";
        return false;
    }
    return true;
}

int BeatFrames(const BeatAnalysis* a) {
    if (!a || a->bpm <= 1e-6) return 15;  // 120 bpm at 30 fps
    return std::max(1, (int)std::lround(30.0 * 60.0 / a->bpm));
}

}  // namespace

bool App::ExecuteMcpLight(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise) {
    if (!promise) return false;
    const bool rigTool = tool == "studio_light_rig";
    const bool cuesTool = tool == "studio_light_cues";
    if (!rigTool && !cuesTool) return false;
    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;
    const BoneSampler sampleBones = [this, &d](StudioModel& m, const std::vector<int>& bones) {
        return StudioSampleBones(m, {d.Frame()}, bones);
    };

    // -----------------------------------------------------------------------
    // studio_light_rig
    // -----------------------------------------------------------------------
    if (rigTool) {
        std::string error;
        const std::string rig = args.value("rig", std::string());
        if (!IsRig(rig)) {
            promise->Reject("rig must be key_fill_rim, front_wash, top_spots, side_bars or back_beams");
            return true;
        }
        SubjectInfo subject;
        const int subjectArg = args.value("subject", -1);
        if (!SampleSubject(d, subjectArg, sampleBones, subject, error)) {
            promise->Reject(error);
            return true;
        }
        const float h = subject.height;
        const float radius = args.value("radius", 1.6f);
        const float rigHeight = args.value("height", 1.6f);
        const std::string prefix = args.value("name_prefix", std::string("AI "));
        const bool replace = args.value("replace", false);

        DirectX::XMFLOAT3 color{1, 1, 1};
        if (args.contains("color") && !ReadVec3(args["color"], color)) {
            promise->Reject("color must be [r,g,b] (linear RGB)");
            return true;
        }
        std::vector<DirectX::XMFLOAT3> palette;
        if (args.contains("palette")) {
            if (!args["palette"].is_array()) {
                promise->Reject("palette must be an array of [r,g,b] colours");
                return true;
            }
            for (const auto& c : args["palette"]) {
                DirectX::XMFLOAT3 v;
                if (!ReadVec3(c, v)) {
                    promise->Reject("palette must be an array of [r,g,b] colours");
                    return true;
                }
                palette.push_back(v);
            }
        }
        ShadowType shadow = ShadowType::NoCast;
        bool explicitShadow = false;
        if (args.contains("shadow")) {
            if (!args["shadow"].is_string() ||
                !ParseShadowType(ToLowerAscii(args["shadow"].get<std::string>()), shadow)) {
                promise->Reject("shadow must be nocast, hard or soft");
                return true;
            }
            explicitShadow = true;
        }
        const ShadowType nonKeyShadow = explicitShadow ? shadow : ShadowType::NoCast;
        const ShadowType keyShadow = explicitShadow ? shadow : ShadowType::Hard;
        const float coneDeg = args.value("cone_deg", 25.0f);
        if (coneDeg <= 0.0f || coneDeg > 180.0f) {
            promise->Reject("cone_deg must be in (0, 180]");
            return true;
        }
        const DirectX::XMFLOAT2 defaultSize{h * 0.8f, h * 0.8f};

        std::vector<SceneLight> before = d.lights;
        if (replace) {
            std::erase_if(d.lights, [&](const SceneLight& l) {
                return (l.kind == LightKind::Point || l.kind == LightKind::Spot || l.kind == LightKind::Area) &&
                       l.name.rfind(prefix, 0) == 0;
            });
        }

        std::vector<int> created;
        auto add = [&](LightKind kind, const DirectX::XMFLOAT3& position, const DirectX::XMFLOAT3& aim,
                       float intensity, float coneOuterDeg, const DirectX::XMFLOAT2& size, AimMode aimMode,
                       float swayPhase, ShadowType castShadow, const DirectX::XMFLOAT3& lightColor) {
            SceneLight l;
            l.uid = d.nextLightUid++;
            l.kind = kind;
            l.enabled = true;
            l.name = prefix + rig + " " + std::to_string(created.size() + 1);
            l.v.position = position;
            l.v.aim = aim;
            l.v.color = lightColor;
            l.v.intensity = intensity;
            l.v.range = std::max(1.0f, h * 4.0f);
            if (kind == LightKind::Spot) {
                l.v.coneOuter = DirectX::XMConvertToRadians(coneOuterDeg);
                l.v.coneInner = l.v.coneOuter * 0.7f;
            }
            if (kind == LightKind::Area) l.v.size = size;
            l.aimMode = aimMode;
            l.targetUid = (aimMode == AimMode::Target) ? subject.uid : 0u;
            l.targetPart = TargetPart::Centre;
            l.swayPhase = swayPhase;
            l.shadow = castShadow;
            d.lights.push_back(std::move(l));
            created.push_back((int)d.lights.size() - 1);
        };

        const auto chooseColor = [&](size_t i) {
            if (!palette.empty()) return palette[i % palette.size()];
            return color;
        };
        const DirectX::XMFLOAT3 focus = subject.centre;
        const float groundY = subject.ground.y;
        const float y = groundY + rigHeight * h;
        const float r = radius * h;
        const float defaultIntensity = args.value("intensity", rig == "key_fill_rim" ? 3.0f : 1.5f);

        if (rig == "key_fill_rim") {
            const float side = DirectX::XMConvertToRadians(35.0f);
            const float keyx = std::sin(side) * r;
            const float keyz = -std::cos(side) * r;
            add(LightKind::Spot, {focus.x + keyx, y, focus.z + keyz}, focus, defaultIntensity, coneDeg,
                defaultSize, AimMode::Target, 0.0f, keyShadow, chooseColor(0));
            add(LightKind::Area, {focus.x - keyx, groundY + rigHeight * 0.75f * h, focus.z + keyz * 0.9f}, focus,
                defaultIntensity * 0.4f, coneDeg, defaultSize, AimMode::Target, 0.0f, nonKeyShadow, chooseColor(1));
            add(LightKind::Spot, {focus.x, groundY + (rigHeight + 0.3f) * h, focus.z + r * 1.1f}, focus,
                defaultIntensity * 0.8f, coneDeg, defaultSize, AimMode::Target, 0.0f, nonKeyShadow, chooseColor(2));
        } else if (rig == "front_wash") {
            int count = args.value("count", 3);
            if (count < 1 || count > 64) {
                d.lights = before;  // a rejected call changes nothing
                promise->Reject("count must be in 1..64");
                return true;
            }
            for (int i = 0; i < count; ++i) {
                const float t = count > 1 ? ((float)i / (float)(count - 1)) * 2.0f - 1.0f : 0.0f;
                const float a = t * DirectX::XMConvertToRadians(35.0f);
                const DirectX::XMFLOAT3 pos{focus.x + std::sin(a) * r, y, focus.z - std::cos(a) * r};
                add(LightKind::Area, pos, focus, defaultIntensity, coneDeg, defaultSize, AimMode::Target, 0.0f,
                    nonKeyShadow, chooseColor((size_t)i));
            }
        } else if (rig == "top_spots") {
            int count = args.value("count", 6);
            if (count < 1 || count > 64) {
                d.lights = before;  // a rejected call changes nothing
                promise->Reject("count must be in 1..64");
                return true;
            }
            const std::string aim = args.value("aim", std::string("subject"));
            if (aim != "subject" && aim != "down") {
                d.lights = before;
                promise->Reject("aim must be subject or down");
                return true;
            }
            for (int i = 0; i < count; ++i) {
                const float t = count > 1 ? ((float)i / (float)(count - 1)) * 2.0f - 1.0f : 0.0f;
                const DirectX::XMFLOAT3 pos{focus.x + t * r, groundY + (rigHeight + 1.2f) * h,
                                            focus.z - 0.4f * h};
                if (aim == "down") {
                    add(LightKind::Spot, pos, {pos.x, groundY, pos.z}, defaultIntensity, coneDeg, defaultSize,
                        AimMode::Manual, 0.0f, nonKeyShadow, chooseColor((size_t)i));
                } else {
                    add(LightKind::Spot, pos, focus, defaultIntensity, coneDeg, defaultSize, AimMode::Target, 0.0f,
                        nonKeyShadow, chooseColor((size_t)i));
                }
            }
        } else if (rig == "side_bars") {
            int count = args.value("count", 4);
            if (count < 1 || count > 64) {
                d.lights = before;  // a rejected call changes nothing
                promise->Reject("count must be in 1..64");
                return true;
            }
            for (int side = 0; side < 2; ++side) {
                for (int i = 0; i < count; ++i) {
                    const float t = count > 1 ? (float)i / (float)(count - 1) : 0.5f;
                    const float px = (side == 0 ? -1.0f : 1.0f) * r * 1.2f;
                    const DirectX::XMFLOAT3 pos{focus.x + px, groundY + (0.2f + t * 2.0f) * h, focus.z};
                    add(LightKind::Point, pos, focus, defaultIntensity, coneDeg, defaultSize, AimMode::Manual, 0.0f,
                        nonKeyShadow, chooseColor((size_t)(side * count + i)));
                }
            }
        } else {  // back_beams
            int count = args.value("count", 4);
            if (count < 1 || count > 64) {
                d.lights = before;  // a rejected call changes nothing
                promise->Reject("count must be in 1..64");
                return true;
            }
            for (int i = 0; i < count; ++i) {
                const float t = count > 1 ? ((float)i / (float)(count - 1)) * 2.0f - 1.0f : 0.0f;
                const DirectX::XMFLOAT3 pos{focus.x + t * r, groundY + 0.3f * h, focus.z + r * 1.25f};
                const DirectX::XMFLOAT3 aim{focus.x + t * r * 0.5f, groundY + 1.6f * h, focus.z - h};
                add(LightKind::Spot, pos, aim, defaultIntensity, coneDeg, defaultSize, AimMode::Sway,
                    (float)(2.0 * 3.14159265358979323846 * (double)i / (double)count),
                    nonKeyShadow, chooseColor((size_t)i));
            }
        }

        std::vector<SceneLight> after = d.lights;
        if (before != after) {
            d.history.Push(std::make_unique<LightsCommand>(d, "AI: studio_light_rig", before, std::move(after)));
            ++d.projectVersion;
            d.rowsKey = ~0ull;
        }
        nlohmann::json lights = nlohmann::json::array();
        for (int idx : created) {
            const SceneLight& l = d.lights[(size_t)idx];
            lights.push_back({{"index", idx},
                              {"uid", l.uid},
                              {"name", l.name},
                              {"kind", LightKindName(l.kind)},
                              {"position", Vec3Json(l.v.position)},
                              {"aim", Vec3Json(LightAimForOutput(l, l.v))}});
        }
        promise->Resolve({{"lights", std::move(lights)},
                          {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()}});
        return true;
    }

    // -----------------------------------------------------------------------
    // studio_light_cues
    // -----------------------------------------------------------------------
    if (!args.contains("cues") || !args["cues"].is_array() || args["cues"].empty()) {
        promise->Reject("cues must be a non-empty array");
        return true;
    }

    const BeatAnalysis* analysis = nullptr;
    std::string beatsError;
    bool triedBeats = false;
    const auto beats = [&]() -> const BeatAnalysis* {
        if (!triedBeats) {
            analysis = StudioBeats(beatsError);
            triedBeats = true;
        }
        return analysis;
    };

    const std::vector<SceneLight> original = d.lights;
    const double audioOffset = d.audioOffset;
    const auto beatLength = [&]() { return BeatFrames(beats()); };

    struct PendingKey {
        int light = -1;
        int frame = 0;
        LightValues values;
    };
    struct CuePlan {
        std::string pattern;
        std::vector<int> lights;
        int events = 0;
        int start = 0;
        int end = 0;
        std::vector<PendingKey> keys;
        std::vector<int> eraseLights;
        std::vector<int> manualAimLights;
    };
    std::vector<CuePlan> plans;

    auto sample = [&](int light, int frame) { return SampleLightValues(original[(size_t)light], frame); };

    std::function<bool(const nlohmann::json&, std::vector<int>&, std::string&)> resolveLights =
        [&](const nlohmann::json& j, std::vector<int>& out, std::string& error) -> bool {
        out.clear();
        if (j.is_string()) {
            const std::string kind = ToLowerAscii(j.get<std::string>());
            for (int i = 0; i < (int)d.lights.size(); ++i) {
                const LightKind k = d.lights[(size_t)i].kind;
                if (kind == "spots" && k == LightKind::Spot) out.push_back(i);
                else if (kind == "points" && k == LightKind::Point) out.push_back(i);
                else if (kind == "areas" && k == LightKind::Area) out.push_back(i);
                else if (kind == "all" && k != LightKind::Sun && k != LightKind::Ambient) out.push_back(i);
            }
            if (kind != "spots" && kind != "points" && kind != "areas" && kind != "all") {
                error = "lights selector must be an array of indices or spots / points / areas / all";
                return false;
            }
            return true;
        }
        if (!j.is_array()) {
            error = "lights must be an array of indices or spots / points / areas / all";
            return false;
        }
        for (const auto& e : j) {
            if (e.is_number_integer()) {
                const int idx = e.get<int>();
                if (idx < 0 || idx >= (int)d.lights.size()) {
                    error = "Light index out of range: " + std::to_string(idx);
                    return false;
                }
                if (d.lights[(size_t)idx].kind == LightKind::Sun || d.lights[(size_t)idx].kind == LightKind::Ambient) {
                    error = "The sun and ambient light cannot be keyed by cues";
                    return false;
                }
                out.push_back(idx);
            } else if (e.is_string()) {
                std::vector<int> sub;
                if (!resolveLights(e, sub, error)) return false;
                out.insert(out.end(), sub.begin(), sub.end());
            } else {
                error = "lights must be an array of indices or spots / points / areas / all";
                return false;
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return true;
    };

    for (const auto& cj : args["cues"]) {
        if (!cj.is_object()) {
            promise->Reject("every cue must be an object");
            return true;
        }
        CuePlan plan;
        std::string error;
        plan.pattern = cj.value("pattern", std::string());
        static const char* const kPatterns[] = {"pulse",      "chase",     "alternate",   "strobe", "sweep",
                                                "color_cycle", "breathe", "fade",        "section_levels",
                                                "blackout"};
        bool known = false;
        for (const char* p : kPatterns)
            if (plan.pattern == p) known = true;
        if (!known) {
            promise->Reject("unknown cue pattern: " + plan.pattern);
            return true;
        }
        if (!cj.contains("lights") || !resolveLights(cj["lights"], plan.lights, error)) {
            promise->Reject(error.empty() ? "cues need lights" : error);
            return true;
        }
        if (plan.lights.empty()) {
            promise->Reject("cue '" + plan.pattern + "' selects no lights");
            return true;
        }
        int start = 0, end = 0;
        if (!cj.contains("start") || !cj.contains("end") || !ReadFrame(cj["start"], start, error) ||
            !ReadFrame(cj["end"], end, error)) {
            promise->Reject("every cue needs integer start and end frames");
            return true;
        }
        if (end < start) {
            promise->Reject("cue end must be >= start");
            return true;
        }
        plan.start = start;
        plan.end = end;
        const std::string timing = ToLowerAscii(cj.value("timing", std::string("beat")));
        const int every = std::max(1, cj.value("every", 1));
        const int offset = cj.value("offset", 0);
        const bool needsEvents = plan.pattern != "breathe" && plan.pattern != "fade" &&
                                 plan.pattern != "section_levels" && plan.pattern != "blackout";

        // Beat-grid events for this cue.
        std::vector<int> events;
        if (needsEvents) {
            if (timing == "section") {
                const BeatAnalysis* a = beats();
                if (!a) {
                    promise->Reject("timing section needs audio analysis: " + beatsError);
                    return true;
                }
                for (const BeatSection& s : a->sections) {
                    const int f = (int)std::lround((s.start + audioOffset) * 30.0);
                    if (f >= start && f <= end) events.push_back(f);
                }
            } else {
                if (timing != "beat" && timing != "half" && timing != "downbeat" && timing != "bar" &&
                    timing != "onset") {
                    promise->Reject("timing must be beat, half, downbeat, bar, onset or section");
                    return true;
                }
                const BeatAnalysis* a = beats();
                if (!a) {
                    promise->Reject("timing " + timing + " needs audio analysis: " + beatsError);
                    return true;
                }
                if (timing == "onset") {
                    const float minStrength = cj.value("min_strength", 0.5f);
                    for (size_t i = 0; i < a->onsets.size(); ++i) {
                        if (i < a->onsetStrength.size() && a->onsetStrength[i] < minStrength) continue;
                        const int f = (int)std::lround((a->onsets[i] + audioOffset) * 30.0);
                        if (f >= start && f <= end) events.push_back(f);
                    }
                } else if (timing == "downbeat" || timing == "bar") {
                    for (double t : a->downbeats) {
                        const int f = (int)std::lround((t + audioOffset) * 30.0);
                        if (f >= start && f <= end) events.push_back(f);
                    }
                } else {
                    for (size_t i = 0; i < a->beats.size(); ++i) {
                        const int f = (int)std::lround((a->beats[i] + audioOffset) * 30.0);
                        if (f >= start && f <= end) events.push_back(f);
                        if (timing == "half" && i + 1 < a->beats.size()) {
                            const int mid = (int)std::lround(((a->beats[i] + a->beats[i + 1]) * 0.5 + audioOffset) * 30.0);
                            if (mid >= start && mid <= end) events.push_back(mid);
                        }
                    }
                }
                std::sort(events.begin(), events.end());
                events.erase(std::unique(events.begin(), events.end()), events.end());
                std::vector<int> selected;
                for (size_t i = 0; i < events.size(); ++i) {
                    const long long k = (long long)i - (long long)offset;
                    if (k >= 0 && k % every == 0) selected.push_back(events[i]);
                }
                events.swap(selected);
            }
            std::sort(events.begin(), events.end());
            events.erase(std::unique(events.begin(), events.end()), events.end());
            if (events.empty()) {
                promise->Reject("cue '" + plan.pattern + "' with timing '" + timing + "' found no events in [" +
                                std::to_string(start) + ", " + std::to_string(end) + "]");
                return true;
            }
        }
        plan.events = (int)events.size();

        const bool replaceRange = cj.value("mode", std::string("replace_range")) == "replace_range";
        if (!replaceRange && cj.value("mode", std::string("replace_range")) != "merge") {
            promise->Reject("cue mode must be replace_range or merge");
            return true;
        }
        if (replaceRange) plan.eraseLights = plan.lights;

        // Per-light intensity defaults (current base intensity x0.3 / x1.0).
        const auto basePeak = [&](int light, float& base, float& peak) {
            const float cur = original[(size_t)light].v.intensity;
            base = cj.value("base", cur * 0.3f);
            peak = cj.value("peak", cur * 1.0f);
        };

        // a cue keys only inside its own range (envelope tails past the ends are dropped: the range ends are keyed by
        // the patterns), so neighbouring cues never overwrite each other and a pre-event key never lands on the event
        auto pushKey = [&](int light, int frame, LightValues v) {
            if (frame < start || frame > end) return;
            plan.keys.push_back({light, frame, v});
        };

        if (plan.pattern == "pulse") {
            const int decay = cj.value("decay", (int)std::lround(beatLength() * 0.6));
            for (int light : plan.lights) {
                float base = 0, peak = 0;
                basePeak(light, base, peak);
                LightValues v = sample(light, start);
                v.intensity = base;
                pushKey(light, start, v);
                v = sample(light, end);
                v.intensity = base;
                pushKey(light, end, v);
                for (int e : events) {
                    v = sample(light, e);
                    v.intensity = peak;
                    pushKey(light, e, v);
                    v = sample(light, e + decay);
                    v.intensity = base;
                    pushKey(light, e + decay, v);
                }
            }
        } else if (plan.pattern == "chase") {
            const int decay = cj.value("decay", (int)std::lround(beatLength() * 0.6));
            for (size_t i = 0; i < events.size(); ++i) {
                const int active = plan.lights[i % plan.lights.size()];
                for (int light : plan.lights) {
                    float base = 0, peak = 0;
                    basePeak(light, base, peak);
                    if (light == active) {
                        LightValues v = sample(light, events[i]);
                        v.intensity = peak;
                        pushKey(light, events[i], v);
                        v = sample(light, events[i] + decay);
                        v.intensity = base;
                        pushKey(light, events[i] + decay, v);
                    } else {
                        LightValues v = sample(light, events[i]);
                        v.intensity = base;
                        pushKey(light, events[i], v);
                    }
                }
            }
        } else if (plan.pattern == "alternate") {
            for (size_t i = 0; i < events.size(); ++i) {
                for (size_t j = 0; j < plan.lights.size(); ++j) {
                    const int light = plan.lights[j];
                    float base = 0, peak = 0;
                    basePeak(light, base, peak);
                    LightValues v = sample(light, events[i]);
                    v.intensity = (j % 2 == i % 2) ? peak : base;
                    pushKey(light, events[i], v);
                }
            }
        } else if (plan.pattern == "strobe") {
            std::map<int, int> perBeat;
            for (int e : events) {
                int& count = perBeat[e / std::max(1, beatLength())];
                if (count >= 4) continue;
                ++count;
                for (int light : plan.lights) {
                    float base = 0, peak = 0;
                    basePeak(light, base, peak);
                    LightValues v = sample(light, std::max(0, e - 1));
                    v.intensity = base;
                    pushKey(light, e - 1, v);
                    v = sample(light, e);
                    v.intensity = peak;
                    pushKey(light, e, v);
                    v = sample(light, e + 2);
                    v.intensity = base;
                    pushKey(light, e + 2, v);
                }
            }
        } else if (plan.pattern == "sweep") {
            std::vector<DirectX::XMFLOAT3> points;
            if (cj.contains("aim_points")) {
                if (!cj["aim_points"].is_array()) {
                    promise->Reject("aim_points must be an array of [x,y,z]");
                    return true;
                }
                for (const auto& p : cj["aim_points"]) {
                    DirectX::XMFLOAT3 v;
                    if (!ReadVec3(p, v)) {
                        promise->Reject("aim_points must be an array of [x,y,z]");
                        return true;
                    }
                    points.push_back(v);
                }
            }
            if (cj.contains("aim_targets")) {
                if (!cj["aim_targets"].is_array()) {
                    promise->Reject("aim_targets must be an array of head, center, left, right, audience");
                    return true;
                }
                SubjectInfo subject;
                std::string err;
                if (!SampleSubject(d, -1, sampleBones, subject, err)) {
                    promise->Reject(err);
                    return true;
                }
                for (const auto& t : cj["aim_targets"]) {
                    if (!t.is_string()) {
                        promise->Reject("aim_targets must be an array of head, center, left, right, audience");
                        return true;
                    }
                    bool ok = false;
                    const DirectX::XMFLOAT3 p = ResolveTarget(subject, ToLowerAscii(t.get<std::string>()), ok);
                    if (!ok) {
                        promise->Reject("unknown aim target: " + t.get<std::string>());
                        return true;
                    }
                    points.push_back(p);
                }
            }
            if (points.empty()) {
                promise->Reject("sweep needs aim_points or aim_targets");
                return true;
            }
            for (int light : plan.lights) {
                if (original[(size_t)light].kind != LightKind::Spot && original[(size_t)light].kind != LightKind::Area)
                    continue;
                plan.manualAimLights.push_back(light);
                for (size_t i = 0; i < events.size(); ++i) {
                    LightValues v = sample(light, events[i]);
                    v.aim = points[i % points.size()];
                    pushKey(light, events[i], v);
                }
            }
        } else if (plan.pattern == "color_cycle") {
            if (!cj.contains("colors") || !cj["colors"].is_array() || cj["colors"].empty()) {
                promise->Reject("color_cycle needs a non-empty colors array");
                return true;
            }
            std::vector<DirectX::XMFLOAT3> colors;
            for (const auto& c : cj["colors"]) {
                DirectX::XMFLOAT3 v;
                if (!ReadVec3(c, v)) {
                    promise->Reject("colors must be an array of [r,g,b]");
                    return true;
                }
                colors.push_back(v);
            }
            for (size_t i = 0; i < events.size(); ++i) {
                const DirectX::XMFLOAT3& next = colors[i % colors.size()];
                const DirectX::XMFLOAT3& prev = colors[(i + colors.size() - 1) % colors.size()];
                for (int light : plan.lights) {
                    LightValues v = sample(light, events[i]);
                    v.color = next;
                    pushKey(light, events[i], v);
                    v = sample(light, events[i] - 1);
                    v.color = prev;
                    pushKey(light, events[i] - 1, v);
                }
            }
        } else if (plan.pattern == "breathe") {
            const int period = std::max(4, every * 4 * beatLength());
            const int step = std::max(1, period / 4);
            for (int light : plan.lights) {
                float base = 0, peak = 0;
                basePeak(light, base, peak);
                for (int f = start; f <= end; f += step) {
                    const double phase = 2.0 * 3.14159265358979323846 * (double)(f - start) / (double)period;
                    LightValues v = sample(light, f);
                    v.intensity = base + (peak - base) * (float)(0.5 - 0.5 * std::cos(phase));
                    pushKey(light, f, v);
                }
            }
            plan.events = (int)plan.keys.size();
        } else if (plan.pattern == "fade") {
            for (int light : plan.lights) {
                float base = 0, peak = 0;
                basePeak(light, base, peak);
                LightValues v = sample(light, start);
                v.intensity = base;
                if (cj.contains("from")) {
                    DirectX::XMFLOAT3 c;
                    if (!ReadVec3(cj["from"], c)) {
                        promise->Reject("from must be [r,g,b]");
                        return true;
                    }
                    v.color = c;
                }
                pushKey(light, start, v);
                v = sample(light, end);
                v.intensity = peak;
                if (cj.contains("to")) {
                    DirectX::XMFLOAT3 c;
                    if (!ReadVec3(cj["to"], c)) {
                        promise->Reject("to must be [r,g,b]");
                        return true;
                    }
                    v.color = c;
                }
                pushKey(light, end, v);
            }
            plan.events = 2;
        } else if (plan.pattern == "section_levels") {
            const BeatAnalysis* a = beats();
            if (!a) {
                promise->Reject("section_levels needs audio analysis: " + beatsError);
                return true;
            }
            struct LevelRange {
                int start = 0, end = 0;
                std::string level;
            };
            std::vector<LevelRange> ranges;
            for (const BeatSection& s : a->sections) {
                int sf = (int)std::lround((s.start + audioOffset) * 30.0);
                int ef = (int)std::lround((s.end + audioOffset) * 30.0);
                if (ef < start || sf > end) continue;
                ranges.push_back({std::max(sf, start), std::min(ef, end), s.level});
            }
            if (ranges.empty()) {
                promise->Reject("section_levels found no sections in [" + std::to_string(start) + ", " +
                                std::to_string(end) + "]");
                return true;
            }
            plan.events = (int)ranges.size();
            for (int light : plan.lights) {
                float base = 0, peak = 0;
                basePeak(light, base, peak);
                const float mid = (base + peak) * 0.5f;
                for (size_t i = 0; i < ranges.size(); ++i) {
                    const LevelRange& rg = ranges[i];
                    float level = mid;
                    if (rg.level == "low") level = base;
                    else if (rg.level == "high") level = peak;
                    if (i > 0) {
                        float prevLevel = mid;
                        const std::string& pl = ranges[i - 1].level;
                        if (pl == "low") prevLevel = base;
                        else if (pl == "high") prevLevel = peak;
                        LightValues v = sample(light, rg.start - 1);
                        v.intensity = prevLevel;
                        pushKey(light, rg.start - 1, v);
                    }
                    LightValues v = sample(light, rg.start);
                    v.intensity = level;
                    pushKey(light, rg.start, v);
                    v = sample(light, rg.end);
                    v.intensity = level;
                    pushKey(light, rg.end, v);
                }
            }
        } else {  // blackout
            for (int light : plan.lights) {
                LightValues v = sample(light, start);
                v.intensity = 0.0f;
                pushKey(light, start, v);
                v = sample(light, end);
                v.intensity = 0.0f;
                pushKey(light, end, v);
            }
            plan.events = 2;
        }

        plans.push_back(std::move(plan));
    }

    size_t totalKeys = 0;
    for (const CuePlan& p : plans) totalKeys += p.keys.size();
    if (totalKeys > (size_t)kMaxCueKeys) {
        promise->Reject("Too many keys: " + std::to_string(totalKeys) + " (max " + std::to_string(kMaxCueKeys) + ")");
        return true;
    }

    // Apply: erase ranges first, then write every planned key (sampled from the pre-call lights).
    std::vector<SceneLight> after = d.lights;
    for (const CuePlan& p : plans) {
        for (int light : p.eraseLights) {
            std::vector<LightKey>& keys = after[(size_t)light].keys;
            keys.erase(std::remove_if(keys.begin(), keys.end(),
                                      [&](const LightKey& k) { return k.frame >= p.start && k.frame <= p.end; }),
                       keys.end());
        }
        for (int light : p.manualAimLights) after[(size_t)light].aimMode = AimMode::Manual;
        for (const PendingKey& k : p.keys) {
            if (k.light < 0 || k.light >= (int)after.size()) continue;
            UpsertKey(after[(size_t)k.light].keys, LightKey{k.frame, k.values});
        }
    }

    if (original != after) {
        d.history.Push(std::make_unique<LightsCommand>(d, "AI: studio_light_cues", original, std::move(after)));
        ++d.projectVersion;
        d.rowsKey = ~0ull;
    }

    nlohmann::json cueJson = nlohmann::json::array();
    for (const CuePlan& p : plans) {
        cueJson.push_back({{"pattern", p.pattern},
                           {"lights", p.lights},
                           {"events", p.events},
                           {"keys_written", p.keys.size()}});
    }
    const BeatAnalysis* a = beats();
    promise->Resolve({{"cues", std::move(cueJson)},
                      {"undo", d.history.CanUndo() ? d.history.UndoName() : std::string()},
                      {"bpm", a ? a->bpm : 0.0}});
    return true;
}

}  // namespace mmdx
