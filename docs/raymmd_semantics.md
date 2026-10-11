# Ray-MMD material semantics (work unit P0)

Fact-checked 2026-10-12 against ray-cast/ray-mmd master and the local library.

Implementer's reference for reproducing Ray-MMD material data in our own PBR shading. Everything below is
either **confirmed from source** (with a citation) or explicitly marked **inferred** / **unknown**.
No Ray-MMD code is copied; formulas are restated in words or pseudo-math.

## 0. Sources and how to read citations

Public source: `https://github.com/ray-cast/ray-mmd` (MIT). All public citations are paths relative to the
repository root at `master`; line ranges refer to the master revision fetched 2026-10-12 via
`raw.githubusercontent.com/ray-cast/ray-mmd/master/<path>`.

Local library copy (version 1.5.2). The library holds **several distinct variants** of the material-side file, not
a single byte-identical copy:

- `.../MovieSonicMMD_Ver2.0.2/Ray1.5.2Materials/FX/material_common_2.0.fxsub`, plus the copies under
  `BeeNic_v1.11_MMD` and `stage/moonlit_waves` → **byte-identical** to public
  `Materials/material_common_2.0.fxsub` (sha256 `c7307832…`, 48,596 bytes; verified with `sha256sum`). Their
  `ALPHA_THRESHOLD` is the standard `0.999`.
- `.../Anyisdragon_MMD/RayMaterial_1.5.x/Material/material_common_2.0.fxsub`,
  `.../Anyisdragon_MMD_NSFW/...` and `.../MMD_RomEthanShax_Release/textures/RayFX/material_common_2.0.fxsub`
  → a **modified** `material_common_2.0.fxsub` (sha256 `e7df2e3d…`, 48,842 bytes; 39 files across those sets)
  with `ALPHA_THRESHOLD 0.75` instead of `0.999`, cutout enforcement in `MaterialPS`, and `Material2PS` clipping
  `alpha - ALPHA_THRESHOLD` (with `material.alpha = 0` in the non-animated case) instead of the master
  `clip(alpha - 0.01)`.
- `.../FX/material_common_2.0_Cutout.fxsub` → local-only variant (not in master `Materials/`). It occurs in
  **three threshold flavours** across the library: `0.2` (`AlcanderMMD`,
  `Alcander_MMD_ClothVER2_Release`), `0.5` (`MovieSonicMMD_Ver2.0.2`, and `.../material_common_2.0_Cutout2.fxsub`)
  and `0.75` (`MMD Ross`). `Material2PS` clips `alpha - ALPHA_THRESHOLD` instead of `0.01`, and forces
  `material.alpha = 0` (so the "alpha blend" pass never blends).
- `.../MMD_RomEthanShax_Release/textures/RayFX/material_Hypnosis.fxsub` → a `material_common` derivative
  (3 files) that injects an animated UV scroll `coord.x += UvMoveX * time; coord.y += UvMoveY * time;` into the
  sampling coordinate of every `Get*` function.
- `.../FX/main.fxsub` + `.../FX/main.fx` → local-only **older matcap** variant (uses `MATCAP_MAP_FROM` /
  `GetSpecularHighlight` and does *not* include `material_common_2.0.fxsub`). It shares `Other/ray.conf` with the
  material path but writes a specular highlight, not the PBR G-buffer. Out of scope for the PBR decode; noted here
  so a parser does not mistake it for a PBR material.
- `.../FX/Other/ray.conf` → controller config used by the 1.5.2 material set. Differs from master `ray.conf` only in
  `OUTLINE_QUALITY` (`3` vs `0`) and `TOON_ENABLE` (`1` vs `0`). `SSSS_QUALITY 1` in both.

**Implication for the parser:** do not assume the public master `material_common_2.0.fxsub`. The parser must read
the actual included `material_common_2.0*.fxsub` (or `material_Hypnosis.fxsub`) that sits next to each material
`.fx`, because `ALPHA_THRESHOLD` and the cutout behaviour in `MaterialPS`/`Material2PS` differ per set
(`0.999` master, `0.75` Anyisdragon/RomEthanShax, `0.2`/`0.5`/`0.75` Cutout, plus the Hypnosis UV scroll).

Two sides of Ray-MMD:

- **Material side** (`Materials/material_common_2.0.fxsub`, lines 1–1599): reads the slot `#define`s, samples maps,
  and *encodes* a G-buffer. It does not light anything.
- **Controller side** (`ray.fx`, `Shader/*.fxsub`): *decodes* the G-buffer and performs the actual shading
  (`Shader/gbuffer.fxsub` decode, `Shader/BRDF.fxsub`, `Shader/ShadingMaterials.fxsub`, `Shader/ibl.fxsub`).

The local 1.5.2 library's material `.fx` files include the material-side file only; all controller-side math below
comes from public `master`, which is the same Ray-MMD generation as the local material files (the controller side
is not vendored locally).

---

## 1. Map slots

### 1.1 Slot list

Confirmed from `Materials/material_common_2.0.fxsub` (local, identical): the 13 slots are
`ALBEDO`, `ALBEDO_SUB`, `ALPHA`, `NORMAL`, `NORMAL_SUB`, `SMOOTHNESS`, `METALNESS`, `SPECULAR`, `OCCLUSION`,
`PARALLAX`, `EMISSIVE`, `CUSTOM_A`, `CUSTOM_B`. Each slot has a `*_MAP_FROM` and (usually) `*_MAP_TYPE`,
`*_MAP_SWIZZLE`, `*_MAP_UV_FLIP`, `*_MAP_APPLY_SCALE`, `*_MAP_COLOR_FLIP`, `*_MAP_FILE`, and a `const` value
(`albedo`, `albedoSub`, `alpha`, `normalMapScale`, `smoothness`, `metalness`, `specular`, `occlusion`,
`parallaxMapScale`, `emissive`, `customA`, `customB`). Confirmed from `Materials/README.md`.

**Terminology note (inferred):** the P0 plan mentions `*_MAP_UV_SCALE` and `*_MAP_LOOP`; those exact names do
**not** appear in this version. The equivalent is the per-slot `const ... <slot>MapLoopNum` (a float2 UV tiling
multiplier) plus `normalMapScale` (normal strength). There is no `*_MAP_UV_SCALE` token in
`material_common_2.0.fxsub` (grep: 0 hits). See §1.7.

### 1.2 `*_MAP_FROM` values 0..9

Confirmed from `Materials/README.md` (ALBEDO_MAP_FROM section, lines 17–35) and from the code in
`material_common_2.0.fxsub` (`GetAlbedo` 722–784, `GetSubAlbedo` 786–839, `GetAlpha` 841–882, `GetSmoothness`
1021–1074, `GetMetalness` 1076–1123, `GetSpecular` 1125–1190, `GetOcclusion` 1192–1243, `GetEmissiveColor`
1245–1296, `GetCustomDataA` 1311–1374, `GetCustomDataB` 1376–1425).

| FROM | Meaning | Where the value comes from |
|----|---------|-----------------------------|
| 0 | None (use the `const` slot value) | constant `<slot>` value |
| 1 | File | texture with `ResourceName = <slot>_MAP_FILE` |
| 2 | Animated file | `ANIMATEDTEXTURE` with `ResourceName = <slot>_MAP_FILE` |
| 3 | PMX material texture | `MATERIALTEXTURE` (the PMX diffuse/base texture) |
| 4 | PMX sphere map | `MATERIALSPHEREMAP` |
| 5 | PMX toon map | `MATERIALTOONTEXTURE` |
| 6 | Screen/AVI dummy | `DummyScreenTex` render target |
| 7 | PMX **ambient** color | `MaterialAmbient` (a float4) |
| 8 | PMX **specular** color | `MaterialSpecular` (a float4) |
| 9 | PMX **specular power** | `MaterialPower`; valid **only for SMOOTHNESS/METALNESS** |

Key facts:

- **7 and 8 are not texture variations.** They read the PMX material's ambient / specular *color* (`MaterialAmbient`
  / `MaterialSpecular`, the `AMBIENT`/`SPECULAR` MMD semantics bound at `material_common_2.0.fxsub:6–9` — note line 7
  binds `MaterialAmbient : EMISSIVE`, line 8 `MaterialSpecular : SPECULAR`). For most slots the code assigns the
  `float4` directly (no texture read). **Confirmed** at e.g. `GetSmoothness:1038–1041`, `GetMetalness:1093–1096`,
  `GetSpecular:1146–1149`, `GetOcclusion:1209–1212`, `GetCustomDataA:1331–1334`. Only `GetAlbedo:739–742` reads the
  *diffuse* texture when `use_texture` is true (FROM 7 uses `MaterialAmbient.rgb`, FROM 8 `MaterialSpecular.rgb`
  as fallback); `GetAlpha:858–862` (FROM 7/8) reads `MaterialAmbient`/`MaterialSpecular` **directly, with no
  texture read at all**; `GetSubAlbedo:804–807` likewise uses the raw color.
- **NORMAL and NORMAL_SUB reject FROM 7/8/9.** Unlike every other slot, the normal slots `#error` on options 7, 8
  and 9 (`GetMainNormal:902`, `GetSubNormal:947`: `#error Unsupported options 7, 8, 9.`).
- **9 is only meaningful for SMOOTHNESS and METALNESS** (`GetSmoothness:1042–1043`, `GetMetalness:1097–1098`), where it
  is `ShininessToSmoothness(MaterialPower)`. For every other slot `9` is a compile `#error` (e.g. `GetAlbedo:743–744`).
- `*_MAP_FILE` matters only for FROM 1 and 2 (it becomes the `ResourceName`). For 3/4/5/6/7/8 the sampler is bound to
  the PMX texture / dummy target (see the `sampler_state` blocks, e.g. `material_common_2.0.fxsub:128–144`).
- The same `GET*` function is used for the CUSTOM_A/B slots; the `#if CUSTOM_ENABLE` gates whether CUSTOM maps are even
  declared/sampled (`material_common_2.0.fxsub:397–445`).
- **Compilation hazard — `DiffuseMap` declaration:** `material_common_2.0.fxsub:90–96` only declares
  `texture DiffuseMap: MATERIALTEXTURE;` when at least one slot sets `*_MAP_FROM == 3`. A material that uses e.g.
  `ALBEDO_MAP_FROM 7` (which also samples `DiffuseMap`, line 131) but no slot with FROM 3 leaves `DiffuseMap`
  undeclared, causing an HLSL compile error.

### 1.3 `*_MAP_TYPE`

Confirmed from `Materials/README.md`.

- `NORMAL_MAP_TYPE` / `NORMAL_SUB_MAP_TYPE` (`GetMainNormal:907–921`, `GetSubNormal:952–970`):
  - `0`: RGB tangent-space normal (raw `rgb*2-1`), recompute `z` only for type 1? — no: type 0 keeps the full RGB and
    just rescales/renormalizes `rg` by `normalMapScale`.
  - `1`: RG compressed tangent-space; `z = sqrt(1 - x² - y²)`.
  - `2`: grayscale bump, low quality (`PerturbNormalLQ:548–563`, screen-derivative based).
  - `3`: grayscale bump, high quality (`PerturbNormalHQ:567–582`, samples neighbours at `coord ± ddx/ddy`).
  - `4`: RGB **world-space** normal (no TBN transform; `NORMAL_MAP_UV_FLIP == 4` flips `N.x`, `GetNormal:980–982`).
  - `Normal_SUB` differs subtly: for type 0 the sub map is treated as raw RGB then converted with `*2-1`
    (`GetSubNormal:952–954`), and type 4 skips that conversion.
- `SMOOTHNESS_MAP_TYPE` (`GetSmoothness:1058–1062`):
  - `0`: value is smoothness as-is.
  - `1`: value is roughness, converted `smoothness = 1 - sqrt(roughness)` (`RoughnessToSmoothness:679–682`).
  - `2`: value is linear roughness, converted `smoothness = 1 - roughness`.
- `SPECULAR_MAP_TYPE` (`GetSpecular:1156–1172`):
  - `0`: RGB, `F = 0.08 * x`.
  - `1`: RGB, `F = 0.16 * x²`.
  - `2`: grayscale (uses swizzle channel only), `F = 0.08 * x`.
  - `3`: grayscale, `F = 0.16 * x²`.
  - `4`: for FROM 0 only — returns `saturate(specular)`, i.e. the `specular` constant is used as F0 directly
    (it does **not** hardcode `0.04`; default `specular = 0.5` gives `0.04` only through type 0). When
    `SPECULAR_MAP_FROM != 0`, type 4 falls through to the `0.08 * specularColor` branch, not a constant.
  - Note: the sampler branch clamps with `clamp(specularColor.rgb, 0.01, 1.0)` (`GetSpecular:1180`), so any
    dielectric F0 texture value below `0.01` is clamped up to `0.01`.
  - Note: RGB types (0/1) are **incompatible with `CUSTOM_ENABLE > 0`** (`#error` at `GetSpecular:1128–1130`).
- `OCCLUSION_MAP_TYPE` (`GetOcclusion`, `MaterialVS/MaterialPS:1447–1471`):
  - `0`: linear color-space AO from UV0.
  - `1`: sRGB AO (applies `srgb2linear`), UV0.
  - `2`: linear AO from **UV1** (second UV set — the VS only then passes `TEXCOORD1`).
  - `3`: sRGB AO from UV1.
- `PARALLAX_MAP_TYPE` (`MaterialPS:1477–1486`, `Material2PS:1525–1537`):
  - `0`: sample at the plain (loop-scaled) coordinate.
  - `1`: run parallax occlusion mapping (`ParallaxOcclusionMap:624–671`).
  - **Caveat:** the type branch is only evaluated in the transparent pass (`Material2PS`). In the opaque
    `MaterialPS` (`1477–1486`) POM runs **unconditionally** whenever `PARALLAX_MAP_FROM` is set, ignoring
    `PARALLAX_MAP_TYPE` entirely.
- `CUSTOM_B_MAP_TYPE`: only used as `CUSTOM_B_MAP_TYPE == 0` ⇒ apply `srgb2linear` to the sampled color
  (`GetCustomDataB:1406–1408`).
- `ALPHA`, `METALNESS`, `EMISSIVE`, `CUSTOM_A` have no meaningful `*_MAP_TYPE` in this file (ALPHA has none at all).

### 1.4 `*_MAP_SWIZZLE`

Confirmed from `Materials/README.md` (`ALPHA_MAP_SWIZZLE` section) and code: `0 = R`, `1 = G`, `2 = B`, `3 = A`.
Applied to ALPHA (`GetAlpha:868–876`), SMOOTHNESS (`1048–1056`), METALNESS (`1103–1111`), OCCLUSION (`1219–1227`),
CUSTOM_A (`1341–1349`), and to SPECULAR **only when type is 2 or 3** (`GetSpecular:1156–1166` — RGB types ignore
swizzle). The map is a `float4` and the chosen channel is a scalar, so RGB specular picks the whole `rgb`.

Local usage: every one of the 163 local materials sets `ALPHA_MAP_SWIZZLE 3` (alpha in the A channel);
`CUSTOM_A_MAP_SWIZZLE 0`, `OCCLUSION_MAP_SWIZZLE 0` everywhere.

### 1.5 `*_MAP_UV_FLIP`

Confirmed: `0` none, `1` flip X, `2` flip Y, `3` flip both (`x = 1-x` / `y = 1-y`, e.g. `GetAlbedo:725–731`).
Special case: for NORMAL, `NORMAL_MAP_UV_FLIP == 4` flips the shading normal `N.x` instead of the UV
(`GetNormal:980–982`).

### 1.6 `*_MAP_APPLY_SCALE` and `*_MAP_COLOR_FLIP`

- `*_MAP_APPLY_SCALE` (`Materials/README.md`): `0` none, `1` `map * value`, `2` `map ^ value` (component-wise).
  Applied per slot, e.g. albedo `GetAlbedo:763–767`; sub-albedo `814–818`; smoothness `1064–1068`; metalness
  `1113–1117`; specular `1174–1178`; occlusion `1233–1237`; custom A `1351–1355`; custom B `1414–1418`.
- `ALBEDO_MAP_APPLY_DIFFUSE` (`GetAlbedo:749–760`): when enabled, multiplies the sampled albedo by
  `MaterialDiffuse.rgb` (or by `1.0` when the corresponding PMX texture is missing). Present in the Ray-MMD
  templates.
- `*_MAP_COLOR_FLIP`: only CUSTOM_A/B have it. `1` inverts the value. For CUSTOM_A under
  `CUSTOM_ENABLE == CLEAR_COAT` the inversion is the smoothness↔roughness conversion
  (`RoughnessToSmoothness`), otherwise `1 - value` (`GetCustomDataA:1358–1370`). For CUSTOM_B it is a plain
  `1 - value` applied after optional sRGB decode (`GetCustomDataB:1410–1412`).
  Local: `CUSTOM_B_MAP_COLOR_FLIP 1` in 18 materials; `CUSTOM_A_MAP_COLOR_FLIP 0` everywhere.

### 1.7 UV tiling / loop counts

Confirmed: instead of `*_MAP_UV_SCALE`/`*_MAP_LOOP`, each slot defines a `const float2 <slot>MapLoopNum`
(e.g. `albedoMapLoopNum`, `normalMapLoopNum`, `parallaxMapLoopNum`) that multiplies the sampled UV: every `Get*`
does `tex2D(sampler, coord * <slot>MapLoopNum)` (e.g. `GetAlbedo:734–746`, `GetNormal:896–904`). `normalMapScale`
(a `const float`, not float2) is the normal strength (`GetMainNormal:919`). Values in the local library range from
`1.0` to `80.0` (e.g. skin sub-normal uses `80.0`). Confirmed PARALLAX interaction: if parallax runs, the parallax
coordinate is computed once and then also multiplied by the per-slot loop counts (`MaterialPS:1477–1495`), so
increasing `parallaxMapLoopNum` tiling ripples into all slots (documented in `Materials/README.md` FAQ).

### 1.8 Sub-map blend modes

**`ALBEDO_SUB_ENABLE`** (confirmed `Materials/README.md` SubAlbedo section; code `GetSubAlbedo:823–833`). The sub
sample (`albedoSubColor`) is obtained like a normal albedo sample (with `albedoSub` as its `const` value) and then:

| value | blend applied to base albedo |
|----|--------------------------------------------------------------|
| 0 | none (`saturate(albedo)`) |
| 1 | `albedo = albedo * albedoSub` |
| 2 | `albedo = albedo ^ albedoSub` |
| 3 | `albedo = albedo + albedoSub` |
| 4 | melanin synthesis (`ColorSynthesis:510–514`): `albedo * lerp(1, albedo*luminance(albedo), sub)` |
| 5 | alpha blend: `lerp(albedo, albedoSub, albedoSub.a)` |

Result is `saturate`d (`835`). Local: 22 materials use `ALBEDO_SUB_ENABLE 1`, the rest `0`.

**NORMAL_SUB blend** (confirmed `GetNormal:996–997`, `RNMBlendUnpacked:540–545`): when both NORMAL and NORMAL_SUB
are present they are combined with Reoriented Normal Mapping (RNM), not a plain lerp. When only one is present it is
used as-is. Type-2 bump cannot be combined with a second normal map (`#error` at `914`, `963`).

**EMISSIVE has no sub map**; it has morph controls instead (§1.10).

### 1.9 Alpha behaviour

Confirmed:

- `ALPHA_MAP_FROM == 0`: `alpha = saturate(alpha * MaterialDiffuse.a)` (`GetAlpha:880`).
- Otherwise the sampled/swizzled channel is `saturate`d (`878`). For FROM 3/4/5 the sampled alpha is multiplied by
  `MaterialDiffuse.a` (`853–857`).
- Opaque (non-blend) pass `MaterialPS`: `clip(alpha - ALPHA_THRESHOLD)` (`1489`) with `ALPHA_THRESHOLD = 0.999`
  (standard file; local Cutout variants use `0.2`/`0.5`/`0.75` and the Anyisdragon/RomEthanShax modified
  `material_common_2.0.fxsub` uses `0.75` — see §0). `MaterialPS` then writes `material.alpha = 1` (`1510`) — the
  alpha registered in the G-buffer for the opaque pass is 1, the *cutout* already happened.
- Alpha-blend pass `Material2PS`: `clip(alpha - 0.01)` (`1541`); `material.alpha = alpha > ALPHA_THRESHOLD ? 0 : alpha`
  (`1562`); and if `ALPHA_MAP_FROM == 2` (animated) `clip(-1)` kills the whole pass in `MaterialPS` (`1490–1492`).
  (In the Cutout/Anyisdragon variants the `0.01` is replaced by `ALPHA_THRESHOLD` and the non-animated branch
  forces `material.alpha = 0`.)
- `ALPHA_THRESHOLD 0.999` and its companion `EMISSIVE_EPSILON = 2/255` are at `material_common_2.0.fxsub:78–80`.

### 1.10 Emissive and morph controls

Confirmed from `Materials/README.md` and code `material_common_2.0.fxsub:18–57, 1245–1309, 1427–1441`.

- `EMISSIVE_ENABLE` gates the whole emissive path; when 0, `GetEmissiveColor` returns 0 (`1293–1294`).
- `GetEmissiveColor` sample modes: FROM 1/2/6 sample the emissive map and use `lerp(0, rgb, a)` (alpha is emissive
  strength); FROM 3/4/5 same but gated by `use_texture/use_spheremap/use_toon`; FROM 7/8 use ambient/specular color;
  FROM 0 uses the `const emissive` value. The color is `srgb2linear`'d (`1278`).
- `EMISSIVE_MAP_APPLY_SCALE` (`1280–1282`): multiply by `emissive` when non-zero.
- `EMISSIVE_MAP_APPLY_MORPH_COLOR` (`1284–1286`): multiply by `MorphColor = (R+, G+, B+)` morph values
  (`18–24, 770, 780, 1285`). Also usable for ALBEDO via `ALBEDO_MAP_APPLY_MORPH_COLOR`.
- `EMISSIVE_MAP_APPLY_BLINK` (`26–48, 1288–1290`): `1` = multiply by a smooth-triangle wave of
  `time * emissiveBlink`; `2` = same but scaled by the `Blink` morph (`mBlink`). `emissiveBlink` is a `const float3`
  per channel.
- `EMISSIVE_MAP_APPLY_MORPH_INTENSITY` (`50–57, 1298–1309`): `LightIntensity` is `lerp(log(50), log(2100), mIntensityP - mIntensityM)`
  from the `Intensity+`/`Intensity-` morphs; final intensity = `emissiveIntensity * LightIntensity`.
- `GetLightMode` (`1427–1441`): if `EMISSIVE_ENABLE` and any emissive channel exceeds `EMISSIVE_EPSILON = 2/255`, the
  light model is promoted to `EMISSIVE (2)`, overriding `CUSTOM_ENABLE`.

Local usage: 22 materials set `EMISSIVE_ENABLE 1`; all morph/blink controls are 0 in the local library, and only 7
materials use `EMISSIVE_MAP_APPLY_SCALE 1`.

---

## 2. Shading models 0..10 (`CUSTOM_ENABLE`)

Confirmed model IDs (`material_common_2.0.fxsub:63–73`) and the semantic table from `Materials/README.md`
("Shading Model ID" section):

| ID | Name | CUSTOM_A | CUSTOM_B |
|----|------|----------|----------|
| 0 | DEFAULT | invalid | invalid |
| 1 | SKIN (pre-integrated) | curvature | transmittance color |
| 2 | EMISSIVE (unlit placeholder) | invalid | invalid |
| 3 | ANISOTROPY | anisotropic | shift tangent |
| 4 | GLASS | curvature | transmittance color |
| 5 | CLOTH | sheen | fuzz color |
| 6 | CLEAR_COAT | smoothness | invalid |
| 7 | SUBSURFACE | curvature | transmittance color |
| 8 | CEL | threshold | shadow color |
| 9 | TONEBASED | hardness | shadow color |
| 10 | MASK | — | — |

Controller-side math: `Shader/BRDF.fxsub` and `Shader/ShadingMaterials.fxsub`. The core is `SurfaceEnergy`
(`BRDF.fxsub:249–322`), which computes `(diffuse, specular)` for a light `L`, view `V`, normal `N`; the sun path
`ShadingMaterial` (`ShadingMaterials.fxsub:9–41`) adds the subsurface term and multiplies by light color.

Common inputs used by almost every branch:

- `roughness = max(SmoothnessToRoughness(smoothness), 0.001)`, where `SmoothnessToRoughness(s) = (1-s)²`
  (material side `material_common_2.0.fxsub:674–677`; the controller has its own copy in `Shader/math.fxsub`).
- `H = normalize(V + L)`, `nh = sat(N·H)`, `nl = sat(N·L)`, `vh = sat(V·H)`, `nv = |N·V| + 1e-5`.
- `occlusion = ComputeAmbientMicroOcclusion(nl, visibility) * shadow`
  (`BRDF.fxsub:21–25, 268`): `aperture = 2*ao`, `shadow = sat(|nl| + aperture - 1)`.
- Diffuse default is **Burley** (`BRDF.fxsub:77–86`), `diffuse * nl * occlusion`.
- Specular default is **GGX** (`BRDF.fxsub:141–155`).

Per model (all **confirmed from source**, described in words):

- **0 DEFAULT**: Burley diffuse + GGX specular. No custom data.
- **1 SKIN**: diffuse unchanged; adds `SubsurfaceShadingPennerSkin` (`BRDF.fxsub:28–39`). Formula:
  `sss = customB * (1 - nl_sat)² * (1 - nndl)^(3/(customA+0.001)) * sat(customA - 0.04)`, where `nndl = sat(-N·L)`
  and `customA` = curvature / scattering radius, `customB` = transmittance color. The subsurface term is added to
  diffuse and multiplied by shadow (`ShadingMaterials:36`). (An alternative pre-integrated-LUT path exists in
  `Shader/PreIntegratedSkin.fxsub:9–25`; a scan of the master tree found its functions referenced only inside that
  file, so the Penner approximation above is the path actually wired into `ShadingMaterial`. **Inferred**, flagged.)
- **2 EMISSIVE**: the material is an unlit placeholder; the material side promotes to this model when emissive
  exceeds `EMISSIVE_EPSILON` (§1.10). The emissive color (stored in the G-buffer's `customDataB`) is added in the
  transparent pass `ShadingTransparentPS` (`ShadingMaterials.fxsub:197–200`) — that pass adds emissive directly,
  including a view-angle-dependent tint toward `(0.6, 0.45, 0.25)` at grazing angles — i.e. it emits light rather
  than being lit. For **opaque** materials the emissive is *not* added to the main shading target; it is written
  into the G-buffer and reconstructed in post-process (`Shader/PostProcessBloom.fxsub:28–34`,
  `DecodeGBufferEmissive`) so it feeds the bloom.
- **3 ANISOTROPY**: specular is `SpecularBRDF_GGXAniso` (`BRDF.fxsub:158–178`). It builds tangent `X` and bitangent
  `Y` from `cross(N,(0,1,0))` plus `N * shift.r`, makes the GGX lobe elliptical with
  `aspect = rsqrt(1 - anisotropic*0.9)`, `ax = 1/(roughness*aspect)`, `ay = aspect/roughness`. `customA` =
  anisotropic strength, `customB` = shift tangent (its `.r` is used). Fresnel uses `f0 = specular`, `f90 = 1`.
- **4 GLASS**: specular is `SpecularBRDF_Blinn` (`BRDF.fxsub:134–138`), `alpha = exp2(10*smoothness+1)`.
  The refractive blend is in the controller's transparency pass (`ShadingMaterials.fxsub:170–186`): IOR-style UV
  offset `ior = mul(normal, (float3x3)matViewInverse).xy * customA * 0.04` — the normal is transformed by
  `matViewInverse`, so this offset is in **world space**, not view space; refraction color is a screen sample whose
  modulation uses `dielectricColor = max(0.02, material.albedo)` (not raw albedo); final
  `lighting = lerp(refract * dielectricColor, lighting, FresnelSchlickGlass * alpha)`; plus a transmission term
  `customA * customB * trans * sat(N·(-L))`. `customA` = curvature/refraction strength, `customB` = transmittance
  color, `FresnelSchlickGlass` (`ibl.fxsub:11–13`) = `lerp(specular, 1, pow5(1-N·V)/(40-39*smoothness))`.
- **5 CLOTH**: `ClothShading` (`BRDF.fxsub:224–230`) interpolates between GGX and a sheen lobe:
  `lerp(GGX, Sheen, customDataA)`. `Sheen` (`180–192`) = `(2 + 1/roughness) * sin²(nh)^(0.5/roughness) / (2) / G`
  with `G = 4(nl + nv - nl*nv)` and Fresnel `f0 = specular, f90 = 1`. `customDataA` = sheen amount,
  `customDataB` = fuzz color (the sheen Fresnel `f0`, in sRGB).
- **6 CLEAR_COAT**: `ClearCoatShading` (`BRDF.fxsub:232–239`) = `lerp(GGX, SpecularBRDF_ClearCoat, coatFresnel)`.
  `SpecularBRDF_ClearCoat` (`211–221`) is GGX-shaped with `G = 4*vh² + 1e-5`, constant `f0 = 0.04`; the coat
  roughness is `max(SmoothnessToRoughness(customDataA), 0.001)` (customDataA = clear-coat smoothness). `customB`
  invalid.
- **7 SUBSURFACE**: same subsurface emitter as SKIN (curvature + transmittance color). **Unknown**: the specific
  `SubsurfaceShadingScattering` variant referenced by `PreIntegratedSkin.fxsub:22` could not be located in the master
  tree (a full scan of all `.fx`/`.fxsub` found only that reference); the Penner path used by `ShadingMaterial` is the
  practical behavior.
- **8 CEL**: only active when `TOON_ENABLE` (a controller `ray.conf` flag; **1** in our local set, **0** in master
  default). `CelShading` (`BRDF.fxsub:124–131`): `darkness = (nl*0.5+0.5) * shadow`,
  `I = lerp(shadowColor, 1, smoothstep(threshold-bias, threshold, darkness))`, with `bias = 0.02`. `customDataA` =
  threshold, `customDataB` = shadow color. Diffuse replaced entirely by this step function.
- **9 TONEBASED**: also `TOON_ENABLE`-gated. `ToonBasedShading` (`BRDF.fxsub:112–121`): a Burley-style `FdV/FdL`
  shadow term modulated by `saturate(nl*hardness + 1 - hardness) * shadow` times `(1 - fresnel)`, blended from
  `shadowColor` to 1. `customDataA` = hardness, `customDataB` = shadow color.
- **10 MASK**: declared but rejected by `#if CUSTOM_ENABLE >= SHADINGMODELID_MASK → #error` (`material_common:1430–1432`),
  so no material may use 10.

Local usage: among the `.fx` files that define `CUSTOM_ENABLE`, the values are `0` (87), `1` (26), `3` (4), `5` (20),
`8` (5). No material
uses 2/4/6/7/9/10 directly (EMISSIVE 2 still occurs at runtime via promotion).

---

## 3. G-buffer encoding

Confirmed from `material_common_2.0.fxsub` `EncodeGbuffer:689–720` and `Shader/gbuffer.fxsub` `DecodeGbuffer:79–107`,
`DecodeNormal:47–57`, `DecodeGBufferEmissive:129–135`. The material writes a `GbufferParam` with four `float4`
targets (`COLOR0..COLOR3`, `463–469`):

| Target | Contents (encoded) |
|--------|--------------------|
| buffer1 (COLOR0) | `rgb = albedo * (1 - metalness)`, `a = smoothness` |
| buffer2 (COLOR1) | `rgb = EncodeNormal(view-space normal)`, `a = customDataA` |
| buffer3 (COLOR2) | `rgb = lerp(specular, max(0.02, albedo), metalness)`, `a = 0` (see note) |
| buffer4 (COLOR3) | `x = linearDepth`, `y = emissiveIntensity`, `z = visibility (AO)`, `w = lightModel + alpha*MAX_FRACTIONAL_8_BIT` |

Encoding details:

- Normal is encoded with `EncodeNormal` (`471–478`) — an octahedral-style packing into 3 channels, done in
  **view space** (normal is multiplied by `matWorldView` first, `695–696`).
- `buffer3` is overwritten for non-default light models (`704–714`): `rgb = (luminance(that), customDataB)` — i.e.
  `x` carries a scalar specular and `yzw` carry `customDataB` (so `.a` is part of the custom data, not padding).
  Exceptions: `EMISSIVE_ENABLE` forces `customDataB = emissive`, and for GLASS the `(1-metalness)` scale is skipped.
- `buffer4.w = lightModel + alpha*MAX_FRACTIONAL_8_BIT`, so decode recovers
  `alpha = frac(buffer4.w)/MAX_FRACTIONAL_8_BIT` and `lightModel = floor(buffer4.w)`.
- The decoded `MaterialParam` (`gbuffer.fxsub:16–26`) needs: `normal, albedo, specular, smoothness, alpha,
  visibility, customDataA, customDataB, linearDepth, lightModel`. Metalness is **not** decoded directly — it is
  baked into albedo (`albedo*(1-metalness)`) and specular (`lerp(specular, albedo, metalness)`).
- For non-default models, decode reconstructs `specular = buffer3.x` (as a scalar), `customDataB = buffer3.yzw`
  (and `customDataA = buffer4.y` for EMISSIVE, since `buffer2.a` is 8-bit and insufficient for emissive intensity).

**Confirmation for our design:** the controller only ever consumes the *decoded* quantities above; the exact
render-target naming (which of `MaterialMap` / `Gbuffer2RT..Gbuffer8RT` is COLOR0) does not matter. The material
technique's `Script` sets `RenderColorTarget1..3 = Gbuffer2RT..Gbuffer4RT` (opaque) / `Gbuffer6RT..Gbuffer8RT`
(alpha) and leaves `RenderColorTarget0` empty (`1568–1592`), so COLOR0 = the currently bound target. **Inferred:** the
controller's `Gbuffer1Map` (= `MaterialMap`, `gbuffer_sampler.fxsub:1–6`) is COLOR0 for opaque and `Gbuffer5Map` is
COLOR0 for the alpha pass (the `ShadingOpacityPS`/`ShadingTransparentPS` sampler names in
`ShadingMaterials.fxsub:77–80, 141–144` read `Gbuffer1Map`/`Gbuffer5Map` for buffer1). The exact binding is not
visible in the static tree; not needed for us.

For our engine we only need to reproduce the *input* quantities that the material computes before encoding:
albedo (post sub-blend), smoothness, metalness, specular/F0, normal, AO/visibility, emissive + intensity, model id,
customA/B.

---

## 4. Specular model

Confirmed from `Shader/BRDF.fxsub`.

- **Distribution: GGX / Trowbridge-Reitz** (`SpecularBRDF_GGX:141–155`):
  `m2 = roughness²`; `spec = m2 / (((nh*m2 - nh)*nh + 1)²)`, then a geometry divisor.
- **Geometry term** (`145–150`): a Schlick-GGX-like Smith pair, not the standard `k = roughness²/2` form:
  `Gv = nl*(nv*(1-roughness)+roughness)`, `Gl = nv*(nl*(1-roughness)+roughness)`, `spec *= 0.5/(Gv+Gl)`.
  (Appears to be a per-term approximation; **inferred** that this is not the exact UE4 split-sum form, but it is
  the literal code.)
- **Fresnel: Schlick** (`fresnelSchlick:11–18`): `lerp(f0, f9, pow5(1-vh))`, with `pow5(x)=x⁵`. `f90` is computed as
  `ComputeSpecularMicroOcclusion(f0) = saturate(dot(f0,1/3)*50)` (`1–8`) rather than 1 — it fades the grazing
  reflection for dark F0.
- **F0 combination** (material side, `EncodeGbuffer:701`): `buffer3.rgb = lerp(specular, max(0.02, albedo), metalness)`.
  So `F0 = specular` for dielectrics and `F0 = albedo` for metals, with 0.02 as the dielectric floor. This is the
  decoded `material.specular` used as `f0` in every lobe.
- **Default F0**: `specular` default `0.5` → `GetSpecular` type 0 → `0.08*0.5 = 0.04` (documented in
  `Materials/README.md` Specular section). Types 1/3 give `0.16*x²`. Type 4 does **not** use a constant: for
  FROM 0 it returns `saturate(specular)` (the raw `specular` value as F0), and for FROM != 0 it falls back to
  `0.08 * specularColor`.
- **Specular occlusion** for IBL (`ibl.fxsub:59–61`): `saturate(pow(nv + ao, smoothness) - 1 + ao)`.

Other lobes (all `BRDF.fxsub`): Blinn (`134–138`, glass), anisotropic GGX (`158–178`), Sheen (`180–192`, cloth),
Velvet (`194–208`, an alternative cloth lobe — **not referenced** anywhere in the mastered tree; dead code at
master), ClearCoat (`211–221`). Diffuse is Burley (`77–86`); `OrenNayarBRDF` (`89–98`) is defined but **not
referenced** in the wired path (dead code at master).

---

## 5. Where tangents come from

Confirmed: Ray-MMD does **not** use vertex tangents. `ComputeTangentBinormalNormal` (`material_common_2.0.fxsub:517–538`)
derives the tangent frame from screen-space derivatives of the view/world position and the UV:

- `dp1 = ddx(P)`, `dp2 = ddy(P)`, `duv1 = ddx(coord)`, `duv2 = ddy(coord)`.
- build `I` from cross products of `[dp1, dp2, N]`, then `T = duv1.x*dp1-ish + duv2.x*dp2-ish` (the code uses
  `mul(float2(duv1.x, duv2.x), I)`), `B` likewise for the y components, normalize with a small epsilon.
- The bitangent is negated when building the TBN (`tbnTransform[1] = -normalize(B)`).

For world-space normals (`NORMAL_MAP_TYPE == 4`) no tangent frame is built at all. This matches the P0 plan's
recommendation: use screen derivatives (raster) / triangle-UV-based frames (RT/PT), because PMX has no tangents.

---

## 6. Proposed MMDX12 internal representation

Minimal per-material PBR record and how each Ray-MMD slot maps in. Values are pre-decoded on the CPU where possible;
the shader receives only the flattened quantities. This is a **proposal** (the plan leaves the internal record to the
implementer); field names are indicative and do not constrain the code layout.

| MMDX12 internal field | Type | Ray-MMD source | Notes |
|-----------------------|------|----------------|-------|
| `albedo` | float3 | ALBEDO (+ ALBEDO_SUB blend, `*_APPLY_SCALE`, `APPLY_DIFFUSE`, morph color, alpha) | sub-blend is a non-trivial op; do it on load where the sub map is static, else in-shader |
| `alpha` | float | ALPHA (`*_MAP_SWIZZLE`, `MaterialDiffuse.a`) | cutout threshold from ALPHA_THRESHOLD |
| `normal` | float3 (tangent) | NORMAL (`*_MAP_TYPE`, `normalMapScale`) | decode to tangent-space vector, strength folded in |
| `normalSub` | float3 (optional) | NORMAL_SUB | combined via RNM when present |
| `roughness` | float | `SmoothnessToRoughness(SMOOTHNESS)` = `(1-s)²` | SMOOTHNESS_MAP_TYPE decides the conversion (0/1/2) |
| `metalness` | float | METALNESS | scalar |
| `f0` | float3 | `lerp(SPECULAR, max(0.02,albedo), metalness)` | the already-combined F0 from `EncodeGbuffer:701`; SPECULAR_MAP_TYPE picks `0.08x`/`0.16x²`/`saturate(specular)` |
| `ao` | float | OCCLUSION (visibility) | OCCLUSION_MAP_TYPE 0/1/2/3 (linear/sRGB, UV0/UV1) |
| `emissive` | float3 | EMISSIVE | sub-morphs (color/intensity/blink) applied to the final color or intensity |
| `emissiveIntensity` | float | EMISSIVE `emissiveIntensity` | morph-intensity exponent applied if enabled |
| `lightModel` | uint | `CUSTOM_ENABLE`, promoted to EMISSIVE if emissive > 2/255 | 0..9 (10 rejected) |
| `customA` | float | CUSTOM_A | per-model meaning (curvature/sheen/threshold/…) |
| `customB` | float3 | CUSTOM_B (sRGB→linear, optional flips) | per-model meaning (transmittance/fuzz/shadow color) |
| `specularColor`/`specularType` | — | SPECULAR_MAP_TYPE | only needed if we keep the raw spec for our own F0 build; otherwise fold into `f0` |

Slot → field summary (what can be safely discarded for a first implementation):

- ALBEDO, ALBEDO_SUB → `albedo`.
- ALPHA → `alpha`.
- NORMAL, NORMAL_SUB → `normal` (+`normalSub`).
- SMOOTHNESS → `roughness`.
- METALNESS → `metalness`.
- SPECULAR → `f0` (combined with albedo/metalness exactly as `EncodeGbuffer` does).
- OCCLUSION → `ao`.
- EMISSIVE → `emissive` + `emissiveIntensity`.
- CUSTOM_A/B → `customA`, `customB`.
- PARALLAX → out of scope per handoff §5.7 (note it exists; the local library uses it in 13–14 materials).
- `*_MAP_UV_FLIP`, `*_MAP_SWIZZLE`, `*_MAP_APPLY_SCALE`, `*_MAP_COLOR_FLIP`, `*_MAP_UV_FLIP`, `MapLoopNum`,
  `*_MAP_FILE` are **load-time instructions**: they decide which texture and which channel to read and how to
  transform the value before it reaches the fields above. They do not need to survive into the runtime record unless
  we defer decoding to the shader.

---

## 7. Local library usage counts

Command (from repo root):

```
grep -rIh --include=*.fx -E "^#define (CUSTOM_ENABLE|[A-Z_]+_MAP_FROM)" library | sort | uniq -c
```

Selected results (142 local `.fx` files carry `ALBEDO_MAP_FROM` / `CUSTOM_ENABLE`; the 163 count in the plan is the
number of `.fx` files touching the general `*_MAP_FROM` defines, e.g. all 163 set `ALPHA_MAP_FROM`):

| define | value | count |
|--------|-------|-------|
| `ALPHA_MAP_FROM` | 3 (PMX texture) | 163 |
| `ALBEDO_MAP_FROM` | 3 | 137 |
| `ALBEDO_MAP_FROM` | 1 | 4 |
| `ALBEDO_MAP_FROM` | 0 | 1 |
| `NORMAL_MAP_FROM` | 1 | 87 |
| `NORMAL_MAP_FROM` | 0 | 76 |
| `NORMAL_SUB_MAP_FROM` | 1 | 37 |
| `SMOOTHNESS_MAP_FROM` | 1 | 60 |
| `SMOOTHNESS_MAP_FROM` | 0 | 55 |
| `SMOOTHNESS_MAP_FROM` | 9 (specular power) | 27 |
| `METALNESS_MAP_FROM` | 1 | 34 |
| `SPECULAR_MAP_FROM` | 0 | 138 |
| `SPECULAR_MAP_FROM` | 1 | 4 |
| `OCCLUSION_MAP_FROM` | 1 | 36 |
| `PARALLAX_MAP_FROM` | 1 | 13 |
| `PARALLAX_MAP_FROM` | 3 | 1 |
| `EMISSIVE_MAP_FROM` | 1 | 12 |
| `EMISSIVE_MAP_FROM` | 3 | 11 |
| `CUSTOM_A_MAP_FROM` | 1 | 21 |
| `CUSTOM_A_MAP_FROM` | 3 | 3 |
| `CUSTOM_B_MAP_FROM` | 1 | 25 |
| `CUSTOM_B_MAP_FROM` | 3 | 4 |

`CUSTOM_ENABLE` values: `0` × 87, `1` × 26, `5` × 20 (one with two spaces), `8` × 5, `3` × 4.

Types: `NORMAL_MAP_TYPE` is `0` in 161, `3` in 2. `SMOOTHNESS_MAP_TYPE` is `0` in 114, `2` in 19, `1` in 9.
`SPECULAR_MAP_TYPE` is `0` in 138, `1` in 4. `OCCLUSION_MAP_TYPE` is `0` in 125, `1` in 2. `ALBEDO_SUB_ENABLE` is
`0` in 120, `1` in 22; `EMISSIVE_ENABLE` is `0` in 120, `1` in 22. `ALPHA_MAP_SWIZZLE` is `3` everywhere.

> Note: `MATCAP_MAP_FROM` also appears (the local matcap `main.fx` variant): `0` × 12, `4` × 9. Those files do not
> use the PBR material file and should be skipped by a Ray-MMD PBR probe.

---

## 8. Confirmed vs inferred vs unknown (summary)

**Confirmed from source (public master + local), citations above:**

- All `*_MAP_FROM` 0..9 meanings, including that 7/8 are PMX ambient/specular colors (not textures) — `GetAlpha`
  from 7/8 reads the color directly with no texture read — and 9 is spec-power only for smoothness/metalness;
  NORMAL/NORMAL_SUB reject 7/8/9.
- `*_MAP_TYPE`, `*_MAP_SWIZZLE`, `*_MAP_UV_FLIP`, `*_MAP_APPLY_SCALE`, `*_MAP_COLOR_FLIP`, per-slot `MapLoopNum`,
  `ALBEDO_MAP_APPLY_DIFFUSE`.
- SPECULAR_MAP_TYPE 4 is `saturate(specular)` (FROM 0) / `0.08*specularColor` (FROM != 0), not a constant 0.04;
  sampler F0 is clamped to `[0.01, 1.0]`.
- The DiffuseMap declaration hazard (FROM 3 required for the ambient/specular UV path to compile).
- ALBEDO_SUB blend modes, RNM normal-sub blend, emissive morph controls, light-mode promotion.
- G-buffer layout and decode; only decoded quantities are consumed downstream. Opaque emissive reaches bloom
  via the G-buffer (MRT3/MRT4, `DecodeGBufferEmissive`), not the main shading target.
- GGX + Schlick Fresnel + the exact geometry divisor; F0 = `lerp(specular, max(0.02,albedo), metalness)`; default
  F0 0.04.
- Glass IOR offset is world-space (`mul(normal, matViewInverse).xy`) and refraction modulates by
  `max(0.02, albedo)`.
- Opaque-pass POM ignores `PARALLAX_MAP_TYPE`.
- Multiple `material_common_2.0*.fxsub` variants exist in the local library (master hash `c7307832…`; modified
  `e7df2e3d…` with `ALPHA_THRESHOLD 0.75`; Cutout at 0.2/0.5/0.75; Hypnosis UV scroll).
- Tangents from screen derivatives.
- The 10 shading models' controller math (as restated), with IDs 0..10.

**Inferred (reasonable reading, not spelled out in one place):**

- COLOR0 render target naming (`MaterialMap` vs `Gbuffer5RT`); irrelevant to our representation.
- The GGX geometry term is an approximation rather than the canonical UE4 split-sum k.
- SUBSURFACE uses the Penner emitter in practice because the LUT/`SubsurfaceShadingScattering` function is not wired
  into `ShadingMaterial`.

**Unknown (sources do not settle):**

- Definition/behavior of `SubsurfaceShadingScattering` (referenced at `PreIntegratedSkin.fxsub:22`, not defined in the
  master tree).
- Whether `Shader/PostProcessScattering.fxsub`'s SSS blur profile (`profileVarArr`, 2 variants) is selected by the
  material's custom data; the material side does not expose that choice.
- Exact runtime meaning of `DummyScreenTex` (FROM 6) in a non-MME host.

## 9. Differences that matter for our parser

- Local `material_common_2.0.fxsub` copies are **not all** byte-identical to master
  `Materials/material_common_2.0.fxsub`. The master-identical hash (`c7307832…`, `ALPHA_THRESHOLD 0.999`) covers
  MovieSonic/BeeNic/moonlit_waves; Anyisdragon/RomEthanShax ship a modified `e7df2e3d…` file with
  `ALPHA_THRESHOLD 0.75` and cutout enforcement. The parser must read the actual included `material_common_2.0*`
  per material rather than keying to the public file.
- Local `material_common_2.0_Cutout.fxsub` is a lightly modified copy (alpha threshold/behaviour only); treat it as
  the same material semantics with `ALPHA_THRESHOLD` of `0.2`, `0.5` or `0.75` depending on the set. The
  Hypnosis variant additionally scrolls UVs by `UvMove * time`.
- Local `main.fxsub`/`main.fx` is an older matcap path that does not include the PBR material file; skip it.
- The local controller config `Other/ray.conf` enables `TOON_ENABLE 1` (master default 0), so CEL/TONEBASED materials
  in this library are intended to be toon-shaded. This is a *controller* setting, not material data.
