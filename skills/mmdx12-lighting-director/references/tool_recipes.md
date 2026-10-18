# Tool recipes

Exact JSON calls for the lighting director. Every argument here matches `src/app/McpTools.h`; do not invent fields.
`studio_contact_sheet` and `studio_singing` follow the contract documented in the skill (new tools).

## Read the scene and the song

```json
{"name": "studio_get_state", "arguments": {}}
{"name": "studio_scene_info", "arguments": {}}
{"name": "studio_audio_analysis", "arguments": {"start": 0, "end": 3600}}
```

`studio_scene_info` gives fps (30), `end_frame`, aspect, audio, models and every scene light with base and sampled
values, including the sun and ambient. Read the current look before changing it.

```json
{"name": "studio_singing", "arguments": {"models": [0, 1, 2, 3, 4], "start": 0, "end": 3600, "threshold": 0.15}}
```

Answer: per character `phrases [{start, end, mean_open, syllables}]`, `breaths [{start, end}]`,
`held_notes [{start, end, vowel}]`, `has_lipsync`, `share`; and `timeline [{start, end, singers: [model indices]}]`
(empty singers = instrumental). Use it to decide who is favoured, when, and where a breath lets the light move.

```json
{"name": "studio_motion_events", "arguments": {"step": 2}}
```

Dance map: jumps, crouches, hand raises, turns, holds, travel, formation, crossovers, energy.

## Look at the work

```json
{"name": "studio_contact_sheet", "arguments": {"frames": [0, 240, 480, 720, 960, 1200, 1500, 1800], "columns": 4, "tile_width": 320, "settle_frames": 4, "return_image": true}}
```

```json
{"name": "studio_contact_sheet", "arguments": {"start": 720, "end": 839, "step": 6, "columns": 4, "tile_width": 320, "settle_frames": 4, "path": "C:/tmp/chorus_light.png"}}
```

One grid image through the motion camera; each tile is labelled with its frame number and time. Choose frames
explicitly, or a range with `step` or `count`. Max 48 tiles. `tiles: [{index, frame, time, x, y, w, h}]` identifies
them. `path` saves, `return_image` also returns inline, `physics` defaults on. A sparse sheet across the whole song
shows the colour flow; a dense sheet shows how a cue moves.

```json
{"name": "seek", "arguments": {"seconds": 24}}
{"name": "wait_frames", "arguments": {"n": 15}}
{"name": "screenshot", "arguments": {"scene_only": true}}
```

## Build the rig

```json
{"name": "studio_light_rig", "arguments": {"rig": "key_fill_rim", "subject": 0, "palette": [[1.0, 0.85, 0.7], [0.4, 0.6, 1.0], [1.0, 0.3, 0.55]], "cone_deg": 24, "shadow": "hard", "name_prefix": "AI ", "replace": false}}
```

```json
{"name": "studio_light_rig", "arguments": {"rig": "top_spots", "count": 6, "subject": 0, "aim": "subject", "radius": 1.8, "height": 2.4, "intensity": 1.6, "color": [0.2, 0.8, 1.0], "cone_deg": 22, "shadow": "nocast", "name_prefix": "AI chorus", "replace": true}}
```

Rigs: `key_fill_rim`, `front_wash`, `top_spots`, `side_bars`, `back_beams`. `replace: true` deletes only lights a
previous call with the same `name_prefix` created. The answer lists `lights: [{index, uid, name, kind, position,
aim}]` — use those indices next.

## Create a follow spot per member

```json
{"name": "studio_set_light", "arguments": {"create": "spot", "name": "AI Follow 0", "props": {"aim_mode": "target", "target_model": 0, "target_part": "head", "shadow": "nocast"}, "base": {"position": [0, 3.2, 4.0], "intensity": 1.4, "color": [1.0, 0.95, 0.9], "cone_outer_deg": 16, "cone_inner_deg": 11}}}
```

Create one per character with `target_model` set to that model index. `aim_mode: "target"` makes the light track
the model; `target_part: "head"` keeps it on the face. Key its intensity around the phrases:

```json
{"name": "studio_set_light", "arguments": {"index": 8, "mode": "replace_range", "keys": [
  {"frame": 426, "intensity": 0.3},
  {"frame": 432, "intensity": 1.6},
  {"frame": 500, "intensity": 1.6},
  {"frame": 512, "intensity": 0.4}
]}}
```

Keys at phrase start - 6 frames and phrase end + 10 frames (offsets are examples; use the phrase data). A missing
value in a key takes the light's sampled value at that frame, so setting only `intensity` is correct. `mode`
merge / replace_range / replace_all applies to the keys only.

## Write the cue sheet

```json
{"name": "studio_light_cues", "arguments": {"cues": [
  {"lights": "all", "start": 0, "end": 479, "pattern": "breathe", "timing": "bar", "base": 0.4, "peak": 1.0, "mode": "replace_range"},
  {"lights": [5, 6, 7, 8], "start": 480, "end": 719, "pattern": "chase", "timing": "half", "every": 1, "base": 0.6, "peak": 2.0, "mode": "replace_range"},
  {"lights": [5, 6, 7, 8], "start": 720, "end": 1199, "pattern": "pulse", "timing": "beat", "base": 0.8, "peak": 3.5, "decay": 5, "mode": "replace_range"},
  {"lights": [5, 6, 7, 8], "start": 720, "end": 1199, "pattern": "color_cycle", "timing": "downbeat", "colors": [[0.2, 0.8, 1.0], [1.0, 0.3, 0.6]], "mode": "merge"}
]}}
```

Here 2-4 are the `key_fill_rim` lights and 5-8 a `top_spots` rig (indices from the rig answers): the rhythmic cues
drive the beams only, so the key light keeps the face lit. `"all"` also selects the key light: use it only for a
gentle global layer such as `breathe` or `section_levels`.

```json
{"name": "studio_light_cues", "arguments": {"cues": [
  {"lights": [3, 4], "start": 1200, "end": 1319, "pattern": "strobe", "timing": "onset", "every": 2, "min_strength": 0.6, "peak": 4.0, "decay": 3, "colors": [[1, 1, 1]], "mode": "replace_range"},
  {"lights": "all", "start": 2640, "end": 2880, "pattern": "fade", "timing": "section", "from": [1.0, 0.6, 0.3], "to": [0, 0, 0], "mode": "replace_range"},
  {"lights": "spots", "start": 720, "end": 1199, "pattern": "sweep", "timing": "bar", "aim_targets": ["left", "center", "right"], "mode": "merge"}
]}}
```

A global baseline in one call:

```json
{"name": "studio_light_cues", "arguments": {"cues": [
  {"lights": "all", "start": 0, "end": 2880, "pattern": "section_levels", "timing": "section", "mode": "replace_range"}
]}}
```

Light groups: light indices, or `spots` / `points` / `areas` / `all` (every non-sun non-ambient light). Ranges
must not overlap on the same lights unless the later cue uses `mode: "merge"`.

## Tune one light

```json
{"name": "studio_set_light", "arguments": {"index": 3, "props": {"shadow": "soft", "shadow_density": 0.4}, "base": {"intensity": 2.5, "cone_outer_deg": 22}, "mode": "merge"}}
```

Never `delete: true` on the user's lights without asking. `props` covers the non-keyed fields — `enabled`, `name`,
`aim_mode`, `target_model`, `target_part`, `shadow`, `shadow_softness`, `shadow_density`, `falloff`, `affect_diffuse`,
`affect_specular`, `rim_strength`, `rim_color`, and sky/ground colours for the sun. `base` holds the keyable values:
`position`, `aim`, `direction`, `color`, `intensity`, `range`, `cone_outer_deg`, `cone_inner_deg`, `size`.

## Inspect the technical result

```json
{"name": "studio_frame_check", "arguments": {"frames": [60, 240, 480, 720, 900, 1200, 2640]}}
```

Framing only; use it when lights and camera change together, mainly to confirm the performer is still in frame.

## Undo and save

```json
{"name": "undo", "arguments": {}}
{"name": "studio_save", "arguments": {"file": "C:/path/to/project.mmdxproj"}}
```

`undo` rolls back exactly the last AI write (one undo step per tool call). Save only when the user asked or
confirms.
