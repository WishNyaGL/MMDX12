// M6b: MME effect diagnostics (see MmeDiagnostics.h). CPU parse + compile only; no D3D objects.
#include "render/MmeDiagnostics.h"

#include "asset/ImageLoader.h"
#include "asset/MmeText.h"
#include "core/TextUtil.h"
#include "render/MmeCompiler.h"

#include <chrono>

namespace mmdx {

MmeEffectDiag BuildMmeEffectDiag(const std::filesystem::path& fx) {
    MmeEffectDiag out;
    const MmeEffectDesc desc = LoadMmeEffect(fx, MmeLoadOptions{});
    out.kind = desc.kind();
    out.ok = desc.ok;

    // Parser diagnostics (errors first).
    for (const MmeDiag& d : desc.diags) {
        if (d.sev == MmeDiag::Error) {
            std::string line = d.message;
            const size_t nl = line.find('\n');
            if (nl != std::string::npos) line = line.substr(0, nl);
            out.unsupported.push_back(line);
            ++out.errorCount;
        } else if (d.sev == MmeDiag::Warning) {
            ++out.warningCount;
        }
    }

    // Missing textures: a texture param with a ResourceName that does not resolve / decode (white is used).
    for (const MmeParam& p : desc.params) {
        if (mmetext::Lower(p.type).rfind("texture", 0) != 0) continue;
        const MmeAnnotation* ann = FindAnnotation(p.annotations, "ResourceName");
        if (!ann) continue;
        const std::string rel = mmetext::Trim(ann->value.str);
        std::filesystem::path resolved;
        const std::filesystem::path base = p.file.empty() ? fx.parent_path() : p.file.parent_path();
        if (rel.empty() || !mmetext::ResolveRel(base, rel, resolved)) {
            out.missingTextures.push_back(p.name + " (" + rel + ")");
            continue;
        }
        ImageRGBA8 img;
        std::string err;
        if (!LoadImageRGBA8(resolved, img, &err) || img.Empty())
            out.missingTextures.push_back(p.name + " (" + PathToUtf8(resolved.filename()) + ")");
    }

    if (!desc.ok) return out;

    // Compile every pass (post mode; object passes with the object wrapper when the effect is an object effect —
    // diagnostics merely report compile success, so the object-mode wrappers are used for object kind).
    MmeCompileOptions co;
    co.objectMode = out.kind == "object";
    const std::vector<MmeProgram> programs = CompileMmeEffect(desc, co);
    bool anyPass = false;
    for (const MmeProgram& p : programs) {
        MmeEffectDiag::Pass pp;
        pp.technique = p.technique;
        pp.pass = p.pass;
        pp.ok = p.ok;
        if (p.ok) {
            anyPass = true;
        } else {
            for (const MmeDiag& d : p.diags) {
                if (d.sev != MmeDiag::Error) continue;
                pp.error = d.message;
                break;
            }
            if (pp.error.empty()) pp.error = "compile failed";
            const size_t nl = pp.error.find('\n');
            if (nl != std::string::npos) pp.error = pp.error.substr(0, nl);
            out.unsupported.push_back(pp.technique + " / " + pp.pass + ": " + pp.error);
            ++out.errorCount;
        }
        out.passes.push_back(std::move(pp));
    }
    for (const MmeProgram& p : programs)
        for (const MmeDiag& d : p.diags)
            if (d.sev == MmeDiag::Warning) ++out.warningCount;
    out.ok = out.ok && anyPass;
    return out;
}

void MmeDiagJob::Start(const std::filesystem::path& fx) {
    file_ = fx;
    ready_ = false;
    result_ = MmeEffectDiag{};
    future_ = std::async(std::launch::async, [fx] { return BuildMmeEffectDiag(fx); });
}

bool MmeDiagJob::Poll(MmeEffectDiag& out) {
    if (ready_) {
        out = result_;
        return true;
    }
    if (!future_.valid()) return false;
    if (future_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    result_ = future_.get();
    ready_ = true;
    out = result_;
    return true;
}

void MmeDiagJob::Reset() {
    if (future_.valid()) future_.wait();
    future_ = std::future<MmeEffectDiag>{};
    ready_ = false;
    result_ = MmeEffectDiag{};
}

} // namespace mmdx
