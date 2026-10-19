#pragma once
#include "app/Settings.h"
#include "render/LookPreset.h"

namespace mmdx {

// Builds a look from the current settings. `surface` non-null gives the look a surface choice and sets the surface
// group.
LookData CaptureLook(const AppSettings& s, const ShaderChoice* surface);

// Applies the look's groups to the settings. Only the groups the look sets are written; a surface group is written
// only when `surface` is non-null.
void ApplyLook(const LookData& look, AppSettings& s, ShaderChoice* surface);

} // namespace mmdx
