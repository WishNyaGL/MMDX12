# Lighting grammar

The vocabulary behind `skills/mmdx12-lighting-director/SKILL.md`. All arguments belong to `studio_light_rig`,
`studio_light_cues`, `studio_set_light` and `studio_scene_info`; no scripts. Use this when you need the exact word
for an intention, not as a source of intentions.

## 1. Light kinds

| Kind | Physical role | Notes |
| :--- | :--- | :--- |
| `sun` | global key / scene light | at most one; keep it low on a dark stage |
| `ambient` | fill environment | at most one; raise only to lift shadows |
| `area` | soft wash | has `size` `[w,h]`; the natural fill and front wash |
| `point` | local glow, bars, practicals | falls off; good for side columns |
| `spot` | beams and key | has `cone_outer_deg` / `cone_inner_deg` (inner <= outer) |

`studio_light_rig` never touches the sun or ambient. It creates only the lights of its rig, and with
`replace: true` it deletes only the lights a previous rig call with the same `name_prefix` created.

## 2. Rigs

| Rig | Builds | Default count | Useful when |
| :--- | :--- | :--- | :--- |
| `key_fill_rim` | key spot 35 deg to one side, soft fill area opposite, rim spot behind-above | 3 | a readable base for a solo or small group |
| `front_wash` | area lights in an arc in front | 3 | even, soft faces; ballads and intimacy |
| `top_spots` | spots above/in front, `aim` `subject` or `down` | 6 | beams from above; pop and group numbers |
| `side_bars` | two columns of point lights left/right | 4 per side | vertical bars, club depth |
| `back_beams` | spots behind aimed up/forward, sway phases spread | 4 | silhouettes, halo, backlight |

Common arguments: `subject` (character model index; default the first character), `radius` and `height` in
character heights (default 1.6), `color` or `palette` (cycled per light), `intensity`, `cone_deg` (spot outer;
inner = 0.7 x outer), `size` `[w,h]` (area), `shadow` (`nocast|hard|soft`), and `aim` (`subject|down`) for
`top_spots`. Positions scale with the character's height, sampled at the current frame, so set the playhead to a
representative pose before building.

## 3. Patterns

| Pattern | What it does | Timing | Use when |
| :--- | :--- | :--- | :--- |
| `pulse` | intensity bump then decay | `beat`, `downbeat`, `onset` | a chorus needs a physical beat |
| `chase` | lights in sequence | `half`, `beat` | a build, a group moving as a wave |
| `alternate` | two groups swap | `beat`, `bar` | simple left/right movement |
| `strobe` | very short on/off | `onset` | one break accent, 1-2 bars, never everywhere |
| `sweep` | aim moves across `aim_points` / `aim_targets` | `bar` | beams scanning the stage |
| `color_cycle` | colour changes | `section`, `bar` | a deliberate palette shift |
| `breathe` | slow intensity swell | `bar`, `section` | life in a quiet passage |
| `fade` | `from` to `to` colour/intensity | `section` | transitions, outro |
| `section_levels` | one level per song section | `section` | a global baseline to layer on |
| `blackout` | intensity to zero | `section` | the very end |

`timing` names the event grid: `beat` (default), `half`, `downbeat`, `bar`, `onset`, `section`. `every` uses each
Nth event, `offset` shifts the phase, `min_strength` filters onsets (default 0.5), `decay` is frames (default
0.6 x beat). Events are rounded to integer frames and duplicates dropped. `base` / `peak` default to the light's
current intensity x0.3 / x1.0; set them explicitly when you add lights.

`aim_targets` accepts `head`, `center`, `left`, `right`, `audience`; `aim_points` accepts world `[x,y,z]`.
`sweep` and `pulse` on the same lights at the same timing fight each other — layer on different lights.

For a follow spot, `studio_set_light` with `create: "spot"` and `props: {"aim_mode": "target",
"target_model": 0, "target_part": "head"}` tracks the performer without aim keys. Key its `intensity` with
`keys` instead.

## 4. Colour

- Two or three colours per section, changing where the song changes, not every bar.
- Saturated colour belongs on beams, rims, bars and backlight; keep the face key near white/warm so skin stays true.
- Linear RGB `0..1+`. Values above 1 are fine for beams; keep the face key near 1.
- Contrast sells the look: a dark stage with a lit performer reads better than everything at full.
- Write down what each colour means in this film. If you cannot, it is decoration.

## 5. Intensity budget

Light adds up. Adding a 6-spot rig to a sun at full and an ambient at full will blow out the frame. When you add
a rig:

1. Lower the sun and ambient first (`studio_set_light` with `base.intensity`), or choose a low-intensity rig.
2. Keep the face key around 1-2, beams 2-4, fills 0.3-0.8 (relative units).
3. Check a chorus frame on screen; if the face is white, lower `peak` on the wash and raise only the rims.

Only the key casts shadows (`shadow: hard` or `soft`); set the rest to `nocast`. One shadow source is enough.

## 6. Drafting aids (not a recipe book)

Section conventions worth knowing, and worth breaking with a reason:

| Section | Common instinct | Why it usually works |
| :--- | :--- | :--- |
| Intro | low, one source, settle to the first downbeat | the room exists before the song does |
| Verse | stable base, slow breathe, face lit | lets the lyric be read |
| Pre-chorus | add a colour or a rim, raise the beams | the lift is visible before the drop |
| Chorus | a clear colour event, one rhythm layer on the beams | the release is physical |
| Break | one big change, strobe only for a bar or two | a breath before the last push |
| Bridge | pull back, isolate, one colour | intimacy after volume |
| Final chorus | wider palette, higher beams than the chorus | the same chorus must feel bigger |
| Outro | fade or blackout, keep the final pose readable | the film ends, not the light |

Rules of thumb: one rhythmic layer at a time (a stable base wash + one moving layer + one accent layer); never key
every light on every beat; the vocal keeps a lit face even when the rest dims. These are craft defaults, not a plan.

## 7. Cue sheet columns

Plan with: section, frames, level (low/rising/full/peak), light group (indices or `spots`/`points`/`areas`/`all`),
pattern, timing, `every`, colours, and the intent. Fill `base`/`peak` from the budget above, then write one cue per
line. Ranges must not overlap within the same lights unless the later cue uses `mode: "merge"` intentionally.

## 8. Technical checks

- `studio_light_cues` answer: `events` > 0 for every cue, `keys_written` plausible, `bpm` matches the song.
- One contact sheet per section plus a chorus beat, inspected: face lit, palette coherent, beams read, no white clip.
- Sun/ambient and user lights unchanged unless deliberately edited.
- Undo name `AI: studio_light_cues` or `AI: studio_light_rig` for the last write.
