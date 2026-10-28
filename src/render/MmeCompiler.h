#pragma once
// M2: MME pass compiler. Turns one IR pass (MmePass, M1's MmeEffect.h) into a compilable SM5 translation unit:
// the preprocessed source with every technique/asm block blanked (line-preserving) plus a generated wrapper entry,
// compiled with FXC to vs_5_0 / ps_5_0 and reflected through ID3D11ShaderReflection. Results are cached on disk
// (sha256-style key over the wrapper source + target + flags + defines + compiler dll version). Thread-safe: called
// from effect-load worker threads; cache writes go through a tmp file + rename. See docs/mme/mme_design.md §2.2.
#include "asset/MmeEffect.h"

// DirectX-Headers' d3dcommon.h first: an older SDK's (10.0.22621) shares its include guard but lacks the types d3dx12 needs
#include <directx/d3dcommon.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mmdx {

using Microsoft::WRL::ComPtr;

// One variable inside a stage's `$Globals` cbuffer (FXC SM5 packs all loose uniforms here).
struct MmeCbVar {
    std::string name;
    uint32_t offset = 0, size = 0;
    uint32_t rows = 0, columns = 0, elements = 0;
    bool columnMajor = false;
    enum Kind { Float, Int, UInt, Bool, Other } kind = Other;
    std::vector<uint8_t> defaultValue;   // D3D11_SHADER_VARIABLE_DESC::DefaultValue copy; empty when null
};

// One bound texture / sampler register. A DX9 `sampler X : register(s0)` appears as a texture and a sampler both
// named X (FXC splits the combined DX9 sampler into a texture + a sampler object under SM5).
struct MmeResBinding {
    std::string name;
    enum Kind { Texture, Sampler } kind = Texture;
    uint32_t slot = 0;
    D3D_SRV_DIMENSION dim = D3D_SRV_DIMENSION_UNKNOWN;
};

// One element of an input / output signature (from reflection).
struct MmeSigElement {
    std::string semantic;
    uint32_t index = 0;
    uint32_t reg = 0;
    uint8_t mask = 0;
    D3D_REGISTER_COMPONENT_TYPE type = D3D_REGISTER_COMPONENT_UNKNOWN;
    D3D_NAME sv = D3D_NAME_UNDEFINED;
};

// One compiled stage: bytecode + reflected $Globals / resources / signatures.
struct MmeStage {
    ComPtr<ID3DBlob> code;
    uint32_t cbSize = 0;                     // $Globals size in bytes (0 when there is no $Globals)
    std::vector<MmeCbVar> cb;
    std::vector<MmeResBinding> res;
    std::vector<MmeSigElement> inputs, outputs;
};

// One compiled pass (VS + PS, either may be absent).
struct MmeProgram {
    std::string technique, pass;
    MmeStage vs, ps;
    bool ok = false;
    bool usedFallbackSource = false;
    // M5a: the PS wrapper injected an alpha-test clip() from the pass's AlphaTestEnable / AlphaRef / AlphaFunc.
    bool alphaTest = false;
    std::vector<MmeDiag> diags;
    uint32_t psOutputs = 0;       // number of SV_Target elements in the PS output signature
    bool psWritesDepth = false;   // PS output signature contains SV_Depth
    bool usesVertexId = false;    // the VS wrapper bound an input to SV_VertexID (the `_INDEX` fix-up)
    bool usesVpos = false;        // the PS wrapper read SV_Position for a VPOS input (reported by the test summary)
    bool usesVface = false;       // the PS wrapper bound SV_IsFrontFace for a VFACE input (same)
    // M4a object mode (docs/mme/mme_design.md §2.6). `objectMode` is set for passes of object / object_ss / edge
    // techniques when the caller asked for it; then the wrappers add the host varyings (previous clip space, view
    // normal) and the PS wrapper writes our G-buffer MRT (linearised colour, normal, velocity).
    bool objectMode = false;      // this pass was compiled in object mode
    // True when the PS wrapper writes the host's 3-target output (single user colour, no DEPTH). False when the
    // pass is not in object mode, or the user PS writes MRT / DEPTH itself: the caller routes that pass differently.
    bool hostMrt = true;
};

struct MmeCompileOptions {
    bool useCache = true;
    std::filesystem::path cacheDir;   // default <exe>/shader_cache/mme
    // Extra defines. MMDX12=1 and MME_VERSION=37 are added by the compiler unless the caller sets them explicitly.
    std::map<std::string, std::string> defines;
    // M4a: compile object / object_ss / edge passes with the object-mode wrappers (host G-buffer MRT + motion
    // vectors). Ignored for other MMDPass values. Included in the disk cache key.
    bool objectMode = false;
};

// Compiles one pass. Thread-safe. `code` is null for a stage that is absent (NULL shader).
MmeProgram CompileMmePass(const MmeEffectDesc& fx, const MmeTechnique& t, const MmePass& p,
                          const MmeCompileOptions& opt = {});

// Compiles every pass of every technique, deduplicated by (vs fn+args, ps fn+args): identical programs compile once
// and the results are copied. Order matches technique / pass order.
std::vector<MmeProgram> CompileMmeEffect(const MmeEffectDesc& fx, const MmeCompileOptions& opt = {});

// Test / debug hook: the wrapper source generated for one pass, exactly as compiled. Empty on failure.
std::string DumpMmeWrapper(const MmeEffectDesc& fx, const MmeTechnique& t, const MmePass& p,
                           const MmeCompileOptions& opt = {}, std::string* reason = nullptr);

// Diagnostic counters for tools/mme_compile_test (the spec's summary reports cache hits, which the per-pass API does
// not otherwise expose). A hit is a stage whose bytecode came from the disk cache instead of a fresh FXC run.
uint32_t GetMmeCacheHitCount();
void ResetMmeCacheHitCount();
uint32_t GetMmeStageCompileCount();
void ResetMmeStageCompileCount();
uint32_t GetMmeStaticFixupCount();
void ResetMmeStaticFixupCount();

} // namespace mmdx
