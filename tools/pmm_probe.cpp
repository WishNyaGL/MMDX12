// pmm_probe <file.pmm>: read-only PMM (MMD project) dump. Prints the header, every model with its
// per-track key counts, the camera / light / self-shadow tracks, accessories (flagging MME effect
// controllers) and the audio path, then (unless --no-fuzz) a fuzz self test that truncates the file
// at every 1 KiB boundary and flips random bytes, asserting the loader never crashes.
//
//   pmm_probe <file.pmm> [--no-fuzz] [--fuzz-rounds N]
#include "asset/PmmLoader.h"
#include "core/TextUtil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace mmdx;

void PrintDoc(const PmmDocument& d) {
    std::printf("version      : %s\n", d.version.c_str());
    std::printf("output       : %d x %d  (timeline fov %.1f, fps %.1f)\n", d.outputWidth, d.outputHeight,
                d.timelineFov, d.preferredFps);
    std::printf("frames       : max %d", d.maxFrame);
    if (d.hasRange) std::printf("  range %d..%d", d.rangeStart, d.rangeEnd);
    if (d.loop) std::printf("  loop");
    std::printf("\n");
    std::printf("audio        : %s%s\n", d.audioPath.empty() ? "(none)" : d.audioPath.string().c_str(),
                d.audioEnabled ? "" : "  [disabled]");
    std::printf("models       : %zu\n", d.models.size());
    for (size_t mi = 0; mi < d.models.size(); ++mi) {
        const PmmModel& m = d.models[mi];
        size_t boneKeys = 0, morphKeys = 0;
        for (const auto& [n, k] : m.bones) boneKeys += k.size();
        for (const auto& [n, k] : m.morphs) morphKeys += k.size();
        std::printf("  [%zu] %s  (%s)\n", mi, m.nameJa.c_str(), m.nameEn.c_str());
        std::printf("      path     : %s\n", m.path.string().c_str());
        std::printf("      visible  : %s   last frame %d   draw idx %d\n", m.visible ? "yes" : "no", m.lastFrame, m.index);
        std::printf("      bones    : %zu tracks, %zu keys", m.boneNames.size(), boneKeys);
        // show the first few bone track names with key counts
        size_t shown = 0;
        for (const auto& [n, k] : m.bones) {
            if (shown++ == 4) break;
            std::printf(" [%s:%zu]", n.c_str(), k.size());
        }
        std::printf("\n");
        std::printf("      morphs   : %zu tracks, %zu keys\n", m.morphNames.size(), morphKeys);
        std::printf("      ik bones : %zu   outside-parent bones: %zu   model keys: %zu\n",
                    m.ikBones.size(), m.outsideParentBones.size(), m.modelKeys.size());
    }
    std::printf("camera keys  : %zu\n", d.cameraKeys.size());
    if (!d.cameraKeys.empty()) {
        std::printf("  first frame %d, last frame %d\n", d.cameraKeys.front().frame, d.cameraKeys.back().frame);
    }
    std::printf("light keys   : %zu\n", d.lightKeys.size());
    std::printf("shadow keys  : %zu\n", d.shadowKeys.size());
    std::printf("accessories  : %zu\n", d.accessories.size());
    for (size_t i = 0; i < d.accessories.size(); ++i) {
        const PmmAccessory& a = d.accessories[i];
        const std::string fn = a.path.filename().string();
        std::string matched;
        const MmeController c = ClassifyMmeController(fn, &matched);
        std::printf("  [%zu] %s  ->  %s   keys %zu", i, a.name.empty() ? "(no name)" : a.name.c_str(),
                    a.path.string().c_str(), a.keys.size());
        if (c != MmeController::None) std::printf("   [MME %s]", MmeControllerName(c));
        else if (LooksLikeUnknownMmeController(a.path)) std::printf("   [unknown MME controller]");
        std::printf("\n");
    }
    if (!d.warnings.empty()) {
        std::printf("warnings     : %zu\n", d.warnings.size());
        for (const std::string& w : d.warnings) std::printf("  - %s\n", w.c_str());
    }
    const PmmEffectSettings eff = MapMmeControllers(d);
    if (!eff.recognised.empty() || !eff.unsupported.empty()) {
        std::printf("MME effects  : %zu recognised, %zu unsupported\n", eff.recognised.size(), eff.unsupported.size());
        for (const std::string& r : eff.recognised) std::printf("  + %s\n", r.c_str());
        for (const std::string& u : eff.unsupported) std::printf("  ? %s\n", u.c_str());
    }
}

// The fuzz self test: truncate at every 1 KiB boundary, then flip random bytes, and load each variant.
// The loader must return a value (true or false) without crashing.
bool FuzzSelfTest(const std::filesystem::path& file, int rounds) {
    std::vector<unsigned char> original;
    {
        FILE* f = nullptr;
        _wfopen_s(&f, file.wstring().c_str(), L"rb");
        if (!f) { std::printf("fuzz: cannot read %s\n", file.string().c_str()); return false; }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        original.resize((size_t)n);
        if (n > 0) (void)std::fread(original.data(), 1, (size_t)n, f);
        std::fclose(f);
    }
    const std::filesystem::path tmp = std::filesystem::temp_directory_path() / "pmm_probe_fuzz.pmm";
    int variants = 0;

    const auto tryLoad = [&](const std::vector<unsigned char>& bytes) {
        {
            FILE* f = nullptr;
            _wfopen_s(&f, tmp.wstring().c_str(), L"wb");
            if (!f) return;
            if (!bytes.empty()) (void)std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
        PmmDocument d;
        std::string err;
        (void)LoadPmm(tmp, d, &err);   // return value is irrelevant: the point is "no crash"
    };

    // 1) truncation at every 1 KiB boundary
    for (size_t cut = 1024; cut <= original.size(); cut += 1024) {
        tryLoad(std::vector<unsigned char>(original.begin(), original.begin() + (ptrdiff_t)cut));
        ++variants;
    }
    // 2) random byte flips
    std::mt19937 rng(0xC0FFEE);
    for (int i = 0; i < rounds && !original.empty(); ++i) {
        std::vector<unsigned char> bytes = original;
        const int flips = 1 + (int)(rng() % 64);
        for (int j = 0; j < flips; ++j) {
            const size_t at = rng() % bytes.size();
            bytes[at] = (unsigned char)(rng() & 0xFF);
        }
        tryLoad(bytes);
        ++variants;
    }
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    std::printf("fuzz self test : %d variants, no crash\n", variants);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path file;
    bool fuzz = true;
    int rounds = 500;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--no-fuzz") fuzz = false;
        else if (a == L"--fuzz-rounds" && i + 1 < argc) rounds = _wtoi(argv[++i]);
        else if (file.empty()) file = a;
    }
    if (file.empty()) {
        std::printf("usage: pmm_probe <file.pmm> [--no-fuzz] [--fuzz-rounds N]\n");
        return 2;
    }
    PmmDocument doc;
    std::string error;
    const bool ok = LoadPmm(file, doc, &error);
    std::printf("=== %s ===\n", file.string().c_str());
    if (!ok) {
        std::printf("LOAD FAILED: %s\n", error.c_str());
    } else {
        PrintDoc(doc);
    }
    int rc = ok ? 0 : 1;
    if (fuzz) {
        if (!FuzzSelfTest(file, rounds)) rc = 1;
    }
    return rc;
}
