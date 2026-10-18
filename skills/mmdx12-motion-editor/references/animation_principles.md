# Animation principles for dance

The principles below are the ones that make an existing dance read as alive. Each entry says what it looks like when
missing on a dense contact sheet (every 2 frames), which op fixes it, sensible parameter ranges, and when NOT to apply
it. Read this before writing the fix plan.

A contact sheet is a flip book. Look at the SPACING between poses first (how far the body moves per tile), then the
ORDER in which parts move, then whether anything keeps moving after the main action stops.

## Spacing / ease in and out

**Missing:** the pose changes by the same amount every tile — the limb travels at a constant speed, then stops dead.
The eye reads a machine. On the sheet, an eased move has small gaps leaving the pose, wider gaps mid-move, small gaps
arriving; a linear move has equal gaps everywhere.

**Fix:** `ease` with `curve: "smooth"` (or `ease_in` / `ease_out` where the action only leaves or only arrives),
`only_linear: true` so hand-authored curves survive. It rewrites curves on keys that already exist — it does not add
poses, so it cannot change what the dance does.

**Ranges:** one call per phrase or range; keep `only_linear: true`; use `attack` / `release` 4-8 frames at the range
edges. Do not ease a hit that is supposed to arrive hard — ease the approach into it, not the stop itself.

**When NOT:** popping, locking, robot dance, tutting, and any accent that is a deliberate snap on the beat. Those
sections are supposed to have even or abrupt spacing.

## Overlap and follow-through

**Missing:** parent and child arrive on the same frame. The upper arm, elbow and wrist all stop together, so the arm
reads as one stick; the head moves with the chest like it is welded. On the sheet, every joint reaches the pose on the
same tile.

**Fix:** `overlap` with `chains`, lag 1-3 frames per link. Successive breaking of joints: the parent leads, each child
arrives a little later, and the tip keeps going after the parent has stopped. `chain_lag` in
`studio_motion_quality` should move from ~0 into 1-3.

**Ranges:** `lag` 1-3 (2 is the default and the safe choice). Longer lag on soft, slow phrases; 1 frame on fast
sections or it smears. Chains are ordered parent -> child: `["右腕", "右ひじ", "右手首"]`, `["上半身", "首", "頭"]`.

**When NOT:** contacts (hand to face, hand to hand), a hand holding a prop, and the frame of a hard hit — the hit must
land together. Exclude the contacting hand or skip the range. Also not on robot dance, where rigid simultaneity is the
style.

## Secondary motion (spring / settle)

**Missing:** a body part stops with the parent and never reacts. After a turn the head is still; the wrist does not
overshoot; the chest does not settle. The pose is correct but dead.

**Fix:** `spring` on the parts that should react — head, hands, wrists, upper chest, never the whole body. Lag plus
overshoot and settle around the existing motion. The answer reports `max_deviation_deg`; read it (a sane value is a
few degrees on the head, less on the wrist) and look at the sheet.

**Ranges:** `frequency` 2.5-4 Hz, `damping` 0.35-0.6, `mix` 0.5-1. Start at 3 Hz / 0.45 / 1 and lower `mix` if it
reads floaty. Do not spring feet, legs, or hands during a contact.

**When NOT:** robot dance / popping (the whole point is no secondary motion), fast repeated hits where the spring has
no time to settle, and any bone driving a contact. Two springing parts are plenty; spring on everything is jelly.

## Moving holds / breathing

**Missing:** a held pose is a freeze. On the sheet the tiles are pixel-identical for a second or more — no chest rise,
no head drift, no weight shift. This is the most common stiffness in an MMD motion and the most unnatural, because a
real dancer never freezes.

**Fix:** `breathe` inside the hold: a slow chest / torso cycle, with a small independent head drift. `only_holds: true`
keeps it out of the moving sections. `period` around 105 frames (~3.5 s) reads as calm breathing at 30 fps.

**Ranges:** `amount_deg` 0.8-1.5, `head_drift_deg` 0.6-1.2, `period` 90-120. Keep it under the threshold where it
becomes a visible sway.

**When NOT:** during fast dance, inside a hit or a sharp accent, and in a pose that is held only a few frames (there
is no time for a breath; leave it). Never use breathing to fill a hold that should be tense — a held breath is also
alive, just smaller.

## Anticipation and settle

**Missing:** the body snaps from rest into the big pose with no wind-up, and drops out of it with no recovery. On the
sheet the frame before the accent is already the accent pose, and the frame after is already the next pose.

**Fix:** anticipation and settle are mostly authored in the original motion, so prefer small layers: `rotate` a short
counter-move a few frames before the accent, and `spring` or `ease` after it. Do not retime the phrase to invent
anticipation; if it is truly absent, say so instead of rebuilding the choreography.

**Ranges:** counter-move 3-8 degrees over 3-6 frames; settle via `spring` (2.5-3.5 Hz, damping 0.4-0.5) or `ease` on
the outgoing keys.

**When NOT:** when the choreographer deliberately hits cold (a stab, a lock), or when there is no frame budget before
the accent.

## Arcs

**Missing:** a hand or head travels in a straight line between poses. On the sheet the path cuts a corner; an arc
would bow outward. Real limbs rotate around joints, so their paths are arcs.

**Fix:** there is no arc op. Arcs come from the keys: if the source motion is polygonal, use `overlap` so the joint
chain bends through the move, or add a small mid-move `rotate` / `key_pose` waypoint. Check the result on a dense
sheet; a bowed path reads, a straight one does not.

**Ranges:** a waypoint at the midpoint of the move, 5-15 degrees off the straight line; never more than the move
itself.

**When NOT:** a deliberate straight punch or pointing gesture, where the straight line is the intent.

## The judgement

These principles conflict. Ease makes things soft; a hit needs hardness. Overlap makes chains loose; a contact needs
them together. Spring adds life; too much makes jelly. The animator's job is to decide per section — from the music,
the choreography and the sheet — which principle is missing and how much of it the moment can take. Write the reason
down before the call, and look at the result before the next one.
