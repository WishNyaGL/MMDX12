#pragma once
// M6b: shared MME effect diagnostics for the shader manager's "MME" tab (docs/mme/mme_design.md §2.13). Parses an
// effect and compiles every pass on a worker thread (the same path the runtime's async load uses), then reports
// the compile status per pass, unsupported features and missing textures. CPU only (no GPU objects), so it can run
// any time; the UI polls the future for the result.
#include "asset/MmeEffect.h"

#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace mmdx {

struct MmeEffectDiag {
    bool ok = false;                            // the file parsed and at least one pass compiled
    std::string kind;                           // "post" | "pre" | "object"
    // One entry per compiled pass, in file order: "technique / pass — OK" or "technique / pass — <first error line>".
    struct Pass { std::string technique, pass; bool ok = true; std::string error; };
    std::vector<Pass> passes;
    std::vector<std::string> unsupported;       // parser / compile errors (asm, D3D9-only, macro-dependent syntax)
    std::vector<std::string> missingTextures;   // texture params whose ResourceName could not be resolved / decoded
    uint32_t errorCount = 0, warningCount = 0;
};

// Parse + compile `fx` (CPU only) and build its diagnostics. Never throws; an unreadable file returns ok=false.
MmeEffectDiag BuildMmeEffectDiag(const std::filesystem::path& fx);

// Owns one background diagnostics compile; the shader manager's MME tab starts one on selection and polls it.
// Only one runs at a time (starting a new one replaces a finished / running previous one).
class MmeDiagJob {
public:
    void Start(const std::filesystem::path& fx);
    bool Busy() const { return future_.valid(); }
    // Polls the future. Returns true once a result is available (and consumes it); false while still running.
    bool Poll(MmeEffectDiag& out);
    void Reset();

private:
    std::filesystem::path file_;
    std::future<MmeEffectDiag> future_;
    bool ready_ = false;
    MmeEffectDiag result_;
};

} // namespace mmdx
