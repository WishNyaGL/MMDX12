---
name: mmdx12-lighting-director
description: Directs the stage lighting of an open MMDX12 Studio project over MCP. Probes the scene and its existing lights, reads the song's beat and section grid, plans a cue sheet per section, builds a rig with studio_light_rig, writes the cues with studio_light_cues, verifies on frames and screenshots, and fixes only the bad cues. Use for lighting, lighting direction, stage lights, 조명 연출, 라이팅, ライティング, 照明演出, or keying lights to an MMD dance video.
---

# MMDX12 Lighting Director

## Purpose & guarantees

You decide the look and the rhythm; the app computes and writes the light keys. Guarantees:

1. A rig is built first (`studio_light_rig`), then animated (`studio_light_cues`) - never the other way round.
2. Every cue sits on the song's real beat/section grid; the performer's face stays lit during vocals.
3. Colours stay to a 2-3 colour palette per section, and only the key casts shadows.
4. You only write inside your planned ranges; the user's lights and the sun/ambient are preserved.
5. Each write is exactly one undo step (`AI: <tool>`).

## Tools used

| Tool | Why |
| :--- | :--- |
| `studio_get_state` | Is Studio open, what exists, undo name |
| `studio_scene_info` | Aspect, end frame, audio, models, and every scene light with its values |
| `studio_audio_analysis` | bpm, beats, downbeats, onsets, sections for the cue timing |
| `studio_light_rig` | Create a whole rig in one undo step (key_fill_rim, front_wash, top_spots, side_bars, back_beams) |
| `studio_light_cues` | Generate keyed light animation from the beat grid in one undo step |
| `studio_set_light` | Create/edit/delete one light, tune base values or a few keys |
| `studio_frame_check` | Optional framing sanity when lights and camera change together |
| `seek`, `wait_frames`, `screenshot` | Look at the actual lighting (scene_only) |
| `undo` | Roll back the last write when a whole batch is wrong |
| `studio_save` | Save the project, only when the user asked or confirms |

## Workflow

### 1. Probe

```json
{"name": "studio_get_state", "arguments": {}}
{"name": "studio_scene_info", "arguments": {}}
```

Confirm Studio is open with a project. From `studio_scene_info` read the aspect ratio, `end_frame`, the audio
block, the models (indices and height) and every existing light: `index`, kind, name, enabled, base and sampled
values. Note the sun and ambient, their intensity and colour - you will keep them unless the user says otherwise.
Look at the current look before changing anything:

```json
{"name": "seek", "arguments": {"seconds": 0}}
{"name": "wait_frames", "arguments": {"n": 15}}
{"name": "screenshot", "arguments": {"scene_only": true}}
```

### 2. Understand the music

```json
{"name": "studio_audio_analysis", "arguments": {"start": 0, "end": 3600}}
```

Use `bpm`, `beats`, `downbeats`, `onsets`, `beat_energy` and `sections` (`start`, `end`, `level`, `energy`). If the
project has no audio the call fails: say so and fall back to the motion's key range, or ask.

### 3. Plan the cue sheet (before writing anything)

Build a table and show it to the user when present:

| Section | Frames | Level | Rig | Pattern | Timing | Colours | Note |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| verse | 0-479 | low | key_fill_rim | breathe | bar | warm key, cool rim | face lit, slow |
| pre-chorus | 480-719 | rising | same | chase | half | add magenta | fade up |
| chorus | 720-1199 | full | top_spots + back_beams | pulse | beat | cyan / magenta | alternate |
| break | 1200-1319 | peak | back_beams | strobe | onset | white | max 1-2 bars |
| outro | 2640-2880 | low | existing | fade | section | warm | fade to black |

Also plan the rig itself (which rig, count, palette, subject, intensity, shadow) and the global baseline before
the first write.

### 4. Write

Build the rig first - one `studio_light_rig` call is one undo step:

```json
{"name": "studio_light_rig", "arguments": {"rig": "key_fill_rim", "subject": 0, "palette": [[1.0, 0.85, 0.7], [0.4, 0.6, 1.0], [1.0, 0.3, 0.55]], "cone_deg": 24, "shadow": "hard", "name_prefix": "AI ", "replace": false}}
```

Add a second rig or a single light with `studio_set_light` when the plan needs it (a spot for a beam, a point for
a glow). `studio_light_rig` returns `lights: [{index, uid, name, kind, position, aim}]`; use those indices next.

Write the cue sheet in ONE `studio_light_cues` call per section, or one call for a short song:

```json
{"name": "studio_light_cues", "arguments": {"cues": [
  {"lights": "all", "start": 0, "end": 479, "pattern": "breathe", "timing": "bar", "base": 0.4, "peak": 1.0, "mode": "replace_range"},
  {"lights": [5, 6, 7, 8], "start": 480, "end": 719, "pattern": "chase", "timing": "half", "every": 1, "base": 0.6, "peak": 2.0, "mode": "replace_range"},
  {"lights": [5, 6, 7, 8], "start": 720, "end": 1199, "pattern": "pulse", "timing": "beat", "base": 0.8, "peak": 3.5, "decay": 5, "mode": "replace_range"},
  {"lights": [5, 6, 7, 8], "start": 720, "end": 1199, "pattern": "color_cycle", "timing": "downbeat", "colors": [[0.2, 0.8, 1.0], [1.0, 0.3, 0.6]], "mode": "merge"}
]}}
```

Here 2-4 are the `key_fill_rim` lights and 5-8 a `top_spots` rig (indices from the rig answers): the rhythmic cues
drive the beams only, so the key light (index 2) keeps the face lit. `"all"` also selects the key light: use it
only for a gentle global layer such as `breathe` or `section_levels`.

Other valid shapes:

```json
{"name": "studio_light_cues", "arguments": {"cues": [
  {"lights": [3, 4], "start": 1200, "end": 1319, "pattern": "strobe", "timing": "onset", "every": 2, "min_strength": 0.6, "peak": 4.0, "decay": 3, "colors": [[1, 1, 1]], "mode": "replace_range"},
  {"lights": "all", "start": 2640, "end": 2880, "pattern": "fade", "timing": "section", "from": [1.0, 0.6, 0.3], "to": [0, 0, 0], "mode": "replace_range"},
  {"lights": "spots", "start": 720, "end": 1199, "pattern": "sweep", "timing": "bar", "aim_targets": ["left", "center", "right"], "mode": "merge"}
]}}
```

For a global baseline in one call, `section_levels` is the quick path:

```json
{"name": "studio_light_cues", "arguments": {"cues": [
  {"lights": "all", "start": 0, "end": 2880, "pattern": "section_levels", "timing": "section", "mode": "replace_range"}
]}}
```

Tune a single light's base or a few keys with `studio_set_light` (never `delete: true` on the user's lights
without asking):

```json
{"name": "studio_set_light", "arguments": {"index": 3, "props": {"shadow": "soft", "shadow_density": 0.4}, "base": {"intensity": 2.5, "cone_outer_deg": 22}, "mode": "merge"}}
```

### 5. Verify

```json
{"name": "studio_frame_check", "arguments": {"frames": [60, 240, 480, 720, 900, 1200, 2640]}}
```

`studio_frame_check` is about framing; use it when lights and camera change together, mainly to confirm the
performer is still in frame. Then look at the light itself:

```json
{"name": "seek", "arguments": {"seconds": 24}}
{"name": "wait_frames", "arguments": {"n": 15}}
{"name": "screenshot", "arguments": {"scene_only": true}}
```

Check one frame per section plus a chorus beat: the face is lit, the palette holds, beams read, nothing is
blown out, and the background is not brighter than the performer. Fix only the bad cues by re-issuing them alone
with `mode: "replace_range"` on the same range, or `undo` the last batch and rewrite it.

### 6. Save

```json
{"name": "studio_save", "arguments": {"file": "C:/path/to/project.mmdxproj"}}
```

Only when the user asked or confirms.

## Direction rules

**Kinds:** `sun` = global key (keep it low in a dark stage), `ambient` = fill environment (raise only to lift
shadows), `area` = soft wash, `point` = local glow / bars, `spot` = beams and key. The rig tools never touch the
sun or ambient; adjust them with `studio_set_light` by index when the budget needs it.

**Face first:** a key or front wash is never fully off during vocals. If a cue dims `all`, exclude the key light
or keep its `base` high enough to read the face. Blackout is for the end of a song, not for a verse.

**Colour:** warm key / cool rim is the safe base. Use 2-3 colours per section and change the palette on section
boundaries, not every bar. Saturated colours belong on beams, rims and bars, not on the face key (skin goes
green/blue). Linear RGB, `0..1+`; values above 1 are allowed for beams.

**Intensity budget:** many lights add up. Lower the sun/ambient when adding a rig, and check the sum on screen.
`base` / `peak` are per-light intensities; a pulse at peak 4 on 20 lights is a white-out.

**Shadow cost:** only the key casts shadows (`shadow: "hard"` or `"soft"`); everything else is `nocast`. One
shadow-casting light is enough for a concert look.

**Music mapping:**

- Verse: low energy - `breathe` on `bar` or `section`, slow fade, low `peak`.
- Pre-chorus: rising - `fade` up, `chase` on `half`, increasing `peak`.
- Chorus: full - `pulse` on `beat`, `chase`/`alternate` across lights, `sweep` beams on `bar`, colour change on
  `section`.
- Break: `strobe` on `onset`, sparingly - max 1-2 bars, and note photosensitivity to the user.
- Outro: `fade` / `blackout`.

**Readability:** one rhythmic layer at a time. Do not key every light on every beat; keep a stable base layer
(breathing wash), one moving layer (chase/sweep) and one accent layer (pulse/strobe). `section_levels` is the
quick global baseline; layer the rest on top with `mode: "merge"`.

## Verification

- `studio_light_cues` returns `cues: [{pattern, lights, events, keys_written}]`, `undo` and `bpm` - check the
  event counts match the section length and `bpm`.
- `studio_get_state` undo name is `AI: studio_light_cues` / `AI: studio_light_rig` after your last write.
- Screenshots per section, actually inspected: face lit, palette coherent, beams visible, no clipped whites.
- The sun/ambient and the user's pre-existing lights are unchanged unless you deliberately changed them.

## Pitfalls

- The studio must be open with a project; `studio_get_state` rejects otherwise.
- A song with no audio has no beats/sections; `studio_audio_analysis` fails - fall back or ask.
- Frames are 30 fps timeline frames, not seconds; `seek` takes seconds.
- MMD scene-light keys interpolate linearly; pulse/chase cues need their per-event keys (the tool writes them).
- Existing user work: never `replace_all`-style wiping; `studio_light_cues` defaults to `replace_range` on the
  exact ranges you give, so keep them inside the plan.
- `studio_light_rig` with `replace: true` deletes only lights created by a previous rig call with the same
  `name_prefix`; it never deletes the user's lights or the sun/ambient.
- A timing that finds no events in the range rejects the call; check that the range contains beats/onsets.
- Do not invent tools or arguments; only use what is listed above.
