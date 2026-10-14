// M3c: MmePostPass — runs MME post effects (RenderSettings::mmeEffects, CLI --mme) on the real-time frame.
//
// The pass is also the IMmeHost the script VM runs over. Per frame it drives an MmeRuntime (M3a) with the enabled,
// ready, post-class entries in list order, chaining them through two RGBA8 ping-pong targets at output resolution:
// each effect reads the previous result (or targets.ldr for the first) and writes its default target, then the last
// result is copied back into targets.ldr. `ScriptExternal=Color` blits the chain input into the currently bound
// colour target (a fullscreen copy, one PSO per destination format, created lazily).
//
// Zero effects: nothing allocates and nothing runs (a byte-identical frame, like PackEffectPass). A non-lit view
// (unlit / wireframe / quad) is skipped the same way. Any failure marks the instance failed ([E] once) and skips it
// afterwards.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/MmeRuntime.h"
#include "render/MmeSemantics.h"
#include "asset/MmeText.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <directx/d3dx12.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>

using namespace DirectX;

namespace mmdx {

struct MmePostPass::Impl : public IMmeHost {
    Dx12Context* ctx = nullptr;
    std::filesystem::path shaderDir;
    std::filesystem::path blitFile;
    MmeRuntime* rt = nullptr;   // the renderer-owned runtime (PassContext::mme), set per Execute

    Texture ping[2];
    uint32_t pingW = 0, pingH = 0;

    std::map<DXGI_FORMAT, std::unique_ptr<FullscreenPipeline>> blits;
    // M7 fix 3: the copy back into the LDR image is an opaque fullscreen blit (PSBlitOpaque), not a CopyResource:
    // MMD's back buffer is XRGB, so the MME chain's output alpha must not become the scene's coverage alpha.
    std::map<DXGI_FORMAT, std::unique_ptr<FullscreenPipeline>> opaqueBlits;

    // One MmeConstantWriter per (instance, program, stage), bound once and cached.
    struct WriterKey {
        std::string inst;
        int program = 0, stage = 0;
        bool operator<(const WriterKey& o) const {
            if (inst != o.inst) return inst < o.inst;
            if (program != o.program) return program < o.program;
            return stage < o.stage;
        }
    };
    std::map<WriterKey, std::unique_ptr<MmeConstantWriter>> writers;
    // (The stage SRV tables are persistent and cached inside MmeEffectInstance::StageSrvTable since the M4 fix;
    // this pass no longer needs a per-frame table cache.)

    std::unordered_map<std::string, std::chrono::steady_clock::time_point> firstSeen;
    std::unordered_set<std::string> warnedKind;    // "object"/"pre" warning printed
    std::unordered_set<std::string> failedInst;    // failed instances (skipped afterwards)
    std::unordered_set<std::string> loggedFrame;   // "[I] MME FRAME" printed (kept for completeness)
    std::unordered_set<std::string> loggedDiag;    // once-per-message diagnostics

    // per-effect run state
    PassContext* pc = nullptr;
    MmeEffectInstance* inst = nullptr;
    std::string instKey;
    std::filesystem::path instFile;
    Texture* chainInput = nullptr;
    Texture* chainOut = nullptr;
    MmeFrameValues frame;
    std::map<std::string, float> loopOverrides;
    uint32_t draws = 0;

    // bindings (persist across passes within one effect)
    Texture* color[4] = {nullptr, nullptr, nullptr, nullptr};
    bool colorBound[4] = {false, false, false, false};
    Texture* depth = nullptr;
    bool depthBound = false;
    DXGI_FORMAT depthDsvFormat = DXGI_FORMAT_D32_FLOAT;   // fix 1: the bound RENDERDEPTHSTENCILTARGET's DSV format

    bool EnsureRuntime(Dx12Context& c, const std::filesystem::path& dir) {
        ctx = &c;
        shaderDir = dir;
        blitFile = dir / L"mme" / L"blit.hlsl";
        return true;
    }

    bool EnsurePing(Dx12Context& c, uint32_t w, uint32_t h) {
        if (ping[0] && pingW == w && pingH == h) return true;
        c.WaitForGpu();
        ping[0].Release(c);
        ping[1].Release(c);
        pingW = w;
        pingH = h;
        const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        bool ok = ping[0].Create(c, w, h, RenderTargets::kLdrFormat, rt, kSrv, L"mme.ping.a");
        ok &= ping[1].Create(c, w, h, RenderTargets::kLdrFormat, rt, kSrv, L"mme.ping.b");
        if (!ok) {
            LOG_ERROR("MmePostPass: ping-pong target creation failed (%ux%u)", w, h);
            ping[0].Release(c);
            ping[1].Release(c);
            return false;
        }
        return true;
    }

    FullscreenPipeline* Blit(DXGI_FORMAT fmt, bool opaque = false) {
        std::map<DXGI_FORMAT, std::unique_ptr<FullscreenPipeline>>& cache = opaque ? opaqueBlits : blits;
        auto it = cache.find(fmt);
        if (it != cache.end()) return it->second.get();
        auto p = std::make_unique<FullscreenPipeline>();
        if (!p->Create(*ctx, blitFile, opaque ? "PSBlitOpaque" : "PSBlit", {fmt})) {
            LOG_ERROR("MmePostPass: blit pipeline creation failed (fmt=%d)", (int)fmt);
            return nullptr;
        }
        FullscreenPipeline* raw = p.get();
        cache.emplace(fmt, std::move(p));
        return raw;
    }

    // M7 fix 3: prime the chain output with the chain input (the "default target" starts as the current frame).
    void PrimeOutput(Dx12Context& c, Texture& in, Texture& out) {
        if (in.res == out.res) return;
        ID3D12GraphicsCommandList* cmd = pc->cmd;
        in.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        out.Transition(cmd, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(out.res.Get(), in.res.Get());
        in.Transition(cmd, kSrv);
        out.Transition(cmd, kSrv);
    }

    // M7 fix 3: copy a chain result into `out` as an opaque fullscreen blit (rgb only, alpha forced to 1). Falls
    // back to a raw CopyResource when the pipelined blit cannot be created (no ctx / format mismatch).
    // (Implements MmePostPass::CopyBackOpaque, declared in Passes.h.)
    void CopyBackOpaque(PassContext& pc, Texture& out, Texture& src) {
        if (&out == &src) return;
        if (out.format == src.format && ctx) {
            if (FullscreenPipeline* blit = Blit(out.format, /*opaque=*/true)) {
                src.Transition(pc.cmd, kSrv);
                out.Transition(pc.cmd, kRt);
                blit->Draw(pc, {&out}, pc.transient.SrvTable(pc.ctx, {&src}));
                out.Transition(pc.cmd, kSrv);
                return;
            }
        }
        out.Transition(pc.cmd, D3D12_RESOURCE_STATE_COPY_DEST);
        src.Transition(pc.cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pc.cmd->CopyResource(out.res.Get(), src.res.Get());
        out.Transition(pc.cmd, kSrv);
        src.Transition(pc.cmd, kSrv);
    }

    void DiagOnce(const std::string& message) {
        const std::string key = instKey + '\n' + message;
        if (!loggedDiag.insert(key).second) return;
        const std::string name = instFile.empty() ? instKey : PathToUtf8(instFile);
        LOG_WARN("MME %s: %s", name.c_str(), message.c_str());
    }

    void Fail(const char* what) {
        if (instKey.empty()) return;
        if (failedInst.insert(instKey).second)
            LOG_ERROR("MME %s: %s", PathToUtf8(instFile).c_str(), what);
    }

    MmeConstantWriter* Writer(int program, int stage) {
        if (!inst) return nullptr;
        WriterKey key{instKey, program, stage};
        auto it = writers.find(key);
        if (it != writers.end()) return it->second.get();
        const std::vector<MmeProgram>& progs = inst->Programs();
        if (program < 0 || program >= (int)progs.size()) return nullptr;
        const MmeStage& st = stage ? progs[program].ps : progs[program].vs;
        auto w = std::make_unique<MmeConstantWriter>();
        std::vector<MmeDiag> diags;
        w->Bind(inst->Desc(), st, diags);
        for (const MmeDiag& d : diags) {
            const std::string key2 = instKey + '\n' + d.message;
            if (loggedDiag.insert(key2).second)
                LOG_WARN("MME %s: %s", PathToUtf8(instFile).c_str(), d.message.c_str());
        }
        MmeConstantWriter* raw = w.get();
        writers.emplace(key, std::move(w));
        return raw;
    }

    bool BindStage(int program, bool pixel) {
        ID3D12GraphicsCommandList* cmd = pc->cmd;
        MmeConstantWriter* w = Writer(program, pixel ? 1 : 0);
        D3D12_GPU_VIRTUAL_ADDRESS cbAddr = pc->sceneConstants;   // valid CBV even without a $Globals cbuffer
        if (w) {
            if (!w->Bytes().empty()) {
                w->Write(frame);
                for (const auto& [name, value] : loopOverrides) w->SetScalar(name, value);
                cbAddr = rt->AllocConstants(w->Bytes().data(), (uint32_t)w->Bytes().size());
                if (cbAddr == 0) {
                    Fail("constant buffer allocation failed, skipped");
                    return false;
                }
            }
        }
        const int base = pixel ? 3 : 0;
        cmd->SetGraphicsRootConstantBufferView(base, cbAddr);
        // The stage SRV table is persistent and cached inside the instance (per (program, stage), rebuilt when an
        // effect target it references is recreated); it consumes no transient descriptors.
        const D3D12_GPU_DESCRIPTOR_HANDLE srv = inst->StageSrvTable(*pc, program, pixel);
        if (srv.ptr) cmd->SetGraphicsRootDescriptorTable(base + 1, srv);
        D3D12_GPU_DESCRIPTOR_HANDLE smp = inst->StageSamplerTable(program, pixel);
        if (smp.ptr) cmd->SetGraphicsRootDescriptorTable(base + 2, smp);
        return true;
    }

    Texture* ResolveColor(int index) const {
        if (index == 0) return (colorBound[0] && color[0]) ? color[0] : chainOut;
        return (colorBound[index] && color[index]) ? color[index] : nullptr;
    }

    void Draw(const MmeTechnique& t, const MmePass& p) {
        if (!inst || !pc) return;
        const int program = inst->FindProgram(t.name, p.name);
        if (program < 0) {
            DiagOnce("pass '" + p.name + "' has no compiled program, skipped");
            return;
        }

        DXGI_FORMAT rtFormats[4];
        Texture* rts[4];
        uint32_t rtCount = 0;
        for (int i = 0; i < 4; ++i) {
            Texture* tex = ResolveColor(i);
            if (!tex) continue;
            // A target whose creation failed has no resource / RTV: skip it rather than bind a null view.
            if (!tex->res || tex->rtv == DescriptorHeap::kInvalid) continue;
            rts[rtCount] = tex;
            rtFormats[rtCount] = tex->format;
            ++rtCount;
        }
        if (!rtCount) return;

        const bool hasDepth = depth && depth->res && depth->dsv != DescriptorHeap::kInvalid;
        const DXGI_FORMAT dsFormat = hasDepth ? depthDsvFormat : DXGI_FORMAT_UNKNOWN;
        ID3D12PipelineState* pso = inst->Pso(*ctx, program, p, rtFormats, rtCount, dsFormat);
        if (!pso) {
            Fail("PSO creation failed, skipped");
            return;
        }

        ID3D12GraphicsCommandList* cmd = pc->cmd;
        for (uint32_t i = 0; i < rtCount; ++i) rts[i]->Transition(cmd, kRt);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
        for (uint32_t i = 0; i < rtCount; ++i) rtvs[i] = pc->ctx.RtvHeap().Cpu(rts[i]->rtv);
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        if (hasDepth) {
            depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            dsv = pc->ctx.DsvHeap().Cpu(depth->dsv);
        }
        cmd->OMSetRenderTargets(rtCount, rtvs, FALSE, hasDepth ? &dsv : nullptr);
        // fix 1: D3D9's STENCILREF becomes D3D12's per-draw stencil reference (the PSO only carries the compare /
        // ops / masks).
        cmd->OMSetStencilRef(MmeEffectInstance::StencilRefOf(p));

        const uint32_t vw = rts[0]->width, vh = rts[0]->height;
        D3D12_VIEWPORT vp{0.0f, 0.0f, (float)vw, (float)vh, 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, (LONG)vw, (LONG)vh};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);
        cmd->SetGraphicsRootSignature(rt->RootSignature());
        cmd->SetPipelineState(pso);
        if (!BindStage(program, false) || !BindStage(program, true)) return;

        const D3D12_VERTEX_BUFFER_VIEW& vb = rt->QuadVb(pc->ctx, vw, vh);
        cmd->IASetVertexBuffers(0, 1, &vb);
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        cmd->DrawInstanced(4, 1, 0, 0);
        ++draws;

        for (uint32_t i = 0; i < rtCount; ++i) rt->GenerateMips(*pc, *rts[i]);
        for (uint32_t i = 0; i < rtCount; ++i) rts[i]->Transition(cmd, kSrv);
        if (hasDepth) depth->Transition(cmd, kSrv);
    }

    // Runs one effect over `input`, writing its default-target result into `output`. Returns false when it did not run.
    bool RunEffect(Texture* input, Texture* output, const MmeEffectEntry& entry, uint32_t outW, uint32_t outH) {
        inst = rt ? rt->Instance(entry.file) : nullptr;
        if (!inst || !inst->Ready()) {
            inst = nullptr;
            return false;
        }
        // Missing chain textures (allocation failure / resize race): skip rather than dereference.
        if (!input || !input->res || !output || !output->res) {
            inst = nullptr;
            return false;
        }
        instKey = mmetext::PathKey(entry.file);
        instFile = entry.file;
        if (failedInst.count(instKey)) {
            inst = nullptr;
            return false;
        }

        const MmeEffectDesc& d = inst->Desc();
        const std::string kind = d.kind();
        if (kind != "post") {
            if (warnedKind.insert(instKey).second) {
                if (kind == "object")
                    LOG_WARN("MME %s: object effect (needs M4), skipped", PathToUtf8(entry.file).c_str());
                else
                    LOG_WARN("MME %s: pre effect (later milestone), skipped", PathToUtf8(entry.file).c_str());
            }
            inst = nullptr;
            return false;
        }
        if (d.techniques.empty()) {
            Fail("no technique, skipped");
            inst = nullptr;
            return false;
        }

        // time: seconds since the instance was first seen (steady clock)
        const auto now = std::chrono::steady_clock::now();
        auto [seen, inserted] = firstSeen.try_emplace(instKey, now);
        (void)inserted;
        const float t = std::chrono::duration_cast<std::chrono::duration<float>>(now - seen->second).count();

        // camera matrices without jitter (post effects run at output size)
        const CameraParams& cam = pc->view.camera;
        const XMMATRIX viewM = XMLoadFloat4x4(&cam.view);
        const XMMATRIX projM = XMMatrixPerspectiveFovLH(cam.fovYRadians, (float)outW / (float)outH, cam.nearZ, cam.farZ);
        frame = MmeFrameValues{};
        XMStoreFloat4x4(&frame.view, viewM);
        XMStoreFloat4x4(&frame.proj, projM);
        frame.cameraPos = cam.eye;
        const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
        XMStoreFloat3(&frame.cameraDir, XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invView)));
        XMStoreFloat3(&frame.lightDir, XMVector3Normalize(XMLoadFloat3(&pc->view.light.direction)));
        frame.lightColor = pc->view.light.color;
        // M5a fix 2: the MMD self-shadow light camera (Object="Light" matrices) computed once for this effect frame.
        {
            std::vector<const GpuModel*> chars;
            chars.reserve(pc->view.models.size());
            for (const GpuModel* m : pc->view.models) chars.push_back(m);
            BuildLightCamera(frame.lightDir, chars, pc->view.mmeObjects, frame.lightView, frame.lightProj);
        }
        frame.viewportW = (float)outW;
        frame.viewportH = (float)outH;
        frame.time = t;
        frame.elapsed = pc->frameTimeMs * 0.001f;
        // M8a: the MME scene clock follows the song / motion clock (paused = constant, seeks jump). MME TIME without
        // SyncInEditMode uses the wall clock only in edit mode (the studio); while playing / rendering it is the
        // frame time (spec draft §4.6).
        frame.sceneTime = pc->view.sceneTime;
        frame.sceneElapsed = pc->view.sceneElapsed;
        if (!pc->view.mmeEditMode) {
            frame.time = frame.sceneTime;
            frame.elapsed = frame.sceneElapsed;
        }
        const MmeEffectEntry& e = entry;
        frame.self.x = e.x;
        frame.self.y = e.y;
        frame.self.z = e.z;
        frame.self.rx = XMConvertToRadians(e.rx);
        frame.self.ry = XMConvertToRadians(e.ry);
        frame.self.rz = XMConvertToRadians(e.rz);
        frame.self.si = e.si;
        frame.self.tr = e.tr;
        frame.self.visible = e.visible;
        // Object lookups (other CONTROLOBJECT names) are not reachable from a render pass in M3: the binder uses the
        // spec's missing-object defaults.
        frame.lookup = {};
        // fix 5: TEXTUREVALUE — the runtime keeps a CPU copy of each TEXTUREVALUE parameter's texels; the binder
        // reads them through this provider (an effect render target is unsupported; the runtime logged it at load).
        frame.textureValue = [this](const std::string& name, uint32_t& outCount) -> const float* {
            return inst ? inst->TexValueFor(name, outCount) : nullptr;
        };

        color[0] = color[1] = color[2] = color[3] = nullptr;
        colorBound[0] = colorBound[1] = colorBound[2] = colorBound[3] = false;
        depth = nullptr;
        depthBound = false;
        loopOverrides.clear();
        chainInput = input;
        chainOut = output;
        draws = 0;

        // fix 4: advance this effect's ANIMATEDTEXTUREs to the current scene clock before the draws bind them.
        inst->UpdateAnimatedTextures(*pc);

        // M7 fix 3: MMD's "default target" (RenderColorTarget=;) is the BACK BUFFER, which already holds the scene.
        // Our chain output is a fresh ping target: a pass that switches to the default target and blends onto it
        // (ObjectLuminous / PanelLight's final SRCBLEND=ONE/DESTBLEND=ONE Mix) would otherwise add onto stale
        // content. Prime `output` with the chain input first, so "default target" starts as the current frame.
        PrimeOutput(pc->ctx, *input, *output);

        output->Transition(pc->cmd, kRt);
        RunMmeTechnique(d, d.techniques[0], d.scriptClass, *this);
        output->Transition(pc->cmd, kSrv);

        if (!draws) {
            // the technique wrote nothing to `output` (e.g. a disabled LoopByCount): the chain input is preserved
            inst = nullptr;
            return false;
        }
        if (loggedFrame.insert(instKey).second)
            LOG_INFO("MME FRAME %s draws=%u", PathToUtf8(entry.file).c_str(), draws);

        inst = nullptr;
        return true;
    }

    // ---- IMmeHost ----
    void SetColorTarget(int index, const std::string& name) {
        if (index < 0 || index > 3) return;
        colorBound[index] = true;
        if (name.empty()) {
            color[index] = nullptr;
            return;
        }
        MmeTarget* t = inst ? inst->FindTarget(name) : nullptr;
        if (!t || !t->tex) {
            DiagOnce("RenderColorTarget '" + name + "' not found");
            color[index] = nullptr;
            return;
        }
        color[index] = &t->tex;
    }

    void SetDepthTarget(const std::string& name) {
        depthBound = true;
        if (name.empty()) {
            depth = nullptr;
            return;
        }
        MmeTarget* t = inst ? inst->FindTarget(name) : nullptr;
        if (!t || !t->tex) {
            DiagOnce("RenderDepthStencilTarget '" + name + "' not found");
            depth = nullptr;
            return;
        }
        depth = &t->tex;
        depthDsvFormat = t->dsvFormat;
    }

    void Clear(bool color_, bool depth_, const float color4[4], float depthValue) {
        ID3D12GraphicsCommandList* cmd = pc->cmd;
        if (color_) {
            for (int i = 0; i < 4; ++i) {
                Texture* t = ResolveColor(i);
                if (!t || !t->res) continue;
                t->Transition(cmd, kRt);
                if (t->rtv == DescriptorHeap::kInvalid) continue;
                cmd->ClearRenderTargetView(pc->ctx.RtvHeap().Cpu(t->rtv), color4, 0, nullptr);
            }
        }
        if (depth_ && depth && depth->res && depth->dsv != DescriptorHeap::kInvalid) {
            depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmd->ClearDepthStencilView(pc->ctx.DsvHeap().Cpu(depth->dsv), D3D12_CLEAR_FLAG_DEPTH, depthValue, 0, 0,
                                       nullptr);
        }
    }

    void ScriptExternal() {
        Texture* t = ResolveColor(0);
        if (!t || !t->res || !chainInput || !chainInput->res) return;
        FullscreenPipeline* blit = Blit(t->format);
        if (!blit) {
            Fail("blit pipeline unavailable, skipped");
            return;
        }
        ID3D12GraphicsCommandList* cmd = pc->cmd;
        chainInput->Transition(cmd, kSrv);
        t->Transition(cmd, kRt);
        const D3D12_GPU_DESCRIPTOR_HANDLE table = pc->transient.SrvTable(pc->ctx, {chainInput});
        blit->Draw(*pc, {t}, table);
        t->Transition(cmd, kSrv);
        // the bound depth target is only cleared
        if (depth && depth->res && depth->dsv != DescriptorHeap::kInvalid) {
            depth->Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmd->ClearDepthStencilView(pc->ctx.DsvHeap().Cpu(depth->dsv), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            depth->Transition(cmd, kSrv);
        }
    }

    void SetLoopIndex(const std::string& name, int index) { loopOverrides[name] = (float)index; }

    void DrawBuffer(const MmeTechnique& t, const MmePass& p) override { Draw(t, p); }
    void DrawGeometry(const MmeTechnique&, const MmePass&) override {
        DiagOnce("Draw=Geometry is not supported for post effects, skipped");
    }
    void Diag(const std::string& message) override { DiagOnce(message); }

    bool ReadParam(const std::string& name, float out[4]) {
        if (!inst) return false;
        std::vector<MmeConstantWriter*> ws;
        for (auto& [k, w] : writers)
            if (k.inst == instKey) ws.push_back(w.get());
        return MmeConstantWriter::ReadParam(inst->Desc(), ws, name, out);
    }

    void ReleaseRuntime(Dx12Context& c) {
        // the runtime is owned by the renderer; nothing to release here (Il/instances are cleaned up by the
        // renderer's MmeRuntime::Release). Keep this for the destructor's ctx-validity guard.
        (void)c;
    }
};

MmePostPass::MmePostPass() : impl_(std::make_unique<Impl>()) {}
MmePostPass::~MmePostPass() {
    // the runtime is owned by Renderer (PassContext::mme); no GPU objects here
}

bool MmePostPass::CreatePipelines(Dx12Context& ctx, const std::filesystem::path& shaderDir, uint32_t) {
    // The runtime (root signature, sampler heap, constant rings) is owned by the Renderer and passed in through
    // PassContext::mme; this pass only keeps its own ping-pong targets / blit pipelines.
    impl_->ctx = &ctx;
    impl_->shaderDir = shaderDir;
    impl_->blitFile = shaderDir / L"mme" / L"blit.hlsl";
    return true;
}

void MmePostPass::OnResize(Dx12Context& ctx, RenderTargets&) {
    impl_->ping[0].Release(ctx);
    impl_->ping[1].Release(ctx);
    impl_->pingW = impl_->pingH = 0;
}

void MmePostPass::ReleaseTargets(Dx12Context& ctx) {
    impl_->ping[0].Release(ctx);
    impl_->ping[1].Release(ctx);
    impl_->pingW = impl_->pingH = 0;
}

// M5b: copies a chain result back into the image it started from (an odd number of effects). M7 fix 3: opaque blit
// (rgb, alpha = 1): MMD's back buffer is XRGB, so the MME output alpha must not become the scene's coverage alpha.
template <typename ImplT>
static void textureCopyBack(ImplT& im, PassContext& pc, Texture& out, Texture& src) {
    im.CopyBackOpaque(pc, out, src);
}

// M5c: only a `.fx` runs in the post chain; a persisted stack `.x` entry is an effect object (handled through
// FrameView::mmeEffectObjects), not a post effect.
static bool PostChainFx(const std::filesystem::path& file) {
    std::wstring e = file.extension().wstring();
    for (wchar_t& c : e) c = (wchar_t)towlower(c);
    return e == L".fx";
}

// M5c: the post/scene part of every effect object runs in the post chain with its panel as `(self)`. Converts the
// object list into entries (panel radians -> the entry's degrees) and appends them after the user's stack.
static void AppendEffectObjectEntries(const FrameView& view, std::vector<MmeEffectEntry>& out) {
    for (const MmeEffectObject& o : view.mmeEffectObjects) {
        if (!o.enabled || o.fx.empty()) continue;
        MmeEffectEntry e;
        e.file = o.fx;
        e.x = o.panel.x;
        e.y = o.panel.y;
        e.z = o.panel.z;
        e.rx = DirectX::XMConvertToDegrees(o.panel.rx);
        e.ry = DirectX::XMConvertToDegrees(o.panel.ry);
        e.rz = DirectX::XMConvertToDegrees(o.panel.rz);
        e.si = o.panel.si;
        e.tr = o.panel.tr;
        e.visible = o.panel.visible;
        out.push_back(std::move(e));
    }
}

void MmePostPass::Execute(PassContext& pc) {
    Impl& im = *impl_;
    // Unlit / wireframe / quad views draw flat: nothing for a post effect to read (same as PackEffectPass).
    if (pc.settings.shading != ViewShading::Lit || !pc.view.extraViews.empty()) return;

    // M5c: the effect objects' post/scene parts run in the same chain (their own panel is `(self)`).
    std::vector<MmeEffectEntry> combined;
    combined.reserve(pc.settings.mmeEffects.size() + pc.view.mmeEffectObjects.size());
    for (const MmeEffectEntry& e : pc.settings.mmeEffects) combined.push_back(e);
    AppendEffectObjectEntries(pc.view, combined);
    std::vector<const MmeEffectEntry*> entries;
    for (const MmeEffectEntry& e : combined) {
        if (!e.enabled || e.file.empty()) continue;
        if (!PostChainFx(e.file)) continue;   // an `.x` stack entry is an effect object, not a post effect
        entries.push_back(&e);
    }
    if (entries.empty()) return;
    // No output surface this frame (window minimised / zero-size targets): nothing to post-process.
    if (pc.targets.outWidth == 0 || pc.targets.outHeight == 0 || !pc.targets.ldr.res) return;
    im.pc = &pc;
    im.rt = pc.mme;
    if (!im.rt) return;

    // Is at least one entry a ready post-class instance? (the renderer's MmeRuntime::Update already created /
    // finished them and BeginFrame rewound the constant ring this frame.)
    bool anyRunnable = false;
    for (const MmeEffectEntry* e : entries) {
        MmeEffectInstance* inst = im.rt->Instance(e->file);
        if (!inst || !inst->Ready()) continue;
        if (im.failedInst.count(mmetext::PathKey(e->file))) continue;
        if (inst->Desc().kind() != "post") continue;
        anyRunnable = true;
        break;
    }
    if (!anyRunnable || !im.EnsurePing(pc.ctx, pc.targets.outWidth, pc.targets.outHeight)) {
        im.pc = nullptr;
        return;
    }

    // MME draws bind the runtime's sampler heap alongside the SRV heap; other passes expect SRV-only.
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), im.rt->SamplerHeap()};
    pc.cmd->SetDescriptorHeaps(2, heaps);

    Texture* src = &pc.targets.ldr;
    uint32_t cur = 0;
    bool ran = false;
    for (const MmeEffectEntry* e : entries) {
        Texture* dst = &im.ping[cur];
        if (im.RunEffect(src, dst, *e, pc.targets.outWidth, pc.targets.outHeight)) {
            src = dst;
            cur ^= 1;
            ran = true;
        }
    }

    if (ran) {
        textureCopyBack(im, pc, pc.targets.ldr, *src);
    }

    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    pc.cmd->SetDescriptorHeaps(1, restore);
    im.pc = nullptr;
}

// M5b: the same chain on an offline image: `io` is the chain input / output, `scratch` the ping-pong partner.
bool MmePostPass::RunOffline(PassContext& pc, const std::vector<MmeEffectEntry>& effects, Texture& io,
                             Texture& scratch) {
    Impl& im = *impl_;
    std::vector<const MmeEffectEntry*> entries;
    for (const MmeEffectEntry& e : effects) {
        if (!e.enabled || e.file.empty()) continue;
        entries.push_back(&e);
    }
    if (entries.empty() || !io.res || !scratch.res || io.format != scratch.format) return false;
    im.pc = &pc;
    im.rt = pc.mme;
    if (!im.rt) {
        im.pc = nullptr;
        return false;
    }
    bool anyRunnable = false;
    for (const MmeEffectEntry* e : entries) {
        MmeEffectInstance* inst = im.rt->Instance(e->file);
        if (!inst || !inst->Ready()) continue;
        if (im.failedInst.count(mmetext::PathKey(e->file))) continue;
        if (inst->Desc().kind() != "post") continue;
        anyRunnable = true;
        break;
    }
    if (!anyRunnable) {
        im.pc = nullptr;
        return false;
    }

    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), im.rt->SamplerHeap()};
    pc.cmd->SetDescriptorHeaps(2, heaps);

    Texture* src = &io;
    Texture* dst = &scratch;
    bool ran = false;
    for (const MmeEffectEntry* e : entries) {
        if (im.RunEffect(src, dst, *e, io.width, io.height)) {
            std::swap(src, dst);
            ran = true;
        }
    }
    if (ran && src != &io) textureCopyBack(im, pc, io, *src);

    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    pc.cmd->SetDescriptorHeaps(1, restore);
    im.pc = nullptr;
    return ran;
}

// ---- IMmeHost forwards -------------------------------------------------------------------------------------------

// M7 fix 3: forward the opaque copy-back to the implementation.
void MmePostPass::CopyBackOpaque(PassContext& pc, Texture& out, Texture& src) { impl_->CopyBackOpaque(pc, out, src); }

void MmePostPass::SetColorTarget(int index, const std::string& name) { impl_->SetColorTarget(index, name); }
void MmePostPass::SetDepthTarget(const std::string& name) { impl_->SetDepthTarget(name); }
void MmePostPass::Clear(bool color, bool depth, const float color4[4], float depthValue) {
    impl_->Clear(color, depth, color4, depthValue);
}
void MmePostPass::ScriptExternal() { impl_->ScriptExternal(); }
void MmePostPass::DrawBuffer(const MmeTechnique& t, const MmePass& p) { impl_->Draw(t, p); }
void MmePostPass::DrawGeometry(const MmeTechnique&, const MmePass&) {
    impl_->DiagOnce("Draw=Geometry is not supported for post effects, skipped");
}
bool MmePostPass::ReadParam(const std::string& name, float out[4]) { return impl_->ReadParam(name, out); }
void MmePostPass::SetLoopIndex(const std::string& name, int index) { impl_->SetLoopIndex(name, index); }
void MmePostPass::Diag(const std::string& message) { impl_->DiagOnce(message); }

} // namespace mmdx
