# MME compatibility for MMDX12 — design and checklist

Status: design v1 (2026-10-14). Architect: Claude. Inputs: `docs/mme/research/mme_spec_draft.md` (MME reference,
REFERENCE.txt / nanoem-cited), `docs/mme/research/effects_catalog.*` (popular effects), `tools/mme/mme_scan.py`
(feature + compile census), `docs/mme/compat_list.md` (per-effect results).

## 0. Principle: read MME's format, render it our way

We do **not** emulate a Direct3D 9 runtime (no D3DX9 Effect object, no d3d9 device, no state-block replay). MME
effect files are a *description*: global parameters with semantics, textures, samplers, techniques, passes, render
states and a small script language. We parse that description ourselves into an intermediate representation (IR),
compile only the HLSL *functions* the passes name with modern FXC as Shader Model 5 (verified: fxc 10.0.26100
compiles DX9-style `sampler_state` + `tex2D` + `: COLOR` under `/T ps_5_0 /Gec`), and execute the script as an
ordered list of D3D12 operations inside our own pass graph, fed from our own scene data (`FrameView`,
`GpuModel`, `ModelInstance`). Everything MME takes from MMD's internals (CPU-skinned vertices, MMD's shadow map,
the back buffer, the accessory panel) is *produced* by our renderer in the shape the effect expects.

Consequences:
- An MME effect is one more consumer of our renderer, like a shader pack. The native pipeline (PBR, TAA, upscalers,
  RT, PT, offline GI) stays untouched when no MME effect is active (bit-identical frame; same rule as packs).
- Where our renderer already does a popular effect better (Ray-MMD materials, AutoLuminous, DoF, bloom, SSAO) we
  keep a **native equivalence table** and offer it, but the generic runtime remains the default for files the user
  explicitly applies (users expect "their" effect).
- What D3D9-only constructs cannot be translated (inline `asm {}`, ps_1_x assembly, MMD-internal register tricks
  beyond the documented ones) is reported per effect, never silently mis-rendered.

## 1. Scope (what "supported" means)

| Area | v1 (this plan) | Later |
|---|---|---|
| Post effects (`ScriptClass=scene`, `ScriptOrder=postprocess`) | yes, full script language | — |
| Pre effects (`preprocess`) | yes (draw before the scene into the colour target) | — |
| Object effects on characters / stages / props (`object`, `object_ss`, `edge`, `shadow`, `zplot`) | raster + RT (direct); PT + offline GI via the MME primary layer (§2.6) | — |
| `OFFSCREENRENDERTARGET` (+ `DefaultEffect` patterns) | yes | — |
| `CONTROLOBJECT` (models, bones, morphs, effect objects, `(self)`, `(OffscreenOwner)`) | yes | — |
| `.x` accessories (mesh + material) | text `.x` + binary `.x` (uncompressed) | compressed `.x` (mszip) |
| `.emd` (per model), same-name `.fx`, `name[fx].pmx` | yes | — |
| `.emm` (per scene) | via PMM import | standalone `.emm` open |
| `ANIMATEDTEXTURE` (gif/apng), `TEXTUREVALUE` | yes | — |
| Mouse semantics | constant zero / not pressed | live in studio viewport |
| MMM extensions (`MMM_*`, multi-light arrays, GPU skinning API) | parsed, `MIKUMIKUMOVING` undefined → MME branch | — |

## 2. Architecture

```
 asset/                         render/Mme*                                     app/
 ┌──────────────────────┐      ┌───────────────────────────────┐             ┌─────────────────────────┐
 │ MmeEffectParser      │ IR → │ MmeCompiler (FXC SM5, cache)  │             │ MmeLibrary (effects/)    │
 │  .fx → MmeEffectDesc │      │ MmeEffectInstance             │ ←─ owns ─── │ MmeScene (assignments,   │
 │ MmeAssign (.emd/.emm/│      │  resources, PSOs, cbuffers    │             │  effect objects, ctrl)   │
 │  same-name / [fx])   │      │ MmeSemanticBinder             │             │ UI: effect stack, studio │
 │ XFileLoader (.x)     │      │ MmeScriptVM (commands → D3D12)│             │  outliner, inspector     │
 └──────────────────────┘      │ MmeObjectPass / MmePostPass   │             └─────────────────────────┘
                               │ MmeSkinPass (pre-skinned VB)  │
                               │ MmeShadowMap (MMD-style)      │
                               └───────────────────────────────┘
```

### 2.1 Parser and IR (`src/asset/MmeEffect.{h,cpp}`) — CPU only, no D3D

Input: an `.fx` path. Steps:
1. Load bytes (cp932 / UTF-8, reuse `asset/MmeText.h`), run **our own light preprocessor** only to *find
   structure*: `#include` (relative to includer, then to the first file's folder, case-insensitive, depth ≤ 16,
   cycle-safe), `#define` / `#undef` / `#if/#ifdef/#elif/#else/#endif` with integer-expression evaluation, macro
   expansion (object-like and function-like, `#`/`##`). Techniques are frequently macro-generated (§A.1 of the
   spec draft: MES40, ray-mmd), so structure must be read *after* expansion. The full HLSL body is left to FXC.
2. Tokenise the expanded text; extract:
   - `MmeParam`: type (scalar/vector/matrix/bool/int/string/texture*/sampler*), name, semantic (upper-cased),
     annotations (typed values), initializer (numeric array or string), `shared`/`static`/`uniform` flags, array
     size.
   - `MmeSampler`: name, dimension, `texture = <name>`, filter / address / border / maxAnisotropy / mipLodBias
     (symbolic or numeric), `SRGBTexture` (ignored, logged).
   - `MmeTechnique`: name, annotations (`MMDPass`, `Subset`, `UseTexture`, `UseSphereMap`, `UseToon`, `Script`),
     passes in order.
   - `MmePass`: name, `Script` annotation, render states (name → value expression), VS / PS:
     profile, function name, literal argument list (or `NULL`, or `asm` → unsupported flag).
   - `MmeGlobal`: `STANDARDSGLOBAL` (ScriptOutput / ScriptClass / ScriptOrder / Script).
   - `MmeScript`: parsed commands (`RenderColorTarget[0-3]`, `RenderDepthStencilTarget`, `ClearSetColor`,
     `ClearSetDepth`, `Clear`, `ScriptExternal`, `Pass`, `LoopByCount`, `LoopEnd`, `LoopGetIndex`, `Draw`), with
     adjacent-string concatenation and case-insensitive command names; unknown commands kept + reported.
3. Diagnostics list (`MmeDiag{severity, file, line, message}`), never throws.

The IR is pure data so `tools/mme_parse_test` can run it over thousands of files.

### 2.2 Shader translation (`src/render/MmeCompiler.{h,cpp}`)

Per pass and stage, generate a **wrapper translation unit** = the original source (bytes untouched, compiled with
the file's folder as include root through an `ID3DInclude` that mirrors the parser's resolution) **minus** the
`technique { }` blocks and `asm { }` bodies (blanked to spaces so line numbers in errors stay valid), **plus** an
appended wrapper entry:

- **Compile arguments**: `compile ps_3_0 PS(true, 2)` binds the literal args to the target function's `uniform`
  parameters in order (D3DX rule). The wrapper declares the remaining varying parameters with their semantics and
  calls the target with the literals.
- **Semantic fix-ups** (wrapper-side, never editing user code):
  - VS input `_INDEX` → `SV_VertexID` (user param type `int`/`float` cast); MME's `_INDEX` is the vertex index.
  - PS input `VPOS` → `SV_Position.xy - 0.5` (D3D9 pixel coordinates are integer at the pixel's top-left).
  - PS input `VFACE` → `SV_IsFrontFace ? 1 : -1` with D3D9's sign convention (front = positive).
  - Outputs `COLOR0..3` / `DEPTH` are accepted by `/Gec` as `SV_Target0..3` / `SV_Depth`.
  - A function parameter or global the regex rewrite cannot handle → the pass is marked unsupported with the FXC
    message.
- **Host extras** (object passes only, see §2.6): the wrapper VS also outputs the previous clip position (motion
  vectors) and the PS wrapper writes our G-buffer MRTs around the user's colour.
- **Signature linkage**: D3D12 requires the PS input signature to line up with the VS output registers, while D3D9
  let a PS read any subset (missing inputs = 0). The PS wrapper's input struct is therefore generated from the
  *paired VS's output signature* (reflection), and forwards the elements the user's PS asks for by semantic (absent
  → zero). Programs are compiled per (pass, VS, PS) pair; `Draw=Buffer` passes pair with their own VS or with the
  built-in quad VS when the pass sets none.
- **Per-stage binding**: VS and PS each get their own `$Globals` CBV and SRV / sampler tables (FXC numbers `t#`/`s#`
  per shader), visibility-restricted root parameters.
- **Source**: the wrapper compiles M1's fully preprocessed `expandedSource` (includes inlined, technique blocks
  blanked) with `D3DCompile` (no include handler); if that fails while the original file compiles under FXC's own
  preprocessor (our preprocessor diverged), retry from the original file with an `ID3DInclude` and log a Warning.
- Profile mapping: `vs_1_1..vs_3_0` → `vs_5_0`; `ps_2_0..ps_3_0` → `ps_5_0`; flags `/Gec /O3`; defines
  `MMDX12=1`, `MME_VERSION=37` (MMM's `MIKUMIKUMOVING` is *not* defined: effects take their MME branch).
- **Reflection** (`ID3D12ShaderReflection`, FXC DXBC): `$Globals` cbuffer variable offsets / sizes, each sampler
  name → `t#`/`s#` registers (DX9 samplers become a texture + sampler pair with the sampler's name), explicit
  `register(s0)` bindings (MMD's self-shadow map, §2.7). The binder writes values by name into a CPU staging copy
  of `$Globals` per draw and uploads into the per-frame ring.
- Root signature (one for all MME PSOs): per stage (VS, PS) a root CBV `b0` ($Globals), a 16-SRV table `t0..t15`
  and a 16-sampler table `s0..s15` (dynamic samplers from a dedicated sampler heap, because MME sampler states are
  per effect); host extras at `space1`. Effects that exceed 16 textures in one pass are reported unsupported (none observed).
- **Cache**: `shader_cache/mme/<sha256(source closure + entry + wrapper + flags)>.dxbc`; FXC runs on worker threads
  at effect load (FXC is slow for big effects: ray-mmd), never on the render thread. A failed compile disables that
  pass only; the instance reports it.

Measured feasibility (scan of 457 library effects, `captures/mme_scan_library/summary.md`): 1601 / 1785 passes
compile as SM5 (18 fail, 166 skipped by the scanner's signature parser); the failing classes are `_INDEX` input
(handled by the fix-up) and macro-dependent syntax.

### 2.3 Effect instance and resources (`render/MmeEffectInstance`)

Created from IR + compiled passes; owns:
- **Textures** from `ResourceName` (decoded through `asset/ImageLoader` — dds via our `asset/DdsLoader`; missing →
  white +
  diag), `ANIMATEDTEXTURE` (decoded frames, frame chosen by `Offset`/`Speed`/`SeekVariable` + time), `TEXTUREVALUE`
  (CPU readback of a 1×N texture into a float4 array param).
- **Render targets**: `RENDERCOLORTARGET` / `RENDERDEPTHSTENCILTARGET` with `Width/Height` or `ViewportRatio`
  (relative to the **output** resolution), `Format` (D3D9 names → DXGI: `A8R8G8B8`/`X8R8G8B8` → `R8G8B8A8_UNORM`,
  `A16B16G16R16F` → `R16G16B16A16_FLOAT`, `A32B32G32R32F`, `R32F`, `G16R16F`, `G32R32F`, `A16B16G16R16` (UNORM),
  `R16F`, `D24S8`/`D3DFMT_D24S8` → `D24_UNORM_S8_UINT`, `D32F_LOCKABLE`/`D32` → `D32_FLOAT`), `MipLevels`
  (> 1, or 0 = full chain: the level-0 target carries the whole chain and the levels below 0 are box-filtered
  after the pass / offscreen draw that wrote mip 0, `MmeRuntime::GenerateMips`), `AntiAlias` (ignored: we never
  MSAA MME targets; logged). Recreated on resize. `shared` textures are keyed by name in `MmeScene`.
- **Offscreen targets** (`OFFSCREENRENDERTARGET`): an RT + depth with `ClearColor`/`ClearDepth`, plus the parsed
  `DefaultEffect` table (`pattern = fx|none|hide` lines, first match wins, `self` = the owner, `*` wildcard,
  file-name patterns on our model file names) resolved against the scene each frame into a draw list. Each listed
  object is drawn with its *assigned sub-effect* (an `MmeEffectInstance` loaded relative to the owner effect) or,
  for `none`, the built-in standard effect (§2.8).
- **PSO cache** keyed by (pass, render-state hash, RT formats, depth format, input-layout id).
- **Render-state translation** (nanoem's D3D9 enum table, spec §8): blend (incl. separate alpha, `BlendOp`), depth
  (`ZEnable`, `ZWriteEnable`, `ZFunc`), stencil (front/back), cull (`CullMode` CW/CCW in D3D9's left-handed
  winding = ours, no flip), fill mode, colour write mask, `AlphaTestEnable`+`AlphaRef`+`AlphaFunc` → injected
  `clip()` in the PS wrapper (D3D12 has no alpha test), `PointSpriteEnable`/`PointSize` → unsupported diag.
  Defaults = D3D9 defaults as MMD sets them (depth on, LESSEQUAL, write on, cull CCW, alpha blend SRC_ALPHA /
  INV_SRC_ALPHA as MMD enables it for objects, off for Draw=Buffer).

### 2.4 Semantic binder (`render/MmeSemantics`)

One table, semantic → provider, evaluated per frame (scene values), per object (world, material), per draw:

| Semantic | Our source |
|---|---|
| `WORLD`, `VIEW`, `PROJECTION`, `WORLDVIEW`, `VIEWPROJECTION`, `WORLDVIEWPROJECTION` (+`INVERSE`, `TRANSPOSE`, `INVERSETRANSPOSE`), `Object="Camera"` | object world = identity for PMX models (skinned positions are already in world space, §2.6), accessory / prop transform for `.x` and studio props; view/proj = `FrameView` camera **without TAA jitter** when TAA is off; with TAA on, the jittered projection (so MME objects stay stable after TAA) |
| same with `Object="Light"` | the MMD-style light camera of `MmeShadowMap` (§2.7) |
| `DIFFUSE`/`AMBIENT`/`EMISSIVE`/`SPECULAR`/`SPECULARPOWER`/`TOONCOLOR`/`EDGECOLOR`/`GROUNDSHADOWCOLOR` (Geometry/Light) | `MaterialConstantsCpu()` (material morphs applied) and the scene light (`FrameView::light` key light colour, MMD gamma space); MMD's rule "Geometry ambient = material ambient, light ambient = light colour" etc. per spec §4.2 |
| `POSITION`/`DIRECTION` (Camera/Light) | eye position / view dir; MMD light direction |
| `MATERIALTEXTURE`/`MATERIALSPHEREMAP`/`MATERIALTOONTEXTURE` | the material's SRVs (`GpuModel::Material::srvTable` +0/+1/+2) |
| `ADDINGTEXTURE`/`MULTIPLYINGTEXTURE`/`ADDINGSPHERETEXTURE`/`MULTIPLYINGSPHERETEXTURE` | `MaterialConstants::texAdd/texMul/sphereAdd/sphereMul` |
| `VIEWPORTPIXELSIZE` | the output (post) or render (object) viewport size |
| `TIME`/`ELAPSEDTIME` (`SyncInEditMode`) | motion clock (`SyncInEditMode=true` / frame time) vs. wall clock since effect load |
| `CONTROLOBJECT` | §2.9 |
| `STANDARDSGLOBAL` | constant (ScriptOutput etc. are metadata) |
| by-name uniforms `use_texture`, `use_spheremap`, `use_subtexture`, `use_toon`, `parthf`, `transp`, `spadd`, `opadd`, `VertexCount`, `SubsetCount` | material flags (sphere mode add / sub-texture from PMX), `parthf` = MMD's perspective-shadow mode flag (MMD's default self-shadow mode 1 has it off, so we always pass `false`), `transp` = transparent background off |
| MMM-only semantics | zero + one diag per effect |

The binder resolves a parameter once into a `(provider id, offset, size, transpose?)` record; per draw it is a loop
of memcpy. Matrices: DX9 effects default to column-major packing in constant registers; FXC SM5 reflection reports
the same layout for `$Globals` (`/Gec` keeps `float4x4` column-major unless the source says `row_major`), so the
binder writes the transpose of our row-vector `XMMATRIX` unless the variable is declared `row_major` (reflection
`D3D_SVC_MATRIX_ROWS`).

### 2.5 Script VM (`render/MmeScriptVM`)

Interprets technique / pass scripts into calls on an `IMmeHost` (implemented by our passes):
`BindColorTarget(i, rt|default)`, `BindDepth(rt|default)`, `Clear(color|depth)`, `RunScriptExternal()`,
`DrawGeometry(pass)`, `DrawBuffer(pass)`, loops with counters written back into params (`LoopGetIndex`). Rules from
the spec: bindings persist across passes; a technique without a script runs its passes in order; a pass without a
script = `Draw=Geometry`; `Draw=Buffer` draws a full-screen triangle-pair **shifted by half a pixel up-left**
(D3D9 rasterisation rule, so effects that add `ViewportOffset = 0.5 / ViewportSize` land on texel centres exactly as
in MMD); `object` class may not `Draw=Buffer`, `scene` class may not `Draw=Geometry` (diag, skip). Loop guard:
≤ 4096 iterations per frame.

### 2.6 Object effects (`MmeObjectPass` + `MmeSkinPass`)

- **Vertex stream**: MME effects expect CPU-skinned positions. `MmeSkinPass` (compute, after the pose upload,
  reusing `shaders/skin.hlsl`'s skinning incl. SDEF + vertex morphs) writes per model an `MmeVertex` buffer:
  `POSITION` float4 (world space, w=1), `NORMAL` float3, `TEXCOORD0` float2, `TEXCOORD1..4` float4 (PMX additional
  UVs, zero when absent), `COLOR0` float4 (= add UV1 for MES-style effects? no: white), plus the previous position
  for motion vectors. Only models that have an MME object effect (or appear in an offscreen draw list) are skinned
  this way. RT already skins into `RtVertex`; when both exist, `MmeSkinPass` may reuse it (optimisation, not v1).
- **Draw steps** per frame, mirroring MMD (spec §1.4 / nanoem `drawViewport`): `zplot` into the MMD-style shadow
  map → pre effects → for each MME object model: `edge` → `object_ss` (self-shadow on; fall back to `object`) →
  `shadow` (ground shadow, when the floor shadow is on) → post effects. Technique choice per material: first
  technique in file order with matching `MMDPass`, `Subset` (`0-6,8`, `12-`), `UseTexture`/`UseSphereMap`/`UseToon`
  (tri-state); none → the model's **native** shading for that material (our "MMD standard shader"); empty technique
  → not drawn.
- **Targets**: object effects draw into the scene's MSAA colour target inside `ScenePass` (same pass, after the
  native opaque models, keeping MMD's model order for alpha), so SSAO / SSR / TAA / upscalers keep working. The PS
  wrapper writes: colour = `SrgbToLinear(saturate(user.rgb)) * gSunIntensity`, alpha unchanged (our scene colour
  is linear HDR; MME colours are MMD gamma space — the same conversion the native MMD shading uses, mmd.hlsl:575);
  normal target = view normal from the wrapper VS with `nt.z = -1` (no AO / SSR on effect-shaded pixels, they own
  their look); velocity from the wrapper's previous clip position.
- **Edges**: an `edge` technique replaces our edge pass for that model; none → our edge pass.
- **RT path**: same raster draws (the effect replaces the RT-shadowed shading for those materials).
- **PT (real time)**: an MME *primary layer*, the same pattern as the PT outline layer (`ScenePass::DrawPtEdges` +
  `pt_edge.hlsl`): MME-owned materials are rasterised with the tracer's jittered camera into the MSAA scene targets
  (colour = the effect's output linearised, depth, velocity) and `PathTracePass` composites that layer over the
  denoised radiance where its depth wins. In the tracer those materials stay real geometry with native material data
  (albedo from the effect's `MATERIALTEXTURE` × diffuse), so they cast shadows and bounce light into the scene; their
  own on-screen shading is the effect's (MME's meaning of an object effect).
- **Offline GI**: the same layer per iteration with that iteration's lens / shutter camera and pose, accumulated like
  the offline outline layer (`offline_edge_pack.hlsl`), so MME surfaces get AA, DoF and motion blur from the
  accumulation; geometry participates in GI as above.
- Post / pre effects run on every path's final image, including offline GI stills and videos (the
  `PackEffectPass::RunOffline` pattern).
- Known limit: indirect light falling *onto* an MME surface reaches it only if the effect itself shades it (the
  effect owns that surface's lighting).

### 2.7 MMD-style self-shadow map (`MmeShadowMap`)

`object_ss` techniques sample the shadow buffer bound by MMD at `register(s0)` and compute coordinates from
`WORLDVIEWPROJECTION <Object="Light">`. We provide exactly that: when any loaded effect has an `object_ss` or `zplot`
technique, render a single MMD-style light-space depth map (R32_FLOAT, 2048², MMD's self-shadow projection fitted
from the studio self-shadow distance / play-mode default) with each model's `zplot` technique (or our built-in zplot
when none), and bind it at `t0/s0` (point clamp) for `object_ss` draws when the effect declares a `register(s0)`
sampler. Our CSM stays for native shading.

### 2.8 Built-in "standard" effect

`none` in `DefaultEffect`, a missing technique, and offscreen draws of objects without effects need "MMD's standard
shader". We ship `shaders/mme/standard.fx`: our own clean-room MME-format effect reproducing MMD's default look
(object / object_ss / edge / shadow / zplot, MMD colour model as mmd.hlsl:575). It goes through the same parser /
compiler, which also makes it the runtime's first regression test.

### 2.9 Scene objects, controllers and `CONTROLOBJECT`

`MmeScene` (app side) is the list of *drawables* the runtime can see, in MMD order: models (characters, stages, props)
and **effect objects**. An effect object is our version of MMD's `.x` controller accessory: a scene item with an
effect file, an optional mesh (`.x` loaded through `XFileLoader`, else none), and an accessory panel
(`X/Y/Z`, `Rx/Ry/Rz`, `Si` (×10 rule), `Tr`, visible) that is keyable in the studio through the existing accessory
key track (`AccessoryKf`, `RowKind::Accessory`). `CONTROLOBJECT` resolves `name` against drawables by file name
(case-insensitive, then display name), `(self)`, `(OffscreenOwner)`; `item` = panel values, a bone name (world
position / matrix from `ModelInstance`), a morph name (weight). Missing object → the spec's defaults (bool = false,
float = 0, matrix = identity).

### 2.10 Post / pre effects (`MmePostPass`)

- Runs on the **display-referred LDR image** (`targets.ldr`, sRGB 8-bit → we use an RGBA16F copy so effects that
  write > 1 through float targets keep it): MMD's post effects were authored against MMD's back buffer, which holds
  final display values. Position: after our `PostPass` (+ the post-stage effect packs) and before `BackdropPass` /
  `PresentPass`, at output resolution.
- `ScriptExternal=Color` = "make the scene available in the currently bound target": we copy the current frame
  (with earlier post effects applied, MMD chaining order = effect object order) into it, and the bound depth target
  receives our scene depth (converted from reversed/linear as needed into D24 / R32F semantics). No re-render.
- Post effects that need **scene data beyond colour** use offscreen targets (depth, normals, luminous masks: SvSSAO,
  AutoLuminous, o_Bloom's masks). Those go through §2.3 offscreen rendering of the scene's models with the
  sub-effects the effect ships. This is the heavy path; it costs one extra raster draw of the listed models per
  offscreen target per frame.
- Pre effects run before `ScenePass` draws models, into the scene colour target (backgrounds, sky).
- The MME stack appears in the existing effect stack (`RenderSettings::packEffects`) as entries with pack id
  `mme:<library-relative path>`; play mode / lobby / studio share it. In the studio the stack is per project
  (effect objects in the outliner), which also closes the "studio projects don't store the stack" gap for MME.

### 2.11 Colour management and "MME look"

MME effects are written for MMD's gamma-space, non-tonemapped pipeline. Two modes:
- **Integrated** (default): object-effect output linearised into our HDR scene (above); post effects on the final
  LDR image. Our tonemapper / grade still shape the image before post effects.
- **Faithful** (`mmeFaithful` setting, on by default when a PMM with MME is imported or `--classic`): PostPass uses
  exposure 1, no tonemap / grade / LUT / vignette (identity sRGB encode), no bloom / native AO / SSR, so an MME scene
  looks as in MMD. This is the mode compatibility tests compare in.

### 2.12 Assignment sources (load order, highest first)

1. Explicit user choice (studio inspector / play-bar shader popup "MME…", per model, per material).
2. `.emm` / PMM-imported assignments (`asset/PmmLoader` → MME effect objects + model assignments; today only
   mapped to native settings by `MapMmeControllers` — kept as the "native equivalent" suggestion).
3. The model's `.emd` (`Obj`, `Obj[n]`, `Obj.show`; `asset/MmeHints` already reads it) and same-name `.fx` /
   `name[fx].pmx`. These are **suggested, not applied** (same policy as shader-pack suggestions): a toast offers
   "this model ships an MME material", one click applies it. Ray-MMD `.emd` sets keep using our native PBR path
   (§3) unless the user forces MME.
4. Native fallback without the runtime: `asset/MmeHints` classifies a model's own `.emd` `Obj[n]` effects as
   normal / no-depth-write / additive (`MatFlag_NoDepthWrite` / `MatFlag_Additive`, the Alcander fire-tail fix by
   the bugfix session). Any material the MME runtime owns ignores those flags; once M4 lands, that classification
   moves into `MmeAssign` (one reader of `.emd`). `MaterialShaderFlags` bits 10 (`MatFlag_Additive`) and 11
   (`MatFlag_NoDepthWrite`) are taken by that fix; MME flags use other bits.

### 2.13 Library and UI

- `effects/` folder in the user's library (template gains `effects/`): `AssetLibrary` lists `.fx` files that are
  effect entry points (not `.fxsub`), classified by the parser (post / object / mixed / offscreen) with a thumbnail
  (the effect's readme image when present, else a rendered preview later).
- Shader manager screen gets an "MME" tab (list, diagnostics per effect: unsupported features, failed passes,
  missing textures) — the compatibility list is visible to users.
- Studio: "+" menu → "MME 효과" (effect object, optional .x), inspector: effect file, panel values, parameter
  editing for `UIName`/`UIWidget` annotated params (MMM convention), per-material assignment.
- Drag & drop of `.fx` / `.x` (+ `.emd`, `.emm`) onto the studio.

## 3. Native equivalence table

Effects for which a native path exists and is offered as "MMDX12 대체" (alternative), never forced:

| MME effect | Native equivalent | Notes |
|---|---|---|
| ray-mmd (materials + post) | `--material-source ray` PBR + our post | the MME path is attempted but ray-mmd's deferred pipeline is the heaviest case |
| AutoLuminous | `autoLuminous` (MatFlag_AutoLuminous) | AL's offscreen path also runs generically |
| Diffusion / o_Bloom / LightBloom | BloomPass (+ FFT) | |
| SvSSAO / HgSAO | SsaoPass / RTAO | |
| ikBokeh / Dof | DofPass / bokeh_dof pack | |
| ikClut / ColorLUT effects | colour LUT (`Renderer::SetColorLut`) | |
| WorkingFloor | studio floor reflections | |

The PMM importer already maps these controllers (`MapMmeControllers`); with the runtime, the import dialog offers
"MME 그대로" (run the real files) vs. "네이티브로 변환".

## 4. Compatibility tiers (used by `compat_list.md`)

- **A — runs unchanged**: parser OK, every pass compiles, every feature it uses is in v1. Expected to match MMD.
- **B — runs with differences**: uses a feature we approximate (mouse, AntiAlias targets, MMM branch, gif seek).
- **C — partially**: some passes fail / unsupported features (asm, point sprites, > 16 textures); the effect runs
  with those passes skipped.
- **N — native equivalent preferred**: listed in §3.
- **X — not supported**: depends on MMD internals we do not reproduce, D3D9-only extensions, or fails to parse.

Each catalog entry's tier is computed by `tools/mme/mme_scan.py` (static) and confirmed by the runtime test harness
(§6) once implemented.

## 5. Implementation milestones (each = one worker spec, reviewed + QA'd before the next)

| M | Content | Verification |
|---|---|---|
| M1 | `asset/MmeEffect` parser + preprocessor + IR, `asset/MmeAssign` (.emd/same-name/[fx]), `tools/mme_parse_test` | parses every `.fx` in `library/` + `captures/mme_lib`, zero crashes, IR dump diff on 5 golden files |
| M2 | `render/MmeCompiler` (wrapper generation, fix-ups, FXC SM5, reflection, cache), `tools/mme_compile_test` | pass compile rate ≥ scanner's SM5 rate on the library pack; `standard.fx` compiles |
| M3 | Runtime core: instance resources, render-state → PSO, binder (frame-level semantics), script VM, `MmePostPass`, effect stack entries `mme:`; CLI `--mme <fx>[,<fx>]` | post-only effects from the pack (Gaussian, ColorShift, vignette, Diffusion-like) render in play mode; captures |
| M4 | Object effects: `MmeSkinPass`, object binder (material semantics, by-name uniforms), technique selection, ScenePass integration, `standard.fx`, edge/shadow; CLI `--mme-model <fx>` | full.fx-family / toon shaders on a character; raster + RT; no `[E]` |
| M5 | (+ PT / offline-GI MME primary layer, post effects in offline renders) Offscreen targets + DefaultEffect, `MmeShadowMap` (zplot / object_ss), `CONTROLOBJECT`, effect objects, `XFileLoader` | AutoLuminous, SvSSAO, WorkingFloor, ExcellentShadow-class effects |
| M6 | App integration: library `effects/`, shader manager MME tab + diagnostics, studio effect objects (+ accessory keys, project save/load), play-bar / lobby, drag & drop, PMM import "MME 그대로", `.emd` suggestion | studio scripted E2E, project round trip |
| M8 | MV packages (§5b): song-bundled stage + stage motion, alternative audio, video reader (MF + raw AVI), background / overlay video pass, offline render support | `Dreamin Chuchu` plays as the full MV (play mode + offline video) |
| M7 | Compatibility harness: `tools/mme/mme_compat_run.py` renders every pack effect on a pinned scene (`--mme`, faithful mode), writes `compat_list.md` results + contact sheet; fix pass for the top failures | ≥ 80 % of the top-50 at tier A/B |

## 5b. M8 — "complete MV" packages (song folder with its own stage, stage motion, MME, videos)

Goal: a song folder that ships everything an MV needs plays as the full MV — without assuming any one layout
(`library/motion/Dreamin Chuchu` is the QA reference, not the template). Classification is by **content**, names
only break ties; every guess can be overridden per song (lobby song header / right-click, ini lines like
`songCamera=`), and the scan logs each decision as a library note.

| Element | Detection (content first) | Playback |
|---|---|---|
| Song-bundled stage | a `.pmx`/`.pmd`/`.x` under the song folder that the stage classifier accepts (or a sub-folder with one) | becomes the song's default stage (lobby shows "곡 전용 스테이지"; the user's stage choice still wins when set) |
| Stage motion | a VMD whose bone names match the bundled stage model's bones better than any humanoid (score = matched keyed bones / keyed bones; humanoid core bones センター/上半身/下半身 absent) | bound to the stage model (`ModelInstance` + `BoundMotion` like a character, no physics); stages become animatable in play mode |
| Alternative audio | several audio files whose lengths match within 0.5 s | song header offers a choice (like cameras), saved as `songAudio=` ini; default = the current pick |
| Overlay video | a video whose duration ≈ song length (±3 %) | composited over the final image at output resolution, synced to the audio clock; blend = **black key** by default (alpha from luminance, smoothstep 0.02..0.08) with `alpha` (if the codec has one) / `screen` / `add` / `normal` overrides |
| Background video | a shorter video (MMD 背景AVI) | drawn behind the scene (replacing sky / clear colour), looped |
| Model effects | the bundled stage's (and characters') `.emd` / same-name `.fx` | MME object effects (M4/M5) applied automatically for song-bundled assets (they are part of the MV), suggested-only for library characters (§2.12) |

Video decoding: Media Foundation `IMFSourceReader` (H.264 / HEVC / WMV mp4/mov/wmv, hardware decode when
available) to BGRA, and a small built-in RIFF AVI reader for uncompressed DIB AVIs (`rawvideo` BGR24/32 — MMD's
background-AVI format; MF has no AVI source). Frames are decoded on a worker thread ahead of the audio clock into
a ring of upload textures; seeks flush the ring. Video layers also render in offline videos / stills (frame-exact
by time). New code: `asset/VideoReader.*`, `render/PassVideo` (background + overlay), `asset/AssetLibrary`
classification, lobby/song header UI, ini + project fields.

QA (user requirement): `Dreamin Chuchu` plays as the complete MV — 5 performers (formation), the bundled stage
with its stage motion and MME materials (APNG monitors animate), the selected camera, the chosen audio version,
the bg.avi background loop and the CameraDecoration overlay — in play mode and in an offline video render.

## 6. Test strategy

- Static: `mme_parse_test`, `mme_compile_test` over `library/` + `captures/mme_lib` (CI-able, no window).
- Runtime: one app run per effect (sequential, never parallel), pinned frame (`--seek 20 --paused --no-physics
  --frames 60`), faithful mode, `--capture-scene 1`; log lines `MME LOAD <fx> passes=a/b diags=n` and
  `MME FRAME <fx> draws=n` for automation; `[E]` on any runtime failure.
- Reference: the author's preview images (catalog `preview_images`) for a visual sanity check; where the user can
  provide MMD screenshots of the same scene, side-by-side comparison in the contact sheet.

## 7. Checklist

### Parsing / IR
- [ ] cp932 / UTF-8 decode, BOM, CRLF
- [ ] `#include` resolution (relative, case-insensitive, `.fxsub`/`.fxh`/`.h`), cycle guard
- [ ] preprocessor: `#define` (object / function-like, `#`, `##`), `#undef`, `#if` integer expressions, `defined()`
- [ ] adjacent string concatenation in annotations
- [ ] params: semantic, annotations, initializers, arrays, `shared`, `static`
- [ ] samplers: `sampler_state` contents, symbolic + numeric values
- [ ] techniques: MMDPass / Subset / Use* tri-state / Script; passes: states, compile exprs, `asm` flag
- [ ] STANDARDSGLOBAL
- [ ] diagnostics with file:line

### Compilation
- [ ] wrapper entry with compile-argument binding (uniform params in order)
- [ ] technique / asm blanking with line preservation
- [ ] `_INDEX`, `VPOS`, `VFACE` fix-ups; `COLORn`/`DEPTH` outputs
- [ ] SM5 `/Gec`, defines `MMDX12`, `MME_VERSION`
- [ ] reflection: `$Globals` layout, sampler → t/s registers, `register(sN)` explicit bindings, row/column major
- [ ] disk cache, worker-thread compile, per-pass failure isolation

### Runtime resources
- [x] textures (`ResourceName`, missing → white), dds/png/bmp/tga/jpg
- [x] `RENDERCOLORTARGET` formats + ViewportRatio + MipLevels; resize
- [ ] `RENDERDEPTHSTENCILTARGET`
- [ ] `OFFSCREENRENDERTARGET` + `DefaultEffect` patterns (`self`, `*`, `none`, `hide`, sub-effects)
- [ ] `ANIMATEDTEXTURE`, `TEXTUREVALUE`
- [ ] `shared` textures across effects
- [x] render states → PSO (blend, separate alpha, depth, stencil, cull, fill, write mask, alpha test → clip)
- [ ] sampler heap for per-effect samplers

### Semantics
- [ ] matrices × Camera/Light × INVERSE/TRANSPOSE; column/row major
- [ ] material + light colours (per spec §4.2 rules)
- [ ] material textures + morph add/mul semantics
- [ ] VIEWPORTPIXELSIZE, TIME / ELAPSEDTIME (+SyncInEditMode)
- [ ] CONTROLOBJECT (panel items, bones, morphs, `(self)`, `(OffscreenOwner)`, missing defaults)
- [ ] by-name uniforms
- [ ] mouse (constant), MMM (zero + diag)

### Script
- [ ] RenderColorTarget0..3 / RenderDepthStencilTarget (persisting), ClearSet*, Clear
- [ ] ScriptExternal=Color (copy current frame + depth)
- [ ] Pass=, LoopByCount / LoopEnd / LoopGetIndex (nesting, guard)
- [ ] Draw=Geometry / Draw=Buffer (half-pixel shifted quad), class restrictions
- [ ] default scripts (technique without script, pass without script)

### Drawing
- [ ] MmeSkinPass (SDEF, morphs, add UVs, prev positions)
- [ ] technique selection (MMDPass, Subset, Use*, object_ss → object fallback, empty technique, native fallback)
- [ ] object draws inside ScenePass (MRT wrapper: linearised colour, normal with nt.z=-1, velocity)
- [ ] edge / shadow (ground) / zplot steps
- [x] MmeShadowMap + `register(s0)` binding
- [ ] pre effects before models; post effects after PostPass in object order
- [ ] RT path works; PT primary layer (raster MME layer composited over PT); offline GI per-iteration MME layer; post effects on every path incl. offline GI
- [ ] faithful mode (identity post)
- [ ] no MME active → bit-identical frame (A/B capture diff = 0 px)

### Scene / app
- [ ] `XFileLoader` (text + binary .x: mesh, normals, UVs, materials, textures)
- [ ] effect objects (panel values, visibility, order), studio outliner + inspector
- [ ] accessory key track drives panel values
- [ ] `.emd` / same-name `.fx` / `[fx]` suggestion toast; per-material assignment
- [ ] PMM import: MME effect objects + assignments ("MME 그대로" vs native)
- [ ] project save/load (`.mmdxproj` "mme")
- [ ] library `effects/` category, shader manager MME tab with diagnostics
- [ ] drag & drop `.fx` / `.x`
- [ ] CLI: `--mme`, `--mme-model`, `--mme-faithful`; ui-script `studiomme ...`; MCP tool exposure
- [ ] hot reload on `.fx` change

### QA
- [ ] parse / compile census on library + pack
- [ ] per-effect render run, contact sheet, compat_list.md tiers updated
- [ ] studio E2E (add effect object, key Si/Tr, save/load)
- [ ] dark / light theme UI captures
