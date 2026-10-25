#include "asset/StageMaterials.h"

#include <algorithm>
#include <cmath>

namespace mmdx {

namespace {

// A linear stage colour as the sRGB-encoded constant the shader decodes (PbrAlbedo / PbrEmissive do
// SrgbToLinear(saturate(value)) on a slot without a texture).
float EncodeSrgb(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

DirectX::XMFLOAT3 EncodeSrgb(const DirectX::XMFLOAT3& c) {
    return {EncodeSrgb(c.x), EncodeSrgb(c.y), EncodeSrgb(c.z)};
}

bool SlotHasTexture(const PbrSlot& slot) {
    return slot.tex.kind != PbrTexKind::None && slot.from != 0;
}

// Writes one assignment into a rule's field (the evaluator already checked that the param type fits the target).
void ApplyAssignment(StageMaterialRule& rule, const std::string& field, const StageValue& value) {
    if (field == "baseColor") {
        rule.baseColor = value.AsColor();
    } else if (field == "roughness") {
        rule.roughness = value.AsNumber();
    } else if (field == "metallic") {
        rule.metallic = value.AsNumber();
    } else if (field == "emissive") {
        rule.emissive = DirectX::XMFLOAT3{value.x, value.y, value.z};
    } else if (field == "emissiveStrength") {
        rule.emissiveStrength = value.AsNumber();
    } else if (field == "transmission") {
        rule.transmission = value.AsNumber();
    } else if (field == "ior") {
        rule.ior = value.AsNumber();
    } else if (field == "alpha") {
        rule.alpha = value.AsNumber();
    } else if (field == "mirror") {
        rule.mirror = value.AsNumber() > 0.5f;
    } else if (field == "castShadow") {
        rule.castShadow = value.AsNumber() > 0.5f;
    } else if (field == "receiveShadow") {
        rule.receiveShadow = value.AsNumber() > 0.5f;
    } else if (field == "screen") {
        rule.screen = value.text;
    }
}

// The record a material without PBR data gets: its PMX material through the native colour model. The albedo samples
// the PMX diffuse map (APPLY_DIFFUSE, like every imported / Ray albedo map); a material without a map keeps its diffuse
// colour as the constant. Smoothness / metalness are neutral (0.5 / 0), emissive off.
PbrMaterial MakeStageRecord(const PmxMaterial& pm) {
    PbrMaterial m;
    m.valid = true;
    m.shadingModel = 0;   // DEFAULT
    PbrSlot& albedo = m.slots[(int)PbrSlotId::Albedo];
    if (pm.textureIndex >= 0) {
        albedo.from = 3;
        albedo.tex.kind = PbrTexKind::PmxDiffuse;
        albedo.tex.pmxIndex = pm.textureIndex;
        m.albedoApplyDiffuse = 1;
    } else {
        albedo.from = 0;
        albedo.hasValue = true;
        albedo.value[0] = pm.diffuse.x;
        albedo.value[1] = pm.diffuse.y;
        albedo.value[2] = pm.diffuse.z;
    }
    PbrSlot& rough = m.slots[(int)PbrSlotId::Smoothness];
    rough.from = 0;
    rough.type = 2;   // linear (perceptual) roughness: r = value^2
    rough.hasValue = true;
    rough.value[0] = 0.5f;
    PbrSlot& metal = m.slots[(int)PbrSlotId::Metalness];
    metal.from = 0;
    metal.hasValue = true;
    metal.value[0] = 0.0f;
    return m;
}

// One material's PBR record from one rule: the rule's values on top of whatever record the material already has (an
// imported glTF record keeps its maps; a material with none gets the stage record above).
void ApplyRuleToRecord(const StageMaterialRule& rule, PbrMaterial& m, PmxMaterial& pm) {
    if (rule.baseColor) {
        const DirectX::XMFLOAT4 c = *rule.baseColor;
        const float alpha = std::clamp(c.w, 0.0f, 1.0f);
        pm.diffuse = {c.x, c.y, c.z, alpha};
        PbrSlot& albedo = m.slots[(int)PbrSlotId::Albedo];
        if (SlotHasTexture(albedo)) {
            // the map is tinted by the material colour (ALBEDO_MAP_APPLY_DIFFUSE reads pm.diffuse above)
            m.albedoApplyDiffuse = 1;
            albedo.applyScale = 0;
        } else {
            albedo.from = 0;
            albedo.hasValue = true;
            albedo.applyScale = 0;
            albedo.value[0] = EncodeSrgb(c.x);
            albedo.value[1] = EncodeSrgb(c.y);
            albedo.value[2] = EncodeSrgb(c.z);
            m.albedoApplyDiffuse = 0;
        }
    }
    if (rule.roughness) {
        // The shader reads a SMOOTHNESS map without a per-run factor, so an authored roughness replaces the map.
        PbrSlot& rough = m.slots[(int)PbrSlotId::Smoothness];
        rough = PbrSlot{};
        rough.from = 0;
        rough.type = 2;   // linear roughness (glTF / stage convention)
        rough.hasValue = true;
        rough.value[0] = std::clamp(*rule.roughness, 0.0f, 1.0f);
    }
    if (rule.metallic) {
        PbrSlot& metal = m.slots[(int)PbrSlotId::Metalness];
        metal = PbrSlot{};
        metal.from = 0;
        metal.hasValue = true;
        metal.value[0] = std::clamp(*rule.metallic, 0.0f, 1.0f);
    }
    if (rule.emissive || rule.emissiveStrength) {
        DirectX::XMFLOAT3 e{0.0f, 0.0f, 0.0f};
        if (rule.emissive) e = *rule.emissive;
        const float strength = rule.emissiveStrength ? std::max(0.0f, *rule.emissiveStrength) : 1.0f;
        // the emitted colour already includes the strength: a rule that only raises the strength of an existing
        // emissive keeps that colour, a rule that sets the colour scales it
        const bool on = e.x > 0.0f || e.y > 0.0f || e.z > 0.0f;
        m.emissiveEnable = on;
        PbrSlot& em = m.slots[(int)PbrSlotId::Emissive];
        if (SlotHasTexture(em)) {
            em.applyScale = 1;   // linear factor on the decoded map
            em.hasValue = true;
            em.value[0] = e.x * strength;
            em.value[1] = e.y * strength;
            em.value[2] = e.z * strength;
        } else {
            em.from = 0;
            em.applyScale = 0;
            em.hasValue = true;
            em.value[0] = EncodeSrgb(e.x * strength);
            em.value[1] = EncodeSrgb(e.y * strength);
            em.value[2] = EncodeSrgb(e.z * strength);
        }
    }
    if (rule.alpha) {
        const float a = std::clamp(*rule.alpha, 0.0f, 1.0f);
        pm.diffuse.w = a;
    }
    // castShadow / receiveShadow are the PMX material flags (see ApplyStageMaterialRules).
}

} // namespace

std::vector<StageMaterialRule> ResolvedStageMaterialRules(const StageManifest& manifest, const StageEvalResult& eval) {
    std::vector<StageMaterialRule> rules = manifest.materials;
    for (const StageAssignment& a : eval.targets) {
        const StageTargetSpec spec = ResolveStageTarget(manifest, a.target);
        if (!spec.ok || spec.kind != StageTargetSpec::Kind::Materials) continue;   // light / screen / ... targets
        for (StageMaterialRule& r : rules)
            if (r.match == spec.owner) {
                ApplyAssignment(r, spec.field, a.value);
                break;
            }
    }
    return rules;
}

StageMaterialBinding MatchStageMaterialRules(const std::vector<StageMaterialRule>& rules, const PmxModel& pmx) {
    StageMaterialBinding binding(rules.size());
    for (size_t r = 0; r < rules.size(); ++r) {
        const std::string& pattern = rules[r].match;
        for (size_t i = 0; i < pmx.materials.size(); ++i) {
            const PmxMaterial& m = pmx.materials[i];
            if (StageGlobMatch(pattern, m.name) ||
                (!m.nameEn.empty() && StageGlobMatch(pattern, m.nameEn)))
                binding[r].push_back(i);
        }
    }
    return binding;
}

StageMaterialRule StageEffectiveMaterialValues(const std::vector<StageMaterialRule>& rules,
                                               const StageMaterialBinding& binding, size_t material) {
    StageMaterialRule out;
    for (size_t r = 0; r < rules.size() && r < binding.size(); ++r) {
        if (std::find(binding[r].begin(), binding[r].end(), material) == binding[r].end()) continue;
        const StageMaterialRule& src = rules[r];
        if (!out.baseColor && src.baseColor) out.baseColor = src.baseColor;
        if (!out.roughness && src.roughness) out.roughness = src.roughness;
        if (!out.metallic && src.metallic) out.metallic = src.metallic;
        if (!out.emissive && src.emissive) out.emissive = src.emissive;
        if (!out.emissiveStrength && src.emissiveStrength) out.emissiveStrength = src.emissiveStrength;
        if (!out.transmission && src.transmission) out.transmission = src.transmission;
        if (!out.ior && src.ior) out.ior = src.ior;
        if (!out.alpha && src.alpha) out.alpha = src.alpha;
        if (!out.mirror && src.mirror) out.mirror = src.mirror;
        if (!out.screen && src.screen) out.screen = src.screen;
        if (!out.castShadow && src.castShadow) out.castShadow = src.castShadow;
        if (!out.receiveShadow && src.receiveShadow) out.receiveShadow = src.receiveShadow;
    }
    return out;
}

bool StageRuleSetsPbr(const StageMaterialRule& rule) {
    return rule.baseColor || rule.roughness || rule.metallic || rule.emissive || rule.emissiveStrength;
}

void ApplyStageMaterialRules(const std::vector<StageMaterialRule>& rules, const StageMaterialBinding& binding,
                             PmxModel& pmx, PbrModelData& pbr) {
    if (rules.empty() || pmx.materials.empty()) return;
    // An imported part (glTF / GLB) carries its PBR data on the PmxModel: build the records it would get with the PBR
    // material source, so a stage's own materials are the source of truth here (an MMDX stage is authored in PBR).
    if (pbr.materials.empty() && !pmx.importedPbr.empty()) BuildImportedPbrModelData(pmx, pbr);
    if (pbr.materials.size() < pmx.materials.size()) pbr.materials.resize(pmx.materials.size());

    int records = pbr.materialsWithRecord;
    for (size_t i = 0; i < pmx.materials.size(); ++i) {
        const StageMaterialRule v = StageEffectiveMaterialValues(rules, binding, i);
        const bool matched = v.baseColor || v.roughness || v.metallic || v.emissive || v.emissiveStrength ||
                             v.transmission || v.ior || v.alpha || v.mirror || v.screen || v.castShadow ||
                             v.receiveShadow;
        if (!matched) continue;
        PmxMaterial& pm = pmx.materials[i];
        if (StageRuleSetsPbr(v) && !pbr.materials[i].valid) {
            pbr.materials[i] = MakeStageRecord(pm);
            ++records;
        }
        if (pbr.materials[i].valid) ApplyRuleToRecord(v, pbr.materials[i], pm);
        else if (v.baseColor) pm.diffuse = *v.baseColor;
        else if (v.alpha) pm.diffuse.w = std::clamp(*v.alpha, 0.0f, 1.0f);
        if (v.castShadow || v.receiveShadow) {
            if (v.castShadow) {
                pm.flags = (uint8_t)(*v.castShadow ? (pm.flags | PmxMat_CastShadow)
                                                   : (pm.flags & ~(PmxMat_CastShadow | PmxMat_GroundShadow)));
            }
            if (v.receiveShadow) {
                pm.flags = (uint8_t)(*v.receiveShadow ? (pm.flags | PmxMat_ReceiveShadow)
                                                      : (pm.flags & ~PmxMat_ReceiveShadow));
            }
        }
    }
    pbr.materialsWithRecord = records;
}

void ApplyStageMaterialRules(const StageManifest& manifest, const std::string& variant,
                             const StageParamValues& overrides, double timeSeconds,
                             const StageMaterialBinding& binding, PmxModel& pmx, PbrModelData& pbr) {
    if (manifest.materials.empty()) return;
    const StageEvalResult eval = EvaluateStage(manifest, variant, overrides, timeSeconds);
    ApplyStageMaterialRules(ResolvedStageMaterialRules(manifest, eval), binding, pmx, pbr);
}

} // namespace mmdx
