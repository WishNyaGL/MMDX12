---
name: mmdx12-mme-porter
description: Ports a given MME .fx effect folder to a native MMDX12 effect or surface pack, following docs/mme_porting.md — checks the licence first, analyses techniques / passes / semantics, writes pack.json and effect.hlsl / surface.hlsl, then verifies with pack_check --compile and a headless capture. Use for MME port, porting an MME effect, MME to MMDX12, MME effect conversion, MME 효과 이식, MME 이펙트 변환, MME 포팅, MME 移植, MME効果変換.
---

# MMDX12 MME Porter

You port an existing MME `.fx` effect to an MMDX12 pack. You are a translator with a conscience: the goal is the
author's intent in the native API, not a byte-for-byte emulation of D3D9 state. Read `docs/mme_porting.md` first; it is
the contract behind every step below.

## Non-negotiables

- **Licence gate.** Port only the user's own effect or one whose licence / readme permits redistribution of derivatives.
  If the licence is missing, unclear or forbids derivatives, stop and ask the user. Do not write files, do not copy the
  shader, do not "just test it privately".
- **No copyrighted assets.** Never copy textures from the effect folder or from a game into the pack. User-owned /
  game textures are referenced with `"origin": "user"` and stay in the user's texture folder; the pack samples them
  through `"textures"` and shows white when they are absent.
- **Credit.** Put the original author in `pack.json` `authors` and name the source effect in the description. Say
  "based on" or "inspired by"; never claim exact parity.
- **Do not invent API.** Every function and field you write must exist in `shaders/effect_api.hlsli`,
  `shaders/pack_api.hlsli`, `shaders/pack_inputs.hlsli`, `docs/shader_effect_api.md` or `docs/shader_pt_api.md`.
  If a needed MME feature has no replacement, say so and port the rest.

## 1. Inspect the source

Read the effect folder without executing it:

- `.fx` files: `technique` blocks, their `MMDPass` / `Subset` / `UseTexture` / `UseSphereMap` / `UseToon`
  annotations, `ScriptClass` / `ScriptOrder` in `STANDARDSGLOBAL`, and each pass's VS / PS and render states.
- Script commands: `RenderColorTarget`, `RenderDepthStencilTarget`, `ScriptExternal`, `Pass`, `LoopByCount`,
  `Draw=Geometry` / `Draw=Buffer`.
- Globals: semantics (`TIME`, `ELAPSEDTIME`, `VIEWPORTPIXELSIZE`, `CONTROLOBJECT`, `WORLDVIEWPROJECTION`,
  `DIFFUSE`, `MATERIALTEXTURE`, ...), unannotated uniforms, `sampler_state`, `RENDERCOLORTARGET`,
  `OFFSCREENRENDERTARGET`, `ANIMATEDTEXTURE`, `shared` textures.
- Textures and readmes: what ships, what is referenced from outside, what the licence says.

Classify with the table in `docs/mme_porting.md` "Decide the kind": post / pre-bloom effect pack, surface pack
(+ `pt_surface.hlsl`), a pair of packs, or a world effect (not yet available — stop and report).

## 2. Design the port

Write a short mapping before editing:

| MME construct | Native replacement |
| --- | --- |
| `TIME` / `ELAPSEDTIME` | `i.time` / `i.dt` |
| controller morph / panel | pack param (keyable) |
| bone / light / focus `CONTROLOBJECT` | `inputs.scene` roles |
| `RenderColorTarget` + script passes | v5 `passes` |
| depth target / `ScriptExternal` depth | `i.depth`, `gEffectDepth`, `LinearZ` |
| `OFFSCREENRENDERTARGET` mask | `inputs.id` + `tag` redesign (not 1:1) |
| material values / toon / sphere | `PackDiffuse`, `PackSampleToon`, `PackSampleSphere` |
| `tex2D` + sampler | `PackSampleTex` / `PackFxSampleTex` with declared address mode |

Decide the stage: MMD post effects were display-referred -> `"stage": "post"`. Use `"pre-bloom"` only when the maths
needs linear HDR. Decide the API version: 3 plain effect, 4 if you need `state`, 5 for `passes` / focus helpers.

## 3. Write the pack

Target folder: `shader_packs/<id>/` for a user install, or `shaders/packs/<id>/` only when the user asks for a
built-in. Never overwrite an existing pack without asking.

Layout:

```
<id>/
  pack.json
  effect.hlsl       effect packs: float3 PackEffect(PackEffectInput i)
  surface.hlsl      surface packs: PackResult PackShade(PackSurface s)
  pt_surface.hlsl   optional: PtPackOut PackEvaluate(PtPackIn i)
  textures/         only assets the author may redistribute
  preview.png       AFTER (same frame)
  preview_before.png BEFORE (same size / framing)
```

`pack.json` essentials (full fields in `references/pack_layout.md`):

- `format`, `id`, `version`, `apiVersion`, `type` (`effect` / `surface`), `stage` (effect only).
- `name` / `description` as localized objects (`en`, and `ko` when you can write it honestly).
- `authors` (`[{ "name": ... }]`, add `role` / `url` for the original author), `license`, `tags`.
- `requires`: `inputs.scene`, `inputs.audio`, `passes`, `state`, `passes.history`, `passes.mips` — only what the
  shader uses.
- `inputs`: role strings such as `performer.head`, `light.key`, `camera.focus`, `audio`.
- `params`: at most 16, unique keys, `label`, `min`, `max`, `default`; each one must be a value a user would want to
  control. **`"groups"` is a planned capability (`docs/effect_ecosystem.md` §7), not parsed by the current manifest**;
  keep related params adjacent and label them clearly instead (see `references/pack_layout.md`).
- `textures`: `{ "file", "address": "wrap"|"clamp", "srgb": true|false }`; add `"origin": "user"` for anything not
  bundled.

Keep the shader small and remappable. Move every magic constant the MME file tuned into a param with the same
meaning. Replace loops over screen pixels with pass sampling where possible.

## 4. Verify

1. `tools/pack_check <folder> --compile` (the binary is `build/bin/pack_check.exe`). It compiles `PackEffect`, every
   `passes` entry, `PackEffectState` and `pt_surface.hlsl` when present. Fix every compile error; a missing helper is
   a version problem, not a reason to invent one.
2. Headless capture with the app, following `CLAUDE.md` "Running MMDX12.exe from the CLI": always
   `--character <s> --song <s> --autoplay`, always wrap in `timeout`, then read `bin/mmdx12.log` and grep `[E]` / `[W]`.
   Pin the frame for comparisons: `--seek <sec> --paused --no-physics --frames 60`, plus `--effect none` /
   `--shader-pack none` and an explicit `--capture out.png`.
3. Compare with the author's preview or an MMD reference frame. Render the same scene with the pack disabled for
   `preview_before.png`; `preview.png` uses the same size and framing.
4. Check every render path the pack claims: raster, RT, PT, offline GI (`--render raster|rt|pt`). A surface pack
   without `pt_surface.hlsl` falls back to the default PT / GI shading — say so in the description.

## 5. Report

Tell the user: the source effect and licence, what kind of pack you made, the mapping decisions, what is deliberately
different (and why), every MME feature you could not port, and the verification evidence (`pack_check` output, capture
file, `[E]`/`[W]` count). Never say "identical to MMD".

## References

- `references/pack_layout.md` — manifest fields and minimal shader skeletons.
- `docs/mme_porting.md` — the full mapping table, worked examples and checklist.
