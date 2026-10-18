# Tool recipes

Exact JSON calls for the camera director. Every argument here matches `src/app/McpTools.h`; do not invent fields.
`studio_contact_sheet` and `studio_singing` follow the contract documented in the skill (new tools).

## Read the scene and the song

```json
{"name": "studio_get_state", "arguments": {}}
{"name": "studio_scene_info", "arguments": {}}
{"name": "studio_get_camera_keys", "arguments": {"start": 0, "end": 3600}}
{"name": "studio_audio_analysis", "arguments": {"start": 0, "end": 3600}}
```

`studio_scene_info` gives fps (30), `end_frame`, aspect, audio, models (index, height, `facing_yaw_deg`, AABB) and
existing camera/lights. If the user already has camera keys, read them and decide whether you extend, replace a
range or leave them alone.

## Who sings when

```json
{"name": "studio_singing", "arguments": {"models": [0, 1, 2], "start": 0, "end": 3600, "threshold": 0.15}}
```

Answer: per character `phrases [{start, end, mean_open, syllables}]`, `breaths [{start, end}]`,
`held_notes [{start, end, vowel}]`, `has_lipsync`, `share`; and `timeline [{start, end, singers: [model indices]}]`
(empty singers = instrumental). Read the phrase starts and breaths as cut candidates, and the held notes as places
for sustained moves. Omit `models` for every character; `threshold` tunes the open-mouth test.

## The dance map

```json
{"name": "studio_motion_events", "arguments": {"step": 2}}
{"name": "studio_sample_motion", "arguments": {"model": 0, "bones": ["頭", "左手首"], "start": 0, "end": 300, "step": 10}}
```

Use `studio_sample_motion` only when one shot depends on a specific body point the events do not describe. Bones are
PMX names (Japanese). Frames are 30 fps timeline frames.

## Look at the work — contact sheet

```json
{"name": "studio_contact_sheet", "arguments": {"frames": [0, 60, 120, 180, 240, 300], "columns": 3, "tile_width": 320, "settle_frames": 4, "return_image": true}}
```

```json
{"name": "studio_contact_sheet", "arguments": {"start": 240, "end": 359, "step": 6, "columns": 4, "tile_width": 320, "settle_frames": 4, "path": "C:/tmp/chorus_sheet.png"}}
```

```json
{"name": "studio_contact_sheet", "arguments": {"start": 720, "end": 839, "count": 12, "columns": 4, "tile_width": 320, "settle_frames": 4, "path": "C:/tmp/signature_move.png", "physics": false}}
```

One grid image of rendered frames through the motion camera; each tile is labelled with its frame number and time.
Choose frames explicitly, or a range with `step` or `count`. Max 48 tiles. `tiles: [{index, frame, time, x, y, w,
h}]` tells you which tile is which. `path` saves it; `return_image` also returns it inline. `physics` defaults on;
settle_frames lets the pose settle before each capture.

## Solve one framing

```json
{"name": "studio_camera_solve", "arguments": {"frame": 120, "subject": {"models": [0], "focus": "face"}, "shot": "cu", "angle": {"yaw_deg": 20, "pitch_deg": 5}, "roll_deg": 0, "fov_deg": 18, "lead": 0.08}}
```

Returns `camera: {target, yaw_deg, pitch_deg, roll_deg, dist, fov_deg, eye}` and `check` (one frame check). It never
writes. `angle.yaw_deg` is relative to the subject's facing: 0 = in front of the face, +90 = the subject's left,
-90 = the right, 180 = behind; `absolute: true` uses world yaw. `pitch_deg` > 0 = camera above looking down.
Several models are fitted as a union in width and height. `shot` sizes are `ecu`..`wide`; `fill` sets the span
directly in `studio_camera_solve` only.

## Write a shot list

```json
{"name": "studio_camera_shots", "arguments": {"dry_run": true, "mode": "replace_range", "shots": [
  {"start": 0, "end": 119, "subject": {"models": [0]}, "shot": "ms", "angle": {"yaw_deg": 18, "pitch_deg": 5}, "move": "dolly_in", "curve": "smooth"},
  {"start": 120, "end": 239, "subject": {"models": [0], "focus": "face"}, "shot": "cu", "angle": {"yaw_deg": -12}, "move": "static", "curve": "ease_in_out", "focus": {"aperture": 0.75}}
]}}
```

Run `dry_run` first on a long film and inspect the returned keys. Then the same call without `dry_run` (one undo
step, `AI: studio_camera_shots`).

```json
{"name": "studio_camera_shots", "arguments": {"mode": "replace_range", "shots": [
  {"start": 240, "end": 359, "subject": {"models": [0, 1, 2, 3, 4]}, "shot": "full", "angle": {"yaw_deg": 5, "pitch_deg": 8}, "move": "crane_up", "amount": 22, "curve": "ease_in_out"},
  {"start": 360, "end": 419, "subject": {"models": [1]}, "shot": "ms", "move": "push_accent", "accents": [366, 380, 394, 408], "accent": {"kind": "push", "amount": 0.12, "attack": 3, "release": 10}, "curve": "ease_in_out"},
  {"start": 420, "end": 539, "subject": {"models": [0]}, "shot": "mcu", "move": "follow", "follow": true, "follow_step": 10, "curve": "smooth"}
]}}
```

`start`/`end` are inclusive; `end + 1 == next start` is a hard cut. `shot` + `end_shot` for a size change,
`angle` + `end_angle` for an angle change, `fov_deg` + `end_fov_deg` for a lens move, `follow`/`follow_step` for a
travelling dancer, `accents` + `accent` for beat bumps. Each shot answer lists `warnings` (head out of frame /
subject behind the camera at the start, quarters or end) and `first_check` / `last_check`; fix those shots.
Overlapping shots are rejected.

## Hand-author a signature shot

```json
{"name": "studio_camera_solve", "arguments": {"frame": 900, "subject": {"models": [0]}, "shot": "mcu", "angle": {"yaw_deg": 25, "pitch_deg": 4}, "fov_deg": 16}}
```

Take the returned `camera` fields as the start key, solve a second and third waypoint, then write the path with
`studio_set_camera_keys`:

```json
{"name": "studio_set_camera_keys", "arguments": {"mode": "replace_range", "keys": [
  {"frame": 900, "target": [0, 0.9, 0], "yaw_deg": 25, "pitch_deg": 4, "roll_deg": 0, "dist": 4.2, "fov_deg": 16, "curve": "ease_in_out"},
  {"frame": 940, "target": [0, 1.05, 0], "yaw_deg": 30, "pitch_deg": 7, "roll_deg": 2, "dist": 3.4, "fov_deg": 16},
  {"frame": 980, "target": [0, 1.1, 0], "yaw_deg": 36, "pitch_deg": 9, "roll_deg": 3, "dist": 2.8, "fov_deg": 16, "curve": "smooth"}
]}}
```

`curve` is the easing INTO the key from the previous key. A camera cut is two keys on adjacent frames. `dist` >
0 = the eye in front of the target (stored as negative distance), `fov_deg` is clamped 1..125. One undo step.
Use this only where the shot generator cannot express the idea.

## Inspect the technical result

```json
{"name": "studio_frame_check", "arguments": {"frames": [0, 60, 119, 120, 180, 239]}}
```

Per frame and character: `head_in_frame`, `cut` (head, head_top, center, upper_body, left_hand, right_hand,
left_foot, right_foot), `shot_size`, `facing_camera_deg`, `on_screen`, `behind_camera`. This is a safety net, not
the judgement.

## Look at a frame directly

```json
{"name": "seek", "arguments": {"seconds": 4.0}}
{"name": "wait_frames", "arguments": {"n": 15}}
{"name": "screenshot", "arguments": {"scene_only": true}}
```

`seek` takes seconds. Use contact sheets for overviews and a screenshot when you need the full-resolution detail.

## Undo, and set a temporary neutral view

```json
{"name": "undo", "arguments": {}}
{"name": "studio_set_camera_keys", "arguments": {"mode": "replace_range", "keys": [
  {"frame": 0, "target": [0, 1.0, 0], "yaw_deg": 0, "pitch_deg": 8, "dist": 12, "fov_deg": 30},
  {"frame": 3600, "target": [0, 1.0, 0], "yaw_deg": 0, "pitch_deg": 8, "dist": 12, "fov_deg": 30}
]}}
```

`undo` rolls back exactly the last AI write. A temporary wide key is one undo step; check `studio_get_camera_keys`
first and remove it after the observation if the user had no keys.
