// AI camera-direction MCP tools: a framing solver (studio_camera_solve) and a shot / sequence generator
// (studio_camera_shots). The solver poses the subject at a frame, fits the requested shot size from the projected
// body span and returns a studio_set_camera_keys-compatible camera. The generator writes motion-camera keys (with
// curves, follow re-solving and beat accents) in one undo step.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/TextUtil.h"
#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"

namespace mmdx {

using namespace studio;

namespace {

const char* const kHeadBone = "\xE9\xA0\xAD";                                  // 頭
const char* const kCenterBone = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC";  // センター
const char* const kUpperBone = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB";         // 上半身
const char* const kLeftHandBone = "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96";      // 左手首
const char* const kRightHandBone = "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96";     // 右手首
const char* const kLeftFootBone = "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96";      // 左足首
const char* const kRightFootBone = "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96";     // 右足首

const nlohmann::json* FindMember(const nlohmann::json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

float NumberOr(const nlohmann::json& j, const char* key, float fallback) {
    const nlohmann::json* v = FindMember(j, key);
    return v && v->is_number() ? v->get<float>() : fallback;
}

bool BoolOr(const nlohmann::json& j, const char* key, bool fallback) {
    const nlohmann::json* v = FindMember(j, key);
    if (!v) return fallback;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_number_integer()) return v->get<int>() != 0;
    return fallback;
}

nlohmann::json Vec3Json(const DirectX::XMFLOAT3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

float ShotFraction(const std::string& shot, bool& ok) {
    ok = true;
    if (shot == "ecu") return 0.10f;
    if (shot == "cu") return 0.22f;
    if (shot == "mcu") return 0.32f;
    if (shot == "ms") return 0.50f;
    if (shot == "cowboy") return 0.70f;
    if (shot == "full") return 1.15f;
    if (shot == "wide") return 2.20f;
    ok = false;
    return 0.0f;
}

// Multi-model fitting uses the union; only full / wide add margin (the others fit the union exactly).
float ShotMargin(const std::string& shot) {
    if (shot == "full") return 1.15f;
    if (shot == "wide") return 2.20f;
    return 1.0f;
}

std::string NeighbourShot(const std::string& shot, int dir) {
    static const char* const kOrder[] = {"ecu", "cu", "mcu", "ms", "cowboy", "full", "wide"};
    int idx = -1;
    for (int i = 0; i < 7; ++i)
        if (shot == kOrder[i]) idx = i;
    if (idx < 0) return shot;
    idx = std::clamp(idx + dir, 0, 6);
    return kOrder[idx];
}

bool IsKnownMove(const std::string& m) {
    static const std::set<std::string> kMoves = {
        "static", "dolly_in", "dolly_out", "zoom_in", "zoom_out", "dolly_zoom", "crane_up", "crane_down",
        "truck_left", "truck_right", "arc_left", "arc_right", "follow", "push_accent"};
    return kMoves.count(m) != 0;
}

DirectX::XMFLOAT3 Add(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
DirectX::XMFLOAT3 Sub(const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
DirectX::XMFLOAT3 Scale(const DirectX::XMFLOAT3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

DirectX::XMFLOAT3 ToVec3(DirectX::FXMVECTOR v) {
    DirectX::XMFLOAT3 out;
    DirectX::XMStoreFloat3(&out, v);
    return out;
}

float Length(const DirectX::XMFLOAT3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

DirectX::XMFLOAT3 Normalize(const DirectX::XMFLOAT3& v) {
    const float l = Length(v);
    return l > 1e-8f ? Scale(v, 1.0f / l) : DirectX::XMFLOAT3{0, 0, 1};
}

nlohmann::json CameraKeyJson(const CameraPose& pose, const nlohmann::json& curve) {
    DirectX::XMFLOAT3 eye;
    CameraMotion::ToView(pose, nullptr, &eye);
    nlohmann::json k;
    k["target"] = Vec3Json(pose.target);
    k["yaw_deg"] = DirectX::XMConvertToDegrees(pose.rotation.y);
    k["pitch_deg"] = -DirectX::XMConvertToDegrees(pose.rotation.x);
    k["roll_deg"] = DirectX::XMConvertToDegrees(pose.rotation.z);
    k["dist"] = -pose.distance;
    k["fov_deg"] = pose.fovDeg;
    k["eye"] = Vec3Json(eye);
    if (!curve.is_null()) k["curve"] = curve;
    return k;
}

}  // namespace

bool App::ExecuteMcpCamera(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise) {
    const bool solveTool = tool == "studio_camera_solve";
    const bool shotsTool = tool == "studio_camera_shots";
    if (!solveTool && !shotsTool) return false;
    if (!promise) return false;
    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;

    // ---- the framing solver (shared by both tools) -------------------------
    // Poses the subject models at `frame`, decides what has to be in frame from the shot / fill, places the anchor at
    // the requested screen point and iterates the distance until the projected body span matches the request.
    // Yaw sign: camera yaw 0 = in front of the subject's face; +90 = at the subject's left (+X when facing -Z).
    auto solve = [&](int frame, const nlohmann::json& subject, const std::string& shotName, float fillValue,
                     float fov, const nlohmann::json& angle, const nlohmann::json& screenArg,
                     const std::string& focus, CameraPose& pose, nlohmann::json& camOut,
                     DirectX::XMFLOAT3* outAnchor, std::string& err, float* outHeight = nullptr) -> bool {
        // subject models: explicit list, else the first visible character
        std::vector<int> models;
        const nlohmann::json* modelsJson = FindMember(subject, "models");
        if (modelsJson && modelsJson->is_array()) {
            for (const nlohmann::json& mj : *modelsJson) {
                if (!mj.is_number_integer()) { err = "subject.models must be integers"; return false; }
                models.push_back(mj.get<int>());
            }
        } else {
            for (size_t i = 0; i < d.models.size(); ++i) {
                const StudioModel& m = *d.models[i];
                if (m.kind == ModelKind::Character && m.visible && m.inst && m.pmx) {
                    models.push_back((int)i);
                    break;
                }
            }
        }
        if (models.empty()) { err = "No visible character to frame"; return false; }

        struct Subject {
            int index = -1;
            float height = 1.0f;
            float facing = 0.0f;
            DirectX::XMFLOAT3 head{}, headTop{}, center{}, upper{}, feet{};
            std::vector<DirectX::XMFLOAT3> points;
        };
        std::vector<Subject> subjects;
        for (int idx : models) {
            if (idx < 0 || idx >= (int)d.models.size()) {
                err = "Model index out of range: " + std::to_string(idx);
                return false;
            }
            StudioModel& m = *d.models[(size_t)idx];
            if (m.kind != ModelKind::Character || !m.inst || !m.pmx) {
                err = "subject models must be characters with a loaded mesh";
                return false;
            }
            const std::vector<std::string> names = {kHeadBone, kCenterBone, kUpperBone, kLeftHandBone,
                                                    kRightHandBone, kLeftFootBone, kRightFootBone};
            std::vector<int> boneIdx;
            std::vector<int> slot(names.size(), -1);
            for (size_t k = 0; k < names.size(); ++k) {
                const int b = m.pmx->FindBone(names[k]);
                if (b >= 0) {
                    slot[k] = (int)boneIdx.size();
                    boneIdx.push_back(b);
                }
            }
            std::vector<float> yaw;
            const auto pos = StudioSampleBones(m, {frame}, boneIdx, &yaw);
            if (pos.empty()) { err = "Could not sample the subject at frame " + std::to_string(frame); return false; }

            Subject s;
            s.index = idx;
            s.facing = yaw.empty() ? 0.0f : yaw[0];
            auto p = [&](int k, DirectX::XMFLOAT3 fallback) {
                return slot[k] >= 0 ? pos[0][(size_t)slot[k]] : fallback;
            };
            s.head = p(0, {0, 10, 0});
            s.center = p(1, s.head);
            s.upper = p(2, s.head);
            const DirectX::XMFLOAT3 lh = p(3, s.head), rh = p(4, s.head);
            const DirectX::XMFLOAT3 lf = p(5, s.center), rf = p(6, s.center);
            s.feet = {s.center.x, std::min(lf.y, rf.y), s.center.z};
            s.height = std::max(1.0f, s.head.y - s.feet.y);
            s.headTop = {s.head.x, s.head.y + 0.08f * s.height, s.head.z};
            s.points = {s.head, s.center, s.upper, lh, rh, lf, rf, s.headTop};
            subjects.push_back(std::move(s));
        }

        // union vertical extents and widths
        float topY = -1e30f, bottomY = 1e30f;
        float sumX = 0, sumZ = 0;
        for (const Subject& s : subjects) {
            topY = std::max(topY, s.headTop.y);
            bottomY = std::min(bottomY, s.feet.y);
            sumX += s.center.x;
            sumZ += s.center.z;
        }
        const float unionHeight = std::max(0.1f, topY - bottomY);
        const float primaryHeight = subjects.front().height;
        const float facing = subjects.front().facing;

        float spanWorld = 0.0f;
        float anchorY = 0.0f;
        float targetFill = 0.9f;
        if (fillValue > 0.0f) {
            spanWorld = unionHeight;
            targetFill = std::clamp(fillValue, 0.02f, 4.0f);
            anchorY = (topY + bottomY) * 0.5f;
        } else {
            if (subjects.size() == 1) {
                bool ok = false;
                const float frac = ShotFraction(shotName, ok);
                if (!ok) { err = "shot must be ecu, cu, mcu, ms, cowboy, full or wide (or give fill)"; return false; }
                spanWorld = frac * primaryHeight;
                anchorY = subjects.front().headTop.y - spanWorld * 0.5f;
                if (shotName == "ecu") anchorY = subjects.front().head.y;
            } else {
                bool ok = false;
                ShotFraction(shotName, ok);
                if (!ok) { err = "shot must be ecu, cu, mcu, ms, cowboy, full or wide (or give fill)"; return false; }
                spanWorld = ShotMargin(shotName) * unionHeight;
                anchorY = (topY + bottomY) * 0.5f;
            }
        }
        if (spanWorld < 0.01f) spanWorld = 0.01f;

        // focus overrides the anchor (face = head / eyes, upper = 上半身, body = センター)
        const std::string explicitFocus = focus.empty() ? subject.value("focus", std::string()) : focus;
        const bool faceLed = explicitFocus == "face" ||
                             (explicitFocus.empty() && (shotName == "ecu" || shotName == "cu" || shotName == "mcu"));
        if (explicitFocus == "face") anchorY = subjects.front().head.y;
        else if (explicitFocus == "upper") anchorY = subjects.front().upper.y;
        else if (explicitFocus == "body") anchorY = subjects.front().center.y;
        else if (explicitFocus.empty() && shotName == "ecu") anchorY = subjects.front().head.y;

        DirectX::XMFLOAT3 anchor{sumX / (float)subjects.size(), anchorY, sumZ / (float)subjects.size()};

        // Yaw sign: relative +yaw_deg puts the camera on the subject's LEFT (camera yaw = facing + yaw_deg). With the
        // subject facing -Z, camera yaw +90 sends the eye to +X, the subject's left side. absolute: true uses the world
        // yaw of studio_set_camera (0 = the camera in front looking along -Z).
        const float yawDeg = BoolOr(angle, "absolute", false)
                                 ? NumberOr(angle, "yaw_deg", 0.0f)
                                 : facing + NumberOr(angle, "yaw_deg", 0.0f);
        const float pitchDeg = NumberOr(angle, "pitch_deg", 0.0f);
        const float rollDeg = NumberOr(angle, "roll_deg", 0.0f);
        const float fovClamped = std::clamp(std::round(fov <= 0.0f ? 25.0f : fov), 1.0f, 125.0f);

        float screenX = NumberOr(screenArg, "x", 0.5f);
        float screenY = NumberOr(screenArg, "y", faceLed ? 0.38f : 0.5f);
        const float lead = std::clamp(NumberOr(args, "lead", 0.0f), 0.0f, 0.2f);

        // camera basis (rotating (0,0,1) etc. exactly like CameraMotion::ToView)
        CameraPose basisPose;
        basisPose.rotation = {DirectX::XMConvertToRadians(-pitchDeg), DirectX::XMConvertToRadians(yawDeg),
                              DirectX::XMConvertToRadians(rollDeg)};
        const DirectX::XMMATRIX rot = DirectX::XMMatrixRotationRollPitchYaw(
            -basisPose.rotation.x, -basisPose.rotation.y, -basisPose.rotation.z);
        const DirectX::XMFLOAT3 fwd = ToVec3(DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 0, 1, 0), rot));
        const DirectX::XMFLOAT3 up = ToVec3(DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 1, 0, 0), rot));
        DirectX::XMFLOAT3 right = ToVec3(DirectX::XMVector3Cross(DirectX::XMLoadFloat3(&up),
                                                                 DirectX::XMLoadFloat3(&fwd)));
        right = Normalize(right);

        const float aspect = d.AspectRatio();
        const float tanHalfY = std::tan(DirectX::XMConvertToRadians(fovClamped * 0.5f));
        const float tanHalfX = tanHalfY * aspect;

        // look room: shift the subject opposite the direction it faces on screen, leaving space in front
        const DirectX::XMFLOAT3 facingVec{std::sin(DirectX::XMConvertToRadians(facing)), 0.0f,
                                          -std::cos(DirectX::XMConvertToRadians(facing))};
        const float facingScreenX = facingVec.x * right.x + facingVec.y * right.y + facingVec.z * right.z;
        screenX = std::clamp(screenX - lead * facingScreenX, -0.5f, 1.5f);
        screenY = std::clamp(screenY, -0.5f, 1.5f);

        // width estimate along the camera right axis
        float minR = 1e30f, maxR = -1e30f;
        for (const Subject& s : subjects)
            for (const DirectX::XMFLOAT3& p : s.points) {
                const float r = p.x * right.x + p.y * right.y + p.z * right.z;
                minR = std::min(minR, r);
                maxR = std::max(maxR, r);
            }
        const float widthWorld = std::max(0.05f, maxR - minR);
        float dist = std::max(spanWorld / (2.0f * targetFill * tanHalfY),
                              widthWorld / (2.0f * targetFill * tanHalfX));
        dist = std::max(1.0f, dist);

        // the full-body fraction this shot should show, used to drive the iteration on the projected bbox
        const float fullHeight = unionHeight;
        const float desiredFull = std::max(0.01f, targetFill * fullHeight / spanWorld);

        pose = CameraPose{};
        pose.fovDeg = fovClamped;
        for (int iter = 0; iter < 10; ++iter) {
            const float a = (2.0f * screenX - 1.0f) * dist * tanHalfX;
            const float b = (1.0f - 2.0f * screenY) * dist * tanHalfY;
            pose.target = Add(anchor, Add(Scale(right, -a), Scale(up, -b)));
            pose.rotation = basisPose.rotation;
            pose.distance = -dist;
            pose.fovDeg = fovClamped;

            const nlohmann::json check = McpFrameCheck(frame, pose);
            float y0 = 1e30f, y1 = -1e30f;
            for (const nlohmann::json& c : check["characters"]) {
                const int mi = c.value("model", -1);
                if (std::find(models.begin(), models.end(), mi) == models.end() || !c.contains("bbox")) continue;
                y0 = std::min(y0, c["bbox"][1].get<float>());
                y1 = std::max(y1, c["bbox"][3].get<float>());
            }
            if (y1 < y0) break;  // nothing sampled: keep the guess
            const float shown = std::max(0.01f, y1 - y0);
            float ratio = shown / desiredFull;
            if (subjects.size() > 1) {
                // union fit: the group must also stay inside the frame width
                float x0 = 1e30f, x1 = -1e30f;
                for (const nlohmann::json& c : check["characters"]) {
                    const int mi = c.value("model", -1);
                    if (std::find(models.begin(), models.end(), mi) == models.end() || !c.contains("bbox")) continue;
                    x0 = std::min(x0, c["bbox"][0].get<float>());
                    x1 = std::max(x1, c["bbox"][2].get<float>());
                }
                if (x1 > x0) ratio = std::max(ratio, (x1 - x0) / 0.9f);
            }
            if (std::abs(ratio - 1.0f) < 0.005f) break;
            dist = std::clamp(dist * ratio, 1.0f, 5000.0f);
        }

        DirectX::XMFLOAT3 eye;
        CameraMotion::ToView(pose, nullptr, &eye);
        camOut = nlohmann::json{{"target", Vec3Json(pose.target)},
                                {"yaw_deg", DirectX::XMConvertToDegrees(pose.rotation.y)},
                                {"pitch_deg", -DirectX::XMConvertToDegrees(pose.rotation.x)},
                                {"roll_deg", DirectX::XMConvertToDegrees(pose.rotation.z)},
                                {"dist", -pose.distance},
                                {"fov_deg", pose.fovDeg},
                                {"eye", Vec3Json(eye)}};
        if (outAnchor) *outAnchor = anchor;
        if (outHeight) *outHeight = primaryHeight;
        return true;
    };

    // ---- studio_camera_solve ----------------------------------------------
    if (solveTool) {
        if (!args.contains("frame") || !args["frame"].is_number_integer()) {
            promise->Reject("studio_camera_solve requires an integer frame");
            return true;
        }
        const int frame = std::max(0, args["frame"].get<int>());
        const nlohmann::json subject = args.contains("subject") ? args["subject"] : nlohmann::json::object();
        nlohmann::json angle = args.contains("angle") ? args["angle"] : nlohmann::json::object();
        nlohmann::json screen = args.contains("screen") ? args["screen"] : nlohmann::json::object();
        if (!angle.is_object() || !screen.is_object()) {
            promise->Reject("angle and screen must be objects");
            return true;
        }
        std::string shotName = args.value("shot", std::string());
        float fill = args.value("fill", 0.0f);
        if (shotName.empty() && fill <= 0.0f) shotName = "ms";
        const float fov = args.value("fov_deg", 25.0f);
        const std::string focus = args.value("focus", std::string());
        CameraPose pose;
        nlohmann::json camera;
        DirectX::XMFLOAT3 anchor;
        std::string error;
        if (!solve(frame, subject, shotName, fill, fov, angle, screen, focus, pose, camera, &anchor, error)) {
            promise->Reject(error);
            return true;
        }
        promise->Resolve({{"camera", camera}, {"check", McpFrameCheck(frame, pose)}});
        return true;
    }

    // ---- studio_camera_shots ----------------------------------------------
    if (!args.contains("shots") || !args["shots"].is_array() || args["shots"].empty()) {
        promise->Reject("studio_camera_shots requires a non-empty 'shots' array");
        return true;
    }
    const bool dryRun = args.value("dry_run", false);
    const std::string mode = args.value("mode", std::string("replace_range"));
    if (mode != "merge" && mode != "replace_range") {
        promise->Reject("mode must be replace_range or merge");
        return true;
    }
    const nlohmann::json& shotsJson = args["shots"];
    if (shotsJson.size() > 2000) {
        promise->Reject("Too many shots (max 2000)");
        return true;
    }

    struct Shot {
        int start = 0, end = 0;
        nlohmann::json subject;
        std::string shot, endShot;
        nlohmann::json angle, endAngle, screen, endScreen;
        float fov = 25.0f, endFov = 0.0f;
        bool hasEndFov = false;
        std::string move;
        float amount = 0.0f;
        bool hasAmount = false;
        nlohmann::json curve = "ease_in_out";
        bool follow = false;
        int followStep = 10;
        std::vector<int> accents;
        nlohmann::json accent;
        bool hasFocus = false;
        nlohmann::json focus;
    };
    std::vector<Shot> shots;
    shots.reserve(shotsJson.size());
    for (const nlohmann::json& sj : shotsJson) {
        if (!sj.is_object()) { promise->Reject("every shot must be an object"); return true; }
        Shot s;
        if (!sj.contains("start") || !sj.contains("end") || !sj["start"].is_number_integer() ||
            !sj["end"].is_number_integer()) {
            promise->Reject("every shot needs integer start and end frames");
            return true;
        }
        s.start = sj["start"].get<int>();
        s.end = sj["end"].get<int>();
        if (s.start < 0 || s.end < s.start) {
            promise->Reject("shot frames must satisfy 0 <= start <= end");
            return true;
        }
        s.subject = sj.contains("subject") ? sj["subject"] : nlohmann::json::object();
        s.shot = sj.value("shot", std::string("ms"));
        s.endShot = sj.value("end_shot", std::string());
        s.angle = sj.contains("angle") ? sj["angle"] : nlohmann::json::object();
        s.endAngle = sj.contains("end_angle") ? sj["end_angle"] : nlohmann::json::object();
        s.screen = sj.contains("screen") ? sj["screen"] : nlohmann::json::object();
        s.endScreen = sj.contains("end_screen") ? sj["end_screen"] : nlohmann::json::object();
        s.fov = sj.value("fov_deg", 25.0f);
        if (sj.contains("end_fov_deg") && sj["end_fov_deg"].is_number()) {
            s.endFov = sj["end_fov_deg"].get<float>();
            s.hasEndFov = true;
        }
        s.move = sj.value("move", std::string("static"));
        if (sj.contains("amount") && sj["amount"].is_number()) {
            s.amount = sj["amount"].get<float>();
            s.hasAmount = true;
        }
        s.curve = sj.contains("curve") ? sj["curve"] : nlohmann::json("ease_in_out");
        s.follow = BoolOr(sj, "follow", false) || s.move == "follow";
        s.followStep = std::max(1, sj.value("follow_step", 10));
        if (sj.contains("accents") && sj["accents"].is_array()) {
            for (const nlohmann::json& a : sj["accents"])
                if (a.is_number_integer()) s.accents.push_back(a.get<int>());
        }
        s.accent = sj.contains("accent") ? sj["accent"] : nlohmann::json::object();
        s.hasFocus = sj.contains("focus") && sj["focus"].is_object();
        s.focus = s.hasFocus ? sj["focus"] : nlohmann::json::object();
        if (!s.angle.is_object() || !s.endAngle.is_object() || !s.screen.is_object() || !s.endScreen.is_object() ||
            !s.subject.is_object() || !s.accent.is_object() || !s.focus.is_object()) {
            promise->Reject("subject / angle / end_angle / screen / end_screen / accent / focus must be objects");
            return true;
        }
        bool known = false;
        ShotFraction(s.shot, known);
        if (!known) { promise->Reject("unknown shot: " + s.shot); return true; }
        if (!s.endShot.empty()) {
            ShotFraction(s.endShot, known);
            if (!known) { promise->Reject("unknown end_shot: " + s.endShot); return true; }
        }
        if (!IsKnownMove(s.move)) { promise->Reject("unknown move: " + s.move); return true; }
        shots.push_back(std::move(s));
    }
    // overlapping shots are rejected; end + 1 == next start is a hard cut (allowed)
    {
        std::vector<size_t> order(shots.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return shots[a].start < shots[b].start; });
        for (size_t i = 0; i + 1 < order.size(); ++i) {
            const Shot& a = shots[order[i]];
            const Shot& b = shots[order[i + 1]];
            if (b.start <= a.end) {
                promise->Reject("shots overlap: [" + std::to_string(a.start) + ", " + std::to_string(a.end) +
                                "] and [" + std::to_string(b.start) + ", " + std::to_string(b.end) + "]");
                return true;
            }
        }
    }

    struct Generated {
        int start = 0, end = 0;
        size_t keys = 0;
        CameraPose firstPose, lastPose;
        bool haveFirst = false, haveLast = false;
    };
    std::vector<Generated> generated(shots.size());
    std::vector<nlohmann::json> allKeys;
    std::string error;
    std::vector<int> focusCalls;  // model indices per shot (parallel to shots; -1 = no focus)

    for (size_t si = 0; si < shots.size(); ++si) {
        const Shot& s = shots[si];
        Generated& g = generated[si];
        g.start = s.start;
        g.end = s.end;
        focusCalls.push_back(-1);

        CameraPose startPose;
        nlohmann::json camStart;
        DirectX::XMFLOAT3 startAnchor;
        float startHeight = 1.0f;
        if (!solve(s.start, s.subject, s.shot, 0.0f, s.fov, s.angle, s.screen, std::string(), startPose, camStart,
                   &startAnchor, error, &startHeight)) {
            promise->Reject(error);
            return true;
        }
        g.firstPose = startPose;
        g.haveFirst = true;
        CameraPose lastPose = startPose;
        g.lastPose = startPose;
        g.haveLast = true;

        const float startYaw = DirectX::XMConvertToDegrees(startPose.rotation.y);
        std::map<int, nlohmann::json> keys;  // frame -> key JSON (a later write on the same frame wins)

        auto putPose = [&](int frame, const CameraPose& pose, const nlohmann::json& curve) {
            nlohmann::json k = CameraKeyJson(pose, curve);
            k["frame"] = frame;
            keys[frame] = std::move(k);
        };

        // end shot / fields (end_shot defaults to the start size for moves that do not change it)
        std::string endShot = s.endShot;
        if (endShot.empty()) endShot = s.shot;
        float endFov = s.hasEndFov ? s.endFov : s.fov;
        nlohmann::json endAngle = s.endAngle.empty() ? s.angle : s.endAngle;
        nlohmann::json endScreen = s.endScreen.empty() ? s.screen : s.endScreen;

        const bool isArc = s.move == "arc_left" || s.move == "arc_right";
        const bool isTruck = s.move == "truck_left" || s.move == "truck_right";
        const bool isCrane = s.move == "crane_up" || s.move == "crane_down";
        const bool isZoom = s.move == "zoom_in" || s.move == "zoom_out" || s.move == "dolly_zoom";
        const bool isDolly = s.move == "dolly_in" || s.move == "dolly_out";

        // base keys along the shot
        if (isArc) {
            const float amount = s.hasAmount ? s.amount : 35.0f;
            const float arcDeg = s.move == "arc_left" ? -std::abs(amount) : std::abs(amount);
            const int step = s.follow ? std::max(1, s.followStep) : 15;
            for (int f = s.start; f <= s.end; f += step) {
                const float t = s.end > s.start ? (float)(f - s.start) / (float)(s.end - s.start) : 0.0f;
                nlohmann::json fracAngle = nlohmann::json::object();
                fracAngle["yaw_deg"] = startYaw + arcDeg * t;
                fracAngle["pitch_deg"] = NumberOr(s.angle, "pitch_deg", 0.0f) +
                                         (NumberOr(endAngle, "pitch_deg", NumberOr(s.angle, "pitch_deg", 0.0f)) -
                                          NumberOr(s.angle, "pitch_deg", 0.0f)) * t;
                fracAngle["roll_deg"] = NumberOr(s.angle, "roll_deg", 0.0f) +
                                        (NumberOr(endAngle, "roll_deg", NumberOr(s.angle, "roll_deg", 0.0f)) -
                                         NumberOr(s.angle, "roll_deg", 0.0f)) * t;
                fracAngle["absolute"] = true;
                CameraPose pose;
                nlohmann::json cam;
                std::string err;
                if (!solve(f, s.subject, s.shot, 0.0f, s.fov, fracAngle, s.screen, std::string(), pose, cam, nullptr,
                           err)) {
                    promise->Reject(err);
                    return true;
                }
                putPose(f, pose, s.curve);
                lastPose = pose;
            }
            if (!keys.count(s.end)) putPose(s.end, lastPose, s.curve);
        } else if (isTruck) {
            const float amount = s.hasAmount ? s.amount : 0.6f * startHeight;
            const float sign = s.move == "truck_left" ? -1.0f : 1.0f;
            CameraPose endPose = startPose;
            nlohmann::json cam;
            std::string err;
            // heading fixed: same orientation, target slides along the camera right axis
            const DirectX::XMMATRIX rot = DirectX::XMMatrixRotationRollPitchYaw(
                -startPose.rotation.x, -startPose.rotation.y, -startPose.rotation.z);
            const DirectX::XMFLOAT3 upv = ToVec3(DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 1, 0, 0), rot));
            const DirectX::XMFLOAT3 fwdv = ToVec3(DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0, 0, 1, 0), rot));
            DirectX::XMFLOAT3 rightv = Normalize(ToVec3(DirectX::XMVector3Cross(
                DirectX::XMLoadFloat3(&upv), DirectX::XMLoadFloat3(&fwdv))));
            endPose.target = Add(startPose.target, Scale(rightv, sign * amount));
            endPose.rotation = startPose.rotation;
            endPose.distance = startPose.distance;
            endPose.fovDeg = s.fov;
            (void)endShot;
            putPose(s.start, startPose, s.curve);
            putPose(s.end, endPose, s.curve);
            lastPose = endPose;
        } else if (isZoom) {
            CameraPose endPose = startPose;
            DirectX::XMFLOAT3 anchorEnd = startAnchor;
            nlohmann::json cam;
            std::string err;
            // re-sample the subject at end to get its anchor there (same start framing)
            CameraPose probe;
            if (!solve(s.end, s.subject, s.shot, 0.0f, s.fov, s.angle, s.screen, std::string(), probe, cam, &anchorEnd,
                       err)) {
                promise->Reject(err);
                return true;
            }
            DirectX::XMFLOAT3 eyeStart;
            CameraMotion::ToView(startPose, nullptr, &eyeStart);
            const DirectX::XMFLOAT3 eDir = Normalize(Sub(eyeStart, startPose.target));
            if (s.move == "dolly_zoom") {
                const float newFov = s.hasEndFov ? endFov : std::clamp(s.fov * 1.4f, 1.0f, 125.0f);
                const float ratio = std::tan(DirectX::XMConvertToRadians(s.fov * 0.5f)) /
                                    std::tan(DirectX::XMConvertToRadians(newFov * 0.5f));
                const float distStart = -startPose.distance;
                const float newDist = std::max(1.0f, distStart * ratio);
                endPose.target = anchorEnd;
                endPose.fovDeg = newFov;
                endPose.distance = -newDist;
                endPose.rotation = startPose.rotation;
            } else {
                const float newFov = s.hasEndFov ? endFov
                                                : std::clamp(s.fov * (s.move == "zoom_in" ? 0.6f : 1.6f), 1.0f, 125.0f);
                endPose.target = anchorEnd;
                endPose.rotation = startPose.rotation;
                endPose.fovDeg = newFov;
                const float proj = (eyeStart.x - anchorEnd.x) * eDir.x + (eyeStart.y - anchorEnd.y) * eDir.y +
                                   (eyeStart.z - anchorEnd.z) * eDir.z;
                endPose.distance = -std::max(1.0f, proj);
            }
            putPose(s.start, startPose, s.curve);
            putPose(s.end, endPose, s.curve);
            lastPose = endPose;
        } else if (isCrane) {
            const float amount = s.hasAmount ? s.amount : 25.0f;
            const float sign = s.move == "crane_up" ? 1.0f : -1.0f;
            nlohmann::json craneAngle = s.angle;
            craneAngle["pitch_deg"] = NumberOr(s.angle, "pitch_deg", 0.0f) + sign * amount;
            CameraPose endPose;
            nlohmann::json cam;
            std::string err;
            if (!solve(s.end, s.subject, endShot, 0.0f, endFov, craneAngle, endScreen, std::string(), endPose, cam,
                       nullptr, err)) {
                promise->Reject(err);
                return true;
            }
            putPose(s.start, startPose, s.curve);
            putPose(s.end, endPose, s.curve);
            lastPose = endPose;
        } else if (isDolly) {
            const std::string targetShot = !s.endShot.empty()
                                               ? s.endShot
                                               : NeighbourShot(s.shot, s.move == "dolly_in" ? 1 : -1);
            CameraPose endPose;
            nlohmann::json cam;
            std::string err;
            if (!solve(s.end, s.subject, targetShot, 0.0f, s.fov, endAngle, endScreen, std::string(), endPose, cam,
                       nullptr, err)) {
                promise->Reject(err);
                return true;
            }
            putPose(s.start, startPose, s.curve);
            putPose(s.end, endPose, s.curve);
            lastPose = endPose;
        } else {
            // static / follow / push_accent: hold the framing (follow re-solves on the moving subject)
            if (s.follow && s.end > s.start) {
                const int step = std::max(1, s.followStep);
                for (int f = s.start + step; f < s.end; f += step) {
                    const float t = (float)(f - s.start) / (float)(s.end - s.start);
                    nlohmann::json fa = s.angle;
                    fa["pitch_deg"] = NumberOr(s.angle, "pitch_deg", 0.0f) +
                                      (NumberOr(endAngle, "pitch_deg", NumberOr(s.angle, "pitch_deg", 0.0f)) -
                                       NumberOr(s.angle, "pitch_deg", 0.0f)) * t;
                    fa["roll_deg"] = NumberOr(s.angle, "roll_deg", 0.0f) +
                                     (NumberOr(endAngle, "roll_deg", NumberOr(s.angle, "roll_deg", 0.0f)) -
                                      NumberOr(s.angle, "roll_deg", 0.0f)) * t;
                    nlohmann::json fs = nlohmann::json::object();
                    fs["x"] = NumberOr(s.screen, "x", 0.5f) +
                              (NumberOr(endScreen, "x", NumberOr(s.screen, "x", 0.5f)) -
                               NumberOr(s.screen, "x", 0.5f)) * t;
                    fs["y"] = NumberOr(s.screen, "y", 0.5f) +
                              (NumberOr(endScreen, "y", NumberOr(s.screen, "y", 0.5f)) -
                               NumberOr(s.screen, "y", 0.5f)) * t;
                    CameraPose pose;
                    nlohmann::json cam;
                    std::string err;
                    if (!solve(f, s.subject, s.shot, 0.0f, s.fov + (endFov - s.fov) * t, fa, fs, std::string(), pose,
                               cam, nullptr, err)) {
                        promise->Reject(err);
                        return true;
                    }
                    putPose(f, pose, s.curve);
                    lastPose = pose;
                }
                if (!keys.count(s.end)) putPose(s.end, lastPose, s.curve);
            }
            if (!keys.count(s.start)) putPose(s.start, startPose, s.curve);
            if (s.end > s.start && !keys.count(s.end)) putPose(s.end, startPose, s.curve);
        }

        // accents: push (dist), zoom (fov) or roll bumps around beat frames
        if (!s.accents.empty()) {
            const std::string kind = s.accent.value("kind", std::string("push"));
            const float amount = NumberOr(s.accent, "amount", kind == "zoom" ? 4.0f : (kind == "roll" ? 6.0f : 0.12f));
            const int attack = std::max(1, (int)NumberOr(s.accent, "attack", 3.0f));
            const int release = std::max(1, (int)NumberOr(s.accent, "release", 10.0f));
            std::vector<int> used;
            std::vector<int> accents = s.accents;
            std::sort(accents.begin(), accents.end());
            for (int a : accents) {
                if (a < s.start + attack || a > s.end - release) continue;
                if (a - attack < s.start || a + release > s.end) continue;
                bool tooClose = false;
                for (int u : used)
                    if (std::abs(a - u) < attack + release) tooClose = true;
                if (tooClose) continue;
                used.push_back(a);

                // pre value: sample the base curve at a - attack (or the start pose)
                CameraPose basePose = startPose;
                auto itKey = keys.upper_bound(a - attack);
                if (itKey != keys.begin()) --itKey;
                if (itKey != keys.end()) {
                    const nlohmann::json& k = itKey->second;
                    basePose.target = {k["target"][0].get<float>(), k["target"][1].get<float>(),
                                       k["target"][2].get<float>()};
                    basePose.rotation = {DirectX::XMConvertToRadians(-k["pitch_deg"].get<float>()),
                                         DirectX::XMConvertToRadians(k["yaw_deg"].get<float>()),
                                         DirectX::XMConvertToRadians(k["roll_deg"].get<float>())};
                    basePose.distance = -k["dist"].get<float>();
                    basePose.fovDeg = k["fov_deg"].get<float>();
                }
                CameraPose peak = basePose;
                if (kind == "zoom") {
                    peak.fovDeg = std::clamp(basePose.fovDeg - amount, 1.0f, 125.0f);
                } else if (kind == "roll") {
                    peak.rotation.z = DirectX::XMConvertToRadians(
                        DirectX::XMConvertToDegrees(basePose.rotation.z) + amount);
                } else {
                    peak.distance = -std::max(1.0f, -basePose.distance * (1.0f - amount));
                }
                // the peak keeps the target: the camera dips in / lens pushes, then settles back
                const nlohmann::json fastStart = nlohmann::json::array({0, 64, 64, 127});
                putPose(a - attack, basePose, s.curve);
                putPose(a, peak, fastStart);
                putPose(a + release, basePose, nlohmann::json("ease_out"));
            }
        }

        // collect keys, honour the shot's range, unwrap the yaw between consecutive keys
        double prevYaw = 0.0;
        bool havePrev = false;
        for (auto& [frame, key] : keys) {
            if (frame < s.start || frame > s.end) continue;
            double yaw = key["yaw_deg"].get<double>();
            if (havePrev) {
                while (yaw - prevYaw > 180.0) yaw -= 360.0;
                while (yaw - prevYaw < -180.0) yaw += 360.0;
                key["yaw_deg"] = yaw;
            }
            prevYaw = yaw;
            havePrev = true;
            allKeys.push_back(key);
            ++g.keys;
            // first / last checks use the actual first / last generated pose
            const float pitch = key["pitch_deg"].get<float>();
            const float roll = key["roll_deg"].get<float>();
            CameraPose kp;
            kp.target = {key["target"][0].get<float>(), key["target"][1].get<float>(), key["target"][2].get<float>()};
            kp.rotation = {DirectX::XMConvertToRadians(-pitch), DirectX::XMConvertToRadians((float)yaw),
                           DirectX::XMConvertToRadians(roll)};
            kp.distance = -key["dist"].get<float>();
            kp.fovDeg = key["fov_deg"].get<float>();
            if (frame == s.start) {
                g.firstPose = kp;
                g.haveFirst = true;
            }
            g.lastPose = kp;
            g.haveLast = true;
        }

        // DoF focus key at start (a separate undo step is acceptable)
        if (s.hasFocus) {
            const nlohmann::json* modelsJson = FindMember(s.subject, "models");
            int mi = -1;
            if (modelsJson && modelsJson->is_array() && !modelsJson->empty() && (*modelsJson)[0].is_number_integer())
                mi = (*modelsJson)[0].get<int>();
            else
                for (size_t i = 0; i < d.models.size(); ++i)
                    if (d.models[i]->kind == ModelKind::Character && d.models[i]->visible) {
                        mi = (int)i;
                        break;
                    }
            focusCalls[si] = mi;
        }
    }

    if (allKeys.empty()) {
        promise->Reject("No camera keys were generated");
        return true;
    }

    nlohmann::json shotResults = nlohmann::json::array();
    for (size_t si = 0; si < shots.size(); ++si) {
        const Generated& g = generated[si];
        nlohmann::json r;
        r["start"] = g.start;
        r["end"] = g.end;
        r["keys_written"] = g.keys;
        r["first_check"] = McpFrameCheck(g.start, g.firstPose);
        r["last_check"] = McpFrameCheck(g.end, g.lastPose);
        shotResults.push_back(std::move(r));
    }

    std::string undo;
    if (!dryRun) {
        // ONE undo step over the whole sequence: capture, clear the shot ranges (replace_range), write with merge
        std::vector<TrackState> before{CaptureTrack(d, -1, RowKind::Camera, "")};
        if (mode == "replace_range") {
            for (const Shot& s : shots) {
                d.camera.camera.erase(std::remove_if(d.camera.camera.begin(), d.camera.camera.end(),
                                                     [&](const CameraKf& k) {
                                                         return k.frame >= s.start && k.frame <= s.end;
                                                     }),
                                      d.camera.camera.end());
            }
        }
        size_t changed = 0;
        if (!McpWriteCameraKeys(nlohmann::json(allKeys), "merge", changed, error, false)) {
            promise->Reject(error);
            return true;
        }
        StudioPushTrackEdit("AI: studio_camera_shots", before);
        undo = d.history.CanUndo() ? d.history.UndoName() : std::string();

        // focus keys after the camera write (the studiofocus command is its own undo step)
        for (size_t si = 0; si < shots.size(); ++si) {
            if (!shots[si].hasFocus || focusCalls[si] < 0) continue;
            const float aperture = std::clamp(NumberOr(shots[si].focus, "aperture", 1.0f), 0.0f, 1.0f);
            std::string focusError;
            ExecuteUiCommand("studiofocus",
                             {std::to_string(shots[si].start), "target", std::to_string(focusCalls[si]),
                              std::to_string(aperture), "0", "head"},
                             nullptr, &focusError);
        }
    }

    nlohmann::json out;
    out["shots"] = std::move(shotResults);
    if (dryRun) out["keys"] = nlohmann::json(allKeys);
    out["undo"] = undo;
    promise->Resolve(std::move(out));
    return true;
}

}  // namespace mmdx
