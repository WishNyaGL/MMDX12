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

// The effect file's required external controllers: the names (lower case, as written) of its CONTROLOBJECT
// parameters that are a scalar / vector / matrix read (type not bool) and whose `name` annotation is not `(self)`
// or `(OffscreenOwner)`, after preprocessing (macros resolved, includes followed). A bool CONTROLOBJECT is an
// existence check, never required. Cached by path + file write time. An unreadable / unparseable file yields {}.
std::vector<std::string> MmeRequiredControllers(const std::filesystem::path& fx);

// Finds the file of a required CONTROLOBJECT controller (`controllerName` is the lower-case name
// MmeRequiredControllers returned) so the Auto workflow can load it automatically. Search roots, in order: the
// model's folder, then the folders of the model's Auto effects (FindModelEffects), each recursively (depth <= 4).
// A match is a file name equal to `controllerName` (case-insensitive), or — when `controllerName` carries no
// extension — a file whose stem equals it with extension .pmx / .pmd / .x. A Ray-MMD file is refused
// (IsRayMmdControllerFile, one LOG_INFO per file). Cached by (model path, controller); an empty result (not
// found) is cached too. Never throws; an empty path means not found.
std::filesystem::path FindMmeControllerFile(const std::filesystem::path& modelPath,
                                            const std::string& controllerName);

// The automatic controller-load Ray-MMD guard: true when `file` (or the requested lower-case `controllerName`
// itself) classifies as a Ray-MMD controller (ClassifyMmeController) or is a Ray-MMD main effect
// (IsRayMmdMainEffect). One rule for every automatic load: Ray-MMD files stay with the native PBR.
bool IsRayMmdControllerFile(const std::filesystem::path& file, const std::string& controllerName = {});

} // namespace mmdx
