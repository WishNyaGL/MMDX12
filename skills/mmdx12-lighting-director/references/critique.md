# Critique

Look at the contact sheets first, then answer these questions in writing. Name the frame and say what is wrong. The
light is judged by what it does to the performer, not by the cue list.

## The rubric

**The face and the singer**
- Is the face readable in every vocal moment? Can you see the eyes?
- Is the person singing the person the light favours?
- In a solo, do the others step back without disappearing into a black hole?
- During a held note, does the light sustain, or does it tap along with the beat?
- Are breaths and instrumental passages allowed to change the air?

**Palette and treatment**
- Does the colour flow across the song match the colour script, section by section?
- Does each colour mean something in this film, or is it decoration?
- Are there two or three colours per section, changing where the song changes?
- Does the light do what the logline promised?

**Contrast and exposure**
- Is there real darkness, or is everything at the same level?
- Is anything blown out — a white face, a clipped beam, a background brighter than the performer?
- Is the background competing with the subject?
- Does the contrast level match the treatment (intimate = dark, euphoric = full)?

**Rhythm and readability**
- Can you read the rhythm layer, or is it noise?
- Is there one stable base, one moving layer, one accent — or everything at once?
- Do the cues land on beats and phrase starts, or drift?
- Does the rhythm stop where the treatment wanted restraint?

**Craft**
- Do the beams read in this aspect ratio, or do they leave the frame?
- Are shadows doing something, and is there only one shadow source?
- Do the follow spots actually land on the head as the performer moves?
- Is the sun/ambient still doing its job, or has the rig buried it?

## The failures and how to fix them

| What you see | What it means | Fix |
| :-- | :-- | :-- |
| The face is a white blob | key too hot, or too many lights adding up | lower key `base`/`peak`; lower the sun/ambient; keep the key out of the rhythmic layer |
| The singer is a silhouette during their line | backlight overpowering the key | raise the follow spot, lower the back beams, or move the key |
| The stage is uniformly grey | no contrast | lower the ambient, let the edges fall to black, keep one source bright |
| The chorus is the same colour as the verse | no colour event | change palette on the section boundary; one clear shift, not a gradient |
| A rainbow | too many colours, changing too often | cut to two or three colours per section; hold them |
| The rhythm layer is a strobe everywhere | pattern applied to all lights | restrict the cue to beams/rims; keep the base wash stable |
| Nothing moves in the chorus | no rhythm layer at all | add one pulse or chase on the beams only, on the beat |
| Beams exit the frame | aspect or aim wrong | re-aim or lower the beams; check the project's aspect |
| The follow spot misses the face | `aim_mode` or target wrong | set `aim_mode: "target"`, `target_model`, `target_part: "head"` |
| Every section feels the same | template, no arc | rewrite the colour script around the emotional arc, then re-cue |
| The light is beautiful but the song is not in it | no singer awareness, no restraint | rebuild around phrases, breaths and held notes |

## Example critique of a weak draft

Draft: a 5-member song, `top_spots` + `back_beams`, `pulse` on every light on every beat for the whole chorus, a
rainbow `color_cycle` every bar, one front wash at a fixed intensity.

> The chorus is loud in the wrong way. Every light pulses on every beat, so there is no base, no depth, and the
> feet of the beams are lost in the wash. The colour changes every bar, which means no colour means anything. In the
> verse the wash is the same intensity as the chorus, so the drop does not exist; the only thing that changes is
> that more lights are on. At 1:44 the solo line is sung by member 3, but the spot is still on member 0 — the light
> is not listening. The faces are readable, technically, but they are flat and slightly green from the magenta key.
> There is no darkness anywhere.

Revision notes:

- Split the chorus into a stable base wash (key + front area, no pulse) and a rhythm layer on the back beams only.
  The chorus now has depth and the drop has a base to lift from.
- Cut the verse wash to 40% and the beams off; the verse is now a different place from the chorus.
- Replaced the per-bar rainbow with two colours per section, changing only at the section boundary.
- Keyed follow spots from `studio_singing`: member 3 comes up 6 frames before 1:44, the others dim 20%; unison
  returns to the group wash.
- Changed the key from magenta to cool white; the beams keep the saturation, the skin is natural.
- Second pass sheet: the verse-to-chorus contrast reads, the solo is on its singer, the palette is coherent. Still
  to fix: the break strobe is one bar too long — cut it next pass.

## After the critique

Revise only what the critique found. Do not re-light the whole song in response to one bad frame. Re-render the
same sheet, compare before and after, and write two or three lines of director's notes: what changed, why, and what
is still unresolved.
