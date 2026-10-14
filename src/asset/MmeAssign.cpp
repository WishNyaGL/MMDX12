// MmeAssign.cpp: which effect files a model / accessory asks for (see MmeAssign.h). It reads MME's .emd sidecar and
// the same-name / name[effect.fx] conventions; it changes no state and never throws.
#include "asset/MmeAssign.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <system_error>

#include "asset/MmeText.h"
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

} // namespace mmdx
