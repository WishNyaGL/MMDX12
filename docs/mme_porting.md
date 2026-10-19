# Porting an MME effect to MMDX12

This guide is for effect authors who have an MME `.fx` (or a small folder of them) and want a native MMDX12 effect
instead. It maps the MME semantics you already use onto the shader-pack API, shows four worked examples, and ends with
a checklist. The API references are `docs/shader_effect_api.md` (effect packs), `docs/shader_pt_api.md` (PT / GI),
`docs/effect_ecosystem.md` (direction), `shaders/effect_api.hlsli`, `shaders/pack_api.hlsli` and
`shaders/pack_inputs.hlsli`; the MME side is described in `docs/mme/mme_design.md` and
`docs/mme/research/mme_spec_draft.md`.

A port is a **rewrite against the pack API**, not a translation of D3D9 state. The app has no arbitrary scene
re-renders, no free off-screen passes and no script VM in the pack format, so some effects become simpler, some
become different, and a few cannot be expressed at all.

## Licence first

Port only material you are allowed to port:

- Your own effects, or effects whose licence / author permits redistribution of derivatives (改変 and redistribution).
  Read the readme / `readme.txt` / licence block that ships with the effect. "Available on the internet" is not a
  licence.
- Credit the original author in the pack's `authors` and, when it helps, in the localized `description`. Keep the
  original effect name in the description so users can find the source.
- Never bundle game-extracted textures. Textures you do not own belong in the user's texture folder, declared in
  `pack.json` with `"origin": "user"` (see `docs/effect_ecosystem.md` §4); the pack then samples them through its
  pack textures and shows white when the user has not connected a folder.

If the licence is unknown, stop and ask the author. Do not ship the port "for private use" through the gallery.

## Decide the kind

| MME effect | MMDX12 form | Where |
| --- | --- | --- |
| Screen / post effect (`ScriptClass=scene`, `ScriptOrder=postprocess`) | **effect pack** (`"type": "effect"`) | `docs/shader_effect_api.md` |
| Object / material shader (`object`, `object_ss`, `edge`) | **surface pack** (`"type": "surface"`, default) | `docs/shader_effect_api.md` / `pack_api.hlsli` |
| Object shader that must follow in PT / offline GI | surface pack **+ `pt_surface.hlsl`** | `docs/shader_pt_api.md` |
| `.x` accessory particles / world props | **world effect** (coming; `docs/shader_world_api.md`) | `docs/effect_ecosystem.md` §7 |
| Controller-driven parameters (`CONTROLOBJECT` morphs) | **pack params**, keyable on the studio timeline | `docs/effect_ecosystem.md` §2 item 1 |

**Post or pre-bloom?** An MME post effect was authored against MMD's back buffer: display-referred, gamma space,
after everything. That is the pack `"stage": "post"` (sRGB, after tonemap and the colour LUT). Use `"pre-bloom"` only
when the port genuinely needs linear HDR (glow, exposure, physically scaled blur) and you accept that it runs before
the bloom. Do not move an MMD colour-grade to `pre-bloom`: its constants were tuned for display values.

**Surface pack or effect pack?** If the effect shades a character / stage material, it is a surface pack and it runs
per pixel of that model, in raster and RT, and through `pt_surface.hlsl` in PT / GI. If it only reads the finished
image, it is an effect pack. An MME effect that does both (a material shader plus a post pass) becomes two packs.

**Accessories are not ported 1:1.** A `.x` accessory with its own `.fx` (snow, dust, magic circles) is a world effect:
a placed, keyable entity whose `world.hlsl` names its own spawn / simulate / shade functions. That API is specified in
`docs/shader_world_api.md` (the built-in "snow" world effect is the reference); until it ships, keep the MME path for those effects.

## Mapping table

| MME | Pack replacement | Caveats |
| --- | --- | --- |
| `TIME` | `i.time` (`PackEffectInput` / `PackPassInput`), `gTime` in surface / pass code | Scene time in seconds, not wall clock. Studio seeking and offline renders reproduce it. |
| `ELAPSEDTIME<SyncInEditMode>` | `i.dt` / `PackStateInput::dt` | Per rendered frame; fixed `1/60` in deterministic runs, `1/fps` in offline video, `1/60` per state step in GI stills. Never assume 60 Hz. |
| `VIEWPORTPIXELSIZE` | `i.outputSize` (effect), `i.size` (pass), `s.pixel` + `gViewportSize` (surface) | The output resolution, not the MME render target. `PackPassSize(n)` gives a pass's own size. |
| `CONTROLOBJECT` `item = "X"/"Y"/"Z"/"Rx"/...` | pack **params** (`PackParam(i)`, keyed on the timeline) | Controllers were an animation channel; params are the native equivalent. Values are static in play mode / Look presets, keyed in the studio. |
| `CONTROLOBJECT` bone position / matrix | typed scene inputs: `"inputs": ["performer.head", ...]` → `PackInputValid`, `PackInputPos`, `PackInputFrame` | Roles resolve through the humanoid map; no bone-name strings in the pack. Roles exist for the center performer's head, center, hands, feet, plus `light.key` and `camera.focus`. |
| `CONTROLOBJECT` morph weight | pack param, or an audio / scene input for automatic motion | There is no morph read. Convert the morph's animation to timeline keys on the param. |
| `RENDERCOLORTARGET` + `ScriptExternal` / multi-pass scripts | v5 `"passes"` (`float4 Entry(PackPassInput i)`, `PackPassSample(n, uv)`, `PackPassLoad`, `PackPassSize`) | Up to 8 passes, 1/64 .. 1 scale, RGBA16F. `ScriptExternal` (draw the scene again) has no equivalent: the effect sees the frame that is already there (`gEffectSource`, depth / normal / velocity). |
| multi-pass effects that need the previous frame | `"passes": [{ ..., "history": true }]` (capability `passes.history`) | The pass's previous-frame result is kept with the same reset / advance rules as v4 state (declared in `src/render/ShaderPack.h`, `passes.history`). The sampling helper is not in `effect_api.hlsli` yet, so check `pack_check --compile` on the app version you target and keep a one-frame-only fallback. |
| mip chains / blurred levels in a script | `"mips": true` on a pass (capability `passes.mips`) | The pass builds a mip chain after it runs. The explicit-LOD sampling helper is still landing; use an explicit downsample pass (`scale`) plus `PackPassSample` today. |
| `RENDERDEPTHSTENCILTARGET` / `ScriptExternal` depth | `PackEffectInput::depth` (raw device depth), `gEffectDepth`, `LinearZ(depth)` | Read-only. `depth = 1` is the background. There is no depth-stencil target to write. |
| `OFFSCREENRENDERTARGET` masks (AutoLuminous, M4Layer, ObjectLuminous) | redesigned on `"requires": ["inputs.id"]` + surface `PackResult::tag` (0..15) | **Do not translate 1:1.** The ID target and its reader are planned (`docs/effect_ecosystem.md` §2 item 5), not part of the current API. Until then, pick the objects you need in a surface pack instead of re-rendering the scene. |
| `WORLDVIEWPROJECTION`, `WORLD`, `VIEW`, `PROJECTION` (+ inverse / transpose) | `gView`, `gProj`, `gViewProj` in `common.hlsli`; `gEyePos`, `gLightDir` | Surface code also has `s.worldPos`, `s.N`, `s.V`, `s.L`, `s.viewZ`. There is no separate MME light camera. |
| `POSITION` / `DIRECTION` (Camera / Light) | `gEyePos`, `gLightDir` | Same values for every pass. |
| `MaterialDiffuse` / `DIFFUSE` | `PackDiffuse()`, `PackAmbient()`, `PackSpecular()`, `PackSpecularPower()`, `PackEdgeReflectivity()` | Gamma-space, after material morphs, like MMD. |
| `MaterialTexture` (`use_texture`) | `PackSurface::tex` (gamma space, morph factors applied), `PackParam` for strength | `s.tex` is white when the material has no texture. |
| sphere map (`use_spheremap`, add / multiply) | `PackSphereMode()`, `PackSampleSphere(s.N)`; Sekai UV-mapped slots: `PackHasSphereTex()`, `PackSampleSphereUv(uv)` | `PackSampleSphere` is the MMD view-space lookup. `PackSampleSphereUv` is only the Project Sekai `_H` layout. |
| toon (`use_toon`) | `PackHasToon()`, `PackSampleToon(v)` (v = 0 lit .. 1 shadow); UV-mapped: `PackHasToonMap()`, `PackSampleToonUv(uv)` | A material without a toon returns white, so guard with `PackHasToon` if the look depends on it. |
| self shadow (`object_ss`, `register(s0)` depth map) | `PackSurface::shadow` (0 shadowed .. 1 lit) and the host's lighting helpers | Do not sample MMD's shadow buffer. The host already composited cascades / RT shadows; `PackMmdLit(s)` starts from MMD's colour model. |
| `sampler_state` + `tex2D` | `PackSampleTex(i, uv)` / `PackSampleTexLevel` (surface), `PackFxSampleTex` / `PackFxSampleTexLevel` (effect), or `gLinear` / `gPoint` / `gClamp` / `gWrap` with `SampleLevel` | Packs sample explicitly; no implicit `Sample` in PT. Address mode is declared per texture in `pack.json` (`"address": "wrap"` or `"clamp"`). sRGB textures already return linear values: do not call `SrgbToLinear` again. |
| `ANIMATEDTEXTURE` (gif / APNG) | **not supported** | Bake the frames into a sprite sheet or convert the animation to a sequence of params; a single static texture is the supported case. |
| `MMM_*` (multi-light arrays, GPU skinning, `MMM_UnpackDepth`, toon / shadow samplers) | **not supported** | MMDX12 runs MMD semantics; the MMM extension branch does not exist. Use the pack's scene / audio inputs instead of MMM globals. |
| `TEXTUREVALUE`, `LEFTMOUSEDOWN` / mouse semantics | **not supported** | No texture readback and no live mouse in the pack contract. |

### Depth and normals

`PackEffectInput` gives the same G-buffer the engine uses:

- `i.depth` — raw device depth (`1` = background); `LinearZ(i.depth)` converts it to view-space z, and `gNearZ` /
  `gFarZ` are in `common.hlsli`.
- `i.normal` — view-space normal, oct-encoded (the normal target). It is defined on the background as a stray
  direction, so gate normal work on `i.depth < 1`.
- `i.motion` — `uv(current) - uv(previous)`; zero in offline GI.

### Samplers and textures

Declare your textures in `pack.json`:

```json
"textures": [
  { "file": "textures/grain.png", "address": "wrap", "srgb": false },
  { "file": "textures/mask.png",  "address": "clamp" }
]
```

Effect packs sample them with `PackFxSampleTex(i, uv)` (sRGB textures return linear values), surface packs with
`PackSampleTex`. A missing file samples white; it never crashes the pack. At most 16 textures, 4096 px and 32 MB.

## Worked examples

These are patterns, not ports of any existing effect. They compile against the current API and show the shape of a
conversion.

### 1. A vignette post effect

MME version: a `sampler_state` and a `tex2D` on the back buffer, with a `TIME`-driven pulse. Native version: an
effect pack, `"stage": "post"`, two params.

`pack.json`:

```json
{
  "format": 1,
  "id": "example_vignette",
  "version": "0.1.0",
  "apiVersion": 3,
  "type": "effect",
  "stage": "post",
  "name": { "en": "Example Vignette" },
  "description": { "en": "A porting-guide example." },
  "authors": [{ "name": "your_name" }],
  "license": "MIT",
  "params": [
    { "key": "strength", "label": { "en": "Strength" }, "default": 0.6, "min": 0.0, "max": 1.0 },
    { "key": "radius",   "label": { "en": "Radius" },   "default": 0.45, "min": 0.0, "max": 1.0 }
  ]
}
```

`effect.hlsl`:

```hlsl
float3 PackEffect(PackEffectInput i) {
    float2 d = i.uv - 0.5;
    d.x *= i.outputSize.x / i.outputSize.y;      // VIEWPORTPIXELSIZE aspect, not MMD's back-buffer size
    float v = smoothstep(PackParam(1), PackParam(1) + 0.5, length(d) * 1.4142);
    float amount = PackParam(0) * (0.85 + 0.15 * sin(i.time * 2.0));
    return i.color.rgb * (1.0 - amount * v);
}
```

The pulse that MME drove with `TIME` is now `i.time`; if it was driven by a controller morph, make it a third param
and key it on the timeline instead.

### 2. A two-pass blur with history

MME version: `RenderColorTarget` + a loop, or `ScriptExternal` with an accumulation target. Native version: v5
passes, and history when the previous frame is part of the result.

```json
{
  "apiVersion": 5,
  "type": "effect",
  "stage": "post",
  "passes": [
    { "entry": "Downsample", "scale": 0.25 },
    { "entry": "BlurH", "scale": 0.25 },
    { "entry": "BlurV", "scale": 0.25 },
    { "entry": "Accumulate", "scale": 1.0, "history": true }
  ]
}
```

```hlsl
float4 Downsample(PackPassInput i) { return gEffectSource.SampleLevel(gLinear, i.uv, 0); }

float4 BlurH(PackPassInput i) {
    float2 px = 1.0 / i.size;
    float4 c = 0;
    [unroll] for (int k = -4; k <= 4; ++k) c += PackPassSample(0, i.uv + float2(k * px.x * 2.0, 0));
    return c / 9.0;
}

float4 BlurV(PackPassInput i) {
    float2 px = 1.0 / i.size;
    float4 c = 0;
    [unroll] for (int k = -4; k <= 4; ++k) c += PackPassSample(1, i.uv + float2(0, k * px.y * 2.0));
    return c / 9.0;
}

float4 Accumulate(PackPassInput i) {
    float4 blurred = PackPassSample(2, i.uv);
    float4 source = gEffectSource.SampleLevel(gLinear, i.uv, 0);
    // A "history": true pass keeps its previous-frame result for temporal blending (capability
    // "passes.history"); this example stays one-frame: mix the blur with the source.
    return lerp(source, blurred, PackParam(0));
}

float3 PackEffect(PackEffectInput i) { return PackPassSample(3, i.uv).rgb; }
```

A pass may read only earlier passes; `PackEffect` may read all. The `history` flag needs `"requires":
["passes.history"]`; it keeps that pass's previous-frame result with the same reset rules as v4 state (the sampling
helper is not yet in `effect_api.hlsli`). If the app version you target lacks it, keep the result inside one frame
(blur only) rather than faking persistence with a jittered sample. `"mips": true` (capability `passes.mips`) builds a
mip chain after the pass; use an explicit downsample pass (`scale`) until its explicit-LOD helper is available.

### 3. A controller-driven fade replaced by a keyable param

MME version: a `CONTROLOBJECT` morph weight drives the opacity of an overlay. Native version: one effect param, keyed
on the studio timeline (`RowKind::EffectParam`, `studioeffectkey <index> <param key> <frame> <value>`).

```hlsl
float3 PackEffect(PackEffectInput i) {
    float fade = saturate(PackParam(0));              // 0 = transparent, 1 = full effect
    float3 graded = /* the effect's own image work */ i.color.rgb;
    return lerp(i.color.rgb, graded, fade);
}
```

Convert the morph's VMD track to keys on the param once, in the studio: the result is editable, undoable and
deterministic in video / GI renders. A rhythm that used to come from a morph can also come from the audio inputs:
`"requires": ["inputs.audio"]`, `"inputs": ["audio"]`, then `PackAudioOnset()` / `PackAudioEnergy()`.

### 4. A material rim light as a surface pack

MME version: an `object` technique that adds `pow(1 - dot(N, V), p)` to the material colour, often with a toon
term. Native version: `surface.hlsl` implementing `PackShade`, and a `pt_surface.hlsl` so the look survives PT / GI.

`surface.hlsl`:

```hlsl
PackResult PackShade(PackSurface s) {
    PackResult r;
    float3 base = PackMmdLit(s);                          // MMD ambient + diffuse * texture
    float rim = pow(saturate(1.0 - dot(s.N, s.V)), PackParam(1));
    r.color = base + PackParam(0) * rim * s.tex.rgb;
    r.alpha = s.alpha;
    r.reflectivity = 0.0;
    r.noAo = false;
    return r;
}
```

`pt_surface.hlsl` (the same idea, as an additive term):

```hlsl
PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    o.albedo = i.baseColor;
    o.shadowTint = float3(1, 1, 1);
    o.shadowBias = 0.0;
    o.terminator = float2(-0.12, 0.22);
    float rim = pow(saturate(1.0 - dot(i.normal, i.V)), i.params[1]);
    o.specular = i.params[0] * rim * i.baseColor * i.sunVis;
    o.flatFace = false;
    return o;
}
```

Assign every field of `PtPackOut`; `pack_check` warns when one is left unset. The MME self-shadow buffer is replaced
by `s.shadow` in raster and by `i.sunVis` in PT.

## Checklist

- [ ] **Licence** checked, original author credited in `authors` / `description`, no game-extracted textures bundled
      (`"origin": "user"`).
- [ ] `tools/pack_check <folder> --compile` passes for every entry point: `PackEffect` / `PackEffectState`, every
      `passes` entry, and `PackEvaluate` when `pt_surface.hlsl` exists.
- [ ] Before / after preview: `preview_before.png` is the same frame with the pack disabled, `preview.png` with it on
      (same size and framing).
- [ ] Disabled = identical frame: no effect in the stack means no targets and no passes; a surface pack left unselected
      changes nothing.
- [ ] All render paths checked: raster, RT, PT, offline GI. A post effect runs on the final image in every path; a
      pre-bloom pack runs on the lit HDR before the bloom. No effect in unlit / wireframe / quad views.
- [ ] No `[E]` / `[W]` lines from the pack in `mmdx12.log`, and the capture reached the scene (`motion bound` +
      `STARTUP first frame`).
- [ ] Constants re-tuned for the pack's space: an MMD gamma-space look belongs in `post`; linear terms belong in
      `pre-bloom`.
- [ ] No claimed exact parity. Say "based on" / "inspired by", and list the deliberate differences in the description.
