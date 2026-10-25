#pragma once
// MMDX stage runtime (docs/stage_format.md, docs/stage_ecosystem.md §3): one loaded stage kept together with the parts
// it was loaded into, so play mode and the studio evaluate it every frame with `EvaluateStage` and hand the result to
// the renderer:
//   - `lights.<name>.<field>` assignments go onto a copy of the manifest's lights, which is what `BuildSceneLighting`
//     resolves (the light rig with its cues; play mode's "스테이지 조명").
//   - `materials.<rule>.<field>` assignments become `GpuModel::SetPbrOverrides` for the materials the rule matches.
// Screen / pack / environment targets are ignored until their milestones (no warnings).
//
// The rule -> material binding is resolved once (at load, `MatchStageMaterialRules`); per frame the evaluator plus a diff
// runs and GpuModel::SetPbrOverrides skips ring entries that already hold the values.
#include "asset/StageEval.h"
#include "asset/StageManifest.h"
#include "asset/StageMaterials.h"
#include "render/GpuModel.h"
#include "studio/SceneLight.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mmdx {

// One loaded stage part: its GPU model plus the rule -> material binding of its materials.
struct StageRuntimePart {
    int part = 0;                    // the part's index in StageManifest::models order
    GpuModel* gpu = nullptr;         // not owned (the scene / the studio document owns it)
    StageMaterialBinding binding;    // parallel to the manifest's `materials`; empty = no override for this part
};

class StageRuntime {
public:
    // `manifest` must outlive the runtime. `variantId` empty = the manifest's default variant.
    void Init(std::shared_ptr<const StageManifest> manifest, std::string variantId, std::vector<StageRuntimePart> parts);

    bool Valid() const { return manifest_ != nullptr; }
    const StageManifest* Manifest() const { return manifest_.get(); }
    const std::string& Variant() const { return variant_; }
    // The stage has lights: play mode's lighting picker defaults to "스테이지 조명" for it.
    bool HasLights() const {
        return manifest_ && (!manifest_->lights.empty() || (!gltfLights_.empty() && manifest_->gltfLights));
    }
    bool Empty() const { return !manifest_; }

    // Evaluates the stage at `timeSeconds` (song time) for GPU frame `frame` and applies the results: the light targets
    // into Lights(), the material targets into the parts' GPU models (only when a value changed).
    const std::vector<studio::SceneLight>& Apply(double timeSeconds, uint64_t frame);

    // S2a: the parts' glTF punctual lights (asset/StageLights.h PartGltfLights), appended to the rig when the manifest
    // opts in with "gltfLights": true. Called once at load; empty = none.
    void SetGltfLights(std::vector<studio::SceneLight> lights) { gltfLights_ = std::move(lights); }

    // The rig at the last Apply (the manifest's lights with the targets applied, plus the glTF lights when enabled;
    // empty when the stage has none).
    const std::vector<studio::SceneLight>& Lights() const { return lights_; }

private:
    void ApplyMaterials(const std::vector<StageMaterialRule>& rules, uint64_t frame);

    std::shared_ptr<const StageManifest> manifest_;
    std::string variant_;   // empty = the manifest's default variant
    std::vector<StageRuntimePart> parts_;
    std::vector<studio::SceneLight> lights_;
    std::vector<studio::SceneLight> gltfLights_;   // S2a KHR_lights_punctual of the parts (manifest.gltfLights)
    uint64_t appliedFrame_ = ~0ull;   // the frame `lights_` / the material targets were last applied for
};

// Replays one `lights.<name>.<field>` assignment onto a scene light (the value is already remapped / scaled). False
// for a field a binding cannot drive. The studio and the runtime share it: a stage's rig lights in the studio are
// ordinary scene lights, tagged with the stage they came from (SceneLight::stageUid / stageLightName).
bool ApplyStageLightField(studio::SceneLight& l, const std::string& field, const StageValue& v);
// The manifest light fields any binding drives for the light called `name` (`lights.<name>.<field>`), i.e. the fields
// the studio shows read-only on that light.
std::set<std::string> StageDrivenLightFields(const StageManifest& manifest, const std::string& name);

// Applies one part's resolved rule values to its GPU model (`GpuModel::SetPbrOverrides`, changed values only). The
// studio and play mode share it; a manifest's rules come from `ResolvedStageMaterialRules(manifest, eval)`.
void ApplyStageMaterialList(GpuModel& gpu, int part, const StageMaterialBinding& binding,
                            const std::vector<StageMaterialRule>& rules, uint64_t frame);

// Studio: one stage part in the per-frame material pass (the studio's light rig is copied into the scene lights when
// the stage is added, so only the material targets run there).
struct StageModelOverridePart {
    int part = 0;                              // the part's index in the manifest's models
    GpuModel* gpu = nullptr;
    const StageMaterialBinding* binding = nullptr;
};
// Applies one manifest's material targets at `timeSeconds` to `parts` (studio per-frame pass).
void ApplyStageMaterialTargets(const StageManifest& manifest, const std::string& variant, double timeSeconds,
                               uint64_t frame, const std::vector<StageModelOverridePart>& parts);

// The variant a stage shows: `cliVariant` (--stage-variant) wins, then the saved per-stage choice, then the manifest's
// default variant (an empty manifest default = the first variant / the plain defaults).
std::string ResolveStageVariant(const std::string& manifestDefault, const std::string& saved,
                                const std::string& cliVariant);

} // namespace mmdx
