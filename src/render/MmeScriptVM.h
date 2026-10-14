#pragma once
// M3b: MME script VM (CPU only). Interprets a technique's script (or the passes in order when it has none) with
// spec draft §7 semantics over an IMmeHost interface; no D3D12 objects live here.
#include "asset/MmeEffect.h"

#include <string>

namespace mmdx {

// The runtime side of the script VM. All calls are recorded / forwarded by the host.
struct IMmeHost {
    virtual ~IMmeHost() = default;
    virtual void SetColorTarget(int index, const std::string& name) = 0;   // "" = default target
    virtual void SetDepthTarget(const std::string& name) = 0;              // "" = default
    virtual void Clear(bool color, bool depth, const float color4[4], float depthValue) = 0;
    virtual void ScriptExternal() = 0;
    virtual void DrawBuffer(const MmeTechnique& t, const MmePass& p) = 0;
    virtual void DrawGeometry(const MmeTechnique& t, const MmePass& p) = 0;
    virtual bool ReadParam(const std::string& name, float out[4]) = 0;     // for ClearSet*, LoopByCount
    virtual void SetLoopIndex(const std::string& name, int index) = 0;     // LoopGetIndex
    virtual void Diag(const std::string& message) = 0;                     // once-per-message logging by the host
};

// Runs one technique (its script, or its passes in order when it has none) with spec §7 semantics.
// scriptClass: "object" | "scene" | "sceneorobject" (Draw restrictions). Returns the number of draws issued.
int RunMmeTechnique(const MmeEffectDesc& fx, const MmeTechnique& t, const std::string& scriptClass, IMmeHost& host,
                    int maxIterations = 4096);

} // namespace mmdx
