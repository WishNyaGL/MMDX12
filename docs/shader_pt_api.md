# PT/GI shader pack hook (phase 1: offline GI stills, phase 2: real-time path tracer)

A surface shader pack can optionally provide `pt_surface.hlsl` next to `surface.hlsl` for the offline GI path tracer (`CSRender`) and the real-time path tracer (`CSPathTrace`, `PathTracePass`, `--render pt`).
When present, characters using the pack will evaluate `PackEvaluate` during offline GI still rendering and real-time path tracing instead of the default `ToonSun` shading.
If `pt_surface.hlsl` is omitted, the pack uses the path tracer's default shading. Real-time raster and RT are unaffected.

## File placement

```
my_pack/
  pack.json        manifest
  surface.hlsl     raster / real-time RT (PackShade)
  pt_surface.hlsl  offline GI / real-time PT (PackEvaluate)
```

No `pack.json` change or `apiVersion` bump is needed.

## pt_surface.hlsl

```hlsl
PtPackOut PackEvaluate(PtPackIn i);
```

### `PtPackIn`

- `float3 pos`: world position
- `float3 normal`: shading normal
- `float3 V`: view direction toward the camera
- `float2 uv`: texture coordinates
- `float3 L`: direction towards the sun/key light
- `float sunVis`: 0..1 sun visibility from the traced shadow ray (0 on diffuse bounces)
- `float3 baseColor`: linear base colour (`SrgbToLinear(texture * diffuse)`)
- `uint materialClass`: pack material class (`PACK_BODY`, `PACK_SKIN`, `PACK_FACE`, `PACK_EYE`, `PACK_HAIR`, `PACK_WEAPON`)
- `float params[16]`: 16 pack sliders
- Head frame: `float3 headPos` (world position of the head bone), `float headScale` (world units per model unit), `float3 headRight`, `float3 headUp`, `float3 headForward`, `bool headValid`. A stage part's material has no head bone: `headPos` / `headScale` / `headRight` / `headUp` / `headForward` are 0 and `headValid` is false there, so a pack must fall back to its non-face path when `headValid` is false

### `PtPackOut`

- `float3 albedo`: linear surface albedo used for GI diffuse bounces and reflections
- `float3 shadowTint`: linear multiplier on the shaded side of the terminator
- `float shadowBias`: shifts the terminator, in N.L units. A pack that shades with a smoothed normal (faces) returns `dot(Nsmooth, L) - dot(normal, L)`
- `float2 terminator`: (lo, hi) N.L edges of the terminator smoothstep. `hi <= lo` (e.g. `float2(0, 0)`) = the engine default (-0.12, 0.22). A half-Lambert ramp `threshold +- softness` maps to `2*(threshold -+ softness) - 1`.
- `float3 specular`: additive linear radiance (rim, matcap, highlights)
- `bool flatFace`: whether to use the engine's flat face handling (no GI gradient). The pack decides: a material the engine would shade flat on its own (no toon texture, `MAT_FLAT`) still uses the pack's `terminator` / `shadowBias` when this is false

**Assign every field of `PtPackOut`**: an unassigned field is undefined (`tools/pack_check` warns).

### Lighting controls (opt-in, API 1.9.1)

A pack can split the two meanings of `flatFace` and weaken the traced sun, for example to match a raster
face that uses an SDF terminator with no cast shadow and a flat fill. Guard the new code with
`PT_PACK_API_LIGHTING` so the pack still compiles on older apps (the macro is absent there):

```hlsl
#ifdef PT_PACK_API_LIGHTING
#define PT_PACK_HAS_LIGHTING 1
#endif
...
#ifdef PT_PACK_HAS_LIGHTING
PtPackLighting PackEvaluateLighting(PtPackIn i, PtPackOut o) { ... }
#endif
```

`PackEvaluateLighting` returns a `PtPackLighting`:

- `float flatIndirect`: 0..1, when > 0.5 the indirect light is gathered flat (around the view direction, nearby
  geometry skipped) while the pack keeps its own `terminator` / `shadowBias`. Without it `flatFace = true` is what
  requests the flat gather. Assign every field (`pack_check` warns when the file defines `PT_PACK_HAS_LIGHTING`).
- `float sunShadow`: 0..1 weight of the traced sun visibility (`sunVis`) in the direct terminator and the specular.
  `0` ignores cast shadows on this surface, `1` is the old behaviour.
- `float lit`: an optional direct 0..1 lit value (the SDF / ramp result). When >= 0 it replaces the `terminator` /
  `shadowBias` composition (`-1` = unused). With the default `sunShadow = 1` it matches the old
  `term = smoothstep(edge, N.L + shadowBias) * sunVis`; `lit` lets a pack skip the bias round-trip.

Packs that do not define `PT_PACK_HAS_LIGHTING` keep the old behaviour exactly (`flatIndirect` follows `flatFace`,
`sunShadow = 1`, no explicit `lit`).

### Additional UVs and Sekai slot textures (API v6)

`PtPackIn` carries the PMX additional UV1/UV2:

- `float4 uv1`: additional UV1, zero for models without additional UVs
- `float4 uv2`: additional UV2, zero for models without additional UVs

A pack feature-detects these (and the helpers below) with `#ifdef PT_PACK_API_ADDUV`.

The hit geometry's raw sphere-slot and toon-slot textures are sampled at a mesh UV with explicit LOD 0 and
wrap addressing (material-morph factors applied), white when absent. For a Project Sekai MME layout material the
sphere slot holds the UV-space parameter mask (`_H`, no sphere-map projection) and the toon slot the shadow
colour (`_S`):

- `bool PtPackHasSphereTex()`: a real sphere-slot texture is bound for this material
- `float4 PtPackSampleSphereUv(float2 uv)`: the raw sphere-slot texture at a mesh UV
- `bool PtPackHasToonMap()`: the material's toon slot is a UV-mapped shadow colour (`MAT_TOON_MAP`)
- `float4 PtPackSampleToonUv(float2 uv)`: the raw toon-slot texture at a mesh UV

### Helpers

Pack textures declared in `pack.json` (`"textures"`, up to 16) are accessible inside `PackEvaluate`.
Sampling uses explicit LOD (no implicit-gradient `.Sample`). Textures declared `"srgb": true` (default) sample as linear; `"srgb": false` return stored values. Out-of-range index or missing texture file on disk returns white `(1, 1, 1, 1)` and size `(1, 1)`:

- `float4 PtPackSampleTex(uint i, float2 uv)`: samples pack texture `i` at LOD 0 using its declared wrap/clamp address mode.
- `float4 PtPackSampleTexLevel(uint i, float2 uv, float lod)`: samples pack texture `i` at explicit `lod` using its declared address mode.
- `uint PtPackTexCount()`: number of declared pack textures (0 if no textures declared or upload failed).
- `float2 PtPackTexSize(uint i)`: dimensions of texture `i` in pixels; returns `(1, 1)` for out-of-range index.

Note: `ScenePass` (raster/RT) and the ray-tracing / path-tracer scene cache (`render/PackTextures`) each acquire their own texture sets independently. When both raster and PT use the same pack, duplicate GPU texture copies may exist.

## Integrator behaviour

- **Camera / specular chain (`!diffuseChain`)**: The sun direct term is evaluated as:
  `lerp(albedo * shadowTint, albedo, terminator * sunVis) * sunIntensity + specular * sunVis`
  where `terminator` is the engine's smoothstep shape shifted by `shadowBias`, and `flatFace` reuses the engine's `s.flat` path. With `PT_PACK_HAS_LIGHTING` the pack can instead supply `lit` (replacing the terminator / shadowBias), scale `sunVis` with `sunShadow`, and request the flat indirect gather with `flatIndirect` without dropping its own terminator.
- **Diffuse bounces (`diffuseChain`)**: Uses `PtPackOut.albedo` as the surface albedo. Toon terms do not run on GI gather rays.
- **Per-material PBR records**: A hit whose geometry carries a Ray-MMD PBR record (`RtGeometry::pbrIndex != 0xFFFFFFFF`) is shaded with its PBR albedo / mapped normal (triangle-UV tangent frame) / GGX lobe, in the real-time PT and in the offline GI camera pass. A hit shaded by an active pack (`RTG_PT_PACK` and a `materialClass != PACK_CLASS_OFF` record) ignores its PBR record — the pack wins, as in raster. Hits without a record are unchanged.
- **Cache prepass**: The irradiance cache prepass keeps default albedo.
- **Denoiser note**: In real-time path tracing, the pack's stylized direct light goes through the same temporal / a-trous filtering as the default toon light (`radiance / a` demodulation, temporal accumulation, a-trous filter, modulation). A test pack with a hard `step()` highlight band kept crisp edges in a still capture, but fast motion was not checked; a later phase may split the stylized term out of the denoiser if ghosting shows up.

## Verification & tooling

- `tools/pack_check <pack>` reports `pt_surface: yes/no` and compiles `CSRender` and `CSPathTrace` with DXC when `--compile` is passed.
- Template: `shaders/pack_template/pt_surface.hlsl`.

## Capabilities, scene and audio inputs

Packs declare capabilities in `"requires": [...]` and requested typed inputs in `"inputs": [...]`.

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

## Surface Output Bridge (`surface.output`)

When a surface pack declares `"requires": ["surface.output"]` in `pack.json`:

```json
{
  "apiVersion": 2,
  "type": "surface",
  "requires": ["surface.output"]
}
```

The output structures gain fields for overriding normal, adding emission, and writing custom pixel tags.

### Raster (`PackResult` in `surface.hlsl`)

- `float3 normal`: custom shading normal (normalized by the engine) written to the G-buffer normal target.
- `float3 emission`: additive linear emission added to the final lit color.
- `uint tag`: 4-bit integer tag (0..15) written into bits 0–3 of the engine's ID buffer (`R16_UINT`).

### Path Tracing / Offline GI (`PtPackOut` in `pt_surface.hlsl`)

- `float3 normal`: custom shading normal used for subsequent ray bounces and reflections.
- `float3 emission`: additive linear emission added along the ray throughput.
- `uint tag`: 4-bit integer tag (0..15) recorded for the primary ray hit and written into bits 0–3 of the ID buffer.

## Extended Parameters (`params.ext`)

Surface and path-traced packs can declare `"requires": ["params.ext"]` to support up to 128 typed parameters organized in collapsible groups.

In Path Tracing / Offline GI shaders (`pt_surface.hlsl`), extended parameters are provided through `StructuredBuffer<float4> gPackExtSlots : register(t3, space1)` and indexed via per-geometry record offsets.

### HLSL Helpers

Under `#ifdef PACK_CAP_PARAMS_EXT`:
- `float PtPackParam(uint index)` / `PackParam(uint index)`: returns slot `index.x`.
- `int PtPackParamInt(uint index)` / `PackParamInt(uint index)`: returns integer value from slot `index.x`.
- `bool PtPackParamBool(uint index)` / `PackParamBool(uint index)`: returns boolean flag from slot `index.x`.
- `float3 PtPackParamColor(uint index)` / `PackParamColor(uint index)`: returns linear RGB from slot `index.rgb`.
- `float4 PtPackParamColor4(uint index)` / `PackParamColor4(uint index)`: returns linear RGBA from slot `index.rgba`.
- `float3 PtPackParamVec3(uint index)` / `PackParamVec3(uint index)`: returns vector from slot `index.xyz`.
- `uint PtPackParamCount()` / `PackParamCount()`: returns parameter count.


