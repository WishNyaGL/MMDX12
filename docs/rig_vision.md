# Rig system — vision, current stage and roadmap

> Last updated 2026-10-30.

## Vision (long-term goal)

A full rig system with large 3D controllers like in animation programs
(Maya / Blender): cubes, spheres, surrounding rings / lines, etc.

Current state is a **skeletal prototype**: controllers are small rings/diamonds
bound to bones, roles are a fixed list (17), and the "editor" is limited to
"generate and look". This is far from the target.

## Target

1. **Large 3D controllers** — cubes, spheres, surrounding rings (NURBS-style
   Maya / Blender bone shapes). Separate visual objects on top of the model,
   not the bones themselves.
2. **In-app rig editor** — create/edit a rig right in the Studio:
   - take model bones, group them;
   - set radius / movement distance;
   - restrict rotation axes;
   - create IK controllers **without adding bones to the model** (IK solved
     virtually, not via ready-made `arm_IK` / `leg_IKP` in the PMX).

## Key architectural shift

A controller becomes an independent entity, not a "role + bone":

- **Shape** — cube / sphere / ring / line: geometry + size (radius / scale).
- **Bindings** — any set of bones (not a single "role").
- **Transform constraints** — translation (with distance limit), rotation (with
  axis limits), aim.
- **Solver type** — FK (rotates the bone) or IK (virtual solve, no extra bones
  in the model).
- A separate data layer (extend `.mmdxrig`), separate rendering, separate editor.

## Roadmap (ordered — the data core comes first)

1. **Architecture / rig data core** (FIRST step, before the editor): the
   "controller as an entity" model (shape, bindings, constraints, solver type)
   + persistence (`.mmdxrig`) + API. The editor is built ON TOP of a stable
   data model, not the other way around — otherwise it would have to be
   reworked on every model change.
2. Controller shape: rendering large 3D shapes (cube / sphere / ring) instead of dots.
3. Group bone binding (bone list + rotate/translate mode).
4. Constraints (rotation axes, range / distance).
5. IK without extra bones (reuse `RigSolver` / AutoIK).
6. Rig editor in the Studio (only after a stable data core).

## Current stage (2026-10-30)

- Stage 1–4 rig work is ported to `feature-port` on top of 3.2.0 (see history:
  RigDefinition/RigGenerator, persistence, stage-3 controllers, bidirectional
  selection sync).
- Viewport controllers: chain rings (hands/feet/toes), body rings, and FK
  diamond joints for upper arms / thighs. Sun (main light) handle is hidden
  while the rig is visible.
- `studio_rig_test` PASS (generator, persistence round-trip, chain solve).
- Selection-sync fix was tried and reverted (was quirky); FK controllers await
  manual testing (camera angles, rotated parents, undo/redo, VMD keys).
