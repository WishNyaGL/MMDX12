# Lighting grammar

Detailed vocabulary behind `skills/mmdx12-lighting-director/SKILL.md`. All arguments belong to `studio_light_rig`,
`studio_light_cues`, `studio_set_light` and `studio_scene_info`; no scripts.

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

| Rig | Builds | Default count | Best for |
| :--- | :--- | :--- | :--- |
| `key_fill_rim` | key spot 35 deg to one side, soft fill area opposite, rim spot behind-above | 3 | the safe base look; solo and small groups |
| `front_wash` | area lights in an arc in front | 3 | even, soft, readable faces; ballads |
| `top_spots` | spots above/in front, `aim` `subject` or `down` | 6 | pop chorus beams from above |
| `side_bars` | two columns of point lights left/right | 4 per side | vertical bars, club/concert depth |
| `back_beams` | spots behind aimed up/forward, sway phases spread | 4 | silhouettes, halo, concert backlight |

Common arguments: `subject` (character model index; default the first character), `radius` and `height` in
character heights (default 1.6), `color` or `palette` (cycled per light), `intensity`, `cone_deg` (spot outer;
inner = 0.7 x outer), `size` `[w,h]` (area), `shadow` (`nocast|hard|soft`), and `aim` (`subject|down`) for
`top_spots`. Positions scale with the character's height, sampled at the current frame, so set the playhead to a
representative pose before building.

## 3. Patterns

| Pattern | What it does | Timing | Use |
| :--- | :--- | :--- | :--- |
| `pulse` | intensity bump then decay | `beat`, `downbeat`, `onset` | chorus beats, accents |
| `chase` | lights in sequence | `half`, `beat` | pre-chorus rise, group energy |
| `alternate` | two groups swap | `beat`, `bar` | simple left/right movement |
| `strobe` | very short on/off | `onset` | break accents only, 1-2 bars |
| `sweep` | aim moves across `aim_points` / `aim_targets` | `bar` | beams scanning the stage |
| `color_cycle` | colour changes | `section`, `bar` | palette shifts |
| `breathe` | slow intensity swell | `bar`, `section` | verses, ambient life |
| `fade` | `from` to `to` colour/intensity | `section` | transitions, outro |
| `section_levels` | one level per song section | `section` | quick global baseline |
| `blackout` | intensity to zero | `section` | the very end |

`timing` names the event grid: `beat` (default), `half`, `downbeat`, `bar`, `onset`, `section`. `every` uses each
Nth event, `offset` shifts the phase, `min_strength` filters onsets (default 0.5), `decay` is frames (default
0.6 x beat). Events are rounded to integer frames and duplicates dropped. `base` / `peak` default to the light's
current intensity x0.3 / x1.0; set them explicitly when you add lights.

`aim_targets` accepts `head`, `center`, `left`, `right`, `audience`; `aim_points` accepts world `[x,y,z]`.
`sweep` and `pulse` on the same lights at the same timing fight each other - layer on different lights.

## 4. Colour theory

- Warm key (amber/straw) with a cool rim (blue/cyan) is the default readable concert palette.
- 2-3 colours per section. Change them at section boundaries, not every bar.
- Saturated colours on beams, rims, bars and backlight; keep the face key near white/warm so skin stays natural.
- Linear RGB `0..1+`. Values above 1 are fine for beams; keep the face key near 1.
- Contrast sells the look: a dark stage with a lit performer reads better than everything at full.

## 5. Intensity budget

Light adds up. Adding a 6-spot rig to a sun at full and an ambient at full will blow out the frame. When you add
a rig:

1. Lower the sun and ambient first (`studio_set_light` with `base.intensity`), or choose a low-intensity rig.
2. Keep the face key around 1-2, beams 2-4, fills 0.3-0.8 (relative units).
3. Check a chorus frame on screen; if the face is white, lower `peak` on the wash and raise only the rims.

Only the key casts shadows (`shadow: hard` or `soft`); set the rest to `nocast`. One shadow source is enough.

## 6. Music mapping

| Section | Energy | Pattern | Timing | Notes |
| :--- | :--- | :--- | :--- | :--- |
| Intro | low | `breathe` / `fade` | `section`/`bar` | settle to the first downbeat |
| Verse | low | `breathe`, slow `chase` | `bar` | face always lit, palette stable |
| Pre-chorus | rising | `chase` / `fade` up | `half` | add a colour, raise `peak` |
| Chorus | full | `pulse` + `alternate`/`chase` | `beat` | one rhythmic layer per light group |
| Break | peak | `strobe` / `sweep` | `onset`/`bar` | strobe max 1-2 bars; warn about photosensitivity |
| Bridge | low | `breathe`, single `color_cycle` | `section` | pull back, isolate |
| Final chorus | full | as chorus, wider palette | `beat` | raise the beams |
| Outro | fall | `fade` / `blackout` | `section` | keep the final pose readable |

Rules of thumb: one rhythmic layer at a time (a stable base wash + one moving layer + one accent layer); never
key every light on every beat; the vocal section keeps a lit face even when the rest dims.

## 7. Cue sheet columns

Plan with: section, frames, level (low/rising/full/peak), light group (indices or `spots`/`points`/`areas`/`all`),
pattern, timing, `every`, colours, and the intent. Fill `base`/`peak` from the budget above, then write one cue per
line. Ranges must not overlap within the same lights unless the later cue uses `mode: "merge"` intentionally.

## 8. Verification checklist

- `studio_light_cues` answer: `events` > 0 for every cue, `keys_written` plausible, `bpm` matches the song.
- One screenshot per section plus a chorus beat, inspected: face lit, palette coherent, beams read, no white clip.
- Sun/ambient and user lights unchanged unless deliberately edited.
- Undo name `AI: studio_light_cues` or `AI: studio_light_rig` for the last write.
