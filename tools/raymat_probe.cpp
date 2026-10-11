// raymat_probe <library dir|model file> [--verbose] : for every PMX/PMD found (recursively), lists the candidate
// .emd files (asset/RayMaterial.h, chosen by content) and parses the best one, printing a per-model summary and a
// global summary: models with Ray data, Ray .fx parsed vs failed, histograms of CUSTOM_ENABLE and each slot's FROM,
// unknown / unparsed preprocessor lines (top N) and the missing referenced-file count. Times the whole scan.
#include "asset/PmxModel.h"
#include "asset/RayMaterial.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace mmdx;
namespace fs = std::filesystem;

namespace {

// Case-insensitive path key so the same .fx reached through different model variants counts once.
std::string PathKey(const fs::path& p) {
    std::string s = PathToUtf8(p);
    for (char& c : s) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    return s;
}

bool LoadMaterialCount(const fs::path& path, int& count, std::string& err) {
    PmxModel m;
    const bool pmx = path.extension() == L".pmx";
    if (!(pmx ? LoadPmx(path, m, &err) : LoadPmd(path, m, &err))) return false;
    count = (int)m.materials.size();
    return true;
}

// Candidate .emd files only need the model's material count.

std::string Rel(const fs::path& p, const fs::path& root) {
    std::string s = PathToUtf8(p.lexically_relative(root));
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    bool verbose = false;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--verbose" || a == L"-v") verbose = true;
        else inputs.push_back(fs::path(a));
    }
    if (inputs.empty()) inputs.push_back(fs::path(L"library"));

    // Gather models, and every .fx in the inputs (the content rule can then be counted library-wide, matching the
    // handoff's ~163 Ray-material .fx figure, not just the ones the chosen .emd files reach).
    std::vector<fs::path> models;
    std::vector<fs::path> fxFiles;
    std::error_code ec0;
    if (fs::is_directory(inputs.front(), ec0)) {
        for (const auto& e : fs::recursive_directory_iterator(inputs.front(),
                 fs::directory_options::skip_permission_denied, ec0)) {
            if (!e.is_regular_file()) continue;
            std::wstring ext = e.path().extension().wstring();
            for (wchar_t& c : ext) c = (wchar_t)towlower(c);
            if (ext == L".fx") fxFiles.push_back(e.path());
        }
    }
    for (const fs::path& in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (const auto& e : fs::recursive_directory_iterator(
                     in, fs::directory_options::skip_permission_denied, ec)) {
                if (!e.is_regular_file()) continue;
                const std::wstring ext = e.path().extension().wstring();
                if (ext == L".pmx" || ext == L".pmd" || ext == L".PMX" || ext == L".PMD")
                    models.push_back(e.path());
            }
        } else if (fs::is_regular_file(in, ec)) {
            models.push_back(in);
        }
    }
    std::sort(models.begin(), models.end());
    const fs::path root = !inputs.empty() && fs::is_directory(inputs.front()) ? inputs.front() : fs::path();

    const auto t0 = std::chrono::steady_clock::now();
    int scanned = 0, loadFailures = 0, withRay = 0, withEmd = 0, rayMaterials = 0, totalMaterials = 0;
    RayParseStats stats;
    std::set<std::string> parsedFx, rayFx, failedFx;   // library-unique across model variants
    int missingFiles = 0;
    std::map<int, int> shadingHist;                       // CUSTOM_ENABLE -> materials
    std::map<int, int> fromHist[kRaySlotCount];           // slot -> FROM -> count (Ray materials only)

    for (const fs::path& model : models) {
        ++scanned;
        int matCount = 0;
        std::string err;
        if (!LoadMaterialCount(model, matCount, err)) {
            ++loadFailures;
            printf("LOAD-FAIL %s: %s\n", Rel(model, root).c_str(), err.c_str());
            continue;
        }
        totalMaterials += matCount;
        const std::vector<RayEmdCandidate> cands = FindRayEmdCandidates(model, matCount);
        std::vector<RayMaterialDesc> descs;
        fs::path chosen;
        RayParseStats local;
        FindAndParseRayMaterials(model, matCount, descs, &chosen, &local, &err);
        stats.emdRead += local.emdRead;
        for (const fs::path& p : local.parsedFx) parsedFx.insert(PathKey(p));
        for (const fs::path& p : local.rayFx) rayFx.insert(PathKey(p));
        for (const fs::path& p : local.failedFx) failedFx.insert(PathKey(p));
        missingFiles += local.missingFiles;
        for (const auto& [k, v] : local.unknownDirectives) stats.unknownDirectives[k] += v;

        int rayMats = 0;
        for (const RayMaterialDesc& d : descs) {
            if (!d.valid || !d.ray) continue;
            ++rayMats;
            ++shadingHist[d.shadingModel];
            for (int s = 0; s < kRaySlotCount; ++s) fromHist[s][d.slots[s].from]++;
        }
        rayMaterials += rayMats;
        if (rayMats > 0) ++withRay;
        if (!chosen.empty()) ++withEmd;

        if (!chosen.empty() || verbose) {
            printf("%-58s mats=%3d %s", Rel(model, root).c_str(), matCount,
                   chosen.empty() ? "(no Ray .emd)" : Rel(chosen, root).c_str());
            if (!chosen.empty()) printf("  rayMats=%d", rayMats);
            printf("\n");
            if (verbose) {
                for (const RayEmdCandidate& c : cands) {
                    printf("    cand %-52s obj=%3d ray=%3d inRange=%3d share=%3d%%%s\n",
                           Rel(c.path, root).c_str(), c.objCount, c.rayEntries, c.inRangeEntries, c.raySharePct,
                           c.readFailed ? " READ-FAIL" : "");
                }
            }
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Library-wide content rule: how many .fx are Ray materials, independent of which .emd reached them.
    int rayFxAll = 0;
    for (const fs::path& fx : fxFiles) {
        if (IsRayMaterialFx(fx)) ++rayFxAll;
    }

    printf("\n== global summary ==\n");
    printf("models scanned        : %d (%d load failures)\n", scanned, loadFailures);
    printf("models with Ray data  : %d (with a chosen .emd: %d)\n", withRay, withEmd);
    printf("Ray materials         : %d of %d PMX materials\n", rayMaterials, totalMaterials);
    printf(".emd read             : %d\n", stats.emdRead);
    printf("Ray .fx (library-wide): %d of %zu .fx files pass the content rule\n", rayFxAll, fxFiles.size());
    printf("Ray .fx (reached)     : %zu of %zu parsed (failed: %zu)\n", rayFx.size(), parsedFx.size(),
           failedFx.size());
    printf("missing referenced map files (per parsed Ray .fx): %d\n", missingFiles);

    printf("CUSTOM_ENABLE histogram:\n");
    for (const auto& [model, count] : shadingHist) printf("  %3d : %d\n", model, count);

    for (int s = 0; s < kRaySlotCount; ++s) {
        if (fromHist[s].empty()) continue;
        printf("%s FROM histogram:\n", RaySlotName((RaySlot)s));
        for (const auto& [from, count] : fromHist[s]) printf("  %3d : %d\n", from, count);
    }

    if (!stats.unknownDirectives.empty()) {
        std::vector<std::pair<std::string, int>> unknown(stats.unknownDirectives.begin(), stats.unknownDirectives.end());
        std::sort(unknown.begin(), unknown.end(),
                  [](const auto& a, const auto& b) { return a.second != b.second ? a.second > b.second
                                                                                 : a.first < b.first; });
        printf("unknown / unparsed preprocessor lines (top %zu):\n", std::min<size_t>(unknown.size(), 20));
        for (size_t i = 0; i < unknown.size() && i < 20; ++i)
            printf("  %6d  %s\n", unknown[i].second, unknown[i].first.c_str());
    }

    printf("scan time             : %.2f s\n", secs);
    return 0;
}
