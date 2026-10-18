---
name: mmdx12-motion-editor
description: Edits a character's existing dance motion in an open MMDX12 Studio project over MCP. Probes the scene and the motion's key ranges, reads the dance's events and the song's beat grid, plans targeted layered edits, writes them as one undo step with studio_motion_edit or the bulk bone tools, verifies by re-sampling and screenshots, and undoes anything that reads worse. Use for motion edit, edit motion, fix the dance, pose, 모션 수정, 모션 편집, 안무 수정, 포즈, モーション編集, モーション修正, or accenting, retiming, mirroring, cleaning and adding expressions to an existing MMD dance.
---

# MMDX12 Motion Editor

## Purpose & guarantees

You edit the dance that is already there; the app evaluates the motion and writes the changed keys. Guarantees:

1. Every change is layered inside a declared range, with an envelope so nothing pops.
2. You read the motion first (events, sampled bones, beats) and never key IK-driven leg bones.
3. Every write is one undo step (`AI: studio_motion_edit` / `AI: studio_set_bone_keys`), re-sampled and
   screenshotted afterwards, and undone if it is worse.

## Tools used

| Tool | Why |
| :--- | :--- |
| `studio_get_state` | Is Studio open, selection, key counts, undo name |
| `studio_scene_info` | Models, kinds, height, facing, key ranges |
| `studio_motion_events` | Jumps, crouches, hand raises, turns, holds, travel, energy |
| `studio_sample_motion` | World-space bone positions at sampled frames |
| `studio_audio_analysis` | bpm, beats, downbeats, onsets, sections |
| `studio_motion_edit` | rotate, translate, look_at, retime, copy, mirror, smooth, morph, key_pose |
| `studio_get_bone_keys` / `studio_set_bone_keys` | Read / write exact keys of bones and morphs |
| `studio_delete_keys` | Remove keys in a frame range |
| `studio_set_bone` | One-off pose on the selected model (`studio_select` first) |
| `studio_camera_solve`, `studio_frame_check` | Frame the edit so the changed bones are visible |
| `seek`, `wait_frames`, `screenshot` | Look at the frames (scene_only) |
| `undo` | Roll back the last write when the batch is wrong |
| `studio_save`, `studio_command` | Save the project or `studioexport <file.vmd>`, only when asked |

## Workflow

### 1. Probe

```json
{"name": "studio_get_state", "arguments": {}}
{"name": "studio_scene_info", "arguments": {}}
```

Confirm Studio is open. From `studio_scene_info` read each model's index, `kind`, height, facing and key range.
The target must be a character (stages and props reject bone edits).

### 2. Understand the motion around the edit

```json
{"name": "studio_motion_events", "arguments": {"model": 0, "step": 2}}
{"name": "studio_sample_motion", "arguments": {"model": 0, "bones": ["頭", "右手首", "右ひじ"], "start": 300, "end": 480, "step": 10}}
```

Events show what the dance already does (`jumps`, `crouches`, `hand_raises`, `turns`, `holds`, `travel`,
energy). Never cross a jump/re-place with an envelope, and do not fight an existing `hand_raise`.

When the edit must land on the music:

```json
{"name": "studio_audio_analysis", "arguments": {"start": 300, "end": 900}}
```

Use `beats`, `downbeats` and `onsets`; with no audio, fall back to the motion's timing and say so. For exact key
work, read the current keys first:

```json
{"name": "studio_get_bone_keys", "arguments": {"model": 0, "bones": ["右腕", "右ひじ"], "start": 240, "end": 480}}
```

### 3. State the edit plan

Before writing anything, write a short list and show it to the user:

| # | Op | Frames | Bones / morph | Amount | Why |
| :-- | :-- | :-- | :-- | :-- | :-- |
| 1 | look_at | 300-420 | 首, 頭 | max 30 deg | chorus line looks into the lens |
| 2 | key_pose | 360 | 右腕, 右ひじ | -40 deg x | arm accent on the downbeat |
| 3 | morph | 402, 432 | まばたき | peak 1 | blink between the existing ones |

Keep each plan small, with ranges you can verify.

### 4. Dry run

Every `studio_motion_edit` call runs the same validation with `dry_run: true` first:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "dry_run": true, "ops": [
  {"op": "look_at", "target": "camera", "bones": ["首", "頭"], "weights": [0.4, 0.6], "start": 300, "end": 420, "attack": 10, "release": 10, "max_deg": 30},
  {"op": "key_pose", "frame": 360, "bones": {"右腕": {"rotate_deg": [-40, 0, 0]}, "右ひじ": {"rotate_deg": [-20, 0, 0]}}, "ease": 7},
  {"op": "morph", "morph": "まばたき", "frames": [402, 432], "peak": 1}
]}}
```

Check per-op `tracks`, `track_changes` (`added` / `modified`) and `keys_written`: an empty or huge count means a
wrong range or bone. Unknown bones / morphs reject the call (nothing written).

### 5. Apply

Issue the same call without `dry_run` (one undo step, `AI: studio_motion_edit`).

### 6. Verify

```json
{"name": "studio_sample_motion", "arguments": {"model": 0, "bones": ["頭", "右手首"], "start": 290, "end": 430, "step": 5}}
```

Re-sample the touched bones: the change must start and end at the original pose. For `look_at`, read
`mean_error_deg`: it averages over the attack / release ramps and the `max_deg` clamp too, so 10-25 deg is normal for a
head turn; judge the hold by the screenshot (the eyes into the lens). Screenshot start, peak and end:

```json
{"name": "studio_camera_solve", "arguments": {"frame": 360, "subject": {"models": [0]}, "shot": "ms", "angle": {"yaw_deg": 15, "pitch_deg": 5}}}
{"name": "seek", "arguments": {"seconds": 10.0}}
{"name": "wait_frames", "arguments": {"n": 15}}
{"name": "screenshot", "arguments": {"scene_only": true}}
```

`studio_frame_check` confirms the framing. Compare before / after; if it reads worse, `undo` and rethink.

## Recipes

**Head turn toward the camera on a chorus line.** Camera-relative, attack / release 8-12 frames:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "look_at", "target": "camera", "bones": ["首", "頭"], "weights": [0.4, 0.6], "start": 300, "end": 420, "attack": 10, "release": 10, "max_deg": 30}
]}}
```

The tool reports `mean_error_deg`; keep `max_deg` at 30 or less so the neck does not break.

**Arm raise accent on a downbeat.** Absolute pose on 右腕 / 右ひじ, eased over 6-8 frames:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "key_pose", "frame": 360, "bones": {"右腕": {"rotate_deg": [-45, 0, 0]}, "右ひじ": {"rotate_deg": [-20, 0, 0]}}, "ease": 7}
]}}
```

Arm axes: `x` swings the arm forward / back, `z` raises / lowers it sideways (the sign differs for 左 / 右). The sign
depends on the model's rest pose: dry-run, apply, take ONE screenshot at the peak, and flip the sign if the arm went
the wrong way (undo first).

For a softer layer that adds to the existing arm instead of replacing the pose, use `rotate`:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "rotate", "bones": ["右腕"], "rotate_deg": [-25, 0, 0], "start": 352, "end": 376, "attack": 7, "release": 7}
]}}
```

**Retime a late section by a few frames.** Move every track in the range 4 frames later:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "retime", "start": 600, "end": 720, "offset": 4}
]}}
```

Add `"bones": ["センター", "上半身"]` to move only those tracks. Move a whole phrase if the audio drifted.

**Mirror a phrase for a partner.** Copy the range to another range with `mirror: true` (each bone's keys go onto its
mirror bone, centre bones mirror in place):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "copy", "start": 120, "end": 240, "to": 480, "mirror": true}
]}}
```

Destination keys are replaced. Use `mirror` (no `to`) to mirror a range in place.

**Blink every 3-4 seconds.** Place morph peaks on beats, avoiding the frames where まばたき already peaks
(from `studio_get_bone_keys` with `morphs: true`):

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "morph", "morph": "まばたき", "frames": [330, 420, 510], "peak": 1, "attack": 2, "hold": 2, "release": 4}
]}}
```

**Smile / mouth shapes on a chorus.** Expression morphs on onsets, low peak:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "morph", "morph": "笑い", "frames": [360, 390, 420], "peak": 0.6, "attack": 3, "hold": 3, "release": 5},
  {"op": "morph", "morph": "あ", "frames": [366, 396], "peak": 0.5, "attack": 2, "hold": 2, "release": 4}
]}}
```

**Smooth jittery hand keys.** Centred moving average, envelope-blended at the ends:

```json
{"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
  {"op": "smooth", "bones": ["左手首", "右手首"], "start": 120, "end": 300, "window": 5, "step": 2}
]}}
```

Read `max_change_deg`; a large value means the window flattened real motion, so undo and use 3.

**Precise key work instead of an op.** When one bone needs exact keys, write them directly (`mode` is `merge`):

```json
{"name": "studio_set_bone_keys", "arguments": {"model": 0, "mode": "merge", "bones": {
  "右腕": [
    {"frame": 352, "rotate_deg": [-10, 0, 0], "curve": "ease_in"},
    {"frame": 360, "rotate_deg": [-45, 0, 0], "curve": "ease_in_out"},
    {"frame": 376, "rotate_deg": [-10, 0, 0], "curve": "ease_out"}
  ]
}}}
```

Delete a range with `studio_delete_keys` (`target: "bone"`). For one pose, `studio_select` then `studio_set_bone`
at the current frame (`key: true` registers it).

## Rules

- **Layer, don't replace.** Prefer `rotate`, `look_at`, `key_pose` and `morph` over rewriting whole tracks; reach for
  `studio_set_bone_keys` only for exact key work.
- **Small amounts.** Head rotation <= 30 degrees, arms <= 45 degrees, and envelopes (`attack` / `release`) >= 6
  frames so nothing pops.
- **Respect IK.** Leg and foot IK bones (足ＩＫ, つま先ＩＫ) drive the legs; edit feet through the IK bones, never the
  leg bones (IK overwrites their rotations). See `references/bones.md`.
- **Respect teleports and keep feet planted.** No envelope across a re-placement; do not translate センター without
  a reason, and if you must, check `左足首` / `右足首` with `studio_sample_motion` afterwards.
- **Physics bones are not keyable.** Hair, skirt and other physics bones are simulated; key the body chain instead.
- **One undo step per call.** Group related ops into one call; save (`studio_save` / `studioexport <file.vmd>`) only
  when asked.

## Limits

- **Authoring a whole dance from nothing is out of scope.** These tools edit, accent, retime, mirror, clean and add
  expressions to an existing motion.
- **Contact-accurate poses are not reliable.** Hand-to-body, hand-to-hand and prop contacts need visual iteration in
  the editor; do not promise exact contact from numeric ops.
- **Finger acting is not practical.** Keying finger bones one by one reads poorly without hand authoring. Say the
  request is out of scope instead of delivering low-quality motion.

## Verification

- The dry run's `track_changes` match the plan (the right tracks, a plausible key count).
- Re-sampled bones: the pose at `start - 1` and `end + 1` equals the original, and the peak is where you planned.
- `look_at`'s `mean_error_deg` is plausible (ramps included) and the peak screenshot shows the face toward the
  target; `smooth` answers with a sane `max_change_deg`.
- Screenshots at start / peak / end from a camera that shows the touched bones, compared with before. The undo
  name is `AI: studio_motion_edit` / `AI: studio_set_bone_keys` and key counts changed only in your range.

## Pitfalls

- The studio must be open with a project and the `studio` screen; the model must be a character.
- Frames are timeline frames at 30 fps, not seconds; `seek` takes seconds.
- Bone names are Japanese PMX names; IK uses the full-width `ＩＫ` (`左足ＩＫ`). Unknown names reject with a list.
- `rotate_deg` is Euler XYZ degrees, VMD-local (`XMQuaternionRotationRollPitchYaw`), not radians or world axes;
  `space: "world"` exists on `rotate` only.
- A missing value in `studio_set_bone_keys` takes the bone's evaluated value at that frame, not zero.
- `studio_motion_edit` ops apply in order; later ops see earlier output, so `retime` before adding new keys.
- `copy` and `replace_range` overwrite keys on the destination frames; check the user's work first.
- Every write is one undo step named `AI: <tool>`; `undo` rolls back exactly the last one.
- Do not invent tools or arguments; only use what is listed above.
