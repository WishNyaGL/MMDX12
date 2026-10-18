#pragma once
#include <json.hpp>
#include <string>

namespace mmdx {

inline const char* kMcpProtocolVersion = "2025-06-18";

inline nlohmann::json GetMcpToolDefinitions() {
    using json = nlohmann::json;
    json tools = json::array();

    // 1. list_instances (bridge-only)
    tools.push_back({
        {"name", "list_instances"},
        {"description", "List running MMDX12 instances listening on named pipes"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 2. connect (bridge-only)
    tools.push_back({
        {"name", "connect"},
        {"description", "Connect to a specific MMDX12 instance pipe"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"pipe_name", {
                    {"type", "string"},
                    {"description", "Named pipe name to connect to (e.g. mmdx12_mcp or mmdx12_mcp_<pid>)"}
                }}
            }}
        }}
    });

    // 3. launch_app (bridge-only)
    tools.push_back({
        {"name", "launch_app"},
        {"description", "Launch MMDX12.exe from the bridge's folder with --mcp and wait until its pipe answers"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"extra_args", {
                    {"type", "string"},
                    {"description", "Optional extra command-line arguments to pass to MMDX12.exe"}
                }}
            }}
        }}
    });

    // 4. get_state
    tools.push_back({
        {"name", "get_state"},
        {"description", "Get current application state (screen, playback, scene, render path, upscaler, fps/gpu ms, window size, studio summary)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 5. get_recent_logs
    tools.push_back({
        {"name", "get_recent_logs"},
        {"description", "Get recent log lines from mmdx12.log ring buffer"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"lines", {
                    {"type", "integer"},
                    {"description", "Number of recent lines to return (default 50)"}
                }},
                {"level", {
                    {"type", "string"},
                    {"enum", {"info", "warn", "error"}},
                    {"description", "Filter logs by level"}
                }}
            }}
        }}
    });

    // 6. list_library
    tools.push_back({
        {"name", "list_library"},
        {"description", "List available library assets (characters, stages, songs) usable by load tools"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"kind", {
                    {"type", "string"},
                    {"enum", {"all", "character", "stage", "song"}},
                    {"description", "Asset kind to list (default: all)"}
                }},
                {"query", {
                    {"type", "string"},
                    {"description", "Optional substring filter on id or display name"}
                }}
            }}
        }}
    });

    // 7. screenshot
    tools.push_back({
        {"name", "screenshot"},
        {"description", "Capture the window (scene + UI, no mouse cursor) as a PNG: returned inline, or saved to 'path' "
                        "at full resolution (then only returned inline when return_image is true)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"max_width", {
                    {"type", "integer"},
                    {"description", "Maximum width in pixels for downscaling (default 1280 inline, 0 = no downscale; "
                                    "a saved file is never downscaled unless max_width is given)"}
                }},
                {"path", {
                    {"type", "string"},
                    {"description", "Save the PNG to this file (folders are created)"}
                }},
                {"region", {
                    {"type", "array"},
                    {"items", {{"type", "number"}}},
                    {"description", "Crop [x, y, width, height] in window pixels before scaling"}
                }},
                {"return_image", {
                    {"type", "boolean"},
                    {"description", "With 'path': also return the image inline (default false)"}
                }},
                {"scene_only", {
                    {"type", "boolean"},
                    {"description", "Capture the renderer's last finished frame without any UI (default false)"}
                }}
            }}
        }}
    });

    // 7b. ui_items
    tools.push_back({
        {"name", "ui_items"},
        {"description", "List the UI widgets of the last frame: label (visible text), id (language-independent id "
                        "string), window, rect [x, y, w, h] in window pixels. 'query' matches label or id "
                        "(case-insensitive substring); Korean source text also matches its translation in the "
                        "current UI language. Use ui_click to press one."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"query", {{"type", "string"}, {"description", "Substring of the label or id (optional)"}}},
                {"window", {{"type", "string"}, {"description", "Substring of the window name (optional)"}}},
                {"include_hidden", {{"type", "boolean"}, {"description", "Also list clipped / hidden items (default false)"}}},
                {"limit", {{"type", "integer"}, {"description", "Maximum items (default 200)"}}}
            }}
        }}
    });

    // 7c. ui_click
    tools.push_back({
        {"name", "ui_click"},
        {"description", "Click (or hover) a UI widget found by label or id, like ui_items' query. Exact matches win "
                        "over substrings; several matches need 'index'. Returns the widget's rect once the click "
                        "has been delivered (the UI reacts on the following frames)."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"target", {{"type", "string"}, {"description", "Label or id (Korean source text works in every UI language)"}}},
                {"window", {{"type", "string"}, {"description", "Substring of the window name (optional)"}}},
                {"index", {{"type", "integer"}, {"description", "Which match when several remain (0-based, top-left first)"}}},
                {"action", {{"type", "string"}, {"enum", {"click", "dblclick", "right", "hover"}},
                            {"description", "Default click"}}},
                {"offset", {{"type", "array"}, {"items", {{"type", "number"}}},
                            {"description", "Point inside the rect as fractions [fx, fy] (default [0.5, 0.5])"}}}
            }},
            {"required", {"target"}}
        }}
    });

    // 8. wait_frames
    tools.push_back({
        {"name", "wait_frames"},
        {"description", "Wait for N frames to render"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"n", {
                    {"type", "integer"},
                    {"description", "Number of frames to wait (default 1)"}
                }}
            }}
        }}
    });

    // 9. set_screen
    tools.push_back({
        {"name", "set_screen"},
        {"description", "Switch active screen: select, studio, shaders, or bench"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"screen", {
                    {"type", "string"},
                    {"enum", {"select", "studio", "shaders", "bench"}},
                    {"description", "Target screen name"}
                }}
            }},
            {"required", {"screen"}}
        }}
    });

    // 10. load_scene
    tools.push_back({
        {"name", "load_scene"},
        {"description", "Load a scene with character, stage, and song by substring/ID match"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"character", {
                    {"type", "string"},
                    {"description", "Character name or ID substring"}
                }},
                {"stage", {
                    {"type", "string"},
                    {"description", "Stage name or ID substring (or 'none')"}
                }},
                {"song", {
                    {"type", "string"},
                    {"description", "Song name or ID substring"}
                }}
            }},
            {"required", {"character"}}
        }}
    });

    // 11. open_studio
    tools.push_back({
        {"name", "open_studio"},
        {"description", "Open Studio editor with character, stage, song, or an existing .mmdxproj project file"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"character", {
                    {"type", "string"},
                    {"description", "Character name or ID substring"}
                }},
                {"stage", {
                    {"type", "string"},
                    {"description", "Stage name or ID substring"}
                }},
                {"song", {
                    {"type", "string"},
                    {"description", "Song name or ID substring"}
                }},
                {"project", {
                    {"type", "string"},
                    {"description", "Path to an existing .mmdxproj file"}
                }}
            }}
        }}
    });

    // 12. play
    tools.push_back({
        {"name", "play"},
        {"description", "Start or resume playback (in play mode or studio)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 13. pause
    tools.push_back({
        {"name", "pause"},
        {"description", "Pause playback (in play mode or studio)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 14. seek
    tools.push_back({
        {"name", "seek"},
        {"description", "Seek playback to specified seconds"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"seconds", {
                    {"type", "number"},
                    {"description", "Target time in seconds"}
                }}
            }},
            {"required", {"seconds"}}
        }}
    });

    // 15. get_render_settings
    tools.push_back({
        {"name", "get_render_settings"},
        {"description", "Get current render and graphics settings"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 16. set_render_settings
    tools.push_back({
        {"name", "set_render_settings"},
        {"description", "Change render and graphics settings"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"render_path", {
                    {"type", "string"},
                    {"enum", {"raster", "rt", "pt"}},
                    {"description", "Render path: raster, rt (ray traced), or pt (path traced)"}
                }},
                {"upscaler", {
                    {"type", "string"},
                    {"enum", {"none", "dlss", "fsr", "xess"}},
                    {"description", "Upscaler: none, dlss, fsr, xess"}
                }},
                {"upscale_quality", {
                    {"type", "string"},
                    {"enum", {"native", "quality", "balanced", "performance", "ultra"}},
                    {"description", "Upscaler quality mode"}
                }},
                {"lighting_preset", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"maximum", 3},
                    {"description", "Lighting preset (0..3)"}
                }},
                {"quality", {
                    {"type", "integer"},
                    {"minimum", 0},
                    {"maximum", 3},
                    {"description", "Quality preset (0: low, 1: medium, 2: high, 3: ultra)"}
                }},
                {"dof", {
                    {"type", "boolean"},
                    {"description", "Depth of field toggle"}
                }},
                {"volumetric", {
                    {"type", "boolean"},
                    {"description", "Volumetric fog / shafts toggle"}
                }},
                {"bloom_conv", {
                    {"type", "boolean"},
                    {"description", "FFT convolution bloom toggle"}
                }},
                {"shader_pack", {
                    {"type", "string"},
                    {"description", "Shader pack ID for the selected character, or 'default'"}
                }},
                {"shader_texture_folder", {
                    {"type", "string"},
                    {"description", "Per-character texture folder for the selected character's shader pack (empty string clears)"}
                }},
                {"material_source", {
                    {"type", "string"},
                    {"description", "PBR material source for the selected character: 'default' or 'ray' (Ray-MMD). Changing it reloads the scene at the current time"}
                }},
                {"material_emd", {
                    {"type", "string"},
                    {"description", "Explicit Ray-MMD .emd path for material_source 'ray' (empty = the best auto candidate)"}
                }},
                {"effects", {
                    {"type", "string"},
                    {"description", "Comma-separated effect pack IDs in stack order, or 'none'. Effects already in the stack keep their parameters; new ones start at the pack defaults"}
                }},
                {"effect_stack", {
                    {"type", "array"},
                    {"description", "The whole effect stack with parameters, in order (replaces it; use instead of effects). Parameter keys are the pack's pack.json params; values are clamped to their ranges, missing keys use the defaults"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"id", {{"type", "string"}, {"description", "Effect pack id"}}},
                            {"enabled", {{"type", "boolean"}, {"description", "Default true"}}},
                            {"params", {{"type", "object"}, {"additionalProperties", {{"type", "number"}}}, {"description", "Parameter key -> value"}}},
                            {"texture_folder", {{"type", "string"}, {"description", "Per-entry texture folder (empty = the pack-level folder)"}}}
                        }},
                        {"required", {"id"}}
                    }}
                }}
            }}
        }}
    });

    // 17. studio_get_state
    tools.push_back({
        {"name", "studio_get_state"},
        {"description", "Get detailed Studio editor state (frame, selection, key counts, undo/redo, models, lights)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 18. studio_command
    tools.push_back({
        {"name", "studio_command"},
        {"description", "Run any existing ui-script studio command (e.g. studionew, studioadd, etc.)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"cmd", {
                    {"type", "string"},
                    {"description", "Command name (e.g. studionew, studiosave, etc.)"}
                }},
                {"args", {
                    {"type", "array"},
                    {"items", {{"type", "string"}}},
                    {"description", "Command arguments array"}
                }}
            }},
            {"required", {"cmd"}}
        }}
    });

    // 19. studio_save
    tools.push_back({
        {"name", "studio_save"},
        {"description", "Save current studio project to a .mmdxproj file"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file", {
                    {"type", "string"},
                    {"description", "Target file path"}
                }}
            }},
            {"required", {"file"}}
        }}
    });

    // 20. studio_open
    tools.push_back({
        {"name", "studio_open"},
        {"description", "Open a .mmdxproj project file in Studio"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"file", {
                    {"type", "string"},
                    {"description", "Source file path"}
                }}
            }},
            {"required", {"file"}}
        }}
    });

    // 21. studio_add_model
    tools.push_back({
        {"name", "studio_add_model"},
        {"description", "Add a character, stage, or prop model to Studio"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"kind", {
                    {"type", "string"},
                    {"enum", {"character", "stage", "prop"}},
                    {"description", "Model kind"}
                }},
                {"file", {
                    {"type", "string"},
                    {"description", "Explicit file path"}
                }},
                {"library", {
                    {"type", "string"},
                    {"description", "Library substring match"}
                }}
            }},
            {"required", {"kind"}}
        }}
    });

    // 22. studio_select
    tools.push_back({
        {"name", "studio_select"},
        {"description", "Select model in Studio by index (-1 for none / camera)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"index", {
                    {"type", "integer"},
                    {"description", "Model index"}
                }}
            }},
            {"required", {"index"}}
        }}
    });

    // 23. studio_set_camera
    tools.push_back({
        {"name", "studio_set_camera"},
        {"description", "Set the studio view (target tx/ty/tz, yaw/pitch degrees, distance, fov). Without key: the free view (the motion camera is switched off in the viewport). With key: true: writes the motion camera key at the playhead (inserted first if missing) and views through the motion camera"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"tx", {{"type", "number"}}},
                {"ty", {{"type", "number"}}},
                {"tz", {{"type", "number"}}},
                {"yaw_deg", {{"type", "number"}}},
                {"pitch_deg", {{"type", "number"}}},
                {"dist", {{"type", "number"}}},
                {"fov_deg", {{"type", "number"}}},
                {"key", {{"type", "boolean"}, {"description", "Insert keyframe at current frame"}}}
            }}
        }}
    });

    // 24. studio_set_bone
    tools.push_back({
        {"name", "studio_set_bone"},
        {"description", "Set bone transform on the selected Studio model and optionally insert a keyframe"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"bone", {
                    {"type", "string"},
                    {"description", "Bone name"}
                }},
                {"translate", {
                    {"type", "array"},
                    {"items", {{"type", "number"}}},
                    {"minItems", 3},
                    {"maxItems", 3},
                    {"description", "[x, y, z] translation offset"}
                }},
                {"rotate_deg", {
                    {"type", "array"},
                    {"items", {{"type", "number"}}},
                    {"minItems", 3},
                    {"maxItems", 3},
                    {"description", "[x, y, z] rotation in degrees"}
                }},
                {"key", {
                    {"type", "boolean"},
                    {"description", "Whether to insert a keyframe at the current frame"}
                }}
            }},
            {"required", {"bone"}}
        }}
    });

    // 25. studio_key
    tools.push_back({
        {"name", "studio_key"},
        {"description", "Insert keyframe(s) in Studio at the current frame"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"what", {
                    {"type", "string"},
                    {"enum", {"all", "selected", "camera", "lights"}},
                    {"description", "selected = the selected rows / bones of the selected character, all = every bone of it, camera / lights = their tracks (default: selected)"}
                }}
            }}
        }}
    });

    // 26. undo
    tools.push_back({
        {"name", "undo"},
        {"description", "Undo the last Studio edit action"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 27. redo
    tools.push_back({
        {"name", "redo"},
        {"description", "Redo the last undone Studio action"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 28. render_still
    tools.push_back({
        {"name", "render_still"},
        {"description", "Start offline still image rendering with the GI renderer"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"output", {
                    {"type", "string"},
                    {"description", "Output PNG path"}
                }},
                {"spp", {
                    {"type", "integer"},
                    {"description", "Samples per pixel"}
                }},
                {"width", {
                    {"type", "integer"},
                    {"description", "Image width override"}
                }},
                {"height", {
                    {"type", "integer"},
                    {"description", "Image height override"}
                }}
            }},
            {"required", {"output"}}
        }}
    });

    // 29. render_video
    tools.push_back({
        {"name", "render_video"},
        {"description", "Start offline video render to an MP4 file"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"output", {
                    {"type", "string"},
                    {"description", "Output MP4 path"}
                }},
                {"start", {
                    {"oneOf", json::array({
                        json{{"type", "number"}},
                        json{{"type", "string"}}
                    })},
                    {"description", "Start time: seconds (number) or a timecode string (HH:MM:SS / MM:SS / SS)"}
                }},
                {"end", {
                    {"oneOf", json::array({
                        json{{"type", "number"}},
                        json{{"type", "string"}}
                    })},
                    {"description", "End time: seconds (number) or a timecode string (HH:MM:SS / MM:SS / SS); default: the whole song"}
                }},
                {"renderer", {
                    {"type", "string"},
                    {"enum", {"raster", "rt", "pt", "gi"}},
                    {"description", "Renderer type"}
                }},
                {"fps", {{"type", "integer"}}},
                {"bitrate", {{"type", "integer"}}},
                {"quality", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
                {"spp", {{"type", "integer"}}},
                {"width", {{"type", "integer"}}},
                {"height", {{"type", "integer"}}}
            }},
            {"required", {"output"}}
        }}
    });

    // 30. render_status
    tools.push_back({
        {"name", "render_status"},
        {"description", "Query current offline render status and progress"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 31. render_cancel
    tools.push_back({
        {"name", "render_cancel"},
        {"description", "Cancel the currently active offline render"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 32. ui_input
    tools.push_back({
        {"name", "ui_input"},
        {"description", "Inject UI input events (move, click, dblclick, down, up, wheel, key, text, mod)"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"events", {
                    {"type", "array"},
                    {"items", {
                        {"type", "object"},
                        {"properties", {
                            {"frame", {{"type", "integer"}, {"description", "Frame offset relative to now (0 = current frame)"}}},
                            {"cmd", {{"type", "string"}, {"description", "Input command (move, click, dblclick, down, up, wheel, key, text, mod)"}}},
                            {"args", {{"type", "array"}, {"items", {{"type", "string"}}}}}
                        }},
                        {"required", {"cmd"}}
                    }}
                }}
            }},
            {"required", {"events"}}
        }}
    });

    // 33. quit_app
    tools.push_back({
        {"name", "quit_app"},
        {"description", "Cleanly close and exit MMDX12"},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 34. studio_scene_info
    tools.push_back({
        {"name", "studio_scene_info"},
        {"description", "Read-only snapshot of the active studio project at the current frame: fps (30), end_frame, "
                        "aspect ratio, audio (file / offset_seconds / end_frame or null), every model (index, uid, "
                        "kind, name, file, visible, placement, key counts, key frame range, world-space AABB "
                        "[minx,miny,minz,maxx,maxy,maxz]; characters also height, center bone world position and "
                        "facing_yaw_deg where 0 = facing -Z toward the default MMD camera, +90 = +X), the camera "
                        "(key count, first/last frame, use_motion_camera, current target / yaw / pitch / roll in "
                        "degrees, dist, fov_deg, eye world position) and every scene light with all base and sampled "
                        "values (cone angles in degrees). Units are MMD world units; frames are timeline frames at "
                        "30 fps. Never changes the document."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", json::object()}
        }}
    });

    // 35. studio_sample_motion
    tools.push_back({
        {"name", "studio_sample_motion"},
        {"description", "Read-only world-space bone positions of one character or stage at sampled timeline frames "
                        "(30 fps): {\"frames\": [...], \"bones\": {\"<bone name>\": [[x,y,z], ...]}, "
                        "\"missing_bones\": [...]}. Pose = the bound motion at that frame plus the model's placement "
                        "and scale, without pose-layer edits and with physics disabled, so the result is deterministic. "
                        "Bones are PMX names (Japanese, e.g. 頭); missing ones are skipped and listed. Defaults: "
                        "bones [センター, 上半身, 頭, 左手首, 右手首, 左足首, 右足首], the model's whole key range, "
                        "step 5. At most 4000 samples (frames x bones). The viewport is restored afterwards. Props "
                        "are rejected."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"model", {{"type", "integer"}, {"description", "Model index as in studio_get_state (character or stage)"}}},
                {"bones", {{"type", "array"}, {"items", {{"type", "string"}}},
                           {"description", "PMX bone names (Japanese), default the 7 tracked bones"}}},
                {"start", {{"type", "integer"}, {"description", "First frame (default: the motion's first key)"}}},
                {"end", {{"type", "integer"}, {"description", "Last frame (default: the motion's last key)"}}},
                {"step", {{"type", "integer"}, {"description", "Frame step (default 5, minimum 1)"}}},
                {"physics", {{"type", "boolean"}, {"description", "Ignored: bones are never physics-simulated"}}}
            }},
            {"required", {"model"}}
        }}
    });

    // 36. studio_motion_events
    tools.push_back({
        {"name", "studio_motion_events"},
        {"description", "Read-only derived choreography facts (timeline frames at 30 fps) to motivate camera shots. "
                        "Per character: jumps [{frame, height}], crouches [{frame, depth}], hand_raises "
                        "[{start, end, hand: left|right|both}], turns [{start, end, delta_yaw_deg}], holds "
                        "[{start, end}], travel {start_xz, end_xz, bounds_min_xz, bounds_max_xz, max_speed} and "
                        "energy windows of 15 frames normalised 0..1 over the character. With two or more characters "
                        "it also returns formation (per 30 frames the left-to-right order of characters as seen from "
                        "the default front camera, -Z looking +Z) and crossovers [{frame, a, b}]. Heights are in MMD "
                        "world units. Never changes the document."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"model", {{"type", "integer"}, {"description", "Character index; default: every character"}}},
                {"start", {{"type", "integer"}, {"description", "First frame (default: the motion's first key)"}}},
                {"end", {{"type", "integer"}, {"description", "Last frame (default: the motion's last key)"}}},
                {"step", {{"type", "integer"}, {"description", "Sampling step in frames (default 2, minimum 1)"}}}
            }}
        }}
    });

    // 37. studio_frame_check
    tools.push_back({
        {"name", "studio_frame_check"},
        {"description", "Read-only framing evaluation at timeline frames (30 fps) using the project's motion camera "
                        "(or one camera override for every frame). Per visible character: screen bbox [x0,y0,x1,y1] "
                        "normalised 0..1 with y down, on_screen (fraction of sampled points inside), head_in_frame, "
                        "cut (list of the sampled points outside: head, head_top, center, upper_body, left_hand, "
                        "right_hand, left_foot, right_foot), screen_height (fraction of the frame height), center "
                        "screen point, distance eye->center, facing_camera_deg (0 = looking into the lens). Also per "
                        "frame: eye, fov_deg, aspect and shot_size from the largest character (extreme_close > 1.6, "
                        "close > 1.0, medium > 0.55, full > 0.3, else wide). FOV is vertical, D3D left-handed "
                        "perspective, and the project's aspect ratio is used. Poses are sampled without physics and "
                        "the viewport is restored. Never changes the document."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"frames", {{"type", "array"}, {"items", {{"type", "integer"}}},
                            {"description", "Timeline frames to evaluate"}}},
                {"camera", {
                    {"type", "object"},
                    {"description", "Optional camera override for every frame (instead of the motion camera): orbit "
                                    "style like studio_set_camera (yaw 0 / pitch 0 looks along -Z at the target)"},
                    {"properties", {
                        {"target", {{"type", "array"}, {"items", {{"type", "number"}}},
                                    {"description", "World target [x,y,z]"}}},
                        {"yaw_deg", {{"type", "number"}, {"description", "Yaw in degrees"}}},
                        {"pitch_deg", {{"type", "number"}, {"description", "Pitch in degrees"}}},
                        {"roll_deg", {{"type", "number"}, {"description", "Roll in degrees"}}},
                        {"dist", {{"type", "number"}, {"description", "Distance from the target (positive)"}}},
                        {"fov_deg", {{"type", "number"}, {"description", "Vertical FOV in degrees (default 30)"}}}
                    }}
                }}
            }},
            {"required", {"frames"}}
        }}
    });

    // 38. studio_audio_analysis
    tools.push_back({
        {"name", "studio_audio_analysis"},
        {"description", "Read-only music structure analysis of the project's audio: bpm, confidence, frames_per_beat, "
                        "beats, downbeats, beat_energy, onsets plus onset_strength, sections [{start, end, level, "
                        "energy}] and audio_offset_seconds. All times are timeline frames at 30 fps (frame = (seconds "
                        "+ audio_offset) * 30, rounded to 0.01). start/end filter the returned frames. The first call "
                        "may take about a second; the result is cached by audio path and file write time. Fails when "
                        "the project has no audio."},
        {"inputSchema", {
            {"type", "object"},
            {"properties", {
                {"start", {{"type", "integer"}, {"description", "First timeline frame to return (default: all)"}}},
                {"end", {{"type", "integer"}, {"description", "Last timeline frame to return (default: all)"}}}
            }}
        }}
    });

    // 34..39. Bulk key tools (AppMcpKeys.cpp). The descriptions carry the conventions: the AI only reads these.
    {
        const json curveDesc =
            "Easing INTO this key from the previous key (VMD semantics). Preset name: linear (20,20,107,107), "
            "ease_in_out (64,0,64,127), ease_in (64,0,127,127), ease_out (0,0,64,127), smooth (42,0,86,127), "
            "fast_start (0,64,64,127), slow_start (64,0,107,64); [x1,y1,x2,y2] integers 0..127 for every "
            "parameter/channel; or an object per parameter/channel, each a preset name or [4] (missing = linear).";
        const json curveSchema = {
            {"description", curveDesc},
            {"oneOf", json::array({
                {{"type", "string"}, {"enum", {"linear", "ease_in_out", "ease_in", "ease_out", "smooth", "fast_start", "slow_start"}}},
                {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 127}}}, {"minItems", 4}, {"maxItems", 4}},
                {{"type", "object"}}
            })}
        };
        const json vec3 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
        const json modeSchema = {
            {"type", "string"},
            {"enum", {"merge", "replace_range", "replace_all"}},
            {"description", "merge (default): upsert by frame. replace_range: delete the existing keys in [min frame, "
                            "max frame] of the supplied keys first. replace_all: delete every key of the track(s) first."}
        };

        tools.push_back({
            {"name", "studio_get_camera_keys"},
            {"description",
             "Read the motion camera key list as JSON. Frame range {start,end} optional (inclusive). Each key: frame, "
             "target [x,y,z], yaw_deg, pitch_deg, roll_deg, dist, fov_deg (1..125), perspective, curve (object form: "
             "x/y/z/rotation/distance/fov, each a preset name or [x1,y1,x2,y2]), eye [x,y,z] (CameraMotion::ToView). "
             "Convention: CameraKf rotation.x = -pitch, rotation.y = yaw, rotation.z = roll (radians internally), "
             "distance = -dist (dist > 0 = the eye in front of the target, as studio_set_camera's dist). 'cuts' lists "
             "frames f where keys exist at f and f+1 (MMD camera cut convention)."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"start", {{"type", "integer"}, {"description", "First frame (default 0)"}}},
                    {"end", {{"type", "integer"}, {"description", "Last frame (default: no limit)"}}}
                }}
            }}
        });

        tools.push_back({
            {"name", "studio_set_camera_keys"},
            {"description",
             "Write motion camera keys (max 20000). Each key needs frame; every other field is optional and a missing "
             "field takes the value the existing track evaluates to at that frame (the default CameraKf when empty). "
             "Fields: target [x,y,z], yaw_deg, pitch_deg, roll_deg, dist (dist > 0 = the eye in front of the target, "
             "stored as distance = -dist), fov_deg (clamped 1..125), perspective, curve. Curve = the easing INTO the key "
             "from the previous key: preset name, [x1,y1,x2,y2] (0..127), or an object {x,y,z,rotation,distance,fov}. "
             "mode merge (default) / replace_range / replace_all. A camera cut is two keys on adjacent frames (frame f, "
             "then frame f+1 with the new pose); there is no separate cut flag. Switches the viewport to the motion "
             "camera. One undo step."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"keys", {
                        {"type", "array"},
                        {"minItems", 1},
                        {"items", {
                            {"type", "object"},
                            {"properties", {
                                {"frame", {{"type", "integer"}, {"minimum", 0}}},
                                {"target", vec3},
                                {"yaw_deg", {{"type", "number"}}},
                                {"pitch_deg", {{"type", "number"}}},
                                {"roll_deg", {{"type", "number"}}},
                                {"dist", {{"type", "number"}}},
                                {"fov_deg", {{"type", "number"}, {"minimum", 1}, {"maximum", 125}}},
                                {"perspective", {{"type", "boolean"}}},
                                {"curve", curveSchema}
                            }},
                            {"required", {"frame"}}
                        }}
                    }},
                    {"mode", modeSchema}
                }},
                {"required", {"keys"}}
            }}
        });

        tools.push_back({
            {"name", "studio_set_light"},
            {"description",
             "Create, edit or delete a Studio scene light, optionally writing its whole key list, as ONE undo step. "
             "Identify an existing light with index; create one with create (sun|point|spot|area|ambient) plus optional "
             "name; delete one with delete: true (and index). At most one sun and one ambient are allowed. props (base, "
             "non-keyed fields): name, enabled, vmd_link, rim_strength, rim_color [r,g,b], aim_mode manual|target|sway, "
             "target_model (model index, -1 = performer), target_part centre|head, sway_phase, sky_zenith, sky_horizon, "
             "ground_color, shadow nocast|hard|soft, shadow_softness, shadow_density, shadow_color, falloff "
             "none|linear|inverse_square, affect_diffuse, affect_specular, viewport_visible. base holds the keyable "
             "values: position [x,y,z], aim [x,y,z], direction [x,y,z], color [r,g,b], intensity, range, "
             "cone_outer_deg, cone_inner_deg, size [w,h]. keys: [{frame, ...any keyable value}]; a missing value takes "
             "SampleLightValues of the light at that frame. Cone inner is clamped to <= outer. mode merge / "
             "replace_range / replace_all applies to the keys only. Answers index and uid."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"index", {{"type", "integer"}, {"minimum", 0}, {"description", "Existing light index"}}},
                    {"create", {{"type", "string"}, {"enum", {"sun", "point", "spot", "area", "ambient"}}}},
                    {"name", {{"type", "string"}, {"description", "Name for a newly created light"}}},
                    {"delete", {{"type", "boolean"}, {"description", "Remove the light at index"}}},
                    {"props", {{"type", "object"}, {"description", "Non-keyed light fields"}}},
                    {"base", {{"type", "object"}, {"description", "Keyable base values"}}},
                    {"keys", {
                        {"type", "array"},
                        {"items", {
                            {"type", "object"},
                            {"properties", {{"frame", {{"type", "integer"}, {"minimum", 0}}}}},
                            {"required", {"frame"}}
                        }}
                    }},
                    {"mode", modeSchema}
                }}
            }}
        });

        tools.push_back({
            {"name", "studio_light_rig"},
            {"description",
             "Create a whole Studio lighting rig in ONE undo step (\"AI: studio_light_rig\"). rig: key_fill_rim "
             "(key spot front 35 deg to one side, soft fill area other side, rim spot behind-above) | front_wash "
             "(count area lights in an arc in front, default 3) | top_spots (count spots above/in front, default "
             "6; aim subject (default) or down) | side_bars (two columns of count point lights left/right, default "
             "4) | back_beams (count spots behind aimed up/forward, sway with spread phases, default 4). subject = "
             "model index of the character (default: the first character); its centre / height come from the "
             "current frame, and positions scale with height (1.0 height unit = the character's height). Common: "
             "radius, height (both in character-height units), color [r,g,b] (linear RGB 0..1+) or palette "
             "[[r,g,b]...] cycled per light, intensity, cone_deg (spot outer degrees; inner = 0.7 x outer), size "
             "[w,h] (area), shadow nocast|hard|soft (default: key light hard, others nocast), name_prefix "
             "(default \"AI \"), replace (delete the lights a previous rig call with the same name_prefix "
             "created). Never touches the sun or ambient. Answers lights [{index, uid, name, kind, position, "
             "aim}] and undo."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"rig", {{"type", "string"},
                             {"enum", {"key_fill_rim", "front_wash", "top_spots", "side_bars", "back_beams"}}}},
                    {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}}},
                    {"subject", {{"type", "integer"}, {"minimum", 0},
                                 {"description", "Character model index (default: the first character)"}}},
                    {"radius", {{"type", "number"}, {"description", "In character heights (default 1.6)"}}},
                    {"height", {{"type", "number"}, {"description", "In character heights (default 1.6)"}}},
                    {"color", vec3},
                    {"palette", {{"type", "array"}, {"items", vec3}}},
                    {"intensity", {{"type", "number"}}},
                    {"cone_deg", {{"type", "number"}, {"minimum", 0}, {"maximum", 180}}},
                    {"size", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2},
                              {"maxItems", 2}}},
                    {"shadow", {{"type", "string"}, {"enum", {"nocast", "hard", "soft"}}}},
                    {"name_prefix", {{"type", "string"}}},
                    {"replace", {{"type", "boolean"}}},
                    {"aim", {{"type", "string"}, {"enum", {"subject", "down"}}}}
                }},
                {"required", {"rig"}}
            }}
        });

        tools.push_back({
            {"name", "studio_get_bone_keys"},
            {"description",
             "Read a model's bone (and optionally morph) keys as JSON. model = model index. bones: optional list of "
             "bone names (default: every bone with keys; unknown names are rejected). start/end optional inclusive "
             "frame range. Each bone key: frame, translate [x,y,z], rotate_deg [x,y,z] Euler XYZ degrees in "
             "studio_set_bone's convention (XMQuaternionRotationRollPitchYaw), quat [x,y,z,w] (lossless), curve "
             "(object form x/y/z/rotation, each a preset name or [x1,y1,x2,y2]). morphs: true adds morphs {name: "
             "[{frame, weight}]}. Output capped at 20000 keys with truncated: true."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"model", {{"type", "integer"}, {"minimum", 0}}},
                    {"bones", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"start", {{"type", "integer"}}},
                    {"end", {{"type", "integer"}}},
                    {"morphs", {{"type", "boolean"}, {"description", "Also return morph keys"}}}
                }},
                {"required", {"model"}}
            }}
        });

        tools.push_back({
            {"name", "studio_light_cues"},
            {"description",
             "Generate keyed light animation from the song's beats / sections in ONE undo step (\"AI: "
             "studio_light_cues\"). Every cue: lights (light indices or spots / points / areas / all = every "
             "non-sun non-ambient light), start / end (timeline frames at 30 fps), pattern pulse | chase | "
             "alternate | strobe | sweep | color_cycle | breathe | fade | section_levels | blackout, timing beat "
             "(default) | half | downbeat | bar | onset | section, every (use every Nth event, default 1), offset "
             "(event phase shift), min_strength (onset, default 0.5), base / peak intensity (default: the light's "
             "current intensity x0.3 / x1.0), decay (frames, default 0.6 x beat), colors [[r,g,b] linear 0..1+], "
             "aim_points [[x,y,z] world] or aim_targets [head, center, left, right, audience], from / to colour "
             "(fade), mode replace_range (default: delete each light's keys inside [start,end] first) | merge. "
             "Key values not driven by the pattern keep the light's sampled value at that frame. Events are "
             "rounded to integer frames and duplicates dropped; a timing that finds no events rejects the call. "
             "Answers cues [{pattern, lights, events, keys_written}], undo and bpm."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"cues", {
                        {"type", "array"},
                        {"minItems", 1},
                        {"items", {
                            {"type", "object"},
                            {"properties", {
                                {"lights", {
                                    {"description", "Light indices, or spots / points / areas / all"},
                                    {"oneOf", json::array({
                                        {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}}},
                                        {{"type", "string"}, {"enum", {"spots", "points", "areas", "all"}}}
                                    })}
                                }},
                                {"start", {{"type", "integer"}, {"minimum", 0}}},
                                {"end", {{"type", "integer"}, {"minimum", 0}}},
                                {"pattern", {{"type", "string"},
                                             {"enum", {"pulse", "chase", "alternate", "strobe", "sweep",
                                                       "color_cycle", "breathe", "fade", "section_levels",
                                                       "blackout"}}}},
                                {"timing", {{"type", "string"},
                                            {"enum", {"beat", "half", "downbeat", "bar", "onset", "section"}}}},
                                {"every", {{"type", "integer"}, {"minimum", 1}}},
                                {"offset", {{"type", "integer"}}},
                                {"min_strength", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                                {"base", {{"type", "number"}}},
                                {"peak", {{"type", "number"}}},
                                {"decay", {{"type", "integer"}, {"minimum", 0}}},
                                {"colors", {{"type", "array"}, {"items", vec3}}},
                                {"aim_points", {{"type", "array"}, {"items", vec3}}},
                                {"aim_targets", {{"type", "array"},
                                                 {"items", {{"type", "string"},
                                                            {"enum", {"head", "center", "left", "right",
                                                                      "audience"}}}}}},
                                {"from", vec3},
                                {"to", vec3},
                                {"mode", {{"type", "string"}, {"enum", {"replace_range", "merge"}}}}
                            }},
                            {"required", {"lights", "start", "end", "pattern"}}
                        }}
                    }}
                }},
                {"required", {"cues"}}
            }}
        });

        tools.push_back({
            {"name", "studio_set_bone_keys"},
            {"description",
             "Write bone and/or morph keys of a model (max 20000), ONE undo step over every track touched. model = "
             "model index. bones: {name: [{frame, translate?, rotate_deg? | quat?, curve?}]}; values are VMD-local "
             "like studio_set_bone. A missing translate / rotation takes the bone's evaluated value at that frame "
             "(interpolated from the current track; zero / identity when it has no keys). rotate_deg is [x,y,z] Euler "
             "degrees (XMQuaternionRotationRollPitchYaw(x,y,z)); quat is [x,y,z,w]; give one of them. Curve = easing "
             "into the key: preset, [x1,y1,x2,y2] (0..127), or {x,y,z,rotation}. morphs: {name: [{frame, weight}]}. "
             "Bones and morphs must exist in the model's PMX (unknown names are rejected with the list). mode merge / "
             "replace_range / replace_all applies per listed track. Bumps the model's motion version."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"model", {{"type", "integer"}, {"minimum", 0}}},
                    {"bones", {{"type", "object"}, {"description", "{bone name: [key...]}"}}},
                    {"morphs", {{"type", "object"}, {"description", "{morph name: [{frame, weight}]}"}}},
                    {"mode", modeSchema}
                }},
                {"required", {"model"}}
            }}
        });

        tools.push_back({
            {"name", "studio_delete_keys"},
            {"description",
             "Delete keys in the inclusive frame range [start, end], one undo step. target: camera (no model), light "
             "(needs index), bone (needs model, optional bones list; default every bone with keys), morph (needs "
             "model, optional morphs list; default every morph with keys). Answers changed_keys and undo."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"target", {{"type", "string"}, {"enum", {"camera", "light", "bone", "morph"}}}},
                    {"index", {{"type", "integer"}, {"minimum", 0}, {"description", "target=light: light index"}}},
                    {"model", {{"type", "integer"}, {"minimum", 0}, {"description", "target=bone/morph: model index"}}},
                    {"bones", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"morphs", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                    {"start", {{"type", "integer"}, {"minimum", 0}}},
                    {"end", {{"type", "integer"}, {"minimum", 0}}}
                }},
                {"required", {"target", "start", "end"}}
            }}
        });
    }

    // 39. studio_camera_solve / studio_camera_shots (AppMcpCamera.cpp): AI camera direction.
    {
        const json vec3 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}};
        const json vec2 = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 2}, {"maxItems", 2}};
        const json subjectSchema = {
            {"type", "object"},
            {"description", "What the shot is about"},
            {"properties", {
                {"models", {{"type", "array"}, {"items", {{"type", "integer"}}},
                            {"description", "Model indices (studio_get_state). Default: the first visible character. Several models are fitted as one group in width and height"}}},
                {"focus", {{"type", "string"}, {"enum", {"face", "upper", "body"}},
                           {"description", "What the shot is built around: face (head/eyes), upper (upper body) or body (centre). Default follows the shot size (face for ecu/cu/mcu)"}}}
            }}
        };
        const json angleSchema = {
            {"type", "object"},
            {"description", "Camera angle. yaw_deg is relative to the subject's facing (0 = in front, looking at the face); +90 puts the camera at the subject's LEFT (-X while the subject faces -Z), negative is the right. absolute: true makes yaw_deg a world yaw like studio_set_camera. pitch_deg: + is above looking down. roll_deg: dutch angle"},
            {"properties", {
                {"yaw_deg", {{"type", "number"}, {"description", "Relative yaw (default 0 = front); ignored with absolute"}}},
                {"absolute", {{"type", "boolean"}, {"description", "yaw_deg is a world yaw instead of relative to the subject"}}},
                {"pitch_deg", {{"type", "number"}, {"description", "+ = camera above looking down (default 0)"}}},
                {"roll_deg", {{"type", "number"}, {"description", "Dutch roll in degrees (default 0)"}}}
            }}
        };
        const json screenSchema = {
            {"type", "object"},
            {"description", "Where the subject anchor lands, normalised 0..1 with y down (default x 0.5, y 0.38 for face-led shots else 0.5)"},
            {"properties", {
                {"x", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
                {"y", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}
            }}
        };
        const json shotEnum = json::array({"ecu", "cu", "mcu", "ms", "cowboy", "full", "wide"});

        tools.push_back({
            {"name", "studio_camera_solve"},
            {"description",
             "Framing solver: computes WHERE the camera goes for a requested shot, so the AI only decides WHAT. Samples "
             "the subject bones at 'frame' (no physics, viewport restored), fits the shot size from the projected body "
             "span (head top = head + 0.08 x height), places the anchor at 'screen' and iterates the distance until the "
             "projected span matches. Shot sizes (fraction of body height): ecu 0.10 around the eyes, cu 0.22, mcu 0.32, "
             "ms 0.50 (waist), cowboy 0.70, full 1.15, wide 2.20. Alternatively 'fill' = the subject span as a fraction "
             "of the frame height. Several models: the union is fitted in BOTH width and height (project aspect); 'shot' "
             "then only adds margin for full/wide. fov_deg default 25 (MMD stores integers; the solve compensates with "
             "distance). Optional 'lead' (0..0.2) adds look room: the subject shifts opposite its facing. Returns "
             "{camera: {target, yaw_deg, pitch_deg, roll_deg, dist, fov_deg, eye} - exactly studio_set_camera_keys' key "
             "fields - and check: one studio_frame_check frame. Never writes."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"frame", {{"type", "integer"}, {"minimum", 0}, {"description", "Timeline frame (30 fps)"}}},
                    {"subject", subjectSchema},
                    {"shot", {{"type", "string"}, {"enum", shotEnum}, {"description", "Shot size (default ms)"}}},
                    {"fill", {{"type", "number"}, {"description", "Subject span as a fraction of frame height (overrides shot)"}}},
                    {"angle", angleSchema},
                    {"fov_deg", {{"type", "number"}, {"minimum", 1}, {"maximum", 125}, {"description", "Vertical FOV (default 25)"}}},
                    {"screen", screenSchema},
                    {"lead", {{"type", "number"}, {"minimum", 0}, {"maximum", 0.2}, {"description", "Look-room offset in the subject's facing direction (default 0)"}}}
                }},
                {"required", {"frame"}}
            }}
        });

        const json accentSchema = {
            {"type", "object"},
            {"description", "Beat accent inside the shot"},
            {"properties", {
                {"kind", {{"type", "string"}, {"enum", {"push", "zoom", "roll"}}, {"description", "push = dist, zoom = fov, roll = roll (default push)"}}},
                {"amount", {{"type", "number"}, {"description", "push: fraction of dist (default 0.12); zoom: fov degrees (default 4); roll: degrees (default 6)"}}},
                {"attack", {{"type", "integer"}, {"minimum", 1}, {"description", "Frames from the pre value to the peak (default 3)"}}},
                {"release", {{"type", "integer"}, {"minimum", 1}, {"description", "Frames from the peak back to the settle (default 10)"}}}
            }}
        };
        const json shotSchema = {
            {"type", "object"},
            {"properties", {
                {"start", {{"type", "integer"}, {"minimum", 0}, {"description", "First frame (inclusive)"}}},
                {"end", {{"type", "integer"}, {"minimum", 0}, {"description", "Last frame (inclusive)"}}},
                {"subject", subjectSchema},
                {"shot", {{"type", "string"}, {"enum", shotEnum}, {"description", "Start shot size"}}},
                {"end_shot", {{"type", "string"}, {"enum", shotEnum}, {"description", "End shot size (dolly; default one size tighter / wider)"}}},
                {"angle", angleSchema},
                {"end_angle", angleSchema},
                {"fov_deg", {{"type", "number"}, {"minimum", 1}, {"maximum", 125}}},
                {"end_fov_deg", {{"type", "number"}, {"minimum", 1}, {"maximum", 125}}},
                {"screen", screenSchema},
                {"end_screen", screenSchema},
                {"move", {{"type", "string"},
                          {"enum", {"static", "dolly_in", "dolly_out", "zoom_in", "zoom_out", "dolly_zoom", "crane_up",
                                    "crane_down", "truck_left", "truck_right", "arc_left", "arc_right", "follow",
                                    "push_accent"}},
                          {"description", "static holds the framing; dolly in/out changes dist; zoom in/out changes fov with the position fixed; dolly_zoom changes both (vertigo); crane up/down changes pitch and target height; truck left/right slides along the camera right axis (parallax, heading fixed); arc left/right orbits around the subject (amount degrees, default 35, keys every ~15 frames); follow re-solves the framing on the moving subject; push_accent is a static shot driven by accents"}}},
                {"amount", {{"type", "number"}, {"description", "Move amount: crane degrees (default 25), truck world units (default 0.6 x height), arc degrees (default 35)"}}},
                {"curve", {{"type", "string"}, {"description", "Easing between this shot's keys (preset name, default ease_in_out)"}}},
                {"follow", {{"type", "boolean"}, {"description", "Re-solve the framing on the moving subject (default false)"}}},
                {"follow_step", {{"type", "integer"}, {"minimum", 1}, {"description", "Follow key step in frames (default 10)"}}},
                {"accents", {{"type", "array"}, {"items", {{"type", "integer"}}},
                             {"description", "Beat frames inside the shot for accent bumps"}}},
                {"accent", accentSchema},
                {"focus", {{"type", "object"}, {"description", "Optional DoF focus key at start on the subject's head"},
                           {"properties", {{"aperture", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"description", "Focus aperture scale (default 1)"}}}}}}}
            }},
            {"required", {"start", "end"}}
        };

        tools.push_back({
            {"name", "studio_camera_shots"},
            {"description",
             "Shot / sequence generator: writes motion-camera keys for a list of shots in ONE undo step (unless "
             "dry_run). Every shot covers [start, end] inclusive and writes keys only inside its range. Overlapping "
             "shots are rejected. Consecutive shots with end+1 == next start form a HARD CUT (MMD: keys on adjacent "
             "frames); there is no cut flag. mode replace_range (default) deletes existing keys inside each shot first; "
             "merge upserts. Each shot solves its start framing (same rules as studio_camera_solve), then computes the "
             "end pose from 'move'. 'curve' is the easing between the shot's keys (preset name, default ease_in_out). "
             "follow: true re-solves the framing every follow_step frames so a travelling dancer stays framed. accents "
             "adds push / zoom / roll bumps at beat frames (attack into the peak with [0,64,64,127], release with "
             "ease_out); accents closer than attack+release to each other or to the shot ends are skipped. Yaw is "
             "unwrapped so consecutive keys never jump more than 180 degrees. focus: {aperture} writes a DoF focus key "
             "at start targeting the subject's head (studiofocus; a separate undo step). Returns shots [{start, end, "
             "keys_written, first_check, last_check}], 'keys' with dry_run, and 'undo'."},
            {"inputSchema", {
                {"type", "object"},
                {"properties", {
                    {"shots", {{"type", "array"}, {"items", shotSchema}, {"minItems", 1}}},
                    {"mode", {{"type", "string"}, {"enum", {"replace_range", "merge"}},
                              {"description", "replace_range (default): delete keys inside each shot; merge: upsert only"}}},
                    {"dry_run", {{"type", "boolean"}, {"description", "Return the keys and write nothing (default false)"}}}
                }},
                {"required", {"shots"}}
            }}
        });
    }

    // Tools that leave the current screen: an open studio project with unsaved changes is kept unless the agent says so.
    for (auto& t : tools) {
        const std::string n = t["name"];
        if (n == "set_screen" || n == "load_scene" || n == "open_studio" || n == "studio_open" || n == "studio_command")
            t["inputSchema"]["properties"]["discard_unsaved"] = {
                {"type", "boolean"},
                {"description", "Leave a studio project with unsaved changes anyway (default false: the call fails instead)"}};
        if (n == "render_still" || n == "render_video") {
            t["description"] = t["description"].get<std::string>() +
                               ". Returns once the render has started (the app keeps running); poll render_status. "
                               "Without 'output' the file goes to the app's usual render folder.";
            t["inputSchema"].erase("required");
        }
    }
    return tools;
}

// How long the app may take to answer a tool call (the app's worker waits this long for the main thread; the
// bridge waits a little longer so the app's own timeout error arrives first).
inline unsigned McpToolTimeoutMs(const std::string& tool) {
    if (tool == "load_scene" || tool == "open_studio" || tool == "studio_open" || tool == "studio_add_model" ||
        tool == "studio_command")
        return 180000;
    if (tool == "wait_frames" || tool == "ui_input" || tool == "ui_click") return 120000;
    if (tool == "studio_audio_analysis") return 60000;
    return 30000;
}

} // namespace mmdx
