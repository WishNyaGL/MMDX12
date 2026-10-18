# Shot grammar

The vocabulary behind `skills/mmdx12-camera-director/SKILL.md`. Everything is expressed in the arguments of
`studio_camera_shots` / `studio_camera_solve`; no scripts, no VMD. Use this when you need the exact word for an
intention, not as a source of intentions.

## 1. Shot sizes

`shot` and `end_shot` accept `ecu`, `cu`, `mcu`, `ms`, `cowboy`, `full`, `wide`. The solver fits a fraction of
the subject's body height to the frame.

| Size | Fraction | Contains | Typical intent |
| :--- | :--- | :--- | :--- |
| `ecu` | 0.10 | eyes / a detail | one emotional beat, a whisper line, a held gaze |
| `cu` | 0.22 | face and shoulders | a lyric line, a reaction, a killing part |
| `mcu` | 0.32 | head to chest | singing with some hand gesture |
| `ms` | 0.50 | waist up | a body in a verse, readable choreography |
| `cowboy` | 0.70 | thighs up | leg work, attitude, a stance |
| `full` | 1.15 | whole dancer | solo dance, travelling, jumps |
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

| Move | What changes | Intent |
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
| `arc_left` / `arc_right` | orbit around the subject | swing, a build, a new angle without a cut |
| `follow` | re-solves on the moving subject | travelling dancers, crossovers |
| `push_accent` | static driven by `accents` | beat punctuation |

`amount` is crane degrees (default 25), truck world units (default 0.6 x subject height) or arc degrees (default
35). Arcs write keys roughly every 15 frames.

Every move is relative to the shot's OPENING framing (solved at `start`): a non-follow move does not chase the
dancer, so a dancer who travels far (`travel`, `max_speed` in `studio_motion_events`) needs `follow: true` (any move
can follow) or a shorter shot. `teleports` (frames where the dance re-places the performer) must be cut points:
never let a shot span one.

## 4. Curves

`curve` (preset name, default `ease_in_out`) is the easing between the shot's keys. Presets available:
`linear`, `ease_in_out`, `ease_in`, `ease_out`, `smooth`, `fast_start`, `slow_start`.

- `ease_in_out` — a settled start and end.
- `smooth` — floating, weightless moves (cranes, arcs, slow pushes).
- `fast_start` — into an accent or a hit; also snappy trucks.
- `linear` — constant-speed rails only; mechanical elsewhere.
- Avoid stacking many short keys with alternating curves ("Bezier kicks"); one curve per shot is the point.

## 5. Cutting and continuity

- A hard cut is `end + 1 == next start`; `studio_camera_shots` writes keys on adjacent frames. There is no cut
  flag and no gap.
- Cut where the song gives you a reason: a phrase start, a breath, a change of who is singing, a gesture landing.
  Downbeats are one reason among many; cutting on all of them is a metronome.
- **180-degree rule**: within a scene keep the subject on one side of the action axis. Cross only on a section
  change or inside a moving shot (`arc_*`, `truck_*`).
- **Screen direction**: if the dancer moves frame-right, keep moving frame-right across the cut unless the story
  turns.
- **No wiper**: consecutive shots must not alternate yaw left/right just to add motion. Change the size, lens,
  height or move instead.
- **No jump cut**: consecutive shots differ by >= 2 shot sizes, or by >= 30 degrees of angle, or by a clear move.

## 6. Drafting aids (not a recipe book)

These are conventions worth knowing, and worth breaking with a reason. They are the crew's experience, not your
taste. The treatment decides which of these apply.

**Section pacing, as a starting instinct:**

| Section | Common choice | Why it usually works |
| :--- | :--- | :--- |
| Intro | wide -> full, slow crane or dolly | establishes geography before the first vocal |
| Verse | ms / mcu / cu, mostly settled | lets the lyric and the dance be read |
| Pre-chorus | mcu -> full, rising or accelerating | the audience feels the lift before the drop |
| Chorus | full / wide alternating with cu | the contrast between body and face is the chorus |
| Break | full / wide / ms, one big move | an instrumental is a place to change the air |
| Bridge | cu / ecu, telephoto, long holds | intimacy after volume |
| Final chorus | new centre, wider than before | the same chorus must feel bigger, not identical |
| Outro | full / wide, static or slow retreat | hold the last image |

**Choreography mapping** from `studio_motion_events`:

- `jumps` -> keep the apex in frame; cut before the jump if the rise is the point.
- `crouches` -> descend with the body, or hold and let it drop out of frame on purpose.
- `hand_raises` -> decide whether the hand or the face is the subject; do not frame between them.
- `turns` -> travel with the turn or hold and let it read; never counter-rotate for no reason.
- `holds` -> do not cut inside a hold unless the cut is the point.
- `travel.max_speed` high -> `follow: true`, `follow_step` 8-12, or `truck_*` alongside.
- `energy` windows -> a reason to change the cut rate or move amplitude, not an instruction.
- `formation` / `crossovers` -> a chance to re-establish the group; cut at the crossover only if it means something.

**DoF and lead room:** `focus: {"aperture": a}` writes a DoF focus key at the shot's start targeting the subject's
head. 0 for groups and wide stage shots, 0.6-0.85 for close shots; do not change aperture mid-shot. `lead` (0-0.2)
adds look room in the subject's facing direction — useful for profile and three-quarter angles, not for a
straight-on symmetrical shot. Dutch `roll_deg` is an accent only: <= 8 degrees, and return to level on the cut.

**Multi-character:** fit the group as a union (`subject.models`) rather than guessing a distance. At a formation
change or crossover, widen first (or crane up), then cut to a solo. Keep the featured member's face toward the lens:
`facing_camera_deg` in `studio_frame_check` near 0 means looking into the lens; large values mean profile/back.
