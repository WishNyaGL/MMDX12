// sekai_detect_test [dir...] [--expect <file>] : a synthetic self test of the Project Sekai layout
// detection (asset/SekaiLayout.h), then the same detection over every .pmx / .pmd under the given
// directories (recursively; default: "library"). One line per model, then a summary.
// Exit code 1 when a self test fails, or --expect is given and the detected set differs.
#include "asset/ImageLoader.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/SekaiLayout.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace mmdx;
namespace fs = std::filesystem;

namespace {

// A texture whose rows are constant along x: a classic toon ramp.
ImageRGBA8 MakeRampImage(uint32_t w, uint32_t h) {
    ImageRGBA8 img;
    img.mips.resize(1);
    ImageRGBA8::Level& l = img.mips[0];
    l.width = w;
    l.height = h;
    l.pixels.assign((size_t)w * h * 4, 255);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t v = (uint8_t)(255 * y / (h ? h - 1 : 1));
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &l.pixels[((size_t)y * w + x) * 4];
            p[0] = p[1] = p[2] = v;
        }
    }
    return img;
}

// A texture that varies along x: the UV-mapped shadow-colour (_S) map of the Sekai layout.
ImageRGBA8 MakeUvMappedImage(uint32_t w, uint32_t h) {
    ImageRGBA8 img;
    img.mips.resize(1);
    ImageRGBA8::Level& l = img.mips[0];
    l.width = w;
    l.height = h;
    l.pixels.assign((size_t)w * h * 4, 255);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = &l.pixels[((size_t)y * w + x) * 4];
            const uint8_t v = (uint8_t)(255 * x / (w ? w - 1 : 1));
            p[0] = v;
            p[1] = (uint8_t)(255 - v);
            p[2] = (uint8_t)((v * 3) & 0xFF);
        }
    }
    return img;
}

PmxModel MakeModel(int addUv, bool sharedToon, int32_t toonIndex) {
    PmxModel m;
    m.additionalUvCount = addUv;
    PmxMaterial mat;
    mat.sharedToon = sharedToon;
    mat.toonIndex = toonIndex;
    m.materials.push_back(mat);
    return m;
}

int SelfTest() {
    int fails = 0;
    auto expect = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++fails;
    };

    const ImageRGBA8 ramp = MakeRampImage(8, 8);
    const ImageRGBA8 toonMap = MakeUvMappedImage(16, 8);

    expect(IsToonRampImage(ramp), "ramp image is a toon ramp");
    expect(!IsToonRampImage(toonMap), "UV-mapped image is not a toon ramp");

    {   // ramp image -> not a toon-map material
        const PmxModel m = MakeModel(2, false, 0);
        const SekaiLayoutInfo i = DetectSekaiLayout(m, {ramp});
        expect(i.toonMapMaterials == 0 && !i.sekai, "ramp image -> not a toon-map material");
    }
    {   // UV-mapped (varying along x) toon image -> toon-map
        const PmxModel m = MakeModel(2, false, 0);
        const SekaiLayoutInfo i = DetectSekaiLayout(m, {toonMap});
        expect(i.toonMapMaterials == 1, "UV-mapped toon image -> toon-map material");
    }
    {   // addUv 0 / 1 + toon-map -> false
        const PmxModel a = MakeModel(0, false, 0);
        const PmxModel b = MakeModel(1, false, 0);
        expect(!DetectSekaiLayout(a, {toonMap}).sekai, "addUv 0 + toon-map -> false");
        expect(!DetectSekaiLayout(b, {toonMap}).sekai, "addUv 1 + toon-map -> false");
    }
    {   // addUv 2 / 3 + toon-map -> true
        const PmxModel a = MakeModel(2, false, 0);
        const PmxModel b = MakeModel(3, false, 0);
        expect(DetectSekaiLayout(a, {toonMap}).sekai, "addUv 2 + toon-map -> true");
        expect(DetectSekaiLayout(b, {toonMap}).sekai, "addUv 3 + toon-map -> true");
    }
    {   // addUv 3 + only ramps / shared toons -> false
        const PmxModel ramps = MakeModel(3, false, 0);
        const SekaiLayoutInfo ri = DetectSekaiLayout(ramps, {ramp});
        expect(!ri.sekai, "addUv 3 + only ramps -> false");
        const PmxModel shared = MakeModel(3, true, 0);
        const SekaiLayoutInfo si = DetectSekaiLayout(shared, {toonMap});
        expect(!si.sekai && si.toonMapMaterials == 0, "addUv 3 + only shared toons -> false");
    }
    {   // toon texture that failed to load (empty image) -> not a toon-map material
        const PmxModel m = MakeModel(3, false, 0);
        const SekaiLayoutInfo i = DetectSekaiLayout(m, {ImageRGBA8{}});
        expect(i.toonMapMaterials == 0 && !i.sekai, "toon texture that failed to load -> false");
    }

    printf("selftest: %s\n", fails ? "FAILED" : "ok");
    return fails;
}

// Loads only the toon-slot textures of a model, the same way the app resolves them
// (app/SceneLoader.cpp DecodeModelTextures): embedded bytes first, else ResolveTexturePath.
void LoadToonTextures(const PmxModel& pmx, std::vector<ImageRGBA8>& textures) {
    textures.assign(pmx.textures.size(), ImageRGBA8{});
    std::set<int32_t> toon;
    for (const PmxMaterial& m : pmx.materials)
        if (!m.sharedToon && m.toonIndex >= 0 && (size_t)m.toonIndex < pmx.textures.size())
            toon.insert(m.toonIndex);
    for (int32_t i : toon) {
        std::string err;
        if ((size_t)i < pmx.embeddedTextures.size() && !pmx.embeddedTextures[i].empty()) {
            LoadImageRGBA8FromMemory(pmx.embeddedTextures[i].data(), pmx.embeddedTextures[i].size(), textures[i], &err);
            continue;
        }
        const fs::path p = pmx.ResolveTexturePath(i);
        if (!p.empty()) LoadImageRGBA8(p, textures[i], &err);
    }
}

bool IsPmxOrPmd(const fs::path& p) { return IsMmdModelFormat(ModelFormatFromPath(p)); }

std::string Trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string RelPath(const fs::path& p, const fs::path& root) {
    std::string rel = PathToUtf8(p.lexically_relative(root));
    for (char& c : rel)
        if (c == '\\') c = '/';
    return rel;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<fs::path> dirs;
    fs::path expectFile;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--expect" && i + 1 < argc) {
            expectFile = argv[++i];
        } else {
            dirs.push_back(fs::path(arg));
        }
    }
    if (dirs.empty()) dirs.push_back(fs::path(L"library"));

    int failures = SelfTest();

    int scanned = 0, detected = 0, loadFailures = 0;
    std::set<std::string> detectedSet;
    for (const fs::path& dir : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            fprintf(stderr, "not a directory: %s\n", PathToUtf8(dir).c_str());
            continue;
        }
        for (const auto& e : fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file()) continue;
            const fs::path& path = e.path();
            if (!IsPmxOrPmd(path)) continue;
            ++scanned;
            PmxModel pmx;
            std::string err;
            if (!LoadModelFile(path, ModelRole::Character, pmx, &err)) {
                ++loadFailures;
                printf("LOAD-FAIL %s: %s\n", RelPath(path, dir).c_str(), err.c_str());
                continue;
            }
            std::vector<ImageRGBA8> textures;
            LoadToonTextures(pmx, textures);
            const SekaiLayoutInfo info = DetectSekaiLayout(pmx, textures);
            const std::string rel = RelPath(path, dir);
            printf("SEKAI|%s  addUv=%d toonMaps=%d/%d  %s\n", info.sekai ? "yes" : "no ", info.additionalUvCount,
                   info.toonMapMaterials, info.materials, rel.c_str());
            if (info.sekai) {
                ++detected;
                detectedSet.insert(rel);
            }
        }
    }

    printf("summary: %d models scanned, %d detected, %d load failures\n", scanned, detected, loadFailures);
    for (const std::string& s : detectedSet) printf("DETECTED %s\n", s.c_str());

    if (!expectFile.empty()) {
        std::ifstream in(expectFile);
        if (!in) {
            fprintf(stderr, "cannot read expect file: %s\n", PathToUtf8(expectFile).c_str());
            return 1;
        }
        std::set<std::string> expected;
        std::string line;
        while (std::getline(in, line)) {
            const std::string t = Trim(line);
            if (!t.empty()) expected.insert(t);
        }
        bool same = expected == detectedSet;
        for (const std::string& s : detectedSet)
            if (!expected.count(s)) printf("UNEXPECTED %s\n", s.c_str());
        for (const std::string& s : expected)
            if (!detectedSet.count(s)) printf("MISSING %s\n", s.c_str());
        if (!same) ++failures;
    }

    return failures ? 1 : 0;
}
