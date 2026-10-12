# CLAUDE.md

MMDX12: a Windows Direct3D 12 MikuMikuDance client (C++20). See README.md for features and CLI;
progress.md is the session log. Read its latest entry first.

## Build & verify
- `build.cmd [dir] [--target X]` sets up the VS 18 environment and builds with CMake + Ninja (RelWithDebInfo).
  The default dir is `build`; the output is `build\bin\MMDX12.exe`. CMake and Ninja come from pip (`%USERPROFILE%\miniconda3\Scripts`).
  From bash: `cmd //c "F:\\repos\\MMDX12\\build.cmd build"`.
- The source lists are globbed (`CONFIGURE_DEPENDS`). If a new `.cpp` is not picked up, delete the build dir.
- Header dependencies come from cl's localized `/showIncludes` prefix stored at configure time; a dir configured from a
  UTF-8 console stored it in the wrong code page (no deps: header edits rebuilt nothing, stale-layout crashes).
  build.cmd detects that and reconfigures once.
- Shaders are compiled at runtime from `build/bin/shaders` (the `copy_shaders` target keeps them in sync).
  Raster shaders use FXC (`*_5_1`); ray-query shaders use DXC (`*_6_5`, `-HV 2018` because common.hlsli uses vector ternaries that HLSL 2021 rejects). `dxcompiler.dll`/`dxil.dll` are copied from the Windows SDK.
- Upscaler runtime DLLs are git-ignored: run `tools/fetch_sdks.ps1` once (DLSS 310.9.1, XeSS 3.0.2, FidelityFX 2.3.0), then rebuild. Missing DLLs only disable that upscaler.
- Render path / upscaler from the CLI: `--render raster|rt|pt`, `--upscaler none|dlss|fsr|xess`, `--upscale-quality native|quality|balanced|performance|ultra`.
- PBR material data (2.0.0, temporary until the UI): `--material-source default|ray` (Ray-MMD `.emd` data for every model),
  `--pbr-debug none|albedo|normal|roughness|metalness|ao|emissive|model` (lit raster outputs that channel; `asset/PbrMaterial`, `shaders/pbr_common.hlsli`).
- No debugger is installed (no cdb). For crashes, use the dbghelp `CrashFilter` in `tools/render_smoke.cpp`.
- Visual verification without a human: render a frame headlessly, then Read the PNG. First read "Running MMDX12.exe from the CLI" below.
  - Play scene: `MMDX12.exe --character <s> --stage <s|none> --song <s> --autoplay --seek <sec> --frames N --capture out.png`
    Unit formation: `--formation <spec>` (one entry per part in part order, each a character name substring or `-` for empty; optional `*` suffix marks the center; skips the modal).
  - Menu screens: `--screen select|bench --frames N --capture ui.png`
  - Offline GI still/video: `--autoplay --seek <sec> --offline-still out.png` (or `--offline-video out.mp4 --offline-range a b`);
    add `--offline-size 960 540 --offline-spp 256` for quick checks. The app quits by itself when done.
    Video format: `--offline-fps 30 --offline-bitrate 20 --offline-quality 0..3` (default: the saved lobby dialog settings, `AppSettings::video`).
    Lobby path (select screen -> load -> render -> back to select): `--character c --song s --screen video --offline-video out.mp4 --offline-range a b`.
    `--screen video` alone opens the lobby's render dialog for a capture.
    Video renderer: `--offline-renderer raster|rt|pt|gi` (the real-time ones render frame by frame at the video size via `Renderer::Render` +
    `ReadFinalImage`; PT accumulates several passes per frame). The render dialog measures its time estimate by itself: a headless sample render behind the UI (`--screen video` shows it; log `VIDEO PROBE`,
    results in `videoProbe=` ini lines). `--offline-probe` runs the same measurement from a play scene.
    The dialog's effects are `VideoRenderConfig` (bloom, convolution bloom, volumetric, DoF for every renderer: GI's thin lens
    follows `OfflineJobDesc::dof` / `dofAperture`, the render benchmark keeps the defaults); stills use the effects chosen at
    scene entry (`AppSettings`). Check `build_dev` builds when `build/` is locked by a running render.
  - Studio: `--character <s> [--stage <s>] [--song <s>] --screen studio --seek <sec> --frames N --capture out.png`.
    `--screen studio` without `--character` opens a new empty project; `--project <file.mmdxproj>` opens a project.
    Studio render (quits when done): `--project p.mmdxproj --offline-video out.mp4 [--offline-range a b]` (timeline
    seconds; default: the timeline range, else the whole project; motion camera; project audio + offset) or
    `--offline-still out.png [--seek s]` (GI, the view the viewport shows). `--offline-renderer/-size/-fps/-spp` apply.
  - High-DPI captures: `--ui-scale 1.5` replaces the monitor's DPI scale (1080p window + 1.5 = a 150 % laptop screen).
  - Scripted UI input: `--ui-script file.txt` (lines `<frame> <command> [args]`: move/down/up/click/dblclick/wheel/key,
    `capture <png>`, `text <chars>`, `mod ctrl|shift down|up`, `studiostate` logs a `STUDIOSTATE` line,
    `studioexport`/`studioimport <vmd>` and `studiovpdexport`/`studiovpdimport <vpd>` skip the file dialogs; pose editing:
    `studiobone <name>` clicks that bone's joint, `studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [steps]` drags a gizmo
    part; `studiostate` also logs `STUDIOPOSE`, `STUDIOPROJ` (including `aspect=`) and one `STUDIOMODEL` line per model). Projects/models
    without dialogs: `studionew`, `studiosave`/`studioopen <file>`, `studioautosave`, `studioadd <character|stage|prop> <file>`,
    `studioaddlib <character|stage> <substr>`, `studiosong <substr>`, `studioaudio <file|none> [offset]`,
    `studioselect`/`studioremove <index>`, `studioreplace <index> <file>` / `studioreplacelib <index> <substr>` (swap a model's file), `studiorename <text>`, `studioattach <parent|-1> <bone|-> tx ty tz rx ry rz s`,
    `studioaspect <w:h>` (frame aspect preset, e.g. `9:16`, `16:9`, `2.39:1`).
    Focus: `studiofocus <frame> <auto|target|manual> [model index|distance] [aperture] [transition] [head|upper|center]`
    upserts a focus key (`studiostate` logs a `STUDIOFOCUS` line: distance, aperture, subject).
    Lighting rig: `studiolight <vmd|preset|custom> [presetIndex] [spotCount] [keyOverride 0|1]` sets the studio's
    lighting source (+ demo spots / key override; `studiostate` logs them), `studiokeyspot <spotIndex> [frame]` keys
    that spot's values through the ordinary undoable insert.
    While a script runs, real mouse/keyboard input is ignored.
    Frames count like `--frames`. Use it to click through editor features headlessly (examples: `captures/studio/t*.txt`).
  - `--frames N` counts frames only inside a scene or with `--screen`; a run with neither never quits.
  - Benchmark: `--benchmark dx12-raster-fhd --bench-frames 600 --frames 100000` (result goes to `build/bin/mmdx12.log` as a `BENCHMARK` line)
  - GI render benchmark: `--benchmark dx12-gi-render` (quits by itself; ~41 s). Quick check: add `--bench-spp 64 --offline-size 960 540`.
    Add `--frames 100000 --capture out.png` to capture the result screen.
  - Each run opens a visible window: never launch app runs in parallel or in batches without telling the user.
- Tools:
  - `asset_probe <library> [--full]`: scan, classify (prints notes on skipped/guessed files), and load everything
  - `anim_probe <pmx> <vmd> [cam.vmd]`: IK convergence, CPU skinning bounds, NaN scan, physics scan (explosions, cost)
  - `render_smoke`: renders a cube with no assets
  - `vmd_roundtrip <file|dir> [--vpd-selftest]`: VMD load -> save -> load comparison (SaveVmd), VPD self test
  - `rest_pose_test [model|dir...]`: T-pose -> A-pose self test (synthetic model: bones, weights, morphs, bodies, joints), then the arm angle of every PMX/PMD found
  - `studio_edit_test`: Studio key-edit core (move/insert/delete frames, undo byte budget, 100k-key timings)
  - `studio_gizmo_test` / `studio_pose_test`: gizmo projection/hit/drag math and bone overlay; mirror names/poses, VPD pose ops
  - `studio_project_test`: .mmdxproj save/load round trip, VMD naming/cleanup, atomic writes, prop offset matrix
  - `pbr_tex_probe <pmx> [--dump <dir>] [--verbose]`: PBR (Ray-MMD) map decode per material: paths, channel, mip-0 size, loops, UV scale/flip
  - `studio_focus_test`: DoF focus track evaluation, automatic focus (score, hysteresis, rack), focus keys in projects
  - `mmdx12_mcp [--pipe <name>] [--pid <pid>] [--launch] [--timeout <ms>]`: Model Context Protocol stdio bridge
  - `mcp_smoke.py [--handshake-only] [--scenario]`: MCP test client (protocol handshake & end-to-end scenario)
  - `pack_previews.py [--only ids] [--out-dir d] [--bump]`: re-shoots every public pack's before/after preview over the pipe
    (shot table in the script; scene-only `screenshot`, `set_render_settings shader_texture_folder`; writes the website
    repo's `shader-packs/<id>/` and the built-in packs, contact sheet in `captures/pack_previews`)
  - `make_update.py <stage> --version X --out d [--base-url u]`: delta-update manifest + content-addressed objects (+ .gz)
    for the smart updater (feed `manifest` / `manifestSha256`); `update_e2e.py`: headless updater E2E (delta + corrupt-object fallback)
  - `mcp_pipe.py` / `mcp_x.py`: talk to a running app's pipe without the bridge (`python tools/mcp_x.py "click 플레이" "wait 30" "shot out.png" "items 셰이더"`)
- The play bar auto-hides while playing with no mouse movement, so captures usually don't show it.

## Running MMDX12.exe from the CLI (read this before EVERY launch)
Agents keep misusing the CLI and then blaming the app. These rules are verified (2026-10-12); follow them literally.
- **No `--autoplay` = no scene.** `--character/--song` only preselect cards in the lobby. Without `--autoplay` (or when
  either name does not resolve) the app sits in the lobby, and `--frames` never counts there (it counts only inside a
  scene or with `--screen`), so the run never quits. Every scene capture needs `--character <s> --song <s> --autoplay`.
- **Always wrap a run in `timeout <sec>`** and read `<build>/bin/mmdx12.log` afterwards (overwritten every run;
  `mmdx12.previous.log` keeps the one before). Scene reached = `motion bound` + `STARTUP first frame` lines; neither =
  it never left the lobby (bad `--character/--song`, multi-part song without `--formation`, or a cold library scan in a
  fresh build dir: the first run of a build dir rescans the library, ~25 s). Grep `[E]`/`[W]` after every capture;
  unknown arguments are only a `[W] unknown command line argument` line, never an error.
- **Name matching** = case-insensitive substring of the library id (the relative path, e.g.
  `model/Sekai Model Pack v4/LeoNeed/Ichika/01_9001.pmx`) or the display name; the FIRST match wins, so use a
  distinctive fragment (`01_9001`, `LeoNeed/Ichika`). `--stage none` = no stage. Songs with >= 2 dance parts open the
  formation modal: pass `--formation` (see below) or pick a one-part song. Ids: `build/bin/cache/library_result.json`.
- **Same frame across runs (A/B comparisons): `--seek <sec> --paused --no-physics`.**
  `--seek` alone keeps playing: the capture happens `--frames` frames later in wall-clock time, so two identical runs
  differ (measured: 113k of 518k pixels). `--paused` holds the scene at the seek time (residual 1.2k px = physics
  settling); adding `--no-physics` makes two runs bit-identical (measured: 0 px). Use `--frames 60` or more so TAA
  converges. `--seek`, `--autoplay` and `--paused` are one-shot: consumed when the scene loads.
- **Settings come from `<build dir>/bin/mmdx12.ini`, one per build dir**, and they differ (window size, lighting preset,
  DoF, effect stack, shader packs per character). An A/B between two build dirs must either copy the same ini into both
  (back it up and restore it) or pin everything on the command line: `--width W --height H --render raster|rt|pt
  --upscaler none --lighting <n> --dof 0|1 --effect none --shader-pack none|<id>` (+ `--camera tx,ty,tz,yawDeg,pitchDeg,dist`
  for a fixed free camera; it implies `--free-camera`). A HEAD baseline = `git stash`, build another dir, `git stash pop`.
- **One window at a time.** Every run opens a visible window: never launch runs in parallel, tell the user before a
  batch. A running exe locks its build dir (`LNK1168`): when a worker is building in `build/`, run another build dir.
- Canonical A/B capture (run once per build, then diff the PNGs):
  `timeout 120 <build>/bin/MMDX12.exe --character 01_9001 --stage none --song Evergreen --autoplay --seek 20 --paused --no-physics --frames 60 --effect none --shader-pack none --capture out.png`

## Architecture (src/)
- `core`: logging, text encodings (Shift-JIS/UTF-16/UTF-8).
- `asset`: PMX/VMD parsers, image loading (stb with a WIC fallback, also from memory), `AssetLibrary`, `ModelImport`.
  - Unit songs: same-length dances in one song folder are the song's `SongAsset::parts` (byte-identical renamed copies
    merge, labels from the file names; `danceVmd` stays the primary part), ordered left to right on stage and with
    `centerPart` = the part closest to the group's middle in most samples (from `VmdProbe::stageX`, the sampled
    センター + 全ての親 X). Cameras: every candidate in `SongAsset::cameraVmds`; the default prefers plain MMD cameras over
    plugin ones ("Camera for FOVMOD", MME/MMM; "normal" wins); the lobby's song header offers the others when there are
    several (`songCamera=` ini, `App::SongAsChosen` applies it to every load). Play / Enter / the lobby video render go
    through `App::StartLobbyLoad`: >= 2 parts open the formation modal (`app/UiFormation.cpp`: slots, center, auto
    formation by part-label tokens, saved per song by dance file as `songFormationPart=` / `songFormationCenter=` ini lines); the cast loads into
    `ScenePackage::character` (the center: focus, spotlights, F framing) + `extraCharacters`, posed every frame as
    `SceneRuntime::extras` (the render benchmark's extras stay posed once).
  - `AssetLibrary` precedence: app overrides (`library_overrides.json` next to the exe, right-click on a card) > `mmdx.json`
    sidecars > folder names (characters/models, stages, songs/motions, several languages) > content. Several songs in one
    folder are split only when it also holds several audio files or cameras; audio/cameras are matched by length + file name.
    New installs get `assets/library_template` (characters/ stages/ songs/) copied into an empty library.
  - `RestPose` (asset/RestPose.h): `LoadModelFile` turns a native PMX/PMD *character* whose upper arms rest above 20 deg below horizontal
    (T-pose: the Project Sekai rips; every other library model measures 28..43 deg) down to MMD's 38 deg A-pose, because VMD rotations
    are relative to the rest pose. Rotates the 腕 subtree (+ IK bones chasing it) about the shoulder: bone positions, vertices (linear
    blend by the chain's weight share), vertex morphs, rigid bodies, joints (follow body B), tail / fixed / local axes. VMD and bone
    morph data stay valid (MMD bone frames are world-aligned). Stages / props / glTF-FBX imports (own `ToMmdArmPose`) are untouched.
  - `ModelImport`: `LoadModelFile(path, role)` reads PMX natively and converts glTF/GLB/VRM (cgltf) and FBX/OBJ (ufbx) into a
    `PmxModel` via `ImpScene` (ImportScene.h). Characters: humanoid map (VRM table or name dictionary) -> MMD bone names,
    synthesised センター/グルーブ + leg IK, arms re-posed to 38 deg (MMD A-pose), morph aliases (あいうえお, まばたき); no physics.
    Stages: baked, one root bone, moved onto their main floor when nothing is under the origin (`ModelRole::Prop`: baked,
    not moved — studio props). Embedded textures live in
    `PmxModel::embeddedTextures`. Test assets: `captures/testlib` (messy layout) and `captures/assets_dl` (three.js/Khronos/VRM samples).
- `anim`: `ModelInstance` (bones, append, IK, morphs) and `Motion` (VMD Bezier evaluation, camera).
  - `PhysicsWorld` (Bullet, `Physics.cpp` only) runs between the before- and after-physics bones in `UpdatePose(dt)`.
    Only the character enables it. `App::UpdateScene` derives dt from the motion clock and resets the bodies on seeks/loops.
- `render`: `Dx12Context` (device, frames, descriptors, `UploadBatch`, PNG capture) and `Renderer`.
  - The renderer runs an ordered `IRenderPass` list (see `Passes.h`): Shadow → Scene → Resolve → PathTrace → SSAO → SSR → Composite → Volumetric → TAA → Upscale → DoF → Bloom → Post → Backdrop → Present. `PassContext::path` decides which passes work (RayTraced: ray-query sun shadows in the scene PS, RTAO, RT reflections; PathTraced: compute path tracer + temporal/à-trous denoiser writing the G-buffer). Passes own their targets via `OnResize`; inputs are bound through per-frame `TransientDescriptors`.
    PT outlines: in PT mode `ScenePass::DrawPtEdges` rasterises a depth pre-pass (`PSDepthAlpha`) + the edges (default or the
    pack's) into the cleared MSAA scene targets with the tracer's jittered camera (`RenderTargets::ptEdges`), and
    `PathTracePass` composites that layer over the denoised colour (`pt_edge.hlsl`, outline velocity where it covers), as the
    offline GI renderer composites its outline layer.
    Pass implementations live in `Passes.cpp` and `Pass*.cpp` (shared helpers in `PassCommon.h`). Volumetric and the FFT bloom are `ComputePipeline` (cs_6_5) and silently disable themselves without DXR-class hardware.
    Colour LUTs (`ColorLut.h`) are 32³ strips uploaded with `Renderer::SetColorLut`; PostPass applies them after the sRGB encode.
  - `GpuModel` keeps a 3-frame ring of bone/morph buffers so the previous pose (motion vectors) stays valid.
  - `GpuModel` handles GPU skinning. `RtScene` (RayTracing.h) skins every model into world-space `RtVertex` buffers with a compute shader and builds BLAS (characters every frame, stages once) + TLAS; shaders read geometry bindlessly (`rt_common.hlsli`, `RtGeometry` table, unbounded tables over the whole SRV heap, root signature 1.0 so descriptors stay volatile).
  - Render resolution (`targets.width/height`) vs output resolution (`outWidth/outHeight`): the upscaler (DLSS/FSR/XeSS behind `IUpscaler`) runs after TAA/composite; bloom, post, backdrop and present work at output size. Jitter uses the FSR convention (`jitterPx`), motion vectors are uv(cur) − uv(prev) so SDK MV scale is −renderSize.
- Theme: `ui::P()` is the live palette (light / dark, `ui::SetTheme`); never cache colours in statics. `App::ApplyTheme` (settings `theme`, `--theme auto|light|dark`) runs before the next ImGui frame and re-runs `ui::ApplyStyle`. Capture both: `--theme dark|light`.
- `app`: the `App` state machine, ImGui screens (`Ui*.cpp`) built on `UiKit` (tokens, fonts, widgets; see DESIGN.md), `ThumbnailCache`, `Lighting` presets, `SceneLoader` (worker thread), benchmark, WinHTTP leaderboard.
- `audio`: miniaudio. The audio cursor is the master clock.
- `studio` (+ `app/UiStudio*.cpp`): the Studio editor (Screen::Studio). `StudioDoc` holds the models with name-keyed editable
  motions (`MotionData`, StudioMotion.h, converted to VmdMotion and re-bound with `BoundMotion::Bind` after edits), the camera
  track, `CommandStack` undo (edits are `TrackEditCommand` track snapshots / `MotionSwapCommand`) and the editor state.
  `UiTimeline`/`UiBezier` are data-agnostic widgets. The viewport is the renderer drawing into `RenderSettings::viewport*`.
  Compatibility with MMD goes through standard files: VMD (`SaveVmd`) and VPD (`asset/VpdFile.h`); PMM is not supported.
  - VMD bone interpolation: bytes 2/3 are MMD physics flags; Z/rotation x1 are bytes 17/18 (`GetBoneCurve`/`SetBoneCurve`).
  - Pose editing (`app/UiStudioPose.cpp`, `studio/Gizmo.*`, `studio/StudioPose.*`): viewport edits go to the model's
    `PoseLayer` (bone/morph overrides valid at one frame, applied after `BoundMotion::Evaluate`, so drags never re-bind).
    Leaving the frame discards it as an undoable `PoseEditCommand`; Register (I, Ctrl+I all bones) writes keys +
    clears the layer in one `CompositeCommand`. Bone values are VMD-local (parent frame); the gizmo converts world deltas
    with the parent's world rotation. Bones listed in two display frames select keys through `CanonicalRow`.
  - `asset/ModelImport.h` and `render/GpuModel.h` both declare `mmdx::ModelRole`: never include both in one .cpp.
  - Camera/light/self-shadow (`app/UiStudioCamera.cpp`): `StudioDoc::camera` holds the camera, light (`LightKf`, linear) and
    self-shadow (`ShadowKf`, stepped, VMD distance = 0.1 - UI*1e-5) tracks as timeline rows `RowKind::Camera/Light/Shadow`.
    The lighting source is `StudioDoc::lighting` (`studio/LightRig.h`, a 3-way choice saved in the project, timeline rows
    `RowKind::Spot` key the rig's spots): VMD track (the camera VMD's light keys override the preset's key light, empty
    falls back), Preset (the project's own preset choice + a key-light override) or Custom (the concert spot rig, undoable
    add/delete, auto swing / follow centre / head / manual aim modes, warm front fill). `BuildStudioLighting` (app/Lighting.cpp)
    resolves the rig into `FrameView::light` for the studio (viewport, offline stills/videos). Play mode and the lobby keep
    the global preset (App.cpp `BuildFrameView`); the benchmark stays Studio/ deterministic. In the studio the shadow track
    sets `FrameView::shadowsOff`/`shadowDistance`; play mode ignores VMD light/shadow unless `settings_.motionLighting`.
    Perspective-off keys render as a 3 degree lens from far away.
  - DoF focus (`studio/StudioFocus.*`, `app/UiStudioFocus.cpp`): the camera target's `RowKind::Focus` row = `MotionData::focus`
    (`FocusKf`, stepped segments: Auto / Target model uid + bone / Manual distance linear to a following Manual key; per-key
    aperture scale (linear, 0 = sharp) and `transition` frames = a smoothstep rack in 1/z). Saved in the project's editor
    "focus" array (targets as 1 + model index), never in VMD. No keys = `AutoFocus`: visible characters scored by on-screen
    height^1.5 x visible share x head centrality, 0.7 hysteresis, 0.35 s rack, snaps on camera cuts / time jumps (stateful:
    the viewport and renders share `studioAutoFocus_`). `StudioComputeFocus` writes `FrameView::focusDistance` and
    `apertureScale` (DofPass and the GI lens multiply it in). Play mode runs its own `AutoFocus` (`playAutoFocus_`, reset
    per scene) over every performer, falling back to the center performer's head; the benchmarks keep the fixed head focus.
  - Projects (`studio/StudioProject.*`, `app/UiStudioProject.cpp`): `.mmdxproj` is UTF-8 JSON (format version, relative
    paths) next to standard VMDs (`<stem> - <model>.vmd`, `<stem> - camera.vmd`); every file is written tmp + rename.
    Dirty = `history.Version()` or `projectVersion` (add/remove/rename/visibility/audio) changed since the save. Autosave
    every 60 s on a worker thread to `<exe>/recovery/` (also on window close with unsaved work, not in `--frames`/script
    runs); the select screen offers it at the next start; saving or leaving the studio deletes it.
  - Models are added inside the studio on worker threads (`StudioJob`, `LoadStudioModel/Stage/Song`), uploaded in
    `StudioPollJobs`. `StudioModel::uid` is stable; removal waits for the GPU and clears the undo history (commands hold
    model indices). Replacing a model's file (outliner menu "모델 바꾸기", the "+" popup in replace mode: `StudioJob::replaceUid`,
    `App::StudioReplaceModel`) keeps uid, motion keys (re-canonicalised by name), placement, prop links, shader and the
    pose layer (remapped by name); it also clears the undo history (pose edits hold bone indices). Kinds: character, stage (static BLAS), prop (GPU role Character so its BLAS follows it): the prop's
    root = `PropOffsetMatrix(attach) * parent bone world * parent scale`, set with `ModelInstance::SetRootTransform`
    after the other models are posed (props parent to characters/stages only).
  - Rendering (`app/UiStudioRender.cpp`): the top bar's render menu opens the lobby's video dialog (`DrawVideoRenderDialog`
    is studio-aware: range section, no sample-render probe) or starts a GI still. `OfflineJob::studio` routes the offline
    job through `StudioPoseForRender` / `BuildStudioFrameView` (`OfflinePose`/`OfflineView` in UiOffline.cpp); the
    viewport rect is cleared for the render and `StudioRestoreAfterRender` puts time, camera mode and physics back.
    Videos skip unregistered pose edits. Audio: `VideoEncoder::Desc::audioStartSeconds` = start - audioOffset (negative:
    leading silence). Shortcut overlay: `DrawStudioHelp` (? / F1); `StudioModal()` blocks the studio's shortcuts.
  - UI restraint (the competitor-comparison pass, 2026-10): new actions go into existing places, not new toolbars.
    Timeline right click = key menu (`DrawStudioKeyMenu`: add / copy / cut / paste here / mirrored paste
    `StudioPaste(curves, mirrored)` / curves / delete); transport: volume icon + popover, one-frame steppers around
    the frame field; screen effects are a dock tab next to the properties (`###studio_effects`,
    `DrawStudioEffectsPanel`; the top-bar sparkle focuses it); camera-target rows carry a type tint
    (`TimelineRow::tint`); drag fields type on a plain click (`io.ConfigDragClickToInputText`).
  - Panels (`App::DrawStudio`): ImGui docking (the vendored imgui is the docking branch). A fixed top-bar window, a transparent
    dock-space host and one dockable window per panel (`###studio_outliner/inspector/timeline/viewport`); each panel function
    still draws in screen coordinates and gets its window's content rect. The viewport window is transparent and its content
    rect is `RenderSettings::viewport*`. Default arrangement = `BuildDefaultStudioDock` (DockBuilder); the layout persists in
    `mmdx12_layout.ini` next to the exe, except in scripted / `--frames` runs (default layout every time; tab bars add ~32 px
    to the panels, so old `--ui-script` coordinates shifted). Panels float / dock inside the main window only (no OS-level
    viewports: one swap chain).
  - Editing model: auto-key (`StudioDoc::autoKey`, transport bar button): a finished pose / morph / camera edit keys itself at the
    playhead (`StudioSetPose` -> `StudioRegisterPose(…, layerBefore)`; camera fields live in `DrawStudioCameraKeyFields(live)`).
    Camera possession (`StudioPossess`, C4D style): viewport navigation edits the motion camera key (`StudioWriteCamera`, one
    undo step per gesture via `StudioNavEditTick`); without it the free view shows eye / target handles
    (`StudioViewportCameraHandles`). Characters have a world placement (`StudioModel::place`, a `PropAttach` used as
    translation / rotationDeg / scale; applied as root transform + display scale in `StudioUpdateModel`).
  - Viewports: `StudioViewportNavigate` (RMB look + WASDQE fly, Alt+LMB orbit, MMB pan, F focus, numpad views). Shading
    (`ViewShading`: Lit / Unlit / Wireframe, raster only) and the quad view (`StudioDoc::viewLayout`): the renderer draws
    `FrameView::extraViews` (orthographic top / front / left, `ExtraView`) as extra viewports of the same target inside
    `ScenePass::Execute`, each with its own SceneConstants (`PassContext::extraSceneConstants`); quad forces flat shading, no
    TAA / upscaler. `ViewProj::ortho` makes the overlays / gizmos work in those views; the active view is the one under the
    mouse (`studioActiveView_`).
- Shader packs (an ecosystem: anyone can make one; docs + gallery on mmdx.codingbot.kr `/<lang>/docs/shader-packs/`, sources
  in the website repo): opt-in per-character replacements of the scene pass's material shading. A pack = `pack.json`
  (metadata, localized names, authors, license, tags, `apiVersion`, material class rules, <= 16 params) + `surface.hlsl`
  implementing `PackShade` (contract: `shaders/pack_api.hlsli`, `kPackApiVersion`) + optional `preview.png|jpg` (AFTER) and
  `preview_before.png|jpg` (same frame without the pack; index.json `previewBefore`); with both, the manager's detail panes
  draw a draggable split (`App::DrawPackCompare`, Drag mode, uiprobe id `compare:<id>`; cards scrub on hover). Built-in in
  `shaders/packs/<id>`, installed in `<exe>/shader_packs/<id>`; template `shaders/pack_template`; `tools/pack_check`.
  Registry `render/ShaderPack.*` (status per pack, hot reload `PollChanges`, install / uninstall / template); online gallery
  `app/ShaderPackStore.*` (index.json, SHA-256 checked zips); manager screen `app/UiShaders.cpp` (Screen::Shaders, top tab
  "셰이더", `--screen shaders|shaders-online`; per-tab type filter + sort `shaderType=` / `shaderSort=` ini, index.json
  `type` / `updated`), shared picker `app/UiShaderPack.cpp`. Networking / zip / version helpers live
  in `core/NetUtil.*` (shared with the updater).
  `GpuModel::SetShaderPack` writes class / head bone / params into `MaterialConstants`; `ScenePass::PackPsos` compiles
  mmd.hlsl's `PSPack` with DXC (FXC cannot `#include` a macro: `MMDX_PACK` = the quoted surface path) on first use and
  falls back to the default PSOs on errors (`[E]` + toast). `surface.hlsl` runs in the lit raster / RT camera view; PT and offline GI use the pack's
  `pt_surface.hlsl` when it has one (see below), else the default; unlit / wire / ortho views always use the default. A negative normal-target reflectivity (`nt.z < 0`) = "no AO" (composite). Choice is saved per
  character (`characterShader=` ini) / per studio model (`.mmdxproj` "shader"); `--shader-pack <id|none>` overrides a run.
  Per-material overrides (`ShaderChoice::materials`, material name -> PackClass 0..5 or -1 = default shading; ini
  `characterShaderMaterial=<cls>|<character id>|<material>` (name last: names may hold '|'), `.mmdxproj` "shader"."materials"): `GpuModel::SetShaderPack`
  writes the class, or `kPackClassOff` (0xFF) = that material takes the no-pack path everywhere (raster PSO / edge,
  RT, PT, offline GI; pack code never sees 0xFF). UI: `App::DrawMaterialShaderList` (studio inspector "재질" popup,
  play bar shader popup, library shader tab via `LoadCharacterModelOnly`).
  API v6 (`kPackApiVersion` 6; apiVersion 1..5 packs still load): `PackSurface` / `PtPackIn` carry the PMX additional
  UV1/UV2 (zero without them, feature macro `PACK_API_ADDUV` / `PT_PACK_API_ADDUV`), the raw sphere-slot (`_H`) and
  toon-slot (`_S`) textures are exposed (`PackHasSphereTex` / `PackSampleSphereUv`, `PackHasToonMap` / `PackSampleToonUv`,
  PT mirrors; a real sphere texture bound = `MAT_HAS_SPHERE_TEX`), and `PackEdge` sees the vertex's uv2 (`PackEdgeUv2`).
  Online-gallery-only `project_sekai_toon` ("프로젝트 세카이 툰", sources in the website repo `shader-packs/`, not bundled;
  `tools/pack_previews.py` has its shot): the Project Sekai MME look (hard terminator with the `_H` shadow bias, the `_S`
  shadow colour, the skin ramp, the uv1 face SDF and uv2 masks) from the model's own data only; `asset/MmeHints` follows the
  model's own `.emd` / `.fx` references (same-stem .emd, else the folder's only .emd; never folder / file-name guesses) to
  find its face SDF and set a per-model default pack texture folder. Packs declare the model-data traits they fit in an
  optional pack.json `"suggest"` block (`additionalUvsMin` / `toonMapMaterialsMin`; `ShaderPack::Suggests`), evaluated
  generically over `ModelTraits` (`ComputeModelTraits`: additional UV count + non-ramp UV-mapped toon-slot materials;
  `GpuModel::Traits()`), checked over the whole library by `tools/pack_suggest_test` (self test + scan). The app only
  SUGGESTS such a pack: one toast per (pack id, character id) when a character with no saved shader choice matches the
  first suggesting installed pack (remembered as `characterShaderSuggested` ini lines), a "추천" tag in the pack pickers,
  and the choice is never set by the app. Users fill their library their own way: nothing may depend on its layout or names.
  API v2 (`kPackApiVersion` 2; apiVersion 1 packs still load): a pack may declare `"textures"` in pack.json (at most 16:
  `{ "file": "textures/x.png", "address": "wrap"|"clamp", "srgb": true }`, png/jpg/jpeg, paths inside the pack), uploaded
  once per pack (DEFAULT heap, `ScenePass::EnsurePackTextures`) and shared by every model using it; sampled with
  `PackSampleTex` / `PackSampleTexLevel` / `PackTexSize` / `PackTexCount` (pack_api.hlsli, a fixed 16-SRV table at
  `t0, space5`, bound as root param 12). sRGB textures use `_SRGB` SRVs and sample as LINEAR values (no SrgbToLinear in
  the pack); `"srgb": false` returns stored values (data maps). Missing textures sample white (`[W]` per load; the
  manager shows the count). Game textures can't be redistributed: each pack gets a user "texture folder" (shader manager
  screen, picker + clear, saved as `packTextureFolder=<id>|<path>` ini lines, registry `TextureFolder`); lookup order:
  user folder (same relative path, then the same file name, then a file ending in `_<declared name>`, case-insensitive,
  shortest wins) → pack folder → white. The folder is per character too (`ShaderChoice::textureFolder`: ini
  `characterShaderTextures=<folder>|<character id>`, `.mmdxproj` "shader"."textureFolder", the "텍스처 폴더" row under the
  pack's sliders); it wins over the pack-level folder, and ScenePass keeps one texture set per (pack id, folder).
  Class rules can match the material's diffuse texture path (`"texture": [...]`), not only its names: game texture
  sets follow the texture sheet (hoyo_toon_v2 picks hair maps for anything on the hair sheet). A class rule may carry
  `"alphaKey": <0..1>` (colour-key cut-out: cut out pixels whose texture red is below the value, an additive-key
  sprite such as an MME eye highlight; applies in every render path; the first rule of a class wins). A pack with all textures missing
  still compiles and renders. `PACK_HAS_EDGE` + `PackEdge` in surface.hlsl wire the pack into the edge pass (per-material
  outline colour + width scale; `PSEdgePack` / a `VSEdge` width-scale build from `PackPsos`, root param 12 bound there
  too); otherwise the edge pass is unchanged. PT and offline GI draw the same pack outlines: PT through `ScenePass::DrawPtEdges`
  (below), offline GI through `offline_edge_pack.hlsl` (`VSEdgeOfflinePack` / `PSEdgeOfflinePack`, mmd.hlsl included with
  `MMDX_NO_SHADOW_PASS` because its `ShadowCB` is b2; one DXC PSO per pack in `OfflineRenderer::Impl::PreparePackEdges`). `PACK_WEAPON` = 5 joins the material classes; built-in `hoyo_toon_v2` (issue #2) = Genshin light maps / ramps / face SDF
  from a user texture folder, v1 fallback per missing map; `tools/pack_check` validates
  textures (format / count / size / total ≤ 32 MB / bad paths; missing files are warnings) and literal
  `PackSampleTex(N)` indices against the declared count.
  `pt_surface.hlsl` (docs/shader_pt_api.md, phases 1-3): optional `PackEvaluate(PtPackIn)` hook for offline GI (`CSRender`, `OfflineRenderer`) and real-time PT (`CSPathTrace`, `PathTracePass`) with pack textures supported (`PtPackSampleTex*`, `render/PackTextures.*`); packs without it leave PT/GI untouched; shared variant cache `render/PtPackVariants.*`. `PtPackIn` also carries `headPos`/`headScale`; `PtPackOut::terminator` = N.L edges of the ramp (hi <= lo: engine default), `shadowBias` = smoothed-normal offset, and every field must be assigned (`pack_check` warns). Online-gallery-only `nimble_toon` (sources in the website repo `shader-packs/`; Eternal Return MMD models) = soft cel look (low-contrast coloured shadows, lifted dark-material shadow floor, skull-sphere face normals, soft hair gloss, wide environment rim, tinted outlines via `PackEdge`); `surface.hlsl` (raster / RT) and `pt_surface.hlsl` (PT / GI) share `nimble_core.hlsli`. `PtPackOut::flatFace` alone decides the
  flat-face path for pack surfaces (a material without toon, `MAT_FLAT`, no longer drops the pack's terminator in PT / GI). Opt-in `PT_PACK_HAS_LIGHTING` / `PackEvaluateLighting` (API macro `PT_PACK_API_LIGHTING`) splits that: `flatIndirect` gathers indirect flat without dropping the pack's terminator, `sunShadow` (0..1) weights the traced sun visibility in the direct term / specular, and `lit` (0..1, -1 = unused) replaces the terminator / shadowBias; packs without it keep the old behaviour exactly.
  Online-gallery-only `starrail_face` = Star Rail: game light maps, warm / cool ramps
  read with the Unity row flip `1 - (2 id + 1) / 16`, body-type `*_FaceMap_00` SDF in A + nose line in B; the "weapon"
  class = a model's second sheet; test texture sets in `library/gamerip/StarRailTextures/_sets`; hoyo_toon_v2 stays Genshin-only.
  API v3 effect packs (docs/shader_effect_api.md): `"type": "effect"`, `"stage": "post" | "pre-bloom"`, `effect.hlsl` implements
  `PackEffect(PackEffectInput)` (contract `shaders/effect_api.hlsli`, host `shaders/effect.hlsl`, DXC `ps_6_0` with `MMDX_PACK`).
  The user's ordered stack is `RenderSettings::packEffects` (`EffectStackEntry`; `AppSettings::effectStack`, ini `effect=`,
  CLI `--effect id[,id]|none`). `PackEffectPass` (`PassEffect.cpp`) is registered twice (pre-bloom after DoF, post after Post), runs
  its share with ping-pong targets created only while an entry is enabled (zero effects = no targets, identical frame), skips
  unlit / wire / quad views, and ends with a CopyResource into `hdrFinal` / `ldr`. Pack textures use the v2 table (`PackFxSampleTex`,
  t0 space5), frame inputs are t0..t3 space6, params 16 root constants at b2. UI: `App::DrawEffectStackEditor` (shader manager detail
  of an effect pack + the play bar's effect button); surface pickers list surface packs only. Templates `shaders/pack_template`
  (surface) / `pack_template_effect`; online-gallery-only examples chromatic_aberration, film_grain, crt_scanlines, auto_luminous (not bundled; sources in the website
  repo). Studio projects don't store the stack yet (app-level setting). API v4 effect state (docs/shader_effect_api.md): pack.json `"state": {"floats": N}` (<= 16, apiVersion 4) gives each stack entry a 4x1 RGBA32F ping-pong slot (`PackEffectPass::stateSlots_`, key = stack index + pack id) updated by `PackEffectState` in a pre-pass before `PackEffect` (`PackState(i)`, `PackEffectInput::dt`); state packs use their own root signature (`t0, space7`), packs without state keep the v3 PSO / defines. dt / reset / advance come from `FrameView::effectDt / effectStateReset / effectStateAdvance` and `OfflineJobDesc::effectDt / still / effectReset` (GI stills run the state pass 64x). Offline GI runs the stack too (`job.effects`, `OfflineRenderer::Impl::Finish`):
  pre-bloom entries run on the lit HDR before the bloom (`CSLitCompose`, the bloom then reads the effect output), post entries on the sRGB
  result (`PackEffectPass::RunOffline`). Their depth / normal come from the offline G-buffer (depth from `offline_effect.hlsl`, the normal in view
  space like the real-time normal target), motion is zero.
  API v5 (docs/shader_effect_api.md): pack.json `"passes"` (<= 8 `{entry, scale}`; `PSPackPass` with `PACK_PASS_ENTRY`,
  RGBA16F targets pooled by size in `PackEffectPass::passPool_`, read as `PackPassSample(n, uv)` from a t0..t7 space8 table
  that exists only in packs with passes) and the focus helpers (`PackFocusZ` / `PackFocusAperture` / `PackDofAperture` /
  `PackCocPx`, gP1 = FrameView focus + the DoF settings). `"replaces": ["dof"]` (`EffectStackReplacesDof`) skips DofPass and
  the GI thin lens while that entry is enabled. One entry's draws = `PackEffectPass::RunEntry` (shared by RunStack /
  RunOffline). Gallery examples on it: lens_dirt, lens_reflection, lens_flare, bokeh_dof, screen_ao (and
  chromatic_aberration 2.0.0, still v3). MMD scenes rarely hold HDR lights (a lit window is ~0.4 linear): the lens packs
  pick "lights" by luminance x local contrast, not a plain threshold.
- MCP (docs/mcp.md): `tools/mmdx12_mcp.cpp` is the stdio MCP bridge; it forwards tool calls over the named pipe
  `\\.\pipe\mmdx12_mcp[_<pid>]` to `McpServer` (app/McpServer.*, worker thread), which queues them for the main thread:
  `App::PumpMcp` / `ExecuteMcp` (app/AppMcp.cpp) before ImGui::NewFrame. Tool schemas + timeouts live in app/McpTools.h
  (shared by both sides). Navigation goes through `McpLeaveScreen` (the UI's own leave paths); multi-frame calls
  (loads, wait_frames, ui_input) resolve later from PumpMcp; `ui_input` / `studio_command` reuse `ExecuteUiCommand`
  (UiScript.cpp) with their own queue counted in `mcpFrame_`.
  Widget registry (`src/ui_probe/UiProbe.*`, compiled into the imgui target with `IMGUI_ENABLE_TEST_ENGINE`, on while the
  MCP server runs): imgui's test-engine hooks record every item of the last frame (rect, window, label = text before
  "##", id string); UiKit widgets report their id + visible text (`uiprobe::Info`), `CardItem(..., label)` names cards,
  drawn-only elements register with `uiprobe::Add` (timeline `##tlrow` / `##tlkey:<row>:<frame>`). MCP `ui_items` /
  `ui_click` match label or id (Korean source text also through `Tr()`, so one script serves every UI language) and skip
  clipped / covered items (`uiprobe::Covered`: topmost window under the centre). `screenshot` takes `path` / `region`.
  The website's user guide screenshots come from `MMDX12_Web/scripts/guide` (capture.py drives a packaged copy outside
  the repo, e.g. `E:\MMDX12`, per language; annotate.py -> public/guide/<lang>/*.webp). New UI: give clickable widgets
  stable ids so the guide script keeps working. Renders use `mcpOfflineOutput_`, never `options_.offline*`
  paths (those make a CLI job that quits). On by default (`mcp=` ini, lobby detail switch), headless runs need `--mcp`.
- `OfflineRenderer` (render/OfflineRenderer.h) is independent of `RenderSettings`: `Renderer::BeginOffline` builds the TLAS,
  then `Renderer::RenderOffline` replaces `Render` each frame (GPU-time-budgeted iterations, preview present) until Done.
  Motion blur: each iteration re-skins the character at its shutter time (`RtScene::Build(..., time)`) from the models'
  previous ring entry (the shutter-open pose; for videos the App re-uploads the previous video frame's pose there) and
  interpolates the camera (`SceneConstants::prevInvView`, offline_common.hlsli).
  GI: the prepass builds a screen-space irradiance cache (6 adaptive levels, 512 gather paths per sample, smoothed);
  `CSRender` takes indirect diffuse at camera hits from it via `IcLookup` (projects the hit into the frame camera,
  so lens/shutter samples work) and falls back to brute force where it has no matching surface.
  Note `--autoplay` and `--seek` are consumed after loading (App.cpp), so CLI triggers must not depend on them.
- Headers are the module contracts. Keep `App.h`, `Renderer.h` etc. authoritative when extending.

## Conventions / pitfalls
- Coordinates: native MMD space (left-handed, +Y up), which equals D3D. Never flip axes.
- Math is DirectXMath row-vector (`v * M`). HLSL uses `pack_matrix(row_major)` + `mul(v, M)`.
  - FXC ignores `pack_matrix` for StructuredBuffer elements. Wrap the type in a struct with `row_major` (see `BoneMatrix` in mmd.hlsl).
- Quaternions: `XMQuaternionMultiply(a, b)` means "a, then b" (it equals glm `b * a`).
- VMD interpolation byte layouts are documented in `src/asset/VmdMotion.h`.
  Some camera VMDs use distance 0 (the target is the eye position). `CameraMotion::ToView` handles this.
- Per-frame GPU buffers must live in the UPLOAD heap and stay persistently mapped. `UploadBatch` is for DEFAULT-heap static data only.
- UI text is Korean. Fonts are Pretendard + Phosphor icons (assets/fonts, copied to bin by `copy_assets`), with YuGoth/msyh merged for JP/CN. Pretendard's Private Use Area is excluded so Phosphor glyphs win.
- Runtime shader compile errors only appear in `build/bin/mmdx12.log` as `[E]` lines (warnings `[W]`); a failed optional pipeline silently falls back, so grep the log after every capture. `line` is a reserved word in HLSL (DXC rejects it as a variable name).
- Shaders under `build/bin/shaders` compile at runtime, so a shader can be debugged by editing that copy and rerunning (restore it afterwards).
- `library/` (MMD assets, about 1.4 GB) and `captures/` are git-ignored test data. Never modify `library/`.
- `external/` is vendored third-party code (imgui, stb, miniaudio, DirectX-Headers, nlohmann json, Bullet 3.25 subset, cgltf 1.15, ufbx 0.23.1). Don't edit it. imgui is the `docking` branch (same version number as master 1.93 WIP).
  - miniaudio's implementation lives in `core/AudioProbe.cpp` (the scanner measures audio lengths).
  - Bullet is built per file, not from its `*All.cpp` unity files (`btVector3.cpp` defines `BT_USE_SSE_IN_API`, which breaks later files).

## Private notes
- Server and asset-source details live in `CLAUDE.local.md` (git-ignored). Never commit server paths, hostnames or local user paths.

## Session close ("세션 마무리")
1. Commit everything so the working tree is clean.
2. Append a dated summary of the session to progress.md, and include it in the commit.
