#include "app/StageRuntime.h"

#include <algorithm>
#include <cstring>

namespace mmdx {

namespace {

// Replays one `lights.<name>.<field>` assignment onto the rig light (`v` is the already-remapped value).
bool AssignLightField(studio::SceneLight& l, const std::string& field, const StageValue& v) {
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
                    AssignLightField(l, spec.field, a.value);
                    break;
                }
        }
    } else {
        lights_.clear();
    }

    // Materials: the manifest's rules plus the targets. Called every frame: SetPbrOverrides writes only ring entry
    // frame % 3 and skips it when that entry already holds this version, so the GPU is still touched only on changes,
    // and every ring entry catches up (gating here on a changed hash left the other entries stale / in flight).
    ApplyMaterials(ResolvedStageMaterialRules(*manifest_, eval), frame);
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

} // namespace mmdx
