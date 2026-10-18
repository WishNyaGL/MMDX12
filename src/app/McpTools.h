#pragma once
#include <json.hpp>
#include <string>

namespace mmdx {

inline const char* kMcpProtocolVersion = "2025-06-18";

// The MCP tool schemas (shared by the app and the bridge), one JSON document per tool, parsed once. They used to be
// nested nlohmann::json initializer lists: MSVC's overload resolution over those grows explosively with nesting, and
// at ~50 tools every translation unit including this header needed > 25 GB to compile (C1060 / a frozen machine).
// Keep them as JSON text. Each raw string must stay under MSVC's 16 KB string-literal limit.
inline const char* const kMcpToolSchemas[] = {
R"json({
 "description": "List running MMDX12 instances listening on named pipes",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "list_instances"
})json",
R"json({
 "description": "Connect to a specific MMDX12 instance pipe",
 "inputSchema": {
  "properties": {
   "pipe_name": {
    "description": "Named pipe name to connect to (e.g. mmdx12_mcp or mmdx12_mcp_<pid>)",
    "type": "string"
   }
  },
  "type": "object"
 },
 "name": "connect"
})json",
R"json({
 "description": "Launch MMDX12.exe from the bridge's folder with --mcp and wait until its pipe answers",
 "inputSchema": {
  "properties": {
   "extra_args": {
    "description": "Optional extra command-line arguments to pass to MMDX12.exe",
    "type": "string"
   }
  },
  "type": "object"
 },
 "name": "launch_app"
})json",
R"json({
 "description": "Get current application state (screen, playback, scene, render path, upscaler, fps/gpu ms, window size, studio summary)",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "get_state"
})json",
R"json({
 "description": "Get recent log lines from mmdx12.log ring buffer",
 "inputSchema": {
  "properties": {
   "level": {
    "description": "Filter logs by level",
    "enum": [
     "info",
     "warn",
     "error"
    ],
    "type": "string"
   },
   "lines": {
    "description": "Number of recent lines to return (default 50)",
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "get_recent_logs"
})json",
R"json({
 "description": "List available library assets (characters, stages, songs) usable by load tools",
 "inputSchema": {
  "properties": {
   "kind": {
    "description": "Asset kind to list (default: all)",
    "enum": [
     "all",
     "character",
     "stage",
     "song"
    ],
    "type": "string"
   },
   "query": {
    "description": "Optional substring filter on id or display name",
    "type": "string"
   }
  },
  "type": "object"
 },
 "name": "list_library"
})json",
R"json({
 "description": "Capture the window (scene + UI, no mouse cursor) as a PNG: returned inline, or saved to 'path' at full resolution (then only returned inline when return_image is true)",
 "inputSchema": {
  "properties": {
   "max_width": {
    "description": "Maximum width in pixels for downscaling (default 1280 inline, 0 = no downscale; a saved file is never downscaled unless max_width is given)",
    "type": "integer"
   },
   "path": {
    "description": "Save the PNG to this file (folders are created)",
    "type": "string"
   },
   "region": {
    "description": "Crop [x, y, width, height] in window pixels before scaling",
    "items": {
     "type": "number"
    },
    "type": "array"
   },
   "return_image": {
    "description": "With 'path': also return the image inline (default false)",
    "type": "boolean"
   },
   "scene_only": {
    "description": "Capture the renderer's last finished frame without any UI (default false)",
    "type": "boolean"
   }
  },
  "type": "object"
 },
 "name": "screenshot"
})json",
R"json({
 "description": "List the UI widgets of the last frame: label (visible text), id (language-independent id string), window, rect [x, y, w, h] in window pixels. 'query' matches label or id (case-insensitive substring); Korean source text also matches its translation in the current UI language. Use ui_click to press one.",
 "inputSchema": {
  "properties": {
   "include_hidden": {
    "description": "Also list clipped / hidden items (default false)",
    "type": "boolean"
   },
   "limit": {
    "description": "Maximum items (default 200)",
    "type": "integer"
   },
   "query": {
    "description": "Substring of the label or id (optional)",
    "type": "string"
   },
   "window": {
    "description": "Substring of the window name (optional)",
    "type": "string"
   }
  },
  "type": "object"
 },
 "name": "ui_items"
})json",
R"json({
 "description": "Click (or hover) a UI widget found by label or id, like ui_items' query. Exact matches win over substrings; several matches need 'index'. Returns the widget's rect once the click has been delivered (the UI reacts on the following frames).",
 "inputSchema": {
  "properties": {
   "action": {
    "description": "Default click",
    "enum": [
     "click",
     "dblclick",
     "right",
     "hover"
    ],
    "type": "string"
   },
   "index": {
    "description": "Which match when several remain (0-based, top-left first)",
    "type": "integer"
   },
   "offset": {
    "description": "Point inside the rect as fractions [fx, fy] (default [0.5, 0.5])",
    "items": {
     "type": "number"
    },
    "type": "array"
   },
   "target": {
    "description": "Label or id (Korean source text works in every UI language)",
    "type": "string"
   },
   "window": {
    "description": "Substring of the window name (optional)",
    "type": "string"
   }
  },
  "required": [
   "target"
  ],
  "type": "object"
 },
 "name": "ui_click"
})json",
R"json({
 "description": "Wait for N frames to render",
 "inputSchema": {
  "properties": {
   "n": {
    "description": "Number of frames to wait (default 1)",
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "wait_frames"
})json",
R"json({
 "description": "Switch active screen: select, studio, shaders, or bench",
 "inputSchema": {
  "properties": {
   "discard_unsaved": {
    "description": "Leave a studio project with unsaved changes anyway (default false: the call fails instead)",
    "type": "boolean"
   },
   "screen": {
    "description": "Target screen name",
    "enum": [
     "select",
     "studio",
     "shaders",
     "bench"
    ],
    "type": "string"
   }
  },
  "required": [
   "screen"
  ],
  "type": "object"
 },
 "name": "set_screen"
})json",
R"json({
 "description": "Load a scene with character, stage, and song by substring/ID match",
 "inputSchema": {
  "properties": {
   "character": {
    "description": "Character name or ID substring",
    "type": "string"
   },
   "discard_unsaved": {
    "description": "Leave a studio project with unsaved changes anyway (default false: the call fails instead)",
    "type": "boolean"
   },
   "song": {
    "description": "Song name or ID substring",
    "type": "string"
   },
   "stage": {
    "description": "Stage name or ID substring (or 'none')",
    "type": "string"
   }
  },
  "required": [
   "character"
  ],
  "type": "object"
 },
 "name": "load_scene"
})json",
R"json({
 "description": "Open Studio editor with character, stage, song, or an existing .mmdxproj project file",
 "inputSchema": {
  "properties": {
   "character": {
    "description": "Character name or ID substring",
    "type": "string"
   },
   "discard_unsaved": {
    "description": "Leave a studio project with unsaved changes anyway (default false: the call fails instead)",
    "type": "boolean"
   },
   "project": {
    "description": "Path to an existing .mmdxproj file",
    "type": "string"
   },
   "song": {
    "description": "Song name or ID substring",
    "type": "string"
   },
   "stage": {
    "description": "Stage name or ID substring",
    "type": "string"
   }
  },
  "type": "object"
 },
 "name": "open_studio"
})json",
R"json({
 "description": "Start or resume playback (in play mode or studio)",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "play"
})json",
R"json({
 "description": "Pause playback (in play mode or studio)",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "pause"
})json",
R"json({
 "description": "Seek playback to specified seconds",
 "inputSchema": {
  "properties": {
   "seconds": {
    "description": "Target time in seconds",
    "type": "number"
   }
  },
  "required": [
   "seconds"
  ],
  "type": "object"
 },
 "name": "seek"
})json",
R"json({
 "description": "Get current render and graphics settings",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "get_render_settings"
})json",
R"json({
 "description": "Change render and graphics settings",
 "inputSchema": {
  "properties": {
   "bloom_conv": {
    "description": "FFT convolution bloom toggle",
    "type": "boolean"
   },
   "dof": {
    "description": "Depth of field toggle",
    "type": "boolean"
   },
   "effect_stack": {
    "description": "The whole effect stack with parameters, in order (replaces it; use instead of effects). Parameter keys are the pack's pack.json params; values are clamped to their ranges, missing keys use the defaults",
    "items": {
     "properties": {
      "enabled": {
       "description": "Default true",
       "type": "boolean"
      },
      "id": {
       "description": "Effect pack id",
       "type": "string"
      },
      "params": {
       "additionalProperties": {
        "type": "number"
       },
       "description": "Parameter key -> value",
       "type": "object"
      },
      "texture_folder": {
       "description": "Per-entry texture folder (empty = the pack-level folder)",
       "type": "string"
      }
     },
     "required": [
      "id"
     ],
     "type": "object"
    },
    "type": "array"
   },
   "effects": {
    "description": "Comma-separated effect pack IDs in stack order, or 'none'. Effects already in the stack keep their parameters; new ones start at the pack defaults",
    "type": "string"
   },
   "lighting_preset": {
    "description": "Lighting preset (0..3)",
    "maximum": 3,
    "minimum": 0,
    "type": "integer"
   },
   "material_emd": {
    "description": "Explicit Ray-MMD .emd path for material_source 'ray' (empty = the best auto candidate)",
    "type": "string"
   },
   "material_source": {
    "description": "PBR material source for the selected character: 'default' or 'ray' (Ray-MMD). Changing it reloads the scene at the current time",
    "type": "string"
   },
   "quality": {
    "description": "Quality preset (0: low, 1: medium, 2: high, 3: ultra)",
    "maximum": 3,
    "minimum": 0,
    "type": "integer"
   },
   "render_path": {
    "description": "Render path: raster, rt (ray traced), or pt (path traced)",
    "enum": [
     "raster",
     "rt",
     "pt"
    ],
    "type": "string"
   },
   "shader_pack": {
    "description": "Shader pack ID for the selected character, or 'default'",
    "type": "string"
   },
   "shader_texture_folder": {
    "description": "Per-character texture folder for the selected character's shader pack (empty string clears)",
    "type": "string"
   },
   "upscale_quality": {
    "description": "Upscaler quality mode",
    "enum": [
     "native",
     "quality",
     "balanced",
     "performance",
     "ultra"
    ],
    "type": "string"
   },
   "upscaler": {
    "description": "Upscaler: none, dlss, fsr, xess",
    "enum": [
     "none",
     "dlss",
     "fsr",
     "xess"
    ],
    "type": "string"
   },
   "volumetric": {
    "description": "Volumetric fog / shafts toggle",
    "type": "boolean"
   }
  },
  "type": "object"
 },
 "name": "set_render_settings"
})json",
R"json({
 "description": "Get detailed Studio editor state (frame, selection, key counts, undo/redo, models, lights)",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "studio_get_state"
})json",
R"json({
 "description": "Run any existing ui-script studio command (e.g. studionew, studioadd, etc.)",
 "inputSchema": {
  "properties": {
   "args": {
    "description": "Command arguments array",
    "items": {
     "type": "string"
    },
    "type": "array"
   },
   "cmd": {
    "description": "Command name (e.g. studionew, studiosave, etc.)",
    "type": "string"
   },
   "discard_unsaved": {
    "description": "Leave a studio project with unsaved changes anyway (default false: the call fails instead)",
    "type": "boolean"
   }
  },
  "required": [
   "cmd"
  ],
  "type": "object"
 },
 "name": "studio_command"
})json",
R"json({
 "description": "Save current studio project to a .mmdxproj file",
 "inputSchema": {
  "properties": {
   "file": {
    "description": "Target file path",
    "type": "string"
   }
  },
  "required": [
   "file"
  ],
  "type": "object"
 },
 "name": "studio_save"
})json",
R"json({
 "description": "Open a .mmdxproj project file in Studio",
 "inputSchema": {
  "properties": {
   "discard_unsaved": {
    "description": "Leave a studio project with unsaved changes anyway (default false: the call fails instead)",
    "type": "boolean"
   },
   "file": {
    "description": "Source file path",
    "type": "string"
   }
  },
  "required": [
   "file"
  ],
  "type": "object"
 },
 "name": "studio_open"
})json",
R"json({
 "description": "Add a character, stage, or prop model to Studio",
 "inputSchema": {
  "properties": {
   "file": {
    "description": "Explicit file path",
    "type": "string"
   },
   "kind": {
    "description": "Model kind",
    "enum": [
     "character",
     "stage",
     "prop"
    ],
    "type": "string"
   },
   "library": {
    "description": "Library substring match",
    "type": "string"
   }
  },
  "required": [
   "kind"
  ],
  "type": "object"
 },
 "name": "studio_add_model"
})json",
R"json({
 "description": "Select model in Studio by index (-1 for none / camera)",
 "inputSchema": {
  "properties": {
   "index": {
    "description": "Model index",
    "type": "integer"
   }
  },
  "required": [
   "index"
  ],
  "type": "object"
 },
 "name": "studio_select"
})json",
R"json({
 "description": "Set the studio view (target tx/ty/tz, yaw/pitch degrees, distance, fov). Without key: the free view (the motion camera is switched off in the viewport). With key: true: writes the motion camera key at the playhead (inserted first if missing) and views through the motion camera",
 "inputSchema": {
  "properties": {
   "dist": {
    "type": "number"
   },
   "fov_deg": {
    "type": "number"
   },
   "key": {
    "description": "Insert keyframe at current frame",
    "type": "boolean"
   },
   "pitch_deg": {
    "type": "number"
   },
   "tx": {
    "type": "number"
   },
   "ty": {
    "type": "number"
   },
   "tz": {
    "type": "number"
   },
   "yaw_deg": {
    "type": "number"
   }
  },
  "type": "object"
 },
 "name": "studio_set_camera"
})json",
R"json({
 "description": "Set bone transform on the selected Studio model and optionally insert a keyframe",
 "inputSchema": {
  "properties": {
   "bone": {
    "description": "Bone name",
    "type": "string"
   },
   "key": {
    "description": "Whether to insert a keyframe at the current frame",
    "type": "boolean"
   },
   "rotate_deg": {
    "description": "[x, y, z] rotation in degrees",
    "items": {
     "type": "number"
    },
    "maxItems": 3,
    "minItems": 3,
    "type": "array"
   },
   "translate": {
    "description": "[x, y, z] translation offset",
    "items": {
     "type": "number"
    },
    "maxItems": 3,
    "minItems": 3,
    "type": "array"
   }
  },
  "required": [
   "bone"
  ],
  "type": "object"
 },
 "name": "studio_set_bone"
})json",
R"json({
 "description": "Insert keyframe(s) in Studio at the current frame",
 "inputSchema": {
  "properties": {
   "what": {
    "description": "selected = the selected rows / bones of the selected character, all = every bone of it, camera / lights = their tracks (default: selected)",
    "enum": [
     "all",
     "selected",
     "camera",
     "lights"
    ],
    "type": "string"
   }
  },
  "type": "object"
 },
 "name": "studio_key"
})json",
R"json({
 "description": "Undo the last Studio edit action",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "undo"
})json",
R"json({
 "description": "Redo the last undone Studio action",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "redo"
})json",
R"json({
 "description": "Start offline still image rendering with the GI renderer. Returns once the render has started (the app keeps running); poll render_status. Without 'output' the file goes to the app's usual render folder.",
 "inputSchema": {
  "properties": {
   "height": {
    "description": "Image height override",
    "type": "integer"
   },
   "output": {
    "description": "Output PNG path",
    "type": "string"
   },
   "spp": {
    "description": "Samples per pixel",
    "type": "integer"
   },
   "width": {
    "description": "Image width override",
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "render_still"
})json",
R"json({
 "description": "Start offline video render to an MP4 file. Returns once the render has started (the app keeps running); poll render_status. Without 'output' the file goes to the app's usual render folder.",
 "inputSchema": {
  "properties": {
   "bitrate": {
    "type": "integer"
   },
   "end": {
    "description": "End time: seconds (number) or a timecode string (HH:MM:SS / MM:SS / SS); default: the whole song",
    "oneOf": [
     {
      "type": "number"
     },
     {
      "type": "string"
     }
    ]
   },
   "fps": {
    "type": "integer"
   },
   "height": {
    "type": "integer"
   },
   "output": {
    "description": "Output MP4 path",
    "type": "string"
   },
   "quality": {
    "maximum": 3,
    "minimum": 0,
    "type": "integer"
   },
   "renderer": {
    "description": "Renderer type",
    "enum": [
     "raster",
     "rt",
     "pt",
     "gi"
    ],
    "type": "string"
   },
   "spp": {
    "type": "integer"
   },
   "start": {
    "description": "Start time: seconds (number) or a timecode string (HH:MM:SS / MM:SS / SS)",
    "oneOf": [
     {
      "type": "number"
     },
     {
      "type": "string"
     }
    ]
   },
   "width": {
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "render_video"
})json",
R"json({
 "description": "Query current offline render status and progress",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "render_status"
})json",
R"json({
 "description": "Cancel the currently active offline render",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "render_cancel"
})json",
R"json({
 "description": "Inject UI input events (move, click, dblclick, down, up, wheel, key, text, mod)",
 "inputSchema": {
  "properties": {
   "events": {
    "items": {
     "properties": {
      "args": {
       "items": {
        "type": "string"
       },
       "type": "array"
      },
      "cmd": {
       "description": "Input command (move, click, dblclick, down, up, wheel, key, text, mod)",
       "type": "string"
      },
      "frame": {
       "description": "Frame offset relative to now (0 = current frame)",
       "type": "integer"
      }
     },
     "required": [
      "cmd"
     ],
     "type": "object"
    },
    "type": "array"
   }
  },
  "required": [
   "events"
  ],
  "type": "object"
 },
 "name": "ui_input"
})json",
R"json({
 "description": "Cleanly close and exit MMDX12",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "quit_app"
})json",
R"json({
 "description": "Read-only snapshot of the active studio project at the current frame: fps (30), end_frame, aspect ratio, audio (file / offset_seconds / end_frame or null), every model (index, uid, kind, name, file, visible, placement, key counts, key frame range, world-space AABB [minx,miny,minz,maxx,maxy,maxz]; characters also height, center bone world position and facing_yaw_deg where 0 = facing -Z toward the default MMD camera, +90 = +X), the camera (key count, first/last frame, use_motion_camera, current target / yaw / pitch / roll in degrees, dist, fov_deg, eye world position) and every scene light with all base and sampled values (cone angles in degrees). Units are MMD world units; frames are timeline frames at 30 fps. Never changes the document.",
 "inputSchema": {
  "properties": {},
  "type": "object"
 },
 "name": "studio_scene_info"
})json",
R"json({
 "description": "Read-only world-space bone positions of one character or stage at sampled timeline frames (30 fps): {\"frames\": [...], \"bones\": {\"<bone name>\": [[x,y,z], ...]}, \"missing_bones\": [...]}. Pose = the bound motion at that frame plus the model's placement and scale, without pose-layer edits and with physics disabled, so the result is deterministic. Bones are PMX names (Japanese, e.g. 頭); missing ones are skipped and listed. Defaults: bones [センター, 上半身, 頭, 左手首, 右手首, 左足首, 右足首], the model's whole key range, step 5. At most 4000 samples (frames x bones). The viewport is restored afterwards. Props are rejected.",
 "inputSchema": {
  "properties": {
   "bones": {
    "description": "PMX bone names (Japanese), default the 7 tracked bones",
    "items": {
     "type": "string"
    },
    "type": "array"
   },
   "end": {
    "description": "Last frame (default: the motion's last key)",
    "type": "integer"
   },
   "model": {
    "description": "Model index as in studio_get_state (character or stage)",
    "type": "integer"
   },
   "physics": {
    "description": "Ignored: bones are never physics-simulated",
    "type": "boolean"
   },
   "start": {
    "description": "First frame (default: the motion's first key)",
    "type": "integer"
   },
   "step": {
    "description": "Frame step (default 5, minimum 1)",
    "type": "integer"
   }
  },
  "required": [
   "model"
  ],
  "type": "object"
 },
 "name": "studio_sample_motion"
})json",
R"json({
 "description": "Read-only derived choreography facts (timeline frames at 30 fps) to motivate camera shots. Per character: jumps [{frame, height}], crouches [{frame, depth}], hand_raises [{start, end, hand: left|right|both}], turns [{start, end, delta_yaw_deg}], holds [{start, end}], travel {start_xz, end_xz, bounds_min_xz, bounds_max_xz, max_speed} and energy windows of 15 frames normalised 0..1 over the character. With two or more characters it also returns formation (per 30 frames the left-to-right order of characters as seen from the default front camera, -Z looking +Z) and crossovers [{frame, a, b}]. Heights are in MMD world units. Never changes the document.",
 "inputSchema": {
  "properties": {
   "end": {
    "description": "Last frame (default: the motion's last key)",
    "type": "integer"
   },
   "model": {
    "description": "Character index; default: every character",
    "type": "integer"
   },
   "start": {
    "description": "First frame (default: the motion's first key)",
    "type": "integer"
   },
   "step": {
    "description": "Sampling step in frames (default 2, minimum 1)",
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "studio_motion_events"
})json",
R"json({
 "description": "Read-only framing evaluation at timeline frames (30 fps) using the project's motion camera (or one camera override for every frame). Per visible character: screen bbox [x0,y0,x1,y1] normalised 0..1 with y down, on_screen (fraction of sampled points inside), head_in_frame, cut (list of the sampled points outside: head, head_top, center, upper_body, left_hand, right_hand, left_foot, right_foot), screen_height (fraction of the frame height), center screen point, distance eye->center, facing_camera_deg (0 = looking into the lens). Also per frame: eye, fov_deg, aspect and shot_size from the largest character, in studio_camera_solve's vocabulary (ecu > 6.5, cu > 3.65, mcu > 2.4, ms > 1.64, cowboy > 1.09, full > 0.61, else wide; screen_height spans head top to feet even when parts are off screen, so a medium shot reads about 1.9). A character entirely behind the camera has behind_camera: true and no bbox. FOV is vertical, D3D left-handed perspective, and the project's aspect ratio is used. Poses are sampled without physics and the viewport is restored. Never changes the document.",
 "inputSchema": {
  "properties": {
   "camera": {
    "description": "Optional camera override for every frame (instead of the motion camera): orbit style like studio_set_camera (yaw 0 / pitch 0 looks along -Z at the target)",
    "properties": {
     "dist": {
      "description": "Distance from the target (positive)",
      "type": "number"
     },
     "fov_deg": {
      "description": "Vertical FOV in degrees (default 30)",
      "type": "number"
     },
     "pitch_deg": {
      "description": "Pitch in degrees",
      "type": "number"
     },
     "roll_deg": {
      "description": "Roll in degrees",
      "type": "number"
     },
     "target": {
      "description": "World target [x,y,z]",
      "items": {
       "type": "number"
      },
      "type": "array"
     },
     "yaw_deg": {
      "description": "Yaw in degrees",
      "type": "number"
     }
    },
    "type": "object"
   },
   "frames": {
    "description": "Timeline frames to evaluate",
    "items": {
     "type": "integer"
    },
    "type": "array"
   }
  },
  "required": [
   "frames"
  ],
  "type": "object"
 },
 "name": "studio_frame_check"
})json",
R"json({
 "description": "Read-only music structure analysis of the project's audio: bpm, confidence, frames_per_beat, beats, downbeats, beat_energy, onsets plus onset_strength, sections [{start, end, level, energy}] and audio_offset_seconds. All times are timeline frames at 30 fps (frame = (seconds + audio_offset) * 30, rounded to 0.01). start/end filter the returned frames. The first call may take about a second; the result is cached by audio path and file write time. Fails when the project has no audio.",
 "inputSchema": {
  "properties": {
   "end": {
    "description": "Last timeline frame to return (default: all)",
    "type": "integer"
   },
   "start": {
    "description": "First timeline frame to return (default: all)",
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "studio_audio_analysis"
})json",
R"json({
 "description": "Read the motion camera key list as JSON. Frame range {start,end} optional (inclusive). Each key: frame, target [x,y,z], yaw_deg, pitch_deg, roll_deg, dist, fov_deg (1..125), perspective, curve (object form: x/y/z/rotation/distance/fov, each a preset name or [x1,y1,x2,y2]), eye [x,y,z] (CameraMotion::ToView). Convention: CameraKf rotation.x = -pitch, rotation.y = yaw, rotation.z = roll (radians internally), distance = -dist (dist > 0 = the eye in front of the target, as studio_set_camera's dist). 'cuts' lists frames f where keys exist at f and f+1 (MMD camera cut convention).",
 "inputSchema": {
  "properties": {
   "end": {
    "description": "Last frame (default: no limit)",
    "type": "integer"
   },
   "start": {
    "description": "First frame (default 0)",
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "studio_get_camera_keys"
})json",
R"json({
 "description": "Write motion camera keys (max 20000). Each key needs frame; every other field is optional and a missing field takes the value the existing track evaluates to at that frame (the default CameraKf when empty). Fields: target [x,y,z], yaw_deg, pitch_deg, roll_deg, dist (dist > 0 = the eye in front of the target, stored as distance = -dist), fov_deg (clamped 1..125), perspective, curve. Curve = the easing INTO the key from the previous key: preset name, [x1,y1,x2,y2] (0..127), or an object {x,y,z,rotation,distance,fov}. mode merge (default) / replace_range / replace_all. A camera cut is two keys on adjacent frames (frame f, then frame f+1 with the new pose); there is no separate cut flag. Switches the viewport to the motion camera. One undo step.",
 "inputSchema": {
  "properties": {
   "keys": {
    "items": {
     "properties": {
      "curve": {
       "description": "Easing INTO this key from the previous key (VMD semantics). Preset name: linear (20,20,107,107), ease_in_out (64,0,64,127), ease_in (64,0,127,127), ease_out (0,0,64,127), smooth (42,0,86,127), fast_start (0,64,64,127), slow_start (64,0,107,64); [x1,y1,x2,y2] integers 0..127 for every parameter/channel; or an object per parameter/channel, each a preset name or [4] (missing = linear).",
       "oneOf": [
        {
         "enum": [
          "linear",
          "ease_in_out",
          "ease_in",
          "ease_out",
          "smooth",
          "fast_start",
          "slow_start"
         ],
         "type": "string"
        },
        {
         "items": {
          "maximum": 127,
          "minimum": 0,
          "type": "integer"
         },
         "maxItems": 4,
         "minItems": 4,
         "type": "array"
        },
        {
         "type": "object"
        }
       ]
      },
      "dist": {
       "type": "number"
      },
      "fov_deg": {
       "maximum": 125,
       "minimum": 1,
       "type": "number"
      },
      "frame": {
       "minimum": 0,
       "type": "integer"
      },
      "perspective": {
       "type": "boolean"
      },
      "pitch_deg": {
       "type": "number"
      },
      "roll_deg": {
       "type": "number"
      },
      "target": {
       "items": {
        "type": "number"
       },
       "maxItems": 3,
       "minItems": 3,
       "type": "array"
      },
      "yaw_deg": {
       "type": "number"
      }
     },
     "required": [
      "frame"
     ],
     "type": "object"
    },
    "minItems": 1,
    "type": "array"
   },
   "mode": {
    "description": "merge (default): upsert by frame. replace_range: delete the existing keys in [min frame, max frame] of the supplied keys first. replace_all: delete every key of the track(s) first.",
    "enum": [
     "merge",
     "replace_range",
     "replace_all"
    ],
    "type": "string"
   }
  },
  "required": [
   "keys"
  ],
  "type": "object"
 },
 "name": "studio_set_camera_keys"
})json",
R"json({
 "description": "Create, edit or delete a Studio scene light, optionally writing its whole key list, as ONE undo step. Identify an existing light with index; create one with create (sun|point|spot|area|ambient) plus optional name; delete one with delete: true (and index). At most one sun and one ambient are allowed. props (base, non-keyed fields): name, enabled, vmd_link, rim_strength, rim_color [r,g,b], aim_mode manual|target|sway, target_model (model index, -1 = performer), target_part centre|head, sway_phase, sky_zenith, sky_horizon, ground_color, shadow nocast|hard|soft, shadow_softness, shadow_density, shadow_color, falloff none|linear|inverse_square, affect_diffuse, affect_specular, viewport_visible. base holds the keyable values: position [x,y,z], aim [x,y,z], direction [x,y,z], color [r,g,b], intensity, range, cone_outer_deg, cone_inner_deg, size [w,h]. keys: [{frame, ...any keyable value}]; a missing value takes SampleLightValues of the light at that frame. Cone inner is clamped to <= outer. mode merge / replace_range / replace_all applies to the keys only. Answers index and uid.",
 "inputSchema": {
  "properties": {
   "base": {
    "description": "Keyable base values",
    "type": "object"
   },
   "create": {
    "enum": [
     "sun",
     "point",
     "spot",
     "area",
     "ambient"
    ],
    "type": "string"
   },
   "delete": {
    "description": "Remove the light at index",
    "type": "boolean"
   },
   "index": {
    "description": "Existing light index",
    "minimum": 0,
    "type": "integer"
   },
   "keys": {
    "items": {
     "properties": {
      "frame": {
       "minimum": 0,
       "type": "integer"
      }
     },
     "required": [
      "frame"
     ],
     "type": "object"
    },
    "type": "array"
   },
   "mode": {
    "description": "merge (default): upsert by frame. replace_range: delete the existing keys in [min frame, max frame] of the supplied keys first. replace_all: delete every key of the track(s) first.",
    "enum": [
     "merge",
     "replace_range",
     "replace_all"
    ],
    "type": "string"
   },
   "name": {
    "description": "Name for a newly created light",
    "type": "string"
   },
   "props": {
    "description": "Non-keyed light fields",
    "type": "object"
   }
  },
  "type": "object"
 },
 "name": "studio_set_light"
})json",
R"json({
 "description": "Create a whole Studio lighting rig in ONE undo step (\"AI: studio_light_rig\"). rig: key_fill_rim (key spot front 35 deg to one side, soft fill area other side, rim spot behind-above) | front_wash (count area lights in an arc in front, default 3) | top_spots (count spots above/in front, default 6; aim subject (default) or down) | side_bars (two columns of count point lights left/right, default 4) | back_beams (count spots behind aimed up/forward, sway with spread phases, default 4). subject = model index of the character (default: the first character); its centre / height come from the current frame, and positions scale with height (1.0 height unit = the character's height). Common: radius, height (both in character-height units), color [r,g,b] (linear RGB 0..1+) or palette [[r,g,b]...] cycled per light, intensity, cone_deg (spot outer degrees; inner = 0.7 x outer), size [w,h] (area), shadow nocast|hard|soft (default: key light hard, others nocast), name_prefix (default \"AI \"), replace (delete the lights a previous rig call with the same name_prefix created). Never touches the sun or ambient. Answers lights [{index, uid, name, kind, position, aim}] and undo.",
 "inputSchema": {
  "properties": {
   "aim": {
    "enum": [
     "subject",
     "down"
    ],
    "type": "string"
   },
   "color": {
    "items": {
     "type": "number"
    },
    "maxItems": 3,
    "minItems": 3,
    "type": "array"
   },
   "cone_deg": {
    "maximum": 180,
    "minimum": 0,
    "type": "number"
   },
   "count": {
    "maximum": 64,
    "minimum": 1,
    "type": "integer"
   },
   "height": {
    "description": "In character heights (default 1.6)",
    "type": "number"
   },
   "intensity": {
    "type": "number"
   },
   "name_prefix": {
    "type": "string"
   },
   "palette": {
    "items": {
     "items": {
      "type": "number"
     },
     "maxItems": 3,
     "minItems": 3,
     "type": "array"
    },
    "type": "array"
   },
   "radius": {
    "description": "In character heights (default 1.6)",
    "type": "number"
   },
   "replace": {
    "type": "boolean"
   },
   "rig": {
    "enum": [
     "key_fill_rim",
     "front_wash",
     "top_spots",
     "side_bars",
     "back_beams"
    ],
    "type": "string"
   },
   "shadow": {
    "enum": [
     "nocast",
     "hard",
     "soft"
    ],
    "type": "string"
   },
   "size": {
    "items": {
     "type": "number"
    },
    "maxItems": 2,
    "minItems": 2,
    "type": "array"
   },
   "subject": {
    "description": "Character model index (default: the first character)",
    "minimum": 0,
    "type": "integer"
   }
  },
  "required": [
   "rig"
  ],
  "type": "object"
 },
 "name": "studio_light_rig"
})json",
R"json({
 "description": "Read a model's bone (and optionally morph) keys as JSON. model = model index. bones: optional list of bone names (default: every bone with keys; unknown names are rejected). start/end optional inclusive frame range. Each bone key: frame, translate [x,y,z], rotate_deg [x,y,z] Euler XYZ degrees in studio_set_bone's convention (XMQuaternionRotationRollPitchYaw), quat [x,y,z,w] (lossless), curve (object form x/y/z/rotation, each a preset name or [x1,y1,x2,y2]). morphs: true adds morphs {name: [{frame, weight}]}. Output capped at 20000 keys with truncated: true.",
 "inputSchema": {
  "properties": {
   "bones": {
    "items": {
     "type": "string"
    },
    "type": "array"
   },
   "end": {
    "type": "integer"
   },
   "model": {
    "minimum": 0,
    "type": "integer"
   },
   "morphs": {
    "description": "Also return morph keys",
    "type": "boolean"
   },
   "start": {
    "type": "integer"
   }
  },
  "required": [
   "model"
  ],
  "type": "object"
 },
 "name": "studio_get_bone_keys"
})json",
R"json({
 "description": "Generate keyed light animation from the song's beats / sections in ONE undo step (\"AI: studio_light_cues\"). Every cue: lights (light indices or spots / points / areas / all = every non-sun non-ambient light), start / end (timeline frames at 30 fps), pattern pulse | chase | alternate | strobe | sweep | color_cycle | breathe | fade | section_levels | blackout, timing beat (default) | half | downbeat | bar | onset | section, every (use every Nth event, default 1), offset (event phase shift), min_strength (onset, default 0.5), base / peak intensity (default: the light's current intensity x0.3 / x1.0), decay (frames, default 0.6 x beat), colors [[r,g,b] linear 0..1+], aim_points [[x,y,z] world] or aim_targets [head, center, left, right, audience], from / to colour (fade), mode replace_range (default: delete each light's keys inside [start,end] first) | merge. Key values not driven by the pattern keep the light's sampled value at that frame. Events are rounded to integer frames and duplicates dropped; a timing that finds no events rejects the call. Answers cues [{pattern, lights, events, keys_written}], undo and bpm.",
 "inputSchema": {
  "properties": {
   "cues": {
    "items": {
     "properties": {
      "aim_points": {
       "items": {
        "items": {
         "type": "number"
        },
        "maxItems": 3,
        "minItems": 3,
        "type": "array"
       },
       "type": "array"
      },
      "aim_targets": {
       "items": {
        "enum": [
         "head",
         "center",
         "left",
         "right",
         "audience"
        ],
        "type": "string"
       },
       "type": "array"
      },
      "base": {
       "type": "number"
      },
      "colors": {
       "items": {
        "items": {
         "type": "number"
        },
        "maxItems": 3,
        "minItems": 3,
        "type": "array"
       },
       "type": "array"
      },
      "decay": {
       "minimum": 0,
       "type": "integer"
      },
      "end": {
       "minimum": 0,
       "type": "integer"
      },
      "every": {
       "minimum": 1,
       "type": "integer"
      },
      "from": {
       "items": {
        "type": "number"
       },
       "maxItems": 3,
       "minItems": 3,
       "type": "array"
      },
      "lights": {
       "description": "Light indices, or spots / points / areas / all",
       "oneOf": [
        {
         "items": {
          "minimum": 0,
          "type": "integer"
         },
         "type": "array"
        },
        {
         "enum": [
          "spots",
          "points",
          "areas",
          "all"
         ],
         "type": "string"
        }
       ]
      },
      "min_strength": {
       "maximum": 1,
       "minimum": 0,
       "type": "number"
      },
      "mode": {
       "enum": [
        "replace_range",
        "merge"
       ],
       "type": "string"
      },
      "offset": {
       "type": "integer"
      },
      "pattern": {
       "enum": [
        "pulse",
        "chase",
        "alternate",
        "strobe",
        "sweep",
        "color_cycle",
        "breathe",
        "fade",
        "section_levels",
        "blackout"
       ],
       "type": "string"
      },
      "peak": {
       "type": "number"
      },
      "start": {
       "minimum": 0,
       "type": "integer"
      },
      "timing": {
       "enum": [
        "beat",
        "half",
        "downbeat",
        "bar",
        "onset",
        "section"
       ],
       "type": "string"
      },
      "to": {
       "items": {
        "type": "number"
       },
       "maxItems": 3,
       "minItems": 3,
       "type": "array"
      }
     },
     "required": [
      "lights",
      "start",
      "end",
      "pattern"
     ],
     "type": "object"
    },
    "minItems": 1,
    "type": "array"
   }
  },
  "required": [
   "cues"
  ],
  "type": "object"
 },
 "name": "studio_light_cues"
})json",
R"json({
 "description": "Write bone and/or morph keys of a model (max 20000), ONE undo step over every track touched. model = model index. bones: {name: [{frame, translate?, rotate_deg? | quat?, curve?}]}; values are VMD-local like studio_set_bone. A missing translate / rotation takes the bone's evaluated value at that frame (interpolated from the current track; zero / identity when it has no keys). rotate_deg is [x,y,z] Euler degrees (XMQuaternionRotationRollPitchYaw(x,y,z)); quat is [x,y,z,w]; give one of them. Curve = easing into the key: preset, [x1,y1,x2,y2] (0..127), or {x,y,z,rotation}. morphs: {name: [{frame, weight}]}. Bones and morphs must exist in the model's PMX (unknown names are rejected with the list). mode merge / replace_range / replace_all applies per listed track. Bumps the model's motion version.",
 "inputSchema": {
  "properties": {
   "bones": {
    "description": "{bone name: [key...]}",
    "type": "object"
   },
   "mode": {
    "description": "merge (default): upsert by frame. replace_range: delete the existing keys in [min frame, max frame] of the supplied keys first. replace_all: delete every key of the track(s) first.",
    "enum": [
     "merge",
     "replace_range",
     "replace_all"
    ],
    "type": "string"
   },
   "model": {
    "minimum": 0,
    "type": "integer"
   },
   "morphs": {
    "description": "{morph name: [{frame, weight}]}",
    "type": "object"
   }
  },
  "required": [
   "model"
  ],
  "type": "object"
 },
 "name": "studio_set_bone_keys"
})json",
R"json({
 "description": "Delete keys in the inclusive frame range [start, end], one undo step. target: camera (no model), light (needs index), bone (needs model, optional bones list; default every bone with keys), morph (needs model, optional morphs list; default every morph with keys). Answers changed_keys and undo.",
 "inputSchema": {
  "properties": {
   "bones": {
    "items": {
     "type": "string"
    },
    "type": "array"
   },
   "end": {
    "minimum": 0,
    "type": "integer"
   },
   "index": {
    "description": "target=light: light index",
    "minimum": 0,
    "type": "integer"
   },
   "model": {
    "description": "target=bone/morph: model index",
    "minimum": 0,
    "type": "integer"
   },
   "morphs": {
    "items": {
     "type": "string"
    },
    "type": "array"
   },
   "start": {
    "minimum": 0,
    "type": "integer"
   },
   "target": {
    "enum": [
     "camera",
     "light",
     "bone",
     "morph"
    ],
    "type": "string"
   }
  },
  "required": [
   "target",
   "start",
   "end"
  ],
  "type": "object"
 },
 "name": "studio_delete_keys"
})json",
R"json({
 "description": "Framing solver: computes WHERE the camera goes for a requested shot, so the AI only decides WHAT. Samples the subject bones at 'frame' (no physics, viewport restored), fits the shot size from the projected body span (head top = head + 0.08 x height), places the anchor at 'screen' and iterates the distance until the projected span matches. Shot sizes (fraction of body height): ecu 0.10 around the eyes, cu 0.22, mcu 0.32, ms 0.50 (waist), cowboy 0.70, full 1.15, wide 2.20. Alternatively 'fill' = the subject span as a fraction of the frame height. Several models: the union is fitted in BOTH width and height (project aspect); 'shot' then only adds margin for full/wide. fov_deg default 25 (MMD stores integers; the solve compensates with distance). Optional 'lead' (0..0.2) adds look room: the subject shifts opposite its facing. Returns {camera: {target, yaw_deg, pitch_deg, roll_deg, dist, fov_deg, eye} - exactly studio_set_camera_keys' key fields - and check: one studio_frame_check frame. Never writes.",
 "inputSchema": {
  "properties": {
   "angle": {
    "description": "Camera angle. yaw_deg is relative to the subject's facing (0 = in front, looking at the face); +90 puts the camera at the subject's LEFT (-X while the subject faces -Z), negative is the right. absolute: true makes yaw_deg a world yaw like studio_set_camera. pitch_deg: + is above looking down. roll_deg: dutch angle",
    "properties": {
     "absolute": {
      "description": "yaw_deg is a world yaw instead of relative to the subject",
      "type": "boolean"
     },
     "pitch_deg": {
      "description": "+ = camera above looking down (default 0)",
      "type": "number"
     },
     "roll_deg": {
      "description": "Dutch roll in degrees (default 0)",
      "type": "number"
     },
     "yaw_deg": {
      "description": "Relative yaw (default 0 = front); ignored with absolute",
      "type": "number"
     }
    },
    "type": "object"
   },
   "fill": {
    "description": "Subject span as a fraction of frame height (overrides shot)",
    "type": "number"
   },
   "fov_deg": {
    "description": "Vertical FOV (default 25)",
    "maximum": 125,
    "minimum": 1,
    "type": "number"
   },
   "frame": {
    "description": "Timeline frame (30 fps)",
    "minimum": 0,
    "type": "integer"
   },
   "lead": {
    "description": "Look-room offset in the subject's facing direction (default 0)",
    "maximum": 0.2,
    "minimum": 0,
    "type": "number"
   },
   "screen": {
    "description": "Where the subject anchor lands, normalised 0..1 with y down (default x 0.5, y 0.38 for face-led shots else 0.5)",
    "properties": {
     "x": {
      "maximum": 1,
      "minimum": 0,
      "type": "number"
     },
     "y": {
      "maximum": 1,
      "minimum": 0,
      "type": "number"
     }
    },
    "type": "object"
   },
   "shot": {
    "description": "Shot size (default ms)",
    "enum": [
     "ecu",
     "cu",
     "mcu",
     "ms",
     "cowboy",
     "full",
     "wide"
    ],
    "type": "string"
   },
   "subject": {
    "description": "What the shot is about",
    "properties": {
     "focus": {
      "description": "What the shot is built around: face (head/eyes), upper (upper body) or body (centre). Default follows the shot size (face for ecu/cu/mcu)",
      "enum": [
       "face",
       "upper",
       "body"
      ],
      "type": "string"
     },
     "models": {
      "description": "Model indices (studio_get_state). Default: the first visible character. Several models are fitted as one group in width and height",
      "items": {
       "type": "integer"
      },
      "type": "array"
     }
    },
    "type": "object"
   }
  },
  "required": [
   "frame"
  ],
  "type": "object"
 },
 "name": "studio_camera_solve"
})json",
R"json({
 "description": "Shot / sequence generator: writes motion-camera keys for a list of shots in ONE undo step (unless dry_run). Every shot covers [start, end] inclusive and writes keys only inside its range. Overlapping shots are rejected. Consecutive shots with end+1 == next start form a HARD CUT (MMD: keys on adjacent frames); there is no cut flag. mode replace_range (default) deletes existing keys inside each shot first; merge upserts. Each shot solves its start framing (same rules as studio_camera_solve); the move is a camera move RELATIVE TO THAT OPENING FRAMING (it does not chase the subject). 'curve' is the easing of the move (preset name, default ease_in_out). follow: true (any move) is a steadicam: heading, distance and lens stay, the camera travels with the subject's smoothed movement (keys every follow_step frames); use it for travelling dancers. Arcs, follows and shots with accents are baked into keys with linear segments (the easing is baked in). Each shot answer lists 'warnings' (head out of frame / subject behind the camera at the start, quarters or end): fix those shots. accents adds push / zoom / roll bumps at beat frames (attack into the peak with [0,64,64,127], release with ease_out); accents closer than attack+release to each other or to the shot ends are skipped. Yaw is unwrapped so consecutive keys never jump more than 180 degrees. focus: {aperture} writes a DoF focus key at start targeting the subject's head (studiofocus; a separate undo step). Returns shots [{start, end, keys_written, first_check, last_check, warnings}], 'keys' with dry_run, and 'undo'.",
 "inputSchema": {
  "properties": {
   "dry_run": {
    "description": "Return the keys and write nothing (default false)",
    "type": "boolean"
   },
   "mode": {
    "description": "replace_range (default): delete keys inside each shot; merge: upsert only",
    "enum": [
     "replace_range",
     "merge"
    ],
    "type": "string"
   },
   "shots": {
    "items": {
     "properties": {
      "accent": {
       "description": "Beat accent inside the shot",
       "properties": {
        "amount": {
         "description": "push: fraction of dist (default 0.12); zoom: fov degrees (default 4); roll: degrees (default 6)",
         "type": "number"
        },
        "attack": {
         "description": "Frames from the pre value to the peak (default 3)",
         "minimum": 1,
         "type": "integer"
        },
        "kind": {
         "description": "push = dist, zoom = fov, roll = roll (default push)",
         "enum": [
          "push",
          "zoom",
          "roll"
         ],
         "type": "string"
        },
        "release": {
         "description": "Frames from the peak back to the settle (default 10)",
         "minimum": 1,
         "type": "integer"
        }
       },
       "type": "object"
      },
      "accents": {
       "description": "Beat frames inside the shot for accent bumps",
       "items": {
        "type": "integer"
       },
       "type": "array"
      },
      "amount": {
       "description": "Move amount: crane degrees (default 25), truck world units (default 0.6 x height), arc degrees (default 35)",
       "type": "number"
      },
      "angle": {
       "description": "Camera angle. yaw_deg is relative to the subject's facing (0 = in front, looking at the face); +90 puts the camera at the subject's LEFT (-X while the subject faces -Z), negative is the right. absolute: true makes yaw_deg a world yaw like studio_set_camera. pitch_deg: + is above looking down. roll_deg: dutch angle",
       "properties": {
        "absolute": {
         "description": "yaw_deg is a world yaw instead of relative to the subject",
         "type": "boolean"
        },
        "pitch_deg": {
         "description": "+ = camera above looking down (default 0)",
         "type": "number"
        },
        "roll_deg": {
         "description": "Dutch roll in degrees (default 0)",
         "type": "number"
        },
        "yaw_deg": {
         "description": "Relative yaw (default 0 = front); ignored with absolute",
         "type": "number"
        }
       },
       "type": "object"
      },
      "curve": {
       "description": "Easing between this shot's keys (preset name, default ease_in_out)",
       "type": "string"
      },
      "end": {
       "description": "Last frame (inclusive)",
       "minimum": 0,
       "type": "integer"
      },
      "end_angle": {
       "description": "Camera angle. yaw_deg is relative to the subject's facing (0 = in front, looking at the face); +90 puts the camera at the subject's LEFT (-X while the subject faces -Z), negative is the right. absolute: true makes yaw_deg a world yaw like studio_set_camera. pitch_deg: + is above looking down. roll_deg: dutch angle",
       "properties": {
        "absolute": {
         "description": "yaw_deg is a world yaw instead of relative to the subject",
         "type": "boolean"
        },
        "pitch_deg": {
         "description": "+ = camera above looking down (default 0)",
         "type": "number"
        },
        "roll_deg": {
         "description": "Dutch roll in degrees (default 0)",
         "type": "number"
        },
        "yaw_deg": {
         "description": "Relative yaw (default 0 = front); ignored with absolute",
         "type": "number"
        }
       },
       "type": "object"
      },
      "end_fov_deg": {
       "maximum": 125,
       "minimum": 1,
       "type": "number"
      },
      "end_screen": {
       "description": "Where the subject anchor lands, normalised 0..1 with y down (default x 0.5, y 0.38 for face-led shots else 0.5)",
       "properties": {
        "x": {
         "maximum": 1,
         "minimum": 0,
         "type": "number"
        },
        "y": {
         "maximum": 1,
         "minimum": 0,
         "type": "number"
        }
       },
       "type": "object"
      },
      "end_shot": {
       "description": "End shot size (dolly; default one size tighter / wider)",
       "enum": [
        "ecu",
        "cu",
        "mcu",
        "ms",
        "cowboy",
        "full",
        "wide"
       ],
       "type": "string"
      },
      "focus": {
       "description": "Optional DoF focus key at start on the subject's head",
       "properties": {
        "aperture": {
         "description": "Focus aperture scale (default 1)",
         "maximum": 1,
         "minimum": 0,
         "type": "number"
        }
       },
       "type": "object"
      },
      "follow": {
       "description": "Steadicam follow: keep heading / distance / lens and travel with the subject (default false)",
       "type": "boolean"
      },
      "follow_step": {
       "description": "Follow key step in frames (default 10)",
       "minimum": 1,
       "type": "integer"
      },
      "fov_deg": {
       "maximum": 125,
       "minimum": 1,
       "type": "number"
      },
      "move": {
       "description": "static holds the framing; dolly in/out changes dist; zoom in/out changes fov with the position fixed; dolly_zoom changes both (vertigo); crane up/down changes pitch and target height; truck left/right slides along the camera right axis (parallax, heading fixed); arc left/right orbits around the subject (amount degrees, default 35; +yaw direction for arc_right); follow holds the opening framing and travels with the subject; push_accent is a static shot driven by accents",
       "enum": [
        "static",
        "dolly_in",
        "dolly_out",
        "zoom_in",
        "zoom_out",
        "dolly_zoom",
        "crane_up",
        "crane_down",
        "truck_left",
        "truck_right",
        "arc_left",
        "arc_right",
        "follow",
        "push_accent"
       ],
       "type": "string"
      },
      "screen": {
       "description": "Where the subject anchor lands, normalised 0..1 with y down (default x 0.5, y 0.38 for face-led shots else 0.5)",
       "properties": {
        "x": {
         "maximum": 1,
         "minimum": 0,
         "type": "number"
        },
        "y": {
         "maximum": 1,
         "minimum": 0,
         "type": "number"
        }
       },
       "type": "object"
      },
      "shot": {
       "description": "Start shot size",
       "enum": [
        "ecu",
        "cu",
        "mcu",
        "ms",
        "cowboy",
        "full",
        "wide"
       ],
       "type": "string"
      },
      "start": {
       "description": "First frame (inclusive)",
       "minimum": 0,
       "type": "integer"
      },
      "subject": {
       "description": "What the shot is about",
       "properties": {
        "focus": {
         "description": "What the shot is built around: face (head/eyes), upper (upper body) or body (centre). Default follows the shot size (face for ecu/cu/mcu)",
         "enum": [
          "face",
          "upper",
          "body"
         ],
         "type": "string"
        },
        "models": {
         "description": "Model indices (studio_get_state). Default: the first visible character. Several models are fitted as one group in width and height",
         "items": {
          "type": "integer"
         },
         "type": "array"
        }
       },
       "type": "object"
      }
     },
     "required": [
      "start",
      "end"
     ],
     "type": "object"
    },
    "minItems": 1,
    "type": "array"
   }
  },
  "required": [
   "shots"
  ],
  "type": "object"
 },
 "name": "studio_camera_shots"
})json",
R"json({
 "description": "Layered motion editing: ONE undo step ('AI: studio_motion_edit') over every bone / morph track touched. model = character model index. ops apply in order (later ops see earlier output). Everything is validated first; the call is rejected (nothing written) on an unknown bone / morph, a bad field or >100000 written keys. dry_run reports per-op tracks and added / modified key counts (track_changes) and writes nothing.\nCommon op fields: start / end (inclusive frames), bones (PMX names), attack / release (smoothstep envelope fade in / out, default 6 / 6), step (frames between baked keys, default 2), curve (easing of the written keys, default linear). Baked layers also write start-1 / end+1 anchor keys holding the original value, so the motion outside the range is unchanged.\nrotate {bones, rotate_deg [x,y,z], space}: ADDITIVE rotation layer, Euler XYZ degrees like XMQuaternionRotationRollPitchYaw. space 'local' (default) is the bone's parent frame like VMD; 'world' converts the world-space offset into the bone's local frame using the parent's sampled world rotation.\ntranslate {bones? (default センター), offset [x,y,z]}: additive translation with the same envelope; the offset is WORLD for センター (center) and 全ての親 (all-parent), LOCAL (parent frame) for other bones.\nlook_at {target: 'camera' | [x,y,z] | {model, bone}, bones? (default ['首','頭']), weights? (default even), max_deg? (default 60)}: turns the head's forward axis (local -Z of 頭, MMD faces -Z) toward the target each sampled frame, clamps the turn to max_deg, splits the world-space offset across the bones by weights and applies it additively with the envelope. The op result reports mean_error_deg (mean remaining angle between the head and the target, sampled after the edit).\nretime {offset (frames, may be negative), scale? (default 1, about start), bones?}: moves keys in [start,end] (default every bone and morph track) to round(start + (frame-start)*scale) + offset; moved keys replace keys already on the destination frame; scale must be > 0.\ncopy {to, bones?, mirror?}: copies keys of [start,end] to [to, to+end-start], replacing keys there. mirror=true writes each bone's keys onto its mirror bone (MirrorPose: t.x and rotation y/z negated; center bones mirror in place).\nmirror {bones?}: mirrors the range in place (left/right tracks exchange their keys in [start,end]).\nsmooth {bones, window? (default 5), step?}: baked centred moving average (translation mean, normalized quaternion average with hemisphere alignment), envelope-blended with the original at the ends. Reports max_change_deg.\nmorph {morph (PMX name, e.g. まばたき, あ, 笑い), frames? [f...] or every + [start,end], peak? (0..1, default 1), attack? (2), hold? (2), release? (4)}: writes base at f-attack, peak at f, peak at f+hold, base at f+hold+release (base = the morph's original value). Overlaps merge by max.\nkey_pose {frame, bones: {name: {rotate_deg?, translate?}}, ease? (default 8)}: a held accent pose. The bones are set absolutely (local values) at frame and blended to / from the original motion over ease frames on both sides (baked at step).\nease {bones? (default every bone track with keys in range), curve (default smooth), only_linear (default true), channels? (default all four: x, y, z, rotation)}: rewrites the interpolation curves of the EXISTING keys in [start,end] (no new keys, no value change). only_linear skips keys whose curves are not on the diagonal. Reports curves_changed.\noverlap {chains? (default 上半身2/首/頭, 左腕/左ひじ/左手首, 右腕/右ひじ/右手首), lag (frames per link, default 2), step (default 1)}: follow-through; each chain bone at depth k is re-baked as child(f) = original(f - k*lag), blended in / out by the envelope. The chain root is unchanged.\nspring {bones, frequency (Hz, default 3), damping (ratio, default 0.45; < 1 overshoots), mix (0..1, default 1), step (default 1)}: second-order damped secondary motion (semi-implicit, 30 fps x 4 substeps) following the original rotation in rotation-vector space (translation for センター / グルーブ), starting at rest on the original at start - attack, blended by mix and the envelope. Reports max_deviation_deg.\nbreathe {bones? (default 上半身2, fallback 上半身), period (frames, default 105), amount_deg (default 1.2), only_holds (default true), head_drift_deg (default 0.8)}: additive idle pitch on the torso plus a deterministic smooth drift on 頭. With only_holds the layer is applied only inside detected dead holds (8-frame fades), else over the whole range. period must be >= 20.\nUse studio_motion_quality before and after to diagnose linear curves, dead holds and hard / synchronized stops.\nIK: leg and foot IK bones (足ＩＫ etc.) drive the legs; edit feet through the IK bones, not the leg bones (IK overwrites leg bone rotations).",
 "inputSchema": {
  "properties": {
   "dry_run": {
    "description": "Report what would change and write nothing",
    "type": "boolean"
   },
   "model": {
    "minimum": 0,
    "type": "integer"
   },
   "ops": {
    "items": {
     "properties": {
      "attack": {
       "minimum": 0,
       "type": "integer"
      },
      "bones": {
       "items": {
        "type": "string"
       },
       "type": "array"
      },
      "chains": {
       "description": "Bone-name arrays; each array is one parent-to-child chain",
       "items": {
        "items": {
         "type": "string"
        },
        "type": "array"
       },
       "type": "array"
      },
      "channels": {
       "description": "Curve channels rewritten by ease (x / y / z / rotation or 0..3)",
       "items": {},
       "type": "array"
      },
      "amount_deg": {
       "type": "number"
      },
      "damping": {
       "minimum": 0,
       "type": "number"
      },
      "frequency": {
       "minimum": 0,
       "type": "number"
      },
      "head_drift_deg": {
       "type": "number"
      },
      "lag": {
       "maximum": 12,
       "minimum": 0,
       "type": "integer"
      },
      "mix": {
       "maximum": 1,
       "minimum": 0,
       "type": "number"
      },
      "only_holds": {
       "type": "boolean"
      },
      "only_linear": {
       "type": "boolean"
      },
      "period": {
       "minimum": 20,
       "type": "integer"
      },
      "curve": {
       "description": "Curve of the written keys (preset name, [4] or per channel); default linear"
      },
      "ease": {
       "minimum": 1,
       "type": "integer"
      },
      "end": {
       "minimum": 0,
       "type": "integer"
      },
      "every": {
       "minimum": 1,
       "type": "integer"
      },
      "frame": {
       "minimum": 0,
       "type": "integer"
      },
      "frames": {
       "items": {
        "minimum": 0,
        "type": "integer"
       },
       "type": "array"
      },
      "hold": {
       "minimum": 0,
       "type": "integer"
      },
      "max_deg": {
       "type": "number"
      },
      "mirror": {
       "type": "boolean"
      },
      "morph": {
       "type": "string"
      },
      "offset": {
       "items": {
        "type": "number"
       },
       "maxItems": 3,
       "minItems": 3,
       "type": "array"
      },
      "op": {
       "enum": [
        "rotate",
        "translate",
        "look_at",
        "retime",
        "copy",
        "mirror",
        "smooth",
        "morph",
        "key_pose",
        "ease",
        "overlap",
        "spring",
        "breathe"
       ],
       "type": "string"
      },
      "peak": {
       "maximum": 1,
       "minimum": 0,
       "type": "number"
      },
      "release": {
       "minimum": 0,
       "type": "integer"
      },
      "rotate_deg": {
       "items": {
        "type": "number"
       },
       "maxItems": 3,
       "minItems": 3,
       "type": "array"
      },
      "scale": {
       "type": "number"
      },
      "space": {
       "enum": [
        "local",
        "world"
       ],
       "type": "string"
      },
      "start": {
       "minimum": 0,
       "type": "integer"
      },
      "step": {
       "minimum": 1,
       "type": "integer"
      },
      "target": {
       "description": "'camera', [x,y,z] or {model, bone}"
      },
      "to": {
       "minimum": 0,
       "type": "integer"
      },
      "weights": {
       "items": {
        "type": "number"
       },
       "type": "array"
      },
      "window": {
       "minimum": 1,
       "type": "integer"
      }
     },
     "type": "object"
    },
    "minItems": 1,
    "type": "array"
   }
  },
  "required": [
   "model",
   "ops"
  ],
  "type": "object"
 },
 "name": "studio_motion_edit"
})json",
R"json({
 "description": "Render a list of timeline frames through the studio's real renderer into ONE contact-sheet PNG (storyboard), each tile labelled with its frame number and mm:ss.ff in the top-left. Tiles are arranged in `columns` (default 4) with a 2 px gutter. Args: `frames` [int] OR `start`+`end` with `step` or `count` (default 12); `columns` (default 4); `tile_width` (96..640, default 320); `settle_frames` (default 4); `path` (save PNG); `return_image` (default true when no path); `physics` (default true; false disables physics for the sheet and restores it). Pauses playback and restores playing, the playhead and the motion-camera choice afterwards; forces the motion camera on when the project has camera keys. Max 48 tiles. A second sheet while one runs is rejected.",
 "inputSchema": {
  "properties": {
   "columns": {
    "description": "Tiles per row (default 4)",
    "minimum": 1,
    "type": "integer"
   },
   "count": {
    "description": "Number of tiles when start/end are used without step (default 12)",
    "minimum": 1,
    "type": "integer"
   },
   "end": {
    "description": "Last timeline frame (default: the studio end frame)",
    "type": "integer"
   },
   "frames": {
    "description": "Explicit timeline frames to render",
    "items": {
     "type": "integer"
    },
    "type": "array"
   },
   "path": {
    "description": "Save the sheet PNG here",
    "type": "string"
   },
   "physics": {
    "description": "false disables physics for the sheet and restores it afterwards (default true)",
    "type": "boolean"
   },
   "return_image": {
    "description": "Return image_base64 (default true when path is empty)",
    "type": "boolean"
   },
   "settle_frames": {
    "description": "Rendered frames to wait after each seek before capturing (default 4)",
    "minimum": 0,
    "type": "integer"
   },
   "start": {
    "description": "First timeline frame (default 0)",
    "type": "integer"
   },
   "step": {
    "description": "Frame step between start and end",
    "minimum": 1,
    "type": "integer"
   },
   "tile_width": {
    "description": "Tile width in pixels (96..640, default 320)",
    "maximum": 640,
    "minimum": 96,
    "type": "integer"
   }
  },
  "type": "object"
 },
 "name": "studio_contact_sheet"
})json",
R"json({
 "description": "Report singing from the characters' lip-sync morph tracks (motion only, 30 fps). Per character: phrases [{start,end,mean_open,syllables}] (mouth > threshold, gaps < 6 frames merged, runs < 12 frames dropped), breaths (the gaps between phrases), held_notes [{start,end,vowel}] (one vowel >= 0.5 with < 0.15 change for >= 18 frames inside a phrase), has_lipsync (false when no mouth morph has keys) and share (sung frames / total). Across characters: timeline [{start,end,singers:[model indices]}] covering [start,end] (empty singers = instrumental) merged over equal singer sets. Mouth morphs: vowels あ い う え お (+ ワ ∧ ω ▲ □ when present); ん counts as closed.",
 "inputSchema": {
  "properties": {
   "end": {
    "description": "Last timeline frame (default: the studio end frame)",
    "type": "integer"
   },
   "models": {
    "description": "Character model indices (default every character)",
    "items": {
     "minimum": 0,
     "type": "integer"
    },
    "type": "array"
   },
   "start": {
    "description": "First timeline frame (default 0)",
    "type": "integer"
   },
   "threshold": {
    "description": "Mouth-open threshold (default 0.12)",
    "minimum": 0,
    "type": "number"
   }
  },
  "type": "object"
 },
 "name": "studio_singing"
})json",
R"json({
 "description": "Diagnose stiff motion from a character's motion TRACKS (no posing). model = character index, start / end default to the motion's key range, window = frames per ranked window (default 60). Per bone group (head, torso, center, arm_l, arm_r, hand_l, hand_r, legs): linear_ratio (share of keys in range whose rotation, plus translation for center / IK, curve is on the diagonal within 2 units), hold_ratio + holds [{start,end}] (runs >= 15 frames with every bone under 0.15 deg/frame and 0.01 units/frame), hard_stops [frame] (angular speed from > 2 to < 0.2 deg/frame within 2 frames), jerk and stiffness 0..100. Across groups: sync_stops [{frame, groups}] (>= 3 groups hard-stop within +-1 frame), chain_lag (mean cross-correlation lag of angular-speed peaks over +-6 frames for 上半身2->首->頭 and both arm chains) and the 5 worst `window`-frame ranges [{start, end, score, reasons}]. Read-only.",
 "inputSchema": {
  "properties": {
   "end": {
    "description": "Last frame (default the motion's key range end)",
    "minimum": 0,
    "type": "integer"
   },
   "model": {
    "minimum": 0,
    "type": "integer"
   },
   "start": {
    "description": "First frame (default the motion's key range start)",
    "minimum": 0,
    "type": "integer"
   },
   "window": {
    "description": "Frames per ranked window (default 60)",
    "minimum": 1,
    "type": "integer"
   }
  },
  "required": [
   "model"
  ],
  "type": "object"
 },
 "name": "studio_motion_quality"
})json",
};

inline nlohmann::json GetMcpToolDefinitions() {
    static const nlohmann::json tools = [] {
        nlohmann::json list = nlohmann::json::array();
        for (const char* schema : kMcpToolSchemas) list.push_back(nlohmann::json::parse(schema));
        return list;
    }();
    return tools;
}

// How long the app may take to answer a tool call (the app's worker waits this long for the main thread; the
// bridge waits a little longer so the app's own timeout error arrives first).
inline unsigned McpToolTimeoutMs(const std::string& tool) {
    if (tool == "load_scene" || tool == "open_studio" || tool == "studio_open" || tool == "studio_add_model" ||
        tool == "studio_command")
        return 180000;
    if (tool == "wait_frames" || tool == "ui_input" || tool == "ui_click") return 120000;
    if (tool == "studio_contact_sheet") return 180000;
    if (tool == "studio_singing") return 60000;
    if (tool == "studio_audio_analysis") return 60000;
    return 30000;
}

} // namespace mmdx
