# Critique

The review is where the motion is humanized. Look at the before / after contact sheets first, then answer these
questions in writing. Be specific: name the frame, name the range, say what is wrong. "Feels smoother" is not a
critique.

## The rubric

**Spacing and timing**
- Does the body leave a pose softly and arrive softly where it should, and still hit hard on the beat?
- Is any move still at constant speed? Is any stop still a dead stop on a frame that is not a hit?
- Did the fix change the timing of the phrase? It must not.

**Follow-through and weight**
- Does every joint still arrive with its parent? Is the wrist / head a frame or two behind where it should be?
- Do the arms read as a chain (shoulder -> elbow -> wrist) or as one stick?
- After a turn or a hit, does anything settle, or does the body simply stop?
- Does the weight read: does the chest follow the hips, does the head follow the chest?

**Holds**
- Are the dead holds alive (breath, small head drift), and subtle enough not to be a sway?
- Is any breath running during fast dance where it should not?

**Contacts and feet**
- Do hands still meet the face / the other hand / the prop? Compare the same frames before and after.
- Are the feet still planted? No sliding, no floating, no leg twist after `overlap` / `spring`.
- Did the IK chains stay untouched?

**Character and intent**
- Does the dance still look like the same dance, with the same accents and the same attitude?
- Are the deliberately sharp sections still sharp (popping, locking, robot dance, beat hits)?
- Did you lose the choreographer's contrast: does everything now ease and settle the same way?

## The failures and how to fix them

| What you see | What it means | Fix |
| :-- | :-- | :-- |
| Everything glides, nothing lands | over-eased | undo the `ease` pass; keep `only_linear` and raise `attack`; leave the hits sharp |
| Hits still land but the approach is soft | correct ease, too wide | keep, but shrink the eased range to stop before the hit frame |
| Hands float and wobble after a move | too much spring | lower `mix` to 0.4-0.6, raise `damping` toward 0.6, or drop the wrists |
| Head keeps bobbing in a fast section | spring on a rhythm that has no room | undo the spring on 頭 for that range; keep it in the slow section |
| The whole body moves like jelly | spring / overlap everywhere | undo, apply to one chain only |
| Every joint arrives together still | overlap not applied or lag 0 | add `overlap` with lag 2 on the arm and torso chains |
| The wrist is late even at a contact | lag broke the contact | exclude the hand, or the range, from `overlap` |
| Feet slide or twist | legs / feet touched or the center moved | undo; never edit leg bones; check 足ＩＫ |
| The hold still looks frozen | breathe range missed the hold, or `only_holds` found none | read the `holds` from `studio_motion_quality` and target them exactly |
| The hold visibly sways | too much breathing | lower `amount_deg` to 0.8 and `head_drift_deg` to 0.6 |
| The dance feels slower / late | retime or too-long lag | undo the retime; lag back to 1 |
| Numbers improved, motion reads worse | judged by the score | trust the sheet; undo the pass |
| A section lost its snap | that section was style, not defect | restore it and add it to the "deliberately left sharp" list |

## Example critique of an over-processed result

Draft: a 900-frame idol dance, first pass `ease` over the whole motion, then `overlap` lag 3 on both arms and the
torso, `spring` on head, both hands and upper chest, `breathe` over every range.

> The pass is technically alive and emotionally dead. The open hit at 1:12 no longer hits: the ease rewrote the
> approach to the arm accent, and the arm now drifts into the pose over six frames where the choreography wanted a
> snap on the kick. The elbows arrive three frames after the wrists on the fast eighth-note section, so the arms smear
> instead of articulating. The head springs after every step in the chorus, which was already fast enough to carry
> itself — the head now lags the beat and reads drunk. The hands wobble for four frames after every pose; at 1:44 the
> right hand is supposed to be at the cheek and it floats 4 cm off it. The 2:20 freeze, which was the real defect, is
> better: the chest and the small head drift make it alive. But the same breathing is also running through the whole
> chorus, where it is invisible at best and a sway at worst. The stiffness score dropped 38 points and the dance is
> worse.

Revision notes:

- Undid the global `ease`; re-applied it only to 300-360 and 600-690, with `only_linear: true`, leaving the 1:12 hit
  untouched. The hit lands again.
- `overlap` lag 2 on `["右腕","右ひじ","右手首"]` and `["上半身","首","頭"]`, only in the slow verse; the chorus is
  back to simultaneous joints.
- `spring` only on 頭 in 480-600, `mix` 0.6, damping 0.55. The hands are flat again, and the cheek contact at 1:44
  is restored.
- `breathe` limited to the 2:20 hold and the 1:56 pose, `amount_deg` 1.0, `head_drift_deg` 0.7. No breathing in the
  chorus.
- Second pass sheet: the verse breathes, the chorus still snaps, the freeze is alive, the contact holds. Flagged for
  a third pass: the 2:40 turn still stops without a settle, and the arms were deliberately left rigid there because
  the choreography is a lock.

## After the critique

Revise only what the critique found. Do not re-process the whole motion in response to one bad range. Re-render the
same sheet, put before and after next to each other, and write two or three lines of animator's notes: what changed,
why, and what is still unresolved.
