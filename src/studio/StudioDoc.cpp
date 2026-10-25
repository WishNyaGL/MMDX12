#include "studio/StudioDoc.h"
#include "anim/ModelInstance.h"
#include "render/GpuModel.h"
#include <algorithm>
#include <DirectXMath.h>

namespace mmdx::studio {

StudioModel::StudioModel() = default;
StudioModel::~StudioModel() = default;

void StudioModel::BuildRowGroups() {
    // Mirrors StudioRebuildRows: display frame g lists items; unlisted bones go to group frames.size(), morphs to + 1.
    const uint32_t frames = (uint32_t)pmx->displayFrames.size();
    boneRowGroup.assign(pmx->bones.size(), frames);
    morphRowGroup.assign(pmx->morphs.size(), frames + 1);
    std::vector<char> boneSet(pmx->bones.size(), 0), morphSet(pmx->morphs.size(), 0);
    for (uint32_t g = 0; g < frames; ++g) {
        for (const PmxDisplayFrame::Item& it : pmx->displayFrames[g].items) {
            if (it.index < 0) continue;
            const size_t i = (size_t)it.index;
            if (it.morph) {
                if (i < morphSet.size() && !morphSet[i]) { morphSet[i] = 1; morphRowGroup[i] = g; }
            } else if (i < boneSet.size() && !boneSet[i]) {
                boneSet[i] = 1;
                boneRowGroup[i] = g;
            }
        }
    }
}

uint64_t CanonicalRow(const StudioModel& m, RowKind kind, uint32_t index) {
    if (kind == RowKind::Bone && index < m.boneRowGroup.size()) return MakeRowId(kind, m.boneRowGroup[index], index);
    if (kind == RowKind::Morph && index < m.morphRowGroup.size()) return MakeRowId(kind, m.morphRowGroup[index], index);
    return 0;
}

PropAttach AccessoryPlacement(const StudioModel& m, float frame) {
    if (m.motion.accessory.empty()) return m.attach;
    const AccessoryKf k = SampleAccessory(m.motion.accessory, frame);
    PropAttach a;
    a.parent = k.parentUid == 0 ? -1 : (int)k.parentUid;
    a.bone = k.bone;
    a.translation = k.translation;
    a.rotationDeg = k.rotationDeg;
    a.scale = k.scale;
    return a;
}

float AccessoryOpacity(const StudioModel& m, float frame) {
    if (m.motion.accessory.empty()) return 1.0f;
    return SampleAccessory(m.motion.accessory, frame).opacity;
}

bool AccessoryVisible(const StudioModel& m, float frame) {
    if (m.motion.accessory.empty()) return true;
    return SampleAccessory(m.motion.accessory, frame).visible;
}

int StudioDoc::EndFrame() const {
    int end = std::max(camera.EndFrame(), (int)std::ceil(audioEndFrame));
    for (const auto& m : models) end = std::max(end, m->motion.EndFrame());
    for (const StudioCamera& c : cameras) end = std::max(end, c.track.EndFrame());
    for (const CameraSwitchKey& k : cameraSwitch) end = std::max(end, k.frame);
    return std::max(end, 300);
}

int StudioDoc::IndexOfUid(uint32_t uid) const {
    for (size_t i = 0; i < models.size(); ++i)
        if (models[i]->uid == uid) return (int)i;
    return -1;
}

void StudioDoc::TouchModel(int model) {
    if (model < 0) ++cameraVersion;
    else if (model < (int)models.size()) ++models[model]->motionVersion;
    rowsKey = ~0ull;  // timeline rows show keys: rebuild
}

void StudioDoc::TouchCamera(uint32_t uid) {
    // Every camera edit bumps the shared `cameraVersion`: the row fills and the evaluators are rebuilt from it, and
    // there is at most a handful of cameras, so per-camera version counters would not pay for their bookkeeping.
    (void)uid;
    ++cameraVersion;
    rowsKey = ~0ull;
}

const std::vector<CameraKf>& StudioDoc::CameraKeys(uint32_t uid) const {
    static const std::vector<CameraKf> kNone;
    if (uid == 0) return camera.camera;
    const StudioCamera* c = FindCamera(uid);
    return c ? c->track.camera : kNone;
}

void StudioDoc::RefreshCameraEvals() {
    if (cameraEvalVersion == cameraVersion) return;
    cameraEval = camera.camera.empty() ? nullptr : CameraMotion::Create(camera.ToVmd());
    cameraEvals.assign(cameras.size(), nullptr);
    for (size_t i = 0; i < cameras.size(); ++i) {
        if (cameras[i].track.camera.empty()) continue;
        MotionData camOnly;  // an extra camera carries camera keys only
        camOnly.camera = cameras[i].track.camera;
        cameraEvals[i] = CameraMotion::Create(camOnly.ToVmd());
    }
    cameraEvalVersion = cameraVersion;
}

const std::shared_ptr<CameraMotion>& StudioDoc::ActiveCameraEval(int frame) const {
    static const std::shared_ptr<CameraMotion> kNone;
    const uint32_t uid = ActiveCameraUidAt(frame);
    if (uid != 0 && cameraEvals.size() == cameras.size()) {
        for (size_t i = 0; i < cameras.size(); ++i)
            if (cameras[i].uid == uid && cameraEvals[i]) return cameraEvals[i];
    }
    if (cameraEval) return cameraEval;
    // the active camera has no keys of its own: with a switch track, fall back to any camera that has keys (a switch
    // track is meant to be seen, so camera view never ends up with nothing to look through); without one the view stays
    // free, exactly as before S4 (the extra cameras are just parked overheads).
    if (!cameraSwitch.empty() && cameraEvals.size() == cameras.size())
        for (const std::shared_ptr<CameraMotion>& eval : cameraEvals)
            if (eval) return eval;
    return kNone;
}

bool StudioDoc::HasCameraKeys() const {
    if (!camera.camera.empty()) return true;
    for (const StudioCamera& c : cameras)
        if (!c.track.camera.empty()) return true;
    return false;
}

const std::shared_ptr<CameraMotion>& StudioDoc::SelectedCameraEval() const {
    static const std::shared_ptr<CameraMotion> kNone;
    if (selectedCameraUid == 0) return cameraEval;
    for (size_t i = 0; i < cameras.size(); ++i)
        if (cameras[i].uid == selectedCameraUid && cameraEvals.size() == cameras.size()) return cameraEvals[i];
    return kNone;
}

uint32_t StudioDoc::ActiveCameraUidAt(int frame) const {
    if (cameraSwitch.empty()) return 0;
    // stepped: the last switch key at or before the frame (before the first key the main camera is active)
    auto it = std::upper_bound(cameraSwitch.begin(), cameraSwitch.end(), frame,
                               [](int f, const CameraSwitchKey& k) { return f < k.frame; });
    if (it == cameraSwitch.begin()) return 0;
    const uint32_t uid = std::prev(it)->uid;
    return FindCamera(uid) ? uid : 0;  // a switch to a removed camera falls back to the main one
}

std::string StudioDoc::CameraLabel(uint32_t uid) const {
    if (uid == 0) return "main";
    const StudioCamera* c = FindCamera(uid);
    return c ? c->name : std::string();
}

TrackState CaptureTrack(StudioDoc& doc, int model, RowKind kind, const std::string& name) {
    TrackState s;
    s.model = model;
    s.kind = kind;
    s.name = name;
    if (kind == RowKind::CameraSwitch) {  // S4: the stepped active-camera switch track (whole-list)
        s.existed = true;
        s.switchKeys = doc.cameraSwitch;
        return s;
    }
    if (IsCameraKind(kind)) {
        s.existed = true;
        if (kind == RowKind::Light) s.lights = doc.camera.light;
        else if (kind == RowKind::Shadow) s.shadows = doc.camera.shadow;
        else if (kind == RowKind::Focus) s.focuses = doc.camera.focus;
        else {
            // S4: the camera row edits the SELECTED camera (uid 0 = the main camera's `doc.camera`)
            s.cameraUid = doc.selectedCameraUid;
            s.cameras = doc.CameraTrack(s.cameraUid).camera;
        }
        return s;
    }
    if (kind == RowKind::SceneLight) {
        s.uid = LightUidOfTrack(name);
        const SceneLight* light = doc.FindLight(s.uid);
        s.existed = light != nullptr;
        if (light) s.lightKeys = light->keys;
        return s;
    }
    if (kind == RowKind::EffectParam) {
        uint32_t effectUid = 0;
        std::string param;
        if (ParseEffectParamTrackName(name, effectUid, param)) {
            s.uid = effectUid;
            const StudioEffect* eff = doc.FindEffect(effectUid);
            if (eff) {
                for (const EffectParamTrack& t : eff->tracks) {
                    if (t.param == param) {
                        s.existed = true;
                        s.effectParamKeys = t.keys;
                        break;
                    }
                }
            }
        }
        return s;
    }
    if (kind == RowKind::WorldParam) {
        uint32_t worldUid = 0;
        std::string param;
        if (ParseWorldParamTrackName(name, worldUid, param)) {
            s.uid = worldUid;
            const StudioWorld* world = doc.FindWorld(worldUid);
            if (world) {
                for (const EffectParamTrack& t : world->fx.tracks) {
                    if (t.param == param) {
                        s.existed = true;
                        s.worldParamKeys = t.keys;
                        break;
                    }
                }
            }
        }
        return s;
    }
    if (kind == RowKind::StageParam) {
        uint32_t stageUid = 0;
        std::string param;
        if (ParseStageParamTrackName(name, stageUid, param)) {
            s.uid = stageUid;
            const StudioStage* stage = doc.FindStage(stageUid);
            if (stage) {
                for (const EffectParamTrack& t : stage->tracks) {
                    if (t.param == param) {
                        s.existed = true;
                        s.stageParamKeys = t.keys;
                        break;
                    }
                }
            }
        }
        return s;
    }
    MotionData& m = doc.models[model]->motion;
    if (kind == RowKind::Bone) {
        auto it = m.bones.find(name);
        s.existed = it != m.bones.end();
        if (s.existed) s.bones = it->second;
        s.staticFlag = m.staticBones.count(name) != 0;
    } else if (kind == RowKind::Accessory) {
        s.existed = !m.accessory.empty();
        s.accessories = m.accessory;
    } else {
        auto it = m.morphs.find(name);
        s.existed = it != m.morphs.end();
        if (s.existed) s.morphs = it->second;
        s.staticFlag = m.staticMorphs.count(name) != 0;
    }
    return s;
}

TrackState CaptureTrack(StudioDoc& doc, const TrackState& before) {
    TrackState s = CaptureTrack(doc, before.model, before.kind, before.name);
    if (s.kind == RowKind::Camera) {   // the camera row's track: always the camera `before` was taken from
        s.cameraUid = before.cameraUid;
        s.cameras = doc.CameraTrack(s.cameraUid).camera;
    }
    return s;
}

void RestoreTrack(StudioDoc& doc, const TrackState& s) {
    if (s.kind == RowKind::CameraSwitch) {
        doc.cameraSwitch = s.switchKeys;
        doc.rowsKey = ~0ull;
        return;
    }
    if (IsCameraKind(s.kind)) {
        if (s.kind == RowKind::Light) doc.camera.light = s.lights;
        else if (s.kind == RowKind::Shadow) doc.camera.shadow = s.shadows;
        else if (s.kind == RowKind::Focus) doc.camera.focus = s.focuses;
        else {
            // S4: a camera whose entity is gone (removed while a later step is undone) is left alone
            if (s.cameraUid != 0 && !doc.FindCamera(s.cameraUid)) return;
            doc.CameraTrack(s.cameraUid).camera = s.cameras;
        }
        doc.TouchCamera(s.cameraUid);
        return;
    }
    if (s.kind == RowKind::SceneLight) {
        if (SceneLight* light = doc.FindLight(s.uid)) {
            light->keys = s.lightKeys;  // an emptied track keeps the light (its base values drive it again)
            doc.rowsKey = ~0ull;
        }
        return;
    }
    if (s.kind == RowKind::EffectParam) {
        uint32_t effectUid = 0;
        std::string param;
        if (ParseEffectParamTrackName(s.name, effectUid, param)) {
            if (StudioEffect* eff = doc.FindEffect(effectUid)) {
                auto it = std::find_if(eff->tracks.begin(), eff->tracks.end(),
                                       [&](const EffectParamTrack& t) { return t.param == param; });
                if (s.effectParamKeys.empty()) {
                    if (it != eff->tracks.end()) eff->tracks.erase(it);
                } else {
                    if (it != eff->tracks.end()) {
                        it->keys = s.effectParamKeys;
                    } else {
                        eff->tracks.push_back({param, s.effectParamKeys});
                    }
                }
                doc.rowsKey = ~0ull;
            }
        }
        return;
    }
    if (s.kind == RowKind::WorldParam) {
        uint32_t worldUid = 0;
        std::string param;
        if (ParseWorldParamTrackName(s.name, worldUid, param)) {
            if (StudioWorld* world = doc.FindWorld(worldUid)) {
                auto it = std::find_if(world->fx.tracks.begin(), world->fx.tracks.end(),
                                       [&](const EffectParamTrack& t) { return t.param == param; });
                if (s.worldParamKeys.empty()) {
                    if (it != world->fx.tracks.end()) world->fx.tracks.erase(it);
                } else {
                    if (it != world->fx.tracks.end()) {
                        it->keys = s.worldParamKeys;
                    } else {
                        world->fx.tracks.push_back({param, s.worldParamKeys});
                    }
                }
                doc.rowsKey = ~0ull;
            }
        }
        return;
    }
    if (s.kind == RowKind::StageParam) {
        uint32_t stageUid = 0;
        std::string param;
        if (ParseStageParamTrackName(s.name, stageUid, param)) {
            if (StudioStage* stage = doc.FindStage(stageUid)) {
                auto it = std::find_if(stage->tracks.begin(), stage->tracks.end(),
                                       [&](const EffectParamTrack& t) { return t.param == param; });
                if (s.stageParamKeys.empty()) {
                    if (it != stage->tracks.end()) stage->tracks.erase(it);
                } else if (it != stage->tracks.end()) {
                    it->keys = s.stageParamKeys;
                } else {
                    stage->tracks.push_back({param, s.stageParamKeys});
                }
                doc.rowsKey = ~0ull;
            }
        }
        return;
    }
    if (s.model >= 0 && s.model < (int)doc.models.size()) {
        MotionData& m = doc.models[s.model]->motion;
        // A track that ends up empty is removed, so an undone insert leaves no empty track behind.
        if (s.kind == RowKind::Bone) {
            if (s.bones.empty()) m.bones.erase(s.name);
            else m.bones[s.name] = s.bones;
            if (s.staticFlag) m.staticBones.insert(s.name);
            else m.staticBones.erase(s.name);
        } else if (s.kind == RowKind::Accessory) {
            m.accessory = s.accessories;
        } else {
            if (s.morphs.empty()) m.morphs.erase(s.name);
            else m.morphs[s.name] = s.morphs;
            if (s.staticFlag) m.staticMorphs.insert(s.name);
            else m.staticMorphs.erase(s.name);
        }
    }
    doc.TouchModel(s.model);  // scene light rows returned above
}

// ---------------------------------------------------------------------------
// S3: studio stage param values
// ---------------------------------------------------------------------------

std::string StageParamBaseKey(const std::string& name, int& comp) {
    comp = -1;
    const size_t hash = name.find('#');
    if (hash == std::string::npos) return name;
    if (hash + 1 >= name.size() || hash + 2 != name.size()) return name;   // "key#0".."key#3" only
    const char c = name[hash + 1];
    if (c < '0' || c > '3') return name;
    comp = c - '0';
    return name.substr(0, hash);
}

namespace {
// The component `comp` of a colour value, or the number for a scalar.
float StageComponent(const StageValue& v, int comp) {
    if (comp < 0) return v.x;
    const DirectX::XMFLOAT4 c = v.AsColor();
    switch (comp) {
    case 0: return c.x;
    case 1: return c.y;
    case 2: return c.z;
    default: return c.w;
    }
}
void SetStageComponent(StageValue& v, int comp, float value) {
    if (comp < 0) { v = StageValue::Number(value); return; }
    DirectX::XMFLOAT4 c = v.AsColor();
    switch (comp) {
    case 0: c.x = value; break;
    case 1: c.y = value; break;
    case 2: c.z = value; break;
    default: c.w = value; break;
    }
    v = StageValue::Color(c);
}
} // namespace

float StageParamStaticValue(const StudioStage& stage, const std::string& name) {
    int comp = -1;
    const std::string key = StageParamBaseKey(name, comp);
    if (stage.manifest) {
        if (const StageParam* p = stage.manifest->FindParam(key)) {
            const auto it = stage.base.find(key);
            const StageValue base = it != stage.base.end() ? it->second : p->DefaultValue();
            return StageComponent(base, comp);
        }
    }
    return 0.0f;
}

StudioStageOverrides StageOverridesAt(const StudioStage& stage, float frame) {
    StudioStageOverrides out;
    out.values = stage.base;
    for (const EffectParamTrack& t : stage.tracks) {
        int comp = -1;
        const std::string key = StageParamBaseKey(t.param, comp);
        out.skipCues.insert(key);   // a keyed param replaces the stage's own cue track
        if (!stage.manifest) continue;
        const StageParam* p = stage.manifest->FindParam(key);
        if (!p) continue;           // a param the manifest no longer declares: the track is kept, unused
        const float value = EvaluateParamTrack(t, frame, StageParamStaticValue(stage, t.param));
        if (p->type == StageParamType::Color) {
            if (comp < 0) continue;   // a colour is keyed per component ("key#0..3")
            auto it = out.values.find(key);
            StageValue v = it != out.values.end() ? it->second : p->DefaultValue();
            SetStageComponent(v, comp, value);
            out.values[key] = ClampStageParamValue(*p, v);
        } else {
            out.values[key] = ClampStageParamValue(*p, StageValue::Number(value));
        }
    }
    for (const std::string& key : stage.cuesOff) out.skipCues.insert(key);
    return out;
}

bool ImportStageCues(StudioStage& stage) {
    if (!stage.manifest || stage.manifest->cues.empty()) return false;
    bool any = false;
    for (const StageCue& cue : stage.manifest->cues) {
        const StageParam* p = stage.manifest->FindParam(cue.param);
        if (!p || cue.keys.empty()) continue;
        // the cue's own curve wins over keys the user had on this param: the cue becomes the keyed track
        stage.tracks.erase(std::remove_if(stage.tracks.begin(), stage.tracks.end(),
                                          [&](const EffectParamTrack& t) {
                                              int comp = -1;
                                              return StageParamBaseKey(t.param, comp) == cue.param;
                                          }),
                           stage.tracks.end());
        const int comps = p->type == StageParamType::Color ? 4 : 1;
        for (int c = 0; c < comps; ++c) {
            EffectParamTrack track;
            track.param = comps == 1 ? cue.param : cue.param + "#" + std::to_string(c);
            for (const StageCueKey& k : cue.keys) {
                const StageValue v = ClampStageParamValue(*p, k.value);
                EffectParamKey key;
                key.frame = k.frame;
                key.value = StageComponent(v, comps == 1 ? -1 : c);
                // an int / bool / enum cue, or a stepped one, holds its keys (as the timeline's stepped keys do)
                key.stepped = cue.stepped || p->type == StageParamType::Int || p->type == StageParamType::Bool ||
                              p->type == StageParamType::Enum;
                UpsertParamKey(track, key);
            }
            stage.tracks.push_back(std::move(track));
        }
        stage.cuesOff.insert(cue.param);
        stage.base.erase(cue.param);   // a stored base value would win over the imported keys at frames without a key
        any = true;
    }
    return any;
}

bool ResetStageParam(StudioStage& stage, const std::string& key) {
    bool changed = stage.base.erase(key) != 0;
    const size_t before = stage.tracks.size();
    stage.tracks.erase(std::remove_if(stage.tracks.begin(), stage.tracks.end(),
                                      [&](const EffectParamTrack& t) {
                                          int comp = -1;
                                          return StageParamBaseKey(t.param, comp) == key;
                                      }),
                       stage.tracks.end());
    changed |= stage.tracks.size() != before;
    changed |= stage.cuesOff.erase(key) != 0;
    return changed;
}

} // namespace mmdx::studio
