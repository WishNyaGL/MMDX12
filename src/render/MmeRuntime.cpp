// M3a: MME runtime — GPU resources and pipelines. See MmeRuntime.h for the interface and the documented
// deviations. Any remaining open points are marked "DECISION:".
#include "render/MmeRuntime.h"
#include "asset/ImageLoader.h"
#include "asset/MmeText.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <directx/d3dx12.h>
#include <d3dcommon.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

namespace mmdx {

namespace {

// ---- small text / value helpers -----------------------------------------------------------------------------------

std::string UpperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

std::string NormVal(const std::string& raw) {
    std::string s = mmetext::Trim(raw);
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
        s = s.substr(1, s.size() - 2);
    return UpperAscii(s);
}

bool ParseBool(const std::string& raw, bool def) {
    const std::string s = NormVal(raw);
    if (s == "TRUE" || s == "1") return true;
    if (s == "FALSE" || s == "0") return false;
    return def;
}

bool ParseNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

struct NameVal { const char* name; uint32_t value; };

uint32_t ParseEnum(const std::string& raw, const NameVal* table, size_t count, uint32_t def) {
    const std::string s = NormVal(raw);
    if (s.empty()) return def;
    double num = 0;
    if (ParseNumber(s, num)) return (uint32_t)num;
    for (size_t i = 0; i < count; ++i)
        if (s == table[i].name) return table[i].value;
    return def;
}

const NameVal kBlendFactors[] = {
    {"ZERO", 1}, {"ONE", 2}, {"SRCCOLOR", 3}, {"INVSRCCOLOR", 4}, {"SRCALPHA", 5},
    {"INVSRCALPHA", 6}, {"DESTALPHA", 7}, {"INVDESTALPHA", 8}, {"DESTCOLOR", 9},
    {"INVDESTCOLOR", 10}, {"SRCALPHASAT", 11}, {"BOTHSRCALPHA", 12}, {"BOTHINVSRCALPHA", 13},
    {"BLENDFACTOR", 14}, {"INVBLENDFACTOR", 15}};
const NameVal kBlendOps[] = {{"ADD", 1}, {"SUBTRACT", 2}, {"REVSUBTRACT", 3}, {"MIN", 4}, {"MAX", 5}};
const NameVal kCompareFuncs[] = {{"NEVER", 1}, {"LESS", 2}, {"EQUAL", 3}, {"LESSEQUAL", 4}, {"GREATER", 5},
                                 {"NOTEQUAL", 6}, {"GREATEREQUAL", 7}, {"ALWAYS", 8}};
const NameVal kStencilOps[] = {{"KEEP", 1}, {"ZERO", 2}, {"REPLACE", 3}, {"INCRSAT", 4},
                               {"DECRSAT", 5}, {"INVERT", 6}, {"INCR", 7}, {"DECR", 8}};
const NameVal kCullModes[] = {{"NONE", 1}, {"CW", 2}, {"CCW", 3}};
const NameVal kFillModes[] = {{"POINT", 1}, {"WIREFRAME", 2}, {"SOLID", 3}};
const NameVal kFilterModes[] = {{"NONE", 0}, {"POINT", 1}, {"LINEAR", 2}, {"ANISOTROPIC", 3}};
const NameVal kAddressModes[] = {{"WRAP", 1}, {"MIRROR", 2}, {"CLAMP", 3}, {"BORDER", 4}, {"MIRRORONCE", 5}};

D3D12_BLEND ToD3d12Blend(uint32_t d3d9) {
    switch (d3d9) {
        case 1: return D3D12_BLEND_ZERO;
        case 2: return D3D12_BLEND_ONE;
        case 3: return D3D12_BLEND_SRC_COLOR;
        case 4: return D3D12_BLEND_INV_SRC_COLOR;
        case 5: return D3D12_BLEND_SRC_ALPHA;
        case 6: return D3D12_BLEND_INV_SRC_ALPHA;
        case 7: return D3D12_BLEND_DEST_ALPHA;
        case 8: return D3D12_BLEND_INV_DEST_ALPHA;
        case 9: return D3D12_BLEND_DEST_COLOR;
        case 10: return D3D12_BLEND_INV_DEST_COLOR;
        case 11: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 12: return D3D12_BLEND_SRC_ALPHA;       // BOTHSRCALPHA: no D3D12 equivalent
        case 13: return D3D12_BLEND_INV_SRC_ALPHA;   // BOTHINVSRCALPHA
        case 14: return D3D12_BLEND_BLEND_FACTOR;
        case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

D3D12_BLEND_OP ToD3d12BlendOp(uint32_t d3d9) {
    switch (d3d9) {
        case 1: return D3D12_BLEND_OP_ADD;
        case 2: return D3D12_BLEND_OP_SUBTRACT;
        case 3: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 4: return D3D12_BLEND_OP_MIN;
        case 5: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

D3D12_COMPARISON_FUNC ToD3d12Cmp(uint32_t d3d9) {
    switch (d3d9) {
        case 1: return D3D12_COMPARISON_FUNC_NEVER;
        case 2: return D3D12_COMPARISON_FUNC_LESS;
        case 3: return D3D12_COMPARISON_FUNC_EQUAL;
        case 4: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case 5: return D3D12_COMPARISON_FUNC_GREATER;
        case 6: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case 7: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case 8: return D3D12_COMPARISON_FUNC_ALWAYS;
        default: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
    }
}

D3D12_STENCIL_OP ToD3d12StencilOp(uint32_t d3d9) {
    switch (d3d9) {
        case 1: return D3D12_STENCIL_OP_KEEP;
        case 2: return D3D12_STENCIL_OP_ZERO;
        case 3: return D3D12_STENCIL_OP_REPLACE;
        case 4: return D3D12_STENCIL_OP_INCR_SAT;
        case 5: return D3D12_STENCIL_OP_DECR_SAT;
        case 6: return D3D12_STENCIL_OP_INVERT;
        case 7: return D3D12_STENCIL_OP_INCR;
        case 8: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3D9 D3DFORMAT names (with or without a D3DFMT_ prefix) -> DXGI. Unknown -> A8R8G8B8's format.
DXGI_FORMAT MapD3d9Format(const std::string& raw) {
    std::string s = UpperAscii(mmetext::Trim(raw));
    const std::string prefix = "D3DFMT_";
    if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
    if (s.empty()) return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (s == "A8R8G8B8" || s == "X8R8G8B8") return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (s == "A16B16G16R16F") return DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (s == "A32B32G32R32F") return DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (s == "A16B16G16R16") return DXGI_FORMAT_R16G16B16A16_UNORM;
    if (s == "R32F") return DXGI_FORMAT_R32_FLOAT;
    if (s == "G16R16F") return DXGI_FORMAT_R16G16_FLOAT;
    if (s == "G32R32F" || s == "G32R32") return DXGI_FORMAT_R32G32_FLOAT;
    if (s == "G32R32F") return DXGI_FORMAT_R32G32_FLOAT;
    if (s == "R16F") return DXGI_FORMAT_R16_FLOAT;
    if (s == "L8") return DXGI_FORMAT_R8_UNORM;
    if (s == "A8") return DXGI_FORMAT_R8_UNORM;
    if (s == "D24S8" || s == "D24X8" || s == "D24") return DXGI_FORMAT_D24_UNORM_S8_UINT;
    if (s == "D32F_LOCKABLE" || s == "D32" || s == "D32_FLOAT" || s == "D32F") return DXGI_FORMAT_D32_FLOAT;
    if (s == "D32S8X24" || s == "D32_FLOAT_S8X24") return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    if (s == "D16") return DXGI_FORMAT_D16_UNORM;
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

// Parses an OFFSCREENRENDERTARGET DefaultEffect string "pattern = effect; pattern = effect; ..." into entries in
// file order. The split is on ';'; each entry is "pattern=effect". Whitespace is trimmed; empty entries skipped.
void ParseDefaultEffect(const std::string& raw, std::vector<MmeTarget::DefaultLine>& out) {
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t semi = raw.find(';', start);
        const std::string entry = raw.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        const size_t eq = entry.find('=');
        if (eq != std::string::npos) {
            std::string pattern = mmetext::Trim(entry.substr(0, eq));
            std::string effect = mmetext::Trim(entry.substr(eq + 1));
            // strip surrounding quotes left over from the raw text
            auto unquote = [](std::string s) {
                if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front())
                    s = s.substr(1, s.size() - 2);
                return s;
            };
            pattern = unquote(pattern);
            effect = unquote(effect);
            if (!pattern.empty()) out.push_back({pattern, effect});
        }
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
}

// ---- hashing ------------------------------------------------------------------------------------------------------

uint64_t Fnv1a(uint64_t h, const void* data, size_t bytes) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < bytes; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

uint64_t Fnv1aStr(uint64_t h, const std::string& s) { return Fnv1a(h, s.data(), s.size()); }

// ---- quad vertex --------------------------------------------------------------------------------------------------

struct QuadVertex {
    float pos[4];
    float tex[4];
    float normal[4];
    float color[4];
};
static_assert(sizeof(QuadVertex) == 64, "quad vertex must be four float4s");

// ---- sampler state translation ------------------------------------------------------------------------------------

D3D12_FILTER MakeFilter(int minF, int magF, int mipF) {
    if (minF == 3 || magF == 3) return D3D12_FILTER_ANISOTROPIC;
    const D3D12_FILTER_TYPE minT = minF == 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE magT = magF == 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    const D3D12_FILTER_TYPE mipT = mipF == 2 ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    return (D3D12_FILTER)D3D12_ENCODE_BASIC_FILTER(minT, magT, mipT, D3D12_FILTER_REDUCTION_TYPE_STANDARD);
}

D3D12_TEXTURE_ADDRESS_MODE ToAddressMode(uint32_t d3d9) {
    switch (d3d9) {
        case 1: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        case 5: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

// The IR keeps sampler states in file order as UPPER name -> raw text.
std::string StateVal(const MmeSampler& s, const char* name) {
    auto it = s.states.find(name);
    return it == s.states.end() ? std::string() : it->second;
}

void DecodeBorderColor(const std::string& raw, float out[4]) {
    const std::string s = NormVal(raw);
    if (s.empty()) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    double num = 0;
    if (ParseNumber(s, num)) {
        const uint32_t v = (uint32_t)num;   // D3DCOLOR A8R8G8B8
        out[0] = ((v >> 16) & 0xFF) / 255.0f;
        out[1] = ((v >> 8) & 0xFF) / 255.0f;
        out[2] = (v & 0xFF) / 255.0f;
        out[3] = ((v >> 24) & 0xFF) / 255.0f;
    } else {
        out[0] = out[1] = out[2] = 0;
        out[3] = 1;
    }
}

D3D12_SAMPLER_DESC TranslateSampler(const MmeSampler& s) {
    D3D12_SAMPLER_DESC d{};
    const int minF = (int)ParseEnum(StateVal(s, "MINFILTER"), kFilterModes, std::size(kFilterModes), 2);
    const int magF = (int)ParseEnum(StateVal(s, "MAGFILTER"), kFilterModes, std::size(kFilterModes), 2);
    const int mipF = (int)ParseEnum(StateVal(s, "MIPFILTER"), kFilterModes, std::size(kFilterModes), 2);
    d.Filter = MakeFilter(minF, magF, mipF);
    d.AddressU = ToAddressMode(ParseEnum(StateVal(s, "ADDRESSU"), kAddressModes, std::size(kAddressModes), 1));
    d.AddressV = ToAddressMode(ParseEnum(StateVal(s, "ADDRESSV"), kAddressModes, std::size(kAddressModes), 1));
    d.AddressW = ToAddressMode(ParseEnum(StateVal(s, "ADDRESSW"), kAddressModes, std::size(kAddressModes), 1));
    double bias = 0;
    d.MipLODBias = ParseNumber(NormVal(StateVal(s, "MIPMAPLODBIAS")), bias) ? (float)bias : 0.0f;
    d.MaxAnisotropy = (UINT)std::max(1, (int)ParseEnum(StateVal(s, "MAXANISOTROPY"), nullptr, 0, 1));
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    DecodeBorderColor(StateVal(s, "BORDERCOLOR"), d.BorderColor);
    d.MinLOD = 0;
    d.MaxLOD = D3D12_FLOAT32_MAX;
    return d;
}

D3D12_SAMPLER_DESC DefaultSampler() {
    D3D12_SAMPLER_DESC d{};
    d.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    d.AddressU = d.AddressV = d.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    d.MaxAnisotropy = 1;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    d.MaxLOD = D3D12_FLOAT32_MAX;
    return d;
}

// ---- render state translation -------------------------------------------------------------------------------------

struct RenderState {
    D3D12_BLEND_DESC blend{};
    D3D12_RASTERIZER_DESC raster{};
    D3D12_DEPTH_STENCIL_DESC depth{};
    bool alphaTest = false;
};

void TranslateStates(const MmePass& pass, bool dsBound, RenderState& rs, bool objectDefaults) {
    rs.blend = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    rs.raster = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    rs.raster.CullMode = objectDefaults ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;   // object: CCW-back; post: none
    rs.raster.FillMode = D3D12_FILL_MODE_SOLID;
    rs.raster.FrontCounterClockwise = FALSE;
    rs.depth = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);

    auto val = [&](const char* name) -> const std::string* {
        for (const auto& [k, v] : pass.states)
            if (k == name) return &v;
        return nullptr;
    };
    auto getb = [&](const char* name, bool def) {
        const std::string* v = val(name);
        return v ? ParseBool(*v, def) : def;
    };
    auto gete = [&](const char* name, const NameVal* table, size_t n, uint32_t def) {
        const std::string* v = val(name);
        return v ? ParseEnum(*v, table, n, def) : def;
    };

    // Object draws default to MMD's object state (alpha blend SRC_ALPHA/INV_SRC_ALPHA, depth on); post draws keep
    // the pixel-shader defaults (blend off, depth off). A pass state overrides either way.
    // M7 fix 3: an explicit blend factor / op with no ALPHABLENDENABLE is MME's post-effect idiom for "blend is on,
    // use these factors" (MMD leaves alpha blending enabled for scene/post draws); D3D9's effect framework leaves it
    // off otherwise. Without this, a pass like ObjectLuminous' Mix (SRCBLEND=ONE / DESTBLEND=ONE) overwrites the
    // frame instead of adding to it — the Group 3 "black frame".
    bool hasBlendFactor = false;
    for (const auto& [k, v] : pass.states)
        if (k == "SRCBLEND" || k == "DESTBLEND" || k == "BLENDOP" || k == "SRCBLENDALPHA" ||
            k == "DESTBLENDALPHA" || k == "BLENDOPALPHA") {
            hasBlendFactor = true;
            break;
        }
    const bool blendOn = getb("ALPHABLENDENABLE", objectDefaults || (!objectDefaults && hasBlendFactor));
    const bool sepAlpha = getb("SEPARATEALPHABLENDENABLE", false);
    D3D12_RENDER_TARGET_BLEND_DESC& rt = rs.blend.RenderTarget[0];
    rt.BlendEnable = blendOn ? TRUE : FALSE;
    if (blendOn) {
        rt.SrcBlend = ToD3d12Blend(gete("SRCBLEND", kBlendFactors, std::size(kBlendFactors), 5));
        rt.DestBlend = ToD3d12Blend(gete("DESTBLEND", kBlendFactors, std::size(kBlendFactors), 6));
        rt.BlendOp = ToD3d12BlendOp(gete("BLENDOP", kBlendOps, std::size(kBlendOps), 1));
        if (sepAlpha) {
            rt.SrcBlendAlpha = ToD3d12Blend(gete("SRCBLENDALPHA", kBlendFactors, std::size(kBlendFactors), (uint32_t)rt.SrcBlend));
            rt.DestBlendAlpha = ToD3d12Blend(gete("DESTBLENDALPHA", kBlendFactors, std::size(kBlendFactors), (uint32_t)rt.DestBlend));
            rt.BlendOpAlpha = ToD3d12BlendOp(gete("BLENDOPALPHA", kBlendOps, std::size(kBlendOps), 1));
        } else {
            rt.SrcBlendAlpha = rt.SrcBlend;
            rt.DestBlendAlpha = rt.DestBlend;
            rt.BlendOpAlpha = rt.BlendOp;
        }
    } else {
        rt.SrcBlend = rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlend = rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOp = rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    rt.LogicOpEnable = FALSE;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    uint32_t writeMask = gete("COLORWRITEENABLE", nullptr, 0, 0xF);
    if (writeMask == 0) {
        // a numeric-only table lookup cannot express D3D9's symbolic names (ALL etc.); default RGBA
        writeMask = 0xF;
    }
    rt.RenderTargetWriteMask = (UINT8)writeMask;

    const uint32_t fill = gete("FILLMODE", kFillModes, std::size(kFillModes), 3);
    rs.raster.FillMode = fill == 2 ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    const uint32_t cull = gete("CULLMODE", kCullModes, std::size(kCullModes), objectDefaults ? 3u : 1u);
    rs.raster.CullMode = cull == 3 ? D3D12_CULL_MODE_BACK : (cull == 2 ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_NONE);

    // A depth target bound and no explicit ZENABLE: D3D9's default is depth test ON (this is what the MMD-style
    // zplot pass relies on to keep the nearest light-space depth in the shadow map). Post effects without a depth
    // target keep the pixel-shader default (off) unless the pass says otherwise.
    const bool zEnable = getb("ZENABLE", dsBound ? true : objectDefaults);
    rs.depth.DepthEnable = (zEnable && dsBound) ? TRUE : FALSE;
    rs.depth.DepthWriteMask = getb("ZWRITEENABLE", true) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    rs.depth.DepthFunc = ToD3d12Cmp(gete("ZFUNC", kCompareFuncs, std::size(kCompareFuncs), 4));
    const bool stencilEnable = getb("STENCILENABLE", false);
    rs.depth.StencilEnable = (stencilEnable && dsBound) ? TRUE : FALSE;
    rs.depth.StencilReadMask = (UINT8)gete("STENCILMASK", nullptr, 0, 0xFF);
    rs.depth.StencilWriteMask = (UINT8)gete("STENCILWRITEMASK", nullptr, 0, 0xFF);
    auto fillFace = [&](D3D12_DEPTH_STENCILOP_DESC& f, const char* fail, const char* zfail, const char* passOp,
                        const char* func) {
        f.StencilFailOp = ToD3d12StencilOp(gete(fail, kStencilOps, std::size(kStencilOps), 1));
        f.StencilDepthFailOp = ToD3d12StencilOp(gete(zfail, kStencilOps, std::size(kStencilOps), 1));
        f.StencilPassOp = ToD3d12StencilOp(gete(passOp, kStencilOps, std::size(kStencilOps), 1));
        f.StencilFunc = ToD3d12Cmp(gete(func, kCompareFuncs, std::size(kCompareFuncs), 8));
    };
    fillFace(rs.depth.FrontFace, "STENCILFAIL", "STENCILZFAIL", "STENCILPASS", "STENCILFUNC");
    fillFace(rs.depth.BackFace, "CCW_STENCILFAIL", "CCW_STENCILZFAIL", "CCW_STENCILPASS", "CCW_STENCILFUNC");

    rs.alphaTest = getb("ALPHATESTENABLE", false);
}

} // namespace

// ===================================================================================================================
// MmeEffectInstance
// ===================================================================================================================

MmeEffectInstance::MmeEffectInstance(std::filesystem::path file, std::string key, MmeRuntime* rt, bool objectMode)
    : file_(std::move(file)), key_(std::move(key)), rt_(rt), objectMode_(objectMode) {}

MmeEffectInstance::~MmeEffectInstance() = default;

bool MmeEffectInstance::BeginLoad() {
    if (loadStarted_) return true;
    loadStarted_ = true;
    std::filesystem::path file = file_;
    const bool objectMode = objectMode_;
    future_ = std::async(std::launch::async, [file, objectMode]() {
        LoadResult r;
        const auto t0 = std::chrono::steady_clock::now();
        r.desc = LoadMmeEffect(file, MmeLoadOptions{});
        MmeCompileOptions co;
        co.objectMode = objectMode;
        r.programs = CompileMmeEffect(r.desc, co);
        r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return r;
    });
    return true;
}

// M6b: fills unsupportedFeatures_ (failed pass / parser errors) and signature_ (file + #include mtimes) from desc_ /
// programs_. Called from ApplyLoad for both a fresh load and a swap-in.
void MmeEffectInstance::RefreshDiagnostics() {
    unsupportedFeatures_.clear();
    for (const MmeProgram& p : programs_) {
        if (p.ok) continue;
        std::string line;
        for (const MmeDiag& d : p.diags) {
            if (d.sev != MmeDiag::Error) continue;
            line = d.message;
            break;
        }
        if (line.empty()) line = "pass '" + p.pass + "' failed to compile";
        else line = "pass '" + p.pass + "': " + line;
        const size_t nl = line.find('\n');
        if (nl != std::string::npos) line = line.substr(0, nl);
        unsupportedFeatures_.push_back(std::move(line));
    }
    for (const MmeDiag& d : desc_.diags) {
        if (d.sev != MmeDiag::Error) continue;
        std::string line = d.message;
        const size_t nl = line.find('\n');
        if (nl != std::string::npos) line = line.substr(0, nl);
        unsupportedFeatures_.push_back(std::move(line));
    }

    std::string sig;
    auto stamp = [&](const std::filesystem::path& p) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(p, ec);
        const auto sz = std::filesystem::file_size(p, ec);
        sig += PathToUtf8(p) + "|" + std::to_string(ec ? 0 : t.time_since_epoch().count()) + "|" +
               std::to_string(ec ? 0 : sz) + "\n";
    };
    stamp(file_);
    for (const auto& inc : desc_.includes) stamp(inc);
    signature_ = std::move(sig);
}

int MmeEffectInstance::FindProgram(const std::string& technique, const std::string& pass) const {
    for (size_t i = 0; i < programs_.size(); ++i) {
        const MmeProgram& p = programs_[i];
        if (p.technique == technique && p.pass == pass && p.ok) return (int)i;
    }
    return -1;
}

MmeTarget* MmeEffectInstance::FindTarget(const std::string& paramName) {
    auto it = targets_.find(mmetext::Lower(paramName));
    return it == targets_.end() ? nullptr : &it->second;
}

// fix 1: D3D9 STENCILREF for a pass (default 0): the caller sets it with cmd->OMSetStencilRef before the draw.
uint32_t MmeEffectInstance::StencilRefOf(const MmePass& pass) {
    for (const auto& [k, v] : pass.states) {
        if (k != "STENCILREF") continue;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 0);
        if (end == v.c_str()) return 0;
        return (uint32_t)(n & 0xFF);
    }
    return 0;
}

std::vector<MmeTarget*> MmeEffectInstance::OffscreenTargets() {
    std::vector<MmeTarget*> out;
    // Declaration order (desc_.params), so the per-frame draw list is deterministic.
    for (const MmeParam& p : desc_.params) {
        if (p.semantic != "OFFSCREENRENDERTARGET") continue;
        auto it = targets_.find(mmetext::Lower(p.name));
        if (it != targets_.end() && it->second.offscreen) out.push_back(&it->second);
    }
    return out;
}

const MmeSampler* MmeEffectInstance::FindSampler(const std::string& name) const {
    const std::string n = mmetext::Lower(name);
    for (const MmeSampler& s : desc_.samplers)
        if (mmetext::Lower(s.name) == n) return &s;
    return nullptr;
}

const MmeParam* MmeEffectInstance::FindParam(const std::string& name) const {
    const std::string n = mmetext::Lower(name);
    for (const MmeParam& p : desc_.params)
        if (mmetext::Lower(p.name) == n) return &p;
    return nullptr;
}

bool MmeEffectInstance::ResolveTexture(const std::string& bindingName, ID3D12Resource** res,
                                       DXGI_FORMAT* srvFormat, bool* isTarget) {
    *res = nullptr;
    *srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    *isTarget = false;

    // A DX9 combined sampler appears as both a texture and a sampler named `bindingName`; the sampler's
    // `texture = <T>` field names the actual texture parameter.
    std::string target = bindingName;
    if (const MmeSampler* s = FindSampler(bindingName))
        if (!s->texture.empty()) target = s->texture;
    // strip <...> / (...) left over from the sampler_state syntax
    target = mmetext::Trim(target);
    if (target.size() >= 2 && (target.front() == '<' || target.front() == '(') &&
        (target.back() == '>' || target.back() == ')'))
        target = mmetext::Trim(target.substr(1, target.size() - 2));

    const std::string key = mmetext::Lower(target);
    auto tit = targets_.find(key);
    if (tit != targets_.end() && tit->second.tex) {
        *res = tit->second.tex.res.Get();
        *srvFormat = tit->second.tex.srvFormat;
        *isTarget = true;
        return true;
    }
    auto ait = animated_.find(key);
    if (ait != animated_.end() && ait->second.res) {
        *res = ait->second.res.Get();
        *srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        return true;
    }
    auto iit = images_.find(key);
    if (iit != images_.end() && iit->second.res) {
        *res = iit->second.res.Get();
        *srvFormat = iit->second.srvFormat;
        return true;
    }
    return false;
}

void MmeEffectInstance::SetObjectShadowMap(Texture* tex) {
    if (shadowMap_ != tex) {
        shadowMap_ = tex;
        ++targetGen_;   // invalidate cached stage SRV tables that referenced the old shadow map
    }
}

void MmeEffectInstance::FinishLoad(Dx12Context& ctx) {
    if (loadDone_) return;
    LoadResult r;
    if (!TryTakeResult(r)) return;
    loadDone_ = true;
    ApplyLoad(ctx, r);
}

bool MmeEffectInstance::TryTakeResult(LoadResult& r) {
    if (!loadStarted_ || !future_.valid()) return false;
    if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    try {
        r = future_.get();
    } catch (...) {
        failed_ = true;
        LOG_ERROR("MME LOAD %s: worker threw", PathToUtf8(file_).c_str());
        return false;
    }
    return true;
}

void MmeEffectInstance::ApplyLoad(Dx12Context& ctx, LoadResult& r) {
    // A hot reload re-applies a new result over the old instance: drop the previous GPU objects first (the caller
    // guarantees the passes no longer draw the old one this frame).
    Release(ctx);
    targetW_ = targetH_ = 0;   // force EnsureTargets to (re)create the reloaded effect's targets
    failed_ = false;
    missingTextures_.clear();
    unsupportedFeatures_.clear();
    desc_ = std::move(r.desc);
    programs_ = std::move(r.programs);

    uint32_t errs = 0, warns = 0;
    auto countDiags = [&](const std::vector<MmeDiag>& ds, uint32_t& e, uint32_t& w) {
        for (const MmeDiag& d : ds) {
            if (d.sev == MmeDiag::Error) ++e;
            else if (d.sev == MmeDiag::Warning) ++w;
        }
    };
    countDiags(desc_.diags, errs, warns);
    uint32_t ok = 0;
    for (const MmeProgram& p : programs_) {
        if (p.ok) ++ok;
        countDiags(p.diags, errs, warns);
    }
    const std::string k = desc_.kind();
    LOG_INFO("MME LOAD %s kind=%s programs=%u/%u diags=%u/%u ms=%.0f", PathToUtf8(file_).c_str(), k.c_str(),
             ok, (uint32_t)programs_.size(), errs, warns, r.ms);
    uint32_t printed = 0;
    auto printErrors = [&](const std::vector<MmeDiag>& ds) {
        for (const MmeDiag& d : ds) {
            if (d.sev != MmeDiag::Error || printed >= 10) continue;
            LOG_WARN("MME %s(%d): %s", PathToUtf8(d.file.empty() ? file_ : d.file).c_str(), d.line, d.message.c_str());
            ++printed;
        }
    };
    printErrors(desc_.diags);
    for (const MmeProgram& p : programs_) printErrors(p.diags);
    RefreshDiagnostics();

    if (!desc_.ok) {
        failed_ = true;
        return;
    }

    // ---- GPU resources -------------------------------------------------------------------------------------------
    UploadBatch batch(ctx);
    ImageRGBA8 white;
    white.mips.push_back({1, 1, {255, 255, 255, 255}});
    white_ = batch.CreateTexture(white, L"mme.white");
    whiteSrvFormat_ = MmeRuntime::kWhiteFormat;
    if (!white_) {
        failed_ = true;
        LOG_ERROR("MME %s: white fallback upload failed", PathToUtf8(file_).c_str());
        return;
    }

    // file textures (texture params with a ResourceName annotation)
    for (const MmeParam& p : desc_.params) {
        if (mmetext::Lower(p.type).rfind("texture", 0) != 0) continue;
        const MmeAnnotation* ann = FindAnnotation(p.annotations, "ResourceName");
        if (!ann) continue;
        const std::string rel = mmetext::Trim(ann->value.str);
        std::filesystem::path resolved;
        const std::filesystem::path base = p.file.empty() ? file_.parent_path() : p.file.parent_path();
        if (rel.empty() || !mmetext::ResolveRel(base, rel, resolved)) {
            LOG_WARN("MME %s: texture '%s' not found, white is used", PathToUtf8(file_).c_str(), p.name.c_str());
            missingTextures_.push_back(p.name + " (" + rel + ")");
            continue;
        }
        // fix 4: ANIMATEDTEXTURE decodes a GIF / APNG into frames + delays; the runtime picks the frame by the
        // scene clock. The current frame is uploaded through the persistent SRV (not the UploadBatch path below).
        if (p.semantic == "ANIMATEDTEXTURE") {
            AnimTex a;
            a.file = resolved;
            auto annNum = [&](const char* name, float def) -> float {
                const MmeAnnotation* an = FindAnnotation(p.annotations, name);
                if (!an || an->value.nums.empty()) return def;
                return an->value.nums[0];
            };
            a.offset = annNum("Offset", 0.0f);
            a.speed = annNum("Speed", 1.0f);
            if (const MmeAnnotation* sv = FindAnnotation(p.annotations, "SeekVariable"))
                a.seekVar = mmetext::Trim(sv->value.str);
            std::string err;
            if (a.image.Load(resolved, &err) && !a.image.Empty()) {
                animated_.emplace(mmetext::Lower(p.name), std::move(a));
            } else {
                LOG_WARN("MME %s: animated texture '%s' cannot be decoded (%s), white is used",
                         PathToUtf8(file_).c_str(), p.name.c_str(), err.c_str());
                missingTextures_.push_back(p.name + " (" + PathToUtf8(resolved.filename()) + ")");
            }
            continue;
        }
        ImageRGBA8 img;
        std::string err;
        if (LoadImageRGBA8(resolved, img, &err) && !img.Empty()) {
            ImageTex t;
            // DECISION: LoadImageRGBA8 always builds a full mip chain; keep it (MipLevels==1 is not truncated).
            t.res = batch.CreateTextureTyped(img, DXGI_FORMAT_R8G8B8A8_TYPELESS, L"mme.texture");
            t.srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
            if (t.res) images_.emplace(mmetext::Lower(p.name), std::move(t));
            else {
                LOG_WARN("MME %s: texture '%s' upload failed, white is used", PathToUtf8(file_).c_str(), p.name.c_str());
                missingTextures_.push_back(p.name);
            }
        } else {
            LOG_WARN("MME %s: texture '%s' cannot be decoded (%s), white is used", PathToUtf8(file_).c_str(),
                     p.name.c_str(), err.c_str());
            missingTextures_.push_back(p.name + " (" + PathToUtf8(resolved.filename()) + ")");
        }
    }

    // fix 5: TEXTUREVALUE — read the named texture's texels (its CPU copy) into a float4 array. The target texture
    // is resolved by name against the file textures loaded above (an effect render target cannot be read back here;
    // those are logged as unsupported).
    for (const MmeParam& p : desc_.params) {
        if (p.semantic != "TEXTUREVALUE") continue;
        const MmeAnnotation* tn = FindAnnotation(p.annotations, "TextureName");
        const std::string target = tn ? mmetext::Lower(mmetext::Trim(tn->value.str)) : std::string();
        TexValue tv;
        tv.texture = target;
        const MmeParam* tp = FindParam(target);
        const MmeAnnotation* rn = tp ? FindAnnotation(tp->annotations, "ResourceName") : nullptr;
        std::filesystem::path resolved;
        const std::filesystem::path base = (tp && !tp->file.empty()) ? tp->file.parent_path() : file_.parent_path();
        ImageRGBA8 img;
        std::string err;
        if (rn && mmetext::ResolveRel(base, mmetext::Trim(rn->value.str), resolved) &&
            LoadImageRGBA8(resolved, img, &err) && !img.Empty()) {
            tv.width = img.mips[0].width;
            tv.height = img.mips[0].height;
            tv.rgba = img.mips[0].pixels;
        }
        MmeTarget* rt = FindTarget(target);
        if (tv.rgba.empty() && rt && rt->tex.res)
            LOG_WARN("MME %s: TEXTUREVALUE on render target '%s' is not supported (no CPU readback)",
                     PathToUtf8(file_).c_str(), p.name.c_str());
        texValues_.emplace(mmetext::Lower(p.name), std::move(tv));
    }

    // render targets + depth-stencil targets + offscreen targets
    for (const MmeParam& p : desc_.params) {
        const bool color = p.semantic == "RENDERCOLORTARGET";
        const bool depth = p.semantic == "RENDERDEPTHSTENCILTARGET";
        const bool offscreen = p.semantic == "OFFSCREENRENDERTARGET";
        if (!color && !depth && !offscreen) continue;
        MmeTarget t;
        t.name = p.name;
        t.depth = depth;
        t.offscreen = offscreen;
        auto num = [&](const char* name, int idx, float def) -> float {
            const MmeAnnotation* a = FindAnnotation(p.annotations, name);
            if (!a || idx >= (int)a->value.nums.size()) return def;
            return a->value.nums[(size_t)idx];
        };
        const MmeAnnotation* w = FindAnnotation(p.annotations, "Width");
        const MmeAnnotation* h = FindAnnotation(p.annotations, "Height");
        const MmeAnnotation* dim = FindAnnotation(p.annotations, "Dimensions");
        if (w && !w->value.nums.empty()) t.fixedW = (uint32_t)std::max(0.0f, num("Width", 0, 0));
        if (h && !h->value.nums.empty()) t.fixedH = (uint32_t)std::max(0.0f, num("Height", 0, 0));
        if (dim && dim->value.nums.size() >= 2) {
            t.fixedW = (uint32_t)std::max(0.0f, num("Dimensions", 0, 0));
            t.fixedH = (uint32_t)std::max(0.0f, num("Dimensions", 1, 0));
        }
        t.ratioX = num("ViewportRatio", 0, 1.0f);
        t.ratioY = num("ViewportRatio", 1, 1.0f);
        if (t.ratioX <= 0) t.ratioX = 1.0f;
        if (t.ratioY <= 0) t.ratioY = 1.0f;
        // M5a: a declared MipLevels annotation is kept as written (0 = full chain, resolved per size in
        // EnsureTargets); a target with no annotation keeps 1 (a single level, the pre-M5 behaviour).
        const MmeAnnotation* mipAnn = FindAnnotation(p.annotations, "MipLevels");
        if (!mipAnn) mipAnn = FindAnnotation(p.annotations, "Levels");
        t.mipLevels = mipAnn ? (uint32_t)std::max(0.0f, num("MipLevels", 0, num("Levels", 0, 1.0f))) : 1;
        const MmeAnnotation* fmt = FindAnnotation(p.annotations, "Format");
        t.tex.format = fmt ? MapD3d9Format(fmt->value.str) : DXGI_FORMAT_UNKNOWN;
        // OFFSCREEN ClearColor
        if (offscreen) {
            for (int i = 0; i < 4; ++i) t.clear[i] = num("ClearColor", i, i == 3 ? 1.0f : 0.0f);
            t.clearDepth = num("ClearDepth", 0, 1.0f);
            // DefaultEffect: "pattern = effect; pattern = effect; ..." (first match wins).
            if (const MmeAnnotation* de = FindAnnotation(p.annotations, "DefaultEffect"))
                ParseDefaultEffect(de->value.str, t.defaultEffect);
        }
        targets_.emplace(mmetext::Lower(p.name), std::move(t));
    }

    batch.Submit();

    // sampler ranges: the exact number of samplers each program stage uses (max register + 1)
    const size_t stageCount = programs_.size() * 2;
    samplerCount_.assign(stageCount, 0);
    uint32_t totalSamplers = 0;
    for (size_t pi = 0; pi < programs_.size(); ++pi) {
        for (int stage = 0; stage < 2; ++stage) {
            const MmeStage& st = stage ? programs_[pi].ps : programs_[pi].vs;
            uint32_t maxSlot = 0;
            bool any = false;
            for (const MmeResBinding& b : st.res) {
                if (b.kind != MmeResBinding::Sampler) continue;
                maxSlot = std::max(maxSlot, b.slot);
                any = true;
            }
            const uint32_t n = any ? maxSlot + 1 : 0;
            samplerCount_[pi * 2 + stage] = n;
            totalSamplers += n;
        }
    }
    const uint32_t base = totalSamplers ? rt_->AllocSamplers(totalSamplers) : DescriptorHeap::kInvalid;
    if (totalSamplers && base == DescriptorHeap::kInvalid) {
        failed_ = true;
        LOG_ERROR("MME %s: sampler heap out of space (%u samplers)", PathToUtf8(file_).c_str(), totalSamplers);
        return;
    }
    samplerBase_.assign(stageCount, DescriptorHeap::kInvalid);
    ID3D12Device* device = ctx.Device();
    D3D12_SAMPLER_DESC defSampler = DefaultSampler();
    uint32_t cursor = base;
    for (size_t pi = 0; pi < programs_.size(); ++pi) {
        for (int stage = 0; stage < 2; ++stage) {
            const uint32_t n = samplerCount_[pi * 2 + stage];
            if (!n) continue;
            const uint32_t sbase = cursor;
            cursor += n;
            samplerBase_[pi * 2 + stage] = sbase;
            for (uint32_t slot = 0; slot < n; ++slot)
                device->CreateSampler(&defSampler, rt_->samplerHeap_.Cpu(sbase + slot));
            const MmeStage& st = stage ? programs_[pi].ps : programs_[pi].vs;
            for (const MmeResBinding& b : st.res) {
                if (b.kind != MmeResBinding::Sampler || b.slot >= n) continue;
                const MmeSampler* s = FindSampler(b.name);
                D3D12_SAMPLER_DESC sd = s ? TranslateSampler(*s) : defSampler;
                // M5a: an explicit `register(sN)` sampler with no `texture=` is where MMD binds its self-shadow map
                // (an R32_FLOAT depth buffer). MMD samples that buffer POINT/CLAMP; a bilinear tap would pull
                // neighbouring depths across an edge and make every surface self-shadow. Force point clamp when the
                // declaration carries no texture binding.
                if (s && !s->registerBinding.empty() && s->texture.empty()) {
                    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                }
                device->CreateSampler(&sd, rt_->samplerHeap_.Cpu(sbase + b.slot));
            }
        }
    }

    gpuBuilt_ = true;
}

void MmeEffectInstance::EnsureTargets(Dx12Context& ctx, uint32_t outW, uint32_t outH) {
    if (targets_.empty()) { targetW_ = outW; targetH_ = outH; return; }
    if (targetW_ == outW && targetH_ == outH) return;
    for (auto& [name, t] : targets_) {
        t.tex.Release(ctx);
    }
    targetW_ = outW;
    targetH_ = outH;
    ++targetGen_;   // invalidate every cached stage SRV table that referenced the old targets

    for (auto& [name, t] : targets_) {
        uint32_t w = t.fixedW, h = t.fixedH;
        if (w == 0) w = std::max(1u, (uint32_t)std::lround(t.ratioX * outW));
        if (h == 0) h = std::max(1u, (uint32_t)std::lround(t.ratioY * outH));
        if (t.depth) {
            // fix 1: a stencil request (D24S8 / D3DFMT_D24S8) becomes a real typeless R24G8 resource with a
            // D24_UNORM_S8_UINT DSV and an R24_UNORM_X8_TYPELESS SRV; D32S8X24 becomes the matching R32G8X24 pair.
            // A plain depth target (UNKNOWN / D32_FLOAT) stays R32_TYPELESS -> D32_FLOAT. Created directly rather
            // than through Texture::Create (which only knows R32_TYPELESS / D32_FLOAT).
            DXGI_FORMAT resFormat = DXGI_FORMAT_R32_TYPELESS;
            DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;
            DXGI_FORMAT srvFormat = DXGI_FORMAT_R32_FLOAT;
            if (t.tex.format == DXGI_FORMAT_D24_UNORM_S8_UINT) {
                resFormat = DXGI_FORMAT_R24G8_TYPELESS;
                dsvFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            } else if (t.tex.format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
                resFormat = DXGI_FORMAT_R32G8X24_TYPELESS;
                dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            }
            t.dsvFormat = dsvFormat;
            t.srvFormat = srvFormat;
            t.tex.Release(ctx);
            CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(resFormat, w, h, 1, 1, 1, 0,
                                                                    D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            D3D12_CLEAR_VALUE cv{};
            cv.Format = dsvFormat;
            cv.DepthStencil.Depth = 1.0f;
            cv.DepthStencil.Stencil = 0;
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            if (!CheckHr(ctx.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                                               IID_PPV_ARGS(&t.tex.res)),
                         "MmeEffectInstance: depth target")) {
                LOG_ERROR("MME %s: depth target '%s' creation failed (%ux%u)", PathToUtf8(file_).c_str(),
                          t.name.c_str(), w, h);
                continue;
            }
            t.tex.res->SetName(L"mme.depth");
            t.tex.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            t.tex.format = resFormat;
            t.tex.srvFormat = srvFormat;
            t.tex.width = w;
            t.tex.height = h;
            t.tex.samples = 1;
            t.tex.arraySize = 1;
            t.tex.dsv = ctx.DsvHeap().Allocate(1);
            if (t.tex.dsv == DescriptorHeap::kInvalid) {
                LOG_ERROR("MME %s: out of DSV descriptors for depth target '%s'", PathToUtf8(file_).c_str(),
                          t.name.c_str());
                t.tex.Release(ctx);
                continue;
            }
            D3D12_DEPTH_STENCIL_VIEW_DESC d{};
            d.Format = dsvFormat;
            d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            ctx.Device()->CreateDepthStencilView(t.tex.res.Get(), &d, ctx.DsvHeap().Cpu(t.tex.dsv));
        } else {
            const DXGI_FORMAT fmt = t.tex.format == DXGI_FORMAT_UNKNOWN ? DXGI_FORMAT_R8G8B8A8_UNORM : t.tex.format;
            // M5a: MipLevels > 1 -> a real mip chain (one RTV per level, mip 0 is the target's own RTV);
            // MipLevels = 0 -> the full chain. GenerateMips builds the levels after mip 0 was rendered.
            // (This target is created directly rather than through Texture::Create so it can carry more than one
            // mip level; Texture has no mip count, so the effect's own mipLevels drives the chain.)
            const uint32_t fullChain = 1 + (uint32_t)std::floor(std::log2((double)std::max(std::max(w, h), 1u)));
            uint32_t mips = t.mipLevels == 0 ? fullChain : std::min(t.mipLevels, fullChain);
            if (mips < 1) mips = 1;
            t.tex.Release(ctx);
            CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(fmt, w, h, 1, (UINT16)mips, 1, 0,
                                                                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
            D3D12_CLEAR_VALUE cv{};
            cv.Format = fmt;
            std::memcpy(cv.Color, t.clear, sizeof(float) * 4);
            D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
            if (!CheckHr(ctx.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                                               IID_PPV_ARGS(&t.tex.res)),
                         "MmeEffectInstance: target")) {
                LOG_ERROR("MME %s: target '%s' creation failed (%ux%u)", PathToUtf8(file_).c_str(), t.name.c_str(), w, h);
                t.mipLevels = 1;
            } else {
                t.tex.res->SetName(L"mme.target");
                t.tex.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                t.tex.format = fmt;
                t.tex.srvFormat = fmt;
                t.tex.width = w;
                t.tex.height = h;
                t.tex.samples = 1;
                t.tex.arraySize = 1;
                t.mipLevels = mips;
                const uint32_t rtv = ctx.RtvHeap().Allocate(1);
                if (rtv != DescriptorHeap::kInvalid) {
                    D3D12_RENDER_TARGET_VIEW_DESC d{};
                    d.Format = fmt;
                    d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                    ctx.Device()->CreateRenderTargetView(t.tex.res.Get(), &d, ctx.RtvHeap().Cpu(rtv));
                }
                t.tex.rtv = rtv;
            }
            // Offscreen targets own a companion depth surface for the scene re-render draw.
            if (t.offscreen)
                if (!t.depthTex.Create(ctx, w, h, RenderTargets::kDepthFormat,
                                       D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                       D3D12_RESOURCE_STATE_DEPTH_WRITE, L"mme.offscreen.depth"))
                    LOG_ERROR("MME %s: offscreen depth '%s' creation failed (%ux%u)", PathToUtf8(file_).c_str(),
                              t.name.c_str(), w, h);
        }
    }
}

const MmeEffectInstance::ProgLayout& MmeEffectInstance::Layout(int program) {
    if (program < 0 || program >= (int)programs_.size()) {
        static ProgLayout empty;
        return empty;
    }
    // layouts_ is built lazily; size it to the program count before indexing (a shorter vector makes
    // layouts_[program] out of bounds).
    if (layouts_.size() < programs_.size()) layouts_.resize(programs_.size());
    ProgLayout& pl = layouts_[program];
    if (!pl.names.empty()) return pl;
    const MmeProgram& prog = programs_[program];
    const std::vector<MmeSigElement>& inputs = prog.vs.inputs;

    // A pass with a NULL VS is served by the built-in passthrough (POSITION + TEXCOORD0).
    if (!prog.vs.code || inputs.empty()) {
        pl.names = {"POSITION", "TEXCOORD"};
        pl.elems = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        pl.elems[0].SemanticName = pl.names[0].c_str();
        pl.elems[1].SemanticName = pl.names[1].c_str();
        return pl;
    }

    struct Element { std::string sem; uint32_t index; DXGI_FORMAT fmt; uint32_t offset; bool warned; };
    std::vector<Element> elems;
    elems.reserve(inputs.size());
    for (const MmeSigElement& in : inputs) {
        std::string base = UpperAscii(in.semantic);
        uint32_t offset = 16;
        if (base == "POSITION" || base == "SV_POSITION") offset = 0;
        else if (base == "TEXCOORD") offset = 16;
        else if (base == "NORMAL") offset = 32;
        else if (base == "COLOR") offset = 48;
        else {
            LOG_WARN("MME %s: program %d input '%s' has no quad attribute; bound as TEXCOORD", PathToUtf8(file_).c_str(),
                     program, base.c_str());
            offset = 16;
        }
        int comps = 0;
        for (int i = 0; i < 4; ++i)
            if (in.mask & (1u << i)) ++comps;
        comps = std::clamp(comps, 1, 4);
        DXGI_FORMAT fmt = comps == 1 ? DXGI_FORMAT_R32_FLOAT
                        : comps == 2 ? DXGI_FORMAT_R32G32_FLOAT
                        : comps == 3 ? DXGI_FORMAT_R32G32B32_FLOAT
                                     : DXGI_FORMAT_R32G32B32A32_FLOAT;
        elems.push_back({base, in.index, fmt, offset, false});
    }

    pl.names.clear();
    pl.names.reserve(elems.size());
    for (const Element& e : elems) pl.names.push_back(e.sem);
    pl.elems.clear();
    pl.elems.reserve(elems.size());
    for (size_t i = 0; i < elems.size(); ++i) {
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName = pl.names[i].c_str();
        d.SemanticIndex = elems[i].index;
        d.Format = elems[i].fmt;
        d.InputSlot = 0;
        d.AlignedByteOffset = elems[i].offset;
        d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = 0;
        pl.elems.push_back(d);
    }
    if (pl.elems.empty()) {
        pl.names = {"POSITION", "TEXCOORD"};
        pl.elems = {{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        pl.elems[0].SemanticName = pl.names[0].c_str();
        pl.elems[1].SemanticName = pl.names[1].c_str();
    }
    return pl;
}

ID3D12PipelineState* MmeEffectInstance::Pso(Dx12Context& ctx, int program, const MmePass& pass,
                                            const DXGI_FORMAT* rtFormats, uint32_t rtCount, DXGI_FORMAT dsFormat,
                                            uint32_t sampleCount, const std::vector<D3D12_INPUT_ELEMENT_DESC>* inputLayout) {
    if (program < 0 || program >= (int)programs_.size()) return nullptr;
    if (!rtCount) return nullptr;
    const MmeProgram& prog = programs_[program];
    if (!prog.ok || !prog.ps.code) return nullptr;

    uint64_t hash = 1469598103934665603ull;
    hash = Fnv1a(hash, &program, sizeof(program));
    for (const auto& [k, v] : pass.states) {
        hash = Fnv1aStr(hash, k);
        hash = Fnv1aStr(hash, v);
    }
    hash = Fnv1a(hash, rtFormats, sizeof(DXGI_FORMAT) * rtCount);
    hash = Fnv1a(hash, &dsFormat, sizeof(dsFormat));
    hash = Fnv1a(hash, &sampleCount, sizeof(sampleCount));
    // The object input layout is deterministic per program (built from the VS signature); include a flag so the
    // post-mode PSO (built-in layout) and the object-mode PSO never share a cache entry.
    const bool customLayout = inputLayout != nullptr;
    hash = Fnv1a(hash, &customLayout, sizeof(customLayout));
    auto it = psoCache_.find(hash);
    if (it != psoCache_.end()) return it->second.Get();

    RenderState rs;
    TranslateStates(pass, dsFormat != DXGI_FORMAT_UNKNOWN, rs, objectMode_);
    // M5a: the pass's AlphaTestEnable is handled by the compiler as an injected clip() in the generated PS wrapper
    // (see MmeCompiler's AlphaTestKey / AlphaTestOp). A pass with no AlphaFunc (D3D9's default ALWAYS) or ALWAYS
    // discards nothing, so it is a no-op rather than a warning; only a real comparison that produced no clip() (the
    // PS has no colour output to test) is reported.
    if (rs.alphaTest && !prog.alphaTest) {
        const std::string* funcRaw = nullptr;
        for (const auto& [k, v] : pass.states)
            if (k == "ALPHAFUNC") { funcRaw = &v; break; }
        std::string func = funcRaw ? UpperAscii(mmetext::Trim(*funcRaw)) : std::string();
        if (!func.empty() && func != "ALWAYS") {
            const std::string pkey = "at:" + std::to_string(program) + ":" + pass.name;
            if (loggedPasses_.insert(pkey).second)
                LOG_WARN("MME %s: pass '%s' alpha test ignored (no colour output to test)", PathToUtf8(file_).c_str(),
                         pass.name.c_str());
        }
    }
    // states MME carries that we do not translate: report once per instance.
    static const char* kKnown[] = {
        "ALPHABLENDENABLE", "SRCBLEND", "DESTBLEND", "BLENDOP", "SEPARATEALPHABLENDENABLE",
        "SRCBLENDALPHA", "DESTBLENDALPHA", "BLENDOPALPHA", "ZENABLE", "ZWRITEENABLE", "ZFUNC",
        "CULLMODE", "FILLMODE", "COLORWRITEENABLE", "STENCILENABLE", "STENCILFAIL", "STENCILZFAIL",
        "STENCILPASS", "STENCILFUNC", "STENCILREF", "STENCILMASK", "STENCILWRITEMASK",
        "CCW_STENCILFAIL", "CCW_STENCILZFAIL", "CCW_STENCILPASS", "CCW_STENCILFUNC",
        "ALPHATESTENABLE", "ALPHAREF", "ALPHAFUNC", "VERTEXSHADER", "PIXELSHADER",
    };
    for (const auto& [k, v] : pass.states) {
        bool known = false;
        for (const char* n : kKnown)
            if (k == n) { known = true; break; }
        if (!known && loggedStates_.insert(k).second)
            LOG_WARN("MME %s: render state '%s' not translated", PathToUtf8(file_).c_str(), k.c_str());
    }

    const ProgLayout& layout = Layout(program);
    D3D12_INPUT_LAYOUT_DESC inputLayoutDesc;
    if (inputLayout) {
        inputLayoutDesc = {inputLayout->data(), (UINT)inputLayout->size()};
    } else {
        inputLayoutDesc = {layout.elems.data(), (UINT)layout.elems.size()};
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rt_->RootSignature();
    desc.VS = {prog.vs.code ? prog.vs.code->GetBufferPointer() : nullptr,
               prog.vs.code ? prog.vs.code->GetBufferSize() : 0};
    if (!prog.vs.code && rt_->PassthroughVs())
        desc.VS = {rt_->PassthroughVs()->GetBufferPointer(), rt_->PassthroughVs()->GetBufferSize()};
    desc.PS = {prog.ps.code->GetBufferPointer(), prog.ps.code->GetBufferSize()};
    desc.BlendState = rs.blend;
    desc.RasterizerState = rs.raster;
    desc.DepthStencilState = rs.depth;
    desc.InputLayout = inputLayoutDesc;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = rtCount;
    for (uint32_t i = 0; i < rtCount && i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        desc.RTVFormats[i] = rtFormats[i];
    desc.DSVFormat = dsFormat;
    desc.SampleMask = UINT_MAX;
    desc.SampleDesc.Count = sampleCount ? sampleCount : 1;

    ComPtr<ID3D12PipelineState> pso;
    if (!CheckHr(ctx.Device()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)),
                 "MmeEffectInstance: CreateGraphicsPipelineState")) {
        LOG_ERROR("MME %s: PSO creation failed (pass '%s')", PathToUtf8(file_).c_str(), pass.name.c_str());
        return nullptr;
    }
    auto [ins, _] = psoCache_.emplace(hash, std::move(pso));
    return ins->second.Get();
}

namespace {
// The number of texture descriptors a stage's SRV table needs (max texture register + 1).
uint32_t StageTextureCount(const MmeStage& st) {
    uint32_t maxSlot = 0;
    bool any = false;
    for (const MmeResBinding& b : st.res) {
        if (b.kind != MmeResBinding::Texture) continue;
        maxSlot = std::max(maxSlot, b.slot);
        any = true;
    }
    return any ? maxSlot + 1 : 0;
}
} // namespace

MmeEffectInstance::StageSrvCache& MmeEffectInstance::EnsureSrvTable(PassContext& pc, int program, bool pixelStage,
                                                                    uint32_t materialSrvTable, const GpuModel* model) {
    static StageSrvCache kNull;
    if (program < 0 || program >= (int)programs_.size()) return kNull;
    // fix 4: any draw that binds a stage table advances this effect's ANIMATEDTEXTUREs (frame-stamped, so a post
    // effect and an object draw of one frame upload once each).
    if (!animated_.empty()) UpdateAnimatedTextures(pc);
    const MmeStage& st = pixelStage ? programs_[program].ps : programs_[program].vs;
    const uint32_t count = StageTextureCount(st);
    const SrvKey key{model, (uint32_t)program, pixelStage, materialSrvTable};
    auto it = srvCache_.find(key);
    if (it == srvCache_.end()) {
        StageSrvCache c;
        c.count = count;
        if (count) {
            c.first = pc.ctx.SrvHeap().Allocate(count);
            if (c.first == DescriptorHeap::kInvalid) {
                if (!srvFailLogged_) {
                    srvFailLogged_ = true;
                    LOG_ERROR("MME %s: out of SRV descriptors for a stage table", PathToUtf8(file_).c_str());
                }
                failed_ = true;
                c.first = DescriptorHeap::kInvalid;
            }
        }
        it = srvCache_.emplace(key, c).first;
    } else if (it->second.targetGen == targetGen_) {
        // still valid: the referenced targets / images have not been recreated since the table was written
        return it->second;
    }
    StageSrvCache& c = it->second;
    if (c.first == DescriptorHeap::kInvalid) return c;
    if (c.count != count) {  // the stage's texture count changed (should not happen); leave the old table alone
        return c;
    }

    ID3D12Device* device = pc.ctx.Device();
    auto writeSrv = [&](uint32_t idx, ID3D12Resource* res, DXGI_FORMAT fmt) {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = pc.ctx.SrvHeap().Cpu(c.first + idx);
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.Format = fmt;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipLevels = UINT(-1);
        device->CreateShaderResourceView(res, &d, cpu);
    };
    for (uint32_t i = 0; i < count; ++i) writeSrv(i, white_.Get(), whiteSrvFormat_);

    for (const MmeResBinding& b : st.res) {
        if (b.kind != MmeResBinding::Texture || b.slot >= count) continue;
        // M5a: an explicit `sampler X : register(sN)` with no `texture =` state is where MMD fills the self-shadow
        // map. Bind the renderer's MMD-style shadow map there so register(s0) sampler declarations resolve.
        if (shadowMap_ && shadowMap_->res) {
            const MmeSampler* smp = FindSampler(b.name);
            if (smp && !smp->registerBinding.empty() && smp->texture.empty()) {
                writeSrv(b.slot, shadowMap_->res.Get(), DXGI_FORMAT_R32_FLOAT);
                continue;
            }
        }
        // M4b: MATERIALTEXTURE / MATERIALSPHEREMAP / MATERIALTOONTEXTURE bind the material's own SRVs. A DX9
        // combined sampler appears under the sampler's name; resolve its `texture = <T>` target first.
        if (materialSrvTable != DescriptorHeap::kInvalid) {
            std::string target = b.name;
            if (const MmeSampler* s = FindSampler(b.name))
                if (!s->texture.empty()) target = s->texture;
            target = mmetext::Trim(target);
            if (target.size() >= 2 && (target.front() == '<' || target.front() == '(') &&
                (target.back() == '>' || target.back() == ')'))
                target = mmetext::Trim(target.substr(1, target.size() - 2));
            const MmeParam* p = FindParam(target);
            int matOffset = -1;
            if (p) {
                if (p->semantic == "MATERIALTEXTURE") matOffset = 0;
                else if (p->semantic == "MATERIALSPHEREMAP") matOffset = 1;
                else if (p->semantic == "MATERIALTOONTEXTURE") matOffset = 2;
            }
            if (matOffset >= 0) {
                D3D12_CPU_DESCRIPTOR_HANDLE src = pc.ctx.SrvHeap().Cpu(materialSrvTable + (uint32_t)matOffset);
                D3D12_CPU_DESCRIPTOR_HANDLE dst = pc.ctx.SrvHeap().Cpu(c.first + b.slot);
                device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                continue;
            }
        }
        ID3D12Resource* res = nullptr;
        DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
        bool isTarget = false;
        if (ResolveTexture(b.name, &res, &fmt, &isTarget) && res) writeSrv(b.slot, res, fmt);
    }
    c.targetGen = targetGen_;
    return c;
}

D3D12_GPU_DESCRIPTOR_HANDLE MmeEffectInstance::StageSrvTable(PassContext& pc, int program, bool pixelStage) {
    StageSrvCache& c = EnsureSrvTable(pc, program, pixelStage, DescriptorHeap::kInvalid, nullptr);
    return c.first == DescriptorHeap::kInvalid ? D3D12_GPU_DESCRIPTOR_HANDLE{} : pc.ctx.SrvHeap().Gpu(c.first);
}

D3D12_GPU_DESCRIPTOR_HANDLE MmeEffectInstance::StageSrvTableObject(PassContext& pc, int program, bool pixelStage,
                                                                   uint32_t materialSrvTable, const GpuModel* model) {
    StageSrvCache& c = EnsureSrvTable(pc, program, pixelStage, materialSrvTable, model);
    return c.first == DescriptorHeap::kInvalid ? D3D12_GPU_DESCRIPTOR_HANDLE{} : pc.ctx.SrvHeap().Gpu(c.first);
}

void MmeEffectInstance::FreeSrvTable(Dx12Context& ctx, StageSrvCache& c) {
    if (c.first != DescriptorHeap::kInvalid && c.count) ctx.SrvHeap().Free(c.first, c.count);
    c.first = DescriptorHeap::kInvalid;
}

void MmeEffectInstance::ForgetModel(Dx12Context& ctx, const GpuModel* model) {
    if (!model) return;
    for (auto it = srvCache_.begin(); it != srvCache_.end();) {
        if (it->first.model == model) {
            FreeSrvTable(ctx, it->second);
            it = srvCache_.erase(it);
        } else {
            ++it;
        }
    }
}

void MmeEffectInstance::FreeSrvTables(Dx12Context& ctx) {
    for (auto& [k, v] : srvCache_) FreeSrvTable(ctx, v);
    srvCache_.clear();
}

D3D12_GPU_DESCRIPTOR_HANDLE MmeEffectInstance::StageSamplerTable(int program, bool pixelStage) const {
    if (program < 0 || (size_t)(program * 2 + (pixelStage ? 1 : 0)) >= samplerBase_.size()) return {};
    const uint32_t base = samplerBase_[(size_t)program * 2 + (pixelStage ? 1 : 0)];
    if (base == DescriptorHeap::kInvalid) return {};
    return rt_->SamplerTableGpu(base);
}

// fix 4: choose the ANIMATEDTEXTURE frame for the scene clock and (re)upload it when it changed. One upload per
// instance per frame (a shared frame stamp); a fresh staging buffer is used per upload and handed to the frame's
// deferred-release queue (no reuse race with in-flight copies).
void MmeEffectInstance::UpdateAnimatedTextures(PassContext& pc) {
    if (animated_.empty()) return;
    if (animStamp_ != pc.frame) {
        animStamp_ = pc.frame;
        for (auto& [name, a] : animated_) {
            const float t = pc.view.sceneTime;
            float seek = (a.seekVar.empty() ? t : 0.0f) * a.speed + a.offset;
            const size_t fi = a.image.FrameAt(seek);
            if (fi == a.uploadedFrame) continue;
            const AnimImageFrame& f = a.image.Frame(fi);
            Dx12Context& ctx = pc.ctx;
            if (!a.res) {
                CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R8G8B8A8_UNORM, f.width, f.height);
                D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
                if (!CheckHr(ctx.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                   IID_PPV_ARGS(&a.res)),
                             "MmeEffectInstance: animated texture")) {
                    LOG_ERROR("MME %s: animated texture '%s' creation failed", PathToUtf8(file_).c_str(), name.c_str());
                    a.uploadedFrame = (uint32_t)fi;
                    continue;
                }
                a.res->SetName(L"mme.animated");
                a.state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            const uint64_t rowPitch = (uint64_t)f.width * 4;
            const uint64_t bytes = rowPitch * f.height;
            CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            D3D12_HEAP_PROPERTIES uh{D3D12_HEAP_TYPE_UPLOAD};
            ComPtr<ID3D12Resource> staging;
            if (!CheckHr(ctx.Device()->CreateCommittedResource(&uh, D3D12_HEAP_FLAG_NONE, &bd,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&staging)),
                         "MmeEffectInstance: animated staging")) {
                a.uploadedFrame = (uint32_t)fi;
                continue;
            }
            void* mapped = nullptr;
            if (SUCCEEDED(staging->Map(0, nullptr, &mapped)) && mapped)
                std::memcpy(mapped, f.rgba.data(), (size_t)bytes);
            if (mapped) staging->Unmap(0, nullptr);
            if (a.state != D3D12_RESOURCE_STATE_COPY_DEST) {
                D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
                    a.res.Get(), a.state, D3D12_RESOURCE_STATE_COPY_DEST);
                pc.cmd->ResourceBarrier(1, &b);
                a.state = D3D12_RESOURCE_STATE_COPY_DEST;
            }
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = a.res.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION srcLoc{};
            srcLoc.pResource = staging.Get();
            srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            srcLoc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srcLoc.PlacedFootprint.Footprint.Width = f.width;
            srcLoc.PlacedFootprint.Footprint.Height = f.height;
            srcLoc.PlacedFootprint.Footprint.Depth = 1;
            srcLoc.PlacedFootprint.Footprint.RowPitch = (UINT)rowPitch;
            pc.cmd->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
            ctx.DeferRelease(staging);   // freed once the frame's command list has completed
            a.uploadedFrame = (uint32_t)fi;
        }
    }
    // Make every animated texture readable by the SRV tables the draws will bind (they sampled it as a copy dest
    // while the upload was recorded above).
    for (auto& [name, a] : animated_) {
        if (a.res && a.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            D3D12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(
                a.res.Get(), a.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            pc.cmd->ResourceBarrier(1, &b);
            a.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }
}

// fix 5: pack a TEXTUREVALUE parameter's CPU texel copy into float4s once (cached in texValueFloats_).
const float* MmeEffectInstance::TexValueFor(const std::string& paramName, uint32_t& outCount) {
    outCount = 0;
    const std::string key = mmetext::Lower(paramName);
    auto cached = texValueFloats_.find(key);
    if (cached != texValueFloats_.end()) {
        outCount = (uint32_t)(cached->second.size() / 4);
        return cached->second.empty() ? nullptr : cached->second.data();
    }
    auto it = texValues_.find(key);
    if (it == texValues_.end() || it->second.rgba.empty()) {
        texValueFloats_.emplace(key, std::vector<float>{});   // cache the miss
        return nullptr;
    }
    const TexValue& tv = it->second;
    const uint32_t texels = tv.width * tv.height;
    std::vector<float> floats(texels * 4, 0.0f);
    const uint32_t n = PackTexelsToFloat4(tv.rgba.data(), tv.width, tv.height, floats.data(), texels);
    floats.resize((size_t)n * 4);
    auto [ins, _] = texValueFloats_.emplace(key, std::move(floats));
    outCount = (uint32_t)(ins->second.size() / 4);
    return ins->second.empty() ? nullptr : ins->second.data();
}

void MmeEffectInstance::Release(Dx12Context& ctx) {
    FreeSrvTables(ctx);
    for (auto& [name, t] : targets_) {
        t.tex.Release(ctx);
        t.depthTex.Release(ctx);
    }
    targets_.clear();
    images_.clear();
    animated_.clear();
    texValues_.clear();
    animStamp_ = ~0ull;
    white_.Reset();
    psoCache_.clear();
    layouts_.clear();
    if (rt_ && !samplerBase_.empty()) {
        uint32_t minBase = DescriptorHeap::kInvalid;
        uint32_t total = 0;
        for (size_t i = 0; i < samplerBase_.size(); ++i) {
            if (samplerBase_[i] != DescriptorHeap::kInvalid) minBase = std::min(minBase, samplerBase_[i]);
            if (i < samplerCount_.size()) total += samplerCount_[i];
        }
        if (minBase != DescriptorHeap::kInvalid && total)
            rt_->samplerHeap_.Free(minBase, total);
    }
    samplerBase_.clear();
    samplerCount_.clear();
    gpuBuilt_ = false;
}

// ===================================================================================================================
// MmeRuntime
// ===================================================================================================================

bool MmeRuntime::Init(Dx12Context& ctx, const std::filesystem::path& shaderDir) {
    ctx_ = &ctx;
    shaderDir_ = shaderDir;
    ID3D12Device* device = ctx.Device();

    CD3DX12_DESCRIPTOR_RANGE vsSrv, vsSmp, psSrv, psSmp;
    vsSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0);
    vsSmp.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0);
    psSrv.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 0, 0);
    psSmp.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 16, 0, 0);
    CD3DX12_ROOT_PARAMETER params[7];
    params[0].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
    params[1].InitAsDescriptorTable(1, &vsSrv, D3D12_SHADER_VISIBILITY_VERTEX);
    params[2].InitAsDescriptorTable(1, &vsSmp, D3D12_SHADER_VISIBILITY_VERTEX);
    params[3].InitAsConstantBufferView(0, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    params[4].InitAsDescriptorTable(1, &psSrv, D3D12_SHADER_VISIBILITY_PIXEL);
    params[5].InitAsDescriptorTable(1, &psSmp, D3D12_SHADER_VISIBILITY_PIXEL);
    // M4b: the object-mode host cbuffer (b13, space 0; see MmeCompiler.cpp HostCbufferText). Bound by the
    // object draws; the post pass never binds it.
    params[6].InitAsConstantBufferView(13, 0, D3D12_SHADER_VISIBILITY_ALL);
    CD3DX12_ROOT_SIGNATURE_DESC rsDesc;
    rsDesc.Init(7, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> blob, err;
    if (!CheckHr(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err),
                 "MmeRuntime: SerializeRootSignature"))
        return false;
    if (!CheckHr(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&rootSig_)),
                 "MmeRuntime: CreateRootSignature"))
        return false;

    if (!samplerHeap_.Create(device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize, true)) {
        LOG_ERROR("MmeRuntime: sampler heap creation failed");
        return false;
    }

    // constant ring: kFramesInFlight UPLOAD buffers, persistently mapped
    for (ConstantRing& ring : rings_) {
        CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(kRingBytes);
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        if (!CheckHr(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&ring.res)),
                     "MmeRuntime: constant ring")) {
            return false;
        }
        ring.res->SetName(L"mme.constants");
        if (FAILED(ring.res->Map(0, nullptr, (void**)&ring.mapped))) {
            LOG_ERROR("MmeRuntime: failed to map constant ring");
            return false;
        }
        ring.used = 0;
    }

    // built-in passthrough VS (used for passes whose VS is NULL)
    static const char* kQuadVs =
        "struct VSIn { float4 pos : POSITION; float4 tex : TEXCOORD0; };\n"
        "struct VSOut { float4 pos : SV_Position; float4 tex : TEXCOORD0; };\n"
        "VSOut main(VSIn i) { VSOut o; o.pos = i.pos; o.tex = i.tex; return o; }\n";
    ComPtr<ID3DBlob> vsErr;
    D3DCompile(kQuadVs, std::strlen(kQuadVs), "mme_quad", nullptr, nullptr, "main", "vs_5_0",
               D3DCOMPILE_ENABLE_STRICTNESS, 0, &passthroughVs_, &vsErr);
    if (!passthroughVs_)
        LOG_WARN("MmeRuntime: built-in quad VS compile failed%s%s", vsErr ? ": " : "",
                 vsErr ? (const char*)vsErr->GetBufferPointer() : "");
    return true;
}

void MmeRuntime::BeginFrame(Dx12Context& ctx, uint64_t frame) {
    (void)ctx;
    ringSlot_ = (uint32_t)(frame % Dx12Context::kFramesInFlight);
    rings_[ringSlot_].used = 0;
    mipSrvCursor_ = 0;   // M5c fix: the persistent mip SRV tables are re-recorded each frame
}

D3D12_GPU_VIRTUAL_ADDRESS MmeRuntime::AllocConstants(const void* data, uint32_t size) {
    if (!data || !size) return 0;
    ConstantRing& ring = rings_[ringSlot_];
    const uint32_t aligned = (ring.used + 255u) & ~255u;
    if (aligned + size > kRingBytes) {
        if (!ringOverflowLogged_) {
            LOG_WARN("MmeRuntime: constant ring overflow, remaining MME draws this frame are skipped");
            ringOverflowLogged_ = true;
        }
        return 0;
    }
    std::memcpy(ring.mapped + aligned, data, size);
    ring.used = aligned + size;
    return ring.res->GetGPUVirtualAddress() + aligned;
}

const D3D12_VERTEX_BUFFER_VIEW& MmeRuntime::QuadVb(Dx12Context& ctx, uint32_t w, uint32_t h) {
    const uint64_t key = ((uint64_t)w << 32) | h;
    auto it = quads_.find(key);
    if (it != quads_.end()) return it->second.view;

    // M7 fix 3: the quad covers the whole render target, [-1,1] on both axes. A positional half-texel shift (the
    // old `sx = -1/w`, `sy = +1/h`) moved the right edge onto the last pixel's centre and the bottom edge onto the
    // last row's, so rasterisation dropped the right column / bottom row (the Group 4 white border: those pixels
    // kept the target's clear value). With positions [-1,1] and uv [0,1] the interpolated uv at a pixel centre is
    // exactly (i + 0.5) / size — the texel centre — which is what MMD's Draw=Buffer quad gives.
    auto vert = [&](float x, float y, float u, float v) {
        QuadVertex q{};
        q.pos[0] = x; q.pos[1] = y; q.pos[2] = 0.0f; q.pos[3] = 1.0f;
        q.tex[0] = u; q.tex[1] = v; q.tex[2] = 0.0f; q.tex[3] = 0.0f;
        q.normal[0] = 0.0f; q.normal[1] = 0.0f; q.normal[2] = -1.0f; q.normal[3] = 0.0f;
        q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1.0f;
        return q;
    };
    const QuadVertex verts[4] = {vert(-1, 1, 0, 0), vert(1, 1, 1, 0), vert(-1, -1, 0, 1), vert(1, -1, 1, 1)};

    QuadEntry entry;
    CD3DX12_RESOURCE_DESC rd = CD3DX12_RESOURCE_DESC::Buffer(sizeof(verts));
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    if (CheckHr(ctx.Device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&entry.res)),
                "MmeRuntime: quad VB")) {
        entry.res->SetName(L"mme.quad.vb");
        void* mapped = nullptr;
        if (SUCCEEDED(entry.res->Map(0, nullptr, &mapped))) {
            std::memcpy(mapped, verts, sizeof(verts));
            entry.res->Unmap(0, nullptr);
            entry.view.BufferLocation = entry.res->GetGPUVirtualAddress();
            entry.view.SizeInBytes = sizeof(verts);
            entry.view.StrideInBytes = sizeof(QuadVertex);
        }
    }
    auto [ins, _] = quads_.emplace(key, std::move(entry));
    return ins->second.view;
}

MmeEffectInstance* MmeRuntime::Instance(const std::filesystem::path& file, bool objectMode) {
    const std::string base = mmetext::PathKey(file);
    const std::string key = objectMode ? base + "\x01obj" : base;
    auto it = byKey_.find(key);
    return it == byKey_.end() ? nullptr : it->second;
}

MmeEffectInstance* MmeRuntime::EnsureInstance(const std::filesystem::path& file, bool objectMode) {
    const std::string base = mmetext::PathKey(file);
    const std::string key = objectMode ? base + "\x01obj" : base;
    auto it = byKey_.find(key);
    if (it != byKey_.end()) {
        it->second->missingFrames_ = 0;
        return it->second;
    }
    auto inst = std::make_unique<MmeEffectInstance>(file, key, this, objectMode);
    inst->BeginLoad();
    MmeEffectInstance* raw = inst.get();
    byKey_.emplace(key, raw);
    instances_.push_back(std::move(inst));
    return raw;
}

void MmeRuntime::SetObjectShadowMap(Texture* tex) {
    objectShadowMap_ = tex;
    for (auto& inst : instances_) inst->SetObjectShadowMap(tex);
}

void MmeRuntime::ForgetModel(Dx12Context& ctx, const GpuModel* model) {
    for (auto& inst : instances_) inst->ForgetModel(ctx, model);
}

void MmeRuntime::Update(Dx12Context& ctx, const std::vector<MmeWanted>& wanted, uint32_t outW, uint32_t outH) {
    std::unordered_set<std::string> wantedKeys;
    wantedKeys.reserve(wanted.size());
    for (const MmeWanted& w : wanted) {
        const std::string base = mmetext::PathKey(w.file);
        const std::string key = w.objectMode ? base + "\x01obj" : base;
        wantedKeys.insert(key);
        auto it = byKey_.find(key);
        if (it != byKey_.end()) {
            it->second->missingFrames_ = 0;
            continue;
        }
        auto inst = std::make_unique<MmeEffectInstance>(w.file, key, this, w.objectMode);
        inst->BeginLoad();
        byKey_.emplace(key, inst.get());
        instances_.push_back(std::move(inst));
    }

    // finish async loads; create / resize their GPU objects
    for (auto& inst : instances_) {
        if (!inst->loadDone_) inst->FinishLoad(ctx);
        if (inst->Ready()) inst->EnsureTargets(ctx, outW, outH);
    }

    // M6b: swap in a finished hot reload (in place, so the byKey_ pointer and every pass-cached instance stay
    // valid). A failed reload keeps the old instance and reports once.
    for (auto it = reloads_.begin(); it != reloads_.end();) {
        MmeEffectInstance* fresh = it->fresh.get();
        MmeEffectInstance::LoadResult r;
        if (!fresh->TryTakeResult(r)) { ++it; continue; }   // still compiling
        bool compiled = false;
        for (const MmeProgram& p : r.programs) compiled |= p.ok;
        if (!fresh->failed_ && r.desc.ok && compiled) {
            auto byKey = byKey_.find(it->key);
            if (byKey != byKey_.end()) {
                byKey->second->ApplyLoad(ctx, r);   // release + re-create over the old instance
                LOG_INFO("MME RELOADED %s: swapped in", PathToUtf8(byKey->second->file_).c_str());
            }
        } else {
            // a compile / parse failure keeps the old (drawing) instance and reports once
            const std::string msg = PathToUtf8(fresh->file_.filename()) + " (" +
                                    std::to_string(fresh->unsupportedFeatures_.size()) + " errors)";
            LOG_ERROR("MME RELOAD %s: recompile failed, keeping the previous effect", PathToUtf8(fresh->file_).c_str());
            failedReload_[it->key] = it->sig;
            if (errorCb_) errorCb_(msg);
        }
        it = reloads_.erase(it);
    }

    // drop instances the entry list no longer asks for (keep alive kFramesInFlight frames)
    for (auto it = instances_.begin(); it != instances_.end();) {
        MmeEffectInstance* inst = it->get();
        if (wantedKeys.find(inst->key_) == wantedKeys.end()) {
            if (++inst->missingFrames_ >= Dx12Context::kFramesInFlight) {
                inst->Release(ctx);
                byKey_.erase(inst->key_);
                it = instances_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

// ---- M6b: hot reload ---------------------------------------------------------------------------------------------

void MmeRuntime::PollHotReload() {
    // <= 1 Hz: poll changes at the frame rate but stat at most once per second. The runtime has no clock of its
    // own, so the frame number drives it (Dx12Context::FrameNumber is 60 Hz-ish).
    const uint64_t now = ctx_ ? ctx_->FrameNumber() : 0;
    if (now < nextReloadPollFrame_) return;
    nextReloadPollFrame_ = now + 60;

    auto stampOf = [](const std::filesystem::path& p) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(p, ec);
        const auto sz = std::filesystem::file_size(p, ec);
        return PathToUtf8(p) + "|" + std::to_string(ec ? 0 : t.time_since_epoch().count()) + "|" +
               std::to_string(ec ? 0 : sz) + "\n";
    };

    // Start a reload for a changed, already-loaded effect (at most one in flight per key).
    for (auto& inst : instances_) {
        if (!inst->loadDone_ || inst->failed_ || !inst->Ready()) continue;
        bool pending = false;
        for (const Reload& r : reloads_) pending |= r.key == inst->key_;
        if (pending) continue;
        // The signature is file + includes; a new include cannot appear without the file's own mtime changing, so
        // re-stamping the old include list is enough.
        std::string sig = stampOf(inst->file_);
        for (const auto& inc : inst->desc_.includes) sig += stampOf(inc);
        if (sig == inst->signature_) continue;
        // a file version whose reload already failed: do not retry until the file changes again
        auto failed = failedReload_.find(inst->key_);
        if (failed != failedReload_.end() && failed->second == sig) continue;

        // Parse + compile the new version into a detached instance and swap it in when it is ready, so a compile
        // failure keeps the old (drawing) one.
        auto fresh = std::make_unique<MmeEffectInstance>(inst->file_, inst->key_, this, inst->objectMode_);
        fresh->BeginLoad();
        LOG_INFO("MME RELOAD %s: file changed, recompiling", PathToUtf8(inst->file_).c_str());
        reloads_.push_back({inst->key_, sig, std::move(fresh)});
    }
}

void MmeRuntime::Release(Dx12Context& ctx) {
    for (auto& inst : instances_) inst->Release(ctx);
    instances_.clear();
    byKey_.clear();
    reloads_.clear();   // M6b: drop any in-flight hot reload
    failedReload_.clear();
    for (auto& [k, q] : quads_) q.res.Reset();
    quads_.clear();
    for (ConstantRing& ring : rings_) {
        if (ring.mapped) ring.res->Unmap(0, nullptr);
        ring.mapped = nullptr;
        ring.res.Reset();
    }
    samplerHeap_ = DescriptorHeap{};
    rootSig_.Reset();
    passthroughVs_.Reset();
    mipRoot_.Reset();
    mipPsos_.clear();
    if (mipRtv_ != DescriptorHeap::kInvalid) ctx.RtvHeap().Free(mipRtv_, 1);
    mipRtv_ = DescriptorHeap::kInvalid;
    if (mipSrvBase_ != DescriptorHeap::kInvalid)
        ctx.SrvHeap().Free(mipSrvBase_, kMipSrvTables * 8 * Dx12Context::kFramesInFlight);
    mipSrvBase_ = DescriptorHeap::kInvalid;
    mipSrvCursor_ = 0;
    mipLogged_.clear();
    shaderDir_.clear();
    ctx_ = nullptr;
}

// ---- M5a: MME render-target mip generation -----------------------------------------------------------------------

void MmeRuntime::GenerateMips(PassContext& pc, Texture& target) {
    if (!target.res) return;
    // Find the MmeTarget that owns this Texture (its mip count lives there).
    const MmeTarget* mt = nullptr;
    for (const auto& inst : instances_) {
        for (const auto& [name, t] : inst->targets_)
            if (&t.tex == &target) { mt = &t; break; }
        if (mt) break;
    }
    if (!mt || mt->mipLevels <= 1) return;
    const uint32_t mipCount = mt->mipLevels;   // levels 1..mipCount-1 to generate
    Dx12Context& ctx = pc.ctx;
    ID3D12GraphicsCommandList* cmd = pc.cmd;

    // boxdown (inline) shares FullscreenPipeline's root layout: b0 scene constants (unused), b1 root constants
    // (gMipTexel), t0..t7 SRV table, static point-clamp s0. 4 point taps per destination texel = a box filter.
    static const char* kBoxDown =
        "Texture2D gMipSrc : register(t0);\n"
        "SamplerState gMipPoint : register(s0);\n"
        "cbuffer MipCb : register(b1) { float4 gMipTexel; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSFullscreen(uint id : SV_VertexID) {\n"
        "    VSOut o;\n"
        "    float2 uv = float2((id << 1) & 2, id & 2);\n"
        "    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    o.uv = uv;\n"
        "    return o;\n"
        "}\n"
        "float4 PSMipDown(VSOut i) : SV_Target {\n"
        "    const float2 h = 0.5f * gMipTexel.xy;\n"
        "    float4 c = gMipSrc.Sample(gMipPoint, i.uv + float2(-h.x, -h.y))\n"
        "             + gMipSrc.Sample(gMipPoint, i.uv + float2( h.x, -h.y))\n"
        "             + gMipSrc.Sample(gMipPoint, i.uv + float2(-h.x,  h.y))\n"
        "             + gMipSrc.Sample(gMipPoint, i.uv + float2( h.x,  h.y));\n"
        "    return c * 0.25f;\n"
        "}\n";

    if (!mipRoot_) {
        CD3DX12_DESCRIPTOR_RANGE range;
        range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 8, 0);
        CD3DX12_ROOT_PARAMETER params[3];
        params[0].InitAsConstantBufferView(0);
        params[1].InitAsConstants(4, 1);
        params[2].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_STATIC_SAMPLER_DESC samplers[1];
        samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                         D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        CD3DX12_ROOT_SIGNATURE_DESC rs;
        rs.Init(3, params, 1, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE);
        ComPtr<ID3DBlob> blob, err;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1_0, &blob, &err))) {
            LOG_ERROR("MME mips: root signature serialization failed");
            return;
        }
        if (!CheckHr(ctx.Device()->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                       IID_PPV_ARGS(&mipRoot_)),
                     "MME mips: CreateRootSignature"))
            return;
    }

    auto it = mipPsos_.find((uint32_t)target.format);
    if (it == mipPsos_.end()) {
        ComPtr<ID3DBlob> vs, ps, vsErr, psErr;
        const UINT flags = D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        D3DCompile(kBoxDown, std::strlen(kBoxDown), "mme_boxdown", nullptr, nullptr, "VSFullscreen", "vs_5_0", flags, 0,
                   &vs, &vsErr);
        D3DCompile(kBoxDown, std::strlen(kBoxDown), "mme_boxdown", nullptr, nullptr, "PSMipDown", "ps_5_0", flags, 0, &ps,
                   &psErr);
        if (!vs || !ps) {
            LOG_ERROR("MME mips: boxdown compile failed (fmt=%d)%s%s", (int)target.format, vsErr ? ": " : "",
                      vsErr ? (const char*)vsErr->GetBufferPointer() : "");
            mipPsos_[(uint32_t)target.format] = nullptr;   // cache the failure
            return;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.pRootSignature = mipRoot_.Get();
        pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.StencilEnable = FALSE;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = target.format;
        pso.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> created;
        if (!CheckHr(ctx.Device()->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&created)),
                     "MME mips: CreateGraphicsPipelineState")) {
            mipPsos_[(uint32_t)target.format] = nullptr;
            return;
        }
        it = mipPsos_.emplace((uint32_t)target.format, std::move(created)).first;
    }
    ID3D12PipelineState* pso = it->second.Get();
    if (!pso) return;

    ID3D12DescriptorHeap* heaps[1] = {ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootSignature(mipRoot_.Get());
    cmd->SetGraphicsRootConstantBufferView(0, pc.sceneConstants);
    cmd->SetPipelineState(pso);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Mip 0 is a shader resource here (the caller transitioned it); the deeper mips have never been written, so
    // they are also shader resources. To sample mip m-1 while writing mip m the two subresources swap states with
    // per-subresource (MipSlice) barriers and are restored afterwards, leaving the whole texture a shader resource.
    const D3D12_RESOURCE_STATES psr = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    uint32_t srcW = target.width, srcH = target.height;
    D3D12_RESOURCE_BARRIER barriers[2];
    // M5c fix: the per-level SRVs come from the persistent runtime-owned range (one slot per in-flight frame, like
    // the transient windows), not pc.transient (whose 512-descriptor window a full mip chain per effect target
    // exhausted in AutoLuminous4). The slot's previous GPU work is complete (Dx12Context::BeginFrame waited on it).
    if (mipSrvBase_ == DescriptorHeap::kInvalid) {
        mipSrvBase_ = ctx.SrvHeap().Allocate(kMipSrvTables * 8 * Dx12Context::kFramesInFlight);
        if (mipSrvBase_ == DescriptorHeap::kInvalid) {
            LOG_ERROR("MME mips: out of SRV descriptors for the mip tables");
            return;
        }
    }
    for (uint32_t m = 1; m < mipCount; ++m) {
        uint32_t dstW = std::max(1u, srcW / 2), dstH = std::max(1u, srcH / 2);
        const float texel[4] = {1.0f / (float)std::max(1u, srcW), 1.0f / (float)std::max(1u, srcH), 0, 0};
        cmd->SetGraphicsRoot32BitConstants(1, 4, texel, 0);
        barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(target.res.Get(), psr,
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, m - 1);
        barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(target.res.Get(), psr,
                                                           D3D12_RESOURCE_STATE_RENDER_TARGET, m);
        cmd->ResourceBarrier(2, barriers);
        // SRV for the source mip: a custom descriptor naming mip m-1 (the shared table is a plain 2D mip-0 SRV).
        if (mipSrvCursor_ >= kMipSrvTables) {
            if (!mipOverflowLogged_) {
                mipOverflowLogged_ = true;
                LOG_WARN("MME mips: more than %u mip tables in one frame; deeper levels are skipped", kMipSrvTables);
            }
            break;
        }
        const uint32_t idx = mipSrvBase_ +
                             ((uint32_t)ringSlot_ * kMipSrvTables + mipSrvCursor_) * 8;
        ++mipSrvCursor_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = target.format;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MostDetailedMip = m - 1;
        sd.Texture2D.MipLevels = 1;
        ctx.Device()->CreateShaderResourceView(target.res.Get(), &sd, ctx.SrvHeap().Cpu(idx));
        // pad the remaining 7 slots of the t0..t7 table (the shader only reads t0)
        for (uint32_t i = 1; i < 8; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC nd{};
            nd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nd.Texture2D.MipLevels = 1;
            ctx.Device()->CreateShaderResourceView(nullptr, &nd, ctx.SrvHeap().Cpu(idx + i));
        }
        cmd->SetGraphicsRootDescriptorTable(2, ctx.SrvHeap().Gpu(idx));

        // The destination mip's RTV is written into one reusable runtime-owned RTV slot (mip 0 uses the target's
        // own persistent RTV); OMSetRenderTargets copies the handle at record time, so reusing the slot between
        // the mip draws of one frame (and across frames) is safe and needs only one descriptor.
        if (mipRtv_ == DescriptorHeap::kInvalid) {
            mipRtv_ = ctx.RtvHeap().Allocate(1);
            if (mipRtv_ == DescriptorHeap::kInvalid) break;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = target.format;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = m;
        ctx.Device()->CreateRenderTargetView(target.res.Get(), &rd, ctx.RtvHeap().Cpu(mipRtv_));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = ctx.RtvHeap().Cpu(mipRtv_);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{0, 0, (float)dstW, (float)dstH, 0, 1};
        D3D12_RECT sc{0, 0, (LONG)dstW, (LONG)dstH};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->DrawInstanced(3, 1, 0, 0);
        if (m == 1 && mipLogged_.insert((uintptr_t)target.res.Get()).second)
            LOG_INFO("MME MIPS %ux%u levels=%u fmt=%d", target.width, target.height, mipCount,
                     (int)target.format);
        // Restore both subresources to shader-resource form (m-1 keeps being read by the next level's draw).
        barriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(target.res.Get(),
                                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, psr, m - 1);
        barriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(target.res.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, psr,
                                                           m);
        cmd->ResourceBarrier(2, barriers);
        srcW = dstW;
        srcH = dstH;
    }
    target.state = psr;   // the whole texture is a shader resource again

    // Callers draw MME passes right after this (the next offscreen job, the next script pass): put back the MME
    // root signature and both heaps. Leaving the mip root bound made the next SetGraphicsRootDescriptorTable(sampler
    // table) name a heap that was not bound -> driver access violation (HgSpotLight / HgPointLight).
    ID3D12DescriptorHeap* mmeHeaps[2] = {ctx.SrvHeap().Heap(), samplerHeap_.Heap()};
    cmd->SetDescriptorHeaps(2, mmeHeaps);
    cmd->SetGraphicsRootSignature(rootSig_.Get());
}

} // namespace mmdx
