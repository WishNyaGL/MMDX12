#pragma once
// Shared NGX SDK lifetime. NGX is initialised once per device; DLSS Super Resolution (UpscalerDlss.cpp) and
// DLSS Frame Generation (DlssFrameGen.cpp) take a reference each. All calls must happen on the render thread.
namespace mmdx {

class Dx12Context;

// Initialises NGX on the first reference (same app id / data path as the DLSS upscaler used before). Returns true
// when NGX is initialised for the caller (a reference was taken); false when it could not initialise (logged once).
bool NgxSharedInit(Dx12Context& ctx);
// Releases one reference; the last one shuts NGX down.
void NgxSharedRelease();

} // namespace mmdx
