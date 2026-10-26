#!/usr/bin/env python3
"""stagegen: writes procedural MMDX test stages (GLB + stage.json) for tools/stage_check.

    python tools/stagegen/stagegen.py [--out captures/stages] [--only cyclorama,mirror_room,led_stage]

Everything is generated from numpy arrays: no Blender, no pygltflib, no third-party glTF writer. The GLB is written
by hand (a JSON chunk + a BIN chunk, 4-byte padded) and is a valid glTF 2.0 asset with named nodes and materials,
triangle normals and UVs (documents: docs/stage_format.md).

Stages:
  cyclorama   curved floor-to-wall cove, 3 colour variants (cove + key / fill light colours), 7 named lights (4 spots)
  mirror_room a box room, one wall material flagged "mirror", a wood floor with PBR values
  led_stage   a raised stage with 3 screen quads (screen ids main / left / right), truss bars, 8 spots, fog
"""

import argparse
import json
import math
import os
import struct
import zlib

import numpy as np

# ---------------------------------------------------------------------------------------------------------------------
# glTF / GLB writing
# ---------------------------------------------------------------------------------------------------------------------


class Part:
    """One drawable: a triangle list of one material, instanced at one named node."""

    def __init__(self, node, material, positions, normals, uvs, indices):
        self.node = node
        self.material = material
        self.positions = np.asarray(positions, dtype=np.float32).reshape(-1, 3)
        self.normals = np.asarray(normals, dtype=np.float32).reshape(-1, 3)
        self.uvs = np.asarray(uvs, dtype=np.float32).reshape(-1, 2)
        self.indices = np.asarray(indices, dtype=np.uint32).reshape(-1)
        assert len(self.positions) == len(self.normals) == len(self.uvs)
        assert len(self.indices) % 3 == 0


class Gltf:
    """Collects parts + materials and writes a .glb."""

    def __init__(self, scene_name):
        self.scene_name = scene_name
        self.parts = []
        self.materials = {}   # name -> glTF material dict

    def add(self, part):
        self.parts.append(part)

    def material(self, name, base_color=(0.5, 0.5, 0.5, 1.0), metallic=0.0, roughness=0.5,
                 emissive=(0.0, 0.0, 0.0), double_sided=False, alpha=1.0):
        mat = {
            "name": name,
            "pbrMetallicRoughness": {
                "baseColorFactor": [float(base_color[0]), float(base_color[1]), float(base_color[2]), float(alpha)],
                "metallicFactor": float(metallic),
                "roughnessFactor": float(roughness),
            },
            "doubleSided": bool(double_sided),
        }
        if any(c > 0 for c in emissive):
            mat["emissiveFactor"] = [float(emissive[0]), float(emissive[1]), float(emissive[2])]
        if alpha < 1.0:
            mat["alphaMode"] = "BLEND"
        self.materials[name] = mat
        return name

    # -- binary layout ---------------------------------------------------------------------------------------------
    def _flush(self, blob):
        while len(blob) % 4:
            blob.append(0)
        return blob

    def build(self):
        material_names = list(self.materials.keys())
        mat_index = {name: i for i, name in enumerate(material_names)}

        bin_blob = bytearray()
        buffer_views = []
        accessors = []

        def add_view(data, target):
            offset = len(bin_blob)
            bin_blob.extend(data.tobytes())
            self._flush(bin_blob)
            view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data.tobytes()), "target": target}
            buffer_views.append(view)
            return len(buffer_views) - 1

        def add_accessor(array, comp_type, type_name, target, normalized=False, minmax=False):
            view = add_view(array, target)
            acc = {"bufferView": view, "componentType": comp_type, "count": int(array.shape[0]), "type": type_name}
            if normalized:
                acc["normalized"] = True
            if minmax:
                acc["min"] = [float(v) for v in array.min(axis=0)]
                acc["max"] = [float(v) for v in array.max(axis=0)]
            accessors.append(acc)
            return len(accessors) - 1

        meshes = []
        nodes = []
        for part in self.parts:
            pos = add_accessor(part.positions, 5126, "VEC3", 34962, minmax=True)
            nrm = add_accessor(part.normals, 5126, "VEC3", 34962)
            uv = add_accessor(part.uvs, 5126, "VEC2", 34962)
            idx = add_accessor(part.indices.astype(np.uint32), 5125, "SCALAR", 34963)
            meshes.append({
                "name": part.node,
                "primitives": [{
                    "attributes": {"POSITION": pos, "NORMAL": nrm, "TEXCOORD_0": uv},
                    "indices": idx,
                    "material": mat_index[part.material],
                    "mode": 4,
                }],
            })
            nodes.append({"name": part.node, "mesh": len(meshes) - 1})

        gltf = {
            "asset": {"version": "2.0", "generator": "mmdx stagegen"},
            "scene": 0,
            "scenes": [{"name": self.scene_name, "nodes": list(range(len(nodes)))}],
            "nodes": nodes,
            "meshes": meshes,
            "materials": [self.materials[n] for n in material_names],
            "accessors": accessors,
            "bufferViews": buffer_views,
            "buffers": [{"byteLength": len(bin_blob)}],
        }
        json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        while len(json_bytes) % 4:
            json_bytes += b" "
        bin_bytes = bytes(bin_blob)
        total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
        out = bytearray()
        out += struct.pack("<4sII", b"glTF", 2, total)
        out += struct.pack("<II", len(json_bytes), 0x4E4F534A) + json_bytes   # 'JSON'
        out += struct.pack("<II", len(bin_bytes), 0x004E4942) + bin_bytes     # 'BIN'
        return bytes(out)


def write_png(path, width, height, rgb):
    """A minimal RGB PNG (zlib + CRC), for the LED textures the screens reference."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    raw = bytearray()
    for y in range(height):
        raw.append(0)   # filter: none
        for x in range(width):
            raw.extend(rgb(x, y))
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


# ---------------------------------------------------------------------------------------------------------------------
# geometry helpers (metres, +Y up, +Z toward the audience; the importer scales by 12.5 and mirrors Z)
# ---------------------------------------------------------------------------------------------------------------------


def grid_part(node, material, corners, nu, nv, flip=False, uv_scale=(1.0, 1.0)):
    """A quad patch subdivided nu x nv from 3 corners (bilinear): p(u,v) = p00 + u*(p10-p00) + v*(p01-p00)."""
    p00, p10, p01 = (np.asarray(c, dtype=np.float64) for c in corners)
    positions, uvs = [], []
    for j in range(nv + 1):
        for i in range(nu + 1):
            u, v = i / nu, j / nv
            positions.append(p00 + (p10 - p00) * u + (p01 - p00) * v)
            uvs.append((u * uv_scale[0], v * uv_scale[1]))
    positions = np.array(positions)
    n = np.cross(p10 - p00, p01 - p00)
    n /= np.linalg.norm(n)
    normals = np.tile(n, (len(positions), 1))
    indices = []
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            b, c, d = a + 1, a + (nu + 1), a + (nu + 1) + 1
            if flip:
                indices += [a, d, b, a, c, d]
            else:
                indices += [a, b, d, a, d, c]
    return Part(node, material, positions, normals, uvs, indices)


def sweep_profile(node, material, profile, x0, x1, closed_normal_inside=True, uv_scale=(1.0, 1.0)):
    """Sweeps a (y, z) polyline along X (in [-x0, x1]): one quad strip per segment.

    profile: list of (y, z, ny, nz) with the outward normal per point.
    """
    xs = [x0, x1]
    positions, uvs, indices = [], [], []
    length = 0.0
    along = [0.0]
    for k in range(1, len(profile)):
        dy = profile[k][0] - profile[k - 1][0]
        dz = profile[k][1] - profile[k - 1][1]
        length += math.hypot(dy, dz)
        along.append(length)
    for k, (y, z, ny, nz) in enumerate(profile):
        for x in xs:
            positions.append((x, y, z))
            uvs.append(((x - x0) / max(1e-6, (x1 - x0)) * uv_scale[0], along[k] * uv_scale[1]))
    for k in range(len(profile) - 1):
        a = 2 * k            # x0, profile k
        b = 2 * k + 1        # x1, profile k
        c = a + 2            # x0, profile k+1
        d = b + 2            # x1, profile k+1
        # the sweep's triangles as seen from the profile's normal side
        if (profile[k][2] * (profile[k + 1][1] - profile[k][1]) -
                profile[k][3] * (profile[k + 1][0] - profile[k][0])) >= 0:
            indices += [a, b, d, a, d, c]
        else:
            indices += [a, d, b, a, c, d]
    positions = np.array(positions)
    normals = np.array([(ny, nz) for (_, _, ny, nz) in profile for _ in xs])
    normals = np.stack([np.zeros(len(normals)), normals[:, 0], normals[:, 1]], axis=1)
    if closed_normal_inside:
        pass
    return Part(node, material, positions, normals, np.array(uvs), indices)


def box_parts(node_prefix, material, center, size, uv_scale=(1.0, 1.0), skip=()):
    """Six faces of an axis-aligned box, each its own part (named <node_prefix>_<face>)."""
    cx, cy, cz = center
    sx, sy, sz = (s / 2.0 for s in size)
    faces = {
        "px": ((cx + sx, cy - sy, cz - sz), (cx + sx, cy - sy, cz + sz), (cx + sx, cy + sy, cz - sz), False),
        "nx": ((cx - sx, cy - sy, cz + sz), (cx - sx, cy - sy, cz - sz), (cx - sx, cy + sy, cz + sz), False),
        "py": ((cx - sx, cy + sy, cz - sz), (cx + sx, cy + sy, cz - sz), (cx - sx, cy + sy, cz + sz), False),
        "ny": ((cx - sx, cy - sy, cz + sz), (cx + sx, cy - sy, cz + sz), (cx - sx, cy - sy, cz - sz), False),
        "pz": ((cx - sx, cy - sy, cz + sz), (cx + sx, cy - sy, cz + sz), (cx - sx, cy + sy, cz + sz), False),
        "nz": ((cx + sx, cy - sy, cz - sz), (cx - sx, cy - sy, cz - sz), (cx + sx, cy + sy, cz - sz), False),
    }
    out = []
    for name, (p00, p10, p01, flip) in faces.items():
        if name in skip:
            continue
        out.append(grid_part(f"{node_prefix}_{name}", material, (p00, p10, p01), 1, 1, flip, uv_scale))
    return out


def light(kind, name, intensity=1.0, color=(1.0, 1.0, 1.0), position=None, aim=None, direction=None,
          cone=(0.24, 0.15), range_=140.0, shadow="soft", aim_mode="manual", sway_phase=0.0):
    """One stage.json "lights" entry: exactly the .mmdxproj editor.lights schema."""
    values = {"color": list(color), "intensity": float(intensity), "range": float(range_)}
    if position is not None:
        values["position"] = list(position)
    if aim is not None:
        values["aim"] = list(aim)
        values["coneOuter"] = float(cone[0])
        values["coneInner"] = float(cone[1])
    if direction is not None:
        values["direction"] = list(direction)
    entry = {
        "kind": kind,
        "name": name,
        "enabled": True,
        "values": values,
        "shadow": {"type": shadow},
    }
    if kind == "spot":
        entry["spot"] = {"aim": aim_mode, "swayPhase": sway_phase}
    return entry


# ---------------------------------------------------------------------------------------------------------------------
# stage 1: cyclorama
# ---------------------------------------------------------------------------------------------------------------------


def build_cyclorama(out_dir):
    gltf = Gltf("cyclorama")
    cove = gltf.material("cove", base_color=(0.82, 0.82, 0.85, 1.0), roughness=0.45, metallic=0.0)
    floor = gltf.material("floor", base_color=(0.30, 0.31, 0.33, 1.0), roughness=0.28, metallic=0.0)
    wall = gltf.material("wall", base_color=(0.78, 0.78, 0.80, 1.0), roughness=0.6, metallic=0.0)

    half_width, depth, wall_height, radius, segments = 12.0, 14.0, 12.0, 6.0, 24
    z_curve = -depth + radius          # the cove starts here on the floor
    z_wall = -depth                    # ... and ends here at the wall
    # floor: from z = +2 (front, toward the camera) back to z_curve
    gltf.add(grid_part("Cove_Floor", floor,
                       ((-half_width, 0.0, 2.0), (half_width, 0.0, 2.0), (-half_width, 0.0, z_curve)),
                       8, 8, flip=False, uv_scale=(6.0, 6.0)))
    # cove: a quarter circle from (y=0, z=z_curve) to (y=radius, z=z_wall); inward normal (+y / +z)
    profile = []
    for i in range(segments + 1):
        t = math.pi / 2 * i / segments
        y = radius - radius * math.cos(t)
        z = z_curve - radius * math.sin(t)
        ny, nz = math.cos(t), math.sin(t)   # the surface normal points away from the centre (0, radius, z_curve)
        profile.append((y, z, ny, nz))
    gltf.add(sweep_profile("Cove_Curve", cove, profile, -half_width, half_width, uv_scale=(6.0, 0.5)))
    # wall above the cove
    gltf.add(grid_part("Cove_Wall", wall,
                       ((-half_width, radius, z_wall), (half_width, radius, z_wall), (-half_width, wall_height, z_wall)),
                       8, 4, flip=False, uv_scale=(6.0, 3.0)))

    stage_dir = os.path.join(out_dir, "cyclorama")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "cyclorama.glb"), "wb") as f:
        f.write(gltf.build())

    lights = [
        light("sun", "key", intensity=1.1, color=(1.0, 0.96, 0.90), direction=(-0.35, -1.0, 0.25)),
        light("point", "fill", intensity=0.6, color=(0.92, 0.95, 1.0), position=(0.0, 34.0, -40.0), range_=180.0,
              shadow="nocast"),
        light("ambient", "ambient", intensity=0.18, color=(1.0, 1.0, 1.0)),
        light("spot", "spot_l", intensity=2.4, color=(1.0, 0.95, 0.88), position=(-45.0, 62.0, 40.0),
              aim=(-22.0, 0.0, -30.0), cone=(0.26, 0.17), aim_mode="sway", sway_phase=0.0),
        light("spot", "spot_r", intensity=2.4, color=(0.95, 0.97, 1.0), position=(45.0, 62.0, 40.0),
              aim=(22.0, 0.0, -30.0), cone=(0.26, 0.17), aim_mode="sway", sway_phase=1.3),
        light("spot", "spot_back_l", intensity=1.6, color=(0.55, 0.85, 1.0), position=(-50.0, 58.0, -110.0),
              aim=(-10.0, 4.0, -40.0), cone=(0.3, 0.2), shadow="nocast"),
        light("spot", "spot_back_r", intensity=1.6, color=(1.0, 0.6, 0.78), position=(50.0, 58.0, -110.0),
              aim=(10.0, 4.0, -40.0), cone=(0.3, 0.2), shadow="nocast"),
    ]
    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "cyclorama",
        "version": "1.0.0",
        "name": {"ko": "사이클로라마 댄스 스튜디오", "en": "Cyclorama dance studio", "ja": "サイクロラマ ダンススタジオ"},
        "description": {"ko": "바닥과 벽이 하나로 이어지는 무한 배경 댄스 스튜디오.",
                        "en": "A dance studio with an infinite floor-to-wall cove."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["studio", "dance", "cove", "test"],
        "models": [{"file": "cyclorama.glb", "name": "Cove", "role": "floor"}],
        "materials": [
            {"match": "cove", "baseColor": [0.82, 0.82, 0.85, 1.0], "roughness": 0.45, "metallic": 0.0,
             "receiveShadow": True, "castShadow": False},
            {"match": "floor", "roughness": 0.28, "metallic": 0.0},
            {"match": "wall", "roughness": 0.6},
        ],
        "lights": lights,
        "sockets": [
            {"name": "world", "position": [0.0, 0.0, -20.0]},
            {"name": "cove_center", "position": [0.0, 0.0, -75.0], "rotation": [0.0, 180.0, 0.0]},
            {"name": "wall_node", "node": "Cove_Wall"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 16.0, 130.0], "target": [0.0, 11.0, -60.0], "fov": 42.0},
            {"name": "close", "position": [0.0, 13.0, 40.0], "target": [0.0, 11.0, -40.0], "fov": 55.0},
        ],
        "environment": {
            "fog": {"density": 0.004, "color": [0.62, 0.65, 0.72], "anisotropy": 0.3, "heightFalloff": 0.02},
            "ambient": {"intensity": 0.18},
        },
        "groups": [
            {"id": "look", "label": {"ko": "분위기", "en": "Look"}},
            {"id": "lights", "label": {"ko": "조명", "en": "Lights"}},
        ],
        "params": [
            {"key": "coveColor", "type": "color", "label": {"ko": "배경 색", "en": "Cove colour"},
             "group": "look", "default": [0.82, 0.82, 0.85, 1.0]},
            {"key": "floorColor", "type": "color", "label": {"ko": "바닥 색", "en": "Floor colour"},
             "group": "look", "default": [0.30, 0.31, 0.33, 1.0]},
            {"key": "keyColor", "type": "color", "label": {"ko": "키 라이트", "en": "Key light"}, "group": "lights",
             "default": [1.0, 0.96, 0.90, 1.0]},
            {"key": "keyIntensity", "type": "float", "label": {"ko": "키 세기", "en": "Key intensity"},
             "group": "lights", "default": 1.1, "min": 0.0, "max": 3.0},
            {"key": "fillColor", "type": "color", "label": {"ko": "필 라이트", "en": "Fill light"}, "group": "lights",
             "default": [0.92, 0.95, 1.0, 1.0]},
            {"key": "fillIntensity", "type": "float", "label": {"ko": "필 세기", "en": "Fill intensity"},
             "group": "lights", "default": 0.6, "min": 0.0, "max": 2.0},
            {"key": "fogDensity", "type": "float", "label": {"ko": "안개", "en": "Fog"}, "group": "look",
             "default": 0.004, "min": 0.0, "max": 0.05},
            {"key": "showScreens", "type": "bool", "label": {"ko": "밝은 배경", "en": "Bright cove"},
             "group": "look", "default": False},
        ],
        "variants": [
            {"id": "studio", "label": {"ko": "스튜디오", "en": "Studio"},
             "values": {"coveColor": [0.82, 0.82, 0.85, 1.0], "keyColor": [1.0, 0.96, 0.90, 1.0],
                        "fillColor": [0.92, 0.95, 1.0, 1.0], "fogDensity": 0.004}},
            {"id": "daylight", "label": {"ko": "주광", "en": "Daylight"},
             "values": {"coveColor": [0.92, 0.94, 0.98, 1.0], "keyColor": [1.0, 0.99, 0.97, 1.0],
                        "fillColor": [0.86, 0.93, 1.0, 1.0], "keyIntensity": 1.6, "fogDensity": 0.001}},
            {"id": "night", "label": {"ko": "야간", "en": "Night"},
             "values": {"coveColor": [0.05, 0.06, 0.10, 1.0], "keyColor": [0.45, 0.62, 1.0, 1.0],
                        "fillColor": [1.0, 0.45, 0.7, 1.0], "keyIntensity": 0.5, "fogDensity": 0.012}},
        ],
        "defaultVariant": "studio",
        "bindings": [
            {"param": "coveColor", "target": "materials.cove.baseColor"},
            {"param": "floorColor", "target": "materials.floor.baseColor"},
            {"param": "keyColor", "target": "lights.key.color"},
            {"param": "keyIntensity", "target": "lights.key.intensity"},
            {"param": "fillColor", "target": "lights.fill.color"},
            {"param": "fillIntensity", "target": "lights.fill.intensity"},
            {"param": "fogDensity", "target": "environment.fog.density"},
            {"param": "showScreens", "target": "materials.wall.emissiveStrength", "scale": 1.5},
        ],
        "cues": [
            {"param": "keyIntensity", "keys": [
                {"frame": 0, "value": 1.1}, {"frame": 300, "value": 1.5},
                {"frame": 600, "value": 0.9}, {"frame": 900, "value": 1.1}]},
            {"param": "fogDensity", "stepped": True,
             "keys": [{"frame": 0, "value": 0.004}, {"frame": 900, "value": 0.012}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


# ---------------------------------------------------------------------------------------------------------------------
# stage 2: mirror room
# ---------------------------------------------------------------------------------------------------------------------


def build_mirror_room(out_dir):
    gltf = Gltf("mirror_room")
    wood = gltf.material("wood_floor", base_color=(0.34, 0.21, 0.12, 1.0), roughness=0.38, metallic=0.0)
    paint = gltf.material("wall_paint", base_color=(0.72, 0.71, 0.70, 1.0), roughness=0.7, metallic=0.0)
    mirror = gltf.material("mirror_wall", base_color=(0.90, 0.92, 0.95, 1.0), roughness=0.02, metallic=1.0)
    ceiling = gltf.material("ceiling", base_color=(0.85, 0.85, 0.86, 1.0), roughness=0.8, metallic=0.0)

    width, height, depth = 14.0, 5.0, 16.0
    # floor (inward, +y), ceiling, and the four walls (inward normals)
    gltf.add(grid_part("Floor", wood, ((-width / 2, 0.0, -depth / 2), (-width / 2, 0.0, depth / 2),
                                      (width / 2, 0.0, -depth / 2)), 8, 8, flip=False, uv_scale=(7.0, 8.0)))
    gltf.add(grid_part("Ceiling", ceiling, ((-width / 2, height, depth / 2), (-width / 2, height, -depth / 2),
                                           (width / 2, height, depth / 2)), 4, 4, flip=False))
    # the mirror is the back wall (-z); it faces +z (into the room)
    gltf.add(grid_part("Mirror_Wall", mirror, ((-width / 2, 0.0, -depth / 2), (width / 2, 0.0, -depth / 2),
                                              (-width / 2, height, -depth / 2)), 4, 2, flip=False))
    # side walls: left (-x) faces +x, right (+x) faces -x
    gltf.add(grid_part("Wall_Left", paint, ((-width / 2, 0.0, depth / 2), (-width / 2, 0.0, -depth / 2),
                                           (-width / 2, height, depth / 2)), 4, 2, flip=False))
    gltf.add(grid_part("Wall_Right", paint, ((width / 2, 0.0, -depth / 2), (width / 2, 0.0, depth / 2),
                                            (width / 2, height, -depth / 2)), 4, 2, flip=False))
    # front wall (+z, behind the camera): faces -z, so the room can be seen from any angle
    gltf.add(grid_part("Wall_Front", paint, ((width / 2, 0.0, depth / 2), (-width / 2, 0.0, depth / 2),
                                            (width / 2, height, depth / 2)), 4, 2, flip=False))
    # a ballet barre along the mirror, and a bench: props parts
    for part in box_parts("Barre", paint, (0.0, 1.05, -depth / 2 + 0.35), (width - 2.0, 0.08, 0.08),
                          skip=("py", "ny")):
        gltf.add(part)
    for part in box_parts("Bench", wood, (width / 2 - 1.6, 0.22, 1.5), (2.4, 0.35, 0.6), skip=("py", "ny")):
        gltf.add(part)

    stage_dir = os.path.join(out_dir, "mirror_room")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "mirror_room.glb"), "wb") as f:
        f.write(gltf.build())

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "mirror_room",
        "version": "1.0.0",
        "name": {"ko": "거울 연습실", "en": "Mirror practice room", "ja": "鏡の練習室"},
        "description": {"ko": "거울 벽과 나무 바닥이 있는 연습실.", "en": "A practice room with a mirror wall."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["practice", "mirror", "interior", "test"],
        "models": [{"file": "mirror_room.glb", "name": "Room", "role": "floor"}],
        "materials": [
            {"match": "mirror_wall", "baseColor": [0.90, 0.92, 0.95, 1.0], "roughness": 0.02, "metallic": 1.0,
             "mirror": True, "castShadow": False},
            {"match": "wood_floor", "baseColor": [0.34, 0.21, 0.12, 1.0], "roughness": 0.38, "metallic": 0.0,
             "receiveShadow": True},
            {"match": "wall_paint", "roughness": 0.7},
            {"match": "ceiling", "castShadow": False},
        ],
        "lights": [
            light("ambient", "ambient", intensity=0.22),
            light("point", "ceiling_l", intensity=1.4, color=(1.0, 0.97, 0.92), position=(-55.0, 55.0, 20.0),
                  range_=160.0),
            light("point", "ceiling_r", intensity=1.4, color=(1.0, 0.97, 0.92), position=(55.0, 55.0, 20.0),
                  range_=160.0),
            light("point", "ceiling_back", intensity=1.0, color=(0.96, 0.98, 1.0), position=(0.0, 55.0, -80.0),
                  range_=160.0, shadow="nocast"),
            light("spot", "window_sun", intensity=1.8, color=(1.0, 0.93, 0.82), position=(-120.0, 90.0, 60.0),
                  aim=(0.0, 0.0, -20.0), cone=(0.32, 0.22)),
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 0.0, 0.0]},
            {"name": "mirror", "node": "Mirror_Wall"},
            {"name": "floor_center", "node": "Floor"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 18.0, 115.0], "target": [0.0, 14.0, -30.0], "fov": 45.0},
            {"name": "mirror_eye", "position": [0.0, 20.0, 40.0], "target": [0.0, 20.0, -100.0], "fov": 38.0},
        ],
        "environment": {"fog": {"density": 0.002, "color": [0.70, 0.72, 0.76], "anisotropy": 0.2}},
        "params": [
            {"key": "mirrorTint", "type": "color", "label": {"ko": "거울 색", "en": "Mirror tint"},
             "default": [0.90, 0.92, 0.95, 1.0]},
            {"key": "mirrorAmount", "type": "enum", "label": {"ko": "거울", "en": "Mirror"}, "default": "on",
             "options": [{"value": "off", "label": {"ko": "끔", "en": "Off"}},
                         {"value": "on", "label": {"ko": "켬", "en": "On"}}]},
            {"key": "lampIntensity", "type": "float", "label": {"ko": "조명 세기", "en": "Lamp intensity"},
             "default": 1.4, "min": 0.0, "max": 4.0},
            {"key": "floorGloss", "type": "float", "label": {"ko": "바닥 광택", "en": "Floor gloss"},
             "default": 0.38, "min": 0.02, "max": 1.0},
        ],
        "variants": [
            {"id": "rehearsal", "label": {"ko": "연습", "en": "Rehearsal"},
             "values": {"lampIntensity": 1.4, "floorGloss": 0.38}},
            {"id": "curtain_up", "label": {"ko": "공연", "en": "Show"},
             "values": {"mirrorTint": [0.55, 0.60, 0.72, 1.0], "lampIntensity": 2.6, "floorGloss": 0.15}},
        ],
        "defaultVariant": "rehearsal",
        "bindings": [
            {"param": "mirrorTint", "target": "materials.mirror_wall.baseColor"},
            {"param": "mirrorAmount", "target": "materials.mirror_wall.mirror"},
            {"param": "lampIntensity", "target": "lights.ceiling_l.intensity"},
            {"param": "lampIntensity", "target": "lights.ceiling_r.intensity"},
            {"param": "lampIntensity", "target": "lights.ceiling_back.intensity", "scale": 0.7},
            {"param": "floorGloss", "target": "materials.wood_floor.roughness"},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


# ---------------------------------------------------------------------------------------------------------------------
# stage 3: led stage
# ---------------------------------------------------------------------------------------------------------------------


def build_led_stage(out_dir):
    gltf = Gltf("led_stage")
    deck = gltf.material("stage_deck", base_color=(0.10, 0.10, 0.11, 1.0), roughness=0.22, metallic=0.2)
    skirt = gltf.material("stage_skirt", base_color=(0.05, 0.05, 0.06, 1.0), roughness=0.6, metallic=0.1)
    truss = gltf.material("truss_metal", base_color=(0.55, 0.56, 0.58, 1.0), roughness=0.35, metallic=1.0)
    led_main = gltf.material("led_main", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                            emissive=(0.4, 0.5, 0.8), double_sided=True)
    led_left = gltf.material("led_left", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                            emissive=(0.3, 0.6, 0.7), double_sided=True)
    led_right = gltf.material("led_right", base_color=(0.02, 0.02, 0.03, 1.0), roughness=0.35, metallic=0.0,
                             emissive=(0.8, 0.3, 0.5), double_sided=True)

    deck_w, deck_h, deck_d = 18.0, 1.2, 10.0
    # the raised stage: top, front and side faces (the back is against the LED wall)
    gltf.add(grid_part("Stage_Top", deck, ((-deck_w / 2, deck_h, -deck_d / 2), (deck_w / 2, deck_h, -deck_d / 2),
                                          (-deck_w / 2, deck_h, deck_d / 2)), 9, 5, uv_scale=(9.0, 5.0)))
    gltf.add(grid_part("Stage_Front", skirt, ((-deck_w / 2, deck_h, deck_d / 2), (deck_w / 2, deck_h, deck_d / 2),
                                             (-deck_w / 2, 0.0, deck_d / 2)), 9, 1))
    gltf.add(grid_part("Stage_Left", skirt, ((-deck_w / 2, deck_h, -deck_d / 2), (-deck_w / 2, deck_h, deck_d / 2),
                                            (-deck_w / 2, 0.0, -deck_d / 2)), 5, 1))
    gltf.add(grid_part("Stage_Right", skirt, ((deck_w / 2, deck_h, deck_d / 2), (deck_w / 2, deck_h, -deck_d / 2),
                                             (deck_w / 2, 0.0, deck_d / 2)), 5, 1))
    # three screen quads, tilted slightly toward the audience, standing behind the stage
    z_back = -deck_d / 2 - 3.0
    gltf.add(grid_part("Screen_Main", led_main,
                       ((-6.0, 1.2, z_back), (6.0, 1.2, z_back), (-6.0, 8.2, z_back)), 6, 4,
                       uv_scale=(6.0, 4.0)))
    gltf.add(grid_part("Screen_Left", led_left,
                       ((-13.0, 1.2, z_back + 0.5), (-7.2, 1.2, z_back + 0.5), (-13.0, 6.6, z_back + 0.5)), 3, 3,
                       uv_scale=(3.0, 3.0)))
    gltf.add(grid_part("Screen_Right", led_right,
                       ((7.2, 1.2, z_back + 0.5), (13.0, 1.2, z_back + 0.5), (7.2, 6.6, z_back + 0.5)), 3, 3,
                       uv_scale=(3.0, 3.0)))
    # truss: two horizontal beams over the stage and four posts
    for name, center, size in (
            ("Truss_Front", (0.0, 8.6, deck_d / 2 - 1.0), (deck_w + 1.0, 0.35, 0.35)),
            ("Truss_Back", (0.0, 8.6, z_back + 1.5), (deck_w + 1.0, 0.35, 0.35)),
            ("Truss_Post_LF", (-(deck_w / 2 + 0.2), 4.3, deck_d / 2 - 1.0), (0.25, 8.6, 0.25)),
            ("Truss_Post_RF", (deck_w / 2 + 0.2, 4.3, deck_d / 2 - 1.0), (0.25, 8.6, 0.25)),
            ("Truss_Post_LB", (-(deck_w / 2 + 0.2), 4.3, z_back + 1.5), (0.25, 8.6, 0.25)),
            ("Truss_Post_RB", (deck_w / 2 + 0.2, 4.3, z_back + 1.5), (0.25, 8.6, 0.25))):
        for part in box_parts(name, truss, center, size):
            gltf.add(part)

    stage_dir = os.path.join(out_dir, "led_stage")
    os.makedirs(stage_dir, exist_ok=True)
    with open(os.path.join(stage_dir, "led_stage.glb"), "wb") as f:
        f.write(gltf.build())
    # the LED textures the screens reference (a soft gradient)
    def gradient(x, y):
        return (int(40 + 120 * x / 63), int(60 + 90 * y / 35), 180)
    write_png(os.path.join(out_dir, "led_stage", "textures", "led_main.png"), 64, 36, gradient)

    spots = []
    for i in range(8):
        side = -1.0 if i % 2 == 0 else 1.0
        row = i // 2
        spots.append(light(
            "spot", f"spot_{i + 1}", intensity=2.8,
            color=[(0.25, 0.8, 0.78), (0.95, 0.35, 0.62), (1.0, 0.95, 0.88), (0.55, 0.55, 1.0)][i % 4],
            position=[side * (30.0 + 18.0 * row), 105.0, 45.0 - 8.0 * row],
            aim=[side * 8.0, 8.0, -45.0], cone=(0.22, 0.13), aim_mode="sway", sway_phase=i * 1.1))

    manifest = {
        "format": "mmdx-stage",
        "formatVersion": 1,
        "id": "led_stage",
        "version": "1.0.0",
        "name": {"ko": "LED 아레나 스테이지", "en": "LED arena stage", "ja": "LEDアリーナステージ"},
        "description": {"ko": "LED 스크린 세 면과 트러스, 8개의 무빙 스팟이 있는 아레나 무대.",
                        "en": "An arena stage with three LED screens, truss bars and eight moving spots."},
        "authors": [{"name": "MMDX stagegen", "role": "procedural"}],
        "license": "CC0-1.0",
        "tags": ["arena", "led", "concert", "test"],
        "models": [{"file": "led_stage.glb", "name": "Arena", "role": "floor"}],
        "materials": [
            {"match": "stage_deck", "baseColor": [0.10, 0.10, 0.11, 1.0], "roughness": 0.22, "metallic": 0.2},
            {"match": "stage_skirt", "roughness": 0.6},
            {"match": "truss_metal", "metallic": 1.0, "roughness": 0.35},
            {"match": "led_*", "screen": "main", "castShadow": False},
            {"match": "led_left", "screen": "left", "castShadow": False},
            {"match": "led_right", "screen": "right", "castShadow": False},
        ],
        "lights": [
            light("sun", "key", intensity=0.35, color=(0.55, 0.60, 0.75), direction=(0.2, -1.0, 0.35)),
            light("ambient", "ambient", intensity=0.08, color=(1.0, 1.0, 1.0)),
            light("point", "crowd_fill", intensity=0.5, color=(0.85, 0.90, 1.0), position=(0.0, 18.0, 120.0),
                  range_=220.0, shadow="nocast"),
            *spots,
        ],
        "sockets": [
            {"name": "world", "position": [0.0, 15.0, -100.0]},
            {"name": "truss_center", "node": "Truss_Front"},
            {"name": "stage_center", "position": [0.0, 15.0, 0.0]},
            {"name": "screen_main", "node": "Screen_Main"},
        ],
        "cameras": [
            {"name": "wide", "position": [0.0, 28.0, 190.0], "target": [0.0, 32.0, -40.0], "fov": 40.0},
            {"name": "stage", "position": [0.0, 24.0, 70.0], "target": [0.0, 30.0, -60.0], "fov": 50.0},
            {"name": "crowd", "position": [0.0, 45.0, 300.0], "target": [0.0, 25.0, -20.0], "fov": 32.0},
        ],
        "screens": [
            {"id": "main", "materials": ["led_main"], "source": "image", "file": "textures/led_main.png",
             "width": 2048, "height": 1024, "brightness": 1.6, "emitLight": True},
            {"id": "left", "materials": ["led_left"], "source": "camera", "camera": "stage", "width": 1024,
             "height": 1024, "brightness": 1.2, "emitLight": True},
            {"id": "right", "materials": ["led_right"], "source": "none", "width": 1024, "height": 1024,
             "brightness": 1.0, "emitLight": False},
        ],
        "environment": {
            "fog": {"density": 0.020, "color": [0.55, 0.60, 0.72], "anisotropy": 0.55, "heightFalloff": 0.03},
            "ambient": {"intensity": 0.08, "skyZenith": [0.03, 0.04, 0.08], "ground": [0.05, 0.05, 0.06]},
        },
        "groups": [
            {"id": "screens", "label": {"ko": "스크린", "en": "Screens"}},
            {"id": "lights", "label": {"ko": "조명", "en": "Lights"}},
        ],
        "params": [
            {"key": "screenBrightness", "type": "float", "label": {"ko": "스크린 밝기", "en": "Screen brightness"},
             "group": "screens", "default": 1.6, "min": 0.0, "max": 4.0},
            {"key": "mainScreen", "type": "enum", "label": {"ko": "메인 소스", "en": "Main source"},
             "group": "screens", "default": "image",
             "options": [{"value": "none"}, {"value": "image"}, {"value": "camera"}, {"value": "video"}]},
            {"key": "spotLevel", "type": "float", "label": {"ko": "스팟 세기", "en": "Spot level"},
             "group": "lights", "default": 2.8, "min": 0.0, "max": 6.0},
            {"key": "haze", "type": "float", "label": {"ko": "헤이즈", "en": "Haze"}, "group": "lights",
             "default": 0.020, "min": 0.0, "max": 0.08},
        ],
        "variants": [
            {"id": "concert", "label": {"ko": "콘서트", "en": "Concert"},
             "values": {"screenBrightness": 1.6, "spotLevel": 2.8, "haze": 0.020}},
            {"id": "rehearsal", "label": {"ko": "리허설", "en": "Rehearsal"},
             "values": {"screenBrightness": 0.6, "spotLevel": 1.2, "haze": 0.004}},
            {"id": "blackout", "label": {"ko": "블랙아웃", "en": "Blackout"},
             "values": {"screenBrightness": 0.0, "spotLevel": 0.0, "haze": 0.03}},
        ],
        "defaultVariant": "concert",
        "bindings": [
            {"param": "screenBrightness", "target": "screens.main.brightness"},
            {"param": "screenBrightness", "target": "screens.left.brightness", "scale": 0.75},
            {"param": "mainScreen", "target": "screens.main.source"},
            {"param": "spotLevel", "target": "lights.spot_1.intensity"},
            {"param": "spotLevel", "target": "lights.spot_2.intensity"},
            {"param": "spotLevel", "target": "lights.spot_3.intensity"},
            {"param": "spotLevel", "target": "lights.spot_4.intensity"},
            {"param": "spotLevel", "target": "lights.spot_5.intensity"},
            {"param": "spotLevel", "target": "lights.spot_6.intensity"},
            {"param": "spotLevel", "target": "lights.spot_7.intensity"},
            {"param": "spotLevel", "target": "lights.spot_8.intensity"},
            {"param": "haze", "target": "environment.fog.density"},
        ],
        "cues": [
            {"param": "spotLevel", "stepped": False, "keys": [
                {"frame": 0, "value": 0.4}, {"frame": 120, "value": 3.2},
                {"frame": 240, "value": 1.2}, {"frame": 360, "value": 3.6}]},
            {"param": "screenBrightness", "keys": [
                {"frame": 0, "value": 0.8}, {"frame": 120, "value": 2.4}, {"frame": 360, "value": 1.4}]},
        ],
    }
    with open(os.path.join(stage_dir, "stage.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
    return stage_dir


STAGES = {
    "cyclorama": build_cyclorama,
    "mirror_room": build_mirror_room,
    "led_stage": build_led_stage,
}


def main():
    parser = argparse.ArgumentParser(description="write the procedural MMDX test stages")
    parser.add_argument("--out", default=os.path.join("captures", "stages"), help="output folder")
    parser.add_argument("--only", default="", help="comma separated stage names (default: all)")
    args = parser.parse_args()
    root = os.path.abspath(args.out)
    os.makedirs(root, exist_ok=True)
    names = [n.strip() for n in args.only.split(",") if n.strip()] or list(STAGES)
    for name in names:
        if name not in STAGES:
            raise SystemExit(f"unknown stage: {name} (known: {', '.join(STAGES)})")
        os.makedirs(os.path.join(root, name), exist_ok=True)
        written = STAGES[name](root)
        size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(written) for f in fs)
        print(f"{name}: {os.path.relpath(written, os.getcwd())} ({size / 1024.0:.1f} KiB)")


if __name__ == "__main__":
    main()
