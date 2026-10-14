# M3 — MME runtime core + post / pre effects on the real-time frame (`--mme`)

Read first: `docs/mme/mme_design.md` §2.3, §2.4, §2.5, §2.10, checklist "Runtime resources", "Semantics", "Script";
the MME reference `docs/mme/research/mme_spec_draft.md` §4 (semantics, esp. §4.1 matrices, §4.6 time, §4.8
CONTROLOBJECT defaults, §4.9 render targets), §6 (texture annotations / Format names), §7 (script), §8 (states).
M1 (`src/asset/MmeEffect.h`) and M2 (`src/render/MmeCompiler.h`) are done — read their headers before coding.

## Goal
`MMDX12.exe ... --mme path\to\effect.fx[,path2.fx]` runs MME **post effects** (STANDARDSGLOBAL ScriptClass
`scene`/`sceneorobject`, ScriptOrder `postprocess`) on the real-time frame, chained in list order. Object effects
are M4 — an object-class effect given to `--mme` logs `[W] MME <fx>: object effect (needs M4), skipped`.
Pre effects (`preprocess`) are out of scope for M3: log a warning and skip.

## Files
- NEW `src/render/MmeRuntime.h` / `.cpp` — `MmeEffectInstance` (one loaded effect: IR + programs + GPU resources)
  and the per-frame driver used by the pass.
- NEW `src/render/MmeSemantics.h` / `.cpp` — the binder (semantic / annotation → value providers, $Globals writer).
- NEW `src/render/MmeScriptVM.h` / `.cpp` — script interpreter over an `IMmeHost` interface.
- NEW `src/render/PassMme.cpp` — `MmePostPass` (declared in `Passes.h`).
- EDIT (small insertions only): `src/render/Passes.h` (class decl), `src/render/Renderer.cpp` (register the pass
  right after `PackEffectPass(false)`, before `BackdropPass`), `src/render/RenderTypes.h` (`RenderSettings` gains
  `std::vector<MmeEffectEntry> mmeEffects;` — struct below), `src/app/App.h` / `App.cpp` (CLI `--mme`, copied into
  the render settings where `rs.packEffects` is set at App.cpp ~1329; NOT for the benchmark path ~1534).
- `CMakeLists.txt`: nothing (src/render/*.cpp is globbed).

## Data
`struct MmeEffectEntry { std::filesystem::path file; bool enabled = true; float x=0,y=0,z=0, rx=0,ry=0,rz=0 /*deg*/,
si = 1.0f, tr = 1.0f; bool visible = true; bool operator==(const MmeEffectEntry&) const = default; };`
(the accessory panel of the effect's controller object; `si` is the panel value — CONTROLOBJECT `Si` returns si*10).

## Behaviour

### Loading
- An instance is created the first frame its entry appears (keyed by normalised path); it is dropped (after
  `WaitForGpu`-safe deferral: keep it alive for kFramesInFlight frames) when the entry disappears.
- Parse + compile run on a worker thread (`std::async` / `std::thread`), never blocking the render thread; until
  ready the entry is skipped. Log once when ready:
  `[I] MME LOAD <file> kind=<k> programs=<ok>/<total> diags=<errors>/<warnings> ms=<t>` and each Error diag as `[W]`.
- A program that failed to compile disables only the passes that use it (`Pass=` on it is skipped, logged once).

### Resources (per instance, created on the render thread once ready; recreated on output resize)
- `texture` params with `ResourceName` → load via `asset/ImageLoader` (path relative to the declaring file — M1
  gives the param's file), upload to DEFAULT heap with mips if `MipLevels`/`Levels` != 1 (generate on CPU, box
  filter is fine); missing / undecodable → 1x1 white + one `[W]`.
- `RENDERCOLORTARGET`: size = `Width/Height` (or `Dimensions`) else `ViewportRatio` (default 1,1) × output size;
  `Format` mapping per design §2.3 (accept with or without `D3DFMT_` prefix, case-insensitive; default
  `A8R8G8B8` → `R8G8B8A8_UNORM`); `MipLevels` > 1 → create mips, regenerate after each time the target was rendered
  (compute or fullscreen downsample; may be deferred to M5: then log once and use mip 0 only).
- `RENDERDEPTHSTENCILTARGET`: D24S8 / D32 / D16 → `D24_UNORM_S8_UINT` / `D32_FLOAT` / `D16_UNORM`, same sizing.
- `OFFSCREENRENDERTARGET`: M5 — create the texture (cleared to its ClearColor every frame) so effects still bind it,
  log once "offscreen rendering arrives in M5".
- Samplers: one shader-visible sampler heap owned by the runtime (D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048). Per
  program stage, a contiguous sampler table in register order built from the IR sampler states (MINFILTER /
  MAGFILTER / MIPFILTER NONE|POINT|LINEAR|ANISOTROPIC, ADDRESSU/V/W WRAP|CLAMP|MIRROR|BORDER|MIRRORONCE,
  BORDERCOLOR, MAXANISOTROPY, MIPMAPLODBIAS; D3D9 numeric values too). Unknown → linear/wrap. Bind with
  `SetDescriptorHeaps(2, {srvHeap, samplerHeap})` for MME draws and restore the SRV-only heap afterwards (other
  passes expect it).
- SRV tables per draw from `pc.transient` (register order per stage; sampler name → its `texture = <T>` param →
  that texture / render target; unbound → white).

### Pipelines
- One root signature for all MME programs: per stage (VS, PS): root CBV b0 ($Globals), SRV table t0..t15, sampler
  table s0..s15; visibility VERTEX / PIXEL.
- PSO cache key = (program, render-state hash, RT formats/count, DS format) — built lazily.
- Render states from the IR pass (ignore the `VERTEXSHADER`/`PIXELSHADER` entries M1 also records):
  ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP, SEPARATEALPHABLENDENABLE + SRCBLENDALPHA / DESTBLENDALPHA /
  BLENDOPALPHA, ZENABLE, ZWRITEENABLE, ZFUNC, CULLMODE (NONE/CW/CCW, D3D9 left-handed = ours: CCW culls back
  faces as our default does — no flip), FILLMODE, COLORWRITEENABLE(+1..3), STENCIL*, ALPHATESTENABLE / ALPHAREF /
  ALPHAFUNC (no D3D12 equivalent: log once per pass "alpha test ignored" — M4 injects clip), others → log once.
  Values symbolic (`SRCALPHA`, `INVSRCALPHA`, `TRUE`, `LESSEQUAL`, …) or numeric (D3D9 enum numbers, spec §8.1).
  Defaults for post draws: blend off, depth off unless ZENABLE says so and a DS is bound, cull none.
- `Draw=Buffer` geometry: a 4-vertex triangle strip quad, vertex = {float4 POSITION, float4 TEXCOORD (all
  TEXCOORDn map to it), float4 NORMAL (0,0,-1,0), float4 COLOR (1)}; input layout built from the VS input signature
  (M2 `inputs`) by semantic. **D3D9 half-pixel rule**: clip-space positions shifted by (-1/W, +1/H) (W,H = the
  bound target size) so a pixel centre interpolates TEXCOORD = pixel/size exactly as D3D9 did; TEXCOORD 0..1
  top-left (0,0). A pass whose VS is NULL uses a built-in passthrough VS (in `shaders/mme/quad.hlsl`, FXC vs_5_0,
  POSITION/TEXCOORD0 out).

### Binder (MmeSemantics)
- The $Globals staging buffer of each stage is initialised from the variables' reflection default values
  (`D3D11_SHADER_VARIABLE_DESC::DefaultValue`, M2 should expose them — if it does not, add the field in M2's
  `MmeCbVar` (`std::vector<uint8_t> defaultValue`) and fill it) so non-semantic uniforms with literal initializers
  (e.g. `float4 ClearColor = {1,1,1,1}`) hold their values. Then semantics overwrite.
- Matrices: column-major unless `MmeCbVar::columnMajor == false`; our matrices are row-vector `XMMATRIX`: write the
  transpose for column-major variables. Arrays of matrices not needed in M3.
- Frame semantics for post effects (object = the effect's controller accessory, world = scale(si*10) · rotation(rx,ry,rz
  deg, MMD order Y·X·Z? — use the same order our studio props use for `PropAttach` rotation) · translation(x,y,z)):
  WORLD, VIEW, PROJECTION, WORLDVIEW, VIEWPROJECTION, WORLDVIEWPROJECTION (+ INVERSE / TRANSPOSE /
  INVERSETRANSPOSE suffix forms, spec Appendix B), `Object="Camera"` default, `Object="Light"`: a light camera
  looking along the scene light direction (orthographic, covering ±40 MMD units; M5 replaces it); camera = the
  frame's camera **without** TAA jitter. POSITION / DIRECTION with Object Camera / Light. DIFFUSE / AMBIENT /
  SPECULAR / EMISSIVE / SPECULARPOWER with Object="Geometry" → the controller accessory's material
  (no .x mesh in M3: diffuse (1,1,1,tr), ambient (1,1,1), specular (0,0,0), specular power 0, emissive (0,0,0));
  Object="Light" → the scene key light colour (FrameView light, MMD gamma space) for DIFFUSE/AMBIENT/SPECULAR.
  VIEWPORTPIXELSIZE = output size. TIME / ELAPSEDTIME: `SyncInEditMode=true` → the motion clock (the frame's scene
  time) / its delta; false → wall clock since the instance loaded / frame delta. CONTROLOBJECT: name `(self)` (or
  the effect's own `.x` file name) → the entry's panel (bool visible, float si*10, float3/4 position, float4x4
  world, items X/Y/Z/XYZ/Rx/Ry/Rz (radians)/Rxyz/Si(×10)/Tr); other names: a scene model by file name (bone item →
  that bone's world position / matrix from the pose the renderer drew; morph item → weight) if the renderer can
  reach it cheaply, else the spec's missing-object defaults (.x: scale 10, offset (0,0,0,1), world = scale ×10;
  .pmx/.pmd: 1, (0,0,0,1), identity, morph 0). MOUSE* → 0 / false. Unknown semantic → keep default, log once.
- by-name uniforms (`use_texture` etc.) are object-only: M4.

### Script VM
- Implements spec §7 exactly: technique script (or the passes in order when absent), pass script (default
  `Draw=Geometry` → for post effects that is an error: log once, skip), `RenderColorTarget0..3` (empty = default
  target), `RenderDepthStencilTarget`, `ClearSetColor/Depth` (read the named float4/float param from the staging
  values), `Clear=Color|Depth`, `ScriptExternal=Color`, `Pass=`, `LoopByCount=` (int/float/bool param value),
  `LoopEnd=`, `LoopGetIndex=` (write the counter into that param's staging value; affects later draws), nesting,
  ≤ 4096 total iterations per frame, `Draw=Buffer`. Bindings persist across passes; MRT = RT0..3 together.
- Technique choice for post effects: the first technique (post effects have one; `MMDPass` is irrelevant for
  scene class).
- Constant upload per draw (values can change between draws via LoopGetIndex): a runtime-owned per-frame UPLOAD
  ring (`kFramesInFlight` × 4 MB, persistently mapped, 256-byte aligned bump allocator, reset at frame start;
  overflow → skip remaining draws + `[W]` once).

### MmePostPass
- Runs when `settings.mmeEffects` has an enabled, ready, post-class instance; otherwise does nothing (no
  allocation: the frame must stay byte-identical — verify).
- Works at output resolution on `targets.ldr` (RGBA8 UNORM, display values = MMD's back buffer). Default target of
  effect i = a ping-pong RGBA8 target; `ScriptExternal=Color` = blit the chain input (the previous effect's result,
  or `ldr` for the first) into the currently bound colour target (fullscreen copy draw, handles format changes;
  the bound depth target is only cleared). After the last effect, copy the result back into `targets.ldr`.
- Skip when `settings` is in a non-lit view (unlit / wireframe / quad view), like `PackEffectPass`.
- One-time per instance `[I] MME FRAME <file> draws=<n> passes=<m>`; GPU errors must not crash: any failure marks the
  instance failed (`[E]` once) and it is skipped.

### CLI
`--mme <file>[,<file>...]` (paths relative to the current directory or absolute; also accept a library-relative
path under `<exe>/../library` is NOT needed) → `options_.mmeEffects`, copied to `RenderSettings::mmeEffects` for the
run. `--mme-panel si,tr` optional: sets si / tr of every entry (for tests). Unknown files → `[W]`.

## Constraints
- Follow `PassEffect.cpp` (pass structure, transient descriptors, barrier helpers) and `CLAUDE.md` rules
  (per-frame buffers in UPLOAD heap, persistently mapped).
- No changes to M1/M2 files except the `defaultValue` addition if needed.
- Keep `PackEffectPass` untouched.

## Start here
`src/render/PassEffect.cpp` (imitate), `src/render/Passes.h` (~line 300), `src/render/Renderer.cpp` (~line 155-175
pass list), `src/render/RenderTypes.h` (RenderSettings / FrameView), `src/render/RenderPass.h` (PassContext,
TransientDescriptors), `src/app/App.cpp` (`--effect` parsing ~165, `rs.packEffects` ~1329), M1/M2 headers.

## Decided
- d3dx12 helpers (`directx/d3dx12.h`) as PassEffect.cpp uses them.
- The built-in quad VS is FXC-compiled via the existing `CompileShader` from `build/bin/shaders/mme/quad.hlsl`
  (add `shaders/mme/quad.hlsl`; the copy_shaders target copies the shaders folder recursively — verify, and if not,
  compile the VS from an inline string with `D3DCompile` instead).
- Build only in `build_mme` (`cmd //c "F:\\repos\\MMDX12\\build.cmd build_mme"` for the full app this time).

## Off limits
`external/`, `build/`, `library/` (read-only). Do not launch the app (the architect runs visual tests); the
worker verifies by building and by running `mme_compile_test`.

## Verification (worker)
1. Full app builds in `build_mme` with no new warnings in the new files.
2. `mme_compile_test` still passes its self test.
3. Report the exact command lines the architect should run for: o_Monochrome (captures/mme_lib/013_o_tonemap/...),
   a Gaussian / blur effect, o_Vignette, ScreenTex.
