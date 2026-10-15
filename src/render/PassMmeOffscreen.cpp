// M5a: MME offscreen scene renders + the MMD-style self-shadow map.
//   MmeOffscreenPass: for every OFFSCREENRENDERTARGET of every ready MME instance (post or object), clears it and
//     draws every FrameView::models model matched by the target's parsed DefaultEffect table (first match wins;
//     `hide`, `none` = the built-in standard effect, else the sub-effect) with the object draw path *without* the
//     host MRT (the sub-effects are compiled objectMode = false, so their raw colour goes into the effect's target).
//     Registered right before ScenePass; a no-op (byte-identical frame) when no instance declares one.
//   MmeShadowPass: renders the MMD-style light-space depth map (R32_FLOAT 2048^2) and binds it at register(s0) for
//     object_ss draws, when any ready instance has an object_ss / zplot technique and shadows are on.
// See docs/mme/mme_design.md 2.3 / 2.7 / 2.8.
#include "render/Passes.h"
#include "render/PassCommon.h"
#include "render/MmeRuntime.h"
#include "render/MmeSkin.h"
#include "render/GpuModel.h"
#include "render/ShaderInterop.h"
#include "asset/ImageLoader.h"
#include "asset/MmeText.h"
#include "core/Log.h"
#include "core/TextUtil.h"

#include <directx/d3dx12.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

using namespace DirectX;

namespace mmdx {

// The built-in clean-room standard effect (2.8) lives next to the runtime at <shaderDir>/mme/standard.fx. Both
// passes get the shader directory from CreatePipelines; this is the one place that names the file.
static std::filesystem::path StandardEffectPath(const std::filesystem::path& shaderDir) {
    return shaderDir / L"mme" / L"standard.fx";
}

namespace {

std::string LowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Case-insensitive glob with `*` and `?` (iterative, backtracking on star).
bool WildcardMatch(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        const char pc = p < pattern.size() ? (char)std::tolower((unsigned char)pattern[p]) : '\0';
        const char tc = (char)std::tolower((unsigned char)text[t]);
        if (p < pattern.size() && (pc == tc || pc == '?')) { ++p; ++t; }
        else if (p < pattern.size() && pc == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

// The DefaultEffect result for one model against a target's table. Returns "hide" | "none" | "main_default" or a
// sub-effect file (case preserved). No matching line = MMD's default: the standard effect ("none"). `isSelf` marks
// the owner's own model (an object-effect owner); a post effect has no model, so `self` never matches.
std::string MatchDefaultEffect(const MmeTarget& tgt, const std::string& fileName, const std::string& displayName,
                               bool ownerIsModel, bool isSelf) {
    for (const MmeTarget::DefaultLine& line : tgt.defaultEffect) {
        const std::string pat = LowerAscii(line.pattern);
        if (pat == "self") {
            if (ownerIsModel && isSelf) return line.effect;
            continue;
        }
        if (WildcardMatch(line.pattern, fileName) || WildcardMatch(line.pattern, displayName)) return line.effect;
    }
    return "none";
}

// One (instance, program, stage) $Globals writer cache, shared across a pass's offscreen / shadow draws.
struct WriterCache {
    std::map<uint64_t, std::unique_ptr<MmeConstantWriter>> map;
    MmeConstantWriter* Get(MmeEffectInstance* inst, int program, int stage, const MmeEffectDesc& fx,
                           const MmeStage& st) {
        const uint64_t key = ((uint64_t)(uintptr_t)inst << 32) ^ ((uint64_t)program << 1) ^ (uint64_t)stage;
        auto it = map.find(key);
        if (it != map.end()) return it->second.get();
        auto w = std::make_unique<MmeConstantWriter>();
        std::vector<MmeDiag> diags;
        w->Bind(fx, st, diags);
        MmeConstantWriter* raw = w.get();
        map.emplace(key, std::move(w));
        return raw;
    }
    void Clear() { map.clear(); }
};

MmeObjectValues ObjectValuesFor(const GpuModel& model, size_t mi) {
    const GpuModel::Material& m = model.Materials()[mi];
    const MaterialConstants& mc = model.MaterialConstantsCpu()[mi];
    MmeObjectValues ov;
    ov.diffuse = mc.diffuse;
    ov.ambient = {mc.diffuse.x, mc.diffuse.y, mc.diffuse.z, 1.0f};
    ov.specular = {mc.specular.x, mc.specular.y, mc.specular.z, 1.0f};
    ov.specularPower = mc.specularPower;
    ov.emissive = {mc.ambient.x, mc.ambient.y, mc.ambient.z, 1.0f};
    const XMFLOAT3& toon = model.MmeToonColor(mi);
    ov.toonColor = {toon.x, toon.y, toon.z, 1.0f};
    ov.edgeColor = mc.edgeColor;
    ov.texMul = mc.texMul;
    ov.texAdd = mc.texAdd;
    ov.sphereMul = mc.sphereMul;
    ov.sphereAdd = mc.sphereAdd;
    ov.useTexture = (mc.flags & MatFlag_HasTexture) != 0;
    ov.useSpheremap = m.sphereMode != 0;
    ov.useSubtexture = m.sphereMode == 3;
    ov.useToon = true;
    ov.spadd = m.sphereMode == 2;
    // M5a fix 2: `parthf` is MMD's perspective-shadow mode flag, not "self-shadow on"; MMD's default self-shadow
    // mode (mode 1) has parthf = false.
    ov.parthf = false;
    return ov;
}

// Draws one model's materials with an MME object effect into the currently bound RTs, no host MRT. `wantedPass`
// selects the technique (object_ss falls back to object). Multi-pass techniques draw their first pass only (the
// offscreen sub-effects are single-pass in practice). Returns the number of materials drawn.
uint32_t DrawMmeObjectMaterials(PassContext& pc, MmeEffectInstance* inst, GpuModel& model,
                                const std::string& wantedPass, const MmeFrameValues& frame,
                                const DXGI_FORMAT* rtFormats, uint32_t rtCount, DXGI_FORMAT dsFormat,
                                uint32_t sampleCount, WriterCache& writers) {
    if (!inst || !inst->Ready() || inst->Failed()) return 0;
    if (!pc.mme || !pc.mmeSkin) return 0;
    const MmeEffectDesc& fx = inst->Desc();
    if (fx.techniques.empty()) return 0;

    pc.mmeSkin->Skin(pc, model);
    D3D12_VERTEX_BUFFER_VIEW vb0 = pc.mmeSkin->Vb(model);
    if (!vb0.BufferLocation) return 0;
    const D3D12_VERTEX_BUFFER_VIEW vbs[3] = {vb0, model.AddUvBufferView(), MmeZeroWhiteVb(pc.ctx)};
    if (!vbs[2].BufferLocation) return 0;

    // fix 5: object draws share the same TEXTUREVALUE provider as post draws (the instance owns the CPU texels).
    MmeFrameValues frameV = frame;
    frameV.textureValue = [inst](const std::string& name, uint32_t& outCount) -> const float* {
        return inst->TexValueFor(name, outCount);
    };
    const MmeFrameValues& fv = frameV;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    uint32_t drawn = 0;
    for (size_t mi = 0; mi < model.Materials().size(); ++mi) {
        if (inst->Failed()) break;
        const GpuModel::Material& m = model.Materials()[mi];
        if (m.indexCount == 0 || !m.visible) continue;

        const MmeTechnique* t = SelectMmeTechnique(fx, wantedPass, model, mi, /*exactPass=*/wantedPass == "object_ss");
        if (!t || t->passes.empty()) continue;
        const MmePass& pass = t->passes[0];
        const int program = inst->FindProgram(t->name, pass.name);
        if (program < 0) continue;
        const MmeProgram& prog = inst->Programs()[program];
        if (!prog.ok || !prog.ps.code) continue;

        const MmeObjectValues ov = ObjectValuesFor(model, mi);
        std::vector<D3D12_INPUT_ELEMENT_DESC> layout = MmeObjectInputLayout(prog.vs);
        ID3D12PipelineState* pso = inst->Pso(pc.ctx, program, pass, rtFormats, rtCount, dsFormat, sampleCount,
                                             &layout);
        if (!pso) continue;
        cmd->SetPipelineState(pso);
        bool ok = true;
        for (int pixel = 0; pixel < 2 && ok; ++pixel) {
            const MmeStage& st = pixel ? prog.ps : prog.vs;
            MmeConstantWriter* writer = writers.Get(inst, program, pixel, fx, st);
            writer->Write(fv);
            writer->WriteObject(ov);
            D3D12_GPU_VIRTUAL_ADDRESS cbAddr = pc.sceneConstants;
            if (!writer->Bytes().empty()) {
                cbAddr = pc.mme->AllocConstants(writer->Bytes().data(), (uint32_t)writer->Bytes().size());
                if (cbAddr == 0) ok = false;
            }
            if (!ok) break;
            const int base = pixel ? 3 : 0;
            cmd->SetGraphicsRootConstantBufferView(base, cbAddr);
            const D3D12_GPU_DESCRIPTOR_HANDLE srv =
                inst->StageSrvTableObject(pc, program, pixel != 0, m.srvTable, &model);
            if (srv.ptr) cmd->SetGraphicsRootDescriptorTable(base + 1, srv);
            const D3D12_GPU_DESCRIPTOR_HANDLE smp = inst->StageSamplerTable(program, pixel != 0);
            if (smp.ptr) cmd->SetGraphicsRootDescriptorTable(base + 2, smp);
        }
        if (!ok) continue;
        cmd->IASetVertexBuffers(0, 3, vbs);
        cmd->IASetIndexBuffer(&model.IndexBufferView());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->DrawIndexedInstanced(m.indexCount, 1, m.indexStart, 0, 0);
        pc.stats.drawCalls++;
        pc.stats.triangles += m.indexCount / 3;
        ++drawn;
    }
    return drawn;
}

// The MME frame values for an offscreen / shadow draw: the unjittered camera at (w,h) aspect (or the light camera,
// which the binder derives from lightDir). `self` / `owner` are the controller panels.
MmeFrameValues OffscreenFrameValues(PassContext& pc, uint32_t w, uint32_t h, const MmeControllerPanel& self,
                                    const MmeControllerPanel& owner) {
    MmeFrameValues frame{};
    const CameraParams& cam = pc.view.camera;
    const XMMATRIX viewM = XMLoadFloat4x4(&cam.view);
    const XMMATRIX projM = XMMatrixPerspectiveFovLH(cam.fovYRadians, (float)std::max(1u, w) / (float)std::max(1u, h),
                                                    cam.nearZ, cam.farZ);
    XMStoreFloat4x4(&frame.view, viewM);
    XMStoreFloat4x4(&frame.proj, projM);
    frame.cameraPos = cam.eye;
    const XMMATRIX invView = XMMatrixInverse(nullptr, viewM);
    XMStoreFloat3(&frame.cameraDir,
                  XMVector3Normalize(XMVector3TransformNormal(XMVectorSet(0, 0, 1, 0), invView)));
    XMStoreFloat3(&frame.lightDir, XMVector3Normalize(XMLoadFloat3(&pc.view.light.direction)));
    frame.lightColor = pc.view.light.color;
    // M5a fix 2: one MMD self-shadow light camera per frame, shared by the zplot draws and every object_ss draw.
    std::vector<const GpuModel*> chars;
    chars.reserve(pc.view.models.size());
    for (const GpuModel* m : pc.view.models) chars.push_back(m);
    BuildLightCamera(frame.lightDir, chars, pc.view.mmeObjects, frame.lightView, frame.lightProj);
    frame.viewportW = (float)w;
    frame.viewportH = (float)h;
    const float t = (float)pc.frame / 60.0f;
    frame.time = t;
    frame.elapsed = pc.frameTimeMs * 0.001f;
    // M8a: the scene clock (see FrameView); MME TIME is the frame time while playing / rendering.
    frame.sceneTime = pc.view.sceneTime;
    frame.sceneElapsed = pc.view.sceneElapsed;
    if (!pc.view.mmeEditMode) {
        frame.time = frame.sceneTime;
        frame.elapsed = frame.sceneElapsed;
    }
    frame.self = self;
    frame.offscreenOwner = owner;
    return frame;
}

// The controller panel of the effect entry that owns `inst` (post effects + effect objects); the default when none
// matches.
MmeControllerPanel OwnerPanel(const RenderSettings& s, const FrameView& view, const std::filesystem::path& file) {
    MmeControllerPanel panel;
    for (const MmeEffectEntry& e : s.mmeEffects) {
        if (mmetext::PathKey(e.file) != mmetext::PathKey(file)) continue;
        panel.x = e.x; panel.y = e.y; panel.z = e.z;
        panel.rx = XMConvertToRadians(e.rx); panel.ry = XMConvertToRadians(e.ry); panel.rz = XMConvertToRadians(e.rz);
        panel.si = e.si; panel.tr = e.tr; panel.visible = e.visible;
        return panel;
    }
    // M5c: effect objects carry their panel directly.
    for (const MmeEffectObject& o : view.mmeEffectObjects) {
        if (mmetext::PathKey(o.fx) != mmetext::PathKey(file)) continue;
        return o.panel;
    }
    return panel;
}

} // namespace

// ---- shared CONTROLOBJECT lookup (also used by ScenePass's M4b draws) ----------------------------------------------

std::function<bool(const std::string&, const std::string&, int, float*, int&)> MmeSceneLookup(const FrameView& view) {
    struct EffObj {
        std::string fileName, displayName;
        MmeControllerPanel panel;
        XMFLOAT4X4 world;
    };
    std::vector<const MmeSceneObject*> objs;
    objs.reserve(view.mmeObjects.size());
    for (const MmeSceneObject& o : view.mmeObjects) objs.push_back(&o);
    // M5c: effect objects (accessory `.x` + `.fx`) resolve by their file name / display name too.
    std::vector<EffObj> effects;
    effects.reserve(view.mmeEffectObjects.size());
    for (const MmeEffectObject& e : view.mmeEffectObjects) {
        EffObj eo;
        eo.fileName = e.fileName;
        eo.displayName = e.displayName;
        eo.panel = e.panel;
        XMStoreFloat4x4(&eo.world, MmeAccessoryWorld(e.panel));
        effects.push_back(std::move(eo));
    }
    return [objs, effects](const std::string& name, const std::string& item, int kind, float* out,
                           int& outCount) -> bool {
        const std::string lo = LowerAscii(name);
        for (const EffObj& e : effects) {
            if (LowerAscii(e.fileName) != lo && LowerAscii(e.displayName) != lo) continue;
            outCount = 0;
            const std::string it = LowerAscii(item);
            auto setF = [&](float v) { out[0] = v; outCount = 1; };
            auto setV3 = [&](float x, float y, float z) { out[0] = x; out[1] = y; out[2] = z; out[3] = 1.0f; outCount = 4; };
            auto setM = [&](const XMFLOAT4X4& m) { std::memcpy(out, &m.m[0][0], 16 * sizeof(float)); outCount = 16; };
            const MmeControllerPanel& p = e.panel;
            if (it.empty()) {
                if (kind == 0) { setF(p.visible ? 1.0f : 0.0f); return true; }
                if (kind == 3) { setM(e.world); return true; }
                if (kind == 2) { setV3(p.x, p.y, p.z); return true; }
                setF(p.si * 10.0f);   // the accessory scale (×10), matching MMD's default of 10
                return true;
            }
            if (it == "x" || it == "y" || it == "z") { setF(it == "x" ? p.x : (it == "y" ? p.y : p.z)); return true; }
            if (it == "xyz") { setV3(p.x, p.y, p.z); return true; }
            if (it == "rx" || it == "ry" || it == "rz") { setF(it == "rx" ? p.rx : (it == "ry" ? p.ry : p.rz)); return true; }
            if (it == "rxyz") { out[0] = p.rx; out[1] = p.ry; out[2] = p.rz; out[3] = 0.0f; outCount = 4; return true; }
            if (it == "si") { setF(p.si * 10.0f); return true; }
            if (it == "tr") { setF(p.tr); return true; }
            return false;
        }
        const MmeSceneObject* obj = nullptr;
        for (const MmeSceneObject* o : objs)
            if (LowerAscii(o->fileName) == lo) { obj = o; break; }
        if (!obj)
            for (const MmeSceneObject* o : objs)
                if (LowerAscii(o->displayName) == lo) { obj = o; break; }
        if (!obj) return false;   // missing -> spec defaults (the binder keeps its default on false)
        outCount = 0;
        const std::string it = LowerAscii(item);
        auto setF = [&](float v) { out[0] = v; outCount = 1; };
        auto setV3 = [&](float x, float y, float z) { out[0] = x; out[1] = y; out[2] = z; out[3] = 1.0f; outCount = 4; };
        auto setM = [&](const XMFLOAT4X4& m) {
            std::memcpy(out, &m.m[0][0], 16 * sizeof(float));
            outCount = 16;
        };
        // Bone name match (any kind): position or matrix.
        const XMFLOAT4X4* boneM = nullptr;
        for (size_t b = 0; b < obj->boneNames.size(); ++b)
            if (LowerAscii(obj->boneNames[b]) == it) { boneM = &obj->boneWorld[b]; break; }
        if (it.empty()) {
            if (kind == 0) { setF(obj->visible ? 1.0f : 0.0f); return true; }
            if (kind == 3) { setM(obj->rootWorld); return true; }
            if (kind == 2) { setV3(obj->rootWorld.m[3][0], obj->rootWorld.m[3][1], obj->rootWorld.m[3][2]); return true; }
            setF(1.0f);   // kind 1: model scale is 1 (no x10 rule)
            return true;
        }
        if (boneM) {
            if (kind == 3) { setM(*boneM); return true; }
            if (kind == 2) { setV3(boneM->m[3][0], boneM->m[3][1], boneM->m[3][2]); return true; }
            setF(boneM->m[3][0]);   // kind 1: bone world x (rare)
            return true;
        }
        for (size_t mi = 0; mi < obj->morphNames.size(); ++mi)
            if (LowerAscii(obj->morphNames[mi]) == it) { setF(obj->morphWeights[mi]); return true; }
        // Accessory items (X/Y/Z/XYZ/Rx/.../Si/Tr) and unknown items on a model -> spec defaults (the binder keeps
        // its default value when this returns false).
        return false;
    };
}

// ===================================================================================================================
// MmeOffscreenPass
// ===================================================================================================================

struct MmeOffscreenPass::Impl {
    std::filesystem::path shaderDir;
    WriterCache writers;
    std::set<std::string> diagOnce;   // once-per-message diagnostics
};

MmeOffscreenPass::MmeOffscreenPass() : impl_(std::make_unique<Impl>()) {}
MmeOffscreenPass::~MmeOffscreenPass() = default;

bool MmeOffscreenPass::CreatePipelines(Dx12Context&, const std::filesystem::path& shaderDir, uint32_t) {
    impl_->shaderDir = shaderDir;
    return true;
}

void MmeOffscreenPass::Execute(PassContext& pc) {
    if (!pc.mme || !pc.mmeSkin) return;
    if (pc.settings.shading != ViewShading::Lit || !pc.view.extraViews.empty()) return;
    if (pc.view.models.empty()) return;

    // Collect every offscreen target of every ready instance (post or object), in instance/declaration order.
    struct Job { MmeEffectInstance* owner; MmeTarget* target; int selfModel; };   // selfModel: index in view.models, -1 = no model owner
    std::vector<Job> jobs;
    for (const auto& instPtr : pc.mme->Instances()) {
        MmeEffectInstance* inst = instPtr.get();
        if (!inst->Ready() || inst->Failed()) continue;
        // A scene / post effect's object-mode instance (an effect object's `.x` mesh carrying a post effect such as
        // AutoLuminous.x) duplicates the plain instance's targets; the post chain reads the plain instance's, so only
        // that one renders them.
        if (inst->ObjectMode() && inst->Desc().kind() != "object") continue;
        std::vector<MmeTarget*> offs = inst->OffscreenTargets();
        if (offs.empty()) continue;
        // An object-effect owner is one of the models (`self` matches it); a post effect owns no model.
        int selfModel = -1;
        if (inst->ObjectMode()) {
            for (size_t i = 0; i < pc.view.models.size(); ++i)
                if (pc.view.models[i]) {
                    for (const std::filesystem::path& fx : pc.view.models[i]->MmeEffectFiles())
                        if (mmetext::PathKey(fx) == mmetext::PathKey(inst->File())) {
                            selfModel = (int)i;
                            break;
                        }
                    if (selfModel >= 0) break;
                }
        }
        for (MmeTarget* t : offs) jobs.push_back({inst, t, selfModel});
    }
    if (jobs.empty()) return;

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    // MME draws bind the runtime's sampler heap alongside the SRV heap.
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), pc.mme->SamplerHeap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(pc.mme->RootSignature());

    const bool shadowsOn = !pc.view.shadowsOff && pc.settings.shadows;
    const bool haveShadowMap = shadowsOn && pc.mme->HasObjectShadowMap();

    for (const Job& job : jobs) {
        MmeTarget& tgt = *job.target;
        if (!tgt.tex.res || tgt.tex.rtv == DescriptorHeap::kInvalid) continue;
        const uint32_t w = tgt.tex.width, h = tgt.tex.height;
        if (!w || !h) continue;

        // First-match draw list over the models, in scene order.
        struct DrawItem { GpuModel* model; std::string effect; };
        std::vector<DrawItem> list;
        for (size_t i = 0; i < pc.view.models.size(); ++i) {
            GpuModel* model = pc.view.models[i];
            if (!model) continue;
            const std::string fileName = i < pc.view.mmeObjects.size() ? pc.view.mmeObjects[i].fileName
                                                                        : std::string();
            const std::string display = (i < pc.view.mmeObjects.size() && !pc.view.mmeObjects[i].displayName.empty())
                                            ? pc.view.mmeObjects[i].displayName
                                            : model->Name();
            const std::string eff = MatchDefaultEffect(tgt, fileName, display, /*ownerIsModel=*/false, job.selfModel == (int)i);
            list.push_back({model, eff});
        }
        if (list.empty()) continue;
        LOG_INFO("MME OFFSCREEN %s target=%s %ux%u draws=%zu", PathToUtf8(job.owner->File()).c_str(),
                 tgt.name.c_str(), w, h, list.size());

        // Clear.
        tgt.tex.Transition(cmd, kRt);
        cmd->ClearRenderTargetView(pc.ctx.RtvHeap().Cpu(tgt.tex.rtv), tgt.clear, 0, nullptr);
        const bool hasDepth = tgt.depthTex.res && tgt.depthTex.dsv != DescriptorHeap::kInvalid;
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        DXGI_FORMAT dsFormat = DXGI_FORMAT_UNKNOWN;
        if (hasDepth) {
            tgt.depthTex.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            cmd->ClearDepthStencilView(pc.ctx.DsvHeap().Cpu(tgt.depthTex.dsv), D3D12_CLEAR_FLAG_DEPTH,
                                       tgt.clearDepth, 0, 0, nullptr);
            dsv = pc.ctx.DsvHeap().Cpu(tgt.depthTex.dsv);
            dsFormat = DXGI_FORMAT_D32_FLOAT;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = pc.ctx.RtvHeap().Cpu(tgt.tex.rtv);
        cmd->OMSetRenderTargets(1, &rtv, FALSE, hasDepth ? &dsv : nullptr);
        D3D12_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, (LONG)w, (LONG)h};
        cmd->RSSetViewports(1, &vp);
        cmd->RSSetScissorRects(1, &sc);

        const DXGI_FORMAT rtFormats[1] = {tgt.tex.format};
        const std::filesystem::path ownerDir = job.owner->File().parent_path();
        const MmeControllerPanel ownerPanel = OwnerPanel(pc.settings, pc.view, job.owner->File());

        // Resolve sub-effect paths: "none"/"main_default" and a missing file use the built-in standard effect, as
        // does a resolved sub-effect without an object technique (design 2.8: a missing technique needs standard).
        auto hasObjectTech = [](MmeEffectInstance* sub) {
            for (const MmeTechnique& t : sub->Desc().techniques)
                if (t.mmdPass == "object" || t.mmdPass == "object_ss") return true;
            return false;
        };
        auto resolve = [&](const DrawItem& di) -> MmeEffectInstance* {
            if (di.effect.empty() || di.effect == "none" || di.effect == "main_default")
                return pc.mme->EnsureInstance(StandardEffectPath(impl_->shaderDir), false);
            const std::filesystem::path fx = ownerDir / Utf8ToPath(di.effect);
            if (std::filesystem::exists(fx)) {
                MmeEffectInstance* sub = pc.mme->EnsureInstance(fx, false);
                // still compiling: draw nothing this frame (the standard effect would paint the whole model into
                // e.g. AutoLuminous's emitter target, a white glow until the sub-effect is ready)
                if (sub && !sub->Ready() && !sub->Failed()) return nullptr;
                if (sub && sub->Ready() && hasObjectTech(sub)) return sub;
            }
            return pc.mme->EnsureInstance(StandardEffectPath(impl_->shaderDir), false);
        };
        auto resolveEdge = [&](const DrawItem& di) -> MmeEffectInstance* {
            if (di.effect.empty() || di.effect == "none" || di.effect == "main_default") return nullptr;
            const std::filesystem::path fx = ownerDir / Utf8ToPath(di.effect);
            if (!std::filesystem::exists(fx)) return nullptr;
            return pc.mme->EnsureInstance(fx, false);
        };

        for (const DrawItem& di : list) {
            if (di.effect == "hide") continue;
            MmeEffectInstance* sub = resolve(di);
            if (!sub || !sub->Ready()) continue;
            std::string wanted = "object";
            for (const MmeTechnique& t : sub->Desc().techniques)
                if (t.mmdPass == "object_ss") { wanted = "object_ss"; break; }
            if (wanted == "object_ss" && !haveShadowMap) wanted = "object";
            MmeControllerPanel selfPanel;
            selfPanel.si = 0.1f;   // models: scale 1 (Si = si * 10)
            selfPanel.tr = 1.0f;
            MmeFrameValues frame = OffscreenFrameValues(pc, w, h, selfPanel, ownerPanel);
            frame.lookup = MmeSceneLookup(pc.view);
            DrawMmeObjectMaterials(pc, sub, *di.model, wanted, frame, rtFormats, 1, dsFormat, 1, impl_->writers);
        }
        // Edges: the sub-effect's edge technique, drawn with the same depth.
        for (const DrawItem& di : list) {
            if (di.effect == "hide") continue;
            MmeEffectInstance* sub = resolveEdge(di);
            if (!sub || !sub->Ready()) continue;
            bool hasEdge = false;
            for (const MmeTechnique& t : sub->Desc().techniques)
                if (t.mmdPass == "edge" && !t.passes.empty()) { hasEdge = true; break; }
            if (!hasEdge) continue;
            MmeControllerPanel selfPanel;
            selfPanel.si = 0.1f;
            selfPanel.tr = 1.0f;
            MmeFrameValues frame = OffscreenFrameValues(pc, w, h, selfPanel, ownerPanel);
            frame.lookup = MmeSceneLookup(pc.view);
            DrawMmeObjectMaterials(pc, sub, *di.model, "edge", frame, rtFormats, 1, dsFormat, 1, impl_->writers);
        }

        // Mip chain for an offscreen target (MipLevels > 1 / 0): built after the scene re-render wrote mip 0.
        if (tgt.mipLevels > 1) {
            tgt.tex.Transition(cmd, kSrv);
            if (pc.mme) pc.mme->GenerateMips(pc, tgt.tex);
            tgt.tex.Transition(cmd, kSrv);
        }
        if (hasDepth) tgt.depthTex.Transition(cmd, kSrv);
    }

    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, restore);
}

// ===================================================================================================================
// MmeShadowPass
// ===================================================================================================================

struct MmeShadowPass::Impl {
    std::filesystem::path shaderDir;
    Texture map;                       // R32_FLOAT shadow map
    Texture depth;                     // companion D32_FLOAT depth
    WriterCache writers;
    uint32_t size = 2048;
    // Bumped every time map / depth are (re)created; passed to SetObjectShadowMap so a released / recreated map at
    // the same Texture address still invalidates the cached stage SRV tables (spec fix B).
    uint64_t gen = 0;
    bool loggedSize = false;

    // M5a fix 3 debug switch: MMDX12_MME_DUMP_SHADOW=<png path> reads the R32_FLOAT self-shadow map back once and
    // writes it as a grayscale PNG (min..max of the non-clear texels normalised to 0..255; the clear value stays
    // white). The png path is read from the environment once per process. Kept as a cheap, useful diagnostic.
    // Frame timing: a copy recorded in frame N's still-open command list can only be read back once that list has
    // been submitted and the GPU has finished it. `--frames 60` renders exactly 60 frames, so the copy is recorded
    // in frame 59's shadow pass and read back (after WaitForGpu) at the end of frame 60's shadow pass. The pinned
    // repro is `--seek 20 --paused --no-physics`, so both frames hold the identical pose and the dumped map is the
    // frame-60 map's content.
    static constexpr int kDumpFrame = 60;
    std::filesystem::path dumpPath;    // empty = off
    bool dumpPathRead = false;
    int frameCounter = 0;              // frames the map was actually drawn this run
    bool dumpCopyRecorded = false;     // the frame-(kDumpFrame-1) copy is in flight
    bool dumpDone = false;             // PNG written; the switch is one-shot
    ComPtr<ID3D12Resource> dumpReadback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT dumpFootprint{};
};

MmeShadowPass::MmeShadowPass() : impl_(std::make_unique<Impl>()) {}
MmeShadowPass::~MmeShadowPass() = default;

bool MmeShadowPass::CreatePipelines(Dx12Context&, const std::filesystem::path& shaderDir, uint32_t) {
    impl_->shaderDir = shaderDir;
    return true;
}

void MmeShadowPass::OnResize(Dx12Context& ctx, RenderTargets&) {
    // M5a fix B: the map / depth size (im.size) does not depend on the screen, so a resize must NOT release them.
    // Releasing here recreated them at the same Texture address and SetObjectShadowMap saw no change (cached SRV
    // tables kept the freed resource -> GPU page fault). ReleaseTargets still drops them on device / target teardown;
    // the Impl::gen counter makes any recreation safe.
    (void)ctx;
}
void MmeShadowPass::ReleaseTargets(Dx12Context& ctx) {
    impl_->map.Release(ctx);
    impl_->depth.Release(ctx);
}

void MmeShadowPass::Execute(PassContext& pc) {
    Impl& im = *impl_;
    // M5a fix 3: see Impl's comment. The png path is read from the environment once per process. At the START of the
    // shadow pass the previous frame's command list has been submitted and waited on, so a copy recorded then is
    // readable now; the PNG is written here (the map content of that frame).
    if (!im.dumpPathRead) {
        im.dumpPathRead = true;
        if (const char* p = std::getenv("MMDX12_MME_DUMP_SHADOW"); p && *p) im.dumpPath = Utf8ToPath(p);
    }
    if (!im.dumpPath.empty() && im.dumpCopyRecorded && !im.dumpDone) {
        im.dumpDone = true;
        pc.ctx.WaitForGpu();   // the copy recorded in the previous frame's command list has completed
        uint8_t* data = nullptr;
        if (SUCCEEDED(im.dumpReadback->Map(0, nullptr, (void**)&data))) {
            const D3D12_SUBRESOURCE_FOOTPRINT& fp = im.dumpFootprint.Footprint;
            const uint32_t w = fp.Width, h = fp.Height;
            std::vector<float> vals((size_t)w * h);
            float mn = FLT_MAX, mx = -FLT_MAX;
            for (uint32_t y = 0; y < h; ++y) {
                const float* row = (const float*)(data + im.dumpFootprint.Offset + (size_t)y * fp.RowPitch);
                for (uint32_t x = 0; x < w; ++x) {
                    const float v = row[x];
                    vals[(size_t)y * w + x] = v;
                    if (v < 0.9999f) { mn = std::min(mn, v); mx = std::max(mx, v); }   // skip the clear texels
                }
            }
            im.dumpReadback->Unmap(0, nullptr);
            if (mn > mx) { mn = 0.0f; mx = 1.0f; }
            ImageRGBA8 img;
            img.mips.push_back({w, h, std::vector<uint8_t>((size_t)w * h * 4)});
            uint8_t* out = img.mips[0].pixels.data();
            const float span = (mx > mn) ? (mx - mn) : 1.0f;
            for (size_t i = 0; i < vals.size(); ++i) {
                uint8_t g = 255;   // clear / background stays white
                if (vals[i] < 0.9999f)
                    g = (uint8_t)std::clamp((vals[i] - mn) / span * 255.0f, 0.0f, 255.0f);
                out[i * 4 + 0] = out[i * 4 + 1] = out[i * 4 + 2] = g;
                out[i * 4 + 3] = 255;
            }
            if (SavePngRGBA8(im.dumpPath, w, h, out, w * 4))
                LOG_INFO("MME shadow map dumped to %s (min=%.5f max=%.5f)", PathToUtf8(im.dumpPath).c_str(), mn, mx);
            else
                LOG_ERROR("MME shadow map dump failed: %s", PathToUtf8(im.dumpPath).c_str());
        }
        im.dumpReadback.Reset();
    }

    const bool shadowsOn = !pc.view.shadowsOff && pc.settings.shadows;
    if (!pc.mme || !pc.mmeSkin || !shadowsOn) {
        if (pc.mme) pc.mme->SetObjectShadowMap(nullptr, 0);
        return;
    }
    // The map is needed only when some loaded effect actually uses an object_ss / zplot technique: an MME model's
    // effect, or an enabled post effect's effect. A plain native scene never builds it (byte-identical frame).
    bool wanted = false;
    for (GpuModel* m : pc.view.models) {
        if (!m) continue;
        for (const std::filesystem::path& fx : m->MmeEffectFiles()) {
            MmeEffectInstance* inst = pc.mme->Instance(fx, true);
            if (!inst || !inst->Ready()) { wanted = true; break; }
            for (const MmeTechnique& t : inst->Desc().techniques)
                if (t.mmdPass == "object_ss" || t.mmdPass == "zplot") { wanted = true; break; }
            if (wanted) break;
        }
        if (wanted) break;
    }
    if (!wanted) {
        for (const MmeEffectEntry& e : pc.settings.mmeEffects) {
            if (!e.enabled || e.file.empty()) continue;
            MmeEffectInstance* inst = pc.mme->Instance(e.file);
            if (!inst || !inst->Ready()) continue;
            for (const MmeTechnique& t : inst->Desc().techniques)
                if (t.mmdPass == "object_ss" || t.mmdPass == "zplot") { wanted = true; break; }
            if (wanted) break;
        }
    }
    if (!wanted) {
        pc.mme->SetObjectShadowMap(nullptr, 0);
        return;
    }

    if (!im.map.res) {
        if (!im.map.Create(pc.ctx, im.size, im.size, DXGI_FORMAT_R32_FLOAT,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                           L"mme.shadowmap", 1, 1, nullptr))
            return;
        if (!im.depth.Create(pc.ctx, im.size, im.size, RenderTargets::kDepthFormat,
                             D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                             L"mme.shadowmap.depth"))
            return;
        ++im.gen;   // the (address, gen) pair changed -> cached object_ss SRV tables rebuild
        if (!im.loggedSize) {
            im.loggedSize = true;
            LOG_INFO("MME SHADOWMAP %ux%u (MMD self-shadow light camera)", im.size, im.size);
        }
    }

    // Bind the map to every instance before drawing (object_ss SRV tables reference it).
    pc.mme->SetObjectShadowMap(&im.map, im.gen);

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), pc.mme->SamplerHeap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(pc.mme->RootSignature());

    im.map.Transition(cmd, kRt);
    const float clear[4] = {1, 0, 0, 1};
    cmd->ClearRenderTargetView(pc.ctx.RtvHeap().Cpu(im.map.rtv), clear, 0, nullptr);
    im.depth.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = pc.ctx.DsvHeap().Cpu(im.depth.dsv);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = pc.ctx.RtvHeap().Cpu(im.map.rtv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp{0.0f, 0.0f, (float)im.size, (float)im.size, 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, (LONG)im.size, (LONG)im.size};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    const DXGI_FORMAT rtFormats[1] = {DXGI_FORMAT_R32_FLOAT};
    MmeControllerPanel selfPanel;   // models: scale 1, position 0
    selfPanel.si = 0.1f;
    selfPanel.tr = 1.0f;
    for (GpuModel* m : pc.view.models) {
        if (!m || m->Materials().empty()) continue;
        MmeEffectInstance* sub = nullptr;
        for (const std::filesystem::path& fx : m->MmeEffectFiles()) {
            // zplot writes raw z/w into R32F: the plain wrappers, never the object-mode ones (their host MRT
            // linearises and sun-scales SV_Target0, which corrupts the stored depth).
            MmeEffectInstance* inst = pc.mme->EnsureInstance(fx, false);
            if (inst && inst->Ready())
                for (const MmeTechnique& t : inst->Desc().techniques)
                    if (t.mmdPass == "zplot" && !t.passes.empty()) { sub = inst; break; }
            if (sub) break;
        }
        if (!sub) sub = pc.mme->EnsureInstance(StandardEffectPath(impl_->shaderDir), false);
        if (!sub || !sub->Ready()) continue;
        MmeFrameValues frame = OffscreenFrameValues(pc, im.size, im.size, selfPanel, selfPanel);
        frame.lookup = MmeSceneLookup(pc.view);
        DrawMmeObjectMaterials(pc, sub, *m, "zplot", frame, rtFormats, 1, DXGI_FORMAT_D32_FLOAT, 1, im.writers);
    }
    im.map.Transition(cmd, kSrv);
    im.depth.Transition(cmd, kSrv);

    // M5a fix 3: record the readback copy one frame before the dump frame (see the Execute prologue and Impl's comment).
    if (!im.dumpPath.empty() && !im.dumpCopyRecorded && !im.dumpDone) {
        if (++im.frameCounter == Impl::kDumpFrame - 1) {
            D3D12_RESOURCE_DESC desc = im.map.res->GetDesc();
            UINT64 total = 0;
            pc.ctx.Device()->GetCopyableFootprints(&desc, 0, 1, 0, &im.dumpFootprint, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES rb{D3D12_HEAP_TYPE_READBACK};
            CD3DX12_RESOURCE_DESC bd = CD3DX12_RESOURCE_DESC::Buffer(total);
            if (CheckHr(pc.ctx.Device()->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&im.dumpReadback)),
                        "MmeShadowPass: dump readback")) {
                im.map.Transition(cmd, D3D12_RESOURCE_STATE_COPY_SOURCE);
                CD3DX12_TEXTURE_COPY_LOCATION dst(im.dumpReadback.Get(), im.dumpFootprint);
                CD3DX12_TEXTURE_COPY_LOCATION src(im.map.res.Get(), 0);
                cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                im.map.Transition(cmd, kSrv);
                im.dumpCopyRecorded = true;   // read back at the start of the next frame's shadow pass
            }
        }
    }

    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, restore);
}

// M5b fix 2: the offline self-shadow map (see Passes.h). MmeShadowPass::Execute is a real-time pass and is not part
// of the offline renderer's pass list, so the offline MME layer renders the map itself. Same technique selection /
// light camera / binding as MmeShadowPass::Execute, at the offline image size (the caller owns `map` / `depth`).
bool RenderMmeSelfShadowMap(PassContext& pc, Texture& map, Texture& depth, uint32_t size,
                            const std::filesystem::path& shaderDir) {
    if (!pc.mme || !pc.mmeSkin) return false;
    const bool shadowsOn = !pc.view.shadowsOff && pc.settings.shadows;
    if (!shadowsOn) {
        pc.mme->SetObjectShadowMap(nullptr, 0);
        return false;
    }
    // Wanted only when some loaded effect actually uses an object_ss / zplot technique (as MmeShadowPass::Execute).
    bool wanted = false;
    for (GpuModel* m : pc.view.models) {
        if (!m) continue;
        for (const std::filesystem::path& fx : m->MmeEffectFiles()) {
            MmeEffectInstance* inst = pc.mme->Instance(fx, true);
            if (!inst || !inst->Ready()) { wanted = true; break; }
            for (const MmeTechnique& t : inst->Desc().techniques)
                if (t.mmdPass == "object_ss" || t.mmdPass == "zplot") { wanted = true; break; }
            if (wanted) break;
        }
        if (wanted) break;
    }
    if (!wanted) {
        for (const MmeEffectEntry& e : pc.settings.mmeEffects) {
            if (!e.enabled || e.file.empty()) continue;
            MmeEffectInstance* inst = pc.mme->Instance(e.file);
            if (!inst || !inst->Ready()) continue;
            for (const MmeTechnique& t : inst->Desc().techniques)
                if (t.mmdPass == "object_ss" || t.mmdPass == "zplot") { wanted = true; break; }
            if (wanted) break;
        }
    }
    if (!wanted) {
        pc.mme->SetObjectShadowMap(nullptr, 0);
        return false;
    }

    // Generation of the caller-owned offline map resource: bumped whenever it is (re)created, so cached object_ss
    // SRV tables drop a freed one at the same Texture address (spec fix B).
    static uint64_t sOfflineShadowGen = 0;
    if (!map.res) {
        if (!map.Create(pc.ctx, size, size, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, L"offline.mme.shadowmap", 1, 1, nullptr))
            return false;
        if (!depth.Create(pc.ctx, size, size, RenderTargets::kDepthFormat,
                          D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                          L"offline.mme.shadowmap.depth"))
            return false;
        ++sOfflineShadowGen;   // the map resource was (re)created: cached object_ss SRV tables must rebuild
    }

    // Bind the map to every instance before drawing (object_ss SRV tables reference it). The generation changes
    // whenever the caller-owned map resource is (re)created, so cached SRV tables drop the freed one.
    pc.mme->SetObjectShadowMap(&map, sOfflineShadowGen);

    ID3D12GraphicsCommandList* cmd = pc.cmd;
    ID3D12DescriptorHeap* heaps[2] = {pc.ctx.SrvHeap().Heap(), pc.mme->SamplerHeap()};
    cmd->SetDescriptorHeaps(2, heaps);
    cmd->SetGraphicsRootSignature(pc.mme->RootSignature());

    map.Transition(cmd, kRt);
    const float clear[4] = {1, 0, 0, 1};
    cmd->ClearRenderTargetView(pc.ctx.RtvHeap().Cpu(map.rtv), clear, 0, nullptr);
    depth.Transition(cmd, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = pc.ctx.DsvHeap().Cpu(depth.dsv);
    cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = pc.ctx.RtvHeap().Cpu(map.rtv);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    D3D12_VIEWPORT vp{0.0f, 0.0f, (float)size, (float)size, 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, (LONG)size, (LONG)size};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);

    const DXGI_FORMAT rtFormats[1] = {DXGI_FORMAT_R32_FLOAT};
    MmeControllerPanel selfPanel;   // models: scale 1, position 0
    selfPanel.si = 0.1f;
    selfPanel.tr = 1.0f;
    WriterCache writers;
    for (GpuModel* m : pc.view.models) {
        if (!m || m->Materials().empty()) continue;
        MmeEffectInstance* sub = nullptr;
        if (!m->MmeEffectFiles().empty()) {
            // zplot writes raw z/w into R32F: the plain wrappers, never the object-mode ones (their host MRT
            // linearises and sun-scales SV_Target0, which corrupts the stored depth).
            for (const std::filesystem::path& fx : m->MmeEffectFiles()) {
                MmeEffectInstance* inst = pc.mme->EnsureInstance(fx, false);
                if (inst && inst->Ready())
                    for (const MmeTechnique& t : inst->Desc().techniques)
                        if (t.mmdPass == "zplot" && !t.passes.empty()) { sub = inst; break; }
                if (sub) break;
            }
        }
        if (!sub) sub = pc.mme->EnsureInstance(StandardEffectPath(shaderDir), false);
        if (!sub || !sub->Ready()) continue;
        MmeFrameValues frame = OffscreenFrameValues(pc, size, size, selfPanel, selfPanel);
        frame.lookup = MmeSceneLookup(pc.view);
        DrawMmeObjectMaterials(pc, sub, *m, "zplot", frame, rtFormats, 1, DXGI_FORMAT_D32_FLOAT, 1, writers);
    }
    map.Transition(cmd, kSrv);
    depth.Transition(cmd, kSrv);

    ID3D12DescriptorHeap* restore[1] = {pc.ctx.SrvHeap().Heap()};
    cmd->SetDescriptorHeaps(1, restore);
    return true;
}

} // namespace mmdx
