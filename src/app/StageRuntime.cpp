#include "app/StageRuntime.h"
#include "asset/StageLights.h"
#include "render/RenderTypes.h"
#include "render/ShaderPack.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

namespace mmdx {

namespace {

uint64_t HashMix(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

uint64_t HashFloat(uint64_t h, float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    return HashMix(h, bits);
}

uint64_t HashColor(uint64_t h, const DirectX::XMFLOAT4& c) {
    return HashFloat(HashFloat(HashFloat(HashFloat(h, c.x), c.y), c.z), c.w);
}

// The material fields the runtime drives (the rest of a rule is load-time only) as one hash.
uint64_t HashMaterialRules(const std::vector<StageMaterialRule>& rules) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const StageMaterialRule& r : rules) {
        for (unsigned char c : r.match) h = HashMix(h, c);
        if (r.baseColor) h = HashColor(h, *r.baseColor);
        if (r.roughness) h = HashFloat(h, *r.roughness);
        if (r.metallic) h = HashFloat(h, *r.metallic);
        if (r.emissive) h = HashFloat(HashFloat(HashFloat(h, r.emissive->x), r.emissive->y), r.emissive->z);
        if (r.emissiveStrength) h = HashFloat(h, *r.emissiveStrength);
        if (r.transmission) h = HashFloat(h, *r.transmission);
        if (r.ior) h = HashFloat(h, *r.ior);
        if (r.alpha) h = HashFloat(h, *r.alpha);
        if (r.castShadow) h = HashMix(h, *r.castShadow ? 3u : 5u);
        if (r.receiveShadow) h = HashMix(h, *r.receiveShadow ? 7u : 11u);
    }
    return h ? h : 1u;
}

} // namespace

bool ApplyStageLightField(studio::SceneLight& l, const std::string& field, const StageValue& v) {
    const DirectX::XMFLOAT3 vec{v.x, v.y, v.z};
    if (field == "enabled") { l.enabled = v.AsNumber() > 0.5f; return true; }
    if (field == "intensity") { l.v.intensity = v.AsNumber(); return true; }
    if (field == "range") { l.v.range = v.AsNumber(); return true; }
    if (field == "coneOuter") { l.v.coneOuter = v.AsNumber(); return true; }
    if (field == "coneInner") { l.v.coneInner = v.AsNumber(); return true; }
    if (field == "shadowSoftness") { l.shadowSoftness = v.AsNumber(); return true; }
    if (field == "shadowDensity") { l.shadowDensity = v.AsNumber(); return true; }
    if (field == "rimStrength") { l.rimStrength = v.AsNumber(); return true; }
    if (field == "swayPhase") { l.swayPhase = v.AsNumber(); return true; }
    if (field == "color") { l.v.color = vec; return true; }
    if (field == "rimColor") { l.rimColor = vec; return true; }
    if (field == "shadowColor") { l.shadowColor = vec; return true; }
    if (field == "position") { l.v.position = vec; return true; }
    if (field == "aim") { l.v.aim = vec; return true; }
    if (field == "direction") { l.v.direction = vec; return true; }
    if (field == "size") { l.v.size = {v.x, v.y}; return true; }
    if (field == "vmdLink") { l.vmdLink = v.AsNumber() > 0.5f; return true; }
    return false;
}

std::set<std::string> StageDrivenLightFields(const StageManifest& manifest, const std::string& name) {
    std::set<std::string> out;
    if (name.empty()) return out;
    for (const StageBinding& b : manifest.bindings) {
        const StageTargetSpec spec = ResolveStageTarget(manifest, b.target);
        if (!spec.ok || spec.kind != StageTargetSpec::Kind::Lights || spec.owner != name) continue;
        out.insert(spec.field);
    }
    return out;
}

void ApplyStageMaterialList(GpuModel& gpu, int part, const StageMaterialBinding& binding,
                            const std::vector<StageMaterialRule>& rules, uint64_t frame) {
    std::vector<PbrOverride> list;
    const size_t materialCount = gpu.Materials().size();
    for (size_t m = 0; m < materialCount; ++m) {
        const StageMaterialRule v = StageEffectiveMaterialValues(rules, binding, m);
        if (!(v.baseColor || v.roughness || v.metallic || v.emissive || v.emissiveStrength || v.transmission || v.ior ||
              v.alpha || v.castShadow || v.receiveShadow))
            continue;
        PbrOverride o;
        o.material = m;
        if (v.baseColor) o.baseColor = DirectX::XMFLOAT3{v.baseColor->x, v.baseColor->y, v.baseColor->z};
        if (v.roughness) o.roughness = *v.roughness;
        if (v.metallic) o.metallic = *v.metallic;
        if (v.emissive) {
            const float strength = v.emissiveStrength ? std::max(0.0f, *v.emissiveStrength) : 1.0f;
            o.emissive = DirectX::XMFLOAT3{v.emissive->x * strength, v.emissive->y * strength,
                                           v.emissive->z * strength};
        }
        if (v.alpha) o.alpha = *v.alpha;
        if (v.transmission) o.transmission = *v.transmission;
        if (v.ior) o.ior = *v.ior;
        if (v.castShadow) o.castShadow = *v.castShadow;
        if (v.receiveShadow) o.receiveShadow = *v.receiveShadow;
        list.push_back(std::move(o));
    }
    // one version per part (the values differ per material): the rule hash mixed with the part index
    gpu.SetPbrOverrides(frame, list, HashMix(HashMaterialRules(rules), (uint64_t)part + 1));
}

void StageRuntime::Init(std::shared_ptr<const StageManifest> manifest, std::string variantId,
                        std::vector<StageRuntimePart> parts) {
    manifest_ = std::move(manifest);
    variant_ = std::move(variantId);
    parts_ = std::move(parts);
    appliedFrame_ = ~0ull;
    lights_.clear();
    screens_.clear();
}

const std::vector<studio::SceneLight>& StageRuntime::Apply(double timeSeconds, uint64_t frame) {
    if (!manifest_) {
        lights_.clear();
        return lights_;
    }
    // UpdateScene applies the material targets (before the model's UpdateMaterials) and BuildFrameView reads the rig in
    // the same frame: evaluate once per frame.
    if (appliedFrame_ == frame) return lights_;
    appliedFrame_ = frame;
    const StageEvalResult eval = EvaluateStage(*manifest_, variant_, StageParamValues{}, timeSeconds);

    // Lights: the manifest's rig (plus the parts' glTF lights when "gltfLights") with the evaluator's light targets
    // replayed in assignment order (cues animate the params the targets read, so the rig follows the song).
    if (!manifest_->lights.empty() || (!gltfLights_.empty() && manifest_->gltfLights)) {
        lights_ = manifest_->lights;
        if (manifest_->gltfLights) lights_.insert(lights_.end(), gltfLights_.begin(), gltfLights_.end());
        for (const StageAssignment& a : eval.targets) {
            const StageTargetSpec spec = ResolveStageTarget(*manifest_, a.target);
            if (!spec.ok || spec.kind != StageTargetSpec::Kind::Lights) continue;   // screens / packs / env: ignored
            for (studio::SceneLight& l : lights_)
                if (l.name == spec.owner) {
                    ApplyStageLightField(l, spec.field, a.value);
                    break;
                }
        }
    } else {
        lights_.clear();
    }
    // S9: environment.fogVolumes become FogVolume lights of the rig (static: they are placement, not cues). They are
    // appended after the manifest's lights so a light index binding keeps its meaning.
    for (size_t i = 0; i < manifest_->environment.fogVolumes.size(); ++i)
        lights_.push_back(StageFogVolumeLight(manifest_->environment.fogVolumes[i], i));

    // Materials: the manifest's rules plus the targets. Called every frame: SetPbrOverrides writes only ring entry
    // frame % 3 and skips it when that entry already holds this version, so the GPU is still touched only on changes,
    // and every ring entry catches up (gating here on a changed hash left the other entries stale / in flight).
    ApplyMaterials(ResolvedStageMaterialRules(*manifest_, eval), frame);
    // S5: the screens (their sources / brightness / enabled targets). Cheap: a few strings per frame.
    screens_ = ResolveStageScreens(*manifest_, eval);
    return lights_;
}

void StageRuntime::ApplyMaterials(const std::vector<StageMaterialRule>& rules, uint64_t frame) {
    for (StageRuntimePart& part : parts_) {
        if (!part.gpu) continue;
        ApplyStageMaterialList(*part.gpu, part.part, part.binding, rules, frame);
    }
}

void ApplyStageMaterialTargets(const StageManifest& manifest, const std::string& variant, double timeSeconds,
                               uint64_t frame, const std::vector<StageModelOverridePart>& parts) {
    const StageEvalResult eval = EvaluateStage(manifest, variant, {}, timeSeconds);
    const std::vector<StageMaterialRule> rules = ResolvedStageMaterialRules(manifest, eval);
    for (const StageModelOverridePart& p : parts) {
        if (!p.gpu || !p.binding) continue;
        ApplyStageMaterialList(*p.gpu, p.part, *p.binding, rules, frame);
    }
}

std::string ResolveStageVariant(const std::string& manifestDefault, const std::string& saved,
                                const std::string& cliVariant) {
    if (!cliVariant.empty()) return cliVariant;
    if (!saved.empty()) return saved;
    return manifestDefault;
}

std::vector<StageScreenState> ResolveStageScreens(const StageManifest& manifest, const StageEvalResult& eval,
                                                  const std::function<void(StageScreenState&)>& edit) {
    std::vector<StageScreenState> out;
    out.reserve(manifest.screens.size());
    for (const StageScreen& s : manifest.screens) {
        StageScreenState st;
        st.id = s.id;
        st.source = s.source;
        st.file = s.file;
        st.camera = s.camera;
        st.width = s.width;
        st.height = s.height;
        st.brightness = s.brightness;
        st.enabled = true;
        if (edit) edit(st);
        out.push_back(std::move(st));
    }
    // S6: the program pack of each screen. A `packs[]` entry of role "screen" naming it wins over the screen's own
    // `file` (the same pack folder can be named by either).
    const std::vector<StagePackState> packs = ResolveStagePacks(manifest, eval);
    for (StageScreenState& st : out) {
        for (const StagePackState& p : packs) {
            if (p.role != StagePackRole::Screen || manifest.packs[(size_t)p.index].screen != st.id) continue;
            st.pack = p.id;
            st.params = p.params;
        }
    }
    // the manifest's `screens[].source == "program"` + `file` = the pack folder: same id, unless a packs[] entry won
    for (StageScreenState& st : out) {
        if (!st.pack.empty()) continue;
        const StageScreen* src = manifest.FindScreen(st.id);
        if (!src || src->source != StageScreenSource::Program || src->file.empty()) continue;
        st.pack = StagePackId(manifest.id, src->file);
        // the folder's parameters, when a packs[] entry names that same folder (any role)
        for (size_t i = 0; i < manifest.packs.size(); ++i)
            if (manifest.packs[i].folder == src->file) {
                for (const StagePackState& p : packs)
                    if (p.index == (int)i) st.params = p.params;
                break;
            }
    }
    // a screen that shows a program pack but whose pack is not a registered "screen" pack draws black: the runtime has
    // nothing to sample. Not an error here: the loader warned once per broken pack.
    for (StageScreenState& st : out) {
        if (st.source == StageScreenSource::Program && st.pack.empty()) st.pack.clear();
    }
    // the evaluator's screen targets (in assignment order): the last one per field wins
    for (const StageAssignment& a : eval.targets) {
        const StageTargetSpec spec = ResolveStageTarget(manifest, a.target);
        if (!spec.ok || spec.kind != StageTargetSpec::Kind::Screens) continue;
        for (StageScreenState& st : out) {
            if (st.id != spec.owner) continue;
            if (spec.field == "brightness") st.brightness = a.value.AsNumber();
            else if (spec.field == "enabled") st.enabled = a.value.AsNumber() > 0.5f;
            else if (spec.field == "width") st.width = (uint32_t)std::max(1.0f, std::round(a.value.AsNumber()));
            else if (spec.field == "height") st.height = (uint32_t)std::max(1.0f, std::round(a.value.AsNumber()));
            else if (spec.field == "source") {
                StageScreenSource src = StageScreenSource::None;
                if (ParseStageScreenSource(a.value.text, src)) st.source = src;
            } else if (spec.field == "file") st.file = a.value.text;
            else if (spec.field == "camera") st.camera = a.value.text;
            break;
        }
    }
    return out;
}

// ---- S6 bundled packs ---------------------------------------------------------------------------------------------

std::vector<StagePackState> ResolveStagePacks(const StageManifest& manifest, const StageEvalResult& eval) {
    std::vector<StagePackState> out;
    if (manifest.packs.empty()) return out;   // zero cost: no packs, no state
    out.reserve(manifest.packs.size());
    for (size_t i = 0; i < manifest.packs.size(); ++i) {
        StagePackState st;
        st.index = (int)i;
        st.id = StagePackId(manifest.id, manifest.packs[i].folder);
        st.role = manifest.packs[i].role;
        st.params = manifest.packs[i].params;
        out.push_back(std::move(st));
    }
    for (const StageAssignment& a : eval.targets) {
        const StageTargetSpec spec = ResolveStageTarget(manifest, a.target);
        if (!spec.ok || spec.kind != StageTargetSpec::Kind::PackParams) continue;
        for (StagePackState& p : out) {
            if (p.index != spec.packIndex) continue;
            p.params[spec.field] = a.value.AsNumber();
            break;
        }
    }
    return out;
}

void RegisterStagePacks(const StageManifest& manifest) {
    for (const StagePackRef& ref : manifest.packs) {
        const std::string id = StagePackId(manifest.id, ref.folder);
        // the folder is relative to the stage folder and SafeRelativeFile rejected ".." at parse time
        ShaderPacks().RegisterStagePack(id, manifest.dir / Utf8ToPath(ref.folder));
    }
}

StageSurfacePack ResolveStageSurfacePack(const StageManifest& manifest, const GpuModel& part) {
    StageSurfacePack out;
    if (manifest.packs.empty()) return out;
    // the first surface pack with a glob that hits one of this part's materials
    const StagePackRef* chosen = nullptr;
    for (const StagePackRef& ref : manifest.packs) {
        if (ref.role != StagePackRole::Surface) continue;
        for (size_t m = 0; m < part.Materials().size() && !chosen; ++m) {
            const std::string& name = part.MaterialName(m);
            const std::string& nameEn = part.MaterialNameEn(m);
            for (const std::string& pattern : ref.materials)
                if (StageGlobMatch(pattern, name) || (!nameEn.empty() && StageGlobMatch(pattern, nameEn))) {
                    chosen = &ref;
                    break;
                }
        }
        if (chosen) break;
    }
    if (!chosen) return out;
    out.id = StagePackId(manifest.id, chosen->folder);
    // every material the pack's globs do not claim keeps the default shading (kPackClassOff); the claimed ones get the
    // pack's own class rules (ShaderPack::ClassifyStage)
    for (size_t m = 0; m < part.Materials().size(); ++m) {
        const std::string& name = part.MaterialName(m);
        const std::string& nameEn = part.MaterialNameEn(m);
        bool hit = false;
        for (const std::string& pattern : chosen->materials)
            if (StageGlobMatch(pattern, name) || (!nameEn.empty() && StageGlobMatch(pattern, nameEn))) {
                hit = true;
                break;
            }
        if (!hit) out.materials[name] = kMaterialPackOff;
    }
    return out;
}

void ApplyStageSurfacePacks(const std::vector<StagePackState>& packs, const std::vector<StageRuntimePart>& parts,
                            const std::function<StageSurfacePack(int part)>& surfaceOf) {
    if (parts.empty()) return;
    const ShaderPack* chosen = nullptr;
    const StagePackState* state = nullptr;
    for (const StagePackState& p : packs) {
        if (p.role != StagePackRole::Surface) continue;
        const ShaderPack* pack = ShaderPacks().Find(p.id);
        if (!pack || pack->type != PackType::Surface || !pack->Selectable()) continue;
        chosen = pack;
        state = &p;
        break;
    }
    if (!chosen) return;   // no (usable) surface pack: every part keeps the default shading
    for (const StageRuntimePart& part : parts) {
        if (!part.gpu) continue;
        const StageSurfacePack sp = surfaceOf(part.part);
        if (sp.id != chosen->id) continue;
        part.gpu->SetShaderPack(chosen, state->params, {}, sp.materials, state->params);
    }
}

namespace {

// The socket's placement: its node bone's world matrix (when it names one) times the explicit offset below. Row-vector
// convention, like the rest of the engine.
DirectX::XMFLOAT4X4 StageSocketMatrix(const StageSocket& socket, const DirectX::XMFLOAT4X4* nodeWorld) {
    constexpr float kRad = 0.01745329252f;
    const DirectX::XMMATRIX offset =
        DirectX::XMMatrixScaling(socket.scale.x, socket.scale.y, socket.scale.z) *
        DirectX::XMMatrixRotationRollPitchYaw(socket.rotationDeg.x * kRad, socket.rotationDeg.y * kRad,
                                              socket.rotationDeg.z * kRad) *
        DirectX::XMMatrixTranslation(socket.position.x, socket.position.y, socket.position.z);
    const DirectX::XMMATRIX world =
        nodeWorld ? DirectX::XMLoadFloat4x4(nodeWorld) * offset : offset;
    DirectX::XMFLOAT4X4 out;
    DirectX::XMStoreFloat4x4(&out, world);
    return out;
}

} // namespace

void AppendStageWorldPacks(const StageManifest& manifest, const std::vector<StagePackState>& packs, double time,
                           bool exact, const std::shared_ptr<const WorldColliderTrack>& colliders, uint32_t uidBase,
                           const std::function<bool(const std::string& node, DirectX::XMFLOAT4X4& out)>& resolveNode,
                           std::vector<WorldInstance>& out) {
    if (manifest.packs.empty()) return;   // zero cost
    for (const StagePackState& p : packs) {
        if (p.role != StagePackRole::World) continue;
        const StagePackRef& ref = manifest.packs[(size_t)p.index];
        const StageSocket* socket = nullptr;
        for (const StageSocket& s : manifest.sockets)
            if (s.name == ref.socket) {
                socket = &s;
                break;
            }
        if (!socket) continue;   // validation reports it; nothing to place
        const ShaderPack* pack = ShaderPacks().Find(p.id);
        if (!pack || pack->type != PackType::World || !pack->Selectable()) continue;   // one [W] at registration
        DirectX::XMFLOAT4X4 nodeWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const bool haveNode = !socket->node.empty() && resolveNode && resolveNode(socket->node, nodeWorld);
        WorldInstance w;
        w.uid = uidBase + (uint32_t)p.index;
        w.pack = p.id;
        w.transform = StageSocketMatrix(*socket, haveNode ? &nodeWorld : nullptr);
        w.params = p.params;
        w.time = time;
        w.exact = exact;
        w.colliders = colliders;
        out.push_back(std::move(w));
    }
}

CameraParams StageCameraView(const StageCamera& camera, float nearZ, float farZ) {
    CameraParams out;
    const DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&camera.position);
    const DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&camera.target);
    DirectX::XMVECTOR up = DirectX::XMVectorSet(0, 1, 0, 0);
    if (DirectX::XMVectorGetX(DirectX::XMVector3Length(DirectX::XMVectorSubtract(
            DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(target, eye)), up))) < 1e-4f)
        up = DirectX::XMVectorSet(0, 0, 1, 0);
    DirectX::XMStoreFloat4x4(&out.view, DirectX::XMMatrixLookAtLH(eye, target, up));
    DirectX::XMStoreFloat3(&out.eye, eye);
    out.fovYRadians = DirectX::XMConvertToRadians(std::clamp(camera.fov, 1.0f, 170.0f));
    out.nearZ = nearZ;
    out.farZ = farZ;
    return out;
}

std::vector<ScreenView> BuildStageScreenViews(
    const StageManifest& manifest, const std::vector<StageScreenState>& screens,
    const std::vector<StageRuntimePart>& parts,
    const std::function<std::filesystem::path(const std::string&)>& resolveFile,
    const std::function<bool(const std::string&, CameraParams&)>& resolveCamera) {
    std::vector<ScreenView> out;
    for (const StageScreenState& st : screens) {
        const StageScreen* src = manifest.FindScreen(st.id);
        if (!src) continue;
        // the materials this screen draws on: its own globs, plus every rule whose `screen` names it
        std::vector<std::string> patterns = src->materials;
        for (const StageMaterialRule& r : manifest.materials)
            if (r.screen && *r.screen == st.id) patterns.push_back(r.match);
        if (patterns.empty()) continue;
        // the camera resolves once per screen (all of its parts share it)
        CameraParams camera;
        bool haveCamera = false;
        if (st.source == StageScreenSource::Camera && !st.camera.empty() && resolveCamera)
            haveCamera = resolveCamera(st.camera, camera);
        for (const StageRuntimePart& part : parts) {
            if (!part.gpu) continue;
            ScreenView sv;
            sv.modelUid = part.gpu->Uid();
            const size_t count = part.gpu->Materials().size();
            for (size_t m = 0; m < count; ++m) {
                const std::string& name = part.gpu->MaterialName(m);
                const std::string& nameEn = part.gpu->MaterialNameEn(m);
                bool hit = false;
                for (const std::string& p : patterns)
                    if (StageGlobMatch(p, name) || (!nameEn.empty() && StageGlobMatch(p, nameEn))) {
                        hit = true;
                        break;
                    }
                if (hit) sv.materials.push_back((uint32_t)m);
            }
            if (sv.materials.empty()) continue;
            sv.key = st.id + "#" + std::to_string(part.part);
            sv.source = st.source;
            sv.width = st.width;
            sv.height = st.height;
            sv.brightness = st.brightness;
            sv.enabled = st.enabled;
            sv.camera = camera;
            sv.pack = st.pack;
            sv.params = st.params;
            if (st.source == StageScreenSource::Camera && !haveCamera) sv.source = StageScreenSource::None;
            if ((st.source == StageScreenSource::Image || st.source == StageScreenSource::Video) && resolveFile)
                sv.path = resolveFile(st.file);
            out.push_back(std::move(sv));
        }
    }
    return out;
}

} // namespace mmdx
