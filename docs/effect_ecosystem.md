# MMDX12 native effect ecosystem: design direction

Status: direction v1 (2026-10-19). Architect: Claude, debated with Gemini (gemini-4-argon, two read-only rounds),
decisions by the owner (§7). Inputs: `docs/shader_effect_api.md`, `docs/shader_pt_api.md`, `shaders/pack_api.hlsli`,
`shaders/effect_api.hlsli`, `src/render/ShaderPack.h`, `src/app/ShaderPackStore.h`, `docs/mme/mme_design.md`,
`docs/mme/compat_list.md`, `docs/mme/research/effects_catalog.md`.

This is a direction document, not an API reference. Each item becomes its own spec (and its own section in
`shader_effect_api.md` / `shader_pt_api.md`) when it is built.

## 0. Principle

The MME layer reads the old ecosystem; the shader-pack ecosystem is where new capability lands. We do **not** rebuild
MME in JSON: no arbitrary scene re-renders (`ScriptExternal`, free `OFFSCREENRENDERTARGET` passes), no arbitrary
pipeline state. Packs get typed access to what the engine already draws (G-buffer, IDs, history, focus, audio, scene
roles), so a pack keeps working in every render path and costs nothing while it is off.

Properties every new feature must keep (our lead over MME):
- **One pack, every path**: raster / RT / PT / offline GI (MME is D3D9 raster only). A feature without a PT / GI story
  is not finished.
- **Zero cost when unused**: no targets, no passes, identical frame (the rule `PackEffectPass` already follows).
- **Failure is local**: compile errors fall back to the default shading with an `[E]` line and a toast; never a crash.
- **Library independence**: nothing depends on folder layout, file names or bone-name strings; scene data is resolved
  by role (humanoid map), packs are suggested by declared traits (`"suggest"`), never by game detectors.
- **Rights separation**: assets that cannot be redistributed live in the user's texture folder; assets the author owns
  ship in the pack (§4).

## 1. Gap analysis (what MME users can do that pack users cannot)

| MME mechanism | What it enables | Our gap (today) | Blocked popular effects (catalog rank) |
|---|---|---|---|
| `CONTROLOBJECT`, controller morphs keyed in VMD | time-based direction: fades, flashes, light positions, intensity curves | pack params are static (`EffectStackEntry::params`, `ShaderChoice::params`); no timeline rows; the effect stack is not saved in `.mmdxproj` (`ProjectData` has `mmePost` only); `effect_api.hlsli` reads no bones / lights | BlackOut 31, WhiteOut 46, HgSpotLight 67, HgPointLight 72, PostRimLighting 34 |
| `.x` effect objects + custom VS | particles, magic circles, snow, dust placed in the world | surface packs replace the material PS only ("Vertex shading, edges and the shadow pass stay the app's", `pack_api.hlsli`); no world entity type | AirDust 7, KiraKira 26/28, Rain_Snow 44, WorldSnow 52, SimpleMagic 56, BillboardSet 93 |
| `OFFSCREENRENDERTARGET` + `DefaultEffect` | per-object masks, glow sources, layers, mirrors | `PackResult` = color / alpha / reflectivity / noAo only; `PSPack` skips `AutoLumEmission`; effect packs see colour / depth / normal / motion, no object or class ID | AutoLuminous4 2, ObjectLuminous 33, CrossLuminous 38, M4Layer 29, Croquis改 14 |
| un-cleared render targets | feedback across frames | v4 state = 16 floats; v5 passes live within one frame | MotionBlur3 27, TrackBack 77 |
| `.fx` on stages / props, per-subset files | stage toon / water, mixing material shaders | surface packs bind per character; 6 fixed classes (`PackClass`) | GreenerShader 23, water / floor effects |
| copy a `.fx`, edit constants, redistribute (改変); `.fx` shipped inside song / model packages | a long tail of variants by non-programmers | no file that bundles slider values + stack + pack choice; one official index (`ShaderPackStore::kDefaultIndexUrl`) | — (ecosystem breadth) |

Rough count (Gemini, from the catalog's categories, not verified effect by effect): about half of the top 100 are
structurally impossible as packs today.

## 2. Roadmap

### Phase 1: open the ecosystem (little shader API)

**1. Project-stored stack, keyable params, typed scene inputs.**
- `.mmdxproj` stores the effect stack (and per-model surface choices, as now). Each pack param can get a timeline
  row (`RowKind::EffectParam`, undo through the ordinary track commands), evaluated per frame for the viewport and
  every video renderer; play mode and Look presets use the static values.
- Params and inputs are separate. **Params** = user-facing floats (sliders, timeline, MCP, presets). **Inputs** = typed
  scene data the engine fills, declared in pack.json `"inputs"` (e.g. `"performer.head"`, `"performer.hand_l"`,
  `"light.key"`, `"camera.focus"`) and read with typed helpers (`PackInputPos(k)`, `PackInputFrame(k)`). Roles resolve
  through the humanoid map; the UI may override a role with an explicit bone. No bone-name strings in packs.

**2. Audio-reactive inputs.** `"inputs": ["audio"]` exposes `PackAudioBeat()` (phase in the beat), `PackAudioDownbeat()`,
`PackAudioOnset()`, `PackAudioEnergy()`, `PackAudioSection()` sampled at the frame's audio time from the per-song
`AnalyzeBeats` result (`src/audio/BeatAnalysis.h`, already used by the MCP beat tools). Precomputed, so video renders are
deterministic. Not requested = not analysed. MME has no equivalent without plugins: a headline feature.

**3. Look presets (`.mmdxlook`).** One file = lighting preset + per-class surface choices (pack id, version, params) +
effect stack (+ params) + LUT + bloom look. Packs are referenced by id + source index; the app offers to install what
is missing. A preset may bundle the author's own small assets (LUT strips, ramps), same rules as packs (§4). Presets
are gallery items of their own type (§3): the 改変 culture without HLSL.

**4. Capabilities instead of linear versions.** `apiVersion` stays at 6 as the last linear number; new features are
declared in `"requires": ["state", "passes", "inputs.scene", "inputs.audio", "inputs.id", ...]`. An app that lacks a
capability refuses the pack with that name in the status line; the root signature / PSO variant binds only what is
required. The existing macros (`PACK_API_ADDUV`, `PT_PACK_API_LIGHTING`) map onto capability names.

### Phase 2: surface-to-effect bridge

**5. Richer surface output + an ID target.** `PackResult` gains `normal` (default `s.N`), `emission` (HDR, added to the
scene colour like `AutoLumEmission`) and `tag` (a few author bits). While an enabled effect requires `inputs.id`, the
scene pass (and the PT primary hit / offline G-buffer) writes an `R16_UINT` target = model slot | material class | tag,
read with `PackSampleId(uv)`. Covers most `OFFSCREENRENDERTARGET` uses (luminous masks, character-only post, layers).
No separate emission target in v1 (bandwidth); add it as a capability if a pack needs emission intensity apart from
the colour.

**6. Pass history and mips.** `"passes"` entries gain `"history": true` (the pass's previous-frame result, cleared on
reset, same reset / advance rules as v4 state) and `"mips": true`; helpers `PackPassSampleHistory(n, uv)`,
`PackPassSampleLevel(n, uv, lod)`. Offline stills warm history up like the state pass (count decided by prototyping).

### Phase 3: world and coverage

**7. World packs (`"type": "world"`).** A world pack is a placed, keyable entity, the native counterpart of an MME
`.x` accessory: an outliner row (`ModelKind::Effect` already exists), transform, keyable params, parentable to a bone
through `PropAttach`; song packages and Look presets may place one in play mode. Particles are a **pure function of
(id, time)** (`PackParticleSpawn(id, t)` + `PackParticleShade(i)`, `"instances"`, `"blend"`), so seeking, reversing,
video and the GI shutter reproduce them exactly. Raster / RT draw them directly; PT / GI v1 composite them as a depth-
tested layer (like the MME primary layer), emissive particles as lights later.

**8. Stage / prop packs + MME porting.** `ShaderChoice` for stages and props, stage classes (`stage`, `floor`, `glass`).
An agent skill (`skills/`) ports simple post `.fx` files to effect packs **only when the effect's licence allows it**,
plus a guide mapping MME semantics to pack API; `OFFSCREENRENDERTARGET` effects are redesigned on `inputs.id`, not
translated.

**Deferred: vertex hook (`PackDeform`).** It must run identically in the raster VS and in `RtScene`'s skinning
(`skin.hlsl`), stage BLASes are built once (`PREFER_FAST_TRACE`, `RayTracing.h`) and motion vectors would need two
evaluations. Revisit as a character / prop-only capability (those BLASes are rebuilt every frame anyway).

## 3. Open gallery

Decided (§7): users may add external index URLs, and the official gallery becomes a space **anyone can publish to**;
admin review grants a **verified** badge (blue check) instead of gating publication.

- **Sources.** The app keeps a list of indexes (official first, then the user's, ini lines). Every remote pack carries
  its source; an installed pack remembers the index it came from and **updates only from that source** (a third-party
  index publishing a higher version of the same id cannot take over an installed pack). Same id from two sources = two
  entries with their source shown; installing one replaces the other only after a prompt.
- **Verified.** The badge is a field of the official index only; the app shows it only for entries whose source is the
  official index URL. A third-party index cannot claim it. Verified means reviewed (works in the declared render
  paths, licence and asset rights plausible, previews honest), not endorsed.
- **Publishing.** Upload (website) runs automated validation equivalent to `pack_check --compile` (manifest, file
  whitelist and limits, textures, compile of every entry point and pass) before the entry goes live. Required fields:
  licence, authors, the asset declaration (§4). Pack ids are globally unique on the official gallery, owned by the
  first publisher's account; existing ids stay with the official packs. Accounts, ownership transfer and moderation
  tools are website work (MMDX12_Web).
- **Unverified packs in the app.** Installing shows the source and the missing badge and asks once; nothing from a
  song / model folder or a third-party index is ever enabled automatically (same rule as `"suggest"`: the app suggests,
  the user chooses).
- **GPU safety.** Packs cannot touch files or the network, but a runaway shader is a TDR (device lost = the app quits,
  `Dx12Context::DeviceLost`). Mitigations: static limits in validation (bounded loops, instruction / sample budgets);
  a crash guard (a marker written before a pack's first compile / first frame, cleared after the first successful
  Present; found at the next start = that pack is disabled and reported); a revocation list in the official index
  (zip SHA-256) that disables known-bad packs; a report button on gallery entries.
- **Look presets and world packs** are gallery types next to surface / effect, with the same rules.

## 4. Assets in packs

Decided (§7): the user texture folder stays for assets that cannot be redistributed, but packs **may and should**
carry assets their authors made. A pack that can only reference user folders would be the wrong direction.

This is already true technically: pack.json `"textures"` resolve user folder → pack folder → white
(`ResolvePackTextureFile`, API v2). What changes is that it becomes explicit in the format and the UI:
- Each texture entry may declare `"origin": "bundled" | "user"` (default `bundled` when the file is in the pack, `user`
  when it is not). The manager reports missing `user` textures as "connect a texture folder" and missing `bundled`
  ones as a broken pack; the user folder can still override a bundled texture (same file name rules as today).
- Publishing asks the author to confirm that bundled assets are their own work or licensed for redistribution;
  game-extracted files must be `user` origin. The verified review checks this.
- The current limits (16 textures, 4096 px, 32 MB, 200 files) remain the defaults; a capability may raise them later
  (world packs with sprite sheets, LUT packs) once the open gallery's storage costs are known.

## 5. MME stance

- **Opening an existing MMD folder, a PMM or a song package with MME effects = faithful** (decided, §7): the MME layer
  runs what the files reference, as MMD would, including effects that have a native or pack equivalent. Native
  equivalents are offered, never substituted automatically.
- The MME layer stays as compatibility and on-ramp; its scope is frozen to fidelity fixes (compat list tiers).
- New capability (timeline keys, audio inputs, ID / history, world entities, PT / GI integration, the gallery) goes to
  packs only, so authors have a reason to pick the pack format.
- What we promote is our own format: `.mmdxproj` (projects, saved stack and keys) + packs + Look presets. PMM stays
  import-only; an explicit "convert to native" action may build a `.mmdxproj` with native / pack replacements as a
  new project, leaving the faithful one untouched.

## 6. Risks

- Rebuilding MME's freedom inside packs (scene re-renders, raw state) would break TAA / upscalers / PT / GI and the
  zero-cost rule. Every proposal above exposes engine data instead.
- The bottleneck is direction, not shading: without saved stacks and keyable params the best effect packs stay
  unusable in videos. Phase 1 item 1 comes before any new shader capability.
- An open gallery invites low-quality and hostile uploads; the verified badge, source-pinned updates, the crash guard
  and revocation are what keep "open" from meaning "unsafe".
- Capability lists must be adopted before Phase 2, or the version / macro fragmentation grows with every feature.

## 7. Decisions

| date | decision |
|---|---|
| 2026-10-19 | External gallery index URLs: allowed. |
| 2026-10-19 | The official gallery is open to anyone; admin review adds a verified (blue check) badge. |
| 2026-10-19 | User texture folders stay for non-redistributable assets; packs carry their authors' own assets. |
| 2026-10-19 | Existing MMD folders / PMM / MME content open faithfully (no automatic substitution). |
| 2026-10-19 | The format we promote is our own (`.mmdxproj` + packs + Look presets). |

| 2026-10-19 | Gallery scope = app + index format + website (upload, accounts, admin) + deployment, all in this effort. |
| 2026-10-19 | Accounts: GitHub + Discord sign-in for the gallery (OAuth; credentials stay server-side). |
| 2026-10-19 | Backend: a small Node service next to the website, deployed with the site. |
| 2026-10-19 | Admin review: a token-protected admin page (review queue, grant / revoke verified, hide, revoke a zip). |
| 2026-10-19 | Pack ids on the official gallery: flat, globally unique, `[a-z0-9_]{3,40}`, owned by the first publisher's account; ids of existing official packs are reserved. |
| 2026-10-19 | Look presets reference packs by id + minimum version + source index (no exact pin); missing / older packs are offered for install. |
| 2026-10-19 | Pass history in offline stills warms up like v4 state (64 iterations, one constant). |
| 2026-10-19 | Asset limits stay (16 textures, 4096 px, 32 MB, 200 files); world packs get 32 textures / 64 MB. |
| 2026-10-19 | World packs may hold GPU simulation state (particle buffers, ground height maps): a fixed-step, deterministic simulation. Seeking re-simulates (the engine re-evaluates poses at the fixed step; short-lived state over a window before the target time, long-lived state such as footprints from the start at reduced rate), so viewport, video and GI give the same result for a frame. Pure f(id, t) stays the rule for stateless particles. |
| 2026-10-19 | `PackDeform` stays deferred (not part of Phases 1-3). |
| 2026-10-19 | Sample world pack "snow" (falling snow + ground snow cover that the performers' feet kick up and leave prints in) ships built-in (`shaders/packs/snow`). |
| 2026-10-19 | Work lands as local commits per phase on master (no push, no release); the website repo is committed and deployed. |
| 2026-10-19 | Pack parameters (capability `params.ext`): up to 128 (one float4 slot each in a constant buffer, per model for surface packs), types float / int / bool / color (sRGB picker, linear in HLSL) / vec3, collapsible groups (`"groups"` + per-param `"group"`). Stored values keep the `map<string, float>` format (vector components as `key#0..2`), so ini / projects / looks are unchanged; color / vec3 key per component on the timeline, int / bool stepped. Packs without the capability keep the 16-float layout. |
