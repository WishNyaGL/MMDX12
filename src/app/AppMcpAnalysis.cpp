// Read-only MCP studio analysis tools: scene snapshot, motion sampling, derived choreography facts, framing checks and
// music structure. Nothing here edits the document, the undo history, the playhead or the view. Pose sampling re-poses
// the instance like StudioUpdateModel does (bound motion + placement root, no pose layer, physics disabled) and then
// restores the current frame through StudioUpdateModel.
#include "app/App.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "audio/BeatAnalysis.h"
#include "core/TextUtil.h"
#include "studio/SceneLight.h"
#include "studio/StudioDoc.h"

namespace mmdx {

using namespace studio;

namespace {

constexpr double kFrameRound = 100.0;   // timeline frames are rounded to 0.01
constexpr int kSampleCap = 4000;        // studio_sample_motion: samples x bones
// studio_motion_events thresholds (fractions of the character's height unless stated).
constexpr float kJumpHeightRatio = 0.15f;      // centre y above the rolling median
constexpr float kCrouchDepthRatio = 0.12f;     // centre y below the rolling median
constexpr int kBaselineHalfWindow = 15;        // frames: the 1-second rolling median (+-15)
constexpr int kEventMinGap = 15;               // frames between two peaks of the same kind
constexpr float kTurnMinDeg = 60.0f;           // facing change
constexpr int kTurnWindow = 45;                // frames: 1.5 s
constexpr float kHoldSpeedRatio = 0.10f;       // below this x the median speed
constexpr float kTeleportRatio = 0.5f;         // centre travel per frame above this x height = a teleport
constexpr int kHoldMinFrames = 30;             // 1 s
constexpr int kEnergyWindow = 15;              // frames
constexpr int kFormationWindow = 30;           // frames

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

nlohmann::json Vec3(const DirectX::XMFLOAT3& v) {
    return nlohmann::json::array({Round3(v.x), Round3(v.y), Round3(v.z)});
}

nlohmann::json Vec2(const DirectX::XMFLOAT2& v) {
    return nlohmann::json::array({Round3(v.x), Round3(v.y)});
}

double TimelineFrame(double seconds, double offset) {
    return std::round((seconds + offset) * 30.0 * kFrameRound) / kFrameRound;
}

std::pair<int, int> MotionRange(const MotionData& m) {
    int lo = INT_MAX, hi = INT_MIN;
    const auto scan = [&](int f) {
        lo = std::min(lo, f);
        hi = std::max(hi, f);
    };
    for (const auto& [name, keys] : m.bones)
        for (const BoneKf& k : keys) scan(k.frame);
    for (const auto& [name, keys] : m.morphs)
        for (const MorphKf& k : keys) scan(k.frame);
    for (const auto& [name, keys] : m.ik)
        for (const IkKf& k : keys) scan(k.frame);
    for (const VisKf& k : m.visibility) scan(k.frame);
    for (const AccessoryKf& k : m.accessory) scan(k.frame);
    for (const CameraKf& k : m.camera) scan(k.frame);
    for (const LightKf& k : m.light) scan(k.frame);
    for (const ShadowKf& k : m.shadow) scan(k.frame);
    for (const FocusKf& k : m.focus) scan(k.frame);
    if (lo == INT_MAX) return {0, 0};
    return {lo, hi};
}

std::vector<int> SteppedFrames(int start, int end, int step) {
    std::vector<int> frames;
    if (step < 1) step = 1;
    for (int f = start; f <= end; f += step) frames.push_back(f);
    return frames;
}

nlohmann::json LightValuesJson(const LightValues& v) {
    return nlohmann::json{
        {"position", Vec3(v.position)},
        {"aim", Vec3(v.aim)},
        {"direction", Vec3(v.direction)},
        {"color", Vec3(v.color)},
        {"intensity", Round3(v.intensity)},
        {"range", Round3(v.range)},
        {"cone_outer_deg", Round3(DirectX::XMConvertToDegrees(v.coneOuter))},
        {"cone_inner_deg", Round3(DirectX::XMConvertToDegrees(v.coneInner))},
        {"size", Vec2(v.size)},
    };
}

float FacingYawDeg(const DirectX::XMFLOAT4X4& world) {
    // MMD models face -Z at rest: the bone's local -Z is its forward axis. Yaw 0 = -Z, +90 = +X.
    const DirectX::XMVECTOR f = DirectX::XMVector3TransformNormal(
        DirectX::XMVectorSet(0, 0, -1, 0), DirectX::XMLoadFloat4x4(&world));
    return DirectX::XMConvertToDegrees(std::atan2(DirectX::XMVectorGetX(f), -DirectX::XMVectorGetZ(f)));
}

float NormalizeDeg(float d) {
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}

nlohmann::json BoundsJson(const DirectX::XMFLOAT3& lo, const DirectX::XMFLOAT3& hi) {
    return nlohmann::json::array({Round3(lo.x), Round3(lo.y), Round3(lo.z), Round3(hi.x), Round3(hi.y), Round3(hi.z)});
}

}  // namespace

// ---------------------------------------------------------------------------
// Pose sampling (tools 2-4)
// ---------------------------------------------------------------------------

std::vector<std::vector<DirectX::XMFLOAT3>> App::StudioSampleBones(studio::StudioModel& m, const std::vector<int>& frames,
                                                                  const std::vector<int>& bones,
                                                                  std::vector<float>* facingYawDeg) {
    std::vector<std::vector<DirectX::XMFLOAT3>> out;
    if (!studio_ || !m.inst || !m.pmx) return out;
    StudioDoc& d = *studio_;
    ModelInstance& inst = *m.inst;

    if (m.boundVersion != m.motionVersion) {
        if (m.motion.bones.empty() && m.motion.morphs.empty() && m.motion.ik.empty() && m.motion.visibility.empty()) {
            m.bound.reset();
        } else {
            const VmdMotion vmd = m.motion.ToVmd();
            m.bound = BoundMotion::Bind(*m.pmx, {&vmd});
        }
        m.boundVersion = m.motionVersion;
    }

    const bool character = m.kind == ModelKind::Character;
    float scale = 1.0f;
    if (character) {
        scale = (m.libraryId.empty() ? 1.0f : settings_.CharacterScale(m.libraryId)) * std::max(0.01f, m.place.scale);
        inst.SetScale(scale);
        inst.EnablePhysics(false);
    } else if (m.IsStage()) {
        scale = std::max(0.01f, m.place.scale);
        inst.EnablePhysics(false);
    } else {
        return out;  // props are rejected by the callers
    }

    const auto applyRoot = [&]() {
        using namespace DirectX;
        constexpr float kRad = 0.01745329252f;
        XMFLOAT4X4 root;
        if (character) {
            XMStoreFloat4x4(&root, XMMatrixMultiply(
                                       XMMatrixRotationRollPitchYaw(m.place.rotationDeg.x * kRad, m.place.rotationDeg.y * kRad,
                                                                    m.place.rotationDeg.z * kRad),
                                       XMMatrixTranslation(m.place.translation.x / scale, m.place.translation.y / scale,
                                                           m.place.translation.z / scale)));
        } else {
            XMStoreFloat4x4(&root, XMMatrixMultiply(
                                       XMMatrixMultiply(XMMatrixScaling(scale, scale, scale),
                                                        XMMatrixRotationRollPitchYaw(m.place.rotationDeg.x * kRad,
                                                                                    m.place.rotationDeg.y * kRad,
                                                                                    m.place.rotationDeg.z * kRad)),
                                       XMMatrixTranslation(m.place.translation.x, m.place.translation.y,
                                                           m.place.translation.z)));
        }
        inst.SetRootTransform(root);
    };

    const int yawBone = m.pmx->FindBone("\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB");  // 上半身
    const int yawBone2 = m.pmx->FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");  // センター
    if (facingYawDeg) facingYawDeg->clear();

    for (int frame : frames) {
        if (m.bound) m.bound->Evaluate((float)frame, inst);
        else inst.ResetPose();
        applyRoot();
        inst.UpdatePose(0.0f);

        std::vector<DirectX::XMFLOAT3> row;
        row.reserve(bones.size());
        for (int b : bones) {
            if (b < 0 || b >= (int)m.pmx->bones.size()) row.push_back({0, 0, 0});
            else row.push_back(inst.BoneWorldPosition(b));
        }
        out.push_back(std::move(row));

        if (facingYawDeg) {
            const int b = yawBone >= 0 ? yawBone : yawBone2;
            facingYawDeg->push_back(b >= 0 ? FacingYawDeg(inst.BoneWorld(b)) : 0.0f);
        }
    }

    // Restore the viewport pose for the current frame (physics reset like a seek).
    if (!frames.empty()) {
        if (m.IsStage() && !m.bound) m.stageMotionApplied = ~0ull;  // force the keyless stage to be posed again
        StudioUpdateModel(m, ctx_.FrameNumber(), (float)(d.time * kMmdFps), 0.0f, true);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Cached beat analysis (shared with the lighting tools)
// ---------------------------------------------------------------------------

const BeatAnalysis* App::StudioBeats(std::string& error) {
    if (!studio_) {
        error = "Studio is not active";
        return nullptr;
    }
    StudioDoc& d = *studio_;
    if (!d.hasAudio || d.audioPath.empty()) {
        error = "The project has no audio";
        return nullptr;
    }
    std::string key = PathToUtf8(d.audioPath);
    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(d.audioPath, ec);
    key += "|";
    key += ec ? std::string("0") : std::to_string((long long)writeTime.time_since_epoch().count());
    if (!studioBeats_ || studioBeatsKey_ != key) {
        studioBeats_ = std::make_shared<BeatAnalysis>(AnalyzeBeats(d.audioPath));
        studioBeatsKey_ = key;
    }
    if (!studioBeats_->ok) {
        error = studioBeats_->error.empty() ? "Audio analysis failed" : studioBeats_->error;
        return nullptr;
    }
    return studioBeats_.get();
}

// ---------------------------------------------------------------------------
// Framing check (studio_frame_check's per-frame body, also used by studio_camera_solve)
// ---------------------------------------------------------------------------

nlohmann::json App::McpFrameCheck(int frame, const CameraPose& pose) {
    StudioDoc& d = *studio_;
    DirectX::XMFLOAT4X4 view;
    DirectX::XMFLOAT3 eye;
    CameraMotion::ToView(pose, &view, &eye);
    const float aspect = d.AspectRatio();
    const DirectX::XMMATRIX vp = DirectX::XMMatrixMultiply(
        DirectX::XMLoadFloat4x4(&view),
        DirectX::XMMatrixPerspectiveFovLH(DirectX::XMConvertToRadians(pose.fovDeg), aspect, 0.1f, 10000.0f));

    std::vector<int> chars;
    for (size_t i = 0; i < d.models.size(); ++i)
        if (d.models[i]->kind == ModelKind::Character && d.models[i]->visible &&
            d.models[i]->inst && d.models[i]->pmx)
            chars.push_back((int)i);

    const std::vector<std::string> pointNames = {
        "\xE9\xA0\xAD", "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB",
        "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96", "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96", "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96",
        "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96"};
    const std::vector<std::string> pointLabels = {"head", "center", "upper_body", "left_hand", "right_hand",
                                                  "left_foot", "right_foot"};
    nlohmann::json frameJson;
    frameJson["frame"] = frame;
    frameJson["eye"] = Vec3(eye);
    frameJson["fov_deg"] = Round3(pose.fovDeg);
    frameJson["aspect"] = Round3(aspect);

    nlohmann::json charsJson = nlohmann::json::array();
    float mainHeight = 0.0f;
    for (int ci : chars) {
        StudioModel& m = *d.models[(size_t)ci];

        // height at the current frame (used for the head-top point)
        const int headBone = m.pmx->FindBone("\xE9\xA0\xAD");
        const int footL = m.pmx->FindBone("\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96");
        const int footR = m.pmx->FindBone("\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96");
        float height = 1.0f;
        {
            float lo = 0, hi = 0;
            bool any = false;
            for (size_t b = 0; b < m.pmx->bones.size(); ++b) {
                const float y = m.inst->BoneWorldPosition((int)b).y;
                if (!any) { lo = hi = y; any = true; }
                else { lo = std::min(lo, y); hi = std::max(hi, y); }
            }
            height = hi - lo;
            if (headBone >= 0 && (footL >= 0 || footR >= 0)) {
                float feet = 1e30f;
                if (footL >= 0) feet = std::min(feet, m.inst->BoneWorldPosition(footL).y);
                if (footR >= 0) feet = std::min(feet, m.inst->BoneWorldPosition(footR).y);
                height = m.inst->BoneWorldPosition(headBone).y - feet;
            }
            if (height <= 1e-3f) height = 1.0f;
        }

        std::vector<int> boneIdx;
        std::vector<int> slots(pointNames.size(), -1);
        for (size_t k = 0; k < pointNames.size(); ++k) {
            const int b = m.pmx->FindBone(pointNames[k]);
            if (b >= 0) {
                slots[k] = (int)boneIdx.size();
                boneIdx.push_back(b);
            }
        }
        std::vector<float> yaw;
        const auto pos = StudioSampleBones(m, {frame}, boneIdx, &yaw);

        struct ScreenPoint {
            std::string label;
            DirectX::XMFLOAT3 world;
            DirectX::XMFLOAT2 screen;
            bool inside = false;
        };
        std::vector<ScreenPoint> points;
        for (size_t k = 0; k < pointNames.size(); ++k) {
            if (slots[k] < 0 || pos.empty()) continue;
            ScreenPoint sp;
            sp.label = pointLabels[k];
            sp.world = pos[0][(size_t)slots[k]];
            points.push_back(sp);
        }
        if (headBone >= 0 && !pos.empty()) {
            ScreenPoint sp;
            sp.label = "head_top";
            sp.world = pos[0][(size_t)slots[0]];
            sp.world.y += 0.08f * height;
            points.push_back(sp);
        }
        if (points.empty()) continue;

        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        int inside = 0, behind = 0;
        nlohmann::json cut = nlohmann::json::array();
        for (ScreenPoint& sp : points) {
            // a point at or behind the eye plane projects mirrored: it is cut and stays out of the bbox
            const float viewZ = DirectX::XMVectorGetZ(
                DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&sp.world), DirectX::XMLoadFloat4x4(&view)));
            if (viewZ <= 0.1f) {
                ++behind;
                cut.push_back(sp.label);
                continue;
            }
            DirectX::XMVECTOR ndc = DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(&sp.world), vp);
            DirectX::XMFLOAT3 n;
            DirectX::XMStoreFloat3(&n, ndc);
            sp.screen = {n.x * 0.5f + 0.5f, 0.5f - n.y * 0.5f};
            sp.inside = sp.screen.x >= 0.0f && sp.screen.x <= 1.0f && sp.screen.y >= 0.0f &&
                        sp.screen.y <= 1.0f && n.z >= 0.0f && n.z <= 1.0f;
            if (sp.inside) ++inside;
            else cut.push_back(sp.label);
            x0 = std::min(x0, sp.screen.x);
            y0 = std::min(y0, sp.screen.y);
            x1 = std::max(x1, sp.screen.x);
            y1 = std::max(y1, sp.screen.y);
        }
        if (behind == (int)points.size()) {  // the whole character is behind the camera
            charsJson.push_back({{"model", ci}, {"uid", m.uid}, {"name", m.name}, {"behind_camera", true},
                                 {"on_screen", 0.0}, {"head_in_frame", false}, {"cut", cut}, {"screen_height", 0.0}});
            continue;
        }

        DirectX::XMFLOAT2 center{0.5f, 0.5f};
        DirectX::XMFLOAT3 centerWorld{0, 0, 0};
        if (slots[1] >= 0 && !pos.empty()) {
            centerWorld = pos[0][(size_t)slots[1]];
            DirectX::XMVECTOR ndc = DirectX::XMVector3TransformCoord(
                DirectX::XMLoadFloat3(&centerWorld), vp);
            DirectX::XMFLOAT3 n;
            DirectX::XMStoreFloat3(&n, ndc);
            center = {n.x * 0.5f + 0.5f, 0.5f - n.y * 0.5f};
        } else {
            center = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
        }

        bool headIn = false;
        for (const ScreenPoint& sp : points)
            if (sp.label == "head" && sp.inside) headIn = true;

        const float dx = eye.x - centerWorld.x, dy = eye.y - centerWorld.y, dz = eye.z - centerWorld.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        float facing = 0.0f;
        if (distance > 1e-6f) {
            const float yawRad = DirectX::XMConvertToRadians(yaw.empty() ? 0.0f : yaw[0]);
            const DirectX::XMFLOAT3 fwd{std::sin(yawRad), 0.0f, -std::cos(yawRad)};
            const float dot = std::clamp((fwd.x * dx + fwd.y * dy + fwd.z * dz) / distance, -1.0f, 1.0f);
            facing = DirectX::XMConvertToDegrees(std::acos(dot));
        }

        const float screenHeight = y1 - y0;
        mainHeight = std::max(mainHeight, screenHeight);
        charsJson.push_back({{"model", ci},
                             {"uid", m.uid},
                             {"name", m.name},
                             {"bbox", nlohmann::json::array({Round3(x0), Round3(y0), Round3(x1), Round3(y1)})},
                             {"on_screen", Round3(points.empty() ? 0.0 : (double)inside / (double)points.size())},
                             {"head_in_frame", headIn},
                             {"cut", cut},
                             {"screen_height", Round3(screenHeight)},
                             {"center", nlohmann::json::array({Round3(center.x), Round3(center.y)})},
                             {"distance", Round3(distance)},
                             {"facing_camera_deg", Round3(facing)}});
    }
    frameJson["characters"] = charsJson;
    const char* shot = "wide";
    if (mainHeight > 1.6f) shot = "extreme_close";
    else if (mainHeight > 1.0f) shot = "close";
    else if (mainHeight > 0.55f) shot = "medium";
    else if (mainHeight > 0.3f) shot = "full";
    frameJson["shot_size"] = shot;
    return frameJson;
}

// ---------------------------------------------------------------------------
// ExecuteMcpAnalysis
// ---------------------------------------------------------------------------

bool App::ExecuteMcpAnalysis(const std::string& tool, const nlohmann::json& args,
                             std::shared_ptr<McpPromise> promise) {
    if (!promise) return false;
    const bool isAnalysis = tool == "studio_scene_info" || tool == "studio_sample_motion" ||
                            tool == "studio_motion_events" || tool == "studio_frame_check" ||
                            tool == "studio_audio_analysis";
    if (!isAnalysis) return false;
    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;

    // ---- studio_scene_info -------------------------------------------------
    if (tool == "studio_scene_info") {
        nlohmann::json out;
        out["fps"] = 30;
        out["frame"] = d.Frame();
        out["end_frame"] = d.EndFrame();
        out["aspect"] = {{"label", std::to_string(d.aspectW) + ":" + std::to_string(d.aspectH)},
                         {"width", d.aspectW},
                         {"height", d.aspectH},
                         {"ratio", Round3(d.AspectRatio())}};
        if (d.hasAudio && !d.audioPath.empty()) {
            out["audio"] = {{"file", PathToUtf8(d.audioPath)},
                            {"offset_seconds", Round3(d.audioOffset)},
                            {"end_frame", Round3(d.audioEndFrame)}};
        } else {
            out["audio"] = nullptr;
        }

        const int frame = d.Frame();
        nlohmann::json models = nlohmann::json::array();
        for (size_t i = 0; i < d.models.size(); ++i) {
            StudioModel& m = *d.models[i];
            nlohmann::json j;
            j["index"] = i;
            j["uid"] = m.uid;
            j["kind"] = ModelKindName(m.kind);
            j["name"] = m.name;
            j["file"] = PathToUtf8(m.path.filename());
            j["visible"] = m.visible;

            const PropAttach place = m.IsProp() ? AccessoryPlacement(m, (float)frame) : m.place;
            j["placement"] = {{"translation", Vec3(place.translation)},
                              {"rotation_deg", Vec3(place.rotationDeg)},
                              {"scale", Round3(place.scale)}};

            size_t boneKeys = 0, morphKeys = 0;
            for (const auto& [n, k] : m.motion.bones) boneKeys += k.size();
            for (const auto& [n, k] : m.motion.morphs) morphKeys += k.size();
            j["bone_key_count"] = boneKeys;
            j["morph_key_count"] = morphKeys;
            const auto range = MotionRange(m.motion);
            j["key_frame_range"] = nlohmann::json::array({range.first, range.second});

            DirectX::XMFLOAT3 lo{0, 0, 0}, hi{0, 0, 0};
            if (m.kind == ModelKind::Character && m.pmx) {
                bool any = false;
                for (size_t b = 0; b < m.pmx->bones.size(); ++b) {
                    if (!m.inst) break;
                    const DirectX::XMFLOAT3 p = m.inst->BoneWorldPosition((int)b);
                    if (!any) {
                        lo = hi = p;
                        any = true;
                    } else {
                        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
                        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
                    }
                }
            } else {
                auto it = studioAabbCache_.find(m.uid);
                if (it == studioAabbCache_.end() || !m.inst || !m.pmx) {
                    bool any = false;
                    if (m.inst && m.pmx) {
                        const DirectX::XMMATRIX root = DirectX::XMLoadFloat4x4(&m.inst->RootTransform());
                        for (const PmxVertex& v : m.pmx->vertices) {
                            DirectX::XMFLOAT3 p;
                            DirectX::XMStoreFloat3(&p, DirectX::XMVector3TransformCoord(
                                                          DirectX::XMLoadFloat3(&v.position), root));
                            if (!any) {
                                lo = hi = p;
                                any = true;
                            } else {
                                lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y); lo.z = std::min(lo.z, p.z);
                                hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y); hi.z = std::max(hi.z, p.z);
                            }
                        }
                    }
                    studioAabbCache_[m.uid] = {lo, hi};
                    it = studioAabbCache_.find(m.uid);
                }
                lo = it->second.first;
                hi = it->second.second;
            }
            j["world_aabb"] = BoundsJson(lo, hi);

            if (m.kind == ModelKind::Character && m.inst && m.pmx) {
                const int head = m.pmx->FindBone("\xE9\xA0\xAD");  // 頭
                const int footL = m.pmx->FindBone("\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96");  // 左足首
                const int footR = m.pmx->FindBone("\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96");  // 右足首
                const int center = m.pmx->FindBone("\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC");  // センター
                const int upper = m.pmx->FindBone("\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB");  // 上半身
                float height = hi.y - lo.y;
                float minFootY = 0;
                bool anyFoot = false;
                if (footL >= 0) {
                    minFootY = m.inst->BoneWorldPosition(footL).y;
                    anyFoot = true;
                }
                if (footR >= 0) {
                    const float y = m.inst->BoneWorldPosition(footR).y;
                    minFootY = anyFoot ? std::min(minFootY, y) : y;
                    anyFoot = true;
                }
                if (anyFoot && head >= 0)
                    height = m.inst->BoneWorldPosition(head).y - minFootY;
                j["height"] = Round3(height);
                j["center"] = center >= 0 ? Vec3(m.inst->BoneWorldPosition(center))
                                          : nlohmann::json::array({0, 0, 0});
                const int yawBone = upper >= 0 ? upper : center;
                j["facing_yaw_deg"] = yawBone >= 0 ? Round3(FacingYawDeg(m.inst->BoneWorld(yawBone))) : 0.0;
                j["facing_convention"] = "yaw around +Y; 0 = facing -Z (toward the default MMD camera), +90 = +X";
            }
            models.push_back(std::move(j));
        }
        out["models"] = models;

        nlohmann::json cam;
        const CameraKf ck = StudioViewedCamera(frame);
        cam["key_count"] = d.camera.camera.size();
        if (!d.camera.camera.empty()) {
            cam["first_frame"] = d.camera.camera.front().frame;
            cam["last_frame"] = d.camera.camera.back().frame;
        } else {
            cam["first_frame"] = nullptr;
            cam["last_frame"] = nullptr;
        }
        cam["use_motion_camera"] = d.useMotionCamera;
        cam["target"] = Vec3(ck.target);
        // VMD camera rotation = (pitch, yaw, roll) with the pitch negated for the orbit form
        cam["yaw_deg"] = Round3(DirectX::XMConvertToDegrees(ck.rotation.y));
        cam["pitch_deg"] = Round3(-DirectX::XMConvertToDegrees(ck.rotation.x));
        cam["roll_deg"] = Round3(DirectX::XMConvertToDegrees(ck.rotation.z));
        cam["dist"] = Round3(ck.distance);
        cam["fov_deg"] = Round3((double)ck.fovDeg);
        CameraPose pose;
        pose.target = ck.target;
        pose.rotation = ck.rotation;
        pose.distance = ck.distance;
        pose.fovDeg = (float)ck.fovDeg;
        DirectX::XMFLOAT3 eye;
        CameraMotion::ToView(pose, nullptr, &eye);
        cam["eye"] = Vec3(eye);
        out["camera"] = cam;

        nlohmann::json lights = nlohmann::json::array();
        for (size_t i = 0; i < d.lights.size(); ++i) {
            const SceneLight& l = d.lights[i];
            const LightValues cur = SampleLightValues(l, frame);
            nlohmann::json lj;
            lj["index"] = i;
            lj["uid"] = l.uid;
            lj["kind"] = LightKindName(l.kind);
            lj["name"] = l.name;
            lj["enabled"] = l.enabled;
            lj["base"] = LightValuesJson(l.v);
            lj["sampled"] = LightValuesJson(cur);
            nlohmann::json keyFrames = nlohmann::json::array();
            for (const LightKey& k : l.keys) keyFrames.push_back(k.frame);
            lj["key_frames"] = keyFrames;
            lj["aim_mode"] = AimModeName(l.aimMode);
            lj["target_model_index"] = l.targetUid == 0 ? -1 : d.IndexOfUid(l.targetUid);
            lj["target_uid"] = l.targetUid;
            lj["target_part"] = TargetPartName(l.targetPart);
            lj["sway_phase"] = Round3(l.swayPhase);
            lj["shadow"] = ShadowTypeName(l.shadow);
            lj["shadow_softness"] = Round3(l.shadowSoftness);
            lj["shadow_density"] = Round3(l.shadowDensity);
            lj["shadow_color"] = Vec3(l.shadowColor);
            lj["falloff"] = FalloffTypeName(l.falloff);
            lj["vmd_link"] = l.vmdLink;
            lj["rim_strength"] = Round3(l.rimStrength);
            lj["rim_color"] = Vec3(l.rimColor);
            lj["sky_zenith"] = Vec3(l.skyZenith);
            lj["sky_horizon"] = Vec3(l.skyHorizon);
            lj["ground_color"] = Vec3(l.groundColor);
            lj["affect_diffuse"] = l.affectDiffuse;
            lj["affect_specular"] = l.affectSpecular;
            lj["viewport_visible"] = l.viewportVisible;
            lights.push_back(std::move(lj));
        }
        out["lights"] = lights;
        promise->Resolve(out);
        return true;
    }

    // ---- shared char/stage model pick -------------------------------------
    const auto pickModel = [&](const nlohmann::json& modelArgs, int& index, std::string& error) -> StudioModel* {
        index = modelArgs.value("model", -1);
        if (index < 0 || index >= (int)d.models.size()) {
            error = "Model index out of range: " + std::to_string(index);
            return nullptr;
        }
        StudioModel* m = d.models[(size_t)index].get();
        if (m->IsProp()) {
            error = "Props cannot be sampled (characters and stages only)";
            return nullptr;
        }
        if (!m->inst || !m->pmx) {
            error = "Model has no loaded mesh";
            return nullptr;
        }
        return m;
    };

    // ---- studio_sample_motion ---------------------------------------------
    if (tool == "studio_sample_motion") {
        int index = -1;
        std::string error;
        StudioModel* m = pickModel(args, index, error);
        if (!m) {
            promise->Reject(error);
            return true;
        }
        std::vector<std::string> names;
        if (args.contains("bones") && args["bones"].is_array()) {
            for (const auto& n : args["bones"]) names.push_back(n.get<std::string>());
        } else {
            names = {"\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB",
                     "\xE9\xA0\xAD", "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96", "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96",
                     "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96", "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96"};
        }
        const auto range = MotionRange(m->motion);
        int start = args.value("start", range.first);
        int end = args.value("end", range.second);
        if (end < start) std::swap(start, end);
        int step = args.value("step", 5);
        if (step < 1) step = 1;
        const std::vector<int> frames = SteppedFrames(start, end, step);

        std::vector<int> boneIdx;
        std::vector<std::string> foundNames;
        nlohmann::json missing = nlohmann::json::array();
        for (const std::string& n : names) {
            const int b = m->pmx->FindBone(n);
            if (b < 0) missing.push_back(n);
            else {
                boneIdx.push_back(b);
                foundNames.push_back(n);
            }
        }
        const size_t total = frames.size() * boneIdx.size();
        if (total > (size_t)kSampleCap) {
            promise->Reject("Too many samples: " + std::to_string(total) + " (frame count x bone count, limit " +
                            std::to_string(kSampleCap) + ")");
            return true;
        }
        const auto positions = StudioSampleBones(*m, frames, boneIdx);
        nlohmann::json bones = nlohmann::json::object();
        for (size_t k = 0; k < boneIdx.size(); ++k) {
            nlohmann::json arr = nlohmann::json::array();
            for (size_t f = 0; f < frames.size(); ++f) {
                const DirectX::XMFLOAT3& p = positions[f][k];
                arr.push_back(nlohmann::json::array({Round3(p.x), Round3(p.y), Round3(p.z)}));
            }
            bones[foundNames[k]] = std::move(arr);
        }
        // names that were skipped for a missing bone keep their input order in missing_bones
        nlohmann::json frameArr = nlohmann::json::array();
        for (int f : frames) frameArr.push_back(f);
        promise->Resolve({{"model", index},
                          {"frames", frameArr},
                          {"bones", bones},
                          {"missing_bones", missing}});
        return true;
    }

    // ---- studio_motion_events ---------------------------------------------
    if (tool == "studio_motion_events") {
        std::vector<int> chars;
        if (args.contains("model")) {
            int index = -1;
            std::string error;
            StudioModel* m = pickModel(args, index, error);
            if (!m) {
                promise->Reject(error);
                return true;
            }
            if (m->kind != ModelKind::Character) {
                promise->Reject("studio_motion_events takes characters only");
                return true;
            }
            chars.push_back(index);
        } else {
            for (size_t i = 0; i < d.models.size(); ++i)
                if (d.models[i]->kind == ModelKind::Character) chars.push_back((int)i);
        }
        int step = args.value("step", 2);
        if (step < 1) step = 1;

        const std::vector<std::string> tracked = {
            "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC", "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB", "\xE9\xA0\xAD",
            "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96", "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96", "\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96",
            "\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96"};
        nlohmann::json characters = nlohmann::json::array();
        std::map<int, std::map<int, float>> centerX;  // model index -> sampled frame -> センター x
        for (int ci : chars) {
            StudioModel& m = *d.models[(size_t)ci];
            const auto range = MotionRange(m.motion);
            int start = args.value("start", range.first);
            int end = args.value("end", range.second);
            if (end < start) std::swap(start, end);
            const std::vector<int> frames = SteppedFrames(start, end, step);

            std::vector<int> boneIdx;
            std::vector<int> slot(7, -1);
            for (size_t k = 0; k < tracked.size(); ++k) {
                const int b = m.pmx->FindBone(tracked[k]);
                if (b >= 0) {
                    slot[k] = (int)boneIdx.size();
                    boneIdx.push_back(b);
                }
            }
            std::vector<float> yaw;
            const auto pos = StudioSampleBones(m, frames, boneIdx, &yaw);
            if (frames.empty()) continue;

            const int headSlot = slot[2], centerSlot = slot[0], upperSlot = slot[1];
            const int lhSlot = slot[3], rhSlot = slot[4], lfSlot = slot[5], rfSlot = slot[6];
            if (centerSlot >= 0)  // the formation below reuses these samples (no second pose pass per window)
                for (size_t i = 0; i < frames.size() && i < pos.size(); ++i)
                    centerX[ci][frames[i]] = pos[i][(size_t)centerSlot].x;

            // height from the current-frame AABB/bone positions (same definition as studio_scene_info)
            float height = 1.0f;
            {
                const int head = m.pmx->FindBone("\xE9\xA0\xAD");
                float lo = 0, hi = 0;
                bool any = false;
                for (size_t b = 0; b < m.pmx->bones.size(); ++b) {
                    const float y = m.inst->BoneWorldPosition((int)b).y;
                    if (!any) { lo = hi = y; any = true; }
                    else { lo = std::min(lo, y); hi = std::max(hi, y); }
                }
                height = hi - lo;
                const int fl = m.pmx->FindBone("\xE5\xB7\xA6\xE8\xB6\xB3\xE9\xA6\x96");
                const int fr = m.pmx->FindBone("\xE5\x8F\xB3\xE8\xB6\xB3\xE9\xA6\x96");
                if (head >= 0 && (fl >= 0 || fr >= 0)) {
                    float feet = 1e30f;
                    if (fl >= 0) feet = std::min(feet, m.inst->BoneWorldPosition(fl).y);
                    if (fr >= 0) feet = std::min(feet, m.inst->BoneWorldPosition(fr).y);
                    height = m.inst->BoneWorldPosition(head).y - feet;
                }
                if (height <= 1e-3f) height = 1.0f;
            }

            const size_t n = frames.size();
            const auto centerY = [&](size_t i) { return centerSlot >= 0 ? pos[i][(size_t)centerSlot].y : 0.0f; };
            const auto bonePos = [&](size_t i, int slotIdx) -> DirectX::XMFLOAT3 {
                return slotIdx >= 0 ? pos[i][(size_t)slotIdx] : DirectX::XMFLOAT3{0, 0, 0};
            };

            // 1-second rolling median of the centre height (31 frames, sampled)
            std::vector<float> baseline(n, 0.0f);
            for (size_t i = 0; i < n; ++i) {
                std::vector<float> win;
                for (size_t j = 0; j < n; ++j)
                    if (std::abs(frames[j] - frames[i]) <= kBaselineHalfWindow) win.push_back(centerY(j));
                if (!win.empty()) {
                    std::sort(win.begin(), win.end());
                    baseline[i] = win[win.size() / 2];
                }
            }

            nlohmann::json jumps = nlohmann::json::array();
            nlohmann::json crouches = nlohmann::json::array();
            int lastJump = INT_MIN, lastCrouch = INT_MIN;
            for (size_t i = 1; i + 1 < n; ++i) {
                const float y = centerY(i);
                const float rel = y - baseline[i];
                if (rel > kJumpHeightRatio * height && y >= centerY(i - 1) && y > centerY(i + 1) &&
                    frames[i] - lastJump > kEventMinGap) {
                    jumps.push_back({{"frame", frames[i]}, {"height", Round3(rel)}});
                    lastJump = frames[i];
                }
                if (rel < -kCrouchDepthRatio * height && y <= centerY(i - 1) && y < centerY(i + 1) &&
                    frames[i] - lastCrouch > kEventMinGap) {
                    crouches.push_back({{"frame", frames[i]}, {"depth", Round3(-rel)}});
                    lastCrouch = frames[i];
                }
            }

            // hand raises: wrist above the head
            nlohmann::json handRaises = nlohmann::json::array();
            if (headSlot >= 0 && (lhSlot >= 0 || rhSlot >= 0)) {
                bool active = false;
                int startFrame = 0, endFrame = 0;
                bool usedLeft = false, usedRight = false;
                const auto flush = [&](int endAt) {
                    if (!active) return;
                    handRaises.push_back({{"start", startFrame},
                                          {"end", endAt},
                                          {"hand", usedLeft && usedRight ? "both" : (usedLeft ? "left" : "right")}});
                    active = false;
                };
                for (size_t i = 0; i < n; ++i) {
                    const float headY = bonePos(i, headSlot).y;
                    const bool l = lhSlot >= 0 && bonePos(i, lhSlot).y > headY;
                    const bool r = rhSlot >= 0 && bonePos(i, rhSlot).y > headY;
                    if (l || r) {
                        if (!active) {
                            active = true;
                            startFrame = frames[i];
                            usedLeft = l;
                            usedRight = r;
                        }
                        usedLeft = usedLeft || l;
                        usedRight = usedRight || r;
                        endFrame = frames[i];
                    } else {
                        flush(endFrame);
                    }
                }
                flush(endFrame);
            }

            // turns: facing change > 60 degrees within 1.5 s (45 frames)
            nlohmann::json turns = nlohmann::json::array();
            for (size_t i = 0; i < n;) {
                bool found = false;
                for (size_t j = i + 1; j < n && frames[j] - frames[i] <= kTurnWindow; ++j) {
                    const float delta = NormalizeDeg(yaw[j] - yaw[i]);
                    if (std::abs(delta) > kTurnMinDeg) {
                        turns.push_back({{"start", frames[i]}, {"end", frames[j]}, {"delta_yaw_deg", Round3(delta)}});
                        i = j;
                        found = true;
                        break;
                    }
                }
                if (!found) ++i;
            }

            // speeds (world units per frame), summed over the tracked bones
            // A centre jump of more than half the body height per frame is a teleport (the dance re-places the
            // performer between scenes), not movement: it is reported and left out of speeds / energy / travel speed.
            std::vector<float> speed(n, 0.0f);
            std::vector<char> teleport(n, 0);
            nlohmann::json teleports = nlohmann::json::array();
            for (size_t i = 1; i < n; ++i) {
                const float df = (float)std::max(1, frames[i] - frames[i - 1]);
                if (centerSlot >= 0) {
                    const DirectX::XMFLOAT3 a = pos[i - 1][(size_t)centerSlot], c = pos[i][(size_t)centerSlot];
                    const float jump = std::sqrt((c.x - a.x) * (c.x - a.x) + (c.y - a.y) * (c.y - a.y) +
                                                 (c.z - a.z) * (c.z - a.z));
                    if (jump / df > kTeleportRatio * height) {
                        teleport[i] = 1;
                        teleports.push_back(frames[i]);
                        speed[i] = speed[i - 1];
                        continue;
                    }
                }
                float s = 0.0f;
                for (size_t b = 0; b < boneIdx.size(); ++b) {
                    const DirectX::XMFLOAT3 a = pos[i - 1][b], c = pos[i][b];
                    const float dx = c.x - a.x, dy = c.y - a.y, dz = c.z - a.z;
                    s += std::sqrt(dx * dx + dy * dy + dz * dz) / df;
                }
                speed[i] = s;
            }
            std::vector<float> sortedSpeed(speed.begin() + (n > 1 ? 1 : 0), speed.end());
            float median = 0.0f;
            if (!sortedSpeed.empty()) {
                std::sort(sortedSpeed.begin(), sortedSpeed.end());
                median = sortedSpeed[sortedSpeed.size() / 2];
            }

            nlohmann::json holds = nlohmann::json::array();
            {
                const float threshold = kHoldSpeedRatio * median;
                bool active = false;
                int hs = 0, he = 0;
                for (size_t i = 1; i < n; ++i) {
                    if (speed[i] < threshold) {
                        if (!active) { active = true; hs = frames[i]; }
                        he = frames[i];
                    } else if (active) {
                        if (he - hs >= kHoldMinFrames) holds.push_back({{"start", hs}, {"end", he}});
                        active = false;
                    }
                }
                if (active && he - hs >= kHoldMinFrames) holds.push_back({{"start", hs}, {"end", he}});
            }

            nlohmann::json travel;
            {
                const DirectX::XMFLOAT3 a = bonePos(0, centerSlot);
                const DirectX::XMFLOAT3 z = bonePos(n - 1, centerSlot);
                DirectX::XMFLOAT2 lo{a.x, a.z}, hi{a.x, a.z};
                float maxSpeed = 0.0f;
                for (size_t i = 1; i < n; ++i) {
                    const DirectX::XMFLOAT3 p = bonePos(i, centerSlot);
                    lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.z);
                    hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.z);
                    if (teleport[i]) continue;
                    const DirectX::XMFLOAT3 q = bonePos(i - 1, centerSlot);
                    const float df = (float)std::max(1, frames[i] - frames[i - 1]);
                    maxSpeed = std::max(maxSpeed, std::sqrt((p.x - q.x) * (p.x - q.x) + (p.z - q.z) * (p.z - q.z)) /
                                                   df * 30.0f);
                }
                travel = {{"start_xz", nlohmann::json::array({Round3(a.x), Round3(a.z)})},
                          {"end_xz", nlohmann::json::array({Round3(z.x), Round3(z.z)})},
                          {"bounds_min_xz", Vec2(lo)},
                          {"bounds_max_xz", Vec2(hi)},
                          {"max_speed", Round3(maxSpeed)},
                          {"max_speed_unit", "world units per second"}};
            }

            // energy per 15-frame window, normalised over this character
            nlohmann::json energy = nlohmann::json::array();
            {
                std::vector<std::pair<int, float>> windows;
                std::vector<float> sorted;
                for (int w = start; w <= end; w += kEnergyWindow) {
                    float sum = 0.0f;
                    int count = 0;
                    for (size_t i = 0; i < n; ++i)
                        if (frames[i] >= w && frames[i] < w + kEnergyWindow) { sum += speed[i]; ++count; }
                    const float e = count > 0 ? sum / (float)count : 0.0f;
                    windows.push_back({w, e});
                    sorted.push_back(e);
                }
                // normalised by the 95th percentile (one wild window must not flatten the rest), clamped to 1
                std::sort(sorted.begin(), sorted.end());
                const float maxEnergy = sorted.empty() ? 0.0f : sorted[(size_t)((sorted.size() - 1) * 0.95f)];
                for (const auto& [w, e] : windows) {
                    const float norm = maxEnergy > 1e-6f ? std::min(1.0f, e / maxEnergy) : 0.0f;
                    energy.push_back({{"start", w}, {"end", w + kEnergyWindow - 1}, {"energy", Round3(norm)}});
                }
            }

            characters.push_back({{"model", ci},
                                  {"uid", m.uid},
                                  {"name", m.name},
                                  {"height", Round3(height)},
                                  {"jumps", jumps},
                                  {"teleports", teleports},
                                  {"crouches", crouches},
                                  {"hand_raises", handRaises},
                                  {"turns", turns},
                                  {"holds", holds},
                                  {"travel", travel},
                                  {"energy", energy}});
        }

        nlohmann::json out;
        out["characters"] = characters;
        if (chars.size() >= 2) {
            // common range over every selected character
            int lo = INT_MAX, hi = INT_MIN;
            for (int ci : chars) {
                const auto r = MotionRange(d.models[(size_t)ci]->motion);
                lo = std::min(lo, r.first);
                hi = std::max(hi, r.second);
            }
            nlohmann::json formation = nlohmann::json::array();
            nlohmann::json crossovers = nlohmann::json::array();
            std::vector<int> prevOrder;
            for (int f = lo; f <= hi; f += kFormationWindow) {
                std::vector<std::pair<float, int>> xs;
                for (int ci : chars) {
                    const auto it = centerX.find(ci);
                    if (it == centerX.end() || it->second.empty()) continue;
                    auto s = it->second.lower_bound(f);  // nearest sample at or after f, else the last one
                    if (s == it->second.end()) s = std::prev(s);
                    xs.push_back({s->second, ci});
                }
                std::sort(xs.begin(), xs.end());
                std::vector<int> order;
                nlohmann::json ord = nlohmann::json::array();
                for (const auto& [x, ci] : xs) {
                    order.push_back(ci);
                    ord.push_back(ci);
                }
                formation.push_back({{"frame", f}, {"order", ord}});
                if (!prevOrder.empty()) {
                    for (size_t a = 0; a < order.size(); ++a)
                        for (size_t b = a + 1; b < order.size(); ++b) {
                            const int ia = order[a], ib = order[b];
                            const auto pa = std::find(prevOrder.begin(), prevOrder.end(), ia);
                            const auto pb = std::find(prevOrder.begin(), prevOrder.end(), ib);
                            if (pa != prevOrder.end() && pb != prevOrder.end() && (pa < pb) != (a < b))
                                crossovers.push_back({{"frame", f}, {"a", ia}, {"b", ib}});
                        }
                }
                prevOrder = std::move(order);
            }
            out["formation"] = formation;
            out["crossovers"] = crossovers;
        }
        promise->Resolve(out);
        return true;
    }

    // ---- studio_frame_check -----------------------------------------------
    if (tool == "studio_frame_check") {
        if (!args.contains("frames") || !args["frames"].is_array()) {
            promise->Reject("studio_frame_check requires a 'frames' array");
            return true;
        }
        std::vector<int> frames;
        for (const auto& f : args["frames"]) frames.push_back(f.get<int>());
        if (frames.empty()) {
            promise->Reject("studio_frame_check requires at least one frame");
            return true;
        }

        const bool hasOverride = args.contains("camera") && args["camera"].is_object();
        if (!hasOverride && d.camera.camera.empty()) {
            promise->Reject("The project has no motion camera and no camera override was given");
            return true;
        }
        CameraPose overridePose;
        if (hasOverride) {
            const auto& c = args["camera"];
            if (c.contains("target") && c["target"].is_array() && c["target"].size() >= 3) {
                overridePose.target = {c["target"][0].get<float>(), c["target"][1].get<float>(),
                                       c["target"][2].get<float>()};
            }
            const float yaw = c.value("yaw_deg", 0.0f);
            const float pitch = c.value("pitch_deg", 0.0f);
            const float roll = c.value("roll_deg", 0.0f);
            const float dist = c.value("dist", 45.0f);
            overridePose.rotation = {DirectX::XMConvertToRadians(-pitch), DirectX::XMConvertToRadians(yaw),
                                     DirectX::XMConvertToRadians(roll)};
            overridePose.distance = -dist;
            overridePose.fovDeg = c.value("fov_deg", 30.0f);
        } else if (d.cameraEvalVersion != d.cameraVersion) {
            d.cameraEval = d.camera.camera.empty() ? nullptr : CameraMotion::Create(d.camera.ToVmd());
            d.cameraEvalVersion = d.cameraVersion;
        }

        std::vector<int> chars;
        for (size_t i = 0; i < d.models.size(); ++i)
            if (d.models[i]->kind == ModelKind::Character && d.models[i]->visible &&
                d.models[i]->inst && d.models[i]->pmx)
                chars.push_back((int)i);

        nlohmann::json result = nlohmann::json::array();
        for (int frame : frames) {
            CameraPose pose = hasOverride ? overridePose : d.cameraEval->Evaluate((float)frame);
            result.push_back(McpFrameCheck(frame, pose));
        }
        promise->Resolve({{"frames", result}});
        return true;
    }

    // ---- studio_audio_analysis --------------------------------------------
    if (tool == "studio_audio_analysis") {
        std::string beatsError;
        const BeatAnalysis* analysis = StudioBeats(beatsError);
        if (!analysis) {
            promise->Reject(beatsError);
            return true;
        }
        const BeatAnalysis& a = *analysis;

        int start = args.value("start", INT_MIN);
        int end = args.value("end", INT_MAX);
        if (end < start) std::swap(start, end);
        const auto inRange = [&](double sec) {
            const double f = TimelineFrame(sec, d.audioOffset);
            return f >= (double)start - 1e-6 && f <= (double)end + 1e-6;
        };

        nlohmann::json beats = nlohmann::json::array();
        nlohmann::json beatEnergy = nlohmann::json::array();
        for (size_t i = 0; i < a.beats.size(); ++i) {
            if (!inRange(a.beats[i])) continue;
            beats.push_back(TimelineFrame(a.beats[i], d.audioOffset));
            beatEnergy.push_back(Round3(i < a.beatEnergy.size() ? a.beatEnergy[i] : 0.0f));
        }
        nlohmann::json downbeats = nlohmann::json::array();
        for (double t : a.downbeats)
            if (inRange(t)) downbeats.push_back(TimelineFrame(t, d.audioOffset));

        struct Onset {
            double frame;
            float strength;
        };
        std::vector<Onset> onsets;
        for (size_t i = 0; i < a.onsets.size(); ++i) {
            if (!inRange(a.onsets[i])) continue;
            onsets.push_back({TimelineFrame(a.onsets[i], d.audioOffset),
                              i < a.onsetStrength.size() ? a.onsetStrength[i] : 0.0f});
        }
        if (onsets.size() > 400) {
            std::partial_sort(onsets.begin(), onsets.begin() + 400, onsets.end(),
                              [](const Onset& x, const Onset& y) { return x.strength > y.strength; });
            onsets.resize(400);
        }
        std::sort(onsets.begin(), onsets.end(), [](const Onset& x, const Onset& y) { return x.frame < y.frame; });
        nlohmann::json onsetFrames = nlohmann::json::array();
        nlohmann::json onsetStrength = nlohmann::json::array();
        for (const Onset& o : onsets) {
            onsetFrames.push_back(o.frame);
            onsetStrength.push_back(Round3(o.strength));
        }

        nlohmann::json sections = nlohmann::json::array();
        for (const BeatSection& s : a.sections) {
            const double sf = TimelineFrame(s.start, d.audioOffset);
            const double ef = TimelineFrame(s.end, d.audioOffset);
            if (ef < (double)start - 1e-6 || sf > (double)end + 1e-6) continue;
            sections.push_back({{"start", sf}, {"end", ef}, {"level", s.level}, {"energy", Round3(s.energy)}});
        }

        const double fpb = a.bpm > 1e-6 ? 30.0 * 60.0 / a.bpm : 0.0;
        promise->Resolve({{"bpm", Round3(a.bpm)},
                          {"confidence", Round3(a.confidence)},
                          {"frames_per_beat", Round3(fpb)},
                          {"beats", beats},
                          {"downbeats", downbeats},
                          {"beat_energy", beatEnergy},
                          {"onsets", onsetFrames},
                          {"onset_strength", onsetStrength},
                          {"sections", sections},
                          {"audio_offset_seconds", Round3(d.audioOffset)},
                          {"duration_seconds", Round3(a.duration)}});
        return true;
    }

    return false;
}

}  // namespace mmdx
