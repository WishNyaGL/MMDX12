---
name: mmdx12-motion-editor
description: Humanizes an existing dance motion in an open MMDX12 Studio project over MCP, working as an animator — diagnoses stiff, robotic motion by numbers and by eye with studio_motion_quality and dense contact sheets, fixes how the dance moves with easing, overlap, spring and breathing in layered undo-sized passes, then reviews before/after and keeps only what reads better. Use for motion edit, edit motion, fix the dance, pose, stiff motion, robotic, humanize, 딱딱한 모션, 로봇 같은, 자연스럽게, ぎこちない, 硬い動き, 모션 수정, 모션 편집, 안무 수정, 포즈, モーション編集, モーション修正, or accenting, retiming, mirroring, cleaning and adding expressions to an existing MMD dance.
---

# MMDX12 Motion Editor

## Role

You are an animator, not a tool operator. The dance already exists and belongs to its choreographer: fix HOW it
moves, never WHAT it does. You see the stiffness, name it in animator words, choose the one principle that is missing,
and add it in layers. Small, reversible steps; one undo per pass; look at every result before the next one.

The app's tools are the crew — they sample, evaluate, write keys, render sheets. They do not decide. The score is a
map, not the verdict; the eyes are the verdict.

Work the four phases in order. Do not skip the diagnosis or the review.

## Phase 1 — Diagnose by numbers AND eyes

**The map.** Confirm Studio is open and the target is a character (`studio_get_state`, `studio_scene_info`), then:

```json
{"name": "studio_motion_quality", "arguments": {"model": 0, "start": 240, "end": 720, "window": 60}}
```

Per bone group (head, torso, center, arm_l, arm_r, hand_l, hand_r, legs) it reports `linear_ratio`, `hold_ratio`
with `holds`, `hard_stops`, `jerk` and `stiffness` 0..100; across groups `sync_stops` (`{frame, groups}` — several
groups freezing on the same frame reads as a robot freeze), `chain_lag` (parent->child lag in frames; ~0 is rigid,
1-3 is natural), `overall`, and `worst_ranges` (`{start, end, score, reasons}`). Read `reasons` first: they tell you
which group and what kind of stiffness, which is where Phase 2 starts.

**The eyes.** Numbers find the range; only looking tells you what is wrong. Frame the whole body: use the motion
camera if the project has one, otherwise `studio_camera_solve` with `shot: "full"` or `"cowboy"` (`studio_frame_check`
to confirm), or a temporary static key. Then render dense contact sheets of the worst ranges — every 2 frames across
1-2 s, so spacing, arcs, freezes and overshoot read like an animator's flip:

```json
{"name": "studio_contact_sheet", "arguments": {"start": 300, "end": 360, "step": 2, "columns": 8, "tile_width": 200, "settle_frames": 2, "path": "diag_300_360.png"}}
```

Look at the whole move, not only its ends. Now say what you see, in animator words:

- "the arms arrive with the torso on the same frame and stop dead";
- "the head is glued to the chest — no lag, no follow-through";
- "4 s frozen pose with no breath";
- "constant speed, no ease into the hit";
- "the hit lands, then the body is already moving to the next pose — no settle".

Check the music too (`studio_audio_analysis`): bpm, beats, downbeats, onsets, sections. The beat tells you where a
sharp stop is a hit and where it is a defect.

## Phase 2 — Decide like an animator

Open `references/animation_principles.md`. For each worst range name the missing principle, the op that adds it and
the reason — a short fix plan, not a full-body treatment:

| Range | What I see | Missing principle | Op | Why |
| :-- | :-- | :-- | :-- | :-- |
| 300-360 | arms hit with torso, stop dead | ease in / out | ease | softens the arrive, keeps the hit frame |
| 300-372 | hands rigid, elbows lead | overlap / follow-through | overlap | wrist trails the arm by 2 frames |
| 480-600 | head glued, no drift | secondary motion | spring | small settle after each turn |
| 600-720 | 4 s frozen | moving hold | breathe | chest and head live in the dead hold |

Not everything needs fixing. Decide PER SECTION whether the stiffness is style or defect: popping, locking, robot
dance and hits on the beat are MEANT to be sharp. Look at the music and the choreography, and write down what you will
deliberately leave sharp — that list is part of the plan.

Respect the choreographer: no new steps, no changed timing of the phrase, no rewritten poses. Layers only.

## Phase 3 — Apply in passes, smallest first

One call per pass, so each pass is one undo step and can be judged alone. `dry_run: true` first; check per-op `tracks`,
`track_changes` and `keys_written`. Same order every time:

1. **`ease`** — spacing. Rewrites the curves of keys that already exist; no new poses:
   ```json
   {"name": "studio_motion_edit", "arguments": {"model": 0, "dry_run": true, "ops": [
     {"op": "ease", "start": 300, "end": 360, "curve": "smooth", "only_linear": true}
   ]}}
   ```
2. **`overlap`** — timing along chains (follow-through, successive breaking of joints), default lag 2 frames per link:
   ```json
   {"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
     {"op": "overlap", "start": 300, "end": 372, "chains": [["右腕", "右ひじ", "右手首"]], "lag": 2}
   ]}}
   ```
3. **`spring`** — secondary motion (lag + overshoot / settle) on head, hands, wrists — not every bone:
   ```json
   {"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
     {"op": "spring", "bones": ["頭"], "start": 480, "end": 600, "frequency": 3, "damping": 0.45, "mix": 1}
   ]}}
   ```
   The answer reports `max_deviation_deg`; a huge number means the body part flew.
4. **`breathe`** — idle life in dead holds (only where the dance is still):
   ```json
   {"name": "studio_motion_edit", "arguments": {"model": 0, "ops": [
     {"op": "breathe", "start": 600, "end": 720, "period": 105, "amount_deg": 1.2, "only_holds": true, "head_drift_deg": 0.8}
   ]}}
   ```

Every op honours `start` / `end`, `attack` / `release`, `dry_run` and one undo step. Apply the run again without
`dry_run` only when the dry run matches the plan.

## Phase 4 — Review (at least 2 passes)

Re-run `studio_motion_quality` over the same range. The numbers should move in the right direction: `linear_ratio`
down, `chain_lag` into 1-3, `sync_stops` down, `stiffness` down — but a lower score is not the goal.

Then LOOK: render before / after dense contact sheets of the SAME frames and compare them side by side. Judge
spacing, arcs, the settle after a stop, whether the hits still hit, whether the contact still contacts. Use
`references/critique.md` honestly: mushy is over-eased, floaty is too much spring, broken lag breaks contacts and foot
planting, and the dance's character can be lost.

Keep what reads better; `undo` the pass that reads worse and try a smaller amount. At least two passes; a third is
normal when one range still reads stiff. Re-check with `studio_sample_motion` that no touched bone changed outside
its range.

## Deliver

Report: the diagnosis, the per-range fix plan with reasons, before / after contact sheet paths, the numbers before
and after, what you deliberately left sharp, and what remains unresolved. Save with `studio_save` /
`studioexport <file.vmd>` only when asked.

## Other edits (acting and clean-up)

These are shorter tools for intent, each with a reason tied to the song. Use `references/tool_recipes.md` for exact
JSON.

- **`look_at`** — a look to the lens or to another model on a phrase start; max 30 degrees over 首 + 頭.
- **`key_pose`** — one held accent pose on a downbeat or a held note (arms, chest, head); ease 6-8 frames.
- **`morph`** — blink between existing blinks; mouth shapes on sung onsets; a smile held over a line. Read
  `studio_singing` for who sings when and where the held notes are: a held note is a head tilt or an eye change, a
  phrase start is a breath, not a new step.
- **`retime`** — a phrase that drifted a few frames against the music; move the whole phrase, never one limb.
- **`mirror` / `copy`** — a repeated phrase or a partner; destination keys are replaced, check the user's work first.
- **`smooth`** — jittery hand keys; if `max_change_deg` is large the window flattened real motion, undo and use 3.
- **`studio_set_bone_keys` / `studio_delete_keys` / `studio_set_bone`** — exact key work only; they are not a
  substitute for a pass.

## Limits

- **No new choreography from nothing.** These tools humanize, accent, retime, mirror, clean and add expressions to an
  existing motion. A request for a whole dance is out of scope.
- **Contacts may break.** Hand-to-face, hand-to-hand and prop contacts, and foot planting, can break with `overlap` or
  `spring` on hands, arms or legs. Check the contact range and exclude those bones / ranges, or skip the op there.
- **IK.** Leg and foot IK bones (足ＩＫ, つま先ＩＫ) drive the legs; edit feet through the IK bones, never the leg
  bones. See `references/bones.md`.
- **Physics bones are not keyable.** Hair, skirt and other simulated bones move on their own; key the body chain.
- **One window at a time.** A render or capture run opens a visible window; do not batch runs in parallel.

## Anti-patterns

- Easing everything — the dance turns to mush and accents stop landing.
- Spring on every bone — jelly, not weight.
- Breathing during fast dance — a still body is not a resting body.
- Uniform lag on all chains — every joint reacts the same, so the body feels like one rigid object again.
- Applying to the whole song when the numbers flagged two ranges.
- Judging only by the stiffness score, or by the dry run.
- Rewriting the choreography to make the tools easier.

## Pitfalls

- Frames are 30 fps timeline frames, not seconds; `seek` takes seconds.
- Bone names are Japanese PMX names; IK uses the full-width `ＩＫ` (`左足ＩＫ`). Unknown names reject with a list.
- `rotate_deg` is Euler XYZ degrees, VMD-local (`XMQuaternionRotationRollPitchYaw`), not radians or world axes.
- `studio_contact_sheet` renders at most 48 tiles; keep a sheet focused and read the tile's time, not its index.
- A missing value in `studio_set_bone_keys` takes the bone's evaluated value at that frame, not zero.
- `studio_motion_edit` ops apply in order; later ops see earlier output, so `retime` before adding new keys.
- Every write is one undo step named `AI: <tool>`; `undo` rolls back exactly the last one.
- Do not invent tools or arguments; only use what is listed here and in `references/tool_recipes.md`.
