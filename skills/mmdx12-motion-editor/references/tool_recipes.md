# Tool recipes

Exact JSON for every call this skill uses. The existing tools match `src/app/McpTools.h`; the newer motion ops follow
the contract in the spec (`ease`, `overlap`, `spring`, `breathe`, `studio_motion_quality`). Send these to the MCP
bridge as `{"name": ..., "arguments": ...}`. `dry_run` first on a new call.

## Observe

```json
{"name": "studio_get_state", "arguments": {}}
{"name": "studio_scene_info", "arguments": {}}
```

`studio_scene_info`: each model's index, kind, height, facing_yaw_deg, key counts and key frame range; the camera's
key range and `use_motion_camera`. The target must be a character.

```json
{"name": "studio_motion_events", "arguments": {"model": 0, "step": 2}}
```

Jumps, crouches, hand raises, turns, holds, travel, energy, formation and crossovers. `holds` are the ranges where the
dance is still — candidates for `breathe`.

```json
{"name": "studio_motion_quality", "arguments": {"model": 0, "start": 240, "end": 720, "window": 60}}
```

Per group (head, torso, center, arm_l, arm_r, hand_l, hand_r, legs): `linear_ratio`, `hold_ratio` + `holds`,
`hard_stops`, `jerk`, `stiffness` (0..100). Across groups: `sync_stops [{frame, groups}]`, `chain_lag` (frames per
chain; ~0 rigid, 1-3 natural), `overall`, `worst_ranges [{start, end, score, reasons}]`.

```json
{"name": "studio_sample_motion", "arguments": {"model": 0, "bones": ["頭", "右手首", "右ひじ"], "start": 300, "end": 480, "step": 10}}
```

World-space bone positions at sampled frames. Use it to confirm a change starts and ends at the original pose and that
feet did not slide.

```json
{"name": "studio_audio_analysis", "arguments": {"start": 300, "end": 900}}
```

`bpm`, `beats`, `downbeats`, `onsets`, `sections`. Decides where a sharp stop is a hit and where it is a defect. Fails
without audio — say so and use the motion's timing.

```json
{"name": "studio_singing", "arguments": {"start": 0, "end": 900, "models": [0]}}
```

Per character: `phrases`, `breaths`, `held_notes`, `has_lipsync`, `share`; across characters `timeline`
(`singers` empty = instrumental). Use held notes and phrase starts for acting accents.

## See

```json
{"name": "studio_camera_solve", "arguments": {"frame": 360, "subject": {"models": [0]}, "shot": "full", "angle": {"yaw_deg": 15, "pitch_deg": 5}}}
```

Read-only framing solve: returns `camera` (a key you can write with `studio_set_camera_keys`) and `check`. `shot`:
`ecu | cu | mcu | ms | cowboy | full | wide`. Use `full` or `cowboy` for a motion check so the whole body is visible.

```json
{"name": "studio_contact_sheet", "arguments": {"start": 300, "end": 360, "step": 2, "columns": 8, "tile_width": 200, "settle_frames": 2, "path": "before_300_360.png"}}
```

`frames` OR `start`+`end` with `step` or `count`; `columns`, `tile_width` 96..640, `settle_frames`, `path`,
`return_image`, `physics`. Up to 48 tiles. `step: 2` over 1-2 s is the animator's flip. Use the same frames for the
before and after sheets.

```json
{"name": "studio_frame_check", "arguments": {"frames": [300, 330, 360]}}
```

Read-only framing evaluation (bbox, cuts, shot size, facing). A technical safety net, not the judgement.

## Edit: the four humanizing passes

All four accept `start`, `end`, `attack`, `release`, `dry_run`; the call is one undo step. `dry_run` first.

**ease** — rewrite the curves of existing keys (spacing, no new poses):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "dry_run": true, "ops": [
  {"op": "ease", "bones": ["右腕", "右ひじ"], "start": 300, "end": 360, "curve": "smooth", "only_linear": true, "channels": ["rotation"], "attack": 6, "release": 6}
]}}
```

`bones` optional (default every keyed bone); `curve` preset name (`smooth` default; `ease_in`, `ease_out`,
`ease_in_out`, `linear` are preset names); `only_linear` true leaves hand-authored curves alone; `channels` optional
(`x`, `y`, `z`, `rotation`, `translate`).

**overlap** — stagger chains for follow-through / successive breaking of joints:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "overlap", "chains": [["右腕", "右ひじ", "右手首"], ["上半身", "首", "頭"]], "start": 300, "end": 372, "lag": 2, "attack": 6, "release": 6}
]}}
```

`chains` optional (default the mirrored arm and torso chains); `lag` = frames per link, default 2, use 1-3.

**spring** — secondary motion (lag + overshoot / settle):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "spring", "bones": ["頭"], "start": 480, "end": 600, "frequency": 3, "damping": 0.45, "mix": 1, "attack": 6, "release": 6}
]}}
```

`frequency` Hz (default 3), `damping` (default 0.45), `mix` 0..1 (default 1). The op result reports
`max_deviation_deg` — read it.

**breathe** — idle life in dead holds:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "breathe", "bones": ["上半身", "首", "頭"], "start": 600, "end": 720, "period": 105, "amount_deg": 1.2, "only_holds": true, "head_drift_deg": 0.8}
]}}
```

`bones` optional (default the torso / head chain); `period` frames (default 105); `amount_deg` default 1.2;
`only_holds` default true; `head_drift_deg` default 0.8.

## Edit: acting and clean-up

**look_at** — head toward the lens, a model or a point:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "look_at", "target": "camera", "bones": ["首", "頭"], "weights": [0.4, 0.6], "start": 300, "end": 420, "attack": 10, "release": 10, "max_deg": 30}
]}}
```

`target`: `"camera"`, `[x,y,z]` or `{"model": 0, "bone": "頭"}`. Reports `mean_error_deg` (10-25 deg is normal for a
head turn because the ramps are included, even when correct). Keep `max_deg` <= 30.

**key_pose** — one held accent pose (absolute local values, blended in and out):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "key_pose", "frame": 360, "bones": {"右腕": {"rotate_deg": [-45, 0, 0]}, "右ひじ": {"rotate_deg": [-20, 0, 0]}}, "ease": 7}
]}}
```

**rotate** — a small additive layer (not an absolute pose):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "rotate", "bones": ["右腕"], "rotate_deg": [-25, 0, 0], "space": "local", "start": 352, "end": 376, "attack": 7, "release": 7}
]}}
```

`space`: `"local"` (default, parent frame) or `"world"`.

**retime** — move a phrase against the music:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "retime", "start": 600, "end": 720, "offset": 4, "scale": 1}
]}}
```

`offset` may be negative; `scale` > 0 about `start`; optional `bones`. Destination keys are replaced.

**copy / mirror** — repeat or mirror a phrase:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "copy", "start": 120, "end": 240, "to": 480, "mirror": true},
  {"op": "mirror", "start": 300, "end": 360, "bones": ["右腕", "右ひじ"]}
]}}
```

`copy` replaces destination keys. `mirror` with no `to` mirrors in place.

**smooth** — centred moving average for jitter:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "smooth", "bones": ["左手首", "右手首"], "start": 120, "end": 300, "window": 5, "step": 2}
]}}
```

Reports `max_change_deg`; a large value means real motion was flattened.

**morph** — blink, mouth, expression (merged by max):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "morph", "morph": "まばたき", "frames": [330, 420, 510], "peak": 1, "attack": 2, "hold": 2, "release": 4}
]}}
```

**Exact keys** — only when one bone needs hand-authored keys:

```json
{"name": "studio_set_bone_keys", "arguments": {"model": 0, "mode": "merge", "bones": {
  "右腕": [
    {"frame": 352, "rotate_deg": [-10, 0, 0], "curve": "ease_in"},
    {"frame": 360, "rotate_deg": [-45, 0, 0], "curve": "ease_in_out"},
    {"frame": 376, "rotate_deg": [-10, 0, 0], "curve": "ease_out"}
  ]
}}}
```

`mode`: `merge` (default) / `replace_range` / `replace_all`. `morphs`: `{"name": [{"frame": 360, "weight": 1}]}`.
A missing value takes the bone's evaluated value at that frame, not zero.

**Read / delete keys:**

```json
{"name": "studio_get_bone_keys", "arguments": {"model": 0, "bones": ["右腕", "右ひじ"], "start": 240, "end": 480}}
{"name": "studio_get_bone_keys", "arguments": {"model": 0, "morphs": true}}
{"name": "studio_delete_keys", "arguments": {"target": "bone", "model": 0, "bones": ["右腕"], "start": 300, "end": 360}}
```

`studio_delete_keys` target: `camera` / `light` / `bone` / `morph`.

**One-off pose:**

```json
{"name": "studio_select", "arguments": {"index": 0}}
{"name": "studio_set_bone", "arguments": {"bone": "右腕", "rotate_deg": [-45, 0, 0], "key": true}}
```

## Finish

```json
{"name": "studio_save", "arguments": {}}
{"name": "studio_command", "arguments": {"cmd": "studioexport C:/path/dance.vmd"}}
{"name": "undo", "arguments": {}}
```

One undo step per AI call; `undo` rolls back exactly the last one. Save only when asked.
