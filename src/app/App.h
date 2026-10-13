#pragma once
#include <atomic>
#include <functional>
#include "anim/ModelInstance.h"
#include "anim/Motion.h"
#include "app/Benchmark.h"
#include "app/Lighting.h"
#include "app/McpServer.h"
#include "app/RenderBench.h"
#include "app/LeaderboardClient.h"
#include "app/SceneLoader.h"
#include "app/Settings.h"
#include "app/ShaderPackStore.h"
#include "app/ThumbnailCache.h"
#include "app/Updater.h"
#include "app/VideoEncoder.h"
#include "asset/AssetLibrary.h"
#include "asset/MaterialChoice.h"
#include "asset/RayMaterial.h"
#include "audio/AudioPlayer.h"
#include "render/ColorLut.h"
#include "render/Dx12Context.h"
#include "render/Renderer.h"
#include "studio/AutoIk.h"
#include "studio/Gizmo.h"
#include "studio/PhysicsBake.h"
#include "studio/StudioDoc.h"
#include "studio/StudioFocus.h"
#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <set>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mmdx {

// Command line (all optional). Used for normal launches and for automated verification.
//   --library <dir>        override library folder for this run
//   --character <substr>   preselect: first character whose id or displayName contains substr (case-insensitive)
//   --stage <substr>       preselect stage ("none" = no stage)
//   --song <substr>        preselect song
//   --autoplay             after the scan, start playing the preselection immediately
//   --seek <seconds>       start position for --autoplay
//   --benchmark <catId>    after the scan, start the benchmark run for that category immediately
//   --bench-frames <n>     override kBenchMeasuredFrames (testing)
//   --bench-spp <n>        render benchmark: override kRenderBenchSamples (testing; the result cannot be submitted)
//   --frames <n>           quit after n frames have been rendered in Play/BenchmarkRun state
//   --capture <file.png>   capture the final frame (with --frames) to a PNG
//   --ui-script <file>     scripted ImGui input for UI tests (UiScript.cpp): lines "<frame> <command> [args]",
//                          frame = frames counted like --frames; commands: move x y | down [l|r|m] | up [l|r|m] |
//                          click x y | dblclick x y | wheel dy | key <ImGuiKey name> [ctrl] [shift] | text <chars> |
//                          mod <ctrl|shift> <down|up> (held across mouse steps) |
//                          capture <file.png> | log <text> |
//                          studiostate (logs the Studio's frame, selection, rows, range, key counts, undo) |
//                          studioexport <file.vmd> | studioimport <file.vmd> (the Studio's VMD export/import
//                          without the dialogs) | studiovpdexport/studiovpdimport <file.vpd> (pose files, current
//                          scope) | studiobone <name> (clicks that bone's joint in the viewport) |
//                          studiogizmo <x|y|z|yz|zx|xy|rx|ry|rz> <dx> <dy> [steps] (drags that gizmo part by dx,dy px).
//                          studiostate also logs a STUDIOPOSE line (active bone + value, pose layer, tool) and a
//                          STUDIOPROJ line + one STUDIOMODEL line per model (kind, keys, attach, root position).
//                          updatecheck (synchronous update-feed check, logs UPDATECHECK) |
//                          updateinstall (stage the feed's update; the app exits when staged)
//                          Projects / models without dialogs (UiStudioProject.cpp): studionew | studiosave <file> |
//                          studioopen <file> | studioautosave (writes the recovery file now) | studioadd
//                          <character|stage|prop> <file> | studioaddlib <character|stage> <substr> | studiosong <substr>
//                          (library song onto the selected model + camera + audio) | studioaudio <file|none> [offset s] |
//                          studioreplace <index> <file> | studioreplacelib <index> <substr> (swap a model's file) |
//                          studioselect <model index|-1> | studioremove <model index> | studiorename <text> |
//                          studiomaterial <default|ray> [emd path] (selected studio model's PBR material source) |
//                          studioattach <parent index|-1> <bone|-> <tx ty tz rx ry rz s> (selected prop).
//                          drop <file>[|<file>...] injects a drag-and-drop path list (models, .vmd/.vpd, audio,
//                          .mmdxproj) into the Studio; '|' separates paths so they may contain spaces.
//                          Coordinates in window pixels.
//   --width <w> --height <h>  initial window client size
//   --debug                enable the D3D12 debug layer
//   --free-camera          start with the free orbit camera
//   --screen <select|stages|songs|settings|video|bench|bench-gi|studio> open that screen after the scan (UI testing;
//                          --frames counts all frames; studio: opens --character/--stage/--song in the Studio at
//                          --seek and counts frames from the Studio on; without --character: a new empty project)
//   --project <file.mmdxproj>  open that Studio project after the scan (implies --screen studio)
//   --lighting <0..3>      lighting preset for this run (Studio, Sunset, Concert, Night)
//   --quality <0..3>       graphics preset for this run (low, medium, high, ultra)
//   --render <raster|rt|pt>            render path for this run (override settings)
//   --upscaler <none|dlss|fsr|xess>    upscaler for this run (override settings)
//   --upscale-quality <native|quality|balanced|performance|ultra>  upscaler quality for this run
//   --no-physics           disable rigid-body physics for this run
//   --dof <0|1>  --volumetric <0|1>  --bloom-conv <0|1>   post effects for this run
//   --auto-luminous <0|1>  そぼろ AutoLuminous glow convention for this run
//   --volumetric-density <0.25..4>  volumetric medium density for this run
//   --lut <substr|none>    colour LUT for this run (first LUT whose id or name contains substr)
//   --offline-still <file.png>   with --autoplay: render the --seek frame with the offline GI renderer,
//                                save it to the file and quit
//   --offline-video <file.mp4>   with --autoplay: offline video render, then quit
//   --offline-range <a> <b>      video range in song seconds (default: the whole song)
//   --offline-spp <n>            testing: cap samples per pixel (min samples = min(min, n))
//   --offline-size <w> <h>       testing: output size override for stills and videos
//   --offline-fps <n>            video frame rate for this run (24, 30, 60; default: the lobby dialog's)
//   --offline-bitrate <mbps>     video bit rate for this run
//   --offline-quality <0..3>     video quality preset for this run (draft, standard, high, best)
//   --offline-renderer <r>       video renderer for this run: raster, rt, pt or gi (default: the lobby dialog's)
//   --offline-alpha 1            transparent-background export: --offline-video writes a straight-alpha RGBA8 PNG
//                                sequence (<output stem>_frames/<stem>_00000.png), --offline-still one RGBA PNG
//   --offline-probe              with --autoplay: sample render from the middle of the song (the lobby dialog's
//                                time measurement) with the dialog's settings, log "VIDEO PROBE", then quit
//                                Studio (--project / --screen studio): --offline-video renders the project once it is
//                                open (range: --offline-range in timeline seconds, else the timeline range, else the
//                                whole project; motion camera; project audio) and quits; --offline-still renders the
//                                --seek frame (GI).
//   --theme <auto|light|dark>    colour theme for this run instead of the saved one (auto = follow Windows)
//   --material-source <default|ray>  per-material PBR data source (work unit P2); default renders as before and
//                                never reads Ray .md/.fx data
//   --pbr-debug <ch>             with --material-source ray: output one PBR channel (none|albedo|normal|roughness|
//                                metalness|ao|emissive|model) in the lit raster view
//   --classic                    vanilla-MMD look for this run: SSAO, SSR, bloom, volumetric, DoF, LUT, effects and
//                                soft shadows off (the plain toon key light stays); never saved to mmdx12.ini
//   --ui-scale <f>              UI scale for this run instead of the monitor's DPI scale (1.5 = 144 dpi; captures)
//   --update-feed <url-or-file>  auto-update feed override: an https URL, a file: URL or a local
//                                latest.json path (testing); the zip url may also be file:/local
//   --update-relaunch-frames <n> with --update-feed: relaunch the updated app with --frames <n>
//                                (headless E2E; the restarted version quits on its own)
//   --apply-update               internal: apply a staged update with no window, then exit
//   --apply-wait <pid>           internal: process id the applier waits for before the swap
//   --component <id>             optional component (app/Components.h) to act on
//   --download-component         with --component: fetch it now with no window, then exit (headless E2E)
struct AppOptions {
    std::filesystem::path libraryOverride;
    std::string character, stage, song;
    bool autoplay = false;
    double seekSeconds = 0;
    std::string benchmarkCategory;
    int benchFrames = 0;
    int benchSpp = 0;          // --bench-spp (0 = kRenderBenchSamples)
    int quitAfterFrames = 0;
    std::filesystem::path capturePath;
    bool captureSceneOnly = false;   // --capture-scene 1: the capture is the rendered scene without UI
    std::filesystem::path uiScript;
    int width = 0, height = 0;
    bool debugLayer = false;
    bool freeCamera = false;
    float camera[6] = {};        // --camera tx,ty,tz,yawDeg,pitchDeg,dist (implies free camera)
    bool hasCamera = false;
    bool paused = false;        // --paused: start --autoplay paused (overlay captures)
    int lighting = -1;         // --lighting: override settings for this run
    int quality = -1;          // --quality: override settings for this run
    int renderPath = -1;       // --render <raster|rt|pt>: override settings for this run
    int shading = -1;          // --shading <lit|unlit|wire>: raster ViewShading override for this run (hidden)
    int upscaler = -1;         // --upscaler <none|dlss|fsr|xess>: override settings for this run
    int upscalerQuality = -1;  // --upscale-quality <native|quality|balanced|performance|ultra>: override settings for this run
    bool noPhysics = false;    // --no-physics: override settings for this run
    std::string shaderPack;    // --shader-pack <id|none>: the character's shader pack for this run (play and studio)
    bool shaderPackSet = false;
    int materialSource = 0;    // --material-source <default|ray>: per-material PBR source for this run (0 default, 1 ray)
    bool materialSourceSet = false;   // --material-source was given: it wins over the saved per-character choices
    int pbrDebug = 0;          // --pbr-debug <none|albedo|...>: PBR debug channel for this run (0 none .. 7 model)
    std::string packIndex;     // --pack-index <url|path>: the online shader pack gallery index (testing)
    std::vector<EffectStackEntry> effectStack;  // --effect <id>[,<id>...] / none: the effect stack, this run
    bool effectSet = false;
    int dof = -1, volumetric = -1, bloomConv = -1;  // --dof/--volumetric/--bloom-conv <0|1>: override for this run
    int autoLuminous = -1;     // --auto-luminous <0|1>: override for this run (-1 = settings)
    bool classic = false;      // --classic: vanilla-MMD look for this run (SSAO/SSR/bloom/volumetric/DoF/LUT/effects/soft shadows off)
    int motionLighting = -1;   // --motion-lighting <0|1>: camera VMD light/self-shadow tracks in play mode, this run
    float volumetricDensity = -1.0f;                 // --volumetric-density (< 0 = settings)
    std::string lut;           // --lut <substr|none>: override for this run (resolved against the LUT list)
    std::filesystem::path offlineStill, offlineVideo;  // --offline-still / --offline-video
    double offlineRange[2] = {-1.0, -1.0};             // --offline-range (seconds; < 0 = unset)
    int offlineSpp = 0;                                // --offline-spp (0 = default)
    int offlineSize[2] = {0, 0};                       // --offline-size (0 = default)
    int offlineFps = 0, offlineBitrate = 0;            // --offline-fps / --offline-bitrate (0 = settings)
    int offlineQuality = -1;                           // --offline-quality (-1 = settings)
    int offlineRenderer = -1;                          // --offline-renderer <raster|rt|pt|gi> (-1 = settings)
    int offlineAlpha = -1;                             // --offline-alpha <0|1>: transparent PNG sequence for --offline-video/still
    bool offlineProbe = false;                         // --offline-probe: sample render for the time estimate, then quit
    int theme = -1;     // --theme auto|light|dark for this run only (-1: keep the saved setting; 0 auto, 1 light, 2 dark)
    int language = -1;  // --lang auto|ko|en|ja|zh for this run only (-1: keep the saved setting)
    std::filesystem::path project;  // --project
    float uiScale = 0.0f;           // --ui-scale (0 = the window's DPI scale)
    std::string formation;          // --formation <spec>
    std::string startScreen;  // --screen select|bench: open that screen after the scan; --frames then counts every frame  // --free-camera: start in the orbit camera instead of the VMD camera
    std::string updateFeed;   // --update-feed: feed URL / file override (empty: the default feed)
    int updateRelaunchFrames = 0;  // --update-relaunch-frames: relaunch the updated app with --frames (E2E)
    bool applyUpdate = false; // --apply-update (internal): apply a staged update, then exit
    uint32_t applyWaitPid = 0;        // --apply-wait (internal): pid the applier waits for
    std::string componentId;   // --component <id>: optional component (Components.h) to act on
    bool componentDownload = false;  // --download-component: fetch --component now, then exit (headless E2E)
    int mcp = -1;                     // --mcp (1), --no-mcp (0), auto (-1)
};
AppOptions ParseCommandLine(int argc, wchar_t** argv);  // unknown args are logged and ignored

// Folder picker for a pack's user texture folder (FOS_PICKFOLDERS): runs on its own STA thread,
// keeping the owner's messages pumped. Defined in UiShaderPack.cpp, also used by UiShaders.cpp.
std::filesystem::path PickPackTextureFolder(HWND owner);

// Popup width for the pack parameter grid (columns from the pack's param count), clamped to the viewport.
// Defined in UiShaderPack.cpp, also used by UiPlay.cpp.
float PackParamsPopupWidth(const ShaderChoice& choice);

class App {
public:
    int Run(HINSTANCE instance, const AppOptions& options);  // returns process exit code

    // Window procedure trampoline (public so the static WndProc can call it).
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    enum class Screen { Scanning, Select, Loading, Play, BenchLobby, BenchRun, BenchResult, Offline, BenchRender, Studio, Shaders };
    enum class LoadTarget { Play, Benchmark, RenderBench, OfflineVideo, Studio };

    struct SceneRuntime {
        std::unique_ptr<ModelInstance> character;
        std::string characterId;  // library id, key of the saved display scale
        std::string characterDisplayName;
        std::unique_ptr<GpuModel> characterGpu;
        std::vector<std::unique_ptr<ModelInstance>> stages;
        std::vector<std::unique_ptr<GpuModel>> stageGpu;
        std::shared_ptr<BoundMotion> motion;
        std::shared_ptr<CameraMotion> camera;
        // The camera VMD's light / self-shadow tracks (empty when absent or only MMD's defaults); used when
        // AppSettings::motionLighting is on.
        std::vector<studio::LightKf> lightTrack;
        std::vector<studio::ShadowKf> shadowTrack;
        float endFrame = 0;
        float motionFps = kMmdFps;  // play scene's motion time base (60 for 60 fps VMDs)
        bool hasAudio = false;
        float physicsFrame = -1;  // motion frame of the last physics step (-1: reset on next update)
        std::chrono::steady_clock::time_point physicsClock;  // wall time of the last physics step
        // Render benchmark: performers after `character` (posed once by PoseRenderBench, never
        // updated per frame). Empty for normal playback.
        struct Performer {
            std::unique_ptr<ModelInstance> instance;
            std::unique_ptr<GpuModel> gpu;
            std::shared_ptr<BoundMotion> motion;
            std::string characterId;
            std::string displayName;
        };
        std::vector<Performer> extras;
    };

    // --- lifecycle (App.cpp)
    bool InitWindow(HINSTANCE instance, int width, int height);
    bool InitImGui();
    void ShutdownImGui();
    void MainLoop();
    void RenderFrame();          // one iteration: ImGui frame, update, record, present
    std::filesystem::path ResolveLibraryPath() const;
    void StartScan();
    // Per-file classification overrides chosen in the app (library_overrides.json next to the
    // ini, keyed by library folder). Changing them rescans the library.
    LibraryOverrides LoadLibraryOverrides() const;
    void SetLibraryOverride(const std::vector<std::filesystem::path>& files, AssetKind kind);  // Auto removes
    void ClearHiddenOverrides();
    void AssetContextMenu(const std::vector<std::filesystem::path>& files, bool character);
    void PollScan();
    void StartRevalidation();
    void ApplyRevalidatedLibrary(LibraryScanResult fresh);
    void RestoreSelectionsFromSettings();
    void ApplyStartScreen();
    void ApplyCommandLinePreselection();
    void ApplyRenderSettings();
    // Theme: settings_.theme (or --theme) resolved to light / dark, applied to the UI kit palette, the ImGui style and
    // the title bar. Requests go through themeDirty_ and run before the next ImGui frame (never mid-frame).
    void ApplyTheme();
    void DrawThemeButton(ImVec2 pos, float popupRight, float popupY);  // icon button + system / light / dark popup
    void SetThemeSetting(int theme);  // 0 auto, 1 light, 2 dark: saves and applies
    void ApplyGraphicsPreset(int preset);  // sets the effect toggles of AppSettings
    void RefreshColorLuts();            // rescans luts_, resolves options_.lut
    void ApplyColorLut();               // uploads settings_.colorLut when it differs from appliedLut_

    // --- scene (App.cpp)
    void StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st, const SongAsset* song) {
        StartLoad(target, ch, st, song, nullptr);
    }
    void StartLoad(LoadTarget target, const CharacterAsset* ch, const StageAsset* st, const SongAsset* song,
                   const std::vector<PerformerCast>* formation);
    // P5: a play-mode material-source change reloads the scene at the current time (single character; a unit-song
    // formation scene applies it at the next load instead, and the UI says so).
    void ReloadPlaySceneForMaterial();
    void PollLoad();
    bool BuildSceneRuntime(ScenePackage& pkg);  // GPU upload on the main thread
    void UnloadScene();                          // WaitForGpu, release runtime + audio
    void UpdateScene(float frame);               // evaluate motion, update poses, upload to GPU
    void BuildFrameView(float frame, FrameView& view);

    // --- screens (UiSelect.cpp / UiPlay.cpp / UiBenchmark.cpp)
    void DrawScanning();
    void DrawSelect();
    void DrawLoading();
    void UpdatePlay(double dt);
    void DrawPlayOverlay();
    // Play mode: the performer the per-character popups edit (0 = center, i+1 = extras[i]); reset per scene.
    int playEditPerformer_ = 0;
    struct PlayPerformerRef {
        std::string id;              // library id (settings CharacterScale / CharacterShader / CharacterMaterial)
        GpuModel* gpu = nullptr;     // traits for pack suggestions / shader button state (null when out of range)
        ModelInstance* instance = nullptr;  // material list, HasMaterialOptions
    };
    PlayPerformerRef PlayPerformerAt(int index);
    void DrawBenchLobby();
    void UpdateBenchRun();
    void DrawBenchRunOverlay();
    void DrawBenchResult();
    void RefreshLeaderboard();
    void DrawAppBar(int activeNav);  // 0 = library, 1 = benchmark
    void DrawScenePreview(const CharacterAsset* ch, const StageAsset* st, float x0, float y0, float x1, float y1,
                          float rounding);
    void StartBenchmarkLoad();       // the library-picked scene (PickBenchmarkScene)
    void SetPlaying(bool play);
    void FinishBenchmark();

    // --- render benchmark (AppRenderBench.cpp; overlay in UiBenchmark.cpp)
    void StartRenderBenchLoad();     // -> Loading (LoadTarget::RenderBench) -> BenchRender
    void PoseRenderBench();          // after BuildSceneRuntime: placement, physics pre-roll, final pose
    void RecordRenderBenchFrame(ID3D12GraphicsCommandList* cmd);  // instead of renderer_.Render (BenchRender)
    void AfterRenderBenchFrame();    // after ctx_.EndFrame (BenchRender): finish / cancel
    void FinishRenderBench(bool cancelled);
    void ReleaseRenderBenchImage();  // result texture (WaitForGpu first)
    void DrawRenderBenchOverlay();   // progress over the preview (UiBenchmark.cpp)
    // Lobby / loading card for the render benchmark scene: the last finished image when there is
    // one, else a dark studio with the three performers' thumbnails (UiBenchmark.cpp).
    void DrawRenderBenchPreview(float x0, float y0, float x1, float y1, float rounding);

    // --- studio (UiStudio.cpp): the editor screen for keyframe work on a multi-model scene
    void StartStudioLoad(const CharacterAsset* ch, const StageAsset* st, const SongAsset* song);  // -> Loading -> Studio
    bool FinishStudioLoad();          // main thread: GPU upload of studioPackage_, enters Screen::Studio
    void LeaveStudio();               // -> Select (unloads)
    void UpdateStudio(double dt);     // input, playback clock
    void DrawStudio();                // panels; sets the renderer's viewport to the free area
    void UpdateStudioScene();         // evaluate motions at the current frame, upload poses
    void BuildStudioFrameView(FrameView& view);
    void StudioSeek(double seconds);
    void StudioSetPlaying(bool play);
    void StudioRebuildRows();
    void StudioHandleTimeline(const studio::TimelineEvents& ev);
    void StudioPushTrackEdit(const char* name, const std::vector<studio::TrackState>& before);  // after = current state
    std::vector<studio::TrackState> StudioCaptureSelectedTracks();
    bool StudioTrackOfRow(uint64_t row, studio::RowKind& kind, std::string& name) const;
    void StudioInsertKeys(const std::vector<uint64_t>& rows, int frame);  // key the current value of each row
    std::vector<int> StudioRowFrames(uint64_t row) const;                 // key frames of a bone/morph/camera row
    std::vector<uint64_t> StudioExpandRows(const std::set<uint64_t>& rows) const;  // groups -> their key rows
    std::vector<uint64_t> StudioTargetRows() const;  // picked rows + rows of selected keys (expanded)
    void StudioSelectAll();
    void StudioDeleteSelected(const char* undoName);
    void StudioCopySelected();
    // at the current frame; curvesOnly: interpolation onto existing keys; mirrored: bone keys onto the other side, reflected
    void StudioPaste(bool curvesOnly, bool mirrored = false);
    void DrawStudioKeyMenu();                        // timeline right-click menu (key actions at the clicked frame)
    void StudioRegisterKeys();                       // I: key the target rows at the current frame
    void StudioShiftFrames(bool remove);             // MMD frame insert/delete (range length or 1 frame)
    void StudioCopyCurve();                          // inspector: interpolation of the first selected key
    void StudioPasteCurve();                         // onto every selected key of the same kind
    void StudioJumpKey(int dir);                     // previous/next key of the shown rows
    void StudioImportVmd();                                   // open dialog, then StudioImportVmdFrom
    void StudioImportVmdFrom(const std::filesystem::path& path);  // merge into the selected model (camera VMDs: camera)
    void StudioExportVmd();                                   // save dialog, then StudioExportVmdTo
    bool StudioExportVmdTo(const std::filesystem::path& path);  // selected model's motion (or the camera)
    void DrawStudioTopBar(float x0, float y0, float x1, float y1);
    void DrawStudioOutliner(float x0, float y0, float x1, float y1);
    void DrawStudioInspector(float x0, float y0, float x1, float y1);
    void DrawStudioTimeline(float x0, float y0, float x1, float y1);
    void DrawStudioViewport(float x0, float y0, float x1, float y1);
    void StudioCamera(CameraParams& cam) const;      // the view the viewport shows (motion or free camera)
    void StudioEnter(std::unique_ptr<studio::StudioDoc> doc);  // common tail of every way into the studio
    void StudioUpdateModel(studio::StudioModel& m, uint64_t slot, float frame, float physicsDt, bool resetPhysics);
    // The sun's light as the viewport shows it at `frame` (fractional): the camera VMD's light track while the sun is
    // linked to it, else the sun's own keyed values; black with the default direction without a sun.
    studio::LightKf StudioSunLight(float frame) const;
    // --- camera / light / self-shadow (UiStudioCamera.cpp): inspector panel, key fields, camera path, render tracks
    bool StudioCameraPerspective(float frame) const;  // the camera key's perspective switch in effect at `frame`
    void StudioOrthoCamera(CameraPose& pose, CameraParams& camera) const;  // "perspective off" view (approximation)
    void StudioApplyLightTracks(FrameView& view) const;
    // A VMD light / self-shadow track at `frame` -> the frame's key light (direction, colour) and shadows; an empty
    // track leaves the view as it is (UiStudioCamera.cpp). Studio tracks, and in play mode the camera VMD's tracks.
    static void ApplyLightShadowTracks(const std::vector<studio::LightKf>& light,
                                       const std::vector<studio::ShadowKf>& shadow, float frame, FrameView& view);  // light / self-shadow tracks -> the frame's light and shadows
    studio::LightKf StudioCurrentLight() const;      // the sun's light in effect at the playhead (StudioSunLight)
    void StudioKeyCameraFromView();                  // camera key at the current frame from the view being shown
    // The part of the viewport the 3D image fills: the whole rect, or its 16:9 fit when the motion camera is the view.
    void StudioRenderRect(float x0, float y0, float x1, float y1, float out[4]) const;
    void DrawStudioPlacePanel(float w);              // character transform (position / rotation / scale)
    void StudioCommitPlace(uint32_t uid, const studio::PropAttach& before, const studio::PropAttach& after);  // one undo step
    bool studioPlaceDragging_ = false;               // a transform field / gizmo drag is in progress ...
    studio::PropAttach studioPlaceBefore_;                   // ... that started from this placement
    void DrawStudioFrameMask(float x0, float y0, float x1, float y1);  // dims outside the render frame + guides
    struct FreeCamera;                               // orbit camera (defined below)
    void StudioViewportNavigate(bool hovered, bool active);  // viewport mouse / fly navigation (free or possessed camera)
    static FreeCamera FreeFromKey(const studio::CameraKf& k);  // orbit form of a camera key
    static FreeCamera FreeFromPose(const CameraPose& pose);    // orbit camera with the pose's eye and view axis (roll dropped)
    FreeCamera StudioViewedFree(int frame) const;    // the camera as shown now, in orbit form (target, yaw, pitch, distance)
    // possession: store `cam` as the camera key at the playhead. base: the key to start from when none exists there
    void StudioWriteCamera(const FreeCamera& cam, const studio::CameraKf* base = nullptr, bool takeView = true);
    void StudioNavEditTick();                        // closes the undo step of a possession gesture when it went quiet
    void StudioPossess(bool on);
    void StudioViewportCameraHandles(bool hovered);  // eye / target handles of the camera (free view, camera selected)
    std::string iniPath_;                            // ImGui ini (panel layout); must outlive the context
    bool studioFocusEffects_ = false;               // top bar effects button: bring the effects tab forward
    bool studioResetLayoutDone_ = false;            // default layout just built: show the properties tab
    int studioKeyMenuFrame_ = 0;                     // frame / row the timeline key menu was opened on
    uint64_t studioKeyMenuRow_ = 0;
    int studioActiveView_ = 0;                       // quad view: 0 perspective, 1 top, 2 front, 3 left (the view under the mouse)
    void StudioDrawPoseOverlay(const studio::ViewProj& vp, float x0, float y0, float x1, float y1);  // bones + gizmo of another view
    void StudioOrthoNavigate(const studio::ViewProj& vp, bool hovered, bool active);  // pan / zoom of the orthographic views
    void StudioAddQuadViews(FrameView& view) const;  // the quad view's extra views for the live viewport frame
    bool studioResetLayout_ = false;                 // rebuild the default panel arrangement next frame
    int studioNavIdle_ = 0;                          // frames since the last possessed edit (undo grouping)
    bool studioNavWrote_ = false;                    // a possessed edit was written this frame
    int studioCamHandle_ = 0;                        // 0 none, 1 eye, 2 target
    studio::GizmoDrag studioCamDrag_;
    studio::GizmoFrame studioCamFrame_;
    studio::GizmoPart studioCamHot_ = studio::GizmoPart::None;
    void StudioTakeFreeCamera();                    // leave the motion camera at its current view (viewport navigation)
    void StudioFocusSelection();                     // F: frame the picked bone / selected model
    float studioFlyMul_ = 1.0f;                      // RMB + WASD fly speed multiplier (wheel while flying)
    studio::CameraKf StudioViewedCamera(int frame) const;  // the camera as shown now (free view or motion camera) as a key at `frame`
    void StudioBeginKeyEdit(studio::RowKind kind);   // inspector key fields: one undo step per edit
    void StudioEndKeyEdit();
    void DrawStudioCameraPanel(float w);             // view / light / shadow sections of the camera inspector
    // true: no curve editor follows. live (camera only): the playhead's fields, shown with or without a key
    bool DrawStudioCameraKeyFields(float w, studio::RowKind kind, int frame, bool live = false);
    // --- depth-of-field focus (UiStudioFocus.cpp): the focus track + automatic focus -> FrameView focus / aperture
    void StudioComputeFocus(FrameView& view);        // BuildStudioFrameView: view.camera must be set
    bool StudioFocusTargetZ(uint32_t uid, studio::FocusBone bone, const CameraParams& cam, float& z) const;
    studio::FocusKf StudioNewFocusKey(int frame) const;  // a key inserted at `frame`: the segment in effect, or the shown subject
    void StudioSetFocusAtPlayhead(const studio::FocusKf& k, const char* undoName);  // upsert (undoable), keeps the key's fields
    void DrawStudioFocusSection(float w);            // camera panel: what is in focus + the segment picker at the playhead
    bool DrawStudioFocusKeyFields(float w, int frame);  // inspector fields of a selected focus key
    std::string StudioModelLabel(uint32_t uid) const;   // outliner name of a model uid ("" when gone)
    // --- accessory (prop) key track (UiStudioAccessory.cpp)
    studio::AccessoryKf StudioNewAccessoryKey(const studio::StudioModel& m, int frame) const;  // current placement as a key
    void StudioSetAccessoryAtPlayhead(const studio::AccessoryKf& k, const char* undoName);      // upsert (undoable)
    bool DrawStudioAccessoryKeyFields(float w, int frame);  // inspector fields of the accessory key at `frame`
    void DrawStudioAccessorySection(float w);               // prop inspector: accessory key summary + key button
    studio::AutoFocus studioAutoFocus_;
    studio::AutoFocus playAutoFocus_;   // play mode: picks the performer the shot is about (reset per scene)
    float studioFocusShown_ = 0.0f;                  // the last frame's focus distance / aperture scale / auto subject
    float studioFocusAperture_ = 1.0f;
    uint32_t studioFocusSubject_ = 0;                // the model the focus follows now (0: a distance / nothing)
    // --- scene light editing (UiStudioLight.cpp)
    void StudioSelectLight(uint32_t uid);
    void StudioAddLight(studio::LightKind kind);
    void StudioDeleteLight(uint32_t uid);
    void StudioApplyLightPreset(int presetIndex);
    void StudioViewportLightHandles(bool hovered);
    bool StudioScriptLightGizmoPoint(const std::string& part, ImVec2& out) const;
    LightAnchors StudioBuildLightAnchors() const;
    void DrawStudioLightOutliner();
    void DrawStudioLightInspector(float w);
    void DrawStudioLightPresetConfirm();
    void StudioBeginLightEdit(uint32_t uid, bool keyEdit);
    void StudioEndLightEdit(uint32_t uid);

    void StudioUpdateCameraPath();                   // samples the motion camera (cached per cameraVersion)
    void DrawStudioCameraPath(float x0, float y0, float x1, float y1);
    bool StudioPickCameraKey(ImVec2 mouse);          // click on a key dot of the path: select it and seek
    void StudioCameraPathWindow(int& lo, int& hi) const;  // frames of the path drawn: the range, else now +-3 s
    // --- pose editing (UiStudioPose.cpp): bone overlay, picking, gizmo, pose layer, morph panel, VPD, mirror
    studio::StudioModel* StudioPoseModel();          // the selected model (pose editable: any kind)
    void StudioSelectBone(int bone, bool toggle);    // viewport pick / bone row click: selection sync (-1 clears)
    std::vector<studio::PoseBone> StudioCurrentPose(const studio::StudioModel& m) const;  // effective anim values
    void StudioSetPose(const char* undoName, const studio::PoseLayer& before);  // push the selected model's layer edit
    // keys for the edited bones/morphs (all: every listed bone). layerBefore: the layer an undo restores (auto-key)
    void StudioRegisterPose(bool allBones, const studio::PoseLayer* layerBefore = nullptr, const char* undoName = nullptr);
    void StudioResetPose();                          // drop the unregistered edits
    void StudioMirrorPose();                         // left/right mirror (scope: whole model or selected bones)
    void StudioImportVpd();
    void StudioImportVpdFrom(const std::filesystem::path& path);
    void StudioExportVpd();
    bool StudioExportVpdTo(const std::filesystem::path& path);
    bool StudioApplyPose(studio::StudioModel& m);    // writes the pose layer over the evaluated motion (UpdateStudioScene); true = applied
    bool StudioGizmoFrameOf(const studio::StudioModel& m, int bone, studio::GizmoFrame& f, studio::GizmoMode& mode) const;
    void StudioViewportPose(float x0, float y0, float x1, float y1, bool hovered, bool& consumed);  // overlay + input
    void StudioViewportToolbar(float x, float y);
    void DrawStudioBoneTab(float w);
    void DrawStudioMorphTab(float w);
    bool StudioScriptGizmoPoint(int part, ImVec2& out) const;  // ui-script: a screen point on a gizmo part

    // --- projects, adding/removing models, props, audio, autosave (UiStudioProject.cpp)
    enum class StudioAction { None, Leave, New, Open, OpenFile };  // what the unsaved-changes prompt guards
    void StartStudioEmpty();                          // new empty project: camera/light tracks only (no loading)
    void StartStudioProjectLoad(const std::filesystem::path& file, bool recovery);  // -> Loading -> Studio
    void StartStudioPmmImport(const std::filesystem::path& file);  // .pmm import -> a NEW unsaved project
    void StudioRequest(StudioAction a, const std::filesystem::path& file = {});  // prompts first when dirty
    void StudioRunAction(StudioAction a, const std::filesystem::path& file);
    bool StudioSave(bool saveAs);                     // dialog when untitled or saveAs; false: cancelled / failed
    bool StudioSaveTo(const std::filesystem::path& file);
    studio::ProjectData StudioProjectData() const;    // snapshot of the document (copies the motions)
    std::filesystem::path StudioRecoveryFile() const; // <exe>/recovery/autosave.mmdxproj
    void StudioAutosave(bool force, bool wait);       // timer: writes the recovery file on a worker thread
    void StudioDiscardRecovery();                     // waits for a running autosave, deletes the recovery files
    // replaceUid != 0: the loaded model replaces that model's file (StudioReplaceModel) instead of being added.
    // `material` (non-null) overrides the resolved material choice for the loaded model (P5: a material-source change
    // reloads the model's PBR data without touching the saved choice again).
    // `queuedDrops` (drag and drop): VMD/VPD files meant for this model, applied once it finishes loading.
    // `retryQueuedDrops`: when the load fails, these are handed to the next job added, so the motion still lands.
    void StudioAddModelFile(studio::ModelKind kind, const std::filesystem::path& file, uint32_t replaceUid = 0,
                            const MaterialChoice* material = nullptr,
                            std::vector<std::filesystem::path> queuedDrops = {}, size_t retryQueuedDrops = 0);  // async
    void StudioAddModelDialog(studio::ModelKind kind, uint32_t replaceUid = 0);
    void StudioAddLibraryCharacter(int index, uint32_t replaceUid = 0);
    void StudioAddLibraryStage(int index, uint32_t replaceUid = 0);  // replace: the first part replaces, the rest are added
    void StudioApplyLibrarySong(int index);           // dance -> selected character, camera, audio when none
    bool StudioSetAudio(const std::filesystem::path& file);  // empty: remove
    void StudioAudioDialog();
    void StudioPollJobs();                            // finished loads: GPU upload + append (main thread)
    // --- drag and drop (AppDrop.cpp): WM_DROPFILES / the ui-script "drop" command -> the Studio
    void PollDroppedFiles();                          // every frame: consume droppedFiles_ (shader packs keep their screens)
    // The studio model kind a dropped model file becomes: `.x` -> prop; a PMX/PMD the library classified as a stage
    // part -> stage; everything else -> character.
    studio::ModelKind DropModelKind(const std::filesystem::path& file, const std::string& ext) const;
    void ApplyStudioDrop(std::vector<std::filesystem::path> files);  // route a drop into the open studio project
    void ApplyQueuedDrops(int modelIndex, const std::vector<std::filesystem::path>& files);  // VMD/VPD behind a model load
    void StudioRemoveModel(int index);                // waits for the GPU; clears the undo history
    // Swaps model `index`'s file for `pm` and keeps everything else (motion keys by name, placement, prop parent, shader,
    // visibility, uid). Waits for the GPU; clears the undo history (pose edits hold bone indices). False: GPU upload failed.
    bool StudioReplaceModel(int index, studio::StudioPackageModel& pm, std::string* note);
    void StudioClearEditState();                      // undo history, selection, clipboard, drags (model list changed)
    void StudioSelectModel(int index);                // -1 camera
    // A freshly added / opened character with no shader choice: SuggestShaderPack for its library id (never applies).
    void StudioSuggestShader(studio::StudioModel& m);
    DirectX::XMFLOAT4X4 StudioPropRoot(const studio::StudioModel& m) const;  // world matrix of a prop's origin
    void StudioSeekAudio();                           // audio cursor <- timeline time (audio offset)
    void DrawStudioAddMenu();                         // the outliner's "+" popup (library / file)
    void DrawStudioModelMenu(int index);              // outliner row context menu
    // --- physics bake (P2.2, UiStudioBake.cpp): turn a character's live physics into bone keys
    void StudioOpenBakeDialog();                      // model menu: open the options modal (range / step / bones / off)
    bool StudioStartBake(int start, int end, int step, bool selectedOnly, bool turnOff);  // start the worker / script
    void StudioPollBake();                            // every frame: finish the worker, apply one undo step
    void StudioBakeApply(const studio::BakeResult& result, int start, int end, bool turnOff);  // main thread apply
    void DrawStudioBakeDialog();                      // the options / progress modal
    std::future<bool> studioBakeFuture_;
    studio::BakeResult studioBakeResult_;             // worker result (read on the main thread after the future is ready)
    std::atomic<bool> studioBakeCancel_{false};       // cancel request (worker + progress modal)
    std::atomic<float> studioBakeProgress_{0.0f};
    bool studioBakeRunning_ = false;
    int studioBakeModel_ = -1;                        // model index the running bake belongs to
    int studioBakeStart_ = 0, studioBakeEnd_ = 0, studioBakeStep_ = 1;
    bool studioBakeTurnOff_ = true;                   // "physics off after baking" of the running / pending bake
    double studioBakeStartWall_ = 0;                  // for "is it taking a moment" (progress modal)
    // options modal fields
    bool studioBakeDialogOpen_ = false;
    int studioBakeOptStart_ = 0, studioBakeOptEnd_ = 0, studioBakeOptStep_ = 1;
    bool studioBakeOptSelectedOnly_ = false, studioBakeOptTurnOff_ = true;
    void DrawStudioPropPanel(float w);                // inspector: prop parent / bone / offset
    void DrawStudioUnsavedPrompt();
    void DrawStudioProjectMenu();                     // top bar file menu (new / open / recent / save as)
    void DrawRecoveryPrompt();                        // select screen: offer the autosave of a crashed session

    // --- rendering the project, shortcut help (UiStudioRender.cpp)
    // Video (settings_.video through the render dialog) or GI still of the current frame. The video covers
    // StudioRenderRange through the motion camera with the project's audio; the studio comes back as it was.
    void StartStudioRender(bool video);
    void StudioRenderRange(double& startSeconds, double& endSeconds) const;  // CLI range, else timeline range / whole
    void StudioPoseForRender(float frame);            // offline frames: every model at `frame` (physics steps forward)
    void StudioRestoreAfterRender();                  // FinishOffline: time, camera mode, physics back to the editor's
    DirectX::XMFLOAT3 StudioPerformerCenter() const;  // center bone of the first character (teleport detection)
    void DrawStudioRenderMenu();                      // top bar popup: video / still
    void DrawStudioEffectsPanel(float x0, float y0, float x1, float y1);  // the "효과" panel: screen effects + effect stack
    void DrawStudioHelp();                            // shortcut overlay (? key / top bar button)
    bool StudioModal() const { return videoDialogOpen_ || studioHelpOpen_ || studioLeaveConfirm_ || studioLightPresetPending_ >= 0 ||
                                       studioBakeDialogOpen_ || studioBakeRunning_; }

    // --- scripted UI input for tests (UiScript.cpp) and MCP
    void PumpUiScript();  // before ImGui::NewFrame: feeds the events due at framesInScene_
    void ScheduleUiScriptStep(int frame, std::string cmd, std::vector<std::string> args);
    using ScheduleFunc = std::function<void(int frame, std::string cmd, std::vector<std::string> args)>;
    bool ExecuteUiCommand(const std::string& cmd, const std::vector<std::string>& args,
                          const ScheduleFunc& schedule = nullptr, std::string* outError = nullptr);

    // --- MCP server and control (AppMcp.cpp)
    void InitMcp();              // at startup: --mcp, --no-mcp, else the setting (not in headless runs)
    void StartMcpServer();       // the settings switch
    void ShutdownMcp();          // also fails the commands still waiting for a later frame
    void PumpMcp(bool minimized);  // main thread, before ImGui::NewFrame (and while minimized)
    void ExecuteMcp(const std::string& tool, const nlohmann::json& args, std::shared_ptr<McpPromise> promise);
    // Leaves the current screen for a navigation tool like the UI's back buttons do. False (promise rejected) while
    // loading / rendering / benchmarking, or with unsaved studio changes unless args.discard_unsaved.
    bool McpLeaveScreen(const nlohmann::json& args, McpPromise& promise);
    void McpScheduleInput(int frame, std::string cmd, std::vector<std::string> args);  // frame in mcpFrame_ units
    const CharacterAsset* FindCharacter(const std::string& needle) const;
    const StageAsset* FindStage(const std::string& needle) const;
    const SongAsset* FindSong(const std::string& needle) const;
    const char* ScreenName(Screen s) const;

    // --- auto-update (UiUpdate.cpp): background check, notice, staged install across a restart
    void InitUpdater();          // once after the settings load: feed URL + current version
    void StartUpdateCheck();     // background check when the run is not headless/scripted
    void UpdateUpdate();         // every frame (any screen with the app bar): drains the workers
    void DrawUpdateNotice();     // the select screen's notice (available / progress / failed)
    void DrawUpdateProgressDialog();
    void StartUpdateInstall();   // "Update": stages and spawns the applier, then quits
    void UpdateCheckCommand();   // ui-script "updatecheck": synchronous check, logs the result
    void UpdateInstallCommand(); // ui-script "updateinstall": full flow, exits when staged
    int CliComponentE2e();       // Run(): headless component fetch for tools/components_e2e.py
    void UpdateComponentUi();    // every frame: drains component_
    void DrawFontComponentRow(float width);              // lobby settings: offer the CJK fonts
    void DrawUpscalerComponentRow(UpscalerKind kind, float width);  // upscaler picker: download hint
    bool HeadlessRun() const;    // --frames / --ui-script / benchmarks / offline renders

    // --- thumbnails (AppThumbnails.cpp)
    std::filesystem::path ThumbnailCacheDir() const;
    bool RenderThumbnail(ThumbnailKind kind, std::vector<LoadedModelCpu>& models, ImageRGBA8& out);
    uint64_t CharacterThumb(int index);  // ImTextureID, 0 while not ready
    uint64_t StageThumb(int index);

    // --- free camera helpers (UiPlay.cpp)
    void UpdateFreeCamera();
    void SetPlayMotionCamera(bool on);   // C / the play bar: leaving the motion camera keeps its current view

    // --- offline GI render: stills and videos (UiOffline.cpp)
    enum class OfflineMode { None, Still, Video, Probe };
    void StartOfflineStill();   // Play -> Offline: pauses and renders the current frame
    // Play -> Offline: frames of [start, end) in settings_.video's format; fromLobby: the render was started
    // from the select screen, so it returns there (scene unloaded) when done
    void StartOfflineVideo(double startSeconds, double endSeconds, bool fromLobby = false, bool probe = false,
                           bool background = false);
    void StartVideoRenderLoad();    // Select -> Loading (LoadTarget::OfflineVideo) -> StartOfflineVideo
    // Time estimate by sample render, in the background while the render dialog is open: whenever the
    // dialog's settings (or the assets) form a combination that has not been measured, the scene is
    // loaded (once) and one sample frame from the middle of the song is rendered behind the UI; the
    // result is stored in settings_.videoProbes and reused for every later identical combination.
    // Called every frame (also when the dialog is closed, which stops the probe).
    void UpdateVideoProbe();
    void PollProbeScene();            // finishes the background scene load (any screen)
    void StopVideoProbe(bool unloadScene);   // cancels a running probe, forgets pending ones
    void CancelBackgroundProbe();            // ends a running background probe and restores the renderer
    void StartBackgroundProbe(uint64_t key);
    struct VideoProbeStatus {
        enum class Phase { Idle, Preparing, Measuring } phase = Phase::Idle;
        float fraction = 0;           // Measuring: 0..1 progress of the sample render
    };
    VideoProbeStatus ProbeStatus() const;
    void StartOfflineProbe();         // --offline-probe: the same measurement from a scene loaded for playing
    bool VideoRendererAvailable(VideoRenderer r) const;  // the dialog greys out unavailable renderers
    // Time to render the dialog's current selection: measured by a sample render when one exists for this
    // exact combination, else a rough estimate.
    struct VideoEstimate {
        int frames = 0;
        double secondsPerFrame = 0;
        double totalSeconds = 0;
        double fileGigabytes = 0;
        bool measured = false;
    };
    VideoEstimate EstimateVideoRender() const;
    uint64_t CurrentVideoProbeKey() const;
    // Real-time renderers: the renderer settings for a video frame of `cfg` (fixed resolution, no vsync).
    RenderSettings VideoRealtimeSettings(const VideoRenderConfig& cfg) const;
    void RecordRealtimeVideoFrame(ID3D12GraphicsCommandList* cmd);
    VideoRenderConfig ActiveVideoConfig() const;  // settings_.video with the --offline-* overrides
    void UpdateOffline();       // per frame, before UI drawing: CLI triggers, cancel handling
    // Instead of renderer_.Render while screen_ == Offline: poses the next image's frame and
    // begins it, or continues the current one.
    void RecordOfflineFrame(ID3D12GraphicsCommandList* cmd);
    void AfterOfflineFrame();   // after ctx_.EndFrame: collects a finished image (save / encode), advances
    void FinishOffline(bool cancelled);  // Offline -> Play (paused); closes the encoder; toast
    void DrawOfflineOverlay();  // progress panel over the preview (screen_ == Offline)
    void DrawVideoRenderDialog();  // video render settings + start (Select, UiVideoDialog.cpp)
    // formation modal (UiFormation.cpp)
    struct FormationSlot {
        std::string characterId;
    };
    struct FormationState {
        bool open = false;
        LoadTarget target = LoadTarget::Play;
        int songIndex = -1;
        int centerIndex = 0;
        std::vector<FormationSlot> slots;
        int pickerSlot = -1;
        char pickerSearch[64] = {};
        bool justOpened = false;   // set by OpenFormationModal: keys of that frame belong to the lobby
    };
    FormationState formationModal_;

    void StartLobbyLoad(LoadTarget target, bool allowModal = true);
    SongAsset SongAsChosen(const SongAsset& song) const;   // with the camera picked in the lobby (songs with several)
    void OpenFormationModal(LoadTarget target);
    void DrawFormationModal();
    void AutoFormation();
    void StartFormationLoad();
    std::vector<PerformerCast> ParseFormationSpec(const std::string& spec, const SongAsset& song);

    void DrawToast();           // completion / error toast (Select, Play)
    // shader packs (UiShaderPack.cpp)
    void ApplyShaderChoice(GpuModel& gpu, const ShaderChoice& choice);   // every frame, before UpdateMaterials
    // P5: the per-character PBR material source. A saved choice, or the run-wide --material-source override when
    // given (wins over saved choices). Default when nothing applies.
    MaterialChoice ResolveMaterialChoice(const std::string& characterId) const;
    MaterialChoiceFn MaterialChoiceResolver() const {
        return [this](const std::string& id) { return ResolveMaterialChoice(id); };
    }
    // The model's .emd candidates (asset/RayMaterial.h), cached per key (character id / library id / "<path>") for
    // the session so the detail panel never repeats the ~10 ms scan. Empty = none found.
    const std::vector<RayEmdCandidate>& MaterialCandidates(const std::string& key,
                                                           const std::filesystem::path& modelPath, int pmxMaterialCount);
    // P6: does the imported model carry PBR data (asset/ModelImport.h ModelHasImportedPbr)? Cached per key like
    // MaterialCandidates; false for PMX/PMD.
    bool ImportedPbrCandidates(const std::string& key, const std::filesystem::path& modelPath);
    // The shared "재질" widget (Default / Ray-MMD + candidate combo + file picker). Shown only when the model has a
    // Ray candidate or an explicit .emd is saved. Writes `choice`; true = changed. `modelPath`/`count` come from the
    // model (PMX or a loaded-only copy); `cacheKey` names the candidate cache entry.
    bool HasMaterialOptions(const std::string& cacheKey, const std::filesystem::path& modelPath, int pmxMaterialCount,
                            const MaterialChoice& choice);
    bool DrawMaterialSelector(const char* id, MaterialChoice& choice, const std::string& cacheKey,
                              const std::filesystem::path& modelPath, int pmxMaterialCount, float width);
    // P5: the material count of a model file, read once and cached per key (the lobby stage selector needs it; a
    // stage carries none). A cheap PMX/PMD header probe; an imported model with PBR data answers non-zero.
    int ModelMaterialCount(const std::string& key, const std::filesystem::path& modelPath);
    // One toast per character id when a character with no saved material choice has a Ray .emd candidate (never
    // applies). Imitates SuggestShaderPack; skipped in headless / scripted runs and with --material-source.
    void SuggestRayMaterial(const std::string& characterId, const std::filesystem::path& modelPath, int pmxMaterialCount);
    std::map<std::string, std::vector<RayEmdCandidate>> materialCandidates_;   // session cache (see MaterialCandidates)
    std::map<std::string, bool> importedPbrCandidates_;                         // P6 session cache (ImportedPbrCandidates)
    std::map<std::string, int> materialCountCache_;                            // session cache (ModelMaterialCount)
    std::map<std::string, bool> materialSuggestChecked_;                        // ids checked this session
    // Play character: the saved settings choice or the --shader-pack override. A character with no saved choice also
    // gets SuggestShaderPack (a toast only; the choice is never set by the app).
    ShaderChoice PlayShaderChoice(const GpuModel& gpu, const std::string& characterId);
    // Shows ONE toast per (installed pack that Suggests the model's traits, character id), remembered in
    // AppSettings::characterShaderSuggested. Skipped in headless / scripted runs and with --shader-pack. The first
    // suggesting pack in registry order wins. Cheap to call every frame (checked ids are cached per session).
    void SuggestShaderPack(const GpuModel& gpu, const std::string& characterId);
    std::set<std::string> suggestChecked_;   // character ids SuggestShaderPack already evaluated this session
    // A row showing the chosen pack; click opens a searchable pack list (+ "셰이더 관리"). True = changed.
    // `traits` = the shown model's data traits (null = unknown, e.g. the lobby): packs that Suggest them get a "추천" tag.
    bool DrawShaderSelector(const char* id, ShaderChoice& choice, float width, const ModelTraits* traits = nullptr);
    bool DrawShaderPackParams(ShaderChoice& choice);                     // the chosen pack's sliders + reset
    // per-material class / default-shading list of a pack model (`maxHeight` caps the scrolling list)
    bool DrawMaterialShaderList(const char* id, ShaderChoice& choice, const PmxModel& pmx, float width, float maxHeight);
    static int MaterialOverrideCount(const ShaderChoice& choice);
    std::set<int> materialSel_;                    // DrawMaterialShaderList: selected rows (Ctrl / Shift + click)
    std::string materialSelOwner_;                 // the list the selection belongs to
    int materialSelAnchor_ = -1;
    bool playMaterialsOpen_ = false;               // play bar shader popup: the material list is expanded
    bool lobbyMaterialsOpen_ = false;              // library shader tab: the material list is expanded
    std::string lobbyMatId_;                       // character whose model the list below was loaded for
    std::future<std::shared_ptr<PmxModel>> lobbyMatFuture_;
    std::shared_ptr<PmxModel> lobbyMatPmx_;
    enum class PackCompareMode { Drag, Hover };
    uint64_t PackThumb(const ShaderPack& pack);                          // preview texture, 0 = none / loading
    uint64_t PackThumbBefore(const ShaderPack& pack);
    uint64_t RemotePackThumb(const RemotePack& pack);
    uint64_t RemotePackThumbBefore(const RemotePack& pack);
    void DrawPackImage(ImDrawList* dl, uint64_t tex, const std::string& key, ImVec2 a, ImVec2 b, float rounding,
                       ImDrawFlags flags = 0);                           // cover-fit, placeholder when tex == 0
    void DrawPackCompare(const char* id, ImDrawList* dl, uint64_t before, const std::string& beforeKey, uint64_t after,
                         const std::string& afterKey, ImVec2 a, ImVec2 b, float rounding, ImDrawFlags flags,
                         PackCompareMode mode);
    // shader pack manager (UiShaders.cpp)
    void DrawShaders();
    void DrawShaderPackDetail(float x0, float y0, float x1, float y1);
    void DrawRemotePackDetail(float x0, float y0, float x1, float y1);
    void DrawNewPackDialog();
    void InstallPackPath(const std::filesystem::path& path);             // .zip or a pack folder (drop / dialog)
    void PollShaderPacks();                                              // every frame: hot reload, store, drops
    // The effect stack editor (UiShaders.cpp): add / remove / reorder (up/down) / enable + per-effect
    // sliders. Edits `stack` in place; true = changed. `widths` = the content width for layout.
    bool DrawEffectStackEditor(std::vector<EffectStackEntry>& stack, bool header = true);
    bool shaderParamsOpen_ = false;                                      // select screen: pack settings disclosure
    bool effectStackOpen_ = false;                                       // select screen: effect stack disclosure
    int shaderDetailTab_ = 0;                                            // shader detail: 0 info, 1 settings, 2 manage
    ShaderPackStore shaderStore_;
    int shaderTab_ = 0;                                                  // 0 installed, 1 online
    char shaderFilter_[128] = {};
    uint32_t shaderPackWriteTimesGen_ = 0;
    std::unordered_map<std::string, std::filesystem::file_time_type> shaderPackWriteTimes_;
    std::string shaderSelInstalled_, shaderSelRemote_;
    bool newPackOpen_ = false, newPackEffect_ = false;
    char newPackId_[64] = {}, newPackName_[96] = {}, newPackAuthor_[96] = {};
    std::vector<std::filesystem::path> droppedFiles_;
    std::vector<std::filesystem::path> pendingDrop_;  // a drop that arrived while scanning / loading: applied at the lobby
    void DrawStudioShaderRow(float w);                                   // inspector: the selected character's pack
    void DrawStudioMaterialRow(float w);                                 // inspector: the selected model's material source
    void CheckShaderPackErrors();                                        // compile failures -> error toast
    uint32_t shaderPackErrorsSeen_ = 0;
    std::filesystem::path OfflineOutputDir(bool video) const;  // Pictures\MMDX12 or Videos\MMDX12
    // Transparent export: the PNG-sequence folder for a chosen output path ("<stem>_frames" next to it).
    static std::filesystem::path AlphaSequenceDir(const std::filesystem::path& output);
    DirectX::XMFLOAT3 CharacterCenter() const;  // world position of the character's center bone (teleport detection)
    void OfflinePose(float frame);              // UpdateScene, or StudioPoseForRender for a studio job
    void OfflineView(float frame, FrameView& view);  // BuildFrameView, or BuildStudioFrameView for a studio job
    void SaveOfflinePose();                     // current scene pose -> offline_.prevPose (video motion blur)
    void UploadOfflinePrevPose(uint64_t slot);  // offline_.prevPose -> the models' bone/morph ring entry `slot`

    AppOptions options_;
    int shadingOverride_ = -1;  // --shading (hidden): run-only raster ViewShading override, -1 = off
    bool classicLighting_ = false;  // --classic: hard sun shadow, no rim light (vanilla-MMD look), never saved
    AppSettings settings_;
    std::filesystem::path settingsPath_;
    HWND hwnd_ = nullptr;
    bool running_ = true;
    bool minimized_ = false;
    bool themeDirty_ = false;   // re-resolve and apply the theme before the next frame (user choice / Windows theme changed)
    Screen screen_ = Screen::Scanning;

    Dx12Context ctx_;
    Renderer renderer_;
    AudioPlayer audio_;

    // library
    LibraryScanResult library_;
    std::future<LibraryScanResult> scanFuture_;
    ScanProgress scanProgress_;
    bool revalidating_ = false;
    std::future<LibraryScanResult> revalidateFuture_;
    ScanProgress revalidateProgress_;
    std::unique_ptr<LibraryScanResult> pendingLibraryResult_;
    int selCharacter_ = -1, selStage_ = -1, selSong_ = -1;  // indices into library_ vectors; stage -1 = none
    char filterCharacter_[128] = {}, filterStage_[128] = {}, filterSong_[128] = {};
    char libraryPathEdit_[512] = {};
    ThumbnailCache thumbs_;
    bool thumbsClearPending_ = false;
    int libraryTab_ = 0;          // 0 characters, 1 stages, 2 songs
    int lobbyTab_ = 0;                                                   // select screen settings tab: 0 screen, 1 shader, 2 detail
    std::filesystem::path assetsDir_;
    std::vector<ColorLutEntry> luts_;   // built-in looks + .cube files (ListColorLuts)
    std::string appliedLut_ = "\x01";   // id currently uploaded to the renderer ("\x01" = nothing applied yet)

    // loading
    LoadTarget loadTarget_ = LoadTarget::Play;
    std::future<bool> loadFuture_;
    std::unique_ptr<ScenePackage> loadPackage_;
    LoadProgress loadProgress_;
    std::string loadError_;

    // scene / play
    std::unique_ptr<SceneRuntime> scene_;
    double playTime_ = 0;         // seconds into the song
    double lastRenderedTime_ = -1; // for camera-cut / seek detection
    bool playing_ = false;
    bool useMotionCamera_ = true;
    bool overlayVisible_ = true;
    double lastMouseMoveTime_ = 0;
    struct FreeCamera { DirectX::XMFLOAT3 target{0, 10, 0}; float yaw = 0, pitch = 0.1f, distance = 45; float fovDeg = 30; } freeCam_;
    int freeCamDrag_ = 0;                            // mouse buttons (bit per ImGuiMouseButton) whose drag began over the scene
    studio::CameraKf studioCamKeyBase_;              // ... and its key (interpolation curves)
    FreeCamera studioCamBase_;                       // the camera when a handle drag started
    POINT lastMouse_{};
    double timeSeconds_ = 0;      // wall clock since start (QPC)
    std::atomic<bool> loading_{false};  // startup renderer init in flight: splash paint, resize deferred
    RenderInitProgress* renderInitProgress_ = nullptr;
    int framesInScene_ = 0;       // frames rendered in Play/BenchRun (for --frames)
    double gpuMsSum_ = 0.0;       // --frames: GPU time of the second half, logged at quit
    int gpuMsCount_ = 0;
    LARGE_INTEGER startupRunBegan_{};  // Run() entry (STARTUP first-frame timing)
    LARGE_INTEGER startupFreq_{};      // QPC frequency for the STARTUP timings

    // offline render
    struct OfflineJob {
        OfflineMode mode = OfflineMode::None;
        bool beginPending = false;     // the next frame poses and begins a new image
        bool cancelRequested = false;
        bool fromCli = false;          // quit the app when the job ends
        bool fromLobby = false;        // video started from the select screen: back to it when done
        VideoRenderConfig video;       // format of a video job (fixed when it starts)
        std::filesystem::path output;  // .png (still) or .mp4 (video)
        double startWall = 0;          // timeSeconds_ when the job started
        double imageStartWall = 0;     // timeSeconds_ when the current image began
        double startSeconds = 0;       // song time of image 0
        int frame = 0, frameCount = 1; // images done / total (still: 1)
        double avgImageSeconds = 0;    // wall time per finished image (ETA)
        std::unique_ptr<VideoEncoder> encoder;
        bool alpha = false;                // transparent export: write a PNG sequence instead of an encoder
        std::filesystem::path alphaDir;    // sequence folder (AlphaSequenceDir of the output path)
        std::string alphaBase;             // frame file stem: "<alphaBase>_00000.png"
        bool background = false;       // a probe behind the select screen: screen_ stays Select, nothing is presented
        std::chrono::steady_clock::time_point imageStartClock;  // precise start of the current image
        double probeSum = 0;           // probe: seconds per image (render + encode) of the measured images
        int probeN = 0;
        uint64_t probeKey = 0;         // probe: VideoProbeKey of the measured combination
        double avgEncodeSeconds = 0;   // wall time of VideoEncoder::AddFrame per image (ETA)
        int iter = 0, iterCount = 1;   // real-time renderers: passes done / passes accumulated for this frame
        float frameMmd = 0;            // real-time renderers: MMD frame of the image in progress
        bool realtime = false;         // frames come from the real-time renderer (Renderer::Render)
        bool wasPlaying = false;       // still: started during playback (the last live frame opens the shutter)
        bool studio = false;           // the Studio's project is rendered (studio_ instead of scene_); back to the studio
        double studioTime = 0;         // studio: the editor's time and camera mode before the render
        bool studioMotionCamera = false;
        // video motion blur: the previous video frame's pose (character, then stages) and camera
        struct Pose {
            std::vector<DirectX::XMFLOAT4X4> skin;
            std::vector<DirectX::XMFLOAT3> morph;
            uint64_t morphVersion = 0;
        };
        std::vector<Pose> prevPose;
        CameraParams prevCamera;
        DirectX::XMFLOAT3 prevCenter{};   // character center bone at prevPose
    } offline_;
    CameraParams lastLiveCamera_;       // camera of the last real-time frame (still motion blur)
    bool haveLastLiveCamera_ = false;
    bool videoDialogOpen_ = false;      // the select screen's video render dialog
    // background probe scene (UpdateVideoProbe)
    struct BackgroundProbe {
        std::future<bool> loadFuture;
        std::unique_ptr<ScenePackage> package;
        LoadProgress progress;
        std::string error;
        bool loading = false;
        uint64_t loadingSceneKey = 0;  // scene being loaded
        uint64_t sceneKey = 0;         // scene held in scene_ for probing (0: none)
        uint64_t pendingKey = 0;       // combination waiting for the settings to settle
        double pendingSince = 0;       // timeSeconds_ it was first seen
        uint64_t failedKey = 0;        // a combination whose sample render failed (not retried)
    } bgProbe_;
    bool cliOfflineStarted_ = false;
    struct Toast {
        std::string title, detail;
        std::filesystem::path path;   // file to reveal in Explorer (empty = none)
        bool error = false;
        double until = 0;             // timeSeconds_ when it disappears
    } toast_;

    // ui script
    struct UiScriptStep { int frame = 0; std::string cmd; std::vector<std::string> args; };
    std::vector<UiScriptStep> uiScript_;
    size_t uiScriptNext_ = 0;
    bool uiScriptLoaded_ = false;
    float uiScriptMouse_[2] = {-1e30f, -1e30f};  // last scripted mouse position (re-sent every frame)

    // auto-update (Updater.h): the controller owns its worker threads; the UI polls
    updater::Controller update_;
    bool updateCheckQueued_ = false;   // the check starts once the window is up
    bool updateInstallingFromCli_ = false;  // ui-script "updateinstall": exit when staged/failed
    std::string updateRelaunchArgs_;   // argv for the restarted app (cli-triggered updates)
    updater::ComponentFetcher component_;  // optional components (Components.h): on-demand fetch

    // studio
    std::unique_ptr<studio::StudioDoc> studio_;
    std::unique_ptr<studio::StudioPackage> studioPackage_;
    double studioLastBind_ = 0;       // timeSeconds_ of the last motion re-bind (throttled while dragging)
    bool studioLeaveConfirm_ = false; // unsaved-changes prompt is open (guards studioPending_)
    StudioAction studioPending_ = StudioAction::None;
    std::filesystem::path studioPendingFile_;
    // async model / song loads started inside the studio
    struct StudioJob {
        std::string label;
        bool song = false;                         // library song: dance/camera/audio; else models
        bool audioOnly = false;                    // just an audio file (the add menu's audio entry)
        bool audioPreloaded = false;               // the worker decoded `audio` (AudioPlayer::Preload): release it
        std::vector<studio::StudioPackageModel> models;
        studio::MotionData dance, camera;
        std::filesystem::path audio;
        uint32_t targetUid = 0;                    // song: the character it was started for (0: none)
        uint32_t replaceUid = 0;                   // model load: replaces this model (0: adds)
        bool materialReload = false;               // replace = the same file re-read for a material-source change
        std::vector<std::filesystem::path> queuedDrops;  // drop VMD/VPD applied to this model once it is in the doc
        size_t retryQueuedDrops = 0;                     // drop: on load failure, re-queue the first N onto the next job
        std::shared_ptr<McpPromise> mcp;           // studio_add_model: answered when the job ends
        LoadProgress progress;
        std::string error;
        std::future<bool> future;                  // last member: destroyed first, waits for the worker
    };
    std::vector<std::unique_ptr<StudioJob>> studioJobs_;
    std::future<bool> studioAutosave_;             // recovery file writer
    double studioAutosaveAt_ = 0;                  // timeSeconds_ of the last autosave check
    bool recoveryChecked_ = false;                 // the select screen asked about a recovery file this session
    bool recoveryPrompt_ = false;
    int studioRenameModel_ = -1;                   // outliner rename popup target
    int studioRemoveModel_ = -1;                   // outliner remove confirmation target
    std::string studioLoadTitle_;                  // Loading screen title while a project opens (empty: library scene)
    char studioRenameBuf_[128] = {};
    char studioAddFilter_[64] = {};
    bool studioThumbsShown_ = false;               // the add popup's thumbnail grid was drawn (pump thumbnails)
    int studioAddPage_ = 0;                        // "+" popup page: 0 menu, 1 characters, 2 stages, 3 songs
    uint32_t studioReplaceUid_ = 0;                // "+" popup in replace mode: the model whose file is swapped (0: add)
    bool studioOpenAdd_ = false;                   // the outliner opens the "+" popup next frame (model menu's replace)
    bool studioHelpOpen_ = false;                  // shortcut overlay
    bool studioRenderWhole_ = false;               // render dialog: whole project instead of the timeline range
    // viewport pose editing (cached from the last drawn frame: overlay, picking, scripts)
    studio::ViewProj studioVp_;
    bool studioGizmoShown_ = false;
    studio::GizmoFrame studioGizmoFrame_;
    studio::GizmoMode studioGizmoMode_ = studio::GizmoMode::Rotate;
    studio::GizmoPart studioGizmoHot_ = studio::GizmoPart::None;
    int studioViewDrag_ = 0;          // 0 none, 1 camera (orbit/pan), 2 gizmo, 3 press consumed by a bone pick
    ImVec2 studioPressPos_{};
    bool studioPressMoved_ = false;
    studio::GizmoDrag studioGizmoDrag_;
    studio::PoseLayer studioPoseBefore_;  // the layer when a gizmo drag / numeric edit started (one undo step)
    studio::PoseBone studioDragBase_;     // the bone's value when the drag started
    DirectX::XMFLOAT4 studioDragParentRot_{0, 0, 0, 1};
    float studioDragScale_ = 1.0f;
    int studioDragBone_ = -1;
    studio::AutoIkDrag studioAutoIkDrag_;   // auto-IK chain state captured when a wrist drag started
    bool studioAutoIkActive_ = false;       // the current gizmo drag is an auto-IK solve
    bool studioPoseFieldEdit_ = false;    // a numeric pose field / morph slider is being edited
    int studioHoverBone_ = -1;
    // camera inspector / path
    bool studioKeyEdit_ = false;                     // a camera/light/shadow key field is being edited
    bool studioKeyChanged_ = false;                  // ... and its value changed (else no undo step)
    bool studioKeyLive_ = false;                     // ... through the playhead fields (independent of the key selection)
    std::vector<studio::TrackState> studioKeyBefore_;
    std::vector<studio::CameraPathPoint> studioCamPath_;  // motion camera samples (per frame, strided when long)
    std::vector<DirectX::XMFLOAT3> studioCamKeys_;   // eye at each camera key
    uint64_t studioCamPathVersion_ = 0;
    int studioCamPathStride_ = 1;
    // scene light editing
    int studioLightPresetPending_ = -1;
    bool studioLightEdit_ = false;
    bool studioLightChanged_ = false;
    bool studioLightIsKeyEdit_ = false;
    uint32_t studioLightEditingUid_ = 0;
    std::vector<studio::TrackState> studioLightKeyBefore_;
    std::vector<studio::SceneLight> studioLightBaseBefore_;

    enum class LightDragPart { None = 0, PosGizmo, AimGizmo, SunRotate, ConeHandle, RangeHandle };
    LightDragPart studioLightDragPart_ = LightDragPart::None;
    studio::GizmoFrame studioLightPosFrame_;
    studio::GizmoFrame studioLightAimFrame_;
    studio::GizmoFrame studioLightSunFrame_;
    studio::GizmoPart studioLightHotGizmo_ = studio::GizmoPart::None;
    studio::GizmoDrag studioLightGizmoDrag_;
    studio::LightValues studioLightValuesBase_{};
    DirectX::XMFLOAT3 studioLightResolvedAim_{};
    ImVec2 studioLightConeHandlePos_{};
    ImVec2 studioLightRangeHandlePos_{};
    bool studioLightConeHot_ = false;
    bool studioLightRangeHot_ = false;
    bool studioLightGizmoShown_ = false;


    // benchmark
    int benchCategory_ = 0;       // index into kBenchCategories
    bool benchSubmittable_ = false;  // default workload (leaderboard submit allowed)
    std::vector<float> benchFrameTimes_;
    int benchFrameCounter_ = 0;
    int64_t benchLastQpc_ = 0;
    double benchStartTime_ = 0;
    BenchmarkResult benchResult_;
    std::future<LeaderboardPage> leaderboardFuture_;
    std::optional<LeaderboardPage> leaderboard_;
    std::string leaderboardError_;
    std::future<SubmitResponse> submitFuture_;
    std::optional<SubmitResponse> submitResult_;
    char nicknameEdit_[64] = {};

    // render benchmark run (AppRenderBench.cpp)
    struct RenderBenchRun {
        bool beginPending = false;     // the next BenchRender frame uploads the poses and begins the image
        bool cancelRequested = false;  // Esc / cancel button
        bool fromCli = false;          // started by --benchmark dx12-gi-render
        uint32_t width = kRenderBenchWidth, height = kRenderBenchHeight, samples = kRenderBenchSamples;
        bool official = true;          // default size and samples (submittable)
        RenderBenchCast cast;          // picked when the run starts
        int64_t startQpc = 0;          // QueryPerformanceCounter right after BeginOffline
        double elapsed = 0;            // seconds since start (updated every frame while rendering)
    } renderBench_;
    // finished image for the result screen (ImTextureID 0 = none) and where it was saved
    ComPtr<ID3D12Resource> renderBenchTex_;
    uint32_t renderBenchSrv_ = DescriptorHeap::kInvalid;
    uint64_t renderBenchImage_ = 0;
    uint32_t renderBenchImageW_ = 0, renderBenchImageH_ = 0;
    std::filesystem::path renderBenchSaved_;

    // MCP (AppMcp.cpp)
    std::unique_ptr<McpServer> mcpServer_;
    int mcpFrame_ = 0;                        // frames since start (framesInScene_ does not count on every screen)
    std::vector<UiScriptStep> mcpInput_;      // ui_input steps, frame = mcpFrame_ they run at (sorted)
    bool mcpInputActive_ = false;             // real mouse / keyboard input is ignored while steps are queued
    std::filesystem::path mcpOfflineOutput_;  // render_still / render_video output for the job about to start
    std::unique_ptr<AppOptions> mcpOptionsRestore_;  // the run's options before an MCP render's overrides

    struct PendingMcpWait {
        int waitFrames = 0;
        std::shared_ptr<McpPromise> promise;
    };
    std::vector<PendingMcpWait> pendingMcpWaits_;

    struct PendingMcpLoad {
        std::chrono::steady_clock::time_point startTime;
        std::shared_ptr<McpPromise> promise;
    };
    std::vector<PendingMcpLoad> pendingMcpLoads_;

    struct PendingMcpInput {
        int targetFrame = 0;
        std::shared_ptr<McpPromise> promise;
        nlohmann::json result;   // resolved with this (null: {"status": "completed"})
    };
    std::vector<PendingMcpInput> pendingMcpInputs_;
};

} // namespace mmdx
