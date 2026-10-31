# Rig system — vision, current stage and roadmap

> Last updated 2026-10-31.

## Vision (long-term goal)

A full rig system with large 3D controllers like in animation programs
(Maya / Blender): cubes, spheres, surrounding rings / lines, etc.

Current state is a **skeletal prototype**: controllers are small rings/diamonds
bound to bones, roles are a fixed list, and the "editor" is limited to
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

## IK architecture (agreed)

We do NOT pit ready PMX IK against virtual IK — we support both, native first:

1. **Native PMX IK** (preferred): the controller binds to an existing PMX IK
   bone; motion is passed to it and the model's own IK solves the chain per the
   author's settings. No new bones.
2. **Virtual IK**: when the model has no suitable IK chain, MMDX12 builds its
   own virtual chain and solves it without touching the PMX (reuse the analytic
   solve behind `RigSolver` / AutoIK).
3. **FK fallback**: where IK is not applicable, drive the bones directly (FK).

Suggested data shape (conceptual — to be reconciled with the current
`RigDefinition` / `RigSolver`):

```
enum class RigSolverType { FK, IK };
enum class RigIkMode { NativePMX, Virtual };
struct RigSolverSettings { RigSolverType type; RigIkMode ikMode; };

struct RigIkBinding {
    int targetBone;               // the bone the IK moves
    int effectorBone;             // the chain end
    std::vector<int> chainBones;  // needed for Virtual only
    RigIkMode mode;
};
```

**Automatic discovery during generation:**
1. find available PMX IK bones;
2. determine their effector and chain;
3. check which body parts they serve;
4. create matching controllers;
5. keep the link to the source IK bones.

If the author built good IK, the user immediately gets a ready rig. If the
model is non-standard or IK is missing, offer virtual IK.

## Prototype gaps (known, drive the refactor)

1. **Controllers are hardcoded** — `kChains`/`kBody`/`kJoints` in
   `UiStudioRigViewport.cpp` plus a separate 19-row timeline table in
   `UiStudio.cpp`. Adding a controller touches several places. In the target
   architecture the controller is defined by a `RigController` object, not by
   an array index.
2. **Solver is not yet universal** — `RigSolver` is a specialized two-bone
   chain solve via AutoIK; `RigIkBone` references a ready PMX IK bone. Virtual
   IK is not yet a first-class path.
3. **FK is a separate handler** — the FK rotate math lives inside the drag
   handler (camera axes, 0.01 rad/px, parent-quaternion rotation). It should
   become part of the unified solver + transform-constraints layer.
4. **The Rig tab is still a diagnostic tab** — it lists roles and generates;
   part of the control already exists in the viewport, so the UI lags the
   actual capabilities.

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

## Current stage (2026-10-31)

- Rig work lives on `feature-port` on top of 3.2.0 (RigDefinition / RigGenerator,
  persistence, stage-3 controllers, bidirectional selection sync, FK joints,
  waist + shoulder controllers, free-rotate gizmo sphere).
- Controller set is now 19: 6 chain rings (hands/feet/toes), 7 body rings
  (center/groove/spine/chest/neck/head/waist), 6 FK diamonds
  (shoulders/upper arms/thighs).
- Gizmo rotate gained an optional free-rotate sphere (arcball) with a 3-state
  rotate tool.
- `studio_rig_test` PASS (generator incl. waist, persistence round-trip, chain
  solve); full 75-model `ref` scan runs without crashing (u8string path fix).
- Known prototype gaps listed above.
