// pack_suggest_test [--pack <dir|pack.json>]... [dir...] : a synthetic self test of ComputeModelTraits
// (asset/ModelTraits.h) and of the pack.json "suggest" parsing (render/ShaderPack), then the traits of every
// .pmx / .pmd under the given directories (recursively; default: "library") with the packs each --pack suggests.
// Exit code 1 only when a self test fails.
#include "asset/ImageLoader.h"
#include "asset/ModelImport.h"
#include "asset/ModelTraits.h"
#include "asset/PmxModel.h"
#include "core/TextUtil.h"
#include "render/ShaderInterop.h"
#include "render/ShaderPack.h"

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

// A texture that varies along x: a UV-mapped shadow-colour (_S) map.
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

// Parses `json` as a pack manifest in a throwaway folder (the manifest's surface check fails, but "suggest"
// is parsed first), returning the ShaderPack; `error` holds the manifest problems.
ShaderPack ParseManifest(const std::string& json, std::string& error) {
    ShaderPack pack;
    ParseShaderPackManifest(json, fs::path(L"pack_suggest_test_nonexistent"), pack, error);
    return pack;
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
        const ModelTraits t = ComputeModelTraits(m, {ramp});
        expect(t.toonMapMaterials == 0, "ramp image -> not a toon-map material");
    }
    {   // UV-mapped (varying along x) toon image -> toon-map
        const PmxModel m = MakeModel(2, false, 0);
        const ModelTraits t = ComputeModelTraits(m, {toonMap});
        expect(t.toonMapMaterials == 1, "UV-mapped toon image -> toon-map material");
    }
    {   // additionalUvCount is copied through
        expect(ComputeModelTraits(MakeModel(0, false, 0), {toonMap}).additionalUvCount == 0,
               "addUv 0 -> additionalUvCount 0");
        expect(ComputeModelTraits(MakeModel(3, false, 0), {toonMap}).additionalUvCount == 3,
               "addUv 3 -> additionalUvCount 3");
    }
    {   // shared toon -> never a toon-map material
        const PmxModel shared = MakeModel(3, true, 0);
        const ModelTraits t = ComputeModelTraits(shared, {toonMap});
        expect(t.toonMapMaterials == 0, "shared toon -> not a toon-map material");
    }
    {   // toon texture that failed to load (empty image) -> not a toon-map material
        const PmxModel m = MakeModel(3, false, 0);
        const ModelTraits t = ComputeModelTraits(m, {ImageRGBA8{}});
        expect(t.toonMapMaterials == 0, "toon texture that failed to load -> not a toon-map material");
    }
    {   // materials count
        const ModelTraits t = ComputeModelTraits(MakeModel(1, false, 0), {toonMap});
        expect(t.materials == 1, "one material -> materials 1");
    }

    {   // Suggests: absent -> false for anything
        std::string err;
        const ShaderPack pack = ParseManifest(R"({"id":"p","version":"1.0.0"})", err);
        expect(!pack.suggest.present, "absent suggest -> present false");
        expect(!pack.Suggests(ModelTraits{4, 4, 4}), "absent suggest -> Suggests false");
    }
    {   // Suggests: both conditions must hold
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","suggest":{"additionalUvsMin":2,"toonMapMaterialsMin":1}})", err);
        expect(pack.suggest.present, "suggest present -> parsed");
        expect(pack.suggest.additionalUvsMin == 2 && pack.suggest.toonMapMaterialsMin == 1,
               "suggest values parsed");
        expect(pack.Suggests(ModelTraits{2, 1, 0}), "suggest: meets both -> true");
        expect(!pack.Suggests(ModelTraits{1, 1, 0}), "suggest: addUv below -> false");
        expect(!pack.Suggests(ModelTraits{2, 0, 0}), "suggest: toonMaps below -> false");
        expect(pack.Suggests(ModelTraits{3, 5, 0}), "suggest: above both -> true");
    }
    {   // Suggests: unknown key ignored with a warning, known keys still apply
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","suggest":{"additionalUvsMin":2,"bogus":9}})", err);
        expect(pack.suggest.present && pack.suggest.additionalUvsMin == 2,
               "suggest: unknown key ignored, known key kept");
        expect(pack.Suggests(ModelTraits{2, 0, 0}), "suggest: unknown key does not break matching");
    }
    {   // Suggests: wrong type is a manifest problem
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","suggest":{"additionalUvsMin":"x"}})", err);
        expect(err.find("suggest.additionalUvsMin") != std::string::npos,
               "suggest: wrong type reported as a problem");
        expect(pack.status == PackStatus::InvalidManifest, "suggest: wrong type -> InvalidManifest");
    }

    {   // alphaKey: absent -> 0 for every class
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","classes":[{"class":"weapon","texture":["_ehl_"]}]})", err);
        expect(pack.alphaKey[5] == 0.0f, "alphaKey absent -> 0");
    }
    {   // alphaKey: 0.45 on a class rule is stored per class
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","classes":[{"class":"weapon","texture":["_ehl_"],"alphaKey":0.45}]})", err);
        expect(pack.alphaKey[5] > 0.44f && pack.alphaKey[5] < 0.46f, "alphaKey 0.45 parsed");
        expect(pack.alphaKey[0] == 0.0f, "alphaKey only set for its class");
    }
    {   // alphaKey: wrong type is a manifest problem
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","classes":[{"class":"weapon","match":["x"],"alphaKey":"x"}]})", err);
        expect(err.find("alphaKey") != std::string::npos, "alphaKey: wrong type reported as a problem");
        expect(pack.status == PackStatus::InvalidManifest, "alphaKey: wrong type -> InvalidManifest");
    }
    {   // alphaKey: out of range is a manifest problem
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","classes":[{"class":"weapon","match":["x"],"alphaKey":1.5}]})", err);
        expect(err.find("alphaKey") != std::string::npos, "alphaKey: out of range reported as a problem");
        expect(pack.status == PackStatus::InvalidManifest, "alphaKey: out of range -> InvalidManifest");
    }
    {   // alphaKey: the FIRST rule of a class wins
        std::string err;
        const ShaderPack pack = ParseManifest(
            R"({"id":"p","version":"1.0.0","classes":[)"
            R"({"class":"weapon","match":["a"],"alphaKey":0.45},)"
            R"({"class":"weapon","match":["b"],"alphaKey":0.9}]})", err);
        expect(pack.alphaKey[5] > 0.44f && pack.alphaKey[5] < 0.46f, "alphaKey: first rule of a class wins");
    }

    {   // packClass bit encoding: round trip, low byte unchanged, kPackClassOff
        expect(PackClassLow(PackClassEncode(3, 0.45f)) == 3u, "packClass: low byte is the class");
        const float key = PackAlphaKeyFromClass(PackClassEncode(4, 0.45f));
        expect(key > 0.44f && key < 0.46f, "packClass: alpha key round trip");
        expect(PackAlphaKeyFromClass(PackClassEncode(0, 0.0f)) == 0.0f, "packClass: key 0 -> 0");
        expect(PackClassLow(PackClassEncode(kPackClassOff, 0.0f)) == kPackClassOff,
               "packClass: kPackClassOff stays 0xFF in the low byte");
        const float zeroKeys[6] = {0, 0, 0, 0, 0, 0};
        expect(PackAlphaKeyForClass(zeroKeys, 3) == 0.0f, "packClass: no key for the class -> 0");
        const float weaponKey[6] = {0, 0, 0, 0, 0, 0.45f};
        expect(PackAlphaKeyForClass(weaponKey, (uint32_t)PackClass::Weapon) > 0.44f,
               "packClass: key follows the final class");
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

std::string RelPath(const fs::path& p, const fs::path& root) {
    std::string rel = PathToUtf8(p.lexically_relative(root));
    for (char& c : rel)
        if (c == '\\') c = '/';
    return rel;
}

// Reads pack.json from `arg` (a folder or a pack.json path) into a ShaderPack. False with `error` when unreadable.
bool LoadPackArg(const fs::path& arg, ShaderPack& pack, std::string& error) {
    std::error_code ec;
    fs::path dir = arg;
    if (fs::is_regular_file(arg, ec)) dir = arg.parent_path();
    const fs::path manifest = fs::is_regular_file(arg, ec) ? arg : (dir / L"pack.json");
    FILE* f = _wfopen(manifest.c_str(), L"rb");
    if (!f) {
        error = "cannot read " + PathToUtf8(manifest);
        return false;
    }
    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
    fclose(f);
    std::string problem;
    ParseShaderPackManifest(json, dir, pack, problem);
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::pair<fs::path, ShaderPack>> packs;   // --pack arguments (path + parsed pack)
    std::vector<fs::path> dirs;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--pack" && i + 1 < argc) {
            const fs::path p = argv[++i];
            ShaderPack pack;
            std::string error;
            if (!LoadPackArg(p, pack, error)) {
                fprintf(stderr, "%s\n", error.c_str());
                continue;
            }
            packs.emplace_back(p, std::move(pack));
        } else {
            dirs.push_back(fs::path(arg));
        }
    }
    if (dirs.empty()) dirs.push_back(fs::path(L"library"));

    int failures = SelfTest();

    int scanned = 0, loadFailures = 0;
    std::vector<int> matches(packs.size(), 0);
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
            const ModelTraits traits = ComputeModelTraits(pmx, textures);
            const std::string rel = RelPath(path, dir);
            printf("traits addUv=%d toonMaps=%d/%d  %s\n", traits.additionalUvCount, traits.toonMapMaterials,
                   traits.materials, rel.c_str());
            for (size_t i = 0; i < packs.size(); ++i) {
                if (packs[i].second.Suggests(traits)) {
                    ++matches[i];
                    printf("MATCH %s %s\n", packs[i].second.id.c_str(), rel.c_str());
                }
            }
        }
    }

    printf("summary: %d models scanned, %d load failures\n", scanned, loadFailures);
    for (size_t i = 0; i < packs.size(); ++i)
        printf("pack %s: %d matches\n", packs[i].second.id.c_str(), matches[i]);

    return failures ? 1 : 0;
}
