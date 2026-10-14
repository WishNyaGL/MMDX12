# MikuMikuEffect (MME) reference — draft for a modern renderer design

Work unit W1. A source-cited reference of how MikuMikuEffect (MME, by 舞力介入P, MMD's DirectX 9 effect
plug-in) works, so an architect can design an MME-compatible runtime for the MMDX12 D3D12 renderer. This is a
research document, not code; it changes nothing under `src/`, `shaders/`, `tools/`, `external/` or `library/`.

Every claim below is tagged:

- **[source: …]** — confirmed from a named, reachable source (path or URL given).
- **[inferred]** — a reasonable reading of the sources that no single source spells out.
- **[unknown]** — the sources collected here do not settle it.

Japanese identifiers (semantic names, annotation values, file names) are kept verbatim.

## 0. Sources and how to read the citations

### 0.1 Primary source: the MME distribution itself

MME v0.37 (the last release, 2014-03-26), obtained as the official 32-bit + 64-bit distribution and stored
under `captures/mme_lib/_mmeffect/` (see `captures/mme_lib/_mmeffect/source.json` for url / sha256 /
fetched_utc). The distribution's own docs are the authority for semantics and annotations; where this document
says **[source: REFERENCE.txt …]**, it means the file

- `captures/mme_lib/_mmeffect/src/MMEffect_x64_v037/REFERENCE.txt` (Shift-JIS; sha256
  `908cbdfff7a681ec167c54cecd5aa2c7046aa54382f75172de46a094bbd82d65`), and
- `captures/mme_lib/_mmeffect/src/MMEffect_x64_v037/MMEffect.txt` (Shift-JIS; the readme / changelog).

Section numbers quoted as “REFERENCE.txt §2.7” are the document's own numbering (it numbers sections
`0.1.0.0`, `１`, `2.1`, `2.9` …); the section heading is given alongside. The file is unmodified in the
capture; `*.utf8.txt` siblings are read-only UTF-8 copies made for this document.

MME installs as a `d3d9.dll` proxy next to `MikuMikuDance.exe`, plus `MMHack.dll` and `MMEffect.dll`
**[source: MMEffect.txt ●インストール方法]**. It adds an “MMEffect” menu to MMD and hooks MMD's D3D9
drawing. The readme states plainly that it depends on MMD's internal implementation and that non-MME effect
files “まず動きません” (mostly will not work) **[source: MMEffect.txt ●注意事項]**.

### 0.2 Secondary, open-source reference: nanoem

`hkrn/nanoem` (MPL-2.0) is an independent reimplementation of MMD's runtime and contains a from-scratch MME
(`.fx`) loader/executor derived from the D3DX9 effect binary format. It is used here **only as a corroborating
reading of observed semantics behaviour** — file paths are cited, no code is copied into this document beyond
short identifiers. All nanoem citations are relative to the repository root at `main`, fetched 2026-10-13
(latest commit `30acffaa`, 2025-03-03):

- `emapp/src/Effect.cc` — the loader, the semantic handlers, uniform writing, technique lookup.
- `emapp/src/effect/Technique.cc` — technique annotations (`MMDPass`, `Subset`, `UseTexture` …), script.
- `emapp/src/effect/Pass.cc` — pass annotations (`Script`) and the `Draw=Geometry|Buffer` commands.
- `emapp/src/effect/EffectCommon.cc` — render-state → pipeline conversion, matrix/semantic enums.
- `emapp/src/effect/GlobalUniform.cc` — the constant register buffer layout.
- `emapp/src/Project.cc`, `emapp/src/Model.cc`, `emapp/src/Accessory.cc` — the frame's draw order.
- `emapp/include/emapp/effect/Common.h`, `Technique.h` — the annotation/parameter structures.

### 0.3 The VPVP / community docs, MMM

- MikuMikuMoving's English manual, “16. Effect development”, `sites.google.com/site/mikumikumovingeng/manual/16-effect-development`
  — MMM's MME-compatibility notes and its own additions (used for §10).
- MME's own worked usage notes: `okoneya.jp/mme_study/` (“はじめての MikuMikuEffect!”) — effect assignment
  workflow and the same-name `.fx` convention (used for §2).

### 0.4 Local library (read-only examples) and existing MMDX12 code

- Real effects on disk: `library/**/*.fx`, `library/**/*.fxsub` (Ray-MMD and many others), and the existing
  Ray-MMD note `docs/raymmd_semantics.md` (link only, not duplicated here).
- Existing loader code that already reads `.emd` / `.fx` hints:
  `src/asset/MmeHints.h`, `src/asset/MmeHints.cpp`, `src/asset/MmeText.h`, `src/asset/MmeText.cpp`,
  `src/asset/PmmLoader.h` / `PmmLoader.cpp` (`MapMmeControllers`, `ClassifyMmeController`),
  `src/studio/StudioLoader.cpp`. These are cited to record what MMDX12 already parses.

### 0.5 Conventions

- “Effect” = a `.fx` / `.fxm` / `.fxsub` HLSL Effect (fx_2_0) file. In MME context `.fxsub` / `.fxm` are just
  `.fx` files included by another (MME treats them as include/candidate files; see §2 and §10).
- “Technique” and “pass” are the D3DX Effect constructs (D3DX9 `technique` / `pass`).
- “Subset” = one material of a model, in the order the model lists materials.

## 1. Architecture

### 1.1 MME as a `d3d9.dll` proxy inside MMD

MMD uses Direct3D 9. MME ships a replacement `d3d9.dll` (plus `MMHack.dll` and `MMEffect.dll`) that the user
copies next to `MikuMikuDance.exe`. Because Windows loads a DLL from the application folder before the system
copy, MMD links to MME's `d3d9.dll`, which forwards the real D3D9 calls while intercepting MMD's effect
management **[source: MMEffect.txt ●インストール方法 / ●注意事項]**. MME therefore must exactly track MMD's
internal draw calls; it is “MODツールに近いもの … MMDの内部実装に強く依存している” (closer to a mod than a
plug-in; strongly dependent on MMD's internal implementation) **[source: MMEffect.txt ●注意事項]**.

**[inferred]** For a D3D12 reimplementation this means MME compatibility is a *behaviour contract*, not a DLL:
we must reproduce the inputs MME feeds an effect (matrices, material values, draw order, script execution) so
that a D3DX9 `fx_2_0` effect compiles/behaves the same when we translate it.

### 1.2 The D3DX9 Effect framework (`fx_2_0`)

MME consumes D3DX9 Effect files as described by Microsoft's “Effect File Format” and “HLSL reference”
**[source: REFERENCE.txt ○注意 → links to msdn.microsoft.com/library/bb173005 and bb509638]**. The feature set
MME relies on is:

- `technique NAME < annotations > { pass NAME < annotations > { … } … }` — a hierarchy of techniques and passes
  **[source: REFERENCE.txt §1.1]**.
- Techniques/passes carry **annotations** written as `type name = value;` inside `< … >`; annotation names are
  case-insensitive **[source: REFERENCE.txt §1.1]**.
- Global parameters carry a **semantic** (and annotations): `type name : SEMANTIC < … >;` **[source: REFERENCE.txt §2]**.
- Shader compilation via `VertexShader = compile vs_X_Y Fn();` / `PixelShader = compile ps_X_Y Fn();`.

nanoem reads the same constructs from a Microsoft “FX9 effect” binary (the `.fxo`/FX9 representation)

**[source: nanoem `emapp/src/Effect.cc` — `Fx9__Effect__Technique`, `Fx9__Effect__Pass`, `Fx9__Effect__Parameter`,
`Fx9__Effect__Semantic`; technique/pass construction around `Effect::Effect`, lines ~1855–1900]**. It enumerates
the recognised pass-type literals as constants:

```
kPassTypeObject         = "object"
kPassTypeObjectSelfShadow = "object_ss"
kPassTypeShadow         = "shadow"
kPassTypeEdge           = "edge"
kPassTypeZplot          = "zplot"
```

**[source: nanoem `emapp/src/Effect.cc` lines 980–984]**. These match MME's `MMDPass` values exactly (§3).

### 1.3 Per-object effects vs. scene (post) effects

MME distinguishes:

- **Object effects** (`ScriptClass = "object"`): run while an object (model or accessory) is drawn, once per
  material/subset, and can replace the object's vertex/pixel shaders. This is the common “material shader”
  (Ray-MMD materials, `full.fx`, etc.).
- **Scene effects** (`ScriptClass = "scene"`): draw the *screen buffer* — pre-effects and post-effects. They
  are not bound to a material; instead MME draws them against a full-screen quad (`Draw=Buffer`) and they can
  re-render the whole scene into an off-screen target (`ScriptExternal=Color`).
- **`sceneorobject`**: both.

**[source: REFERENCE.txt §2.8 ●STANDARDSGLOBAL → ScriptClass]**. MME doc: “`object` を指定した場合、パスの
スクリプトで Draw=Buffer を実行してはならない。また、`scene` を指定した場合、 Draw=Geometry を実行しては
ならない” (with `object` you must not `Draw=Buffer`; with `scene` you must not `Draw=Geometry`) **[source:
REFERENCE.txt §2.8 ●STANDARDSGLOBAL → ScriptClass]**.

**ScriptOrder** selects *when* a scene/object effect runs: `standard` (default, draws objects), `preprocess`
(before objects; pre-effect), `postprocess` (after objects; post-effect) **[source: REFERENCE.txt §2.8 →
ScriptOrder]**. The reference adds a subtlety: “正確には、preprocessのさらに前に、postprocessの前処理
（テクニックのスクリプトの先頭から `ScriptExternal=Color` まで）が実行される” — precisely, an extra
post-process *pre-pass* (from the technique script's start up to `ScriptExternal=Color`) runs even before
`preprocess` **[source: REFERENCE.txt §2.8 → ScriptOrder]**.

### 1.4 MMD's drawing order (as MME sees it)

The `MMDPass` values are named for MMD's own draw steps **[source: REFERENCE.txt §1.2 → MMDPass]**:

| MMDPass | MMD draw step | Notes |
|---------|---------------|-------|
| `zplot` | self-shadow Z-value plot | the light's depth map (self-shadow) — draws the model transformed by the *light* view/projection; writes Z into a target |
| `object` | object body, self-shadow **off** | normal color pass without self-shadow |
| `object_ss` | object body, self-shadow **on** | color pass with self-shadow |
| `shadow` | simple (non-self) shadow | ground shadow |
| `edge` | outline / edge | PMD/PMX models only |

nanoem's frame order corroborates: `Project::drawViewport()` runs, in order, the post-process pre-pass
(`drawAllEffectsDependsOnScriptExternal`), background + grid, then `PreProcess`, then `Standard`/edge, then
`Standard`/color, then `Standard`/groundShadow, then `PostProcess` **[source: nanoem `emapp/src/Project.cc`
`Project::drawViewport()`, ~lines 2944–2966]**. The self-shadow Z-plot pass is drawn earlier (in the
shadow-camera pass) **[source: nanoem `emapp/src/Project.cc`, the shadow-camera loop ~lines 2920–2938;
`Model::drawShadowMap` in `emapp/src/Model.cc` uses `kPassTypeZplot`]**. For each model, nanoem draws
`kPassTypeObjectSelfShadow` when the self-shadow map is enabled, otherwise `kPassTypeObject` **[source: nanoem
`emapp/src/Model.cc` `Model::drawColor`, ~lines 4048–4050]**; ground shadow uses `kPassTypeShadow`
(`Model::drawGroundShadow`); edges use `kPassTypeEdge` (`Model::drawEdge`); the shadow map uses `kPassTypeZplot`
(`Model::drawShadowMap`) **[source: nanoem `emapp/src/Model.cc`]**. **[inferred]** this is the same order MMD
uses, which is why MME numbers its passes this way.

**Fallback when no technique matches:** “条件に合致するテクニックがエフェクトファイル上に存在しない場合、
MMD標準のシェーダが使用される” — if no technique matches, MMD's standard shader is used **[source:
REFERENCE.txt §1.2]**. This is the same as “no effect assigned”. nanoem models this with a built-in
`ModelProgramBundle` / `AccessoryProgramBundle` fallback that supplies `model_color`, `model_edge`,
`model_ground_shadow`, `model_zplot` shaders when no effect technique is found **[source: nanoem
`emapp/src/ModelProgramBundle.cc` includes `model_color_*`, `model_edge_*`, `model_ground_shadow_*`,
`model_zplot_*`; `Effect::findTechnique` falls back to the program bundle when nothing matches, `Effect.cc`
~1478–1497]**.

### 1.5 What the vertex shader receives (MMD CPU skinning)

MME's REFERENCE documents only `_INDEX` (§2.9) explicitly, but real effects and nanoem fix the rest of the
input layout. From nanoem's vertex attribute table the object vertex stream exposes:

| Shader semantic | Meaning |
|-----------------|---------|
| `POSITION` / `SV_POSITION` | model vertex position (already skinned on the CPU for MMD) |
| `NORMAL` | vertex normal |
| `TEXCOORD0` (`TEXCOORD`) | the material's diffuse UV (uv0) |
| `TEXCOORD1` … `TEXCOORD4` | additional UV sets (uv1..uv4) |
| `COLOR0` / `COLOR` | vertex color |

**[source: nanoem `emapp/src/Effect.cc` ~1791–1830 — `a_position`/`SV_POSITION`, `a_normal`, `a_texcoord0`…
`a_texcoord4` (TEXCOORD 0..4), `a_color0` (COLOR 0)]**. The pass wires `POSITION`→attr0, `NORMAL`→attr1,
`TEXCOORD0`→attr2 … `TEXCOORD4`→attr6, `COLOR0`→attr7 **[source: nanoem `emapp/src/effect/Pass.cc` —
`Technique::overrideScenePipelineDescription` sets the same 8-slot layout for the full-screen quad, and
`Effect.cc` maps the semantics; see also `emapp/include/emapp/effect/Common.h`]**.

**MMD CPU-skins before the effect.** MME does not give an effect bone matrices; MMD runs its own
CPU skinning and hands the effect a position already in **model/world space** for that object. Confirmation:
effects do `Out.Pos = mul(Pos, WorldViewProjMatrix)` where `Pos` is the raw `POSITION` and the model is placed
by `WorldMatrix`; e.g. `library/model/pda_miku/New/full_SimpleSoftShadow.fx` computes `Out.Eye = CameraPosition
- mul(Pos, WorldMatrix)` from the raw input, i.e. the input is pre-skinned in model space and `WorldMatrix` is
the object's world transform **[source: `library/model/pda_miku/New/full_SimpleSoftShadow.fx` Basic_VS]**.
MME itself never documents bone input; only MMM (which skins in the GPU shader) provides `BLENDWEIGHT` /
`BLENDINDICES`/SDEF inputs **[source: MMM effect-development manual §16.3]**, confirming MMD/MME passes
*already-skinned* vertices while MMM does not (§10).

The `zplot` pass likewise receives model-space `POSITION` and is transformed by the **light**
world-view-projection matrix (`WORLDVIEWPROJECTION < string Object = "Light"; >`) **[source:
`library/model/pda_miku/New/full_SimpleSoftShadow.fx` ZValuePlot_VS; REFERENCE.txt §2.1 → Object = "Light"]**.

### 1.6 Accessory (`.x`) vs model (`.pmd`/`.pmx`) differences

- **Outline (`edge`) is PMD/PMX-only.** `MMDPass="edge"` is documented as “輪郭（PMDモデルのみ）” (outline,
  PMD models only) **[source: REFERENCE.txt §1.2 → MMDPass]**.
- **Toon rendering is PMD-only-ish** in the sense of `UseToon`: “トゥーンレンダリングを使用するオブジェクト
  （＝PMDモデル）… 使用しないオブジェクト（＝アクセサリ）” — `UseToon=true` targets models, `false` targets
  accessories **[source: REFERENCE.txt §1.2 → UseToon]**. So `use_toon` is true for a PMD model and false for
  an accessory **[source: REFERENCE.txt §2.9 → use_toon; nanoem writes `use_toon` = 0 for accessories,
  `Effect.cc` accessory material path ~3263]**.
- **Material colors / textures.** `MATERIALTEXTURE`/`MATERIALSPHEREMAP`/`MATERIALTOONTEXTURE` come from the
  material; for accessories “アクセサリの場合（UseToon=falseの場合）は、正しく取得できない” (toon texture
  cannot be read correctly for accessories) and toon-less PMD materials get an all-white toon texture
  **[source: REFERENCE.txt §2.2 → MATERIALTOONTEXTURE]**. `ADDINGTEXTURE`/`MULTIPLYINGTEXTURE` (PMX material
  morphs) “アクセサリの場合 … 加算値は全て0、乗算値は全て1” (for accessories the add values are 0 and the
  multiply values are 1) **[source: REFERENCE.txt §2.2 → ADDINGTEXTURE etc.]**.
- **`CONTROLOBJECT` on a PMD model returns the identity world matrix** (models don't have an independent world
  transform; use it for `.x` accessories) **[source: REFERENCE.txt §2.6 ○補足]**.
- **MMD world matrix of models is constant/identity** — MME gives accessory transforms via `CONTROLOBJECT`
  **[source: REFERENCE.txt §2.6 ○補足]**.

### 1.7 The default effect when no `.fx` is assigned

If an object has no effect file, MMD's standard shaders draw it and MME plays no part **[source: MMEffect.txt
(1) “エフェクトファイルの無いモデルファイルを読み込んだ場合、従来どおりに表示されます”]**. Within an effect
file, a technique with no matching pass gives the same result (§1.4, §4.2). A pass with **no `VertexShader`
/`PixelShader` set** also falls back to MMD's standard shader: “パスのデフォルトのシェーダはMMD標準のシェーダ
であるため … そのパスの描画にはMMD標準のシェーダが使用される” **[source: REFERENCE.txt §4 ●MMD標準のシェーダ
を使用する]**. An **empty technique** draws nothing: “テクニックを空にすると、そのテクニックでは一切描画が
行われなくなる” (an empty technique suppresses that draw) **[source: REFERENCE.txt §4 ●空のテクニック]**.

## 2. Effect assignment

### 2.1 `MMEffect.ini` / `default.fx`

**[unknown]** No `MMEffect.ini`, `default.fx`, “EffectFile tab”, or a *file* named `MMEffect.ini` appears in the
v0.37 distribution, in REFERENCE.txt, or in nanoem. MME stores its per-object assignments inside MMD's own
configuration and in `.emd`/`.emm` sidecars, not in an `MMEffect.ini`:
- `MMEffect.txt` describes the GUI (“エフェクト割当” dialog) and saving assignments to `*.emm` and `*.emd`
  **[source: MMEffect.txt ●エフェクトファイル使用方法 (2) → 割り当てをファイルに保存するには / ○default設定について]**.
- The plan's `MMEffect.ini` / `default.fx` naming is not confirmed by any collected source; treat as
  **[unknown]**. The *concept* of a default assignment does exist: the dialog has a “(default)” row whose
  setting is applied to every newly loaded object that has no explicit/same-name effect **[source: MMEffect.txt
  ○default設定について]**.

### 2.2 Same-name `.fx` next to `.x`/`.pmx`, and the `[effect.fx]` name embedding

MME auto-assigns an effect if the effect file sits beside the model with the **same stem**:

```
UserFile\Model\初音ミク.pmd
UserFile\Model\初音ミク.fx        ← effect file, auto-applied
UserFile\Accessory\negi.x
UserFile\Accessory\negi.fx       ← effect file, auto-applied
```

Alternatively the effect name can be **embedded in the model file name**: `初音ミク[effect.fx].pmd` uses
`UserFile\Model\effect.fx` **[source: MMEffect.txt ●エフェクトファイル使用方法 (1) ファイル名で指定する方法]**.
If no effect is found the object draws as usual **[source: MMEffect.txt (1)]**. This same-name convention is
echoed by the community guide **[source: okoneya.jp/mme_study, シェーダの割り当て §1]**.

**[source: local]** MMDX12's `ReadMmeHints` does *not* implement the same-name `.fx` convention; it follows a
`.emd` sidecar only (`src/asset/MmeHints.cpp`, `ReadMmeHints`). That is out of scope for this document (we are
describing MME, not changing the loader).

### 2.3 `.emd` — per-model assignment sidecar

A `.emd` is a small INI-like text file saved per model/accessory; if saved with the **same name as the
model**, MME loads it automatically, just like a same-name `.fx` **[source: MMEffect.txt changelog 0.24:
“モデル単位でのエフェクト割当をファイル（*.emd)に保存・読込する機能を追加 … emdファイルをモデル・アクセ
サリと同じ名前で保存しておくと、fxファイルと同様、使用時に自動的に読み込まれます”]**. MMDX12 already parses
it **[source: `src/asset/MmeHints.cpp` `ReadMmeHints` reads `[Effect]` `Obj` / `Obj[n]` entries; header
`src/asset/MmeHints.h`]**.

Syntax, confirmed from the local library (read-only) and MMDX12's parser:

```
[Info]
Version = 3

[Effect]
Obj = RayMaterial\FX\SeriousShader.fx     ; effect for the whole object
Obj.show = true                           ; object visibility
Obj[0] = RayMaterial\FX\SeriousShader.fx  ; per-subset (material) effect, index = material number
Obj[1] = RayMaterial\FX\SeriousShader.fx
...
```

**[source: `library/by StarNoodle495/AlcanderMMD/Alcander.emd`; `.../RayMaterial/RayMain.emd`;
`.../RayMaterial/RayMaterialMap.emd` (the latter assigns different `.fx` per subset, e.g. `head.fx`,
`eyes.fx`, `body.fx`)]**. Notes:

- `[Info]` `Version` is `3` in every local sample.
- `Obj` (no index) is the whole-object assignment; `Obj[n]` overrides it for material `n`.
- `Obj.show` is the object's show/hide state (`true`/`false`) **[source: local `.emd` files; consistent with
  MMEffect.txt ●エフェクト割当 → “オブジェクトを非表示にするには … (hide)”]**.
- Paths are relative to the `.emd`; both `\` and `/` appear; values `(none)` / `none` / `(hide)` mean “no
  effect” / “hidden” **[source: MMEffect.txt GUI states (none)/(hide); MMDX12 treats a `none` value as no
  effect, `src/asset/MmeHints.cpp` `ReadMmeHints`]**.
- **[inferred]** Subset indices are the model's material indices, in model order (the GUI “サブセット展開”
  expands materials in that order **[source: okoneya.jp/mme_study §1-3]**).

### 2.4 `.emm` — per-scene assignment file

An `.emm` stores the whole scene's assignment state (every object, plus off-screen render-target tags), saved
and loaded from the MMEffect file menu; it is auto-saved with the `.pmm` when “割当ファイル自動保存” is on and
auto-loaded when a `.pmm` is read **[source: MMEffect.txt ●エフェクトファイル使用方法 (2) → 割り当てをファイル
に保存するには; changelog 0.20, 0.28, 0.30 (auto-save default ON from 0.30), 0.36]**. Relative paths inside
`.emm` were fixed to not begin with a `\` in 0.25 **[source: MMEffect.txt changelog 0.25]**; older `.emm`
versions were made readable in 0.31 **[source: MMEffect.txt changelog 0.31]**. **[unknown]** the exact `.emm`
line grammar: no `.emm` sample exists in the local library and REFERENCE.txt does not specify it. The GUI and
changelog imply it has the same `[Info]`/`[Effect]`-style sections as `.emd` plus off-screen tabs, but this is
**not verified**.

### 2.5 Subset (`Obj[n]`) and material index ranges

“サブセット” = a model material. Per-subset assignment is exposed as `Obj[n]` in `.emd` **[source: local `.emd`
files]** and via the GUI's “サブセット展開” **[source: MMEffect.txt; okoneya.jp/mme_study §1-3]**.

Within an effect **file**, subset targeting is done with the technique annotation `string Subset`, whose
syntax is a comma-separated list of indices and ranges:

- `"0,3,5"` — explicit list.
- `"6-10"` — inclusive range.
- `"12-"` — from 12 to the last material.
- Omitted ⇒ applies to **all** subsets.
- Example from the reference: `string Subset = "0-6,8";`

**[source: REFERENCE.txt §1.2 → string Subset]**. nanoem parses the same grammar: comma-separated, each term
either `a-b` (a range, clamped, `a-` extends to the last material) or a single index **[source: nanoem
`emapp/src/Effect.cc` `Effect::parseSubset` / `PrivateEffectUtils::parseSubsetString`, ~lines 912–946,
1222–1234]**. When **several techniques** match a subset, the **first one in file order wins** **[source:
REFERENCE.txt §1.2: “条件の照合は、エフェクトファイルに書かれている順に行われる …先に記述されているものが
優先される”]**.

### 2.6 The effect registered as an accessory (the `.x` “controller” pattern)

Many MME effects are scene/post effects that need a “handle” the user can position, scale, or toggle in MMD's
UI. The convention is a tiny placeholder accessory model (`.x`), often called a *controller*, placed in the
scene with the actual effect being an object effect on that `.x`; the effect reads the accessory's panel values
via `CONTROLOBJECT` (`name = "<file>.x"`, `item = "X"/"Y"/"Z"/"XYZ"/"Rx"/"Ry"/"Rz"/"Rxyz"/"Si"/"Tr"`, or a
bone/morph name) **[source: REFERENCE.txt §2.6 ●CONTROLOBJECT]**. The accessory may have a same-name `.fx` and
an `.emd`.

MMDX12's PMM loader already recognises this at the PMM level, classifying accessory file names into the ~10
common controller families and mapping their transform/`Si`/`Tr` onto native settings
**[source: `src/asset/PmmLoader.h` `MmeController` enum, `MapMmeControllers`; `src/asset/PmmLoader.cpp`
`ClassifyMmeController` lines 502–526, `MapMmeControllers` lines 556–613; used from `src/studio/StudioLoader.cpp`
line 493]**. Large accessory scale/offsets are normal: `CONTROLOBJECT` scaling is the panel Size ×10, obtained
internally as `length(world_matrix._11_12_13)` **[source: REFERENCE.txt §2.6 ○補足]**.

**[inferred]** `o_*` effect names (e.g. `o_Diffusion.x`, `o_Tonemap*`, `o_LightBloom`, `o_SelfOverlay`) and
names containing “controller” are the community's controller convention, which MMDX12 encodes in
`LooksLikeUnknownMmeController` **[source: `src/asset/PmmLoader.cpp` lines 543–554]**.

## 3. Technique selection

### 3.1 The `MMDPass` annotation

A technique declares which MMD draw step it replaces with `string MMDPass = "<value>";` **[source:
REFERENCE.txt §1.2 → MMDPass]**. Values (exact strings):

| Value | Meaning (MME) | Model vs accessory notes |
|-------|---------------|--------------------------|
| `"object"` | object body, self-shadow **OFF** | default if the annotation is omitted |
| `"zplot"` | self-shadow Z-value plot (light-space depth) | the light view/projection matrices are valid here |
| `"object_ss"` | object body, self-shadow **ON** | |
| `"shadow"` | simple non-self shadow (ground shadow) | |
| `"edge"` | outline | PMD models only |

**[source: REFERENCE.txt §1.2 → MMDPass]**; corroborated by nanoem's `kPassType*` constants **[source: nanoem
`emapp/src/Effect.cc` 980–984]**. “アノテーション省略時には、`object` を指定したとみなされる” — omitting
`MMDPass` means `object` **[source: REFERENCE.txt §1.2 → MMDPass]**.

**Selection rule.** MME scans the effect file's techniques in order and picks the first whose `MMDPass`,
`Subset` and use-flags all match the current draw step and material; otherwise MMD's standard shader is used
**[source: REFERENCE.txt §1.2]**. nanoem implements the same scan: it groups techniques by `MMDPass` and, for a
draw call, walks them in order, testing `Subset` then the `UseTexture`/`UseSphereMap`/`UseToon` predicates
**[source: nanoem `emapp/src/Effect.cc` `Effect::internalFindTechnique` ~4466–4550; `Technique::match` in
`emapp/src/effect/Technique.cc`]**. For `object_ss`, if no `object_ss` technique matches, nanoem retries
`object` before falling back **[source: nanoem `emapp/src/Effect.cc` `Effect::findTechnique` — `candidateTypes[]`
= `{passType, passType=="object_ss" ? "object" : "", ""}` ~1411–1444]**; **[inferred]** MMD behaves the same
(a self-shadow technique is optional, `object` is the fallback).

### 3.2 Self-shadow Z-plot note

For `zplot`/`shadow` the different draw steps need different pipelines: nanoem binds the shadow camera's pass
for `zplot` and the shadow depth/stencil state for `shadow` **[source: nanoem `emapp/src/Effect.cc`
`internalFindTechnique` — `m_project->setCurrentRenderPass(m_project->shadowCamera()->pass())` for `zplot`,
`Project::setShadowDepthStencilState(...)` for `shadow`, ~4484–4489]**.

### 3.3 Technique use-flags: `UseTexture`, `UseSphereMap`, `UseToon`

These are **bool technique annotations** that gate a technique on the material's properties **[source:
REFERENCE.txt §1.2]**:

| Annotation | `true` means | `false` means | Omitted |
|------------|--------------|---------------|---------|
| `bool UseTexture` | only subsets that have a texture | only subsets without | ignored |
| `bool UseSphereMap` | only subsets that use a sphere map (including a PMX sphere-mode subtexture) | only subsets without | ignored |
| `bool UseToon` | only objects using toon rendering (= PMD models) | only objects without (= accessories) | ignored |

**[source: REFERENCE.txt §1.2 → UseTexture / UseSphereMap / UseToon]**. Caveat from the reference: “MMDPass =
`object`,`object_ss` 以外のテクニックでは、UseTexture,UseSphereMap,UseToonは正しく機能しない” (these flags only
work for `object`/`object_ss`) **[source: REFERENCE.txt §1.2 ○補足]**. nanoem treats them as **tri-state**
(absent = `-1` = ignored, else boolean) **[source: nanoem `emapp/src/effect/Technique.cc` `Technique` ctor —
`triBooleanAnnotation(kUseTextureKeyLiteral, -1)` etc.; `Technique::match`]**.

**Real-world use.** `full.fx`-derived material shaders enumerate eight techniques per draw step (combinations
of texture/sphere/toon): `MainTec0`…`MainTec7` for `object` and `MainTecBS0`…`MainTecBS7` for `object_ss`
**[source: `library/model/pda_miku/New/full_SimpleSoftShadow.fx` (MainTec0..7, MMDPass="object", UseTexture/
UseSphereMap/UseToon combinations); `library/MLTD/Cherry Colored Love/stage051/Shader/MES40.fxsub`
`MES40_Technique(...)` macro generating MainTecBS0..7]**. The library carries 21 such 8-way `object_ss` groups
and 21 `object` groups **[source: grep of `library/**` for `MMDPass`, 2026-10-13]**.

### 3.4 `Subset` and fallback when nothing matches

Covered in §2.5. The fallback chain per draw step is: matching technique in file order → `object` (if the step
was `object_ss`) → MMD standard shader **[source: REFERENCE.txt §1.2; nanoem `Effect::findTechnique`]**.

### 3.5 Other technique annotations MME recognises

Besides `Subset`, `MMDPass`, `UseTexture`, `UseSphereMap`, `UseToon`, every technique/pass may carry the
**`string Script`** annotation (§7). The reference does not list any other technique annotation. Passes carry
their own `Script` and the D3D render states (§8).

### 3.6 `UseSelfShadow`

**[unknown]** The plan asks to verify `UseSelfShadow`. It does **not** appear in REFERENCE.txt's technique
annotation list **[source: REFERENCE.txt §1.2 — only Subset, MMDPass, UseTexture, UseSphereMap, UseToon]**,
and nanoem recognises no `UseSelfShadow` annotation **[source: nanoem `emapp/src/effect/Technique.cc` — only
`kSubsetKeyLiteral`, `kMMDPassKeyLiteral`, `kUseTextureKeyLiteral`, `kUseSphereMapKeyLiteral`,
`kUseToonKeyLiteral`]**. Self-shadow is selected by the `object_ss` / `zplot` MMDPass values, not by an
annotation. Treat `UseSelfShadow` as **not an MME annotation**.

## 4. Semantics

A parameter's semantic is written after `:` and before the optional `< annotations >`:
`type name : SEMANTIC < … >;` **[source: REFERENCE.txt §2]**. Semantic and annotation names are
case-insensitive **[source: REFERENCE.txt §2, §1.1]**. MME also accepts a set of **special parameter names**
that take values automatically with **no semantic** (§5) **[source: REFERENCE.txt §2.9 ●特殊パラメータ]**.

Each row below gives: the semantic, its type, its annotations, what the value is, and how often it updates.
“update frequency” is **[inferred]** from the meaning and from nanoem's writing sites unless the source says
otherwise.

### 4.1 Geometry transform matrices

| Semantic | Type | Annotation | Value |
|----------|------|-----------|-------|
| `WORLD` | `float4x4` | `Object` (optional) | world matrix |
| `VIEW` | `float4x4` | `Object` (optional) | view matrix |
| `PROJECTION` | `float4x4` | `Object` (optional) | projection matrix |
| `WORLDVIEW` | `float4x4` | `Object` (optional) | world × view |
| `VIEWPROJECTION` | `float4x4` | `Object` (optional) | view × projection |
| `WORLDVIEWPROJECTION` | `float4x4` | `Object` (optional) | world × view × projection |

**[source: REFERENCE.txt §2.1]**. Suffixes build variants of each of the six:

- `…INVERSE` → the inverse matrix (`WORLDINVERSE`, `VIEWINVERSE`, … `WORLDVIEWPROJECTIONINVERSE`).
- `…TRANSPOSE` → the transpose (`WORLDTRANSPOSE`, … `WORLDVIEWPROJECTIONTRANSPOSE`).
- `…INVERSETRANSPOSE` → transpose of the inverse (`WORLDINVERSETRANSPOSE`, …).

**[source: REFERENCE.txt §2.1]**.

**`Object` annotation** (optional, on `VIEW…` / `PROJECTION…` and their combos): `"Camera"` (default) means
the camera is the viewpoint; `"Light"` means the light is the viewpoint (used by `zplot`/self-shadow, and it
is *the same matrix MMD uses for its self-shadow processing*). If MMD's self-shadow display is fully turned
off, the `"Light"` matrix cannot be read correctly **[source: REFERENCE.txt §2.1 → ○アノテーション and
○補足]**.

**Update frequency:** once per draw call, from the active camera or the light; `Object="Light"` only while the
self-shadow facility is active **[source: REFERENCE.txt §2.1 ○補足]**. nanoem computes these from the
camera/light at `setCameraParameters`/`setLightParameters` time **[source: nanoem `emapp/src/Effect.cc`
`Effect::setCameraParameters` / `Effect::setLightParameters`; matrix combination in
`emapp/src/effect/EffectCommon.cc` `MatrixUniform::multiply`]**.

### 4.2 Light and material colours

| Semantic | Type | Annotation `Object` (required) | Value |
|----------|------|--------------------------------|-------|
| `DIFFUSE` | `float3`/`float4` | `"Light"` or `"Geometry"` | diffuse (diffuse reflection) colour |
| `AMBIENT` | `float3`/`float4` | `"Light"` or `"Geometry"` | ambient colour |
| `EMISSIVE` | `float3`/`float4` | `"Geometry"` only (no light emissive) | emission colour |
| `SPECULAR` | `float3`/`float4` | `"Light"` or `"Geometry"` | specular colour |
| `SPECULARPOWER` | `float` | `"Geometry"` only | specular power (shininess) |
| `TOONCOLOR` | `float3`/`float4` | `"Geometry"` only | toon shadow colour |
| `EDGECOLOR` | `float3`/`float4` | `"Geometry"` only | outline colour |
| `GROUNDSHADOWCOLOR` | `float3`/`float4` | (none) | ground shadow colour |

**[source: REFERENCE.txt §2.2]**. Details:

- Colour components are (R,G,B,A); `float3` drops alpha **[source: REFERENCE.txt §2.2]**.
- `Object` is **required**; `"Geometry"` = the object's material colour, `"Light"` = the light colour.
  `SPECULARPOWER`, `EMISSIVE` and `TOONCOLOR` have no light colour, so `Object="Light"` is invalid for them
  **[source: REFERENCE.txt §2.2 → ○アノテーション]**.
- `TOONCOLOR` is the shadow colour of PMD toon rendering — specifically the colour at the **bottom-left corner
  of `toon??.bmp`** **[source: REFERENCE.txt §2.2]**.
- `EDGECOLOR` is only correct in an `MMDPass="edge"` technique; `GROUNDSHADOWCOLOR` only in
  `MMDPass="shadow"`; all other colour values are not correct in `zplot`/`edge` techniques **[source:
  REFERENCE.txt §2.2 ○補足]**.

**Update frequency:** per material (geometry colours) and once per frame (light colours). nanoem writes light
colours from the global light and geometry colours from the current material **[source: nanoem
`emapp/src/Effect.cc` `Effect::setLightParameters`, `Effect::setMaterialParameters`; uniform lists
`m_light*Uniforms`, `m_material*Uniforms`]**.

### 4.3 Light/camera position and direction

| Semantic | Type | Annotation `Object` (required) | Value |
|----------|------|--------------------------------|-------|
| `POSITION` | `float3`/`float4` | `"Camera"` or `"Light"` | world-space position |
| `DIRECTION` | `float3`/`float4` | `"Camera"` or `"Light"` | world-space direction |

**[source: REFERENCE.txt §2.3 → ●POSITION / ●DIRECTION]** (the reference numbers it 2.3, between the light
section and the screen section). MMD's light is directional, so the light's “position” is the point at infinity
opposite the light direction **[source: REFERENCE.txt §2.3 ○補足: “ライトの位置座標は、ライトの向きと逆方向の
無限遠点になる”]**. **Update frequency:** once per pass from the active camera/light **[inferred]**; nanoem
writes them in `setCameraParameters`/`setLightParameters` **[source: nanoem `emapp/src/Effect.cc`]**.

### 4.4 Material textures

| Semantic | Type | Annotation | Value |
|----------|------|-----------|-------|
| `MATERIALTEXTURE` | `texture`/`texture2D` | none | the material's diffuse texture |
| `MATERIALSPHEREMAP` | `texture`/`texture2D` | none | the material's sphere map |
| `MATERIALTOONTEXTURE` | `texture`/`texture2D` | none | the material's toon map |

**[source: REFERENCE.txt §2.2 → MATERIALTEXTURE / MATERIALSPHEREMAP / MATERIALTOONTEXTURE]**. Notes: not
correct in `zplot`/`edge`; `MATERIALTOONTEXTURE` not correct for accessories, and a toon-less PMD material
gets an all-white toon texture **[source: REFERENCE.txt §2.2 → MATERIALTOONTEXTURE ○補足]**. `MATERIALSPHEREMAP`
is bound to the sphere texture; **[inferred]** in a host without a real MME material binding, these are simply
the model's diffuse/sphere/toon images (nanoem binds the material's images, `emapp/src/Effect.cc`
`setMaterialParameters` and the sphere-map override path ~3235–3265).

### 4.5 Screen information

| Semantic | Type | Value |
|----------|------|-------|
| `VIEWPORTPIXELSIZE` | `float2` | render-target screen size in pixels |

**[source: REFERENCE.txt §2.3 ●VIEWPORTPIXELSIZE]**. It is the size of MMD's screen **or** the off-screen
render target; it does **not** change when a script's `RenderColorTarget` switches the render target; internally
it reads the viewport's `.Width`/`.Height` **[source: REFERENCE.txt §2.3 ●VIEWPORTPIXELSIZE ○補足]**.
**Update frequency:** once per frame **[inferred]**.

### 4.6 Time

| Semantic | Type | Annotation | Value |
|----------|------|-----------|-------|
| `TIME` | `float` | `bool SyncInEditMode` (optional, default `false`) | play time from frame 0, seconds |
| `ELAPSEDTIME` | `float` | `bool SyncInEditMode` (optional) | time since the previous draw, seconds |

**[source: REFERENCE.txt §2.4 ●TIME / ●ELAPSEDTIME]**. `TIME` at frame 0 is `0.0`, at frame 45 `1.5`.
`ELAPSEDTIME` at 60 fps AVI output is `1/60` **[source: REFERENCE.txt §2.4]**. `SyncInEditMode`:

- `true` → the value follows the *frame* time even in edit mode (so animations pause when playback is paused).
- `false` (default) → in edit mode the value uses *system* time instead, so animations keep running while
  paused **[source: REFERENCE.txt §2.4 → bool SyncInEditMode]**.
- With `SyncInEditMode=true`, `ELAPSEDTIME` can be 0 or negative when the user scrubs frames **[source:
  REFERENCE.txt §2.4 ○補足]**.

**Update frequency:** every frame. nanoem splits these into `m_timeUniforms` (frame-locked) vs
`m_systemTimeUniforms` (system time) depending on `SyncInEditMode`, likewise `m_elapsedTimeUniforms` vs
`m_elapsedSystemTimeUniforms` **[source: nanoem `emapp/src/Effect.cc` `handleTimeSemantic` /
`handleElapsedTimeSemantic`, and the write sites ~2720–2748]**.

### 4.7 Mouse

| Semantic | Type | Value |
|----------|------|-------|
| `MOUSEPOSITION` | `float2` | cursor position, screen centre = (0,0), bottom-left = (-1,-1), top-right = (1,1) |
| `LEFTMOUSEDOWN` | `float4` | (last-press x, last-press y, currently-down 0/1, TIME at last press) |
| `MIDDLEMOUSEDOWN` | `float4` | as above, middle button |
| `RIGHTMOUSEDOWN` | `float4` | as above, right button |

**[source: REFERENCE.txt §2.5 ●MOUSEPOSITION / LEFTMOUSEDOWN / MIDDLEMOUSEDOWN / RIGHTMOUSEDOWN]**. The xy
coordinate convention matches post-projection vertex coordinates **[source: REFERENCE.txt §2.5]**. **Update
frequency:** every frame / on input **[inferred]**; nanoem writes these from the project cursor state
**[source: nanoem `emapp/src/Effect.cc` ~2750–2782]**. MMM does **not** support any of these **[source: MMM
manual §16.1]**.

### 4.8 `CONTROLOBJECT`

| Aspect | Value |
|--------|-------|
| Semantic | `CONTROLOBJECT` |
| Type | `bool`, `float`, `float3`, `float4` or `float4x4` |
| Annotation `string name` (required) | object file name (no folder path); special names `"(self)"`, `"(OffscreenOwner)"` |
| Annotation `string item` (optional) | bone name, morph name, or one of `X`, `Y`, `Z`, `XYZ`, `Rx`, `Ry`, `Rz`, `Rxyz`, `Si`, `Tr` |

**[source: REFERENCE.txt §2.6 ●CONTROLOBJECT]**. Type → value:

- `bool` — is the object visible.
- `float` — the object's scaling value.
- `float3`/`float4` — the object's position (offset).
- `float4x4` — the object's world matrix.

**[source: REFERENCE.txt §2.6]**. `item` special values (accessory panel, except bone/morph):

| `item` | Value | Type |
|--------|-------|------|
| `X`/`Y`/`Z` | accessory position component (panel X/Y/Z) | `float` |
| `XYZ` | accessory position | `float3` |
| `Rx`/`Ry`/`Rz` | accessory rotation component (panel Rx/Ry/Rz), **in radians** | `float` |
| `Rxyz` | accessory rotation | `float3` |
| `Si` | accessory size (panel Si), **×10** | `float` |
| `Tr` | accessory transparency (panel Tr) | `float` |
| *bone name* | PMD model bone position or world matrix | `float3`/`float4`/`float4x4` |
| *morph name* | PMD model morph value | `float` |

**[source: REFERENCE.txt §2.6 → item]** (note: the reference's `Tr` line mislabels the panel as Si; the value
is transparency). Special object names: `"(self)"` = the object the effect is assigned to; `"(OffscreenOwner)"`
= the owner object of the off-screen render target the effect declares (only during off-screen rendering)
**[source: REFERENCE.txt §2.6 → name]**.

Selection rules and defaults:

- If several objects share the name: (1) the one drawn **before** the referencing object and **closest** to it
  in draw order, or (2) the **last** in draw order **[source: REFERENCE.txt §2.6 ○補足]**.
- If the named object does not exist, defaults are set: `.x` → scale `10`, offset `(0,0,0,1)`, world matrix = a
  scale matrix (×10 each axis), item value `0`; PMD → scale `1`, offset `(0,0,0,1)`, world matrix = identity,
  bone position `(0,0,0,1)`, bone matrix identity, morph `0` **[source: REFERENCE.txt §2.6 ○補足]**.
- Since 0.20 the referenced object may be drawn *after* the referencing object; values are still available
  **[source: REFERENCE.txt §2.6 ○補足]**; but an object with no vertices (e.g. `ダミーボーン.pmd`) yields no
  position **[source: REFERENCE.txt §2.6 ○補足]**.

**Update frequency:** per pass, from the current scene state **[inferred]**; nanoem writes these and also
`VertexCount`, `SubsetCount`, `transp`, `opadd` in `setAllModelParameters`/`setAllAccessoryParameters`
**[source: nanoem `emapp/src/Effect.cc` ~2900–2948]**.

### 4.9 `RENDERCOLORTARGET`, `RENDERDEPTHSTENCILTARGET`

| Semantic | Type | Annotations | Value |
|----------|------|-------------|-------|
| `RENDERCOLORTARGET` | `texture`/`texture2D` | `Width`/`Height`/`Depth`, `int2 Dimensions`, `float2 ViewportRatio`, `string Format`, `int Miplevels`/`Levels` | a colour surface usable as a script `RenderColorTarget` and as a `tex2D` input |
| `RENDERDEPTHSTENCILTARGET` | `texture`/`texture2D` | `Width`/`Height`/`Depth`, `int2(int3) Dimensions`, `float2 ViewportRatio`, `string Format` | a depth-stencil surface; usable as `RenderDepthStencilTarget`; **cannot be read back** after rendering |

**[source: REFERENCE.txt §2.7 ●RENDERCOLORTARGET / ●RENDERDEPTHSTENCILTARGET]**. Colour target default size =
`ViewportRatio {1.0,1.0}`; default format `A8R8G8B8`; `Miplevels`/`Levels` `1` or `0` (0 = full chain, 1 = no
mips, the default). Depth-stencil default `ViewportRatio {1.0,1.0}`, default format `D24S8` **[source:
REFERENCE.txt §2.7]**. These update only when the script switches targets (§7) **[inferred]**; nanoem creates
them when the effect loads and binds them via the script commands **[source: nanoem `emapp/src/Effect.cc`
`handleRenderColorTargetSemantic` / `handleRenderDepthStencilTargetSemantic`, ~4096–4164]**.

### 4.10 `OFFSCREENRENDERTARGET`

| Aspect | Value |
|--------|-------|
| Semantic | `OFFSCREENRENDERTARGET` |
| Type | `texture`/`texture2D` |
| Annotations | `Width`/`Height`/`Depth`, `int2 Dimensions`, `float2 ViewportRatio`, `string Format`, `int Miplevels`/`Levels`, `float4 ClearColor`, `float ClearDepth`, `bool AntiAlias` (default false), `string Description`, `string DefaultEffect` |

**[source: REFERENCE.txt §2.7 ●OFFSCREENRENDERTARGET]**. Declaring this makes **all objects automatically
re-render into this target** with the given conditions, and the result is readable with `tex2D` like any
texture **[source: REFERENCE.txt §2.7 ●OFFSCREENRENDERTARGET]**. `Description` is the text shown in the effect
assignment GUI **[source: REFERENCE.txt §2.7 → Description]**. `DefaultEffect` assigns effects for the
off-screen pass:

```
"(object file name)=(effect file name);"
```

repeated per object, first match wins, with `*`/`?` wildcards; special object name `self`; special effect names
`none` (no effect), `hide` (invisible), and `main_default` (use the same object-file-path-based auto assignment,
`.fx`/`.emd`, as the main screen) **[source: REFERENCE.txt §2.7 → DefaultEffect]**. Example from the reference:
`DefaultEffect = "self=hide; Mirror*.x=hide; *=MirrorObject.fx;"` **[source: REFERENCE.txt §2.7]**.

**[source: local example]** An off-screen Decal-type usage appears in MES40:
`texture MirrorRT: OFFSCREENRENDERTARGET < string Description = "…"; int Width = 256; int Height = 256;
float4 ClearColor = {1,1,1,1}; float ClearDepth = 1.0; bool AntiAlias = true; string DefaultEffect =
"self = hide; Mirror*.x = hide; *=MirrorObject.fx;"; >;` **[source: REFERENCE.txt §2.7 example, mirrored in
`library/MLTD/.../MES40.fxsub` patterns]**.

nanoem parses `DefaultEffect` into a condition list and stores `ClearColor`/`ClearDepth` per target
**[source: nanoem `emapp/src/Effect.cc` `handleOffscreenRenderTargetSemantic` ~4216–4285;
`emapp/src/effect/OffscreenRenderTargetOption.cc`]**.

### 4.11 `ANIMATEDTEXTURE`

| Aspect | Value |
|--------|-------|
| Semantic | `ANIMATEDTEXTURE` |
| Type | `texture`/`texture2D` |
| Annotations | `string ResourceName` (required), `float Offset` (optional, seconds, default 0), `float Speed` (optional, default 1), `string SeekVariable` (optional) |
| Formats | `.gif` (animated GIF) and `.png` (APNG) |

**[source: REFERENCE.txt §2.7 ●ANIMATEDTEXTURE]**. By default it animates with frame time
(`TIME<SyncInEditMode=true>`); `Offset` delays the start, `Speed` scales playback, and `SeekVariable` names a
parameter whose value drives the seek instead **[source: REFERENCE.txt §2.7 ●ANIMATEDTEXTURE]**. Frame drops
occur if MMD's draw fps is below the animation's fps **[source: REFERENCE.txt §2.7 ○補足]**. **Update
frequency:** every frame. nanoem decodes APNG and stores seek variable/offset/speed **[source: nanoem
`emapp/src/Effect.cc` `handleAnimatedTextureSemantic`; `emapp/src/effect/AnimatedImageContainer.cc`]**.

### 4.12 `TEXTUREVALUE`

| Aspect | Value |
|--------|-------|
| Semantic | `TEXTUREVALUE` |
| Type | `float4` 2-D (`[height][width]`) or 1-D array |
| Annotation | `string TextureName` (required) |
| Purpose | read a texture's texels into constant registers for the vertex shader (VTF substitution) |

**[source: REFERENCE.txt §2.8 (labelled 2.8 in the reference) ●TEXTUREVALUE]**. Limits: ~200 texels; array
size should match the texture; values are read **at the start of the frame**, so mid-frame updates lag one
frame **[source: REFERENCE.txt §2.8 ●TEXTUREVALUE ○補足]**. **[inferred]** very rarely used; no local example.

### 4.13 `STANDARDSGLOBAL` and `ADDINGTEXTURE` etc.

| Semantic | Type | Annotations | Value |
|----------|------|-------------|-------|
| `STANDARDSGLOBAL` | `float` (value `0.8`) | `ScriptOutput`, `ScriptClass`, `ScriptOrder`, `Script` | SAS version + file-wide annotations (§7) |
| `ADDINGTEXTURE` | `float4` | none | PMX texture material-morph add value |
| `MULTIPLYINGTEXTURE` | `float4` | none | PMX texture material-morph multiply value |
| `ADDINGSPHERETEXTURE` | `float4` | none | PMX sphere-map material-morph add value |
| `MULTIPLYINGSPHERETEXTURE` | `float4` | none | PMX sphere-map material-morph multiply value |

**[source: REFERENCE.txt §2.8 ●STANDARDSGLOBAL; §2.2 ●ADDINGTEXTURE…MULTIPLYINGSPHERETEXTURE]**. The four
morph values are correct only in `MMDPass="object_ss"`; for accessories the add values are 0 and the multiply
values are 1 **[source: REFERENCE.txt §2.2 → ADDINGTEXTURE etc. ○補足]**. `STANDARDSGLOBAL` must be a `float`
parameter named `Script` whose value is `0.8` **[source: REFERENCE.txt §2.8 ●STANDARDSGLOBAL]**.

### 4.14 Vertex-shader `_INDEX`

| Semantic | Type | Value |
|----------|------|-------|
| `_INDEX` | `int` | vertex index (a vertex-shader input parameter only) |

**[source: REFERENCE.txt §2.9 ●_INDEX]**. It is converted to `float` in transit, so indices above 2^24 lose
precision **[source: REFERENCE.txt §2.9 ●_INDEX ○補足]**. Unlike `POSITION`/`NORMAL`/`TEXCOORD`, `_INDEX` is
not a vertex-stream element; it is a vertex-shader-only semantic. **[inferred]** nanoem does not map `_INDEX`
to a vertex attribute in its 8-slot table **[source: nanoem `emapp/src/Effect.cc` ~1791–1830 — no `_INDEX`]**.

### 4.15 Semantics that are *not* MME semantics

The plan lists several candidates that this research cannot confirm:

- `TEXCOORD1..4`, `_INDEX` as ordinary vertex inputs: `TEXCOORD1..4` are vertex-stream UV sets (§1.5), not
  parameter semantics; `_INDEX` is a parameter semantic (§4.14). **[source: nanoem attribute table; §1.5]**
- `EDGEWIDTH`: **[unknown]** not in REFERENCE.txt's semantic list; the parameter table lists no `EDGEWIDTH`
  semantic **[source: REFERENCE.txt §2]**.
- MME-0.3x additions beyond §4 are limited to `MATERIALTOONTEXTURE`, `GROUNDSHADOWCOLOR`, the four
  `*TEXTURE` morph semantics, `use_subtexture`, and the `MME_MIPMAP` macro **[source: REFERENCE.txt changelog
  0.33 / REFERENCE.txt version header]**.

## 5. Special uniform names recognised by name (no semantic)

“以下の名前のパラメータは、セマンティクス無しで自動的に値が設定される” — the following parameter names
receive values automatically without any semantic **[source: REFERENCE.txt §2.9 ●特殊パラメータ]**. Case is
ignored. All of them only have correct values in `MMDPass="object"` / `"object_ss"` techniques **[source:
REFERENCE.txt §2.9 ○補足]**.

| Name | Type | Meaning | Source |
|------|------|---------|--------|
| `parthf` | `bool` | self-shadow **perspective** flag: corresponds to self-shadow mode1/mode2 (`false` = mode1) | REFERENCE.txt §2.9 |
| `spadd` | `bool` | sphere-map **additive** blend flag (`true` = add) | REFERENCE.txt §2.9 |
| `transp` | `bool` | semi-transparent flag (`true` = transparent); the [表示]-[半透明化] menu | REFERENCE.txt §2.9 |
| `use_texture` | `bool` | the material being drawn uses a texture | REFERENCE.txt §2.9 |
| `use_spheremap` | `bool` | the material uses a sphere map (also true for a PMX subtexture sphere) | REFERENCE.txt §2.9 |
| `use_subtexture` | `bool` | the PMX subtexture is used | REFERENCE.txt §2.9 |
| `use_toon` | `bool` | toon rendering is used (PMD model) | REFERENCE.txt §2.9 |
| `opadd` | `bool` | the object's draw is set to additive blending | REFERENCE.txt §2.9 |
| `VertexCount` | `int` | the object's vertex count | REFERENCE.txt §2.9 |
| `SubsetCount` | `int` | the object's subset (material) count | REFERENCE.txt §2.9 |

**[inferred]** `parthf` is best read as a *self-shadow mode* flag; the reference calls it “セルフシャドウフラグ”.
Note the reference's label “パースペクティブ” (perspective) in the local `full_SimpleSoftShadow.fx` comment —
the two descriptions (self-shadow mode flag vs perspective flag) differ; REFERENCE.txt §2.9 says “セルフシャドウの
mode1/mode2に対応”.

**Which of the plan's candidates are real:** `parthf`, `transp`, `spadd`, `use_texture`, `use_spheremap`,
`use_toon`, `opadd`, `use_subtexture`, `VertexCount`, `SubsetCount` are **real**. `MaterialDiffuse`,
`MaterialAmbient`, etc. are **not** special-by-name — they are ordinary parameter names used in examples for the
`DIFFUSE`/`AMBIENT` semantics **[source: REFERENCE.txt §2.2 examples — `float4 MaterialDiffuse : DIFFUSE
<string Object = "Geometry"; >;`]**. `use_subtexture` is real **[source: REFERENCE.txt §2.9]**. The plan's
uncertain “`use_subtexture`?” is therefore **confirmed**.

nanoem writes the same names: `transp`, `opadd`, `spadd`, `use_texture`, `use_spheremap`, `use_toon`,
`use_subtexture`, `VertexCount`, `SubsetCount` **[source: nanoem `emapp/src/Effect.cc` — `setMaterialParameters`
/ `setAllModelParameters` / `setAllAccessoryParameters`, e.g. ~2947–2948, 3080–3081, 3245–3270, 3401–3403]**.
nanoem additionally writes `EgColor` (emissive-ish albedo × light) and `SpcColor` (specular × light + power)
**[source: nanoem `emapp/src/Effect.cc` ~3270–3277]**; these are **not** documented MME names and are
**[inferred]** nanoem-internal conveniences for its own fallback material shader, not MME semantics.

## 6. Texture declarations

A texture parameter is declared with a type and, usually, a semantic:

```hlsl
texture NAME : SEMANTIC < annotations > ;
texture2D NAME : SEMANTIC < annotations > ;
```

(examples use `texture`, `texture2D`, `texture3D`, `textureCUBE`) **[source: REFERENCE.txt §2.7 ●通常テクスチャ]**.
For plain textures (not `RENDERCOLORTARGET`/`RENDERDEPTHSTENCILTARGET`/`ANIMATEDTEXTURE`), any other semantic on
the declaration is ignored **[source: REFERENCE.txt §2.7 ●通常テクスチャ]**.

### 6.1 Texture annotations

| Annotation | Type | Meaning |
|------------|------|---------|
| `ResourceType` | `string` | texture kind: `"2D"`, `"3D"`, `"CUBE"`. Required only when the type is `texture` and the kind is not 2D; otherwise optional. |
| `ResourceName` | `string` | source image file. Supported: `bmp`, `dds`, `dib`, `jpg`, `png`, `tga`. A relative path is resolved against the folder of the effect file that declares it. |
| `Width`, `Height`, `Depth` | `int` | pixel dimensions (depth only for volume textures). Mutually exclusive with `Dimensions`, `ViewportRatio`. |
| `Dimensions` | `int2`/`int3` | pixel dimensions as a vector. Mutually exclusive with `ViewportRatio`, `Width`/`Height`/`Depth`. |
| `ViewportRatio` | `float2` | size relative to the render-target screen; `{1,1}` = same size, `{2,2}` = twice. Mutually exclusive with `Dimensions`, `Width`/`Height`/`Depth`. |
| `Format` | `string` | pixel format (default `A8R8G8B8` for colour, `D24S8` for depth-stencil); ignored when `ResourceName` is given. Accepts `D3DFORMAT` names with or without the `FMT_` / `D3DFMT_` prefix. |
| `Miplevels` / `Levels` | `int` | mip levels (`Levels` is an alias). Omitted/`0` = full chain; `1` = no mips. |
| `ClearColor` | `float4` | (off-screen targets) clear colour. |
| `ClearDepth` | `float` | (off-screen targets) clear depth. |
| `AntiAlias` | `bool` | (targets) enable MSAA; default `false`. |
| `Description` | `string` | (off-screen targets) GUI description text. |
| `DefaultEffect` | `string` | (off-screen targets) effect assignment (§4.10). |
| `Offset`, `Speed`, `SeekVariable` | `float`/`float`/`string` | (animated textures, §4.11). |

**[source: REFERENCE.txt §2.7 ●通常テクスチャ and each target's annotation list]**. If none of
`(Width,Height,Depth)`, `Dimensions`, `ViewportRatio` is set, the default is 64 pixels; if `ResourceName` is
set the size is taken from the image **[source: REFERENCE.txt §2.7 → Dimensions / ViewportRatio]**.

### 6.2 `Format` names accepted

The reference points at the D3DFORMAT enum and says `"A8R8G8B8"`, `"FMT_A8R8G8B8"`, `"D3DFMT_A8R8G8B8"` are all
accepted **[source: REFERENCE.txt §2.7 → string Format]**. A reimplementation must therefore accept the
`D3DFMT_`/`FMT_` prefix. nanoem enumerates an explicit accepted set and adds all three prefixes automatically
**[source: nanoem `emapp/src/Effect.cc` `Effect::addImageFormat` (prefixes `""`, `"FMT_"`, `"D3DFMT_"`) and the
format table ~1340–1368]**. The nanoem table (a practical, non-exhaustive D3DFORMAT subset) is:

`A8`, `A8L8`, `A8R8G8B8`, `A8R8G8B8`, `A16B16G16R16`, `A16B16G16R16F`, `A16R16G16B16`, `A16R16G16B16F`,
`A32B32G32R32F`, `A32R32G32B32F`, `X8R8G8B8`, `R16F`, `R32F`, `G16R16`, `G16R16F`, `G32R32F`, `R5G6B5`,
`A4R4G4B4`, `A1R5G5B5`, `A2B10G10R10`, `L8`, `L16`, `DXT1`…`DXT5` (bc formats), and depth formats `D16`,
`D24X8`, `D24S8`, `D32`, `DF16`, `DF24`, `D32F_LOCKABLE`, `S8_LOCKABLE` **[source: nanoem `emapp/src/Effect.cc`
`addImageFormat` table, ~1330–1368]**.

### 6.3 Relative path resolution

`ResourceName` is “相対パスでファイル名を指定した場合、エフェクトファイルが格納されているフォルダが基準と
なる” — relative to the folder of the effect file that carries the declaration **[source: REFERENCE.txt §2.7 →
ResourceName]**. MMDX12's `ReadMmeHints` already resolves a `ResourceName` relative to the declaring file and
then the effect folder, case-insensitively, and keeps only files that exist **[source: `src/asset/MmeHints.cpp`
`ReadMmeHints` — `ResolveRel(td.dir.parent_path(), …)` then `ResolveRel(td.root, …)`; `src/asset/MmeText.h`
`ResolveRel`]**. The `#define NAME "literal"` indirection MME allows in `ResourceName` (e.g. Ray-MMD's
`texture AlphaMap<string ResourceName = ALPHA_MAP_FILE;>;`) is also followed **[source:
`library/by StarNoodle495/MovieSonicMMD_Ver2.0.2/Ray1.5.2Materials/FX/main.fxsub`; `src/asset/MmeHints.cpp`
`ResolveExpr`/`ResolveDefine`]**.

### 6.4 `shared` textures

A parameter declared `shared` is shared across effect files by name, and **both** declarations must specify
`shared` and agree on name, type and semantic **[source: REFERENCE.txt §4 ●エフェクトファイル間でパラメータを
共有]**. Example: `shared texture ShadowBuffer : RENDERCOLORTARGET < … >;` in effect1, then
`shared texture ShadowBuffer : RENDERCOLORTARGET;` in effect2 **[source: REFERENCE.txt §4]**. nanoem tracks
this with a `kShared` parameter flag and a shared render-target map **[source: nanoem `emapp/src/Effect.cc`
`parameter.m_shared = (parameterPtr->flags & TypedSemanticParameter::kShared) != 0` ~1896;
`findSharedRenderTargetImageContainer`]**. **[source: local]** Ray-MMD uses `shared texture2D DummyScreenTex :
RENDERCOLORTARGET;` **[source:
`library/by StarNoodle495/MovieSonicMMD_Ver2.0.2/Ray1.5.2Materials/FX/main.fxsub`]**.

### 6.5 `MME_MIPMAP`

A macro `MME_MIPMAP` is defined when MMD supports texture mipmaps; effects guard `MIPFILTER = LINEAR` with it
**[source: REFERENCE.txt §2.10 ●MME_MIPMAP]**.

## 7. The script language

A **technique** and a **pass** may carry `string Script = "command; command; …";`. Commands run sequentially in
order and can change render targets, clear them, loop, and execute passes **[source: REFERENCE.txt §3]**. String
literals adjacent with only whitespace between them are concatenated (used heavily by macro-generated
techniques) **[source: REFERENCE.txt §3 ○使用例]**. If a **technique** has no script, its passes run in order;
if a **pass** has no script, `Draw=Geometry` runs **[source: REFERENCE.txt §3]**.

### 7.1 Commands

| Command | Where | Meaning |
|---------|-------|---------|
| `RenderColorTarget=(name-or-empty)` | technique/pass | alias of `RenderColorTarget0` |
| `RenderColorTarget0=(name-or-empty)` | technique/pass | bind a `RENDERCOLORTARGET` texture as colour target 0; empty = the default target |
| `RenderColorTarget1/2/3=(name-or-empty)` | technique/pass | additional colour targets; may not be used alone, must accompany target 0 |
| `RenderDepthStencilTarget=(name-or-empty)` | technique/pass | bind a `RENDERDEPTHSTENCILTARGET`; empty = default |
| `ClearSetColor=(param)` | technique/pass | set the colour to clear to (from a `float4` parameter); does not clear yet |
| `ClearSetDepth=(param)` | technique/pass | set the depth to clear to (from a `float` parameter); does not clear yet |
| `Clear=Color` | technique/pass | clear the bound colour target with the colour set by `ClearSetColor` |
| `Clear=Depth` | technique/pass | clear the bound depth-stencil with the value set by `ClearSetDepth` |
| `ScriptExternal=Color` | **technique only** | draw the other objects (the scene) into the currently bound target; post-effects must use it exactly once |
| `Pass=(pass name)` | **technique only** | run that pass; if a technique has a script, passes run **only** via `Pass=` |
| `LoopByCount=(param)` / `LoopEnd=` | **technique only** | repeat the commands between them `param` times; nestable |
| `LoopGetIndex=(param)` | technique only, inside a loop | write the current loop counter into `param` |
| `Draw=Geometry` | **pass only** | draw the object. Default when the pass script is omitted. Forbidden when `ScriptClass="scene"` |
| `Draw=Buffer` | **pass only** | draw a screen-sized rectangle polygon (full-screen quad); used by pre/post effects. Forbidden when `ScriptClass="object"` |
| `RenderPort=*` | — | not supported |

**[source: REFERENCE.txt §3 ○コマンド and ●Tips]**. Bindings of `RenderColorTarget*`/`RenderDepthStencilTarget`
persist until changed again (even across the technique) **[source: REFERENCE.txt §3 → RenderColorTarget]**. The
`RenderColorTarget` command's argument must name a parameter declared with `RENDERCOLORTARGET`;
`RenderDepthStencilTarget`'s must name a `RENDERDEPTHSTENCILTARGET` **[source: REFERENCE.txt §3]**.

**`ScriptExternal` semantics.** It draws the objects (pre-effect, objects, other post-effects) into the
currently bound target; a post-effect must run it exactly once in the technique script **[source: REFERENCE.txt
§3 → ScriptExternal=Color; §4 ●ポストエフェクト]**. This is the mechanism behind the “preprocess runs before
the post-process pre-pass” note in §1.3.

### 7.2 `ScriptClass` and `ScriptOrder` (on `STANDARDSGLOBAL`)

`ScriptClass` values **[source: REFERENCE.txt §2.8 → ScriptClass]**:

| Value | Meaning | `Draw=` restriction |
|-------|---------|---------------------|
| `"object"` (default) | draws objects | must not `Draw=Buffer` |
| `"scene"` | draws the screen buffer | must not `Draw=Geometry` |
| `"sceneorobject"` | both | both allowed |

`ScriptOrder` values **[source: REFERENCE.txt §2.8 → ScriptOrder]**:

| Value | Meaning |
|-------|---------|
| `"standard"` (default) | draws objects |
| `"preprocess"` | before objects (pre-effect) |
| `"postprocess"` | after objects (post-effect) |

The reference's precise ordering: “preprocessのさらに前に、postprocessの前処理（テクニックのスクリプトの先頭
から `ScriptExternal=Color` まで）が実行される” — the post-process pre-pass (from the technique script's start
up to `ScriptExternal=Color`) runs before even `preprocess` **[source: REFERENCE.txt §2.8 → ScriptOrder]**.
nanoem mirrors this: `Project::drawViewport()` calls `drawAllEffectsDependsOnScriptExternal()` first, then
PreProcess, Standard/edge, Standard/color, Standard/groundShadow, PostProcess **[source: nanoem
`emapp/src/Project.cc` `drawViewport` ~2948–2963]**.

### 7.3 Default scripts and pass ordering

- Technique without `Script`: its passes run in order **[source: REFERENCE.txt §3]**.
- Pass without `Script`: `Draw=Geometry` **[source: REFERENCE.txt §3]**. nanoem's `Pass` constructor defaults the
  script string to `"Draw=Geometry;"` **[source: nanoem `emapp/src/effect/Pass.cc` `Pass::Pass` —
  `annotations.stringAnnotation(kScriptKeyLiteral, String("Draw=Geometry;"))`]**.
- Technique with `Script` but no `Pass=`: no pass runs **[source: REFERENCE.txt §3 → Pass=]**.
- nanoem, when a technique has no script, synthesises one as `Pass=<name>;` for each pass in order **[source:
  nanoem `emapp/src/effect/Technique.cc` `Technique` ctor]**.

### 7.4 Script execution model and clearing

Commands are executed one by one until a `Pass=` produces a pass, at which point control returns to the caller
(the pass runs, then the technique script resumes after the `Pass=` line) **[source: nanoem
`emapp/src/effect/Technique.cc` `Technique::execute` and `interpretScriptCommand`, and `Pass::execute`;
`emapp/src/effect/Pass.cc`]**. `ClearSetColor`/`ClearSetDepth` merely record values; `Clear=Color`/`Clear=Depth`
perform the clear **[source: REFERENCE.txt §3]**. If a technique script omits an explicit `Clear=Color`/`Depth`
but sets the corresponding clear value, nanoem inserts the missing clear command before the first `Pass=`
**[source: nanoem `emapp/src/effect/Technique.cc` `ensureScriptCommandClear` and `Effect::parseScript`'s
`hasClearViewport*` tracking ~1152–1200]**.

### 7.5 Loop implementation

`LoopByCount=(param)` reads a numeric parameter (int/bool/float), pushes a loop counter; commands between it and
`LoopEnd=` repeat that many times; loops nest; `LoopGetIndex=(param)` writes the counter **[source: REFERENCE.txt
§3; nanoem `emapp/src/Effect.cc` `pushLoopCounter`/`popLoopCounter`/`handleLoopGetIndex` ~5910–5955]**. A
boolean `true`/`false` counts as `1`/`0`, which is used to conditionally run a pass **[source: REFERENCE.txt §4
●オブジェクトの有無でパスをON/OFF]**.

### 7.6 Post-effect chaining order

Post-effects (and pre-effects) are applied in the order the objects appear in MMD's accessory/effect list; the
reference describes a typical post-effect as (1) switch to an own target and clear, (2) `ScriptExternal=Color`
to render the scene into it, (3) restore the target and run the pass(es) that read the result **[source:
REFERENCE.txt §4 ●ポストエフェクト]**. nanoem collects effect-bearing drawables into per-order sets and draws
them in the MMD object order within each order bucket **[source: nanoem `emapp/src/Project.cc`
`addEffectOrderSet`/`drawViewport`, ~6351–6380, 7109–7130]**. **[inferred]** chaining order is therefore
“accessory order in MMD, filtered by ScriptOrder”.

## 8. Render states usable in passes

MME effect files are ordinary D3D9 Effect files, so a pass may set any D3D9 render-state or sampler-state name
as `Name = Value;`. REFERENCE.txt does not enumerate them (it defers to the MSDN Effect File Format), so the
canonical reference for the value encoding is nanoem's state converter, which is a faithful reading of the D3D9
enum values **[source: nanoem `emapp/src/effect/EffectCommon.cc` `RenderState::convertPipeline` /
`convertSamplerState` / `convertBlendFactor` / `convertBlendOperator` / `convertCompareFunc` / `convertWrap` /
`convertFilter` / `convertStencilOp`]**.

### 8.1 Render states (pass body) and their D3D9 enum values

| State | D3D9 enum value | nanoem handling | Source |
|-------|-----------------|-----------------|--------|
| `ZENABLE` | `7` | depth test (see also `ZFUNC`) | EffectCommon.cc `convertPipeline` case 7 |
| `FILLMODE` | `8` | `1`=point, `2`=wireframe, else triangles | case 8 |
| `ZWRITEENABLE` | `14` | depth write on/off | case 14 |
| `SRCBLEND` | `19` | blend source factor (RGB) | case 19 |
| `DESTBLEND` | `20` | blend dest factor (RGB) | case 20 |
| `CULLMODE` | `22` | `1`=NONE, `2`=CW(front), `3`=CCW(back) | case 22 |
| `ZFUNC` | `23` | depth compare func | case 23 |
| `ALPHAREF` | `24` | alpha-to-coverage (nanoem approximation) | case 24 |
| `ALPHABLENDENABLE` | `27` | alpha blending on/off; when on and factors unset, defaults SRC_ALPHA / INV_SRC_ALPHA | case 27 |
| `STENCILENABLE` | `52` | stencil on/off | case 52 |
| `STENCILFAIL` | `53` | front stencil fail op | case 53 |
| `STENCILZFAIL` | `54` | front stencil depth-fail op | case 54 |
| `STENCILPASS` | `55` | front stencil pass op | case 55 |
| `STENCILFUNC` | `56` | front stencil compare | case 56 |
| `STENCILREF` | `57` | stencil reference | case 57 |
| `STENCILMASK` | `58` | stencil read mask | case 58 |
| `STENCILWRITEMASK` | `59` | stencil write mask | case 59 |
| `COLORWRITEENABLE` | `168` | colour write mask (0=none, else RGBA) | case 168 |
| `BLENDOP` | `171` | blend operator (RGB) | case 171 |
| `CCW_STENCILFAIL` | `186` | back-face stencil fail (and following `CCW_*`) | case 186 |

**[source: nanoem `emapp/src/effect/EffectCommon.cc` `RenderState::convertPipeline`]**. Blend factors
(`convertBlendFactor`): `1`=ZERO, `2`=ONE, `3`=SRCCOLOR, `4`=INVSRCCOLOR, `5`=SRCALPHA, `6`=INVSRCALPHA,
`7`=DESTALPHA, `8`=INVDESTALPHA, `9`=DESTCOLOR, `10`=INVDESTCOLOR, `11`=SRCALPHASAT. Blend ops:
`1`=ADD, `2`=SUBTRACT, `3`=REVSUBTRACT, `4`=MIN, `5`=MAX. Compare funcs (`ZFUNC`/`STENCILFUNC`): `1`=NEVER,
`2`=LESS, `3`=EQUAL, `4`=LESSEQUAL, `5`=GREATER, `6`=NOTEQUAL, `7`=GREATEREQUAL, `8`=ALWAYS. Stencil ops:
`1`=KEEP, `2`=ZERO, `3`=REPLACE, `4`=INCRSAT, `5`=DECRSAT, `6`=INVERT, `7`=INCR, `8`=DECR **[source: nanoem
`emapp/src/effect/EffectCommon.cc`]**.

### 8.2 Sampler states

Sampler state is set inside a `sampler_state { … }` block or as `SamplerState`:

| State | D3D9 enum value | nanoem handling | Source |
|-------|-----------------|-----------------|--------|
| `ADDRESSU` | `1` | `2`=MIRROR, `3`=CLAMP, `4`=BORDER, else WRAP | EffectCommon.cc `convertSamplerState` case 1 |
| `ADDRESSV` | `2` | as above | case 2 |
| `ADDRESSW` | `3` | as above | case 3 |
| `BORDERCOLOR` | `4` | ignored | case 4 |
| `MAGFILTER` | `5` | `2`=LINEAR, `3`=ANISOTROPIC, else POINT | case 5 |
| `MINFILTER` | `6` | as above | case 6 |
| `MIPFILTER` | `7` | `0`=NONE, `1`=POINT, `2`=LINEAR | case 7 |
| `SRGBTEXTURE` | `11` | ignored | case 11 |

**[source: nanoem `emapp/src/effect/EffectCommon.cc` `convertSamplerState` / `convertFilter` / `convertWrap` /
`setMipFilter`]**. In `.fx` source, filter/wrap names are written symbolically (`LINEAR`, `NONE`, `WRAP`,
`CLAMP`, `MIRROR`, `ANISOTROPIC`), as in `full.fx`/Ray-MMD samplers **[source:
`library/model/pda_miku/New/full_SimpleSoftShadow.fx`; `library/.../main.fxsub` sampler blocks]**.

### 8.3 MME's default render state

The reference does not dictate a specific default. The two facts that matter:

- A pass with **no shader** uses MMD's standard shader **[source: REFERENCE.txt §4]**.
- nanoem supplies defaults when a state is unset: blend defaults to SRC_ALPHA / INV_SRC_ALPHA (or ONE for
  add-blend drawables) with op ADD, depth compare LESSEQUAL, stencil off, colour write RGBA, cull BACK
  **[source: nanoem `emapp/src/effect/Technique.cc` `overrideColorState` / `overrideDepthState` /
  `overrideStencilState`]**.

**[inferred]** A D3D12 host should set explicit defaults matching D3D9's (depth test on, `LESSEQUAL`, depth
write on, cull CCW-as-back in D3D9's left-handed convention, blend off, colour write all) and let a pass
override them.

## 9. Vertex input layout MME provides

MME hands each pass a fixed vertex stream. The layout is a property of MMD's vertex format, not of the effect
file, so an effect may only use the semantics MMD supplies. The confirmed set:

| Shader semantic | Meaning | nanoem attribute slot |
|-----------------|---------|-----------------------|
| `POSITION` (or `SV_POSITION`) | model vertex position (already CPU-skinned) | 0 |
| `NORMAL` | vertex normal | 1 |
| `TEXCOORD0` / `TEXCOORD` | material UV0 (diffuse UV) | 2 |
| `TEXCOORD1` | UV1 | 3 |
| `TEXCOORD2` | UV2 | 4 |
| `TEXCOORD3` | UV3 | 5 |
| `TEXCOORD4` | UV4 | 6 |
| `COLOR0` / `COLOR` | vertex colour | 7 |
| `_INDEX` | vertex index (vertex shader only; §4.14) | — |

**[source: nanoem `emapp/src/Effect.cc` ~1791–1830 (`a_position`/`SV_POSITION`, `a_normal`, `a_texcoord0`…
`a_texcoord4`, `a_color0`) and the semantic→slot map that follows; `emapp/src/effect/Pass.cc`
`overrideScenePipelineDescription` for the equivalent full-screen-quad layout]**. The MME reference documents
only `_INDEX` explicitly **[source: REFERENCE.txt §2.9 ●_INDEX]** — the rest is fixed by MMD and confirmed by
real effects:

- `full.fx`-derived effects declare `float4 Pos : POSITION, float3 Normal : NORMAL, float2 Tex : TEXCOORD0`
  **[source: `library/model/pda_miku/New/full_SimpleSoftShadow.fx` Basic_VS]**.
- Additional UVs are documented by effect authors as `AddUV1 = TEXCOORD1`, `AddUV2 = TEXCOORD2`,
  `AddUV3 = TEXCOORD3`, `AddUV4 = TEXCOORD4` **[source: `library/MLTD/Cherry Colored Love/stage051/Shader/
  Object_submap.fx` — `#define Vertex_Color_Stored_At TEXCOORD2` with the comments `AddUV1 = TEXCOORD1` …
  `AddUV4 = TEXCOORD4`; `Object_Commentless.fx`]**.
- Vertex colour arrives on `TEXCOORD2` in MES40's layout (via `#define Vertex_Color_Stored_At TEXCOORD2`) or as
  `COLOR0`; both are accepted vertex-input names **[source: `library/MLTD/.../Object_submap.fx`;
  `library/model/pda_miku/New/full_SimpleSoftShadow.fx` `float4 Color : COLOR0`]**.
- The MES40 submap decode reads `float2 Tex2 : TEXCOORD1` (the sub-map UV) **[source:
  `library/MLTD/Cherry Colored Love/stage051/Shader/MES40.fxsub` BufferShadow_VS]**.

**[inferred]** The additional UV sets are the MMD-side UVs used by light maps / sub-maps; PMX supports up to
four extra UVs, matching `TEXCOORD1..4`.

## 10. MikuMikuMoving (MMM) differences

MMM is MME-compatible but not identical **[source: MMM manual §16.1]**. Listed differences:

- **Not supported in MMM:** the mouse semantics `MOUSEPOSITION`, `LEFTMOUSEDOWN`, `MIDDLEMOUSEDOWN`,
  `RIGHTMOUSEDOWN`; and `ANIMATEDTEXTURE` **[source: MMM manual §16.1]**.
- Other functions are “implemented but do not yet possess exactly the same functionality as they do in MME”
  **[source: MMM manual §16.1]**.
- **A macro `MIKUMIKUMOVING`** is implied by the manual's “save the effect file as `.fxm` or `.fxsub` to use
  these” mechanism; **[unknown]** the exact macro name is not stated in the collected manual text (the manual
  says only that skinning functions require `.fxm`/`.fxsub`). Treat the precise `MIKUMIKUMOVING` spelling as
  **[unknown]**.
- **MMM does GPU skinning in the shader**, unlike MMD/MME which CPU-skins before the effect. MMM provides a
  `MMM_SKINNING_INPUT` struct with `BLENDWEIGHT`, `BLENDINDICES`, SDEF inputs, extra UVs and edge weight, and
  the functions `MMM_SkinninedPositionNormal` / `MMM_SkinninedPosition` (the manual's spelling) **[source: MMM
  manual §16.3]**. This confirms MME's input is *already skinned* (§1.5).
- **MMM extends lighting and shadow semantics:** `POSITION0..2`, `DIRECTION0..2`, `DIFFUSE0..2`,
  `AMBIENT0..2`, `SPECULAR0..2`, per-light matrices `VIEW0..2`/`PROJECTION0..2`/`WORLDVIEW0..2`/
  `VIEWPROJECTION0..2`/`WORLDVIEWPROJECTION0..2` (inverse/transpose also usable), array forms
  `LIGHTWVPMATRICES`, `LIGHTDIRECTIONS`, `LIGHTPOSITIONS`, `LIGHTDIFFUSECOLORS`, `LIGHTAMBIENTCOLORS`,
  `LIGHTSPECULARCOLORS`, shadow-thickness values `SHADOWDEEP`, `SHADOWDEEP_POSITIVE`, `SHADOWDEEP_NEGATIVE`,
  and the constant `MMM_LightCount` (= 3) **[source: MMM manual §16.4]**. `POSITION` and `POSITION0` are the
  same; `object="Camera"` gives all lights the same value; only `object="Light"` is active **[source: MMM
  manual §16.4]**.
- **MMM toon/self-shadow samplers:** `MMM_ToonTexSampler`, `MMM_SelfShadowSampler[MMM_LightCount]`, and the
  function `MMM_UnpackDepth` (returns a 0..1 depth from the 16-bit-stored self-shadow map) **[source: MMM manual
  §16.5]**.
- **MMM UI annotations:** MMM adds parameter UI annotations (`UIWidget`, `UIName`, `UIVisible`, `UIMin`,
  `UIMax`, `UIHelp`) letting an effect's parameters be edited in the MMM GUI, with six control forms (bool, int,
  float, float2, float3, float4); bool → checkbox **[source: MMM manual §16.2]**. (nanoem's `UIWidgetParameter`
  reads the same annotation names, `emapp/src/effect/EffectCommon.cc`, confirming they are a shared convention.)

The four `MMM_*` sample/UI additions are **MMM-only**; MMD/MME effects do not use them.

## 11. What a D3D12 reimplementation must provide

A bullet list of the runtime features needed to be MME-compatible, grouped by responsibility. Each item links
back to the section that justifies it.

**Effect loading / parsing**
- Parse fx_2_0 effect files and their `#include` chains (with `.fxsub`/`.fxm` treated as includable `.fx`),
  case-insensitively and tolerantly (Shift-JIS / UTF-8) (§1.2, §2.3, §6.3). MMDX12 already does the include
  walk for texture discovery — `src/asset/MmeHints.cpp` `ParseFile`/`ReadMmeHints`.
- Resolve `ResourceName` textures relative to the declaring effect file, following `#define` indirection, and
  keep only existing files (§6.3).
- Enumerate techniques and their annotations: `MMDPass`, `Subset`, `UseTexture`, `UseSphereMap`, `UseToon`, and
  `Script` (§3, §7).
- Parse `STANDARDSGLOBAL` (`ScriptOutput`, `ScriptClass`, `ScriptOrder`, `Script`) (§4.13, §7.2).
- Parse pass `Script` and technique `Script` into the command list (§7).

**Assignment**
- Same-name `.fx` beside `.pmx`/`.x`, plus `name[effect.fx].pmx` embedding (§2.2). *(MMDX12 currently follows
  only `.emd`.)*
- `.emd` per-model and per-subset assignment (`Obj`, `Obj[n]`, `Obj.show`) (§2.3).
- `.emm` per-scene assignment (§2.4).
- A default assignment applied to unassigned objects (§2.1).
- Treat an `.x` accessory as a controller and expose its panel values through `CONTROLOBJECT` (§2.6). MMDX12
  already classifies controllers — `src/asset/PmmLoader.cpp` `ClassifyMmeController`/`MapMmeControllers`.

**Draw-step dispatch**
- Reproduce MMD's draw steps and for each step select the first matching technique, else the built-in standard
  shader: `zplot` (light-space self-shadow depth), `object`, `object_ss`, `shadow` (ground shadow), `edge`
  (PMD-only) (§1.4, §3.1).
- For `object_ss`, fall back to `object` when no self-shadow technique matches (§3.1).
- Draw order: post-process pre-pass → pre-effects → edges → objects → ground shadow → post-effects (§1.3, §7.2).
- Empty techniques suppress the draw; missing effects use the standard shader (§1.7).

**Semantic binding (per draw call / pass)**
- All matrix semantics plus `INVERSE`/`TRANSPOSE`/`INVERSETRANSPOSE`, and the `Object="Camera"|"Light"` view/
  projection variant (§4.1).
- Light/geometry colours `DIFFUSE`/`AMBIENT`/`EMISSIVE`/`SPECULAR`/`SPECULARPOWER`/`TOONCOLOR`/`EDGECOLOR`/
  `GROUNDSHADOWCOLOR` with the correct per-MMDPass validity (§4.2).
- `POSITION`/`DIRECTION` for camera and light (§4.3).
- `MATERIALTEXTURE`/`MATERIALSPHEREMAP`/`MATERIALTOONTEXTURE` (§4.4).
- `VIEWPORTPIXELSIZE` (viewport, not render target) (§4.5).
- `TIME`/`ELAPSEDTIME` with `SyncInEditMode` frame-vs-system-time split (§4.6).
- Mouse semantics (§4.7) — optional for non-interactive rendering.
- `CONTROLOBJECT` with all `item` values, the `(self)`/`(OffscreenOwner)` names, draw-order selection, and the
  missing-object defaults (§4.8).
- `RENDERCOLORTARGET`, `RENDERDEPTHSTENCILTARGET` creation + binding (§4.9, §7).
- `OFFSCREENRENDERTARGET` with `ClearColor`/`ClearDepth`/`AntiAlias`/`DefaultEffect` re-render of the scene
  (§4.10).
- `ANIMATEDTEXTURE` (GIF/APNG) with `Offset`/`Speed`/`SeekVariable` (§4.11).
- `TEXTUREVALUE` (vetex-shader texel array) (§4.12).
- The four PMX material-morph value semantics (§4.13).
- `_INDEX` vertex-shader semantic (§4.14).

**Special-by-name uniforms** (no semantic) (§5): `parthf`, `spadd`, `transp`, `use_texture`, `use_spheremap`,
`use_subtexture`, `use_toon`, `opadd`, `VertexCount`, `SubsetCount`.

**Script execution**
- `RenderColorTarget[0..3]`, `RenderDepthStencilTarget`, `ClearSetColor`, `ClearSetDepth`, `Clear=Color|Depth`,
  `ScriptExternal=Color`, `Pass=`, `LoopByCount=`/`LoopEnd=`/`LoopGetIndex=`, `Draw=Geometry|Buffer` (§7).
- Target bindings persist until changed; multi-target (MRT) via `RenderColorTarget1..3` (§7).
- Pre/post-effect chaining with the post-process pre-pass before `preprocess` (§7.6).

**Rendering**
- A full-screen-quad path for `Draw=Buffer` with a D3D9 sub-pixel offset for the full-screen quad **[source:
  nanoem `emapp/src/effect/Pass.cc` `interpretScriptCommand` `kScriptCommandTypeDraw` — generates a quad tri-strip
  with a subpixel shift “due to DirectX9 spec”]**.
- D3D9 render states and sampler states from the pass, mapped to D3D12 (blend, depth, stencil, cull, fill,
  colour write, address modes, filters) (§8).
- Vertex input layout of §9.
- `shared` textures across effects (§6.4).

**Not required for MME but for MMM compatibility** (§10): mouse semantics are absent in MMM; `MIKUMIKUMOVING`
macro, GPU skinning inputs/functions, multi-light and shadow-thickness semantics, `MMM_*` samplers, and UI
annotations.

## 12. Confirmed vs inferred vs unknown (summary)

**Confirmed from primary sources (REFERENCE.txt / MMEffect.txt):**

- MME is a `d3d9.dll` proxy with `MMHack.dll`/`MMEffect.dll` (§1.1).
- `MMDPass` values `object`, `zplot`, `object_ss`, `shadow`, `edge`; default `object` (§3.1).
- Technique annotations `Subset` (list/ranges), `MMDPass`, `UseTexture`, `UseSphereMap`, `UseToon`; first match
  wins; standard shader on no match (§1.4, §2.5, §3).
- `UseSelfShadow` is **not** an MME annotation (§3.6).
- Every semantic in §4, including `Object="Camera"|"Light"`, `SyncInEditMode`, `CONTROLOBJECT` items,
  off-screen `DefaultEffect` with `none`/`hide`/`main_default` and `self`, `TEXTUREVALUE`, `STANDARDSGLOBAL`,
  the four morph semantics, `_INDEX`, `MME_MIPMAP`.
- Every special-by-name uniform in §5 (including `use_subtexture`).
- Texture annotations and `Format` prefix tolerance; relative-path rule (§6).
- Script commands and `ScriptClass`/`ScriptOrder` semantics and the post-process pre-pass ordering (§7).
- Same-name `.fx`, `.emd`, `.emm`, default assignment, subset expansion, controller semantics (§2).

**Confirmed from nanoem (secondary):** the fixed vertex layout and attribute slots (§1.5, §9); the render-state
and sampler-state enum tables (§8); the technique/subset/use-flag matching implementation (§3); the aggregation
of effect draws into per-`ScriptOrder` buckets (§7.6); the standard-shader fallback (`ModelProgramBundle`) (§1.4).

**Inferred:** MMD CPU-skins before the effect and passes model-space positions (§1.5, §10); post-effect
chaining follows MMD accessory order (§7.6); the exact default render state (§8.3); additional UVs are PMX
uv1..uv4 (§9).

**Unknown:** the exact `.emm` line grammar (§2.4); whether an `MMEffect.ini`/`default.fx` file exists (§2.1 —
no evidence in v0.37); the exact `MIKUMIKUMOVING` macro spelling in the MMM manual (§10); `EDGEWIDTH` (§4.15);
the internal meaning of nanoem's `EgColor`/`SpcColor` (§5).

## 13. Cross-reference: what MMDX12 already parses

Read-only notes so a future implementation knows the existing surface (this document changes none of it):

- `src/asset/MmeHints.h` / `MmeHints.cpp` — reads a model's `.emd`, follows `[Effect] Obj` / `Obj[n]` entries
  and each `.fx`'s `#include` chain (case-insensitive, depth ≤ 8, cycle-safe), collecting `ResourceName`
  textures and `#define` literals. Resolves `ResourceName` against the declaring file then the effect folder.
  Shift-JIS / UTF-8 tolerant. This corresponds to MME's `.emd` sidecar (§2.3) and the texture-declaration path
  (§6).
- `src/asset/MmeText.h` / `MmeText.cpp` — the shared text/path helpers (`Trim`, `Lower`, `SplitLines`,
  `ReadTextFile`, `ReadTextFileText` with a Shift-JIS fallback, `ResolveRel`, `PathKey`).
- `src/asset/PmmLoader.h` / `PmmLoader.cpp` — `MmeController` families and `ClassifyMmeController`,
  `LooksLikeUnknownMmeController`, `MapMmeControllers`, mapping known controllers' `Si`/`Tr`/position to native
  settings; called from `src/studio/StudioLoader.cpp`. This corresponds to MME's controller-accessory pattern
  (§2.6) and the `.emm`/PMM auto-assignment story (§2.4).
- `docs/raymmd_semantics.md` — existing Ray-MMD material-semantics notes; **do not duplicate here**. Ray-MMD's
  `main.fx`/`main.fxsub`/`material_common_2.0.fxsub` are ordinary MME object effects using `MMDPass`
  `object`/`object_ss`/`edge`/`zplot`/`shadow`, `MATERIALTEXTURE`/`MATERIALSPHEREMAP`/`MATERIALTOONTEXTURE`,
  the special uniforms, and `RENDERCOLORTARGET` (§6.4 DummyScreenTex) **[source:
  `library/by StarNoodle495/MovieSonicMMD_Ver2.0.2/Ray1.5.2Materials/FX/main.fxsub`; `.../main.fx`]** — so the
  MME contract above is also the contract Ray-MMD relies on for its envelope.

## 14. Blocked sources

- **MME official distribution pages (Google Sites / uploader).** The author's original download pages could not
  be reached from this environment; the archive was obtained from a GitCode mirror of the v0.37 distribution
  (see `source.json`). Original distribution pages recorded but not fetched:
  `https://bowlroll.net/file/35012` (32-bit) and `https://bowlroll.net/file/35013` (64-bit) — BowlRoll serves
  the download only through its own UI/session, not a direct fetch of the ZIP.
  `https://sites.google.com/site/mikumikumovingeng/manual/16-effect-development` (MMM English manual) was
  reachable and is cited.
- **VPVP wiki / MME wiki (wikiwiki / atwiki).** Not fetched (search engines returned no direct reachable page
  for them in this environment); their content is not relied on here.
- **D3DX9 / Effect File Format and HLSL MSDN pages** referenced by REFERENCE.txt
  (`msdn.microsoft.com/library/bb173005`, `bb509638`, `bb172558`) — not independently fetched; the render-state
  enum values are instead taken from nanoem's converter, cited in §8.
- **No `.emm` sample** was found in the local library, so §2.4 is partly **[unknown]** rather than cited.

## 15. Versions and dates

- MME v0.37, distribution dated 2014-03-26 (DLLs); `REFERENCE.txt` dated 2013-02-13 (MME v0.33 revision);
  `MMEffect.txt` dated 2014-03-26 (v0.37). **[source: file timestamps in the capture; REFERENCE.txt version
  header; MMEffect.txt changelog ●0.37]**
- nanoem `main` fetched 2026-10-13 (latest commit `30acffaa`, 2025-03-03). **[source: GitHub API]**
- Local library examples read 2026-10-13. **[source: repository files]**

## 16. Appendix A — local-library usage counts (sanity check)

Recomputed over `library/**/*.fx` and `library/**/*.fxsub` on 2026-10-13 (read-only). Useful as a priority
guide for a first implementation; it matches the semantics/annotations this document describes.

Semantics appearing (occurrences; `:`-prefixed match, so `WORLDVIEWPROJECTION` is counted separately from
`WORLDVIEW`):

| Semantic | Count | Notes |
|----------|-------|-------|
| `CONTROLOBJECT` | 1452 | by far the most common — controllers drive everything |
| `POSITION` | 983 | includes camera+light |
| `ANIMATEDTEXTURE` | 318 | used heavily despite MMM not supporting it |
| `SPECULAR` | 223 | |
| `DIFFUSE` | 215 | |
| `WORLDVIEWPROJECTION` | 210 | |
| `RENDERCOLORTARGET` | 196 | |
| `AMBIENT` | 194 | |
| `MATERIALTEXTURE` | 164 | |
| `WORLD` | 123 | |
| `SPECULARPOWER` | 120 | |
| `DIRECTION` | 113 | |
| `EMISSIVE` | 110 | |
| `TOONCOLOR` | 98 | |
| `VIEW` | 83 | |
| `MATERIALSPHEREMAP` | 81 | |
| `TIME` | 80 | |
| `EDGECOLOR` | 51 | |
| `MATERIALTOONTEXTURE` | 40 | added in MME 0.33 |
| `WORLDVIEW` | 35 | |
| `VIEWPORTPIXELSIZE` | 21 | |
| `VIEWPROJECTION` | 20 | |
| `PROJECTION` | 17 | |
| `MULTIPLYINGTEXTURE` | 17 | MME 0.33 |
| `ADDINGTEXTURE` | 17 | MME 0.33 |
| `OFFSCREENRENDERTARGET` | 16 | rarer, but must exist |
| `GROUNDSHADOWCOLOR` | 15 | MME 0.33 |
| `WORLDINVERSE` | 11 | |
| `RENDERDEPTHSTENCILTARGET` | 11 | |
| `VIEWPROJECTIONINVERSE` | 10 | |
| `VIEWINVERSE` | 10 | |
| `PROJECTIONINVERSE` | 10 | |
| `MOUSEPOSITION` | 10 | |
| `ELAPSEDTIME` | 10 | |
| `STANDARDSGLOBAL` | 2 | post/sceneorobject effects only |

**[source: grep of `library/**` for `: <SEMANTIC>` on 2026-10-13]**.

### A.1 `MMDPass` is frequently macro-generated

A raw `grep MMDPass` mostly finds macro definitions and macro invocations, not literal values:

```
technique name<string MMDPass = mmdpass;\
technique name < string MMDPass = "object_ss"; bool UseTexture = tex; ...
technique name < string MMDPass = "object"; bool UseTexture = tex; ...
```

**[source: `library/**` — e.g. Ray-MMD `main.fxsub` `OBJECT_TEC(name, mmdpass)` macro and MES40
`MES40_Technique(...)`]**. So a loader must expand/parse the value, not just the token `MMDPass` — which is
also why nanoem reads techniques from the *compiled* fx9 binary rather than the source
**[source: nanoem `emapp/src/Effect.cc` `Fx9__Effect__Technique`]**. The literal-value histogram for
techniques that do write a literal is dominated by `zplot`/`shadow`/`edge` stubs (`technique ShadowTech<string
MMDPass = "shadow";>{}` etc.) **[source: `library/**` grep]**, matching §1.7's empty-technique idiom.

### A.2 Subset targeting in practice

The local `.emd` files show both styles: whole-object `Obj` only (`Alcander.emd`) and per-subset `Obj[n]` with
different `head.fx`/`eyes.fx`/`body.fx` per material (`RayMaterialMap.emd`) **[source:
`library/by StarNoodle495/AlcanderMMD/...`]**. A first implementation must therefore support per-subset `.emd`
assignment (§2.3), which MMDX12's current `ReadMmeHints` already collects.

## 17. Appendix B — matrix semantic spellings to accept

The full set of geometry-matrix semantics an effect may name, formed by combining the six base names with the
three suffixes. All are case-insensitive **[source: REFERENCE.txt §2.1]**; a parser should accept every
spelling below (matching MME). nanoem registers exactly this table **[source: nanoem `emapp/src/Effect.cc`
`addSemanticParameterHandler` calls, lines ~1282–1306]**:

```
WORLD, VIEW, PROJECTION, WORLDVIEW, VIEWPROJECTION, WORLDVIEWPROJECTION
WORLDINVERSE, VIEWINVERSE, PROJECTIONINVERSE, WORLDVIEWINVERSE, VIEWPROJECTIONINVERSE, WORLDVIEWPROJECTIONINVERSE
WORLDTRANSPOSE, VIEWTRANSPOSE, PROJECTIONTRANSPOSE, WORLDVIEWTRANSPOSE, VIEWPROJECTIONTRANSPOSE, WORLDVIEWPROJECTIONTRANSPOSE
WORLDINVERSETRANSPOSE, VIEWINVERSETRANSPOSE, PROJECTIONINVERSETRANSPOSE, WORLDVIEWINVERSETRANSPOSE,
VIEWPROJECTIONINVERSETRANSPOSE, WORLDVIEWPROJECTIONINVERSETRANSPOSE
```

Together with the non-matrix semantics MME registers **[source: nanoem `emapp/src/Effect.cc`]**: `DIFFUSE`,
`AMBIENT`, `EMISSIVE`, `SPECULAR`, `SPECULARPOWER`, `TOONCOLOR`, `EDGECOLOR`, `GROUNDSHADOWCOLOR`, `POSITION`,
`DIRECTION`, `MATERIALTEXTURE`, `MATERIALSPHEREMAP`, `MATERIALTOONTEXTURE`, `ADDINGTEXTURE`,
`MULTIPLYINGTEXTURE`, `ADDINGSPHERETEXTURE`, `MULTIPLYINGSPHERETEXTURE`, `VIEWPORTPIXELSIZE`, `TIME`,
`ELAPSEDTIME`, `MOUSEPOSITION`, `LEFTMOUSEDOWN`, `MIDDLEMOUSEDOWN`, `RIGHTMOUSEDOWN`, `CONTROLOBJECT`,
`RENDERCOLORTARGET`, `RENDERDEPTHSTENCILTARGET`, `ANIMATEDTEXTURE`, `OFFSCREENRENDERTARGET`, `TEXTUREVALUE`,
`STANDARDSGLOBAL`.

Note: nanoem also accepts `ADDINGTEXTURE`…`MULTIPLYINGSPHERETEXTURE` as MME semantics (its handler table) even
though REFERENCE.txt files them under §2.2 **[source: nanoem `Effect.cc` lines 1320–1323; REFERENCE.txt §2.2]**.

## 18. Appendix C — MME version history relevant to the format

From MMEffect.txt's changelog **[source: MMEffect.txt ●更新履歴]**, the changes that altered the *format* a
reimplementation must understand:

| Version | Change relevant here |
|---------|----------------------|
| 0.20 | `OFFSCREENRENDERTARGET` added; `CONTROLOBJECT` gained more item values; `EDGECOLOR` added; CPU-order relaxation for `CONTROLOBJECT` targets |
| 0.22 | `Miplevels` semantics for `RENDERCOLORTARGET`/`OFFSCREENRENDERTARGET` changed |
| 0.24 | `.emd` per-model assignment added; `(self)` added to `CONTROLOBJECT`; `TEXTUREVALUE` added |
| 0.27 | PMX (partial); `_INDEX` added; `VertexCount`/`SubsetCount` added; `opadd` added |
| 0.30 | off-screen effects usable as off-screen effects and per-subset; `main_default` special effect name added |
| 0.31 | `CompileMode` option removed for x64 (always Type0); device-validation skip option |
| 0.33 | PMX material morph (`ADDINGTEXTURE`…); PMX subtexture (`UseSphereMap`, `use_spheremap`, `use_subtexture`); `MATERIALTOONTEXTURE`; `GROUNDSHADOWCOLOR`; `MME_MIPMAP` macro |
| 0.34 | toon-texture internal fetch changed; material names in subset expansion |
| 0.36 | EMM auto-save fix; texture material-morph fix |
| 0.37 | MMD ver9 support |

The `REFERENCE.txt` in the v0.37 distribution is dated 2013-02-13 and documents the *0.33* revision's semantics
header, i.e. it predates 0.34–0.37 but 0.34–0.37 did not add new semantics **[source: REFERENCE.txt version
header vs MMEffect.txt changelog 0.34–0.37]**.

## 19. Appendix D — consolidated semantic type / annotation table

A single table for implementers: every MME semantic with the exact parameter type MME expects and its
annotations. A parameter whose type does not match is an MME load error for that parameter **[inferred]**;
nanoem reports and skips a wrong-typed semantic parameter **[source: nanoem `emapp/src/Effect.cc`
`addInvalidParameterTypeError` calls throughout the handlers]**.

| Semantic | Accepted type(s) | Annotation keys | Annotation values |
|----------|------------------|-----------------|-------------------|
| `WORLD`, `VIEW`, `PROJECTION`, `WORLDVIEW`, `VIEWPROJECTION`, `WORLDVIEWPROJECTION` (+`INVERSE`/`TRANSPOSE`/`INVERSETRANSPOSE`) | `float4x4` | `Object` | `"Camera"`(default) / `"Light"` |
| `DIFFUSE`, `AMBIENT`, `SPECULAR` | `float3`/`float4` | `Object` (required) | `"Geometry"` / `"Light"` |
| `EMISSIVE` | `float3`/`float4` | `Object` (required) | `"Geometry"` |
| `SPECULARPOWER` | `float` | `Object` (optional) | `"Geometry"` |
| `TOONCOLOR`, `EDGECOLOR` | `float3`/`float4` | `Object` (optional) | `"Geometry"` |
| `GROUNDSHADOWCOLOR` | `float3`/`float4` | — | — |
| `POSITION`, `DIRECTION` | `float3`/`float4` | `Object` (required) | `"Camera"` / `"Light"` |
| `MATERIALTEXTURE`, `MATERIALSPHEREMAP`, `MATERIALTOONTEXTURE` | `texture`/`texture2D` | — | — |
| `ADDINGTEXTURE`, `MULTIPLYINGTEXTURE`, `ADDINGSPHERETEXTURE`, `MULTIPLYINGSPHERETEXTURE` | `float4` | — | — |
| `VIEWPORTPIXELSIZE` | `float2`/`float4` | — | — |
| `TIME`, `ELAPSEDTIME` | `float` | `SyncInEditMode` | `true` / `false`(default) |
| `MOUSEPOSITION` | `float2`/`float4` | — | — |
| `LEFTMOUSEDOWN`, `MIDDLEMOUSEDOWN`, `RIGHTMOUSEDOWN` | `float4` | — | — |
| `CONTROLOBJECT` | `bool`/`float`/`float3`/`float4`/`float4x4` | `name` (required), `item` (optional) | see §4.8 |
| `RENDERCOLORTARGET` | `texture`/`texture2D` | `Width`,`Height`,`Depth`,`Dimensions`,`ViewportRatio`,`Format`,`Miplevels`,`Levels`,`ClearColor`,`ClearDepth`,`AntiAlias` | §4.9, §6.1 |
| `RENDERDEPTHSTENCILTARGET` | `texture`/`texture2D` | `Width`,`Height`,`Depth`,`Dimensions`,`ViewportRatio`,`Format` | §4.9, §6.1 |
| `OFFSCREENRENDERTARGET` | `texture`/`texture2D` | `Width`,`Height`,`Depth`,`Dimensions`,`ViewportRatio`,`Format`,`Miplevels`,`Levels`,`ClearColor`,`ClearDepth`,`AntiAlias`,`Description`,`DefaultEffect` | §4.10 |
| `ANIMATEDTEXTURE` | `texture`/`texture2D` | `ResourceName`(required), `Offset`, `Speed`, `SeekVariable` | §4.11 |
| `TEXTUREVALUE` | `float4[]` / `float4[][]` | `TextureName`(required) | §4.12 |
| `STANDARDSGLOBAL` | `float` (=`0.8`) | `ScriptOutput`,`ScriptClass`,`ScriptOrder`,`Script` | §4.13, §7.2 |

**[source: REFERENCE.txt §2.1–§2.9; nanoem `emapp/src/Effect.cc` handlers where the accepted-type checks
live — e.g. `handleViewportPixelSizeSemantic` requires `float4`, `handleDiffuseSemantic` requires `float4`,
`handleSpecularPowerSemantic` requires `float`, `handleTimeSemantic` requires `float`]**. Where REFERENCE.txt
says `float2` (e.g. `VIEWPORTPIXELSIZE`, `MOUSEPOSITION`) nanoem stores four components; a `float2`/`float4`
declaration is accepted and the extra components ignored **[inferred]**.

### D.1 Special-by-name parameters (no semantic)

| Name | Accepted type | Value |
|------|---------------|-------|
| `parthf` | `bool` | self-shadow mode flag (mode1/mode2) |
| `spadd` | `bool` | sphere-map additive blend |
| `transp` | `bool` | translucent (the [表示]-[半透明化] toggle) |
| `use_texture` | `bool` | material has a texture |
| `use_spheremap` | `bool` | material uses a sphere map |
| `use_subtexture` | `bool` | PMX subtexture in use |
| `use_toon` | `bool` | toon rendering (PMD) |
| `opadd` | `bool` | object drawn additively |
| `VertexCount` | `int` | object vertex count |
| `SubsetCount` | `int` | object material count |

**[source: REFERENCE.txt §2.9 ●特殊パラメータ]**.

## 20. Appendix E — worked script traces

Two traces that exercise the whole script language, both from real library effects (read-only). They are the
acceptance tests a script interpreter should pass.

### E.1 A `sceneorobject` post/pre-effect (MES40-style)

MES40's object technique script draws two point/spot-light passes into off-screen colour targets, re-rendering
the scene with `ScriptExternal=Color`, then restores the default target and runs the object pass:

```
technique MainTecBS? <
    string MMDPass = "object_ss"; bool UseTexture = ...; bool UseSphereMap = ...; bool UseToon = ...;
    string Script =
        "RenderColorTarget0=PointLightRender;"          // bind off-screen colour target
        "RenderDepthStencilTarget=DepthRender;"         // and its depth
        "ClearSetColor=ClearColor; ClearSetDepth=ClearDepth;"
        "Clear=Color; Clear=Depth;"                     // perform the clears
        "ScriptExternal=Color;"                         // draw the scene into the target
        "Pass=DrawPointLight;"                          // run the point-light pass
        "RenderColorTarget0=SpotLightRender;"
        "RenderDepthStencilTarget=DepthRender;"
        "ClearSetColor=ClearColor; ClearSetDepth=ClearDepth;"
        "Clear=Color; Clear=Depth;"
        "ScriptExternal=Color;"
        "Pass=DrawSpotLight;"
        "RenderColorTarget0=;"                          // back to the default target
        "RenderDepthStencilTarget=;"
        "Pass=DrawObject;"                              // finally draw the object
    ;
> {
    pass DrawObject    { VertexShader = compile vs_3_0 BufferShadow_VS(...); ... }
    pass DrawPointLight{ ... }
    pass DrawSpotLight { ... }
}
```

**[source: `library/MLTD/Cherry Colored Love/stage051/Shader/MES40.fxsub` `MES40_Technique(...)` macro
~1329–1372]**; its `STANDARDSGLOBAL` is `ScriptClass="sceneorobject"`, `ScriptOrder="standard"`
**[source: same file ~461–465]**. Note the ordering the interpreter must honour: bind→set→clear→external→pass,
twice, then reset→object pass; and that `RenderColorTarget0=` with an empty value resets to the default target
(§7.1). This is the same shape as the reference's canonical post-effect example **[source: REFERENCE.txt §3
○使用例; §4 ●ポストエフェクト]**.

### E.2 A post-effect with `ScriptExternal` and `LoopByCount`

The reference's own post-effect recipe:

```
technique TShader <
    string Script =
        "RenderColorTarget0=RenderTarget;"             // own target
        "RenderDepthStencilTarget=DepthBuffer;"
        "ClearSetColor=ClearColor; ClearSetDepth=ClearDepth;"
        "Clear=Color; Clear=Depth;"
        "ScriptExternal=color;"                        // draw the other objects here
        "Pass=P0;" ;                                   // run the pass that reads it
> {
    pass P0 < string Script= "RenderColorTarget0=; RenderDepthStencilTarget=; Draw=Buffer;" ; > { ... }
}
```

**[source: REFERENCE.txt §3 ○使用例]** — note the pass resets the target and draws a full-screen quad with
`Draw=Buffer` (the pass-level script overriding the technique-level target binding is the pattern to
implement). And the conditional-pass idiom using a `CONTROLOBJECT` bool as a loop count:

```
bool flag : CONTROLOBJECT < string name = "aaa.x"; >;
technique Tech1 < string Script =
    "LoopByCount=flag;"
    "Pass=Pass1;"
    "LoopEnd=;"
> { pass Pass1 { ... } }
```

**[source: REFERENCE.txt §4 ●オブジェクトの有無でパスをON/OFF]** — a pass runs only while the accessory is
visible; this is the reference's documented way to toggle a pass with an object's on/off state.

### E.3 A per-subset material assignment (`.emd`)

```
[Info]
Version = 3

[Effect]
Obj = RAY_FX\Material_2.0.fx      ; fallback for any material not listed
Obj.show = true
Obj[0]  = RAY_FX\head.fx
Obj[1]  = RAY_FX\head.fx
Obj[2]  = RAY_FX\eyes.fx
Obj[3]  = RAY_FX\eyesLight.fx
Obj[10] = RAY_FX\body.fx
...
```

**[source: `library/by StarNoodle495/AlcanderMMD/RayMaterial/RayMaterialMap.emd`]**. The interpreter applies
`Obj[n]` for material `n` and `Obj` otherwise, and honours `Obj.show`. This is exactly what MMDX12's
`ReadMmeHints` walks today **[source: `src/asset/MmeHints.cpp` `ReadMmeHints`]**.







