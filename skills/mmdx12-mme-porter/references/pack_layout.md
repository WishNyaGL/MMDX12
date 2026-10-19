# Pack layout reference

Manifest fields below are the ones `src/render/ShaderPack.cpp` parses. Anything else is ignored or reported as a
problem; do not add speculative keys.

## `pack.json` — effect pack

```json
{
  "format": 1,
  "id": "example_vignette",
  "version": "0.1.0",
  "apiVersion": 3,
  "type": "effect",
  "stage": "post",
  "name": { "en": "Example Vignette", "ko": "예시 비네트" },
  "description": { "en": "Based on ... by ...", "ko": "..." },
  "authors": [
    { "name": "your_name" },
    { "name": "original_author", "role": "original effect" }
  ],
  "license": "MIT",
  "homepage": "https://...",
  "tags": ["post", "vignette"],
  "textures": [
    { "file": "textures/grain.png", "address": "wrap", "srgb": false }
  ],
  "params": [
    { "key": "amount", "label": { "en": "Amount", "ko": "세기" }, "default": 0.5, "min": 0.0, "max": 1.0 }
  ]
}
```

- `id`: 1-64 chars of lower-case `a-z`, `0-9`, `_` or `-`; the manifest id is used, the folder name is only the fallback.
- `version`: `major.minor.patch`.
- `apiVersion`: `3` plain effect, `4` with `"state": { "floats": N }` (`1..16`), `5` with `"passes"` /
  `"replaces"`, `6` for current capabilities / surface output.
- `type`: `effect` for whole-screen, `surface` (or omitted) for material shading.
- `stage`: `post` (default; display-referred sRGB) or `pre-bloom` (linear HDR).
- `authors`: array of `{ "name", "role", "url" }` or plain strings; a name is required.
- `license`: a string; say `"unknown"` only when the source licence really is unknown (then you should have stopped).
- `textures`: at most 16; `png` / `jpg` / `jpeg` inside the pack; `address` is `wrap` (default) or `clamp`; `srgb`
  defaults to `true` and samples as linear values. Files that cannot be redistributed belong in the user's texture
  folder (declared with `"origin": "user"`), not in the pack.
- `params`: at most 16; unique keys; `min` <= `max`; `default` is clamped into range. The label is a string or a
  localized object. **There is no `"groups"` field in this manifest version** — the group capability in
  `docs/effect_ecosystem.md` §7 (`params.ext`) is planned, not implemented. Do not emit it; order the params so
  related controls are adjacent.
- `requires`: declare only the capabilities the shader uses; names are listed in
  `src/render/ShaderPack.h` (`state`, `passes`, `passes.history`, `passes.mips`, `inputs.scene`, `inputs.audio`, ...).
- `inputs`: role strings from the table in `docs/shader_effect_api.md` /
  `docs/shader_pt_api.md` (`performer.head`, `performer.center`, `performer.hand_l`, `performer.hand_r`,
  `performer.foot_l`, `performer.foot_r`, `light.key`, `camera.focus`, `audio`).

API 5 additions:

```json
"passes": [
  { "entry": "Bright", "scale": 0.25 },
  { "entry": "Blur",   "scale": 0.125, "history": true, "mips": true }
],
"replaces": ["dof"]
```

- `passes`: at most 8; `scale` is `1/64 .. 1` of the output; `history` needs `passes.history`, `mips` needs
  `passes.mips`.
- `replaces`: only `["dof"]`, pre-bloom packs only.
- `state`: `{ "floats": N }` needs `apiVersion` 4 or higher; write `PackEffectState` and read with `PackState(i)`.

## `effect.hlsl`

The host includes `effect_api.hlsli` and calls one function:

```hlsl
float3 PackEffect(PackEffectInput i) {
    return i.color.rgb;
}
```

Optional state (API 4+):

```hlsl
void PackEffectState(PackStateInput i, out float newState[16]) {
    for (int k = 0; k < 16; ++k) newState[k] = 0;
    newState[0] = i.reset ? 0 : i.prevState[0] + i.dt;
}
```

Optional pass entry points (API 5), one per `passes[].entry`, `float4` out:

```hlsl
float4 Bright(PackPassInput i) {
    return float4(max(gEffectSource.SampleLevel(gLinear, i.uv, 0).rgb - 1.0, 0.0), 1);
}
```

Details: `i.uv`, `i.pixel`, `i.outputSize`, `i.time`, `i.frameIndex`, `i.color`, `i.depth`, `i.normal`, `i.motion`,
`i.dt`; `PackParam(i)`, `PackFxSampleTex` / `PackFxSampleTexLevel` / `PackFxTexSize` / `PackFxTexCount`; `gTime`,
`LinearZ`, `Luminance`, `Ign`, `SrgbToLinear` from `common.hlsli`.

## `surface.hlsl`

```hlsl
PackResult PackShade(PackSurface s) {
    PackResult r;
    r.color = PackMmdLit(s);
    r.alpha = s.alpha;
    r.reflectivity = 0.0;
    r.noAo = false;
    return r;
}
```

- Inputs: `s.worldPos`, `s.N`, `s.V`, `s.L`, `s.uv`, `s.uv1`, `s.uv2`, `s.pixel`, `s.viewZ`, `s.tex`, `s.alpha`,
  `s.shadow`, `s.materialClass`, head frame fields.
- Helpers: `PackParam`, `PackDiffuse`, `PackAmbient`, `PackSpecular`, `PackSpecularPower`, `PackEdgeReflectivity`,
  `PackMmdLit`, `PackSphereMode`, `PackSampleSphere`, `PackSampleToon`, `PackHasToon`, `PackHasSphereTex`,
  `PackSampleSphereUv`, `PackHasToonMap`, `PackSampleToonUv`, `PackSampleTex` / `PackSampleTexLevel` / `PackTexSize` /
  `PackTexCount`.
- `PackResult`: `color` (linear HDR), `alpha`, `reflectivity`, `noAo`. With the `surface.output` capability it also has
  `normal`, `emission` and `tag` (0..15).
- Optional outlines: `#define PACK_HAS_EDGE 1` and `PackEdgeResult PackEdge(uint materialClass, float4 mmdEdgeColor,
  float mmdEdgeSize)`.

## `pt_surface.hlsl`

```hlsl
PtPackOut PackEvaluate(PtPackIn i) {
    PtPackOut o;
    o.albedo = i.baseColor;
    o.shadowTint = float3(1, 1, 1);
    o.shadowBias = 0.0;
    o.terminator = float2(-0.12, 0.22);
    o.specular = float3(0, 0, 0);
    o.flatFace = false;
    return o;
}
```

Assign every field. `PtPackIn` gives `pos`, `normal`, `V`, `uv`, `uv1`, `uv2`, `L`, `sunVis`, `baseColor`,
`materialClass`, `params[16]` and the head frame. Lighting controls (`PackEvaluateLighting`, `PT_PACK_API_LIGHTING`)
are optional; see `docs/shader_pt_api.md`.

## Verify commands

```
build\bin\pack_check.exe <pack folder> --compile
timeout 120 build\bin\MMDX12.exe --character <name> --stage none --song <name> --autoplay \
    --seek 20 --paused --no-physics --frames 60 --effect <id> --capture out.png
```

For a surface pack use `--shader-pack <id>` instead of `--effect`. Then read `build/bin/mmdx12.log`: the scene is
reached when `motion bound` and `STARTUP first frame` are present; grep `[E]` / `[W]`. Never run two instances at once.
