#include "render/ShaderPack.h"

#include <Windows.h>

#include <json.hpp>

#include "core/I18n.h"
#include "core/Log.h"
#include "core/NetUtil.h"
#include "core/TextUtil.h"
#include "render/LookPreset.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace mmdx {

namespace {

namespace fs = std::filesystem;

constexpr const char* kManifestName = "pack.json";

bool ParseClass(const std::string& s, PackClass& out) {
    static const std::pair<const char*, PackClass> kNames[] = {
        {"body", PackClass::Body}, {"skin", PackClass::Skin}, {"face", PackClass::Face},
        {"eye", PackClass::Eye},   {"hair", PackClass::Hair}, {"weapon", PackClass::Weapon},
    };
    for (const auto& [name, cls] : kNames)
        if (s == name) { out = cls; return true; }
    return false;
}

bool IsValidHlslIdentifier(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (s[0] >= '0' && s[0] <= '9') return false;
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

std::string Str(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

float Num(const nlohmann::json& j, const char* key, float def) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : def;
}

bool ParseHexColor(const std::string& hex, float outRgba[4]) {
    std::string s = hex;
    if (!s.empty() && s[0] == '#') s = s.substr(1);
    if (s.size() == 6) {
        unsigned int val = 0;
        if (sscanf(s.c_str(), "%x", &val) == 1) {
            outRgba[0] = ((val >> 16) & 0xFF) / 255.0f;
            outRgba[1] = ((val >> 8) & 0xFF) / 255.0f;
            outRgba[2] = (val & 0xFF) / 255.0f;
            outRgba[3] = 1.0f;
            return true;
        }
    } else if (s.size() == 8) {
        unsigned int r = 0, g = 0, b = 0, a = 0;
        if (sscanf(s.c_str(), "%02x%02x%02x%02x", &r, &g, &b, &a) == 4) {
            outRgba[0] = r / 255.0f;
            outRgba[1] = g / 255.0f;
            outRgba[2] = b / 255.0f;
            outRgba[3] = a / 255.0f;
            return true;
        }
    }
    return false;
}

// A plain JSON string (stored under "") or an object of language -> string.
LocalizedText Text(const nlohmann::json& j, const char* key) {
    LocalizedText t;
    auto it = j.find(key);
    if (it == j.end()) return t;
    if (it->is_string()) {
        t.byLang[""] = it->get<std::string>();
    } else if (it->is_object()) {
        for (const auto& [lang, v] : it->items())
            if (v.is_string()) t.byLang[lang] = v.get<std::string>();
    }
    return t;
}

bool HttpUrl(const std::string& s) { return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0; }

bool ParsePackInput(const std::string& s, PackInput& out) {
    for (uint8_t i = 0; i < (uint8_t)PackInput::Count; ++i) {
        const PackInput in = (PackInput)i;
        if (s == PackInputName(in)) {
            out = in;
            return true;
        }
    }
    return false;
}

// ---- small file helpers (all filesystem calls take an error_code, never throw) --------------------------------------

uint64_t Fnv(uint64_t h, const void* data, size_t n) {
    const auto* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

void FnvString(uint64_t& h, const std::string& s) {
    h = Fnv(h, s.data(), s.size());
    h = Fnv(h, "|", 1);
}

// (relative path, size, last_write_time) of every file in the folder: the pack's file stamp.
uint64_t FolderStamp(const fs::path& dir) {
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    std::vector<fs::path> files;
    while (!ec && it != end) {
        std::error_code e2;
        if (it->is_regular_file(e2) && !e2) files.push_back(it->path());
        it.increment(e2);
        ec = e2;
    }
    std::sort(files.begin(), files.end());
    uint64_t h = 1469598103934665603ull;
    for (const fs::path& f : files) {
        FnvString(h, PathToUtf8(f.lexically_relative(dir)));
        std::error_code e2;
        const uint64_t size = (uint64_t)fs::file_size(f, e2);
        h = Fnv(h, &size, sizeof(size));
        const auto t = fs::last_write_time(f, e2).time_since_epoch().count();
        h = Fnv(h, &t, sizeof(t));
    }
    return h;
}

// pack.json / *.hlsl / *.hlsli / *.png / *.jpg / preview.* of every pack folder, plus the folder list: cheap, stat only.
// Textures live in subfolders ("textures/x.png"), so this walks the pack recursively.
uint64_t PollStamp(const fs::path roots[2]) {
    uint64_t h = 1469598103934665603ull;
    std::error_code ec;
    for (int r = 0; r < 2; ++r) {
        FnvString(h, PathToUtf8(roots[r]));
        if (!fs::is_directory(roots[r], ec)) {
            FnvString(h, "missing");
            continue;
        }
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(roots[r], ec)) {
            std::error_code e2;
            if (e.is_directory(e2)) dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& d : dirs) {
            FnvString(h, PathToUtf8(d.filename()));
            const std::string rel = PathToUtf8(d);
            std::vector<fs::path> watched;
            std::error_code e2;
            fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, e2), end;
            while (!e2 && it != end) {
                std::error_code e3;
                if (it->is_regular_file(e3) && !e3) {
                    const std::string name = PathToUtf8(it->path().filename());
                    const std::string ext = ToLowerAscii(PathToUtf8(it->path().extension()));
                    if (name == kManifestName || ext == ".hlsl" || ext == ".hlsli" || name == "preview.png" ||
                        name == "preview.jpg" || ext == ".png" || ext == ".jpg" || ext == ".jpeg")
                        watched.push_back(it->path());
                }
                it.increment(e3);
                e2 = e3;
            }
            std::sort(watched.begin(), watched.end());
            for (const fs::path& f : watched) {
                std::error_code e3;
                FnvString(h, PathToUtf8(fs::relative(f, d, e3)));
                const auto t = fs::last_write_time(f, e3).time_since_epoch().count();
                h = Fnv(h, &t, sizeof(t));
            }
        }
    }
    return h;
}

// true when the path itself, or anything inside it, is a symlink or junction (a reparse point).
bool HasReparsePoint(const fs::path& dir) {
    std::error_code ec;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        const DWORD a = GetFileAttributesW(it->path().c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        it.increment(ec);
    }
    return false;
}

// pack.json, *.hlsl, *.hlsli, *.png, *.jpg, *.md, *.txt, LICENSE*
bool AllowedPackFile(const fs::path& f) {
    const std::string name = PathToUtf8(f.filename());
    if (name == kManifestName || name.rfind("LICENSE", 0) == 0) return true;
    const std::string ext = ToLowerAscii(PathToUtf8(f.extension()));
    return ext == ".hlsl" || ext == ".hlsli" || ext == ".png" || ext == ".jpg" || ext == ".md" || ext == ".txt";
}

// The pack folder's files with the registry's limits (200 files, 32 MB). Empty on a problem, with a Korean `error`.
std::vector<fs::path> PackFiles(const fs::path& root, std::string& error) {
    std::vector<fs::path> files;
    uint64_t total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) {
            it.increment(e2);
            continue;
        }
        if (files.size() == 200) {
            error = Tr("팩 안에 파일이 너무 많습니다 (200개 초과)");
            return {};
        }
        if (!AllowedPackFile(it->path())) {
            error = std::string(Tr("허용되지 않은 파일이 있습니다: ")) + PathToUtf8(it->path().filename());
            return {};
        }
        total += (uint64_t)fs::file_size(it->path(), e2);
        if (e2) {
            error = Tr("팩 안의 파일을 읽을 수 없습니다");
            return {};
        }
        files.push_back(it->path());
        it.increment(e2);
        ec = e2;
    }
    if (total > 32ull << 20) {
        error = Tr("팩이 너무 큽니다 (32MB 초과)");
        return {};
    }
    return files;
}

// ---- manifest --------------------------------------------------------------------------------------------------------

std::string JoinProblems(const std::vector<std::string>& problems) {
    std::string joined;
    for (size_t i = 0; i < problems.size(); ++i) {
        if (i) joined += "; ";
        joined += problems[i];
    }
    return joined;
}

// ---- load a pack folder ----------------------------------------------------------------------------------------------

// Reads a pack folder: pack.json is required (folders without one are not packs). A bad manifest is still returned
// (out.status = InvalidManifest) so the UI can list it with the reason.
bool LoadPackFolder(const fs::path& dir, PackSource source, ShaderPack& out) {
    std::error_code ec;
    if (!fs::is_regular_file(dir / Utf8ToPath(kManifestName), ec)) return false;
    out = {};
    out.dir = dir;
    out.source = source;
    {
        std::ifstream in(dir / Utf8ToPath(kManifestName), std::ios::binary);
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!in && json.empty()) return false;
        std::string error;
        ParseShaderPackManifest(json, dir, out, error);
    }
    // online installs carry .mmdx_install.json
    std::ifstream ins(dir / L".mmdx_install.json", std::ios::binary);
    if (ins) {
        try {
            nlohmann::json m = nlohmann::json::parse(ins);
            if (m.is_object()) {
                const auto it = m.find("from");
                if (it != m.end() && it->is_string() && !it->get<std::string>().empty()) {
                    out.source = PackSource::Online;
                    out.installedFrom = it->get<std::string>();
                }
                const auto sha = m.find("sha256");
                if (sha != m.end() && sha->is_string()) {
                    out.installedSha = sha->get<std::string>();
                    std::transform(out.installedSha.begin(), out.installedSha.end(), out.installedSha.begin(),
                                   [](char c) { return (char)tolower((unsigned char)c); });
                }
            }
        } catch (const std::exception&) {
            // a broken marker is ignored: the pack still installs as a user pack
        }
    }
    // PACK_HAS_EDGE: the pack opts in to the pack variant of the edge pass (PackEdge in surface.hlsl)
    std::ifstream surf(dir / L"surface.hlsl", std::ios::binary);
    if (surf) {
        std::string line;
        // only a "#define PACK_HAS_EDGE" line counts (a mention in a comment must not opt in)
        while (std::getline(surf, line)) {
            size_t p = line.find_first_not_of(" \t");
            if (p == std::string::npos || line[p] != '#') continue;
            p = line.find_first_not_of(" \t", p + 1);
            if (p != std::string::npos && line.compare(p, 6, "define") == 0) {
                p = line.find_first_not_of(" \t", p + 6);
                if (p != std::string::npos && line.compare(p, 13, "PACK_HAS_EDGE") == 0) {
                    out.hasEdge = true;
                    break;
                }
            }
        }
    }
    return true;
}

// A pack texture path: relative to the pack, no "..", no drive / root (the pack is copied and the
// file name ends up in an #include path's sibling folder).
bool ValidPackTexturePath(const std::string& file) {
    if (file.empty() || file.front() == '/' || file.front() == '\\') return false;
    if (file.find(':') != std::string::npos) return false;   // drive letter
    const fs::path p = Utf8ToPath(file);
    for (const auto& part : p)
        if (PathToUtf8(part) == "..") return false;
    return true;
}

} // namespace

// ---- LocalizedText / PackLanguage ------------------------------------------------------------------------------------

const std::string& LocalizedText::Get(const std::string& lang) const {
    static const std::string empty;
    if (!byLang.empty()) {
        for (const char* key : {lang.c_str(), "en", "", "ko"}) {
            const auto it = byLang.find(key);
            if (it != byLang.end()) return it->second;
        }
        return byLang.begin()->second;
    }
    return empty;
}

bool LocalizedText::Empty() const {
    for (const auto& [lang, text] : byLang)
        if (!text.empty()) return false;
    return true;
}

std::string PackLanguage() {
    switch (ActiveLanguage()) {
    case Language::Korean: return "ko";
    case Language::English: return "en";
    case Language::Japanese: return "ja";
    case Language::Chinese: return "zh";
    default: return "en";
    }
}

// ---- ShaderPack -------------------------------------------------------------------------------------------------------

const char* PackInputName(PackInput input) {
    switch (input) {
    case PackInput::PerformerHead: return "performer.head";
    case PackInput::PerformerCenter: return "performer.center";
    case PackInput::PerformerHandL: return "performer.hand_l";
    case PackInput::PerformerHandR: return "performer.hand_r";
    case PackInput::PerformerFootL: return "performer.foot_l";
    case PackInput::PerformerFootR: return "performer.foot_r";
    case PackInput::LightKey: return "light.key";
    case PackInput::CameraFocus: return "camera.focus";
    case PackInput::Audio: return "audio";
    default: return "";
    }
}

bool ShaderPack::HasCap(std::string_view cap) const {
    if (type == PackType::World && cap == "params.ext") return true;
    for (const std::string& r : requiresCaps)
        if (r == cap) return true;
    for (const PackCapability& c : kPackCapabilities)
        if (c.name == cap && c.apiVersion != 0 && apiVersion >= c.apiVersion) return true;
    return false;
}

PackClass ShaderPack::Classify(const std::string& name, const std::string& nameEn, const std::string& texture) const {
    for (const Rule& r : rules) {
        for (const std::string& m : r.match)
            if (name.find(m) != std::string::npos || nameEn.find(m) != std::string::npos) return r.cls;
        for (const std::string& t : r.texture)
            if (texture.find(t) != std::string::npos) return r.cls;
    }
    return PackClass::Body;
}

static float SrgbTransferToLinear(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.04045f ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void BuildPackParamSlots(const ShaderPack& pack, const std::map<std::string, float>& values,
                         std::vector<DirectX::XMFLOAT4>& slots) {
    slots.assign(pack.params.size(), DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f));
    for (size_t i = 0; i < pack.params.size(); ++i) {
        const ShaderPackParam& p = pack.params[i];
        switch (p.type) {
        case ShaderPackParamType::Float: {
            auto it = values.find(p.key);
            float v = it != values.end() ? std::clamp(it->second, p.min, p.max) : p.def;
            slots[i] = DirectX::XMFLOAT4(v, 0.0f, 0.0f, 0.0f);
            break;
        }
        case ShaderPackParamType::Int: {
            auto it = values.find(p.key);
            float raw = it != values.end() ? it->second : p.def;
            float v = std::round(std::clamp(raw, p.min, p.max));
            slots[i] = DirectX::XMFLOAT4(v, 0.0f, 0.0f, 0.0f);
            break;
        }
        case ShaderPackParamType::Bool: {
            auto it = values.find(p.key);
            float raw = it != values.end() ? it->second : p.def;
            float v = (raw > 0.5f) ? 1.0f : 0.0f;
            slots[i] = DirectX::XMFLOAT4(v, 0.0f, 0.0f, 0.0f);
            break;
        }
        case ShaderPackParamType::Color: {
            float srgb[4] = {p.defVec[0], p.defVec[1], p.defVec[2], p.alpha ? p.defVec[3] : 1.0f};
            for (int c = 0; c < (p.alpha ? 4 : 3); ++c) {
                std::string compKey = p.key + "#" + std::to_string(c);
                auto it = values.find(compKey);
                if (it != values.end()) srgb[c] = std::clamp(it->second, 0.0f, 1.0f);
            }
            float r = SrgbTransferToLinear(srgb[0]);
            float g = SrgbTransferToLinear(srgb[1]);
            float b = SrgbTransferToLinear(srgb[2]);
            float a = p.alpha ? srgb[3] : 1.0f;
            slots[i] = DirectX::XMFLOAT4(r, g, b, a);
            break;
        }
        case ShaderPackParamType::Vec3: {
            float vec[3] = {p.defVec[0], p.defVec[1], p.defVec[2]};
            for (int c = 0; c < 3; ++c) {
                std::string compKey = p.key + "#" + std::to_string(c);
                auto it = values.find(compKey);
                if (it != values.end()) vec[c] = std::clamp(it->second, p.min, p.max);
            }
            slots[i] = DirectX::XMFLOAT4(vec[0], vec[1], vec[2], 0.0f);
            break;
        }
        }
    }
}

void ShaderPack::ResolveSlots(const std::map<std::string, float>& values,
                              std::vector<DirectX::XMFLOAT4>& slots) const {
    BuildPackParamSlots(*this, values, slots);
}

PackParamValues ShaderPack::Resolve(const std::map<std::string, float>& values) const {
    PackParamValues out{};
    std::vector<DirectX::XMFLOAT4> slots;
    BuildPackParamSlots(*this, values, slots);
    const size_t n = std::min<size_t>(slots.size(), (size_t)kPackMaxParams);
    for (size_t i = 0; i < n; ++i) out[i] = slots[i].x;
    return out;
}

// ---- manifest parsing -------------------------------------------------------------------------------------------------

bool ParseShaderPackManifest(const std::string& json, const std::filesystem::path& dir, ShaderPack& out,
                             std::string& error) {
    namespace fs = std::filesystem;
    error.clear();
    // reset what the manifest owns (dir / source / installedFrom stay the caller's)
    out.dir = dir;
    out.preview.clear();
    out.previewBefore.clear();
    out.version.clear();
    out.apiVersion = 1;
    out.minAppVersion.clear();
    out.name = {};
    out.description = {};
    out.recommendedFor = {};
    out.suggest = {};
    out.authors.clear();
    out.license.clear();
    out.homepage.clear();
    out.repository.clear();
    out.tags.clear();
    out.rules.clear();
    for (float& k : out.alphaKey) k = 0.0f;
    out.groups.clear();
    out.params.clear();
    out.textures.clear();
    out.hasEdge = false;
    out.hasPtSurface = false;
    out.type = PackType::Surface;
    out.look.reset();
    out.world.reset();
    out.stage = PackEffectStage::Post;
    out.stateFloats = 0;
    out.passes.clear();
    out.replacesDof = false;
    out.requiresCaps.clear();
    out.inputMask = 0;
    out.status = PackStatus::Ready;
    out.statusMessage.clear();

    std::vector<std::string> problems;
    nlohmann::json j;
    bool object = false;
    try {
        j = nlohmann::json::parse(json);
        object = j.is_object();
        if (!object) problems.push_back("pack.json: not a JSON object");
    } catch (const std::exception& e) {
        problems.push_back(std::string("pack.json: ") + e.what());
    }
    if (!object) {
        out.id = PathToUtf8(dir.filename());   // at least the folder name, so the UI can list it
        out.status = PackStatus::InvalidManifest;
        out.statusMessage = JoinProblems(problems);
        error = out.statusMessage;
        return false;
    }

    // id: the manifest's, else the folder name; must be a valid pack id
    const std::string idInManifest = Str(j, "id");
    out.id = !idInManifest.empty() ? idInManifest : PathToUtf8(dir.filename());
    if (!ValidShaderPackId(out.id))
        problems.push_back("id '" + out.id + "' must be 1-64 characters of lower-case a-z, 0-9, _ or -");

    // version: required, digits and dots
    out.version = Str(j, "version");
    if (out.version.empty() || out.version.find_first_of("0123456789") == std::string::npos ||
        out.version.find_first_not_of("0123456789.") != std::string::npos)
        problems.push_back("version is required (digits and dots)");

    out.apiVersion = j.value("apiVersion", 1);
    if (auto it = j.find("apiVersion"); it != j.end() && !it->is_number())
        problems.push_back("apiVersion must be a number");
    out.minAppVersion = Str(j, "minAppVersion");
    if (auto it = j.find("format"); it != j.end() && !it->is_number())
        problems.push_back("format must be a number");

    // type: "surface" (default), "effect", "look" or "world"
    const std::string type = ToLowerAscii(Str(j, "type"));
    if (type.empty() || type == "surface") {
        out.type = PackType::Surface;
    } else if (type == "effect") {
        out.type = PackType::Effect;
        const std::string stage = ToLowerAscii(Str(j, "stage"));
        if (stage.empty() || stage == "post")
            out.stage = PackEffectStage::Post;
        else if (stage == "pre-bloom")
            out.stage = PackEffectStage::PreBloom;
        else
            problems.push_back("stage '" + Str(j, "stage") + "' must be \"post\" or \"pre-bloom\"");
    } else if (type == "look") {
        out.type = PackType::Look;
    } else if (type == "world") {
        out.type = PackType::World;
    } else {
        problems.push_back("type '" + Str(j, "type") + "' must be \"surface\", \"effect\", \"look\" or \"world\"");
    }

    // look: the LookData block of a look pack (render/LookPreset.h); a "look" key on any other pack is a problem
    if (auto it = j.find("look"); it != j.end() && !it->is_null()) {
        if (out.type != PackType::Look) {
            problems.push_back("look is only supported for look packs");
        } else {
            auto data = std::make_shared<LookData>();
            std::vector<std::string> lookProblems;
            ParseLookJson(*it, *data, lookProblems);
            for (const std::string& p : lookProblems) problems.push_back("look: " + p);
            out.look = std::move(data);
        }
    } else if (out.type == PackType::Look) {
        problems.push_back("look packs need a \"look\" object");
    }

    // world: the PackWorldData block of a world pack
    if (auto it = j.find("world"); it != j.end() && !it->is_null()) {
        if (out.type != PackType::World) {
            problems.push_back("world is only supported for world packs");
        } else if (!it->is_object()) {
            problems.push_back("world must be an object");
        } else {
            auto data = std::make_shared<PackWorldData>();
            if (auto s = it->find("step"); s != it->end() && s->is_number()) {
                float st = s->get<float>();
                if (st < 1.0f / 240.0f - 1e-6f || st > 1.0f / 15.0f + 1e-6f)
                    problems.push_back("world.step must be between 1/240 and 1/15 (" + std::to_string(st) + ")");
                else
                    data->step = st;
            }
            if (auto r = it->find("rewind"); r != it->end() && r->is_number()) {
                float rw = r->get<float>();
                if (rw < 0.0f || rw > 20.0f)
                    problems.push_back("world.rewind must be between 0 and 20 (" + std::to_string(rw) + ")");
                else
                    data->rewind = rw;
            }
            if (auto p = it->find("particles"); p != it->end()) {
                if (!p->is_array()) {
                    problems.push_back("world.particles must be an array");
                } else if (p->size() > kPackWorldMaxParticles) {
                    problems.push_back("world.particles cannot exceed " + std::to_string(kPackWorldMaxParticles) + " systems");
                } else {
                    for (size_t pi = 0; pi < p->size(); ++pi) {
                        const auto& pj = (*p)[pi];
                        if (!pj.is_object()) {
                            problems.push_back("world.particles[" + std::to_string(pi) + "] must be an object");
                            continue;
                        }
                        PackWorldParticleSystem ps;
                        ps.name = Str(pj, "name");
                        if (ps.name.empty()) problems.push_back("world.particles[" + std::to_string(pi) + "].name is required");
                        if (auto c = pj.find("count"); c != pj.end() && c->is_number_integer()) {
                            int64_t cnt = c->get<int64_t>();
                            if (cnt < 1 || cnt > (int64_t)kPackWorldMaxParticleCount)
                                problems.push_back("particle count must be 1.." + std::to_string(kPackWorldMaxParticleCount));
                            else
                                ps.count = (uint32_t)cnt;
                        } else {
                            problems.push_back("particle count is required (1.." + std::to_string(kPackWorldMaxParticleCount) + ")");
                        }
                        std::string blendStr = ToLowerAscii(Str(pj, "blend"));
                        if (blendStr == "add") ps.blend = PackWorldParticleSystem::Blend::Add;
                        else if (blendStr == "alpha" || blendStr.empty()) ps.blend = PackWorldParticleSystem::Blend::Alpha;
                        else problems.push_back("particle blend must be 'alpha' or 'add'");

                        ps.simulate = Str(pj, "simulate");
                        ps.spawn = Str(pj, "spawn");
                        if (ps.simulate.empty() && ps.spawn.empty())
                            problems.push_back("particle system '" + ps.name + "' must declare 'simulate' or 'spawn'");
                        if (!ps.simulate.empty() && !ps.spawn.empty())
                            problems.push_back("particle system '" + ps.name + "' cannot declare both 'simulate' and 'spawn'");
                        if (!ps.simulate.empty() && !IsValidHlslIdentifier(ps.simulate))
                            problems.push_back("particle simulate function '" + ps.simulate + "' must be an HLSL identifier");
                        if (!ps.spawn.empty() && !IsValidHlslIdentifier(ps.spawn))
                            problems.push_back("particle spawn function '" + ps.spawn + "' must be an HLSL identifier");

                        ps.shade = Str(pj, "shade");
                        if (ps.shade.empty())
                            problems.push_back("particle system '" + ps.name + "' must declare 'shade'");
                        else if (!IsValidHlslIdentifier(ps.shade))
                            problems.push_back("particle shade function '" + ps.shade + "' must be an HLSL identifier");

                        if (auto sz = pj.find("size"); sz != pj.end() && sz->is_array() && sz->size() >= 2) {
                            ps.sizeMin = (*sz)[0].is_number() ? (*sz)[0].get<float>() : 0.0f;
                            ps.sizeMax = (*sz)[1].is_number() ? (*sz)[1].get<float>() : 1.0f;
                        }
                        data->particles.push_back(std::move(ps));
                    }
                }
            }
            if (auto f = it->find("fields"); f != it->end()) {
                if (!f->is_array()) {
                    problems.push_back("world.fields must be an array");
                } else if (f->size() > kPackWorldMaxFields) {
                    problems.push_back("world.fields cannot exceed " + std::to_string(kPackWorldMaxFields) + " grids");
                } else {
                    for (size_t fi = 0; fi < f->size(); ++fi) {
                        const auto& fj = (*f)[fi];
                        if (!fj.is_object()) {
                            problems.push_back("world.fields[" + std::to_string(fi) + "] must be an object");
                            continue;
                        }
                        PackWorldField fld;
                        fld.name = Str(fj, "name");
                        if (fld.name.empty()) problems.push_back("world.fields[" + std::to_string(fi) + "].name is required");
                        if (auto sz = fj.find("size"); sz != fj.end() && sz->is_number_integer()) {
                            int64_t s = sz->get<int64_t>();
                            if (s < 64 || s > 2048)
                                problems.push_back("field size must be 64..2048 (" + std::to_string(s) + ")");
                            else
                                fld.size = (uint32_t)s;
                        }
                        if (auto ex = fj.find("extent"); ex != fj.end() && ex->is_number())
                            fld.extent = std::max(0.01f, ex->get<float>());
                        std::string fmtStr = ToLowerAscii(Str(fj, "format"));
                        if (fmtStr == "r16f") fld.format = PackWorldField::Format::R16F;
                        else if (fmtStr == "rg16f") fld.format = PackWorldField::Format::RG16F;
                        else if (fmtStr == "rgba16f" || fmtStr.empty()) fld.format = PackWorldField::Format::RGBA16F;
                        else problems.push_back("field format must be 'r16f', 'rg16f', or 'rgba16f'");

                        if (auto p = fj.find("persistent"); p != fj.end() && p->is_boolean())
                            fld.persistent = p->get<bool>();
                        if (auto rt = fj.find("rate"); rt != fj.end() && rt->is_number())
                            fld.rate = std::max(0.1f, rt->get<float>());
                        fld.update = Str(fj, "update");
                        if (fld.update.empty())
                            problems.push_back("field '" + fld.name + "' must declare 'update'");
                        else if (!IsValidHlslIdentifier(fld.update))
                            problems.push_back("field update function '" + fld.update + "' must be an HLSL identifier");

                        data->fields.push_back(std::move(fld));
                    }
                }
            }
            if (auto s = it->find("surfaces"); s != it->end()) {
                if (!s->is_array()) {
                    problems.push_back("world.surfaces must be an array");
                } else if (s->size() > kPackWorldMaxSurfaces) {
                    problems.push_back("world.surfaces cannot exceed " + std::to_string(kPackWorldMaxSurfaces) + " surfaces");
                } else {
                    for (size_t si = 0; si < s->size(); ++si) {
                        const auto& sj = (*s)[si];
                        if (!sj.is_object()) {
                            problems.push_back("world.surfaces[" + std::to_string(si) + "] must be an object");
                            continue;
                        }
                        PackWorldSurface srf;
                        srf.field = Str(sj, "field");
                        if (srf.field.empty()) problems.push_back("world.surfaces[" + std::to_string(si) + "].field is required");
                        if (auto g = sj.find("grid"); g != sj.end() && g->is_number_integer()) {
                            int64_t grid = g->get<int64_t>();
                            if (grid < 16 || grid > 512)
                                problems.push_back("surface grid must be 16..512 (" + std::to_string(grid) + ")");
                            else
                                srf.grid = (uint32_t)grid;
                        }
                        srf.height = Str(sj, "height");
                        if (srf.height.empty())
                            problems.push_back("surface must declare 'height'");
                        else if (!IsValidHlslIdentifier(srf.height))
                            problems.push_back("surface height function '" + srf.height + "' must be an HLSL identifier");

                        srf.shade = Str(sj, "shade");
                        if (srf.shade.empty())
                            problems.push_back("surface must declare 'shade'");
                        else if (!IsValidHlslIdentifier(srf.shade))
                            problems.push_back("surface shade function '" + srf.shade + "' must be an HLSL identifier");

                        data->surfaces.push_back(std::move(srf));
                    }
                }
            }
            out.world = std::move(data);
        }
    } else if (out.type == PackType::World) {
        problems.push_back("world packs need a \"world\" object");
    }

    out.name = Text(j, "name");
    if (out.name.Empty()) {
        problems.push_back("name is required");
        out.name.byLang[""] = out.id;   // a display fallback for the listing
    }
    out.description = Text(j, "description");
    out.recommendedFor = Text(j, "recommendedFor");

    // suggest: optional {"additionalUvsMin": N, "toonMapMaterialsMin": M}; unknown keys ignored
    if (auto it = j.find("suggest"); it != j.end()) {
        if (!it->is_object()) {
            problems.push_back("suggest must be an object");
        } else {
            out.suggest.present = true;
            for (auto k = it->begin(); k != it->end(); ++k)
                if (k.key() != "additionalUvsMin" && k.key() != "toonMapMaterialsMin")
                    LOG_WARN("shader pack %s: suggest: unknown key '%s' (ignored)", out.id.c_str(), k.key().c_str());
            const auto readInt = [&](const char* key, int& dst) {
                auto k = it->find(key);
                if (k == it->end()) return;
                if (!k->is_number_integer() || k->get<long long>() < 0 || k->get<long long>() > 1000000) {
                    problems.push_back(std::string("suggest.") + key + " must be a non-negative integer");
                    return;
                }
                dst = (int)k->get<long long>();
            };
            readInt("additionalUvsMin", out.suggest.additionalUvsMin);
            readInt("toonMapMaterialsMin", out.suggest.toonMapMaterialsMin);
        }
    }

    // authors: [{name, role, url}]; the old "author": "string" is one author
    if (auto a = j.find("author"); a != j.end() && a->is_string() && !a->get<std::string>().empty())
        out.authors.push_back({a->get<std::string>(), {}, {}});
    if (auto a = j.find("authors"); a != j.end() && a->is_array()) {
        int index = 0;
        for (const auto& x : *a) {
            PackAuthor au;
            if (x.is_object()) {
                au.name = Str(x, "name");
                au.role = Str(x, "role");
                au.url = Str(x, "url");
                if (!HttpUrl(au.url)) au.url.clear();
            } else if (x.is_string()) {
                au.name = x.get<std::string>();
            }
            if (au.name.empty())
                problems.push_back("authors[" + std::to_string(index) + "] needs a name");
            else
                out.authors.push_back(std::move(au));
            ++index;
        }
    }

    out.license = Str(j, "license");
    const std::string homepage = Str(j, "homepage");
    if (HttpUrl(homepage)) out.homepage = homepage;   // non-http(s) URLs are dropped
    const std::string repository = Str(j, "repository");
    if (HttpUrl(repository)) out.repository = repository;

    if (auto t = j.find("tags"); t != j.end() && t->is_array()) {
        for (const auto& x : *t) {
            if (!x.is_string()) {
                problems.push_back("tags must be strings");
                continue;
            }
            if (out.tags.size() == 12) {
                problems.push_back("more than 12 tags");
                break;
            }
            std::string tag = ToLowerAscii(x.get<std::string>());
            if (tag.size() > 24) {
                problems.push_back("tag '" + tag + "' is longer than 24 characters");
                continue;
            }
            if (!tag.empty()) out.tags.push_back(std::move(tag));
        }
    }

    // classes: class body/skin/face/eye/hair with a match array; unknown classes are a problem
    if (auto it = j.find("classes"); it != j.end()) {
        if (out.type == PackType::Look || out.type == PackType::World) problems.push_back("classes are not supported for this pack type");
    }
    if (auto it = j.find("classes"); it != j.end() && it->is_array()) {
        for (const auto& r : *it) {
            if (!r.is_object()) continue;
            ShaderPack::Rule rule;
            if (!ParseClass(Str(r, "class"), rule.cls)) {
                problems.push_back("unknown class '" + Str(r, "class") + "'");
                continue;
            }
            if (auto m = r.find("match"); m != r.end() && m->is_array())
                for (const auto& s : *m)
                    if (s.is_string() && !s.get<std::string>().empty()) rule.match.push_back(s.get<std::string>());
            if (auto m = r.find("texture"); m != r.end() && m->is_array())
                for (const auto& s : *m)
                    if (s.is_string() && !s.get<std::string>().empty()) rule.texture.push_back(s.get<std::string>());
            // "alphaKey": optional 0..1 colour-key cut-out for the class this rule assigns (bad type / range =
            // a manifest problem, like the other pack.json fields). The first rule of a class sets its key.
            if (auto a = r.find("alphaKey"); a != r.end() && !a->is_null()) {
                if (!a->is_number()) {
                    problems.push_back("class '" + Str(r, "class") + "': alphaKey must be a number between 0 and 1");
                } else {
                    const double v = a->get<double>();
                    if (v < 0.0 || v > 1.0) {
                        problems.push_back("class '" + Str(r, "class") + "': alphaKey must be between 0 and 1 (" +
                                           std::to_string(v) + ")");
                    } else {
                        const uint32_t ci = (uint32_t)rule.cls;
                        if (out.alphaKey[ci] == 0.0f) out.alphaKey[ci] = (float)v;   // first rule of the class wins
                    }
                }
            }
            if (!rule.match.empty() || !rule.texture.empty()) out.rules.push_back(std::move(rule));
        }
    }

    // requires: optional array of capability names (kPackCapabilities). Unknown = Incompatible: an app that lacks a
    // feature refuses the pack cleanly instead of calling the manifest invalid.
    std::vector<std::string> unknownCaps;
    if (auto it = j.find("requires"); it != j.end()) {
        if (!it->is_array()) {
            problems.push_back("requires must be an array of capability names");
        } else {
            for (const auto& r : *it) {
                if (!r.is_string()) {
                    problems.push_back("requires entries must be strings");
                    continue;
                }
                const std::string cap = r.get<std::string>();
                bool known = false;
                for (const PackCapability& c : kPackCapabilities) known |= c.name == cap;
                if (!known) {
                    unknownCaps.push_back(cap);
                    continue;
                }
                out.requiresCaps.push_back(cap);
            }
        }
    }

    // groups: optional array of { "id": "...", "label": "...", "collapsed": bool }
    if (auto it = j.find("groups"); it != j.end()) {
        if (!out.HasCap("params.ext")) {
            problems.push_back("groups require \"params.ext\" in requires");
        }
        if (!it->is_array()) {
            problems.push_back("groups must be an array");
        } else {
            for (const auto& g : *it) {
                if (!g.is_object()) {
                    problems.push_back("groups entries must be objects");
                    continue;
                }
                ShaderPackGroup grp;
                grp.id = Str(g, "id");
                if (grp.id.empty()) {
                    problems.push_back("a group id is empty");
                    continue;
                }
                bool dup = false;
                for (const auto& existing : out.groups) {
                    if (existing.id == grp.id) { dup = true; break; }
                }
                if (dup) {
                    problems.push_back("duplicate group id '" + grp.id + "'");
                    continue;
                }
                grp.label = Text(g, "label");
                if (grp.label.Empty()) grp.label.byLang[""] = grp.id;
                if (auto c = g.find("collapsed"); c != g.end() && c->is_boolean())
                    grp.collapsed = c->get<bool>();
                out.groups.push_back(std::move(grp));
            }
        }
    }

    // params: at most kPackMaxParams (or kPackMaxParamsExt with params.ext)
    const bool extParams = out.HasCap("params.ext");
    const uint32_t maxParams = extParams ? kPackMaxParamsExt : kPackMaxParams;
    if (auto it = j.find("params"); it != j.end()) {
        if (out.type == PackType::Look) problems.push_back("params are not supported for look packs");
    }
    if (auto it = j.find("params"); it != j.end() && it->is_array()) {
        if (!extParams && it->size() > kPackMaxParams) {
            problems.push_back("more than 16 params (" + std::to_string(it->size()) + ") requires \"params.ext\" in requires");
        } else if (extParams && it->size() > kPackMaxParamsExt) {
            problems.push_back("more than 128 params (" + std::to_string(it->size()) + ")");
        }
        for (const auto& p : *it) {
            if (!p.is_object()) {
                problems.push_back("params entries must be objects");
                continue;
            }
            if (out.params.size() >= maxParams) {
                break;
            }
            ShaderPackParam sp;
            sp.key = Str(p, "key");
            if (sp.key.empty()) {
                problems.push_back("a param key is empty");
                continue;
            }
            sp.label = Text(p, "label");
            if (sp.label.Empty()) sp.label.byLang[""] = sp.key;

            // type: "float" (default), "int", "bool", "color", "vec3"
            const std::string typeStr = ToLowerAscii(Str(p, "type"));
            if (typeStr.empty() || typeStr == "float") {
                sp.type = ShaderPackParamType::Float;
            } else if (typeStr == "int") {
                sp.type = ShaderPackParamType::Int;
            } else if (typeStr == "bool") {
                sp.type = ShaderPackParamType::Bool;
            } else if (typeStr == "color") {
                sp.type = ShaderPackParamType::Color;
            } else if (typeStr == "vec3") {
                sp.type = ShaderPackParamType::Vec3;
            } else {
                problems.push_back("param '" + sp.key + "': unknown type '" + Str(p, "type") + "'");
            }
            if (!extParams && sp.type != ShaderPackParamType::Float) {
                problems.push_back("param '" + sp.key + "': non-float type requires \"params.ext\" in requires");
            }

            // group
            if (auto git = p.find("group"); git != p.end()) {
                if (git->is_string()) sp.group = git->get<std::string>();
                else problems.push_back("param '" + sp.key + "': group must be a string");
            }
            if (!extParams) {
                if (!sp.group.empty())
                    problems.push_back("param '" + sp.key + "': group requires \"params.ext\" in requires");
            } else if (!out.groups.empty()) {
                bool found = false;
                for (const auto& g : out.groups) {
                    if (g.id == sp.group) { found = true; break; }
                }
                if (!found) {
                    problems.push_back("param '" + sp.key + "': group '" + sp.group + "' not declared in groups");
                }
            }

            // alpha flag for color
            if (sp.type == ShaderPackParamType::Color) {
                if (auto ait = p.find("alpha"); ait != p.end()) {
                    if (ait->is_boolean()) sp.alpha = ait->get<bool>();
                    else problems.push_back("param '" + sp.key + "': alpha must be true or false");
                }
            }

            // min / max / step
            if (sp.type == ShaderPackParamType::Int) {
                sp.min = Num(p, "min", 0.0f);
                sp.max = Num(p, "max", 100.0f);
                if (sp.max < sp.min) std::swap(sp.min, sp.max);
                sp.step = Num(p, "step", 1.0f);
            } else if (sp.type == ShaderPackParamType::Vec3) {
                sp.min = Num(p, "min", 0.0f);
                sp.max = Num(p, "max", 1.0f);
                if (sp.max < sp.min) std::swap(sp.min, sp.max);
                sp.step = Num(p, "step", 0.01f);
            } else {
                sp.min = Num(p, "min", 0.0f);
                sp.max = Num(p, "max", 1.0f);
                if (sp.max < sp.min) std::swap(sp.min, sp.max);
                sp.step = Num(p, "step", 0.0f);
            }

            // default
            auto dit = p.find("default");
            if (sp.type == ShaderPackParamType::Float) {
                sp.def = std::clamp(Num(p, "default", sp.min), sp.min, sp.max);
                sp.defVec[0] = sp.def;
            } else if (sp.type == ShaderPackParamType::Int) {
                sp.def = std::round(std::clamp(Num(p, "default", sp.min), sp.min, sp.max));
                sp.defVec[0] = sp.def;
            } else if (sp.type == ShaderPackParamType::Bool) {
                if (dit != p.end() && dit->is_boolean()) sp.defBool = dit->get<bool>();
                else if (dit != p.end() && dit->is_number()) sp.defBool = (dit->get<float>() > 0.5f);
                sp.def = sp.defBool ? 1.0f : 0.0f;
                sp.defVec[0] = sp.def;
            } else if (sp.type == ShaderPackParamType::Color) {
                sp.defVec[0] = 1.0f; sp.defVec[1] = 1.0f; sp.defVec[2] = 1.0f; sp.defVec[3] = 1.0f;
                if (dit != p.end()) {
                    if (dit->is_string()) {
                        if (!ParseHexColor(dit->get<std::string>(), sp.defVec)) {
                            problems.push_back("param '" + sp.key + "': invalid color hex string '" + dit->get<std::string>() + "'");
                        }
                    } else if (dit->is_array()) {
                        for (size_t c = 0; c < dit->size() && c < 4; ++c) {
                            if ((*dit)[c].is_number()) sp.defVec[c] = std::clamp((*dit)[c].get<float>(), 0.0f, 1.0f);
                        }
                    }
                }
                sp.def = sp.defVec[0];
            } else if (sp.type == ShaderPackParamType::Vec3) {
                sp.defVec[0] = sp.min; sp.defVec[1] = sp.min; sp.defVec[2] = sp.min; sp.defVec[3] = 0.0f;
                if (dit != p.end() && dit->is_array()) {
                    for (size_t c = 0; c < dit->size() && c < 3; ++c) {
                        if ((*dit)[c].is_number()) sp.defVec[c] = std::clamp((*dit)[c].get<float>(), sp.min, sp.max);
                    }
                }
                sp.def = sp.defVec[0];
            }

            bool duplicate = false;
            for (const ShaderPackParam& q : out.params) duplicate |= q.key == sp.key;
            if (duplicate) {
                problems.push_back("duplicate param key '" + sp.key + "'");
                continue;
            }
            out.params.push_back(std::move(sp));
        }
    }

    // textures: at most kPackMaxTextures (or kPackMaxTexturesWorld for world packs)
    const uint32_t maxTextures = (out.type == PackType::World) ? kPackMaxTexturesWorld : kPackMaxTextures;
    if (auto it = j.find("textures"); it != j.end()) {
        if (!it->is_array())
            problems.push_back("textures must be an array");
        else
            for (const auto& x : *it) {
                if (!x.is_object()) {
                    problems.push_back("textures entries must be objects");
                    continue;
                }
                if (out.textures.size() == maxTextures) {
                    LOG_WARN("shader pack %s: more than %u textures, the rest are ignored", out.id.c_str(),
                             maxTextures);
                    break;
                }
                PackTexture t;
                t.file = Str(x, "file");
                if (t.file.empty()) {
                    problems.push_back("a texture entry has no file");
                    continue;
                }
                const std::string address = ToLowerAscii(Str(x, "address"));
                if (address.empty() || address == "wrap")
                    t.clamp = false;
                else if (address == "clamp")
                    t.clamp = true;
                else {
                    problems.push_back("texture '" + t.file + "': address must be wrap or clamp");
                    continue;
                }
                if (auto s = x.find("srgb"); s != x.end() && s->is_boolean()) t.srgb = s->get<bool>();
                const std::string origin = ToLowerAscii(Str(x, "origin"));
                if (origin.empty()) {
                    std::error_code oec;
                    t.origin = fs::is_regular_file(dir / Utf8ToPath(t.file), oec) ? PackTexture::Origin::Bundled
                                                                                 : PackTexture::Origin::User;
                } else if (origin == "bundled") {
                    t.origin = PackTexture::Origin::Bundled;
                } else if (origin == "user") {
                    t.origin = PackTexture::Origin::User;
                } else {
                    problems.push_back("texture '" + t.file + "': origin must be bundled or user");
                    continue;
                }
                const std::string ext = ToLowerAscii(PathToUtf8(fs::path(Utf8ToPath(t.file)).extension()));
                if (ext != ".png" && ext != ".jpg" && ext != ".jpeg") {
                    problems.push_back("texture '" + t.file + "' must be a png / jpg / jpeg file");
                    continue;
                }
                if (!ValidPackTexturePath(t.file)) {
                    problems.push_back("texture '" + t.file + "' must be a path inside the pack (no '..', no drive)");
                    continue;
                }
                out.textures.push_back(std::move(t));
            }
    }

    // inputs: optional array of PackInput names (surface and effect packs). performer.* / light.key / camera.focus need
    // "inputs.scene", audio needs "inputs.audio".
    if (auto it = j.find("inputs"); it != j.end()) {
        if (out.type == PackType::Look) problems.push_back("inputs are not supported for look packs");
        if (!it->is_array()) {
            problems.push_back("inputs must be an array of input names");
        } else {
            for (const auto& r : *it) {
                if (!r.is_string()) {
                    problems.push_back("inputs entries must be strings");
                    continue;
                }
                const std::string name = r.get<std::string>();
                PackInput in;
                if (!ParsePackInput(name, in)) {
                    problems.push_back("inputs: unknown input '" + name + "'");
                    continue;
                }
                out.inputMask |= 1u << (uint8_t)in;
            }
            if (out.inputMask & (1u << (uint8_t)PackInput::Audio)) {
                if (!out.HasCap("inputs.audio"))
                    problems.push_back("input 'audio' needs the \"inputs.audio\" capability in requires");
            }
            uint32_t sceneInputs = 0;
            for (uint8_t i = 0; i < (uint8_t)PackInput::Audio; ++i) sceneInputs |= 1u << i;
            if (out.inputMask & sceneInputs) {
                if (!out.HasCap("inputs.scene"))
                    problems.push_back("scene inputs need the \"inputs.scene\" capability in requires");
            }
        }
    }

    // state: optional "state": { "floats": N } (1..16), effect packs only, apiVersion must be 4
    if (auto it = j.find("state"); it != j.end()) {
        if (out.type == PackType::Look) problems.push_back("state is not supported for look packs");
        if (!it->is_object()) {
            problems.push_back("state must be an object with \"floats\": N");
        } else {
            auto fit = it->find("floats");
            if (fit == it->end() || !fit->is_number_integer()) {
                problems.push_back("state.floats must be an integer between 1 and 16");
            } else {
                int64_t n = fit->get<int64_t>();
                if (n < 1 || n > 16) {
                    problems.push_back("state.floats must be between 1 and 16 (" + std::to_string(n) + ")");
                } else {
                    out.stateFloats = static_cast<uint32_t>(n);
                }
            }
        }
        if (!out.HasCap("state")) {
            problems.push_back("packs with 'state' must declare \"apiVersion\": 4 or newer");
        }
        if (out.type != PackType::Effect) {
            problems.push_back("state is only supported for effect packs");
        }
    }

    // passes (API v5): optional "passes": [ { "entry": "Name", "scale": 0.5 }, ... ], effect packs only
    if (auto it = j.find("passes"); it != j.end()) {
        if (out.type == PackType::Look) problems.push_back("passes are not supported for look packs");
        if (!it->is_array()) {
            problems.push_back("passes must be an array of { \"entry\": name, \"scale\": s }");
        } else {
            if (it->size() > kPackMaxPasses)
                problems.push_back("at most " + std::to_string(kPackMaxPasses) + " passes (" + std::to_string(it->size()) + ")");
            for (const auto& pj : *it) {
                if (out.passes.size() >= kPackMaxPasses) break;
                if (!pj.is_object()) {
                    problems.push_back("every passes entry must be an object");
                    continue;
                }
                ShaderPack::Pass pass;
                pass.entry = Str(pj, "entry");
                bool ident = !pass.entry.empty() && pass.entry.size() <= 64 &&
                             !(pass.entry[0] >= '0' && pass.entry[0] <= '9');
                for (char c : pass.entry)
                    ident &= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
                if (!ident || pass.entry == "PackEffect" || pass.entry == "PackEffectState") {
                    problems.push_back("pass entry '" + pass.entry + "' must be an HLSL function name");
                    continue;
                }
                if (auto sit = pj.find("scale"); sit != pj.end()) {
                    if (!sit->is_number() || sit->get<double>() < 1.0 / 64.0 || sit->get<double>() > 1.0) {
                        problems.push_back("pass '" + pass.entry + "': scale must be a number in 1/64 .. 1");
                        continue;
                    }
                    pass.scale = sit->get<float>();
                }
                bool passOk = true;
                for (const char* flag : {"history", "mips"}) {
                    auto fit = pj.find(flag);
                    if (fit == pj.end()) continue;
                    if (!fit->is_boolean()) {
                        problems.push_back("pass '" + pass.entry + "': " + flag + " must be true or false");
                        passOk = false;
                        continue;
                    }
                    (std::string(flag) == "history" ? pass.history : pass.mips) = fit->get<bool>();
                }
                if (!passOk) continue;
                out.passes.push_back(std::move(pass));
            }
        }
        if (!out.HasCap("passes")) problems.push_back("packs with 'passes' must declare \"apiVersion\": 5 or newer");
        for (const ShaderPack::Pass& pass : out.passes) {
            if (pass.history && !out.HasCap("passes.history"))
                problems.push_back("pass '" + pass.entry + "': history needs the \"passes.history\" capability in requires");
            if (pass.mips && !out.HasCap("passes.mips"))
                problems.push_back("pass '" + pass.entry + "': mips needs the \"passes.mips\" capability in requires");
        }
        if (out.type != PackType::Effect) problems.push_back("passes are only supported for effect packs");
    }

    // replaces (API v5): engine effects the pack draws itself while it is enabled; only "dof" is known
    if (auto it = j.find("replaces"); it != j.end()) {
        if (!it->is_array()) {
            problems.push_back("replaces must be an array of strings (\"dof\")");
        } else {
            for (const auto& r : *it) {
                if (r.is_string() && r.get<std::string>() == "dof") out.replacesDof = true;
                else problems.push_back("replaces: unknown entry (known: \"dof\")");
            }
        }
        if (!out.HasCap("replaces.dof")) problems.push_back("packs with 'replaces' must declare \"apiVersion\": 5 or newer");
        if (out.type != PackType::Effect || out.stage != PackEffectStage::PreBloom)
            problems.push_back("replaces needs an effect pack with \"stage\": \"pre-bloom\"");
    }

    // optional previews (AFTER = preview.*, BEFORE = preview_before.*); with both a .png and a .jpg the newer wins:
    // a folder unzipped over an older pack keeps the old preview.png next to a new preview.jpg
    std::error_code ec;
    const auto pickPreview = [&](const wchar_t* stem) {
        const fs::path png = dir / (std::wstring(stem) + L".png"), jpg = dir / (std::wstring(stem) + L".jpg");
        const bool hasPng = fs::is_regular_file(png, ec), hasJpg = fs::is_regular_file(jpg, ec);
        if (hasPng && hasJpg) return fs::last_write_time(jpg, ec) > fs::last_write_time(png, ec) ? jpg : png;
        return hasPng ? png : hasJpg ? jpg : fs::path();
    };
    out.preview = pickPreview(L"preview");
    out.previewBefore = pickPreview(L"preview_before");

    // the pack's shader file must match its type (an effect pack implements PackEffect in effect.hlsl; a world pack
    // implements world functions in world.hlsl; a look pack needs no shader and is never compiled)
    if (out.type == PackType::Effect) {
        if (!fs::is_regular_file(dir / L"effect.hlsl", ec)) problems.push_back("effect packs need effect.hlsl");
    } else if (out.type == PackType::World) {
        if (!fs::is_regular_file(dir / L"world.hlsl", ec)) problems.push_back("world packs need world.hlsl");
    } else if (out.type == PackType::Surface) {
        if (!fs::is_regular_file(dir / L"surface.hlsl", ec)) {
            problems.push_back("surface.hlsl is missing");
        } else {
            out.hasPtSurface = fs::is_regular_file(dir / L"pt_surface.hlsl", ec);
        }
    }
    // a look pack's bundled LUT must be an existing png inside the pack
    if (out.type == PackType::Look && out.look && !out.look->lutFile.empty()) {
        const std::string& lut = out.look->lutFile;
        const std::string ext = ToLowerAscii(PathToUtf8(fs::path(Utf8ToPath(lut)).extension()));
        if (!ValidPackTexturePath(lut) || ext != ".png") {
            problems.push_back("look.lutFile must be a png path inside the pack");
        } else if (!fs::is_regular_file(dir / Utf8ToPath(lut), ec)) {
            problems.push_back("look.lutFile not found: " + lut);
        }
    }

    if (!problems.empty()) {
        out.status = PackStatus::InvalidManifest;
        out.statusMessage = JoinProblems(problems);
        error = out.statusMessage;
        return false;
    }
    if (!unknownCaps.empty()) {
        out.status = PackStatus::Incompatible;
        out.statusMessage = "requires unsupported capability '" + unknownCaps.front() + "'";
        error = out.statusMessage;
        return false;
    }
    if (out.apiVersion > kPackApiVersion) {
        out.status = PackStatus::Incompatible;
        out.statusMessage = "pack apiVersion " + std::to_string(out.apiVersion) +
                            " is newer than this build's " + std::to_string(kPackApiVersion);
        error = out.statusMessage;
        return false;
    }
    if (!out.minAppVersion.empty() && net::CompareVersions(out.minAppVersion, MMDX12_VERSION) > 0) {
        out.status = PackStatus::Incompatible;
        out.statusMessage =
            "requires MMDX12 " + out.minAppVersion + " or newer (this build is " + MMDX12_VERSION + ")";
        error = out.statusMessage;
        return false;
    }
    return true;
}

// ---- registry ---------------------------------------------------------------------------------------------------------

std::filesystem::path ShaderPackRegistry::BuiltInRoot() const { return ExecutableDir() / L"shaders" / L"packs"; }

std::filesystem::path ShaderPackRegistry::UserRoot() const { return ExecutableDir() / L"shader_packs"; }

void ShaderPackRegistry::Scan() {
    const fs::path roots[2] = {BuiltInRoot(), UserRoot()};
    std::vector<ShaderPack> packs;
    const auto findIn = [&packs](const std::string& id) -> bool {
        for (const ShaderPack& p : packs)
            if (p.id == id) return true;
        return false;
    };
    for (int r = 0; r < 2; ++r) {
        std::error_code ec;
        if (!fs::is_directory(roots[r], ec)) continue;
        std::vector<fs::path> dirs;
        for (const auto& e : fs::directory_iterator(roots[r], ec)) {
            std::error_code e2;
            if (e.is_directory(e2)) dirs.push_back(e.path());
        }
        std::sort(dirs.begin(), dirs.end());
        for (const fs::path& dir : dirs) {
            ShaderPack pack;
            if (!LoadPackFolder(dir, r == 0 ? PackSource::BuiltIn : PackSource::User, pack)) continue;
            if (findIn(pack.id)) {
                pack.status = PackStatus::Duplicate;   // the later one, still listed
            } else {
                // a pack whose files did not change keeps its compile status + message across Scan()
                const uint64_t stamp = FolderStamp(dir);
                const auto it = statusById_.find(pack.id);
                if (it != statusById_.end() && it->second.first == stamp &&
                    (it->second.second == PackStatus::Compiled || it->second.second == PackStatus::CompileError)) {
                    pack.status = it->second.second;
                    const auto msg = messageById_.find(pack.id);
                    if (msg != messageById_.end()) pack.statusMessage = msg->second;
                }
                statusById_[pack.id] = {stamp, pack.status};
                messageById_[pack.id] = pack.statusMessage;
            }
            if (pack.status == PackStatus::InvalidManifest || pack.status == PackStatus::Incompatible)
                LOG_WARN("shader pack %s: %s", PathToUtf8(dir).c_str(), pack.statusMessage.c_str());
            packs.push_back(std::move(pack));
        }
    }
    packs_ = std::move(packs);
    stamp_ = PollStamp(roots);
    if (!revokedHashes_.empty()) {
        for (ShaderPack& p : packs_) {
            if (p.installedSha.empty()) continue;
            bool revoked = false;
            for (const std::string& h : revokedHashes_) revoked |= h == p.installedSha;
            if (revoked) {
                p.status = PackStatus::Revoked;
                p.statusMessage = "revoked by the gallery";
                statusById_[p.id] = {statusById_[p.id].first, p.status};
                messageById_[p.id] = p.statusMessage;
            }
        }
    }
    ++generation_;
    LOG_INFO("shader packs: %zu loaded (generation %u)", packs_.size(), generation_);
}

bool ShaderPackRegistry::PollChanges(double nowSeconds) {
    if (nowSeconds < nextPoll_) return false;
    nextPoll_ = nowSeconds + 0.5;   // cheap: stat only, at most twice a second
    const fs::path roots[2] = {BuiltInRoot(), UserRoot()};
    const uint64_t stamp = PollStamp(roots);
    if (stamp == stamp_) return false;
    Scan();
    return true;
}

const ShaderPack* ShaderPackRegistry::Find(const std::string& id) const {
    for (const ShaderPack& p : packs_)
        if (p.id == id) return &p;
    return nullptr;
}

void ShaderPackRegistry::ReportCompiled(const std::string& id) {
    for (ShaderPack& p : packs_) {
        if (p.id != id || p.status == PackStatus::Compiled) continue;
        p.status = PackStatus::Compiled;
        p.statusMessage.clear();
        const auto it = statusById_.find(id);
        if (it != statusById_.end()) it->second.second = PackStatus::Compiled;
        messageById_[id].clear();
    }
}

void ShaderPackRegistry::ReportError(const std::string& id, const std::string& message) {
    ++errorCount_;
    lastErrorPack_ = id;
    for (ShaderPack& p : packs_) {
        if (p.id != id) continue;
        p.status = PackStatus::CompileError;
        p.statusMessage = message;
        const auto it = statusById_.find(id);
        if (it != statusById_.end()) it->second.second = PackStatus::CompileError;
        messageById_[id] = message;
    }
}

// ---- install / uninstall ----------------------------------------------------------------------------------------------

void ShaderPackRegistry::ApplyRevocations(const std::vector<std::string>& hashes) {
    revokedHashes_ = hashes;
    if (hashes.empty()) return;
    bool changed = false;
    for (ShaderPack& p : packs_) {
        if (p.installedSha.empty()) continue;
        bool revoked = false;
        for (const std::string& h : hashes) revoked |= h == p.installedSha;
        if (!revoked || p.status == PackStatus::Revoked) continue;
        p.status = PackStatus::Revoked;
        p.statusMessage = "revoked by the gallery";
        statusById_[p.id] = {statusById_[p.id].first, p.status};
        messageById_[p.id] = p.statusMessage;
        changed = true;
        LOG_WARN("shader pack %s: %s", p.id.c_str(), p.statusMessage.c_str());
    }
    if (changed) ++generation_;
}

bool ShaderPackRegistry::InstallFolder(const std::filesystem::path& src, const std::string& installedFrom,
                                       std::string& id, std::string& error, const std::string& installedSha) {
    std::error_code ec;
    // pack.json at the root, or one level below (zips usually wrap the pack folder)
    fs::path root = src;
    if (!fs::is_regular_file(src / Utf8ToPath(kManifestName), ec)) {
        std::vector<fs::path> candidates;
        for (const auto& e : fs::directory_iterator(src, ec)) {
            std::error_code e2;
            if (fs::is_regular_file(e.path() / Utf8ToPath(kManifestName), e2)) candidates.push_back(e.path());
        }
        if (candidates.empty()) {
            error = Tr("pack.json을 찾을 수 없습니다");
            return false;
        }
        if (candidates.size() > 1) {
            error = Tr("pack.json이 여러 개 있어 하나를 고를 수 없습니다");
            return false;
        }
        root = candidates[0];
    }
    if (HasReparsePoint(src) || HasReparsePoint(root)) {
        error = Tr("폴더에 바로가기(링크)가 있어 설치할 수 없습니다");
        return false;
    }

    // the manifest is validated before anything is copied
    ShaderPack parsed;
    {
        std::ifstream in(root / Utf8ToPath(kManifestName), std::ios::binary);
        if (!in) {
            error = Tr("pack.json을 읽을 수 없습니다");
            return false;
        }
        std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::string parseError;
        if (!ParseShaderPackManifest(json, root, parsed, parseError)) {
            error = std::string(Tr("매니페스트에 문제가 있습니다: ")) + parseError;
            return false;
        }
    }
    for (const ShaderPack& p : packs_)
        if (p.id == parsed.id && p.source == PackSource::BuiltIn) {
            error = std::string(Tr("기본 제공 팩과 같은 id입니다: ")) + parsed.id;
            return false;
        }

    const std::vector<fs::path> files = PackFiles(root, error);
    if (files.empty()) return false;

    // copy into <id>.tmp, then rename over the old folder: never leave a half-copied pack
    fs::create_directories(UserRoot(), ec);
    const fs::path dest = UserRoot() / Utf8ToPath(parsed.id);
    const fs::path tmp = UserRoot() / Utf8ToPath(parsed.id + ".tmp");
    fs::remove_all(tmp, ec);
    for (const fs::path& f : files) {
        std::error_code e2;
        const fs::path target = tmp / f.lexically_relative(root);
        fs::create_directories(target.parent_path(), e2);
        fs::copy_file(f, target, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            fs::remove_all(tmp, ec);
            error = Tr("팩을 복사하지 못했습니다");
            return false;
        }
    }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove_all(tmp, ec);
        error = Tr("팩 폴더를 옮기지 못했습니다");
        return false;
    }
    if (!installedFrom.empty()) {
        SYSTEMTIME st;
        GetSystemTime(&st);
        char iso[40];
        std::snprintf(iso, sizeof(iso), "%04d-%02d-%02dT%02d:%02d:%02dZ", st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);
        const nlohmann::json marker = {{"from", installedFrom}, {"version", parsed.version},
                                       {"sha256", installedSha}, {"installedAt", iso}};
        std::ofstream out(dest / L".mmdx_install.json", std::ios::binary);
        out << marker.dump(2);
    }
    id = parsed.id;
    Scan();
    return true;
}

bool ShaderPackRegistry::InstallZip(const std::filesystem::path& zip, const std::string& installedFrom,
                                    std::string& id, std::string& error) {
    std::error_code ec;
    // %TEMP%/mmdx12_pack_<random>, always deleted (the cleanup guard runs on every return)
    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    char name[64];
    std::snprintf(name, sizeof(name), "mmdx12_pack_%08x",
                  (unsigned)((GetTickCount64() ^ (GetCurrentProcessId() * 2654435761u)) & 0xffffffffu));
    const fs::path temp = fs::path(tempPath) / Utf8ToPath(name);
    struct Cleanup {
        const fs::path& path;
        ~Cleanup() {
            std::error_code e;
            fs::remove_all(path, e);
        }
    } guard{temp};

    std::string extractError;
    if (!net::ExtractZip(zip, temp, extractError)) {
        error = std::string(Tr("압축을 풀지 못했습니다: ")) + extractError;
        return false;
    }
    // zip-slip: every extracted path must stay inside the temp dir (no symlinks / junctions either)
    if (HasReparsePoint(temp)) {
        error = Tr("압축 안에 바로가기(링크)가 있어 설치할 수 없습니다");
        return false;
    }
    const fs::path canon = fs::weakly_canonical(temp, ec);
    if (ec) {
        error = Tr("임시 폴더를 확인하지 못했습니다");
        return false;
    }
    fs::recursive_directory_iterator it(temp, fs::directory_options::skip_permission_denied, ec), end;
    while (!ec && it != end) {
        std::error_code e2;
        const fs::path c = fs::weakly_canonical(it->path(), e2);
        if (!e2) {
            auto pi = c.begin();
            auto bi = canon.begin();
            for (; bi != canon.end(); ++bi, ++pi)
                if (pi == c.end() || *pi != *bi) {
                    error = Tr("압축 안의 파일이 임시 폴더 밖으로 나가려 했습니다");
                    return false;
                }
        }
        it.increment(e2);
        ec = e2;
    }
    return InstallFolder(temp, installedFrom, id, error, net::Sha256OfFile(zip, nullptr));
}

bool ShaderPackRegistry::Uninstall(const std::string& id, std::string& error) {
    const ShaderPack* p = Find(id);
    if (!p) {
        error = Tr("설치된 팩을 찾을 수 없습니다");
        return false;
    }
    if (p->source == PackSource::BuiltIn) {
        error = Tr("기본 제공 팩은 삭제할 수 없습니다");
        return false;
    }
    std::error_code ec;
    fs::remove_all(p->dir, ec);
    if (ec) {
        error = Tr("팩 폴더를 삭제하지 못했습니다");
        return false;
    }
    Scan();
    return true;
}

bool ShaderPackRegistry::CreateFromTemplate(const std::string& id, const std::string& name,
                                            const std::string& author, std::filesystem::path& dir,
                                            std::string& error, bool effect) {
    if (!ValidShaderPackId(id)) {
        error = Tr("id는 소문자 a-z, 0-9, _, -만 사용할 수 있습니다");
        return false;
    }
    if (Find(id)) {
        error = Tr("같은 id의 팩이 이미 있습니다");
        return false;
    }
    const fs::path tmpl = ExecutableDir() / L"shaders" / (effect ? L"pack_template_effect" : L"pack_template");
    std::error_code ec;
    if (!fs::is_directory(tmpl, ec)) {
        error = Tr("템플릿 폴더를 찾을 수 없습니다");
        return false;
    }
    fs::create_directories(UserRoot(), ec);
    const fs::path dest = UserRoot() / Utf8ToPath(id);
    fs::remove_all(dest, ec);   // an empty stray folder (packs with this id would have failed Find above)
    fs::copy(tmpl, dest, fs::copy_options::recursive, ec);
    if (ec) {
        fs::remove_all(dest, ec);
        error = Tr("템플릿을 복사하지 못했습니다");
        return false;
    }
    // rewrite id, name and authors in the copied pack.json (keep the rest)
    const fs::path manifest = dest / Utf8ToPath(kManifestName);
    {
        std::ifstream in(manifest, std::ios::binary);
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(in);
        } catch (const std::exception&) {
            fs::remove_all(dest, ec);
            error = Tr("템플릿의 pack.json을 읽지 못했습니다");
            return false;
        }
        if (!j.is_object()) j = nlohmann::json::object();
        j["id"] = id;
        j["name"] = nlohmann::json::object({{"ko", name}, {"en", name}});
        j["authors"] = nlohmann::json::array({nlohmann::json::object({{"name", author}})});
        std::ofstream out(manifest, std::ios::binary);
        out << j.dump(2);
        if (!out) {
            fs::remove_all(dest, ec);
            error = Tr("pack.json을 쓰지 못했습니다");
            return false;
        }
    }
    dir = dest;
    Scan();
    return true;
}

bool ShaderPackRegistry::WriteLookPack(const std::filesystem::path& folder, const std::string& id,
                                       const LocalizedText& name, const std::vector<PackAuthor>& authors,
                                       const std::string& license, const LookData& look, std::string* error) {
    const auto fail = [error](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    if (!ValidShaderPackId(id)) return fail(Tr("id는 소문자 a-z, 0-9, _, -만 사용할 수 있습니다"));

    nlohmann::json authorArray = nlohmann::json::array();
    for (const PackAuthor& a : authors) {
        nlohmann::json entry = {{"name", a.name}};
        if (!a.role.empty()) entry["role"] = a.role;
        if (HttpUrl(a.url)) entry["url"] = a.url;
        authorArray.push_back(std::move(entry));
    }
    nlohmann::json manifest = {
        {"id", id},
        {"version", "1.0.0"},
        {"apiVersion", kPackApiVersion},
        {"type", "look"},
        {"name", name.byLang},
        {"authors", authorArray},
        {"license", license},
        {"requires", nlohmann::json::array()},
        {"look", LookToJson(look)},
    };

    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return fail(Tr("팩 폴더를 만들지 못했습니다"));

    const std::filesystem::path manifestPath = folder / Utf8ToPath(kManifestName);
    const std::filesystem::path tmp = folder / Utf8ToPath(std::string(kManifestName) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary);
        out << manifest.dump(2);
        out.flush();
        if (!out) {
            out.close();
            std::error_code e2;
            std::filesystem::remove(tmp, e2);
            return fail(Tr("pack.json을 쓰지 못했습니다"));
        }
    }
    std::filesystem::rename(tmp, manifestPath, ec);
    if (ec) {
        std::error_code e2;
        std::filesystem::remove(manifestPath, e2);
        std::filesystem::rename(tmp, manifestPath, ec);
        if (ec) {
            std::filesystem::remove(tmp, e2);
            return fail(Tr("pack.json을 쓰지 못했습니다"));
        }
    }
    Scan();
    return true;
}

bool EffectStackReplacesDof(const std::vector<EffectStackEntry>& stack) {
    for (const EffectStackEntry& e : stack) {
        if (!e.enabled) continue;
        const ShaderPack* pack = ShaderPacks().Find(e.pack);
        if (pack && pack->type == PackType::Effect && pack->Selectable() && pack->replacesDof) return true;
    }
    return false;
}

ShaderPackRegistry& ShaderPacks() {
    static ShaderPackRegistry registry = [] {
        ShaderPackRegistry r;
        r.Scan();
        return r;
    }();
    return registry;
}

namespace {

// The missingTexBySet_ key for one (pack id, texture folder) set.
std::string SetKey(const std::string& id, const std::filesystem::path& folder) {
    return id + "\n" + PathToUtf8(folder);
}

} // namespace

void ShaderPackRegistry::SetTextureFolder(const std::string& id, const std::filesystem::path& dir) {
    if (dir.empty()) {
        if (textureFolders_.erase(id)) ++generation_;   // pack textures reload on the next draw
        return;
    }
    const auto it = textureFolders_.find(id);
    if (it != textureFolders_.end() && it->second == dir) return;
    textureFolders_[id] = dir;
    ++generation_;
}

std::filesystem::path ShaderPackRegistry::TextureFolder(const std::string& id) const {
    const auto it = textureFolders_.find(id);
    return it == textureFolders_.end() ? std::filesystem::path() : it->second;
}

void ShaderPackRegistry::ReportMissingTextures(const std::string& id, const std::filesystem::path& folder,
                                               uint32_t missing) {
    missingTexById_[id] = missing;   // pack-level: the manager screen (the pack folder's set)
    missingTexBySet_[SetKey(id, folder)] = missing;
}

uint32_t ShaderPackRegistry::MissingTextures(const std::string& id, const std::filesystem::path& folder) const {
    const auto it = missingTexBySet_.find(SetKey(id, folder));
    return it == missingTexBySet_.end() ? 0 : it->second;
}

bool ValidShaderPackId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    // pack ids end up in settings, project files and an #include path: keep them plain
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

} // namespace mmdx
