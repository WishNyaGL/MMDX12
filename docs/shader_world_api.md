# Shader Pack World Entities (World Packs)

A shader pack with `"type": "world"` defines a placed, keyable 3D world entity (such as falling snow, dust, rain, magic circles, grass, or ground cover) with an optional GPU simulation. World entities are placed and keyed in the studio and live play mode (`FrameView::worlds`), simulated via compute shaders, and rendered across all raster, ray-traced, path-traced, and offline GI render paths.

---

## 1. Folder Structure

```
my_world_pack/
  pack.json      manifest ("type": "world", "world": { ... })
  world.hlsl     entry points (simulate, spawn, update, height, shade)
  preview.png    optional preview image
  textures/      optional pack textures (up to 32 files, 64 MB total)
```

---

## 2. Manifest Schema (`pack.json`)

```json
{
  "name": "Falling Snow",
  "version": "1.0.0",
  "apiVersion": 1,
  "type": "world",
  "world": {
    "step": 0.0166667,
    "rewind": 4.0,
    "particles": [
      {
        "name": "snow_flakes",
        "count": 65536,
        "blend": "alpha",
        "simulate": "SimulateFlakes",
        "shade": "ShadeFlakes",
        "size": [0.05, 0.2]
      }
    ],
    "fields": [
      {
        "name": "snow_depth",
        "size": 512,
        "extent": 40.0,
        "format": "rgba16f",
        "persistent": true,
        "rate": 10.0,
        "update": "UpdateSnowField"
      }
    ],
    "surfaces": [
      {
        "field": "snow_depth",
        "grid": 128,
        "height": "SnowHeight",
        "shade": "SnowShade"
      }
    ]
  },
  "params": [
    {
      "key": "wind_speed",
      "label": "Wind Speed",
      "type": "float",
      "def": 1.5,
      "min": 0.0,
      "max": 10.0
    }
  ]
}
```

### Top-Level Manifest Properties
- `"type"`: Must be `"world"`. `classes` (material body/hair/face rules) are disallowed for world packs.
- `"world"`: The world entity specification (required).
- `"params"`: Extended parameter declaration. World packs support up to 128 typed parameters (`params.ext` capability is always enabled).
- `"textures"`: Up to 32 texture files (total uncompressed/compressed footprint up to 64 MB).

### `"world"` Object Properties
- `"step"`: Fixed simulation step size in seconds. Allowed range: `1/240` (0.004167) to `1/15` (0.066667). Default: `1/60` (0.0166667).
- `"rewind"`: Duration of short-lived state re-simulation window in seconds. Allowed range: `0.0` to `20.0`. Default: `4.0`.
- `"particles"`: Array of up to 8 particle systems.
- `"fields"`: Array of up to 4 2D local-space simulation grids.
- `"surfaces"`: Array of up to 2 procedural heightfield meshes.

### Particle System Specification
Each particle system in `"particles"`:
- `"name"`: Unique identifier within the pack.
- `"count"`: Particle count between `1` and `262144` (256K).
- `"blend"`: `"alpha"` (premultiplied/interpolated alpha blend) or `"add"` (additive blend).
- `"simulate"`: Optional function name for stateful simulation (called once per fixed step). Mutually exclusive with `"spawn"`.
- `"spawn"`: Optional function name for stateless procedural evaluation ($f(id, t)$). Mutually exclusive with `"simulate"`.
- `"shade"`: Function name for shading particles in the pixel shader.
- `"size"`: 2-element array `[min, max]` specifying world-space particle radius hint for culling.

### Field Specification
Each field in `"fields"`:
- `"name"`: Unique identifier within the pack.
- `"size"`: Square resolution in texels: `64` to `2048`.
- `"extent"`: World units across the XZ plane, centered at the entity origin ($[-extent/2, +extent/2]$).
- `"format"`: `"r16f"`, `"rg16f"`, or `"rgba16f"`.
- `"persistent"`: Boolean. If `true`, on timeline seeks this field is re-simulated from $t = 0$ at its specified `"rate"`. If `false`, it re-simulates over the entity's `"rewind"` window.
- `"rate"`: Steps per second for persistent field simulation (default: `10.0`). A persistent field always steps on
  its own grid, step k at time k / rate, both when re-simulating after a seek and during playback, so a seek and
  continuous playback reach the same field at the same time.
- `"update"`: Function name for compute shader ping-pong field updates.

### Surface Specification
Each surface in `"surfaces"`:
- `"field"`: Name of the referenced field in `"fields"`.
- `"grid"`: Quads per side: `16` to `512` (generates `grid * grid * 2` triangles procedurally).
- `"height"`: Function name returning vertex local height $Y$.
- `"shade"`: Function name returning surface shading attributes (`PackSurfaceOut`).

---

## 3. HLSL Contract (`world.hlsl` and `world_api.hlsli`)

All world shaders compile under Shader Model 6.0 (`cs_6_0`, `vs_6_0`, `ps_6_0`) using DXC. `world_api.hlsli` is automatically available.

### Data Structures

```hlsl
struct PackParticle {
    float3 pos;     // World-space position
    float size;     // World-space radius
    float3 vel;     // Units per second
    float age;      // Age in seconds; < 0 marks inactive particle
    float4 color;   // Base color and alpha
    float4 data;    // Custom user attributes
};

struct PackSimInput {
    uint id;            // Particle index (0 .. count - 1)
    float dt;           // Fixed step delta time in seconds
    float time;         // Target time in seconds
    float3 posLocal;    // Entity local-space position
};

struct PackFieldInput {
    float2 uv;          // Normalized UV in [0, 1]
    float2 posLocal;    // Local-space XZ coordinates in world units
    float4 prev;        // Previous texel value from ping-pong texture (zero on the first step of a re-simulation)
    float dt;           // Step delta time
    float time;         // Current simulation time
    uint2 texel;        // Integer texel coordinate
};

struct PackSurfaceIn {
    float2 uv;          // Grid UV in [0, 1]
    float2 posLocal;    // Local-space XZ coordinates
    float time;         // Current time
};

struct PackSurfaceOut {
    float3 albedo;      // Linear RGB albedo
    float alpha;        // Surface opacity (1.0 for opaque)
    float roughness;    // Material roughness [0, 1]
    float3 normal;      // World normal (host calculates gradient default)
    float3 emission;    // Emissive linear RGB
};

struct PackSurfaceShadeIn {
    float2 uv;          // Grid UV
    float3 posLocal;    // Local-space XYZ
    float3 posWorld;    // World-space XYZ
    float3 normal;      // Computed world-space gradient normal
    float3 viewDir;     // Normalized vector towards camera
    float time;         // Current time
};

struct PackParticleShadeIn {
    float2 uv;              // Quad billboard UV [0, 1]
    PackParticle particle;  // Particle state
    float3 viewDir;         // Normalized vector towards camera
    float3 posWorld;        // World position of the billboard vertex
};
```

### Function Signatures

1. **Stateful Particle Simulation**:
   ```hlsl
   void SimulateFlakes(inout PackParticle p, PackSimInput s);
   ```
   Particles start with `age = -1.0f`. `SimulateFlakes` is responsible for spawning and updating particles.

2. **Stateless Particle Spawn**:
   ```hlsl
   PackParticle SpawnFlakes(uint id, float t);
   ```
   Evaluates particle position and attributes procedurally as a function of index and time.

3. **Field Update**:
   ```hlsl
   float4 UpdateSnowField(PackFieldInput f);
   ```
   Computes the updated value for the texel in the field's ping-pong texture.

4. **Surface Height**:
   ```hlsl
   float SnowHeight(PackSurfaceIn i);
   ```
   Returns the local vertical displacement ($Y$) of the surface grid at $(X, Z)$.

5. **Surface Shading**:
   ```hlsl
   PackSurfaceOut SnowShade(PackSurfaceShadeIn i);
   ```
   Returns albedo, alpha, roughness, normal, and emission. The host automatically applies direct scene lighting (sun direction, shadow map visibility, sun color, and ambient light) so the surface matches the scene.

6. **Particle Shading**:
   ```hlsl
   float4 ShadeFlakes(PackParticleShadeIn i);
   ```
   Returns linear HDR color and alpha.

---

## 4. Built-in API Helpers

- **Parameters**: `PackParam(index)` (float), `PackParamInt(i)`, `PackParamBool(i)`, `PackParamColor(i)`, `PackParamVec3(i)`.
- **Transforms**:
  - `PackWorldToLocal(posWorld)` / `PackLocalToWorld(posLocal)` (points)
  - `PackWorldToLocalVec(vecWorld)` / `PackLocalToWorldVec(vecLocal)` (vectors)
- **Colliders**:
  - `PackColliderCount()`: Number of active scene colliders this frame.
  - `PackCollider(i)`: Returns `PackColliderData` with `pos` (float3), `radius` (float), `vel` (float3), and `flags` (uint). Flags indicate body parts: `kColliderFoot = 1`, `kColliderHand = 2`, `kColliderLeft = 4`.
- **Fields**:
  - `PackFieldSample(fieldIndex, posLocalXZ)`: Bilinear sample of field `fieldIndex` (0 .. 3) at local XZ coordinates.
  - `PackFieldLoad(fieldIndex, int2 texel)`: Point fetch of field texel.
- **Pack Textures**:
  - `PackSampleTex(i, uv)` / `PackSampleTexLevel(i, uv, lod)`: Sample pack texture `i` (0 .. 31).
  - `PackTexSize(i)` / `PackTexCount()`.
- **Lighting**:
  - `PackSunDir()`: Vector pointing toward the sun.
  - `PackSunColor()`: Sun light color and intensity.
  - `PackAmbient()`: Ambient light color.
  - `PackHemiAmbient(normal)`: the scene's sky / ground hemisphere light for a world-space normal (ambient that
    follows the stage lighting; `PackAmbient()` is only the ground colour).
  - `PackSunShadow(worldPos)`: Sun shadow visibility factor in [0, 1], from the scene's cascaded shadow map in
    raster camera views; 1 in ray-traced / path-traced frames and offline GI (no shadow map there in v1).
- **Deterministic Hashing**:
  - `PackRandom(seed)` / `PackRandom(seed, salt)` / `PackRandom(seed, salt1, salt2)`: PCG-based deterministic pseudo-random float in [0, 1).
  - `PackRandom2(seed, salt)` / `PackRandom3(seed, salt)`.

---

## 5. Determinism & Seek Rewind

World pack simulations run in fixed time increments `step` and are completely deterministic across live viewport playback, scrubbing, offline rendering, and video export:

1. **Forward Live Playback**:
   When time progresses normally ($0 \le \Delta t \le 0.25\text{ s}$), the engine steps forward by fixed `step` intervals up to the target time.
2. **Timeline Seek & Re-simulation**:
   When jumping backwards or forward by $> 0.25\text{ s}$ (or when `inst.exact` is true):
   - **Persistent fields** (e.g., ground deformation tracks) re-simulate from $t = 0$ to $t_{target}$ at their declared `"rate"` (default 10 Hz).
   - **Particles and transient fields** reset and re-simulate from $\max(0, t_{target} - \text{rewind})$ to $t_{target}$ in fixed `step` increments.
   - Character interaction colliders are sampled strictly from the precomputed `WorldColliderTrack` using `track->Sample(t)`, ensuring identical state regardless of playback speed or frame rate.

---

## 6. Rendering Pipeline Integration

- **Raster and Ray-Traced Real-Time Views**:
  - `WorldPass` executes between `ScenePass` and `ResolvePass`.
  - Surfaces render as opaque geometry with depth write and motion vectors into MSAA scene targets (`colorMsaa`, `normalMsaa`, `velocityMsaa`, `depthMsaa`).
  - Particles render after opaque geometry with depth testing (`LESS_EQUAL`) and no depth write. Alpha and additive blending are supported. Camera-facing motion vectors are computed from $(pos - vel \cdot dt)$.
  - `ResolvePass` automatically resolves world geometry and motion vectors, ensuring full compatibility with TAA and hardware upscalers (DLSS, FSR, XeSS).
- **Path-Traced & Offline GI Views**:
  - Surfaces and particles are rendered directly into the edge/outline layer targets (`DrawPtEdges` and `OfflineRenderer::Impl::DrawEdges`), occluded by the tracer's depth prepass, and composited over the denoised radiance.
  - World surfaces in PT/GI receive direct lighting matching the scene lighting.
- **Zero-Cost Rule**:
  - When `FrameView::worlds` is empty, `WorldPass` executes zero draw calls, dispatches zero compute passes, allocates zero GPU resources, and produces an identical byte-for-byte frame.
- **Exemptions**:
  - Unlit, wireframe, and extra quad views skip world simulation and rendering.
  - Shadow casting by world particles/surfaces is not enabled in v1.

---

## 7. Verification

Validate world packs using the CLI tool:
```cmd
build_dev\bin\pack_check.exe <path_to_pack> --compile
```
This parses `pack.json`, enforces all limits, and compiles every declared entry point (`simulate`, `spawn`, `update`, `height`, `shade`).
