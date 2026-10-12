#pragma once
// Per-model PBR material source choice (work unit P5). Kept in its own tiny header so the studio project
// format (studio/StudioProject.h) and the tool tests can carry it without pulling in the PBR / import stack.
//   Default            : no Ray-MMD material data, the model renders exactly as before (choice.ray == false).
//   Ray-MMD            : the model's Ray-MMD material data is parsed and decoded (choice.ray == true). The .emd
//                        is either the best content-based candidate (empty `emd`) or the user's explicit file.
#include <functional>
#include <string>

namespace mmdx {

struct MaterialChoice {
    bool ray = false;   // false = Default, true = Ray-MMD
    std::string emd;    // explicit .emd path (UTF-8); empty = the best auto candidate (FindRayEmdCandidates)
    bool operator==(const MaterialChoice&) const = default;
};

// Resolves a model's saved material choice by its library character id (empty id = a stage / no id). Returns an
// empty (Default) choice when nothing was saved. Built by the App from AppSettings; used on the loader worker.
using MaterialChoiceFn = std::function<MaterialChoice(const std::string& libraryId)>;

} // namespace mmdx
