// M3b: MME script VM (CPU only). Implements spec draft §7 (commands, defaults, loops, Draw restrictions) over
// IMmeHost. No D3D12 objects. See MmeScriptVM.h.
#include "render/MmeScriptVM.h"

#include <algorithm>
#include <string>
#include <vector>

namespace mmdx {
namespace {

std::string ToLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

bool IEquals(const std::string& a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

std::string TechniqueScript(const MmeTechnique& t) {
    if (const MmeAnnotation* a = FindAnnotation(t.annotations, "Script")) return a->value.str;
    return {};
}

std::string PassScript(const MmePass& p) {
    if (const MmeAnnotation* a = FindAnnotation(p.annotations, "Script")) return a->value.str;
    return {};
}

const MmePass* FindPass(const MmeTechnique& t, const std::string& name) {
    for (const MmePass& p : t.passes)
        if (IEquals(p.name, name)) return &p;
    return nullptr;
}

class Vm {
public:
    Vm(const MmeEffectDesc& fx, const MmeTechnique& t, std::string scriptClass, IMmeHost& host, int maxIterations)
        : fx_(fx), t_(t), host_(host), maxIterations_(maxIterations),
          class_(ToLower(std::move(scriptClass))) {}

    int Run() {
        const std::string script = TechniqueScript(t_);
        if (script.empty()) {
            // Technique without a script: its passes run in order (spec §7.3 / nanoem's synthesis).
            for (const MmePass& p : t_.passes) RunPass(p);
            return draws_;
        }
        std::vector<MmeScriptCommand> cmds = ParseMmeScript(script);
        std::vector<int> match(cmds.size(), -1);
        std::vector<int> stack;
        for (size_t i = 0; i < cmds.size(); ++i) {
            if (cmds[i].command == "LoopByCount") stack.push_back((int)i);
            else if (cmds[i].command == "LoopEnd" && !stack.empty()) {
                match[stack.back()] = (int)i;
                stack.pop_back();
            }
        }
        Exec(cmds, match, 0, (int)cmds.size(), true);
        return draws_;
    }

private:
    // Executes commands in [begin, end); passScope=true for a pass body (Draw only), false for a technique.
    void Exec(const std::vector<MmeScriptCommand>& cmds, const std::vector<int>& match, int begin, int end,
              bool techniqueScope) {
        for (int i = begin; i < end; ++i) {
            if (iterations_ >= maxIterations_) {
                if (!limitHit_) {
                    limitHit_ = true;
                    host_.Diag("MME script: iteration limit reached");
                }
                return;
            }
            const MmeScriptCommand& c = cmds[i];
            if (c.command == "LoopByCount") {
                if (!techniqueScope) { host_.Diag("MME script: LoopByCount in a pass script is ignored"); continue; }
                const int loopEnd = match[i];
                if (loopEnd < 0) { host_.Diag("MME script: LoopByCount without LoopEnd"); continue; }
                float v[4] = {0, 0, 0, 0};
                host_.ReadParam(c.arg, v);
                int count = (int)v[0];
                if (count < 0) count = 0;
                for (int k = 0; k < count; ++k) {
                    if (iterations_++ >= maxIterations_) break;
                    loopStack_.push_back(k);
                    Exec(cmds, match, i + 1, loopEnd, techniqueScope);
                    loopStack_.pop_back();
                    if (iterations_ >= maxIterations_) {
                        if (!limitHit_) {
                            limitHit_ = true;
                            host_.Diag("MME script: iteration limit reached");
                        }
                        break;
                    }
                }
                i = loopEnd;   // skip to the matching LoopEnd (the for loop then advances past it)
                continue;
            }
            if (c.command == "LoopEnd") continue;
            if (c.command == "LoopGetIndex") {
                if (!loopStack_.empty()) host_.SetLoopIndex(c.arg, loopStack_.back());
                continue;
            }
            if (c.command == "RenderColorTarget0" || c.command == "RenderColorTarget1" ||
                c.command == "RenderColorTarget2" || c.command == "RenderColorTarget3") {
                const int idx = c.command.back() - '0';
                host_.SetColorTarget(idx, c.arg);
                continue;
            }
            if (c.command == "RenderDepthStencilTarget") {
                host_.SetDepthTarget(c.arg);
                continue;
            }
            if (c.command == "ClearSetColor") {
                float v[4] = {0, 0, 0, 0};
                host_.ReadParam(c.arg, v);
                clearColor_[0] = v[0]; clearColor_[1] = v[1]; clearColor_[2] = v[2]; clearColor_[3] = v[3];
                continue;
            }
            if (c.command == "ClearSetDepth") {
                float v[4] = {1, 0, 0, 0};
                host_.ReadParam(c.arg, v);
                clearDepth_ = v[0];
                continue;
            }
            if (c.command == "Clear") {
                const bool color = IEquals(c.arg, "Color");
                const bool depth = IEquals(c.arg, "Depth");
                if (!color && !depth) { host_.Diag("MME script: Clear='" + c.arg + "' unknown"); continue; }
                host_.Clear(color, depth, clearColor_, clearDepth_);
                continue;
            }
            if (c.command == "ScriptExternal") {
                if (!techniqueScope) { host_.Diag("MME script: ScriptExternal is technique-only"); continue; }
                host_.ScriptExternal();
                ++draws_;
                continue;
            }
            if (c.command == "Pass") {
                if (!techniqueScope) { host_.Diag("MME script: Pass= is technique-only"); continue; }
                const MmePass* p = FindPass(t_, c.arg);
                if (!p) { host_.Diag("MME script: no pass named '" + c.arg + "'"); continue; }
                RunPass(*p);
                continue;
            }
            if (c.command == "Draw") {
                // Draw is pass-only; a technique script carrying it is malformed.
                if (techniqueScope) { host_.Diag("MME script: Draw= outside a pass"); continue; }
                DrawArg(c.arg);
                continue;
            }
            host_.Diag("MME script: unknown command '" + c.command + "'");
        }
    }

    void RunPass(const MmePass& p) {
        if (iterations_ >= maxIterations_) return;
        currentPass_ = &p;
        const std::string script = PassScript(p);
        std::vector<MmeScriptCommand> cmds;
        if (script.empty()) {
            // Default when the pass has no script: Draw=Geometry (spec §7.3).
            MmeScriptCommand cmd;
            cmd.command = "Draw";
            cmd.arg = "Geometry";
            cmd.known = true;
            cmds.push_back(cmd);
        } else {
            cmds = ParseMmeScript(script);
        }
        const std::vector<int> match(cmds.size(), -1);
        Exec(cmds, match, 0, (int)cmds.size(), false);
        currentPass_ = nullptr;
    }

    void DrawArg(const std::string& arg) {
        if (!currentPass_) return;   // Draw only ever appears inside a pass script
        const bool buffer = IEquals(arg, "Buffer");
        const bool geometry = IEquals(arg, "Geometry");
        if (!buffer && !geometry) { host_.Diag("MME script: Draw='" + arg + "' unknown"); return; }
        if (geometry && class_ == "scene") {
            host_.Diag("MME script: Draw=Geometry is forbidden in a scene-class effect");
            return;
        }
        if (buffer && class_ == "object") {
            host_.Diag("MME script: Draw=Buffer is forbidden in an object-class effect");
            return;
        }
        if (buffer) host_.DrawBuffer(t_, *currentPass_);
        else host_.DrawGeometry(t_, *currentPass_);
        ++draws_;
    }

    const MmeEffectDesc& fx_;
    const MmeTechnique& t_;
    IMmeHost& host_;
    int maxIterations_;
    std::string class_;
    int draws_ = 0;
    int iterations_ = 0;
    bool limitHit_ = false;
    std::vector<int> loopStack_;
    const MmePass* currentPass_ = nullptr;
    float clearColor_[4] = {0, 0, 0, 0};
    float clearDepth_ = 1.0f;
};

} // namespace

int RunMmeTechnique(const MmeEffectDesc& fx, const MmeTechnique& t, const std::string& scriptClass, IMmeHost& host,
                    int maxIterations) {
    Vm vm(fx, t, scriptClass, host, maxIterations);
    return vm.Run();
}

} // namespace mmdx
