---
name: mmdx12-camera-director
description: Directs music-video camera work in an open MMDX12 Studio project over MCP. Probes the scene, reads the song's beat/section grid and the dance's derived events, plans a shot list on downbeats and section boundaries, then writes motion-camera keys with the app's framing solver and shot generator, verifies the framing on real frames and screenshots, and fixes only the bad shots. Use for camera work, camera movement, camera motion, 카메라 무빙, 카메라 워크, 카메라 연출, カメラワーク, カメラ演出, or directing an MMD dance video.
---

# MMDX12 Camera Director

## Purpose & guarantees

You are the director: you decide the shot size, angle, move and timing. The app computes where the camera goes
(`studio_camera_solve`, `studio_camera_shots`) and writes the keys. Guarantees of this workflow:

1. Every shot is planned against the song's real beat/section grid and the dance's real events, not a template.
2. FOV stays in the 10-40 degree range, with the shot size the song asks for.
3. Cuts land on downbeats or section boundaries; no left-right yaw "wiper" oscillation and no jump cuts.
4. The framing is checked on actual frames and screenshots before you call the work done.
5. You only write inside the ranges you planned, and every write is exactly one undo step.

## Tools used

| Tool | Why |
| :--- | :--- |
| `studio_get_state` | Is Studio open, what is selected, key counts, undo name |
| `studio_scene_info` | Aspect, end frame, audio, models/height/facing, existing camera and lights |
| `studio_audio_analysis` | bpm, beats, downbeats, onsets, sections for the shot timing |
| `studio_motion_events` | Jumps, crouches, hand raises, turns, holds, travel, energy, formation/crossovers |
| `studio_sample_motion` | Exact bone positions only when a shot needs a specific gesture or body point |
| `studio_camera_solve` | One framing solution to inspect before committing a shot |
| `studio_camera_shots` | Write a whole shot list in one undo step (dry_run first for long songs) |
| `studio_get_camera_keys` | Read existing camera keys and `cuts` before planning around them |
| `studio_frame_check` | Verify head_in_frame, cut list, shot_size at a shot's start/middle/end |
| `seek`, `wait_frames`, `screenshot` | Look at the actual frames (scene_only) |
| `undo` | Roll back your last write when a whole batch is wrong |
| `studio_save` | Save the project, only when the user asked or confirms |

## Workflow

### 1. Probe

```json
{"name": "studio_get_state", "arguments": {}}
{"name": "studio_scene_info", "arguments": {}}
```

Confirm Studio is open with a project (`studio_get_state` rejects otherwise). From `studio_scene_info` read the
aspect ratio (frame for it: scope `2.39:1` leaves little headroom; change it only when the user asks, with
`studio_command` `{"cmd": "studioaspect", "args": ["2.39:1"]}`), `end_frame`, the audio block, the model list (index, height, `facing_yaw_deg`, AABB) and the existing
camera key count/range. If the user already has camera keys, read them and decide whether you extend, replace a
range or leave them alone:

```json
{"name": "studio_get_camera_keys", "arguments": {"start": 0, "end": 3600}}
```

### 2. Understand the music and the dance

```json
{"name": "studio_audio_analysis", "arguments": {"start": 0, "end": 3600}}
{"name": "studio_motion_events", "arguments": {"step": 2}}
```

Build the timing from `bpm`, `beats`, `downbeats` and `sections`; read the dance from jumps, crouches, hand_raises,
turns, holds, `travel.max_speed`, energy windows, and (2+ characters) `formation` and `crossovers`. Use
`studio_sample_motion` only when one shot depends on a specific body point that the events do not describe:

```json
{"name": "studio_sample_motion", "arguments": {"model": 0, "bones": ["頭", "左手首"], "start": 0, "end": 300, "step": 10}}
```

Frames are timeline frames at 30 fps. If the project has no audio, the analysis fails: use the motion's key range
and events for timing, and say so.

### 3. Plan the shot list (before writing anything)

Write a table with one row per shot and show it to the user when the user is present:

| # | Frames | Section | Shot | Lens (fov) | Angle | Move | Subject | Motivation |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| 1 | 0-119 | verse | ms | 25 | +18 yaw, +5 pitch | dolly_in | 0 | establish, push on the vocal |
| 2 | 120-239 | verse | cu | 16 | -12 yaw | static | 0 | lyric close-up |
| 3 | 240-359 | chorus | full | 35 | 0 yaw | crane_up | 0,1,2,3,4 | formation drop |
| 4 | 360-419 | chorus | ms | 24 | +25 yaw | push_accent | 1 | accent on strong onsets |

Rules for the plan: cut on downbeats/section boundaries, keep consecutive shots clearly different, and cover every
planned range exactly once. Only after the plan is settled, write.

### 4. Write

Camera: ONE `studio_camera_shots` call per section, or one call for the whole song. For a long song, run `dry_run`
first and inspect the returned keys.

```json
{"name": "studio_camera_shots", "arguments": {"dry_run": true, "mode": "replace_range", "shots": [
  {"start": 0, "end": 119, "subject": {"models": [0]}, "shot": "ms", "angle": {"yaw_deg": 18, "pitch_deg": 5}, "move": "dolly_in", "curve": "smooth"},
  {"start": 120, "end": 239, "subject": {"models": [0], "focus": "face"}, "shot": "cu", "angle": {"yaw_deg": -12}, "move": "static", "curve": "ease_in_out", "focus": {"aperture": 0.75}}
]}}
```

Then the same call without `dry_run` (one undo step, `AI: studio_camera_shots`). A shot's `start`/`end` are
inclusive; `end + 1 == next start` is a hard cut. Use `shot` + `end_shot` for size changes, `angle` + `end_angle`
for an angle change, `fov_deg` + `end_fov_deg` for a lens move, `follow`/`follow_step` for a travelling dancer, and
`accents` + `accent` for beat bumps:

```json
{"name": "studio_camera_shots", "arguments": {"mode": "replace_range", "shots": [
  {"start": 240, "end": 359, "subject": {"models": [0, 1, 2, 3, 4]}, "shot": "full", "angle": {"yaw_deg": 5, "pitch_deg": 8}, "move": "crane_up", "amount": 22, "curve": "ease_in_out"},
  {"start": 360, "end": 419, "subject": {"models": [1]}, "shot": "ms", "move": "push_accent", "accents": [366, 380, 394, 408], "accent": {"kind": "push", "amount": 0.12, "attack": 3, "release": 10}, "curve": "ease_in_out"},
  {"start": 420, "end": 539, "subject": {"models": [0]}, "shot": "mcu", "move": "follow", "follow": true, "follow_step": 10, "curve": "smooth"}
]}}
```

For a single hard framing, `studio_camera_solve` returns the exact key fields plus a check; add them with
`studio_set_camera_keys` only when you truly need one key, otherwise prefer `studio_camera_shots`:

```json
{"name": "studio_camera_solve", "arguments": {"frame": 120, "subject": {"models": [0]}, "shot": "ms", "angle": {"yaw_deg": 20, "pitch_deg": 5}}}
```

### 5. Verify

```json
{"name": "studio_frame_check", "arguments": {"frames": [0, 60, 119, 120, 180, 239, 240, 300, 359]}}
```

First read each shot's `warnings` in the `studio_camera_shots` answer (the app checks the start, quarters and end
of every shot: a head out of frame or a subject behind the camera is listed there) and fix those shots. Then for
every shot's start, middle and end confirm `head_in_frame`, a `cut` list that fits the shot size (feet are cut in a
`ms`, nothing is cut in a `full`) and that `shot_size` matches the plan (`studio_frame_check` uses the same
ecu..wide vocabulary). Then look at a few frames:

```json
{"name": "seek", "arguments": {"seconds": 4.0}}
{"name": "wait_frames", "arguments": {"n": 15}}
{"name": "screenshot", "arguments": {"scene_only": true}}
```

Fix only the bad shots by re-issuing them alone with `mode: "replace_range"` (same frame range), or `undo` if the
whole last batch is wrong. Re-check the same frames afterwards.

### 6. Save

```json
{"name": "studio_save", "arguments": {"file": "C:/path/to/project.mmdxproj"}}
```

Call it only when the user asked to save or confirms it.

## Direction rules

**Angles:** `angle.yaw_deg` is relative to the subject's facing: 0 = in front of the face, +90 = from the subject's
left, -90 = from the right, 180 = from behind; `absolute: true` uses world yaw. `pitch_deg` > 0 = camera above looking
down. Keep a scene's yaws on one side of the subject (e.g. all within -60..+20) rather than alternating signs.

**Shot sizes** (`shot` / `end_shot`): `ecu` 0.10 body height, eyes - a single emotional beat; `cu` 0.22 - a lyric
line, face and shoulders; `mcu` 0.32 - a singing line with some hands; `ms` 0.50 - waist up, the default for
choreography in a verse; `cowboy` 0.70 - thighs up, leg work and attitude; `full` 1.15 - the whole dancer, solo
dance; `wide` 2.20 - the whole formation or stage.

**Lens** (`fov_deg`, integer degrees, vertical): 10-20 telephoto for emotion and isolation; 20-28 natural for
choreography and duos; 28-40 wide for formations, stage geometry and reveals. Default 25. MMD stores integers, so
pick whole degrees; the solver compensates with distance.

**Cutting:** hard cut = the next shot starts at the previous `end + 1`. Cut on downbeats and section boundaries.
Verse shots 2-4 bars; chorus shots 1-2 bars; place extra cuts on strong onsets. Never leave a gap or overlap.

**Moves:** `static` (hold, let the dance read), `dolly_in`/`dolly_out` (distance, draw in/let go), `zoom_in`/
`zoom_out` (fov only, position fixed; flatter, more graphic), `dolly_zoom` (dist + fov, vertigo), `crane_up`/
`crane_down` (pitch and target height, reveal/descend), `truck_left`/`truck_right` (slide sideways, parallax),
`arc_left`/`arc_right` (orbit/swing around the subject), `follow` (steadicam: heading, distance and lens stay, the
camera travels with the dancer's smoothed movement), `push_accent` (static driven by beat accents).
Every move is relative to the shot's OPENING framing (solved at `start`): a non-follow move does not chase the dancer,
so a dancer who travels far (`travel`, `max_speed` in `studio_motion_events`) needs `follow: true` (any move can follow)
or a shorter shot. `teleports` (frames where the dance re-places the performer) must be cut points: never let a shot
span one.

**Curves** (`curve` between a shot's keys): `ease_in_out` default; `smooth` for floating moves; `fast_start` into
an accent; `linear` only for a constant-speed rail. Accents use `attack` into the peak and `release` back.

**Continuity:** respect the 180-degree rule - keep the subject on the same side of the axis within a scene; cross
the axis only on a section change or with a moving shot. Keep screen direction across cuts. Never alternate
left/right yaw on consecutive shots (the "wiper"): that is oscillation, not direction.

**Variety:** consecutive shots must differ by >= 2 shot sizes or a clear angle change (>= 30 deg); otherwise it is
a jump cut. Vary the lens with the size, not randomly.

**Groups:** `subject.models` accepts several indices; widen at formation changes and `crossovers`. Use `focus`
(`face`/`upper`/`body`) deliberately, and `lead` (0-0.2) for look room. DoF: `focus.aperture` 0 on group shots,
0.6-0.85 on close shots. Dutch roll only as an accent, <= 8 degrees.

**Beat recipes:** `push_accent` on chorus downbeats; `zoom_in` accents on strong onsets; `arc_left`/`arc_right`
over a 2-bar build; `crane_up` on a pre-chorus rise.

## Verification

- `studio_get_camera_keys` shows the final keys and `cuts`; every planned cut should appear as adjacent frames.
- No `warnings` left in the last `studio_camera_shots` answer.
- `studio_frame_check` on all shot boundaries: `head_in_frame` true, `cut` fits the size, `shot_size` as planned.
- Screenshots (`seek` -> `wait_frames: 15` -> `screenshot` with `scene_only: true`) at one frame per section, and
  actually look at them for clipping, empty frames and unwanted angles.
- `studio_get_state` undo name is `AI: studio_camera_shots` after your last write.

## Pitfalls

- The studio must be open with a project; `studio_get_state` rejects otherwise.
- A song with no audio has no beats or sections; fall back to the motion's timing or ask.
- Frames are 30 fps timeline frames, not seconds; `seek` takes seconds.
- MMD camera keys ease between keys with the key's curve; a plain move is two keys, while arcs, follows and accents
  are baked by `studio_camera_shots` into keys every 10 frames (linear segments, the easing is baked in). Writing the
  same with `studio_set_camera_keys` by hand is rarely worth it.
- MMD scene-light keys interpolate linearly too: do not expect a smooth theatrical fade from two distant light
  keys when tuning a look around a shot.
- Existing user work: never `replace_all` without asking; restrict writes to the ranges you planned.
- Every AI write is one undo step named `AI: <tool>`; `undo` rolls back exactly that last write.
- `studio_camera_shots` rejects overlapping shots, and skips accents closer than `attack + release` to each other
  or to the shot ends.
- Do not invent tools or arguments; only use what is listed above.
