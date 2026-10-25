#pragma once
// MMDX stage material rules (docs/stage_format.md, "materials"): the load-time application to one loaded model part
// (its PBR records and PMX material flags) and the rule -> material binding the runtime reuses every frame.
//
// One implementation resolves "the first matching rule wins per field" (`StageEffectiveMaterialValues`): the loader
// applies it before the GPU upload, app/StageRuntime.cpp every frame for the evaluator's `materials.<rule>.<field>`
// assignments. transmission / ior / mirror / screen are carried by the manifest for later milestones and have no
// effect here.
#include "asset/PbrMaterial.h"
#include "asset/StageEval.h"
#include "asset/StageManifest.h"

#include <cstdint>
#include <vector>

namespace mmdx {

// The manifest's `materials` rules with `eval`'s `materials.<rule>.<field>` assignments applied on top, in assignment
// order. A target the evaluator emitted for a rule that no longer exists is skipped.
std::vector<StageMaterialRule> ResolvedStageMaterialRules(const StageManifest& manifest, const StageEvalResult& eval);

// For each rule (parallel to `rules`): the indices of `pmx`'s materials whose name matches the rule's glob. Rules
// keep their manifest order; a glob may match nothing (stage_check reports it).
using StageMaterialBinding = std::vector<std::vector<size_t>>;
StageMaterialBinding MatchStageMaterialRules(const std::vector<StageMaterialRule>& rules, const PmxModel& pmx);

// The effective values of one material: for every field the first rule of `rules` that sets it wins. Fields no rule
// sets stay unset. `binding` is parallel to `rules`.
StageMaterialRule StageEffectiveMaterialValues(const std::vector<StageMaterialRule>& rules,
                                              const StageMaterialBinding& binding, size_t material);

// True when `rule` sets at least one field that needs a PBR record (shadow flags and the plain opacity do not).
bool StageRuleSetsPbr(const StageMaterialRule& rule);

// Applies `rules` / `binding` to a loaded part before its GPU upload: a material a rule matches gets a PBR record
// (an existing one is kept) with the rule's values, the record's alpha / the PMX material's diffuse alpha, and the
// cast / receive shadow flags. Materials no rule matches are untouched. The linear stage colours are written the way
// the shader reads the record's slots (sRGB-encoded constants, linear factors on a sampled map).
void ApplyStageMaterialRules(const std::vector<StageMaterialRule>& rules, const StageMaterialBinding& binding,
                             PmxModel& pmx, PbrModelData& pbr);
// `eval` is evaluated with `variant` / `overrides` at `timeSeconds`; the convenience the loaders use (they pass empty
// overrides and 0 s: a stage's material targets are applied every frame by the runtime, only the variant matters here).
void ApplyStageMaterialRules(const StageManifest& manifest, const std::string& variant,
                             const StageParamValues& overrides, double timeSeconds, const StageMaterialBinding& binding,
                             PmxModel& pmx, PbrModelData& pbr);

} // namespace mmdx
