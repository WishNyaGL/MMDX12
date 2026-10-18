# MMD bone names and morphs

Bone names are PMX names exactly as they appear in the model (Japanese, including the full-width `ＩＫ`). Read the
model's keyed bones with `studio_get_bone_keys` and its morph keys with `{morphs: true}`; unknown names written
through `studio_set_bone_keys` are rejected with the model's list. The names below are the standard MMD set.

## Axis convention

`rotate_deg` is `[x, y, z]` Euler degrees in the bone's VMD-local frame, built like `studio_set_bone`
(`XMQuaternionRotationRollPitchYaw(x, y, z)`):

- **x = pitch** - positive tilts the bone's forward axis down / bends forward (for 上半身: lean forward).
- **y = yaw** - positive turns left (around the bone's up axis); for the head, turn to the character's left.
- **z = roll** - positive rolls / tilts sideways to the character's left.

Values are degrees, not radians. On `studio_motion_edit` `rotate` the default `space: "local"` is the parent frame
(like VMD); `space: "world"` converts a world-space offset into the local frame.

## Root / centre

| Bone | Meaning | Typical edit |
| :--- | :--- | :--- |
| 全ての親 | All-parent: moves the whole model | Offset for a whole-body shift or turn; translate is world space |
| センター | Centre: body translation and lean | Translate to move the dancer; do not move without a reason (feet slide) |
| グルーブ | Groove: secondary body motion | Small rotations for bounce / sway; usually left alone |

## Torso

| Bone | Meaning | Typical edit |
| :--- | :--- | :--- |
| 下半身 | Lower body / pelvis | Hip sway, turn; keep small so the feet stay planted |
| 上半身 | Upper body | Lean (x), twist (y), side bend (z); the main expression bone |
| 上半身2 | Upper chest | Extra chest lean / twist over 上半身; layer small amounts |
| 首 | Neck | First half of a head turn; 10-15 deg |
| 頭 | Head | Head turn / tilt; keep the total (首 + 頭) <= 30 deg |

## Arms

| Bone | Meaning | Typical edit |
| :--- | :--- | :--- |
| 左肩 / 右肩 | Shoulder (clavicle) | Shrug / shoulder lead for a raise; small rotations |
| 左腕 / 右腕 | Upper arm | Main raise / swing; <= 45 deg per edit |
| 左ひじ / 右ひじ | Elbow | Bend for a wave / arm accent; layer with the upper arm |
| 左手首 / 右手首 | Wrist | Wrist flick / angle; small amounts |
| 左親指０ / 右親指０, 左親指１ / 右親指１, 左親指２ / 右親指２ | Thumb (0/1/2) | Finger pattern; finger acting is out of scope for this skill |
| 左人指１ / 右人指１, 左人指２ / 右人指２, 左人指３ / 右人指３ | Index finger | Finger pattern |
| 左中指１ / 右中指１, 左中指２ / 右中指２, 左中指３ / 右中指３ | Middle finger | Finger pattern |
| 左薬指１ / 右薬指１, 左薬指２ / 右薬指２, 左薬指３ / 右薬指３ | Ring finger | Finger pattern |
| 左小指１ / 右小指１, 左小指２ / 右小指２, 左小指３ / 右小指３ | Little finger | Finger pattern |

Fingers come in the pattern `<side><finger><1|2|3>`, where side is 左 / 右, finger is 親指 / 人指 / 中指 / 薬指 /
小指; the thumb uses 親指０ / 親指１ / 親指２. Key them only when the user asks for a hand pose; do not attempt
per-finger acting as part of a dance edit.

## Legs

| Bone | Meaning | Typical edit |
| :--- | :--- | :--- |
| 左足 / 右足 | Thigh | Overridden by leg IK: do not key directly |
| 左ひざ / 右ひざ | Knee | Overridden by leg IK: do not key directly |
| 左足首 / 右足首 | Ankle | Overridden by leg IK: do not key directly |

## IK (the bones that actually drive the legs)

| Bone | Meaning | Typical edit |
| :--- | :--- | :--- |
| 左足ＩＫ / 右足ＩＫ | Leg IK target (ankle position / foot angle) | Move / rotate to step, plant or lift a foot |
| 左つま先ＩＫ / 右つま先ＩＫ | Toe IK | Toe roll / heel lift; usually paired with 足ＩＫ |

The full-width `ＩＫ` matters: use `左足ＩＫ`, not `左足IK`. The IK bones override 足, ひざ and 足首 rotations, so
edit feet through the IK bones; keys written on the leg bones are overwritten by the solve and have no visible
effect.

## Eyes

| Bone | Meaning | Typical edit |
| :--- | :--- | :--- |
| 両目 | Both eyes (parent of the two eye bones) | Aim both eyes; small yaw / pitch |
| 左目 / 右目 | Left / right eye | Individual eye aim (wink direction, glance); usually leave to 両目 |

Eyes are for small glances only; the head turn (`首` / `頭`, or `look_at`) does most of the work.

## Common morphs

| Morph | Meaning | Typical edit |
| :--- | :--- | :--- |
| まばたき | Blink | Peak 1 for a blink; 0.4-1 with attack 2, hold 2, release 4 |
| ウィンク | Wink (one eye) | Peak 0.6-1 on an accent |
| ウィンク右 | Wink, right eye | Peak 0.6-1 on an accent |
| あ | Mouth "a" | 0.4-0.7 on sung onsets |
| い | Mouth "i" | 0.4-0.7 on sung onsets |
| う | Mouth "u" | 0.4-0.7 on sung onsets |
| え | Mouth "e" | 0.4-0.7 on sung onsets |
| お | Mouth "o" | 0.4-0.7 on sung onsets |
| 笑い | Smile | 0.4-0.7, held over a chorus line |
| にこり | Soft smile | 0.3-0.6, gentle layer |
| 困る | Troubled | 0.4-0.7 for a lyric moment |
| 怒り | Angry | 0.4-0.7 for a lyric moment |

Morph names must exist in the model; not every PMX carries every morph. Read them with
`studio_get_bone_keys` `{morphs: true}` or write with `studio_motion_edit` `morph` (unknown names reject with the
list). Morph weights are 0..1.
