# Shader pack effects (API 3, 4, 5)

A shader pack with `"type": "effect"` is a whole-screen effect (chromatic aberration, film grain, CRT, glitch, custom
vignette ...). Users stack effects in the shader manager and the play bar's effect button; the stack runs top to bottom on
the final image and is used by the play view, the lobby preview and real-time video / still renders. Surface packs
(`"type": "surface"`, the default) are unchanged.

## Folder

```
my_effect/
  pack.json      manifest (same fields as surface packs, plus type / stage)
  effect.hlsl    implements PackEffect
  preview.png    optional
  preview_before.png optional (same size/framing, effect disabled; with preview.png shows a before/after compare)
  textures/      optional, pack.json "textures" (same rules as surface packs, API 2)
```

`pack.json` additions: `"apiVersion": 3`, `"type": "effect"`, `"stage": "post" | "pre-bloom"` (default `post`).
`classes` are not used. `params` (at most 16) work exactly like surface packs; values reach the shader as `PackParam(i)`.

| stage | input / output | use for |
| --- | --- | --- |
| `post` | display-referred sRGB (after tonemap and the colour LUT), 8-bit | grain, scanlines, lens fringes, vignette |
| `pre-bloom` | linear HDR RGBA16F, before the bloom | glow-friendly or exposure-aware effects |

Effects of both stages can be mixed in one stack: each stage runs its own effects in stack order.

## effect.hlsl

```hlsl
float3 PackEffect(PackEffectInput i) { return i.color.rgb; }
```

`PackEffectInput`: `uv` (0..1, top left = 0,0), `pixel`, `outputSize`, `time` (the frame's scene / timeline seconds; constant while the timeline is paused), `frameIndex`, `color` (the frame so
far, alpha kept), `depth` (raw device depth, 1 = background), `normal` (view space, oct-encoded; undefined on the background), `motion`
(uv(current) - uv(previous)).

Helpers: `gEffectSource.SampleLevel(gLinear, uv, 0)` reads any pixel of the frame so far (bilinear, clamp), `PackParam(i)`,
`PackFxSampleTex(i, uv)` / `PackFxSampleTexLevel` / `PackFxTexSize` / `PackFxTexCount` (pack textures: sRGB textures return
linear values), plus everything in `common.hlsli` (`Luminance`, `LinearZ`, `Ign`, `SrgbToLinear`, `gTime` ...). `line` is a
reserved word in HLSL.

The shader is compiled at runtime with DXC (`ps_6_0`); a compile error skips the effect (log `[E]`, toast, status in the
shader manager) and never crashes. Saving `effect.hlsl` while the app runs reloads it.

## Rules

- Effects apply to the raster / ray-traced / path-traced real-time views and to the offline GI renderer (4K stills and GI
  videos). Unlit / wireframe views and the quad view do not apply them. Offline GI: a pre-bloom pack runs on the lit HDR
  image (the denoised radiance re-lit, volumetric light and outlines composed in) before the bloom, and the bloom reads its
  output; a post pack runs on the final sRGB image. `depth` and `normal` come from the offline G-buffer, `motion` is zero
  (the offline renderer has no per-pixel motion vectors; its motion blur is integrated over the shutter).
- No effects in the stack = no extra targets, no extra passes: the frame is identical to a build without the feature.
- Check a pack with `pack_check <folder> --compile`. Template: `shaders/pack_template_effect`.
- Examples (online gallery only, not bundled): chromatic_aberration, film_grain, crt_scanlines, auto_luminous (sources in the website repo `shader-packs/`).
- CLI: `--effect <id>[,<id>...]` replaces the stack for one run, `--effect none` clears it.

## API v4: persistent state

API v4 introduces optional persistent per-entry state (up to 16 floats) for temporal work like eye adaptation, exposure or focus smoothing, and cumulative effects.

### Manifest

In `pack.json`, declare `"apiVersion": 4`, `"type": "effect"`, and add the `"state"` block:

```json
{
  "apiVersion": 4,
  "type": "effect",
  "stage": "post",
  "state": {
    "floats": 4
  }
}
```

- `"floats"` must be an integer between 1 and 16.
- A pack that declares `"state"` must have `"apiVersion": 4` and `"type": "effect"`. Older versions of the app reject or ignore unknown fields, preventing stateful packs from silently running stateless.
- Stateless effects and surface packs can continue using `"apiVersion": 3`.

### HLSL Contract

State packs implement `PackEffectState` in addition to `PackEffect`:

```hlsl
void PackEffectState(PackStateInput i, out float newState[16]) {
    for (int k = 0; k < 16; ++k) newState[k] = 0;
    float centerLuma = Luminance(gEffectSource.SampleLevel(gLinear, float2(0.5, 0.5), 0).rgb);
    newState[0] = i.reset ? centerLuma : lerp(i.prevState[0], centerLuma, 1.0 - exp(-PackParam(0) * i.dt));
    newState[1] = i.reset ? 0 : (i.prevState[1] + 1);
}

float3 PackEffect(PackEffectInput i) {
    float smoothLuma = PackState(0);
    return i.color.rgb * smoothLuma;
}
```

- `PackStateInput`:
  - `float prevState[16]`: previous state values (all zeros when `reset` is true).
  - `bool reset`: true if state was reset this frame.
  - `float dt`: delta time in seconds.
  - `float time`: the frame's scene / timeline seconds (constant while paused).
  - `float frameIndex`: current frame number.
  - `float2 outputSize`: dimensions of the render target.
- `PackState(i)` reads float `i` (0 <= i < stateFloats) of the state updated for this frame. For non-state packs (`PACK_STATE=0`), `PackState(i)` evaluates to `0.0`.
- `PackEffectInput` gains `float dt` appended as its last member.
- State is stored in a 4x1 RGBA32F ping-pong texture (`gEffectState`, `t0, space7`).

### Reset, dt, and advance rules

- **Time**: `time` (in `PackEffectInput`, `PackStateInput` and `PackPassInput`) is the scene / timeline seconds of the
  frame being rendered (`FrameView::sceneTime`), not a wall clock. The play view, the studio timeline, real-time video
  renders and the offline GI job all pass the rendered frame's time; a paused timeline gives a constant time. It is
  separate from `gTime` / `SceneConstants::time` (still the render clock), and `frameIndex` is unchanged.
- **Reset**: `reset` is true when:
  - The stack entry is created, reordered, or its pack reloads.
  - The entry did not run in the previous executed frame of the pass instance.
  - Camera cut / teleport (`FrameView::cameraCut`).
  - Seek / jump: the scene time moved backwards, or the frame's scene delta is more than 0.25 s.
  - The timeline is paused (every paused frame runs the still-render rule below).
  - Output size changed / window resized.
  - Explicitly requested by offline job or caller (`effectStateReset`).
  When `reset` is true (step 0 of the converge), `prevState` is guaranteed to be all zeros.
- **dt / advance**:
  - Timeline playing with 0 < `sceneElapsed` <= 0.25 s: one state step with `dt = sceneElapsed`. An explicit
    `FrameView::effectDt` (offline video `1/fps`, deterministic runs) overrides the dt value.
  - The same scene time rendered again while playing (`sceneElapsed` 0): no state step; `PackState` reads the latest
    state.
  - Timeline paused: exactly the still-render rule, every paused frame; reset on step 0, then 64 steps of `dt = 1/60`.
    Camera and parameter edits while paused are therefore reflected at once and the result is deterministic.
  - Seek / jump (`cameraCut`, |delta| > 0.25 s, backwards): reset + the same 64-step converge (`dt = 1/60`).
  - Path-traced video: state advances only on the last of the `iterCount` passes per frame (`effectStateAdvance` false
    beforehand).
  - Offline GI:
    - Videos: 1 state pass per output image with `dt = 1/fps`, resetting on first frame or jump cut. State slots persist across frames of the video.
    - Stills: runs the state pass 64 times on the same image with `dt = 1/60` (reset only on step 0) to converge temporal state before running `PackEffect` once.


## API v5: intermediate passes and the camera focus

API v5 lets an effect run several passes before `PackEffect` (downsample chains, blurs, gathers: lens flares, bokeh
depth of field, ambient occlusion) and gives every effect the engine's depth-of-field focus. A pack may also take
over the engine's depth of field.

### Manifest

```json
{
  "apiVersion": 5,
  "type": "effect",
  "stage": "pre-bloom",
  "passes": [
    { "entry": "Bright", "scale": 0.25 },
    { "entry": "Blur",   "scale": 0.0625 }
  ],
  "replaces": ["dof"]
}
```

- `passes` (at most 8): each entry names an HLSL function `float4 Name(PackPassInput i)` and the size of its target as
  a fraction of the output (`scale`, 1/64 .. 1, default 1). Passes run in order; each writes its own RGBA16F target.
- `replaces` (pre-bloom packs only): `["dof"]` = while the entry is enabled, the engine's depth of field (DofPass and
  the offline GI thin lens) is skipped and the pack draws it. No other values exist yet.
- `passes`, `replaces` and the focus helpers need `"apiVersion": 5`; `state` (v4) works together with them.

### HLSL

```hlsl
float4 Bright(PackPassInput i) { return float4(max(gEffectSource.SampleLevel(gLinear, i.uv, 0).rgb - 1.0, 0.0), 1); }
float4 Blur(PackPassInput i)   { return PackPassSample(0, i.uv); }   // pass 0's target, bilinear
float3 PackEffect(PackEffectInput i) { return i.color.rgb + PackPassSample(1, i.uv).rgb; }
```

- `PackPassInput`: `uv` and `pixel` of this pass's target, `size` (its pixels), `outputSize`, `time` (the frame's scene /
  timeline seconds), `frameIndex`,
  `dt`, `pass` (its index). The frame inputs (`gEffectSource`, depth, normal, velocity), the pack textures, the
  parameters and the state are bound in every pass.
- `PackPassSample(n, uv)` (bilinear, clamp), `PackPassLoad(n, pixel)`, `PackPassSize(n)` read pass `n`'s target. A pass
  may read the passes before it; reading its own target or a later one is undefined. `PackEffect` may read all.
- Focus (the studio focus track, play mode's character head, else a five-tap autofocus on the screen centre):
  `PackFocusZ()` = view-space z of the focus plane; `PackFocusAperture()` = the focus key's aperture scale (0 = sharp,
  1 default); `PackDofAperture()` / `PackDofMaxRadius()` = the user's global DoF aperture and max blur radius (output
  px); `PackCocPx(viewZ, focusZ, aperture, maxRadius)` = the engine lens model's signed circle of confusion in output
  px (negative = in front of the focus plane).
- Pass targets are pooled by size and shared by all entries; they exist only while an entry with passes is enabled.
  A pack without `passes` keeps the v3 / v4 root signature and PSO exactly (no extra cost, identical frame).

`pack_check --compile` compiles every pass (`PSPackPass` with `PACK_PASS_ENTRY`) and prints the passes and `replaces`.
Examples (online gallery, sources in the website repo `shader-packs/`): lens_dirt, lens_reflection, lens_flare,
bokeh_dof (replaces the depth of field, follows the focus track), screen_ao.

## Capabilities, scene and audio inputs

Packs declare capabilities in `"requires": [...]` and requested typed inputs in `"inputs": [...]`.
Capabilities require specific support from the engine; an incompatible app will refuse the pack with the capability name in its status line.

### Manifest (`pack.json`)

```json
{
  "requires": ["inputs.scene", "inputs.audio"],
  "inputs": ["performer.head", "performer.center", "light.key", "camera.focus", "audio"]
}
```

- `"inputs.scene"`: exposes typed scene and skeleton inputs (`PackInputValid`, `PackInputPos`, `PackInputFrame`).
- `"inputs.audio"`: exposes music analysis inputs (`PackAudioBeat`, `PackAudioDownbeat`, `PackAudioOnset`, `PackAudioEnergy`, `PackAudioSection`, `PackAudioBpm`).

### Scene role table

| Role Constant | `inputs` string | Description |
| --- | --- | --- |
| `PACK_IN_PERFORMER_HEAD` | `"performer.head"` | Center performer's head bone (頭) |
| `PACK_IN_PERFORMER_CENTER` | `"performer.center"` | Center performer's center bone (センター) |
| `PACK_IN_PERFORMER_HAND_L` | `"performer.hand_l"` | Center performer's left wrist bone (左手首) |
| `PACK_IN_PERFORMER_HAND_R` | `"performer.hand_r"` | Center performer's right wrist bone (右手首) |
| `PACK_IN_PERFORMER_FOOT_L` | `"performer.foot_l"` | Center performer's left ankle bone (左足首) |
| `PACK_IN_PERFORMER_FOOT_R` | `"performer.foot_r"` | Center performer's right ankle bone (右足首) |
| `PACK_IN_LIGHT_KEY` | `"light.key"` | Key light (direction as z axis) |
| `PACK_IN_CAMERA_FOCUS` | `"camera.focus"` | DoF focus point in world space, camera orientation axes |
| `PACK_IN_AUDIO` | `"audio"` | Music audio reactivity |

### Helpers

When `"inputs.scene"` is declared:
- `bool PackInputValid(uint role)`: true if the requested bone or scene role exists and is valid.
- `float3 PackInputPos(uint role)`: world-space position in MMD units.
- `float3x3 PackInputFrame(uint role)`: world orientation matrix (row 0 = right, row 1 = up, row 2 = forward).

When `"inputs.audio"` is declared:
- `float PackAudioBeat()`: 0..1 phase within the current beat.
- `float PackAudioDownbeat()`: 0..1 phase within the bar.
- `float PackAudioOnset()`: onset envelope, decaying with a ~0.25 s time constant.
- `float PackAudioEnergy()`: normalized RMS energy of the current beat (0..1).
- `float PackAudioSection()`: coarse song energy section (0.0 low / 0.5 mid / 1.0 high).
- `float PackAudioBpm()`: song tempo in BPM. All audio inputs return 0 when no audio is loaded or analysis is not ready.

## ID Target (`inputs.id`)

When an effect pack declares `"requires": ["inputs.id"]` (or `"inputs": ["id"]`), the engine provides a non-MSAA `R16_UINT` ID buffer at render resolution (`gEffectId`, `t4, space6`).
Pixels with alpha < 0.98 or belonging to the background clear to 0.

### Bit Layout

| Bits | Field | Description |
| --- | --- | --- |
| 0–3 | `tag` | 4-bit user tag (0..15) written by a surface pack via `PackResult.tag` / `PtPackOut.tag` |
| 4–6 | `class` | Material class (0..5: `PACK_BODY`, `PACK_SKIN`, `PACK_FACE`, `PACK_EYE`, `PACK_HAIR`, `PACK_WEAPON`; 7 = none / pack off) |
| 7 | `isCharacter` | 1 if character model, 0 if stage or other geometry |
| 8–15 | `model` | Model index (1 + model index, 0 = background or transparent) |

### HLSL Helpers

- `uint PackSampleId(float2 uv)`: loads the 16-bit ID at the specified UV coordinate.
- `uint PackIdModel(uint id)`: extracts the 1-based model index (`(id >> 8) & 0xFF`).
- `uint PackIdClass(uint id)`: extracts the material class (`(id >> 4) & 0x7`).
- `uint PackIdTag(uint id)`: extracts the custom tag (`id & 0xF`).
- `bool PackIdIsCharacter(uint id)`: returns true if bit 7 is set.

## Pass History and Mips (`passes.history`, `passes.mips`)

Intermediate passes in API v5 can opt into temporal persistence and mip chains.

### Pass History (`passes.history`)

When an effect declares `"requires": ["passes.history"]`, any pass entry can specify `"history": true`:

```json
{
  "apiVersion": 5,
  "type": "effect",
  "requires": ["passes.history"],
  "passes": [
    { "entry": "Accumulate", "scale": 1.0, "history": true }
  ]
}
```

- Each history pass retains its output from the previous frame in a dedicated history texture (`space9`).
- The history buffer automatically resets (cleared to zero) on camera cut, scene load/reset, or resolution change.
- Helpers:
  - `float4 PackPassSampleHistory(uint passIndex, float2 uv)`: bilinearly samples pass `passIndex`'s history target.
  - `float4 PackPassLoadHistory(uint passIndex, int2 px)`: point loads a texel from pass `passIndex`'s history target.

### Pass Mips (`passes.mips`)

When an effect declares `"requires": ["passes.mips"]`, any pass entry can specify `"mips": true`:

```json
{
  "apiVersion": 5,
  "type": "effect",
  "requires": ["passes.mips"],
  "passes": [
    { "entry": "DownsampleBase", "scale": 0.5, "mips": true }
  ]
}
```

- After the pass executes, the engine generates a full mip chain down to 1x1 using a box downsample compute shader.
- Helpers:
  - `float4 PackPassSampleLevel(uint passIndex, float2 uv, float lod)`: samples pass `passIndex`'s target at explicit mip level `lod`.

## Extended Parameters (`params.ext`)

Packs declaring `"requires": ["params.ext"]` in `pack.json` can declare up to 128 parameters (`kPackMaxParamsExt = 128`), organized in collapsible UI groups and supporting multiple types beyond scalar floats.

### Manifest

```json
{
  "requires": ["params.ext"],
  "groups": [
    { "id": "general", "label": { "ko": "일반", "en": "General" }, "collapsed": false },
    { "id": "color_adj", "label": { "ko": "색상 조정", "en": "Color Adjustments" }, "collapsed": true }
  ],
  "params": [
    { "key": "strength", "type": "float", "group": "general", "label": { "ko": "강도", "en": "Strength" }, "default": 0.5, "min": 0.0, "max": 1.0, "step": 0.01 },
    { "key": "iterations", "type": "int", "group": "general", "label": { "ko": "반복 횟수", "en": "Iterations" }, "default": 4, "min": 1, "max": 16, "step": 1 },
    { "key": "invert", "type": "bool", "group": "general", "label": { "ko": "반전", "en": "Invert" }, "default": false },
    { "key": "tint", "type": "color", "group": "color_adj", "label": { "ko": "색조", "en": "Tint" }, "default": [1.0, 0.8, 0.6] },
    { "key": "overlay", "type": "color", "group": "color_adj", "alpha": true, "label": { "ko": "오버레이", "en": "Overlay" }, "default": [1.0, 1.0, 1.0, 0.5] },
    { "key": "offset", "type": "vec3", "group": "color_adj", "label": { "ko": "오프셋", "en": "Offset" }, "default": [0.0, 0.0, 0.0], "min": -1.0, "max": 1.0, "step": 0.05 }
  ]
}
```

- **Types**:
  - `float` (default): single scalar float slider.
  - `int`: integer slider with discrete stepping.
  - `bool`: toggle switch.
  - `color`: RGB color picker button with popup (sRGB on UI and JSON, automatically converted to linear color in shaders). Set `"alpha": true` for an additional alpha opacity component.
  - `vec3`: 3-component float vector with independent axis sliders.
- **Groups**: collapsible sections in the UI. Collapsed state is remembered per pack and group in settings.
- **Limits**: legacy packs support up to 16 float parameters. Packs with `"params.ext"` support up to 128 parameters.

### HLSL Helpers

When `params.ext` is declared, the macro `PACK_CAP_PARAMS_EXT` is defined during compilation, and parameters are backed by a float4 constant buffer (`gPackParamSlots[128]`):

- `float PackParam(uint index)`: returns slot `index.x` (compatible with legacy float indexing).
- `int PackParamInt(uint index)`: returns integer value from slot `index.x`.
- `bool PackParamBool(uint index)`: returns boolean flag (`> 0.5`) from slot `index.x`.
- `float3 PackParamColor(uint index)`: returns linear RGB color from slot `index.rgb`.
- `float4 PackParamColor4(uint index)`: returns linear RGBA color from slot `index.rgba`.
- `float3 PackParamVec3(uint index)`: returns 3D vector from slot `index.xyz`.
- `uint PackParamCount()`: total declared parameter count.


