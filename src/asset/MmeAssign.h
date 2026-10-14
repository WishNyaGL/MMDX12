#pragma once
// M1: which effect files a model / accessory file asks for. The sources are MME's own conventions, highest-priority
// first: a same-stem `.emd` sidecar (per-object `Obj` and per-subset `Obj[n]`, with `Obj.show` visibility), the
// same-name `.fx` beside the model, and the `name[effect.fx].pmx` embedding. Nothing here applies the effect; it
// only reports what the asset files ask for (docs/mme/research/mme_spec_draft.md §2.2–§2.3).
#include <filesystem>
#include <vector>

namespace mmdx {

struct MmeAssignment {
    std::filesystem::path effect;   // the effect file the entry names
    int subset = -1;                // -1 = whole object, >= 0 = PMX material index
    bool hidden = false;            // Obj.show = false / (hide): the object is hidden
};

// Reads an `.emd`'s [Effect] section: `Obj`, `Obj[n]`, `Obj.show = false`. Values `none` / `(none)` mean "no
// effect" and are skipped; relative paths resolve against the .emd's folder (both `\` and `/`).
std::vector<MmeAssignment> ReadEmd(const std::filesystem::path& emd);

// The effects a model / accessory file asks for: `<stem>.emd` beside it if present, else same-name `<stem>.fx`,
// else a `name[effect.fx].pmx` embedding (effect resolved in the model's folder). Empty when none applies.
std::vector<MmeAssignment> FindModelEffects(const std::filesystem::path& model);

} // namespace mmdx
