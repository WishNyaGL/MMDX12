// pbr_tex_probe <model.pmx|.pmd> [--dump <dir>] [--verbose] : runs the loader's PBR conversion
// (BuildPbrModelData) on one model and prints, per material with a record, every File slot: path, channel, decoded
// mip-0 size, mip count, loopU/V, uvScale and uvFlip. With --dump it also writes each decoded texture's level 0 as
// PNG, so the CPU-side decode can be compared against the render. Pure asset layer (mmdx_core), no GPU.
#include "asset/ImageLoader.h"
#include "asset/PbrMaterial.h"
#include "asset/PmxModel.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace mmdx;
namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    fs::path modelPath;
    fs::path dumpDir;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--dump" && i + 1 < argc) {
            dumpDir = fs::path(argv[++i]);
        } else if (a == L"--verbose" || a == L"-v") {
            verbose = true;
        } else {
            modelPath = fs::path(a);
        }
    }
    if (modelPath.empty()) {
        printf("usage: pbr_tex_probe <model.pmx|.pmd> [--dump <dir>] [--verbose]\n");
        return 2;
    }

    PmxModel pmx;
    std::string err;
    const bool pmxFile = modelPath.extension() == L".pmx";
    if (!(pmxFile ? LoadPmx(modelPath, pmx, &err) : LoadPmd(modelPath, pmx, &err))) {
        printf("load failed: %s: %s\n", PathToUtf8(modelPath).c_str(), err.c_str());
        return 1;
    }

    PbrModelData pbr;
    BuildPbrModelData(modelPath, pmx.materials, PathToUtf8(modelPath.filename()), pbr);
    printf("model          : %s\n", PathToUtf8(modelPath).c_str());
    printf("materials      : %d (%d with a Ray record)\n", (int)pmx.materials.size(), pbr.materialsWithRecord);
    printf("file textures  : %d (missing %d, vram %.1f MB)\n", (int)pbr.texturePaths.size(), pbr.filesMissing,
           (double)pbr.textureBytes / (1024.0 * 1024.0));

    // Channel -> colour label (a single-channel map is uploaded as R8).
    auto ChannelLabel = [](int ch) {
        switch (ch) {
        case -1: return "rgba";
        case 0: return "R";
        case 1: return "G";
        case 2: return "B";
        default: return "A";
        }
    };

    printf("\n== decoded file textures ==\n");
    for (size_t i = 0; i < pbr.texturePaths.size(); ++i) {
        const ImageRGBA8& img = pbr.textures[i];
        const int ch = i < pbr.textureChannels.size() ? pbr.textureChannels[i] : -1;
        if (img.Empty()) {
            printf("  [%2zu] %-7s (decode failed) %s\n", i, ChannelLabel(ch), PathToUtf8(pbr.texturePaths[i]).c_str());
            continue;
        }
        printf("  [%2zu] %-7s %4ux%-4u mips=%-3d l0=%zu B  %s\n", i, ChannelLabel(ch), img.Width(), img.Height(),
               (int)img.mips.size(), img.mips[0].pixels.size(), PathToUtf8(pbr.texturePaths[i]).c_str());
        if (!dumpDir.empty()) {
            fs::create_directories(dumpDir);
            char name[64];
            std::snprintf(name, sizeof(name), "tex%02zu_%s.png", i, ChannelLabel(ch));
            const fs::path out = dumpDir / name;
            if (SavePngRGBA8(out, img.mips[0].width, img.mips[0].height, img.mips[0].pixels.data(),
                             img.mips[0].width * 4))
                printf("        -> %s\n", PathToUtf8(out).c_str());
            else
                printf("        -> write failed: %s\n", PathToUtf8(out).c_str());
        }
    }

    // UV span + true coverage per material (from the PMX vertices its index range touches). The triangle UV area
    // is the honest texel-density measure: a tiny UV area over a large screen area magnifies the map even at mip 0.
    struct UvStats {
        XMFLOAT2 lo{1e30f, 1e30f}, hi{-1e30f, -1e30f};
        double area = 0;   // sum of |triangle UV area|
    };
    std::vector<UvStats> uvStats(pmx.materials.size());
    uint32_t idxStart = 0;
    for (size_t mi = 0; mi < pmx.materials.size(); ++mi) {
        const uint32_t end = std::min<uint32_t>(idxStart + pmx.materials[mi].indexCount, (uint32_t)pmx.indices.size());
        UvStats& st = uvStats[mi];
        for (uint32_t i = idxStart; i + 2 < end; i += 3) {
            const uint32_t ia = pmx.indices[i], ib = pmx.indices[i + 1], ic = pmx.indices[i + 2];
            if (ia >= pmx.vertices.size() || ib >= pmx.vertices.size() || ic >= pmx.vertices.size()) continue;
            const XMFLOAT2& a = pmx.vertices[ia].uv;
            const XMFLOAT2& b = pmx.vertices[ib].uv;
            const XMFLOAT2& c = pmx.vertices[ic].uv;
            for (const XMFLOAT2* p : {&a, &b, &c}) {
                st.lo.x = std::min(st.lo.x, p->x);
                st.lo.y = std::min(st.lo.y, p->y);
                st.hi.x = std::max(st.hi.x, p->x);
                st.hi.y = std::max(st.hi.y, p->y);
            }
            st.area += std::abs((double)(b.x - a.x) * (c.y - a.y) - (double)(c.x - a.x) * (b.y - a.y)) * 0.5;
        }
        idxStart = end;
    }

    printf("\n== material records ==\n");
    for (size_t mi = 0; mi < pbr.materials.size(); ++mi) {
        const PbrMaterial& m = pbr.materials[mi];
        const PmxMaterial& pm = mi < pmx.materials.size() ? pmx.materials[mi] : PmxMaterial{};
        if (!m.valid) {
            if (verbose) printf("[%3zu] %-28s (no Ray record)\n", mi, pm.name.c_str());
            continue;
        }
        const auto& box = uvStats[mi];
        const bool haveUv = box.hi.x >= box.lo.x;
        char uvbuf[128] = "";
        if (haveUv)
            std::snprintf(uvbuf, sizeof(uvbuf), " uv=[%.3f,%.3f]-[%.3f,%.3f] uvArea=%.4f", box.lo.x, box.lo.y,
                          box.hi.x, box.hi.y, box.area);
        const fs::path pmxTex = pmx.ResolveTexturePath(pm.textureIndex);
        printf("[%3zu] %-28s model=%d alpha=%.3f emissive=%d%s\n", mi, pm.name.c_str(), m.shadingModel,
               m.alphaThreshold, m.emissiveEnable ? 1 : 0, uvbuf);
        printf("      pmx.diffuse[%d] %s\n", pm.textureIndex,
               pmxTex.empty() ? "(none)" : PathToUtf8(pmxTex).c_str());
        for (int si = 0; si < kPbrSlotCount; ++si) {
            const PbrSlot& s = m.slots[si];
            const char* kind = "none";
            switch (s.tex.kind) {
            case PbrTexKind::File: kind = "file"; break;
            case PbrTexKind::PmxDiffuse: kind = "pmx.tex"; break;
            case PbrTexKind::PmxSphere: kind = "pmx.sph"; break;
            case PbrTexKind::PmxToon: kind = "pmx.toon"; break;
            default: break;
            }
            const bool interesting = s.from != 0 || s.hasValue || s.tex.kind != PbrTexKind::None;
            if (!interesting && !verbose) continue;
            const bool single = s.tex.kind == PbrTexKind::File;
            printf("    %-11s from=%d type=%d swiz=%d uvFlip=%d %-8s loop=%.3f,%.3f uvScale=%.3f,%.3f", PbrSlotName((PbrSlotId)si),
                   s.from, s.type, s.swizzle, s.uvFlip, kind, s.loopU, s.loopV, s.uvScaleU, s.uvScaleV);
            if (s.hasValue) printf(" value=%.3f,%.3f,%.3f", s.value[0], s.value[1], s.value[2]);
            printf("\n");
            if (s.tex.kind == PbrTexKind::File) {
                const int idx = s.tex.fileIndex;
                const fs::path& p = (idx >= 0 && (size_t)idx < pbr.texturePaths.size()) ? pbr.texturePaths[idx] : s.tex.path;
                printf("        file[%d] %s%s\n", idx, PathToUtf8(p).c_str(),
                       single ? "" : " (rgba)");
            }
        }
    }
    return 0;
}
