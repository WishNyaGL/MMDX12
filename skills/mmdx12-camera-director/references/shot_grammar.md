# Shot grammar

Detailed vocabulary behind `skills/mmdx12-camera-director/SKILL.md`. Everything is expressed in the arguments of
`studio_camera_shots` / `studio_camera_solve`; no scripts, no VMD.

## 1. Shot sizes

`shot` and `end_shot` accept `ecu`, `cu`, `mcu`, `ms`, `cowboy`, `full`, `wide`. The solver fits a fraction of
the subject's body height to the frame.

| Size | Fraction | Contains | Use |
| :--- | :--- | :--- | :--- |
| `ecu` | 0.10 | eyes / a detail | one emotional beat, a whisper line, a held gaze |
| `cu` | 0.22 | face and shoulders | a lyric line, a reaction, a killing part |
| `mcu` | 0.32 | head to chest | singing with some hand gesture |
| `ms` | 0.50 | waist up | default choreography shot in verses and pre-chorus |
| `cowboy` | 0.70 | thighs up | leg work, attitude, a stance |
| `full` | 1.15 | whole dancer | solo dance, traveling, jumps |
| `wide` | 2.20 | formation / stage | group hooks, formations, reveals |

For several models (`subject.models: [0,1,2,...]`) the union is fitted in width and height; `shot` then only adds
margin for `full`/`wide`. `studio_camera_solve` alone also accepts `fill` (subject span as a fraction of the frame
height) when you need an exact span instead of a size name; `studio_camera_shots` has no `fill`.

## 2. Lenses

`fov_deg` is the vertical FOV in integer degrees (MMD stores integers; the solver keeps the framing by changing
distance). Default 25.

- **10-20 telephoto**: compresses depth and background, isolates one performer, flattering faces. Emotional solos,
  vocal climaxes, tight gesture inserts. Pair with `focus.aperture` 0.75-0.85.
- **20-28 natural**: the human-eye feel. Duos, `mcu`/`ms` choreography, most of a verse. Aperture 0.65-0.75.
- **28-40 wide**: stage geometry, full formations, fast reveals, `full`/`wide`. Aperture 0 on group shots.

Keep each shot's lens coherent with its size: a `wide` at 12 degrees and a `cu` at 38 degrees both look wrong.

## 3. Movement vocabulary

Passed as `move`; the end pose is derived from `start`, `end`, `shot`/`end_shot`, `angle`/`end_angle`,
`fov_deg`/`end_fov_deg` and `amount`.

| Move | What changes | Use |
| :--- | :--- | :--- |
| `static` | nothing (one framing) | let the dance read; a held emotional beat |
| `dolly_in` | distance | draw the viewer into a line; verse into chorus |
| `dolly_out` | distance | release, reveal the stage, end of a phrase |
| `zoom_in` | fov, position fixed | graphic push, flatter than a dolly, accents |
| `zoom_out` | fov, position fixed | pull the context back without moving |
| `dolly_zoom` | distance + fov | vertigo, disorientation, a break or a drop |
| `crane_up` | pitch + target height | reveal a formation, a rise into the chorus |
| `crane_down` | pitch + target height | descend onto a soloist, an intimate landing |
| `truck_left` / `truck_right` | lateral slide, heading fixed | parallax along a line or a formation |
| `arc_left` / `arc_right` | orbit around the subject | swing, a 2-bar build, a new angle without a cut |
| `follow` | re-solves on the moving subject | travelling dancers, crossovers |
| `push_accent` | static driven by `accents` | chorus downbeat punctuation |

`amount` is crane degrees (default 25), truck world units (default 0.6 x subject height) or arc degrees (default
35). Arcs write keys roughly every 15 frames.

## 4. Curves

`curve` (preset name, default `ease_in_out`) is the easing between the shot's keys. Presets available:
`linear`, `ease_in_out`, `ease_in`, `ease_out`, `smooth`, `fast_start`, `slow_start`.

- `ease_in_out` - default; a settled start and end.
- `smooth` - floating, weightless moves (cranes, arcs, slow pushes).
- `fast_start` - into an accent or a hit; also snappy trucks.
- `linear` - constant-speed rails only; mechanical elsewhere.
- Avoid stacking many short keys with alternating curves ("Bezier kicks"); one curve per shot is the point.

## 5. Cutting and continuity

- A hard cut is `end + 1 == next start`; `studio_camera_shots` writes keys on adjacent frames. There is no cut
  flag and no gap.
- Cut on downbeats and section boundaries. `studio_audio_analysis` gives `beats`, `downbeats`, `sections`.
- Shot length by energy: verse 2-4 bars, chorus 1-2 bars, accents on strong `onsets`. A pre-chorus can shorten
  progressively toward the drop.
- **180-degree rule**: within a scene keep the subject on one side of the action axis. Cross only on a section
  change or inside a moving shot (`arc_*`, `truck_*`).
- **Screen direction**: if the dancer moves frame-right, keep moving frame-right across the cut unless the story
  turns.
- **No wiper**: consecutive shots must not alternate yaw left/right just to add motion. Change the size, lens,
  height or move instead.
- **No jump cut**: consecutive shots differ by >= 2 shot sizes, or by >= 30 degrees of angle, or by a clear move.

## 6. Song structure pacing

| Section | Shot sizes | Length | Camera behaviour |
| :--- | :--- | :--- | :--- |
| Intro | wide -> full | 2-4 bars | slow crane down or dolly in, settle before beat 1 |
| Verse | ms / mcu / cu | 2-4 bars | intimate, dolly_in/out, mostly static, floating curves |
| Pre-chorus | mcu -> full | 1-2 bars | rising: crane_up, dolly_in, accelerating cuts |
| Chorus | full / wide / cu alternating | 1-2 bars | impact cuts on downbeats, push_accent, arcs, group fits |
| Break | full / wide / ms | 1 bar or less | crane, truck, dolly_zoom; strobe-like cutting sparingly |
| Bridge | cu / ecu | 2-4 bars | telephoto, slow arc, shallow DoF, long holds |
| Final chorus | full / wide -> ms | 1-2 bars | new centre, formation fit, crane rise |
| Outro | full / wide | 2+ bars | static or slow dolly_out, hold the final pose |

Match the cut rate to energy; do not cut faster than the dance can be read.

## 7. Choreography mapping

From `studio_motion_events` (frames at 30 fps):

- `jumps` -> `crane_up` or `dolly_out` to keep the apex in frame; cut just before the jump.
- `crouches` -> `crane_down` or a low `ms`; a beat accent on the landing (`accents`).
- `hand_raises` -> `mcu`/`cu` toward that hand's side; `arc_*` toward the gesture.
- `turns` -> `arc_*` in the same direction, or hold and let the turn read; never counter-rotate.
- `holds` -> `static` or a very slow `dolly_in`; do not cut inside a hold.
- `travel.max_speed` high -> `follow: true` with `follow_step` 8-12, or `truck_*` alongside.
- `energy` windows -> cut rate and move amplitude; high energy allows faster cuts and bigger moves.
- `formation` / `crossovers` -> `subject.models` with every member, `wide`/`full`, `crane_up`; cut at the crossover.

## 8. DoF and lead room

`focus: {"aperture": a}` on a shot writes a DoF focus key at the shot's start targeting the subject's head. Use
0 for groups and wide stage shots, 0.6-0.85 for close shots, and do not change aperture mid-shot. `lead` (0-0.2)
adds look room in the subject's facing direction - useful for profile and three-quarter angles, not for a
straight-on symmetrical shot. Dutch `roll_deg` is an accent only: <= 8 degrees, and return to level on the cut.

## 9. Multi-character rules

- Fit the group as a union (`subject.models`) rather than guessing a distance.
- At a formation change or crossover, widen first (or crane up), then cut to a solo.
- Keep the featured member's face toward the lens: `facing_camera_deg` in `studio_frame_check` near 0 means
  looking into the lens; large values mean profile/back.
