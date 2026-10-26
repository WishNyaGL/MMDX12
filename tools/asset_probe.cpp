#include "asset/AssetLibrary.h"
#include "asset/DdsLoader.h"
#include "asset/ImageLoader.h"
#include "asset/MmeHints.h"
#include "asset/ModelImport.h"
#include "asset/PmxModel.h"
#include "asset/VmdMotion.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <Windows.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

std::string Rel(const std::filesystem::path& p, const std::filesystem::path& root) {
    std::string rel = mmdx::PathToUtf8(p.lexically_relative(root));
    for (char& c : rel) {
        if (c == '\\') c = '/';
    }
    return rel;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);

    std::filesystem::path libraryDir;
    std::filesystem::path mmeModel;
    bool full = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--full") full = true;
        else if (arg == L"--mme" && i + 1 < argc) mmeModel = argv[++i];
        else if (libraryDir.empty()) libraryDir = arg;
    }
    if (!mmeModel.empty()) {
        const mmdx::MmeHints hints = mmdx::ReadMmeHints(mmeModel);
        printf("mme %s: found=%d textures=%d defines=%d\n", mmdx::PathToUtf8(mmeModel).c_str(), hints.found ? 1 : 0,
               (int)hints.textures.size(), (int)hints.defines.size());
        for (const auto& t : hints.textures)
            printf("  texture %s = %s\n", t.variable.c_str(), mmdx::PathToUtf8(t.file).c_str());
        for (const auto& [k, v] : hints.defines) printf("  define %s = %s\n", k.c_str(), v.c_str());
        return 0;
    }
    if (libraryDir.empty()) {
        fprintf(stderr, "usage: asset_probe <libraryDir> [--full] | --mme <model>\n");
        return 2;
    }

    const mmdx::LibraryScanResult scan = mmdx::ScanLibrary(libraryDir, nullptr);

    printf("root: %s\n", mmdx::PathToUtf8(scan.root).c_str());
    printf("characters (%d):\n", static_cast<int>(scan.characters.size()));
    for (const auto& c : scan.characters) {
        printf("  %s | %s | %s | v=%u b=%u m=%u\n", c.displayName.c_str(), c.id.c_str(), c.format.c_str(),
               c.vertexCount, c.boneCount, c.materialCount);
    }
    printf("stages (%d):\n", static_cast<int>(scan.stages.size()));
    for (const auto& s : scan.stages) {
        printf("  %s | %s | %s | parts=%d v=%u\n", s.displayName.c_str(), s.id.c_str(), s.format.c_str(),
               static_cast<int>(s.parts.size()), s.vertexCount);
        for (const auto& part : s.parts) {
            printf("    - %s\n", Rel(part, scan.root).c_str());
        }
    }
    printf("songs (%d):\n", static_cast<int>(scan.songs.size()));
    for (const auto& s : scan.songs) {
        printf("  %s | %s | %.1fs | dance=%s | cam=%s | audio=%s | extra=%d%s\n",
               s.displayName.c_str(), s.id.c_str(), s.durationSec,
               s.danceVmd.empty() ? "-" : Rel(s.danceVmd, scan.root).c_str(),
               s.cameraVmd.empty() ? "-" : Rel(s.cameraVmd, scan.root).c_str(),
               s.audioPath.empty() ? "-" : Rel(s.audioPath, scan.root).c_str(),
               static_cast<int>(s.extraVmds.size()),
               s.parts.size() >= 2 ? (" | parts=" + std::to_string(s.parts.size())).c_str() : "");
        if (!s.bundledStageId.empty())
            printf("    - bundledStage: %s\n", s.bundledStageId.c_str());
        if (!s.stageVmd.empty())
            printf("    - stageMotion: %s\n", Rel(s.stageVmd, scan.root).c_str());
        if (s.stageVmds.size() >= 2)
            for (const auto& v : s.stageVmds)
                printf("    - stageMotion%s: %s\n", v == s.stageVmd ? " (best)" : "", Rel(v, scan.root).c_str());
        if (s.audioPaths.size() >= 2)
            for (const auto& a : s.audioPaths)
                printf("    - audio%s: %s\n", a == s.audioPath ? " (default)" : "", Rel(a, scan.root).c_str());
        if (s.cameraVmds.size() >= 2)
            for (const auto& c : s.cameraVmds)
                printf("    - camera%s: %s\n", c == s.cameraVmd ? " (default)" : "", Rel(c, scan.root).c_str());
        if (s.parts.size() >= 2) {
            for (size_t pi = 0; pi < s.parts.size(); ++pi) {
                const auto& p = s.parts[pi];
                printf("    - part%s: %s | %s | extra=%d\n", (int)pi == s.centerPart ? " (center)" : "", p.label.c_str(),
                       p.danceVmd.empty() ? "-" : Rel(p.danceVmd, scan.root).c_str(),
                       static_cast<int>(p.extraVmds.size()));
            }
        }
    }
    printf("warnings (%d):\n", static_cast<int>(scan.warnings.size()));
    for (const auto& w : scan.warnings) {
        printf("  %s\n", w.c_str());
    }
    printf("notes (%d):\n", static_cast<int>(scan.notes.size()));
    for (const auto& n : scan.notes) printf("  %s\n", n.c_str());
    printf("scan time: %.2fs\n", scan.scanSeconds);

    if (full) {
        printf("\n--- full load ---\n");
        // S11: library-wide texture totals, printed again at the end.
        uint64_t totalRgba8 = 0, totalBc = 0, totalAllRgba8 = 0;
        std::vector<std::pair<std::filesystem::path, mmdx::ModelRole>> pmxPaths;
        for (const auto& c : scan.characters) pmxPaths.push_back({c.modelPath, mmdx::ModelRole::Character});
        for (const auto& s : scan.stages) {
            for (const auto& part : s.parts) pmxPaths.push_back({part, mmdx::ModelRole::Stage});
        }
        for (const auto& [p, role] : pmxPaths) {
            mmdx::PmxModel model;
            std::string error;
            const std::string rel = Rel(p, scan.root);
            if (!mmdx::LoadModelFile(p, role, model, &error)) {
                printf("LOAD %s: FAIL %s\n", rel.c_str(), error.c_str());
                continue;
            }
            printf("LOAD %s: OK v=%u f=%u mat=%d bones=%d morphs=%d rigid=%d joints=%d\n",
                   rel.c_str(), static_cast<uint32_t>(model.vertices.size()),
                   static_cast<uint32_t>(model.indices.size() / 3),
                   static_cast<int>(model.materials.size()),
                   static_cast<int>(model.bones.size()),
                   static_cast<int>(model.morphs.size()),
                   static_cast<int>(model.rigidBodies.size()),
                   static_cast<int>(model.joints.size()));

            // Texture indices referenced by any material (texture, sphere, non-shared toon).
            std::map<int32_t, std::string> referenced;
            for (const auto& m : model.materials) {
                auto add = [&referenced, &model](int32_t idx) {
                    if (idx >= 0 && idx < static_cast<int32_t>(model.textures.size())) {
                        referenced.emplace(idx, model.textures[static_cast<size_t>(idx)]);
                    }
                };
                add(m.textureIndex);
                add(m.sphereTextureIndex);
                if (!m.sharedToon) add(m.toonIndex);
            }
            int texOk = 0;
            std::vector<std::string> missing;
            // S11: the same VRAM totals the TEXTURES log line prints per model (RGBA8 = textures uploaded
            // uncompressed, BCn = the DDS blocks actually kept, all_rgba8 = everything as RGBA8 + mips).
            uint64_t rgba8Bytes = 0, bcBytes = 0, allRgba8Bytes = 0;
            for (const auto& [idx, raw] : referenced) {
                (void)raw;
                mmdx::ImageRGBA8 image;
                mmdx::CompressedImage compressed;
                std::string texError;
                const bool embedded = (size_t)idx < model.embeddedTextures.size() && !model.embeddedTextures[idx].empty();
                const std::filesystem::path texPath = embedded ? std::filesystem::path{} : model.ResolveTexturePath(idx);
                if (embedded ? mmdx::LoadImageWithCompressedFromMemory(model.embeddedTextures[idx].data(), model.embeddedTextures[idx].size(), image, compressed, &texError)
                             : (!texPath.empty() && mmdx::LoadImageWithCompressed(texPath, image, compressed, &texError))) {
                    ++texOk;
                    if (!compressed.Empty()) {
                        bcBytes += compressed.data.size();
                        uint64_t rgba8 = 0;
                        if (!image.Empty()) {
                            for (const mmdx::ImageRGBA8::Level& l : image.mips) rgba8 += l.pixels.size();
                        } else {
                            // No CPU decode (BC4/5/6H/7): the RGBA8 form would be level 0 + its halving chain.
                            uint32_t w = compressed.width, h = compressed.height;
                            while (w > 0 && h > 0) {
                                rgba8 += (uint64_t)w * h * 4;
                                if (w == 1 && h == 1) break;
                                w = std::max(1u, w / 2);
                                h = std::max(1u, h / 2);
                            }
                        }
                        allRgba8Bytes += rgba8;
                    } else if (!image.Empty()) {
                        uint64_t rgba8 = 0;
                        for (const mmdx::ImageRGBA8::Level& l : image.mips) rgba8 += l.pixels.size();
                        rgba8Bytes += rgba8;
                        allRgba8Bytes += rgba8;
                    }
                } else {
                    missing.push_back(raw);
                }
            }
            printf("  textures: %d/%d ok\n", texOk, static_cast<int>(referenced.size()));
            {
                const uint64_t uploaded = rgba8Bytes + bcBytes;
                printf("  vram: rgba8=%.2f MB bcn=%.2f MB all_rgba8=%.2f MB saved=%.1f%%\n",
                       (double)rgba8Bytes / (1024.0 * 1024.0), (double)bcBytes / (1024.0 * 1024.0),
                       (double)allRgba8Bytes / (1024.0 * 1024.0),
                       allRgba8Bytes && allRgba8Bytes > uploaded
                           ? 100.0 * (double)(allRgba8Bytes - uploaded) / (double)allRgba8Bytes
                           : 0.0);
                totalRgba8 += rgba8Bytes;
                totalBc += bcBytes;
                totalAllRgba8 += allRgba8Bytes;
            }
            for (const auto& raw : missing) {
                printf("  missing: %s\n", raw.c_str());
            }
        }

        std::vector<std::filesystem::path> vmdPaths;
        for (const auto& s : scan.songs) {
            if (!s.danceVmd.empty()) vmdPaths.push_back(s.danceVmd);
            if (!s.cameraVmd.empty()) vmdPaths.push_back(s.cameraVmd);
            for (const auto& e : s.extraVmds) vmdPaths.push_back(e);
        }
        for (const auto& p : vmdPaths) {
            mmdx::VmdMotion motion;
            std::string error;
            const std::string rel = Rel(p, scan.root);
            if (!mmdx::LoadVmd(p, motion, &error)) {
                printf("VMD %s: FAIL %s\n", rel.c_str(), error.c_str());
                continue;
            }
            printf("VMD %s: bones=%d morphs=%d cams=%d ik=%d maxFrame=%u\n", rel.c_str(),
                   static_cast<int>(motion.boneKeys.size()),
                   static_cast<int>(motion.morphKeys.size()),
                   static_cast<int>(motion.cameraKeys.size()),
                   static_cast<int>(motion.ikKeys.size()), motion.maxFrame);
        }
        {
            const uint64_t uploaded = totalRgba8 + totalBc;
            printf("\n--- texture totals ---\n");
            printf("rgba8=%.2f MB bcn=%.2f MB all_rgba8=%.2f MB saved=%.1f%%\n",
                   (double)totalRgba8 / (1024.0 * 1024.0), (double)totalBc / (1024.0 * 1024.0),
                   (double)totalAllRgba8 / (1024.0 * 1024.0),
                   totalAllRgba8 && totalAllRgba8 > uploaded
                       ? 100.0 * (double)(totalAllRgba8 - uploaded) / (double)totalAllRgba8
                       : 0.0);
        }
    }

    const bool atLeastOneCharacter = !scan.characters.empty();
    const bool atLeastOneSong = !scan.songs.empty();
    return (atLeastOneCharacter && atLeastOneSong) ? 0 : 1;
}
