// MmeAssign.cpp: which effect files a model / accessory asks for (see MmeAssign.h). It reads MME's .emd sidecar and
// the same-name / name[effect.fx] conventions; it changes no state and never throws.
#include "asset/MmeAssign.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <system_error>

#include "asset/MmeEffect.h"
#include "asset/MmeText.h"
#include "asset/PmmLoader.h"
#include "asset/RayMaterial.h"
#include "core/Log.h"
#include "core/TextUtil.h"

namespace mmdx {

namespace {

using mmetext::Lower;
using mmetext::ReadTextFile;
using mmetext::ResolveRel;
using mmetext::SplitLines;
using mmetext::Trim;

bool IsNoneValue(const std::string& v) {
    const std::string lo = Lower(Trim(v));
    return lo.empty() || lo == "none" || lo == "(none)";
}

} // namespace

std::vector<MmeAssignment> ReadEmd(const std::filesystem::path& emd) {
    std::vector<MmeAssignment> out;
    try {
        std::string content;
        if (!ReadTextFile(emd, content)) return out;

        const std::filesystem::path base = emd.parent_path();
        bool inEffect = false;
        // Obj[n].show / Obj.show visibility, applied after the effect entries are collected.
        std::map<int, bool> hiddenBySubset;
        for (const std::string& line : SplitLines(content)) {
            const std::string t = Trim(line);
            if (t.empty()) continue;
            if (t[0] == '[') {
                inEffect = Lower(t).rfind("[effect]", 0) == 0;
                continue;
            }
            if (!inEffect) continue;
            if (Lower(t).rfind("obj", 0) != 0) continue;

            // `Obj`, `Obj[n]`, `Obj.show`, `Obj[n].show`
            size_t p = 3;
            while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
            int index = -1;
            if (p < t.size() && t[p] == '[') {
                const size_t e = t.find(']', p + 1);
                if (e == std::string::npos) continue;
                try { index = std::stoi(Trim(t.substr(p + 1, e - p - 1))); } catch (...) { continue; }
                p = e + 1;
            }
            // after `Obj` / `Obj[n]` the only valid continuations are `=` or `.show =`
            std::string suffix;
            if (p < t.size() && t[p] == '.') {
                ++p;
                const size_t s = p;
                while (p < t.size() && t[p] != '=' && !std::isspace((unsigned char)t[p])) ++p;
                suffix = Lower(t.substr(s, p - s));
            }
            while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
            if (p >= t.size() || t[p] != '=') continue;   // not an assignment
            const std::string value = Trim(t.substr(p + 1));
            if (suffix == "show") {
                const bool show = Lower(value) != "false";
                if (index >= 0) hiddenBySubset[index] = !show;
                continue;
            }
            if (!suffix.empty()) continue;   // unknown dotted key
            if (IsNoneValue(value)) continue;   // explicit `none` = no effect
            std::filesystem::path fx;
            if (!ResolveRel(base, value, fx)) continue;
            out.push_back({fx, index, false});
        }
        for (MmeAssignment& a : out) {
            const auto it = hiddenBySubset.find(a.subset);
            if (it != hiddenBySubset.end()) a.hidden = it->second;
        }
    } catch (...) {
        // never throw
    }
    return out;
}

namespace {

// The effect file named inside a `name[effect.fx].pmx` model name, resolved in `dir`.
std::filesystem::path EmbeddedEffect(const std::filesystem::path& model) {
    const std::string stem = PathToUtf8(model.stem());
    const size_t open = stem.find('[');
    const size_t close = stem.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close <= open + 1) return {};
    const std::string name = stem.substr(open + 1, close - open - 1);
    if (Lower(name).size() < 3) return {};
    std::filesystem::path fx;
    if (!ResolveRel(model.parent_path(), name, fx)) return {};
    return fx;
}

} // namespace

std::vector<MmeAssignment> FindModelEffects(const std::filesystem::path& model) {
    std::vector<MmeAssignment> out;
    try {
        std::error_code ec;
        // 1. same-stem .emd (its own contents decide, even when empty)
        std::filesystem::path emd = model;
        emd.replace_extension(L".emd");
        if (std::filesystem::exists(emd, ec)) return ReadEmd(emd);
        // 2. same-name .fx
        std::filesystem::path fx = model;
        fx.replace_extension(L".fx");
        if (std::filesystem::exists(fx, ec)) {
            out.push_back({fx, -1, false});
            return out;
        }
        // 3. name[effect.fx].pmx embedding
        const std::filesystem::path embedded = EmbeddedEffect(model);
        if (!embedded.empty()) out.push_back({embedded, -1, false});
    } catch (...) {
        // never throw
    }
    return out;
}

namespace {

// Auto workflow (docs/mme/mme_design.md §2.12): an effect whose CONTROLOBJECT scalar / vector / matrix reads a
// separate controller object the MMD user loads next to the character renders as a dark silhouette without it. A
// bool CONTROLOBJECT is only an existence check (`if (light_check)`), never "required".
//
// Exceptions (a missing controller of a known optional family changes nothing, so it is not required):
//  - the controller is MES40's `mes40 controller.pmx` / `.pmd` (every read is guarded with `if (Controller)`), or
//  - every non-bool CONTROLOBJECT param that reads it carries an `item` annotation (case-insensitive) in
//    {AMBIENT_R, AMBIENT_G, AMBIENT_B, AMBIENT_INT} (the anonRipper / Chips ambient controller: missing = weight 0 =
//    unchanged colour), whatever the controller's name.
// Everything else keeps the plain rule (e.g. `Rim_Luka.pmx` and `mes40 spotlight*/pointlight*` stay required).
struct ControllerCacheEntry {
    std::filesystem::file_time_type stamp{};
    std::vector<std::string> controllers;
};
std::mutex gControllerCacheMutex;
std::map<std::string, ControllerCacheEntry> gControllerCache;

// True when a controller read through `p` (a non-bool CONTROLOBJECT) is in a known optional family.
bool OptionalControllerRead(const MmeParam& p, const std::string& name) {
    if (name == "mes40 controller.pmx" || name == "mes40 controller.pmd") return true;
    if (const MmeAnnotation* item = FindAnnotation(p.annotations, "item")) {
        const std::string v = Lower(Trim(item->value.str));
        if (v == "ambient_r" || v == "ambient_g" || v == "ambient_b" || v == "ambient_int") return true;
    }
    return false;
}

std::vector<std::string> RequiredControllersUncached(const std::filesystem::path& fx) {
    std::vector<std::string> out;
    const MmeEffectDesc desc = LoadMmeEffect(fx);
    if (!desc.ok) return out;
    // A controller is required unless EVERY non-bool CONTROLOBJECT param that reads it is in an optional family.
    std::vector<std::string> order;              // controller names, first-seen order
    std::map<std::string, bool> allOptional;     // name -> every reader so far optional
    for (const MmeParam& p : desc.params) {
        if (p.semantic != "CONTROLOBJECT") continue;
        if (Lower(p.type) == "bool") continue;   // existence check
        const MmeAnnotation* a = FindAnnotation(p.annotations, "name");
        std::string name = a ? Lower(Trim(a->value.str)) : std::string();
        if (name.empty() || name == "(self)" || name == "(offscreenowner)") continue;
        const bool optional = OptionalControllerRead(p, name);
        auto it = allOptional.find(name);
        if (it == allOptional.end()) {
            order.push_back(name);
            allOptional[name] = optional;
        } else if (!optional) {
            it->second = false;
        }
    }
    for (const std::string& name : order)
        if (!allOptional[name]) out.push_back(name);
    return out;
}

} // namespace

std::vector<std::string> MmeRequiredControllers(const std::filesystem::path& fx) {
    if (fx.empty()) return {};
    try {
        const std::string key = PathToUtf8(fx);
        std::error_code ec;
        const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(fx, ec);
        {
            std::lock_guard<std::mutex> lock(gControllerCacheMutex);
            const auto it = gControllerCache.find(key);
            if (it != gControllerCache.end() && !ec && it->second.stamp == stamp) return it->second.controllers;
        }
        std::vector<std::string> controllers = RequiredControllersUncached(fx);
        {
            std::lock_guard<std::mutex> lock(gControllerCacheMutex);
            gControllerCache[key] = ControllerCacheEntry{stamp, controllers};
        }
        return controllers;
    } catch (...) {
        return {};   // includes / macros failing to resolve: do not block the effect
    }
}

// ---- the automatic controller load (Auto workflow, play scenes) ----------------------------------------------------

// The automatic controller-load Ray-MMD guard: a Ray-MMD main effect (ray.x / ray.fx), a file (or the requested
// lower-case `controllerName` itself) that classifies as a Ray-MMD controller (PmmLoader's table) is refused for
// automatic loads: Ray-MMD stays with the native PBR (no double ray.x next to it). One rule for every automatic
// load; never throws.
bool IsRayMmdControllerFile(const std::filesystem::path& file, const std::string& controllerName) {
    try {
        if (!file.empty() && IsRayMmdMainEffect(file)) return true;
        if (!file.empty() && ClassifyMmeController(mmetext::Lower(PathToUtf8(file.filename()))) ==
                                 MmeController::Ray) return true;
        if (!controllerName.empty() && ClassifyMmeController(controllerName) == MmeController::Ray) return true;
    } catch (...) {
        // never throw
    }
    return false;
}

namespace {

// Automatic controller search: hard caps that keep a pathological folder tree from stalling a frame.
constexpr int kMaxSearchDepth = 4;
constexpr size_t kMaxVisitedEntries = 5000;

// Finds inside `dir` (recursion depth < kMaxSearchDepth) the first file matching `name` (lower case: an exact
// case-insensitive file-name equality, or — when `matchStem`, i.e. `name` carries no extension — a file whose
// stem equals it with extension .pmx / .pmd / .x). Unreadable directories are skipped (error_code overloads
// everywhere); `visited` bounds the total entries across the roots. Never throws.
std::filesystem::path SearchControllerDir(const std::filesystem::path& dir, const std::string& name,
                                          bool matchStem, int depth, size_t& visited) {
    if (depth >= kMaxSearchDepth || visited >= kMaxVisitedEntries) return {};
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) return {};
    std::filesystem::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) return {};   // the iteration itself failed: stop this branch
        if (visited >= kMaxVisitedEntries) return {};
        ++visited;
        const std::filesystem::directory_entry& entry = *it;
        std::error_code entryEc;
        const bool isDir = entry.is_directory(entryEc);
        if (entryEc) continue;   // unreadable entry
        if (isDir) {
            std::filesystem::path hit = SearchControllerDir(entry.path(), name, matchStem, depth + 1, visited);
            if (!hit.empty()) return hit;
            continue;
        }
        const std::string fileName = mmetext::Lower(PathToUtf8(entry.path().filename()));
        if (fileName.empty()) continue;
        if (fileName == name) return entry.path();
        if (matchStem) {
            const std::string stem = mmetext::Lower(PathToUtf8(entry.path().stem()));
            const std::string ext = mmetext::Lower(PathToUtf8(entry.path().extension()));
            if (stem == name && (ext == ".pmx" || ext == ".pmd" || ext == ".x")) return entry.path();
        }
    }
    return {};
}

} // namespace

std::filesystem::path FindMmeControllerFile(const std::filesystem::path& modelPath,
                                            const std::string& controllerName) {
    // Cached per (model path, controller name); a negative result is cached too (empty = not found).
    struct ControllerFileCacheEntry {
        std::filesystem::path file;
    };
    static std::mutex gControllerFileCacheMutex;
    static std::map<std::string, ControllerFileCacheEntry> gControllerFileCache;
    if (modelPath.empty() || controllerName.empty()) return {};
    try {
        const bool matchStem = controllerName.find('.') == std::string::npos;
        const std::string key = PathToUtf8(modelPath) + "|" + controllerName;
        {
            std::lock_guard<std::mutex> lock(gControllerFileCacheMutex);
            const auto it = gControllerFileCache.find(key);
            if (it != gControllerFileCache.end()) return it->second.file;
        }
        std::filesystem::path found;
        auto accept = [&](const std::filesystem::path& candidate) -> bool {
            if (candidate.empty()) return false;
            if (IsRayMmdControllerFile(candidate, controllerName)) {
                LOG_INFO("MME auto: controller file %s refused (Ray-MMD stays with the native PBR)",
                         PathToUtf8(candidate).c_str());
                return false;
            }
            found = candidate;
            return true;
        };
        size_t visited = 0;
        // (a) the model's folder, recursively
        const std::filesystem::path modelDir = modelPath.parent_path();
        if (!modelDir.empty()) {
            std::filesystem::path hit = SearchControllerDir(modelDir, controllerName, matchStem, 0, visited);
            if (!hit.empty() && accept(hit)) {
                std::lock_guard<std::mutex> lock(gControllerFileCacheMutex);
                gControllerFileCache[key] = ControllerFileCacheEntry{found};
                return found;
            }
        }
        // (b) the folders of the model's Auto effects, recursively (stop at the first match)
        for (const MmeAssignment& a : FindModelEffects(modelPath)) {
            if (a.effect.empty() || visited >= kMaxVisitedEntries) break;
            const std::filesystem::path dir = a.effect.parent_path();
            if (dir.empty() || dir == modelDir) continue;
            std::filesystem::path hit = SearchControllerDir(dir, controllerName, matchStem, 0, visited);
            if (!hit.empty() && accept(hit)) break;
        }
        std::lock_guard<std::mutex> lock(gControllerFileCacheMutex);
        gControllerFileCache[key] = ControllerFileCacheEntry{found};   // a negative result is cached too
        return found;
    } catch (...) {
        return {};   // never throw
    }
}

} // namespace mmdx
