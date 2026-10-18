// Read-only MCP tool studio_motion_quality: diagnoses stiff motion from the motion TRACKS (SampleBone of each bone's
// local rotation / translation per frame; no posing). It reports linear interpolation, dead holds, hard stops,
// synchronized stops, chain lag and a 0..100 stiffness score with the worst windows.
#include "app/App.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "studio/StudioDoc.h"
#include "studio/StudioMotion.h"

namespace mmdx {

using namespace studio;

namespace {

const char* const kHead = "\xE9\xA0\xAD";                                       // 頭
const char* const kNeck = "\xE9\xA6\x96";                                       // 首
const char* const kUpper = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB";              // 上半身
const char* const kUpper2 = "\xE4\xB8\x8A\xE5\x8D\x8A\xE8\xBA\xAB\x32";         // 上半身2
const char* const kLower = "\xE4\xB8\x8B\xE5\x8D\x8A\xE8\xBA\xAB";              // 下半身
const char* const kCenter = "\xE3\x82\xBB\xE3\x83\xB3\xE3\x82\xBF\xE3\x83\xBC"; // センター
const char* const kGroove = "\xE3\x82\xB0\xE3\x83\xAB\xE3\x83\xBC\xE3\x83\x96"; // グルーブ
const char* const kLeftShoulder = "\xE5\xB7\xA6\xE8\x82\xA9";                    // 左肩
const char* const kLeftArm = "\xE5\xB7\xA6\xE8\x85\x95";                         // 左腕
const char* const kLeftElbow = "\xE5\xB7\xA6\xE3\x81\xB2\xE3\x81\x98";           // 左ひじ
const char* const kRightShoulder = "\xE5\x8F\xB3\xE8\x82\xA9";                   // 右肩
const char* const kRightArm = "\xE5\x8F\xB3\xE8\x85\x95";                        // 右腕
const char* const kRightElbow = "\xE5\x8F\xB3\xE3\x81\xB2\xE3\x81\x98";          // 右ひじ
const char* const kLeftWrist = "\xE5\xB7\xA6\xE6\x89\x8B\xE9\xA6\x96";           // 左手首
const char* const kRightWrist = "\xE5\x8F\xB3\xE6\x89\x8B\xE9\xA6\x96";          // 右手首
const char* const kLeftLegIk = "\xE5\xB7\xA6\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB";    // 左足ＩＫ
const char* const kRightLegIk = "\xE5\x8F\xB3\xE8\xB6\xB3\xEF\xBC\xA9\xEF\xBC\xAB";   // 右足ＩＫ
const char* const kLeftLeg = "\xE5\xB7\xA6\xE8\xB6\xB3";                        // 左足
const char* const kRightLeg = "\xE5\x8F\xB3\xE8\xB6\xB3";                       // 右足

constexpr int kHoldMinFrames = 15;
constexpr float kHoldDegPerFrame = 0.15f;
constexpr float kHoldUnitsPerFrame = 0.01f;
constexpr float kHardFromDeg = 2.0f;
constexpr float kHardToDeg = 0.2f;
constexpr int kHardWindow = 2;

double Round3(double v) { return std::round(v * 1000.0) / 1000.0; }

float QAngleDeg(const DirectX::XMFLOAT4& a, const DirectX::XMFLOAT4& b) {
    float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    d = std::clamp(d, 0.0f, 1.0f);
    return DirectX::XMConvertToDegrees(2.0f * std::acos(d));
}

bool IsLinear(const uint8_t c[4]) {
    return std::abs((int)c[0] - (int)c[1]) <= 2 && std::abs((int)c[2] - (int)c[3]) <= 2;
}

bool UsesTranslation(const std::string& name) {
    return name == kCenter || name == kGroove || name.find("\xEF\xBC\xA9\xEF\xBC\xAB") != std::string::npos;
}

std::pair<int, int> MotionKeyRange(const MotionData& m) {
    int lo = INT_MAX, hi = INT_MIN;
    for (const auto& [name, keys] : m.bones)
        for (const BoneKf& k : keys) {
            lo = std::min(lo, k.frame);
            hi = std::max(hi, k.frame);
        }
    for (const auto& [name, keys] : m.morphs)
        for (const MorphKf& k : keys) {
            lo = std::min(lo, k.frame);
            hi = std::max(hi, k.frame);
        }
    if (lo == INT_MAX) return {0, 0};
    return {lo, hi};
}

struct GroupDef {
    const char* name;
    std::vector<const char*> bones;
};

struct GroupResult {
    std::vector<std::string> bones;          // present bones
    std::vector<float> speed;                // per frame: normalized hold speed (1 = at the hold threshold)
    std::vector<float> angSpeed;             // per frame: max angular speed (deg/frame)
    float linearRatio = 0.0f;
    float holdRatio = 0.0f;
    std::vector<std::pair<int, int>> holds;
    std::vector<int> hardStops;
    float jerk = 0.0f;
    float stiffness = 0.0f;
};

// Angular speed series of one bone (deg between consecutive local rotations), and translation speed (units/frame).
void BoneSpeeds(const std::vector<BoneKf>& keys, int start, int end, std::vector<float>& ang,
                std::vector<float>& trans) {
    const int n = end - start + 1;
    ang.assign((size_t)n, 0.0f);
    trans.assign((size_t)n, 0.0f);
    if (keys.empty()) return;
    BoneKf prev = SampleBone(keys, start);
    for (int f = start; f < end; ++f) {
        BoneKf next = SampleBone(keys, f + 1);
        ang[(size_t)(f - start)] = QAngleDeg(prev.r, next.r);
        const float dx = prev.t.x - next.t.x, dy = prev.t.y - next.t.y, dz = prev.t.z - next.t.z;
        trans[(size_t)(f - start)] = std::sqrt(dx * dx + dy * dy + dz * dz);
        prev = next;
    }
}

// Best lag in [-6, 6] of `child` behind `parent` (cross-correlation of the angular-speed series).
int BestLag(const std::vector<float>& parent, const std::vector<float>& child) {
    int best = 0;
    double bestScore = -1e300;
    for (int lag = -6; lag <= 6; ++lag) {
        double sum = 0, sa = 0, sb = 0;
        int n = 0;
        for (int i = 0; i < (int)parent.size(); ++i) {
            const int j = i - lag;
            if (j < 0 || j >= (int)child.size()) continue;
            sum += (double)parent[(size_t)i] * child[(size_t)j];
            sa += (double)parent[(size_t)i] * parent[(size_t)i];
            sb += (double)child[(size_t)j] * child[(size_t)j];
            ++n;
        }
        if (n < 8 || sa < 1e-8 || sb < 1e-8) continue;
        const double score = sum / std::sqrt(sa * sb);
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    return best;
}

double Clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

}  // namespace

bool App::ExecuteMcpMotionQuality(const std::string& tool, const nlohmann::json& args,
                                  std::shared_ptr<McpPromise> promise) {
    if (tool != "studio_motion_quality") return false;
    if (!promise) return false;
    if (!studio_) {
        promise->Reject("Studio is not active");
        return true;
    }
    StudioDoc& d = *studio_;
    if (!args.contains("model") || !args["model"].is_number_integer()) {
        promise->Reject("studio_motion_quality requires an integer 'model'");
        return true;
    }
    const int modelIndex = args["model"].get<int>();
    if (modelIndex < 0 || modelIndex >= (int)d.models.size()) {
        promise->Reject("Model index out of range: " + std::to_string(modelIndex));
        return true;
    }
    StudioModel& m = *d.models[(size_t)modelIndex];
    if (m.kind != ModelKind::Character) {
        promise->Reject("studio_motion_quality requires a character model");
        return true;
    }
    if (!m.pmx) {
        promise->Reject("The model has no PMX data");
        return true;
    }

    auto [keyLo, keyHi] = MotionKeyRange(m.motion);
    int start = args.value("start", keyLo);
    int end = args.value("end", keyHi);
    if (end < start) std::swap(start, end);
    start = std::max(0, start);
    const int window = std::max(1, args.value("window", 60));
    const int rangeLen = std::max(1, end - start + 1);

    const GroupDef defs[] = {
        {"head", {kNeck, kHead}},
        {"torso", {kUpper, kUpper2, kLower}},
        {"center", {kCenter, kGroove}},
        {"arm_l", {kLeftShoulder, kLeftArm, kLeftElbow}},
        {"arm_r", {kRightShoulder, kRightArm, kRightElbow}},
        {"hand_l", {kLeftWrist}},
        {"hand_r", {kRightWrist}},
        {"legs", {kLeftLegIk, kRightLegIk, kLeftLeg, kRightLeg}},
    };

    std::map<std::string, GroupResult> groups;
    std::map<std::string, std::vector<float>> groupSpeed;

    for (const GroupDef& def : defs) {
        GroupResult gr;
        gr.speed.assign((size_t)rangeLen, 0.0f);
        gr.angSpeed.assign((size_t)rangeLen, 0.0f);
        int keysInRange = 0, linearKeys = 0;
        bool any = false;
        for (const char* bone : def.bones) {
            const int idx = m.pmx->FindBone(bone);
            if (idx < 0) continue;
            const auto it = m.motion.bones.find(bone);
            if (it == m.motion.bones.end()) continue;
            any = true;
            gr.bones.push_back(bone);
            const std::vector<BoneKf>& keys = it->second;
            std::vector<float> ang, trans;
            BoneSpeeds(keys, start, end, ang, trans);
            const bool useTrans = UsesTranslation(bone);
            for (int f = start; f <= end; ++f) {
                const size_t i = (size_t)(f - start);
                const float t = (i < trans.size()) ? trans[i] : 0.0f;
                gr.angSpeed[i] = std::max(gr.angSpeed[i], ang[i]);
                const float moveSpeed = t / kHoldUnitsPerFrame;
                gr.speed[i] = std::max(gr.speed[i], std::max(ang[i] / kHoldDegPerFrame, moveSpeed));
            }
            for (const BoneKf& k : keys) {
                if (k.frame < start || k.frame > end) continue;
                ++keysInRange;
                uint8_t rot[4];
                GetBoneCurve(k.interp, 3, rot);
                bool linear = IsLinear(rot);
                if (useTrans && linear) {
                    for (int ch = 0; ch < 3 && linear; ++ch) {
                        uint8_t c[4];
                        GetBoneCurve(k.interp, ch, c);
                        linear = IsLinear(c);
                    }
                }
                if (linear) ++linearKeys;
            }
        }
        (void)any;
        gr.linearRatio = keysInRange > 0 ? (float)linearKeys / (float)keysInRange : 0.0f;

        // holds: runs where every bone turns < 0.15 deg/frame and moves < 0.01 units/frame
        int runStart = -1;
        int holdFrames = 0;
        for (int f = start; f <= end + 1; ++f) {
            const bool still = f <= end && gr.speed[(size_t)(f - start)] < 1.0f;
            if (still && runStart < 0) runStart = f;
            if (!still && runStart >= 0) {
                const int len = f - runStart;
                if (len >= kHoldMinFrames) {
                    gr.holds.push_back({runStart, f - 1});
                    holdFrames += len;
                }
                runStart = -1;
            }
        }
        gr.holdRatio = (float)holdFrames / (float)rangeLen;

        // hard stops: speed drops from > 2 to < 0.2 within 2 frames
        int primeCount = 0;
        for (int f = start; f <= end; ++f) {
            const float s0 = gr.angSpeed[(size_t)(f - start)];
            if (s0 <= kHardFromDeg) continue;
            for (int d = 1; d <= kHardWindow && f + d <= end; ++d) {
                if (gr.angSpeed[(size_t)(f + d - start)] < kHardToDeg) {
                    gr.hardStops.push_back(f + d);
                    ++primeCount;
                    break;
                }
            }
        }
        // jerk: mean absolute change of angular speed at key frames
        {
            double sum = 0;
            int n = 0;
            for (const char* bone : def.bones) {
                const auto it = m.motion.bones.find(bone);
                if (it == m.motion.bones.end()) continue;
                for (const BoneKf& k : it->second) {
                    if (k.frame <= start || k.frame > end) continue;
                    const float a = std::fabs(gr.angSpeed[(size_t)(k.frame - 1 - start)] -
                                              gr.angSpeed[(size_t)(k.frame - start)]);
                    sum += a;
                    ++n;
                }
            }
            gr.jerk = n > 0 ? (float)(sum / n) : 0.0f;
        }
        groups[def.name] = std::move(gr);
    }

    // sync stops: frames where >= 3 groups hard-stop within +-1
    std::vector<std::pair<int, std::vector<std::string>>> syncStops;
    for (int f = start; f <= end; ++f) {
        std::vector<std::string> hit;
        for (const auto& [name, gr] : groups) {
            for (int hs : gr.hardStops)
                if (std::abs(hs - f) <= 1) {
                    hit.push_back(name);
                    break;
                }
        }
        if (hit.size() >= 3) {
            if (!syncStops.empty() && f - syncStops.back().first <= 1) continue;
            syncStops.push_back({f, hit});
        }
    }

    // chain lag: mean of the adjacent best lags
    struct ChainDef {
        const char* name;
        std::vector<const char*> bones;
    };
    const ChainDef chains[] = {
        {"upper_body", {kUpper2, kNeck, kHead}},
        {"arm_l", {kLeftArm, kLeftElbow, kLeftWrist}},
        {"arm_r", {kRightArm, kRightElbow, kRightWrist}},
    };
    nlohmann::json chainLag = nlohmann::json::object();
    std::map<std::string, float> chainLagValue;
    for (const ChainDef& chain : chains) {
        std::vector<std::vector<float>> series;
        for (const char* bone : chain.bones) {
            const auto it = m.motion.bones.find(bone);
            if (it == m.motion.bones.end()) continue;
            std::vector<float> ang, trans;
            BoneSpeeds(it->second, start, end, ang, trans);
            series.push_back(std::move(ang));
        }
        if (series.size() < 2) {
            chainLag[chain.name] = nullptr;
            continue;
        }
        double sum = 0;
        for (size_t i = 1; i < series.size(); ++i) sum += (double)BestLag(series[i - 1], series[i]);
        const float lag = (float)(-sum / (double)(series.size() - 1));
        chainLag[chain.name] = Round3(lag);
        chainLagValue[chain.name] = lag;
    }

    // stiffness per group. Documented formula below (see docs/mcp.md):
    //   100 * (0.35*linear_ratio + 0.25*hold_ratio + 0.20*hard_rate + 0.20*lag_deficit)
    const auto groupChainLag = [&](const std::string& group) -> float {
        if (group == "head" || group == "torso") return chainLagValue.count("upper_body") ? chainLagValue["upper_body"] : 0.0f;
        if (group == "arm_l" || group == "hand_l") return chainLagValue.count("arm_l") ? chainLagValue["arm_l"] : 0.0f;
        if (group == "arm_r" || group == "hand_r") return chainLagValue.count("arm_r") ? chainLagValue["arm_r"] : 0.0f;
        return -1.0f;
    };

    const double seconds = (double)rangeLen / 30.0;
    double groupSum = 0;
    int groupCount = 0;
    nlohmann::json groupsJson = nlohmann::json::object();
    for (auto& [name, gr] : groups) {
        const float hardRate = (float)Clamp01(gr.hardStops.size() / std::max(1e-3, seconds) / 2.0);
        float lagDeficit;
        const float lag = groupChainLag(name);
        if (lag >= 0.0f) {
            lagDeficit = (float)Clamp01(1.0 - lag / 2.0);
        } else {
            int involved = 0;
            for (const auto& s : syncStops)
                for (const std::string& g : s.second)
                    if (g == name) { involved = 1; break; }
            lagDeficit = involved ? 1.0f : 0.0f;
        }
        gr.stiffness = 100.0f * (0.35f * gr.linearRatio + 0.25f * gr.holdRatio + 0.20f * hardRate +
                                 0.20f * lagDeficit);
        groupSum += gr.stiffness;
        ++groupCount;

        nlohmann::json holds = nlohmann::json::array();
        for (const auto& h : gr.holds) holds.push_back({{"start", h.first}, {"end", h.second}});
        nlohmann::json stops = nlohmann::json::array();
        for (int f : gr.hardStops) stops.push_back(f);
        nlohmann::json bones = nlohmann::json::array();
        for (const std::string& b : gr.bones) bones.push_back(b);
        groupsJson[name] = {{"bones", bones},
                            {"linear_ratio", Round3(gr.linearRatio)},
                            {"hold_ratio", Round3(gr.holdRatio)},
                            {"holds", holds},
                            {"hard_stops", stops},
                            {"jerk", Round3(gr.jerk)},
                            {"stiffness", Round3(gr.stiffness)}};
    }

    const double meanGroup = groupCount > 0 ? groupSum / groupCount : 0.0;
    const double syncRate = Clamp01((double)syncStops.size() / std::max(1.0, seconds / 10.0));
    const double overall = std::clamp(0.85 * meanGroup + 15.0 * syncRate, 0.0, 100.0);

    // worst windows: sliding windows of `window` frames, step = window / 2
    struct RangeScore {
        int start, end;
        double score;
        std::vector<std::string> reasons;
    };
    std::vector<RangeScore> ranges;
    const int slide = std::max(1, window / 2);
    for (int ws = start; ws <= end; ws += slide) {
        const int we = std::min(end, ws + window - 1);
        if (we - ws + 1 < std::min(window, rangeLen)) break;
        double linSum = 0;
        int linGroups = 0;
        int holdFrames = 0;
        int stops = 0;
        for (const auto& [name, gr] : groups) {
            // linear ratio of keys inside the window (recomputed from the group's bones)
            int keys = 0, linear = 0;
            for (const std::string& b : gr.bones) {
                const auto it = m.motion.bones.find(b);
                if (it == m.motion.bones.end()) continue;
                for (const BoneKf& k : it->second) {
                    if (k.frame < ws || k.frame > we) continue;
                    ++keys;
                    uint8_t rot[4];
                    GetBoneCurve(k.interp, 3, rot);
                    bool ok = IsLinear(rot);
                    if (UsesTranslation(b) && ok)
                        for (int ch = 0; ch < 3 && ok; ++ch) {
                            uint8_t c[4];
                            GetBoneCurve(k.interp, ch, c);
                            ok = IsLinear(c);
                        }
                    if (ok) ++linear;
                }
            }
            if (keys > 0) {
                linSum += (double)linear / keys;
                ++linGroups;
            }
            for (const auto& h : gr.holds)
                holdFrames += std::max(0, std::min(we, h.second) - std::max(ws, h.first) + 1);
            for (int hs : gr.hardStops)
                if (hs >= ws && hs <= we) ++stops;
        }
        const double lin = linGroups > 0 ? linSum / linGroups : 0.0;
        const double hold = Clamp01((double)holdFrames / (double)(we - ws + 1) / std::max(1, (int)groups.size()));
        int syncIn = 0;
        for (const auto& s : syncStops)
            if (s.first >= ws && s.first <= we) ++syncIn;
        const double score = std::clamp(100.0 * (0.45 * lin + 0.25 * hold + 0.20 * Clamp01(stops / 2.0) +
                                                 0.10 * Clamp01(syncIn)), 0.0, 100.0);
        std::vector<std::string> reasons;
        if (lin >= 0.5) reasons.push_back("linear curves");
        if (hold > 0.2) reasons.push_back("dead hold");
        if (stops > 0) reasons.push_back("hard stop" + std::to_string(stops));
        for (const auto& s : syncStops)
            if (s.first >= ws && s.first <= we) {
                reasons.push_back("sync stop at " + std::to_string(s.first));
                break;
            }
        if (reasons.empty()) reasons.push_back("stiff window");
        ranges.push_back({ws, we, score, reasons});
    }
    std::sort(ranges.begin(), ranges.end(),
              [](const RangeScore& a, const RangeScore& b) { return a.score > b.score; });
    if ((int)ranges.size() > 5) ranges.resize(5);
    nlohmann::json worst = nlohmann::json::array();
    for (const RangeScore& r : ranges) {
        nlohmann::json reasons = nlohmann::json::array();
        for (const std::string& s : r.reasons) reasons.push_back(s);
        worst.push_back({{"start", r.start}, {"end", r.end}, {"score", Round3(r.score)}, {"reasons", reasons}});
    }

    nlohmann::json syncJson = nlohmann::json::array();
    for (const auto& [frame, hit] : syncStops) {
        nlohmann::json g = nlohmann::json::array();
        for (const std::string& s : hit) g.push_back(s);
        syncJson.push_back({{"frame", frame}, {"groups", g}});
    }

    promise->Resolve({{"model", modelIndex},
                      {"range", {{"start", start}, {"end", end}}},
                      {"overall", Round3(overall)},
                      {"groups", groupsJson},
                      {"sync_stops", syncJson},
                      {"chain_lag", chainLag},
                      {"worst_ranges", worst}});
    return true;
}

}  // namespace mmdx
