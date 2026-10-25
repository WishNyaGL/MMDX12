"""Generates tools/testdata/ext_test.glb: the S2a glTF material / light extension test model.

Run headless (Blender 5.2, see captures/blender_setup.md):

    "C:\\Program Files\\Blender Foundation\\Blender 5.2\\blender.exe" -b --factory-startup \
        --python-exit-code 1 --python tools/testdata/make_ext_test.py

The GLB is committed so gltf_pbr_test / stage_check never need Blender. It carries, one object each:
  * Glass     - transmissive sphere, KHR_materials_transmission + KHR_materials_ior + KHR_materials_volume
                (thickness + attenuation colour / distance), i.e. the S2b / stage glass case.
  * GlassBlock - S2b: a thick tinted glass block (volume thickness + green attenuation), refraction + coloured shadow.
  * Pane      - S2b: a thin-walled window pane (transmission 1, no volume): straight through, Fresnel reflection.
  * Acrylic   - S2b: a frosted acrylic panel (transmission 1, roughness 0.4, no volume): the refraction is GGX-blurred.
  * Panel     - emissive strength 15 (KHR_materials_emissive_strength): the LED-panel HDR case.
  * Coat      - red sphere with KHR_materials_clearcoat (factor 1, roughness 0.05).
  * Checker   - a plane whose base colour map has a KHR_texture_transform (offset / scale / rotation).
  * KeySpot   - one spot light, KeyPoint - one point light (KHR_lights_punctual).
Everything sits in a small scene at the origin so a stage can drop it in unchanged.
"""
import math
import os
import sys

import bpy
import bmesh  # noqa: F401  (imported for parity with other generator scripts; not required here)
from mathutils import Vector

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "ext_test.glb")


def clear_scene():
    for obj in list(bpy.data.objects):
        bpy.data.objects.remove(obj, do_unlink=True)
    for block in (bpy.data.meshes, bpy.data.materials, bpy.data.lights, bpy.data.images):
        for item in list(block):
            block.remove(item)


def principled(mat):
    mat.use_nodes = True
    return next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")


def set_input(node, names, value):
    """Sets the first socket that exists (Blender renames Principled inputs between versions)."""
    for name in names:
        socket = node.inputs.get(name)
        if socket is not None:
            socket.default_value = value
            return True
    print("WARN missing socket one of", names)
    return False


def add_sphere(name, location, radius, material):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=32, ring_count=16, radius=radius, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.append(material)
    bpy.ops.object.shade_smooth()
    return obj


def add_plane(name, location, size, material):
    bpy.ops.mesh.primitive_plane_add(size=size, location=location)
    obj = bpy.context.active_object
    obj.name = name
    obj.data.materials.append(material)
    return obj


def make_checker_image():
    """A 64x64 apron checker as an in-memory image (packed into the GLB)."""
    size = 64
    img = bpy.data.images.new("checker", width=size, height=size, alpha=False)
    px = [0.0] * (size * size * 4)
    for y in range(size):
        for x in range(size):
            on = ((x // 8) + (y // 8)) % 2 == 0
            i = (y * size + x) * 4
            v = 1.0 if on else 0.08
            px[i + 0] = v
            px[i + 1] = v * 0.35 if on else v
            px[i + 2] = v * 0.35 if on else v
            px[i + 3] = 1.0
    img.pixels = px
    img.pack()
    return img


def make_gradient_image(name):
    """A 64x64 map for the two extension maps: KHR_materials_transmission reads its R channel and the
    KHR_materials_volume thickness factor is the G channel. Both are constant here (1) so the sphere stays a clean
    reference: a gradient would band a UV sphere, whose u is the longitude."""
    size = 64
    img = bpy.data.images.new(name, width=size, height=size, alpha=False)
    px = [0.0] * (size * size * 4)
    for y in range(size):
        for x in range(size):
            v = x / (size - 1.0)
            i = (y * size + x) * 4
            px[i + 0] = 1.0               # R: transmission (constant: the sphere stays clear; the map is still sampled)
            px[i + 1] = 1.0               # G: thickness (constant: a gradient bands a UV sphere)
            px[i + 2] = v
            px[i + 3] = 1.0
    img.pixels = px
    img.pack()
    return img


def gltf_output_group(mat, thickness=1.0):
    """KHR_materials_volume travels through Blender's `glTF Material Output` node group (the exporter's convention: it
    reads the group's `Thickness` socket, and the attenuation colour / distance from the material's Volume Absorption
    node, `attenuationDistance = 1 / density`). Blender ships no such group in its asset library, so one is created
    with the name the exporter looks for."""
    group = bpy.data.node_groups.get("glTF Material Output")
    if group is None:
        group = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
        group.interface.new_socket("Thickness", in_out="INPUT", socket_type="NodeSocketFloat").default_value = 0.0
        # Blender 5.2 exports a "Principled-only" inline material by default and that path drops node groups, so the
        # volume extension would be lost. An Occlusion socket makes the exporter keep the original node tree
        # (io_scene_gltf2 materials.py __can_use_inline checks for it).
        group.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat").default_value = 1.0
        group.nodes.new("NodeGroupOutput")
        group.nodes.new("NodeGroupInput")
    node = mat.node_tree.nodes.new("ShaderNodeGroup")
    node.node_tree = group
    node.inputs["Thickness"].default_value = thickness
    return node


def main():
    clear_scene()

    # ---- Glass: transmission 1, ior 1.45, volume thickness + attenuation -------------------------------------
    glass = bpy.data.materials.new("Glass")
    bsdf = principled(glass)
    set_input(bsdf, ["Base Color"], (0.92, 0.96, 1.0, 1.0))
    set_input(bsdf, ["Roughness"], 0.03)
    set_input(bsdf, ["Transmission Weight", "Transmission"], 1.0)
    set_input(bsdf, ["IOR"], 1.45)
    # KHR_materials_volume: the exporter reads Blender's `glTF Material Output` group (thickness / attenuation). A
    # Volume Absorption node is what Blender renders with, so it is kept for the .blend's own look.
    out = next(n for n in glass.node_tree.nodes if n.type == "OUTPUT_MATERIAL")
    absorb = glass.node_tree.nodes.new("ShaderNodeVolumeAbsorption")
    absorb.inputs["Color"].default_value = (0.75, 0.92, 0.85, 1.0)
    absorb.inputs["Density"].default_value = 0.4
    glass.node_tree.links.new(absorb.outputs["Volume"], out.inputs["Volume"])
    gltf_output_group(glass, thickness=0.6)
    # KHR_materials_transmission.texture (R) and KHR_materials_volume.thicknessTexture (G): Blender exports both from
    # image files on the Principled transmission / thickness inputs, with the material's own texture transform.
    transmission_img = make_gradient_image("transmission")
    tex_node = glass.node_tree.nodes.new("ShaderNodeTexImage")
    tex_node.image = transmission_img
    glass.node_tree.links.new(tex_node.outputs["Color"], bsdf.inputs["Transmission Weight"])
    # The volume thickness texture reads the same image's G channel (Separate Color -> the group's Thickness input).
    group_node = next((n for n in glass.node_tree.nodes if n.type == "GROUP" and n.node_tree
                       and n.node_tree.name.lower().startswith("gltf material output")), None)
    if group_node is not None and group_node.inputs.get("Thickness") is not None:
        separate = glass.node_tree.nodes.new("ShaderNodeSeparateColor")
        glass.node_tree.links.new(tex_node.outputs["Color"], separate.inputs["Color"])
        glass.node_tree.links.new(separate.outputs["Green"], group_node.inputs["Thickness"])
    add_sphere("Glass", (-4.0, 0.0, 1.2), 1.2, glass)

    # ---- S2b mesh dielectrics: block / pane / frosted acrylic ------------------------------------------------
    # Blender's exporter writes KHR_materials_transmission from the Principled transmission weight; a material without
    # the `glTF Material Output` group's Thickness (or with 0) exports no KHR_materials_volume = thin-walled.
    def transmissive_material(name, base_color, roughness, thickness=0.0, attenuation=None, attenuation_distance=1.0):
        mat = bpy.data.materials.new(name)
        bsdf = principled(mat)
        set_input(bsdf, ["Base Color"], base_color)
        set_input(bsdf, ["Roughness"], roughness)
        set_input(bsdf, ["Transmission Weight", "Transmission"], 1.0)
        set_input(bsdf, ["IOR"], 1.5)
        if thickness > 0.0:
            gltf_output_group(mat, thickness=thickness)
            if attenuation is not None:
                out = next(n for n in mat.node_tree.nodes if n.type == "OUTPUT_MATERIAL")
                absorb = mat.node_tree.nodes.new("ShaderNodeVolumeAbsorption")
                absorb.inputs["Color"].default_value = attenuation
                # attenuationDistance = 1 / density (the exporter convention)
                absorb.inputs["Density"].default_value = 1.0 / attenuation_distance
                mat.node_tree.links.new(absorb.outputs["Volume"], out.inputs["Volume"])
        return mat

    block_mat = transmissive_material("GlassBlock", (0.55, 0.85, 0.72, 1.0), 0.03, thickness=1.2,
                                      attenuation=(0.55, 0.92, 0.70, 1.0), attenuation_distance=1.5)
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(-2.6, -1.7, 0.9))
    block = bpy.context.active_object
    block.name = "GlassBlock"
    block.scale = (1.4, 1.4, 1.4)
    block.data.materials.append(block_mat)

    # Thin-walled window pane: transmission, no volume (KHR_materials_volume absent) -> no bending, no absorption.
    pane_mat = transmissive_material("Pane", (0.90, 0.95, 1.0, 1.0), 0.02)
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(1.5, -1.5, 1.25))
    pane = bpy.context.active_object
    pane.name = "Pane"
    pane.scale = (1.3, 0.04, 1.3)
    pane.data.materials.append(pane_mat)

    # Frosted acrylic: roughness 0.4 makes each crossing's refraction normal GGX-perturbed. It is given a volume
    # (thickness, no attenuation) so the ray crosses twice and the two perturbed crossings blur the see-through.
    acrylic_mat = transmissive_material("Acrylic", (0.86, 0.88, 0.95, 1.0), 0.4, thickness=0.8)
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(2.75, -2.2, 1.1))
    acrylic = bpy.context.active_object
    acrylic.name = "Acrylic"
    acrylic.scale = (1.5, 0.08, 1.5)
    acrylic.data.materials.append(acrylic_mat)

    # ---- Panel: emission strength 15 (HDR) ------------------------------------------------------------------
    panel_mat = bpy.data.materials.new("Panel")
    bsdf = principled(panel_mat)
    set_input(bsdf, ["Base Color"], (0.05, 0.05, 0.05, 1.0))
    set_input(bsdf, ["Roughness"], 0.4)
    set_input(bsdf, ["Emission Color", "Emission"], (0.25, 0.85, 1.0, 1.0))
    set_input(bsdf, ["Emission Strength"], 15.0)
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, -3.0, 1.6))
    panel = bpy.context.active_object
    panel.name = "Panel"
    panel.scale = (2.2, 0.12, 1.1)
    panel.data.materials.append(panel_mat)

    # ---- Coat: red sphere, clearcoat 1 / roughness 0.05 ------------------------------------------------------
    coat = bpy.data.materials.new("Coat")
    bsdf = principled(coat)
    set_input(bsdf, ["Base Color"], (0.65, 0.03, 0.03, 1.0))
    set_input(bsdf, ["Roughness"], 0.5)
    set_input(bsdf, ["Coat Weight", "Clearcoat"], 1.0)
    set_input(bsdf, ["Coat Roughness", "Clearcoat Roughness"], 0.05)
    add_sphere("Coat", (4.0, 0.0, 1.2), 1.2, coat)

    # ---- Checker: a base colour map with a KHR_texture_transform --------------------------------------------
    checker_mat = bpy.data.materials.new("Checker")
    bsdf = principled(checker_mat)
    set_input(bsdf, ["Roughness"], 0.6)
    img = make_checker_image()
    tex = checker_mat.node_tree.nodes.new("ShaderNodeTexImage")
    tex.image = img
    mapping = checker_mat.node_tree.nodes.new("ShaderNodeMapping")
    mapping.inputs["Location"].default_value = (0.25, 0.1, 0.0)
    mapping.inputs["Rotation"].default_value = (0.0, 0.0, math.radians(30.0))
    mapping.inputs["Scale"].default_value = (2.0, 2.0, 1.0)
    uvn = checker_mat.node_tree.nodes.new("ShaderNodeUVMap")
    checker_mat.node_tree.links.new(uvn.outputs["UV"], mapping.inputs["Vector"])
    checker_mat.node_tree.links.new(mapping.outputs["Vector"], tex.inputs["Vector"])
    checker_mat.node_tree.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    add_plane("Checker", (0.0, 0.0, 0.02), 20.0, checker_mat)

    # ---- Lights: one spot + one point (KHR_lights_punctual) --------------------------------------------------
    # Blender coords: Z up, -Y is the model's front (the exporter maps Blender (x, y, z) -> glTF (x, z, -y)).
    spot_data = bpy.data.lights.new("KeySpot", type="SPOT")
    spot_data.energy = 1000.0           # W
    spot_data.color = (1.0, 0.95, 0.9)
    spot_data.spot_size = math.radians(45.0)
    spot_data.spot_blend = 0.4
    spot = bpy.data.objects.new("KeySpot", spot_data)
    spot.location = (0.0, -6.0, 3.0)   # in front of the stage (MMD -Z is the camera side)
    spot.rotation_euler = (Vector((0.0, 0.0, 1.0)) - spot.location).to_track_quat("-Z", "Y").to_euler()
    bpy.context.collection.objects.link(spot)

    point_data = bpy.data.lights.new("KeyPoint", type="POINT")
    point_data.energy = 1000.0          # W: the unit-conversion reference (5 m away reads like the fill)
    point_data.color = (1.0, 0.8, 0.6)
    point_data.shadow_soft_size = 0.1
    point = bpy.data.objects.new("KeyPoint", point_data)
    point.location = (5.0, -5.0, 5.0)
    bpy.context.collection.objects.link(point)

    # ---- Export ----------------------------------------------------------------------------------------------
    bpy.ops.export_scene.gltf(
        filepath=OUT,
        export_format="GLB",
        export_lights=True,
        export_apply=True,
        export_animations=False,
        use_selection=False,
    )

    # Report what actually made it into the file so a future Blender version does not silently drop an extension.
    import json
    import struct

    with open(OUT, "rb") as f:
        data = f.read()
    json_len = struct.unpack_from("<I", data, 12)[0]
    gltf = json.loads(data[20:20 + json_len].decode("utf-8"))
    used = sorted(gltf.get("extensionsUsed", []))
    print("EXT_TEST extensionsUsed:", ", ".join(used))
    for mat in gltf.get("materials", []):
        print("EXT_TEST material", mat.get("name"), "->", ", ".join(sorted(mat.get("extensions", {}))))
    print("EXT_TEST lights:", len(gltf.get("extensions", {}).get("KHR_lights_punctual", {}).get("lights", [])))
    print("EXT_TEST wrote", OUT, os.path.getsize(OUT), "bytes")
    for need in ("KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_ior",
                 "KHR_materials_emissive_strength", "KHR_materials_clearcoat", "KHR_texture_transform",
                 "KHR_lights_punctual"):
        if need not in used:
            print("EXT_TEST MISSING", need)
            sys.exit(1)


main()
