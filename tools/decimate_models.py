#!/usr/bin/env python3
"""Decimates a .glb under assets/models/ to a target triangle count, via Blender.

Some of the models are downloaded art rather than the box piles gen_models.py
writes, and a few of them are far denser than this game has any use for. That is
free when a model is drawn rigid -- the GPU does not care -- but it stops being
free the moment a mesh is skinned on the CPU every frame, which is what a soft-body
meat does (see src/soft_body.cpp). The burger arrived at 13,226 vertices, thirty-six
times the steak beside it.

This runs INSIDE Blender, not under the system Python: it needs `bpy`, and the glTF
importer/exporter and the Decimate modifier that come with it. Like gen_models.py it
is deliberately not wired into CMake -- building a C++ game should not require
Blender. Run it by hand and commit what it writes:

    blender -b -P tools/decimate_models.py -- assets/models/burger-raw.glb --target-tris 5000

Add --inplace to overwrite the asset; without it the result is written beside the
input with a .decimated.glb suffix, so a bad ratio costs nothing. Either way git is
the real undo.

Blender not on PATH? Use the full path to the executable, e.g.
    "C:\\Program Files\\Blender Foundation\\Blender 5.1\\blender.exe"

Decimating is not just thinning: it can break a mesh
----------------------------------------------------
A naive collapse of the burger to 2,000 triangles came out visibly holed -- the
patty rendered speckled with the grass showing through it -- and, worse, made
PhysX's tetrahedral cooker assert and take the process down. Both symptoms were the
same defect: edge collapses had left degenerate faces and non-manifold edges behind.

So the collapse is bracketed by cleanup. Beforehand, duplicate vertices are merged:
downloaded art often ships a surface split along every seam, and collapsing a mesh
whose triangles are not actually joined tears it apart. Afterwards, zero-area faces
are dissolved, orphaned geometry is deleted and the normals are made to face outward
again.

It also reports mesh health on the way through -- non-manifold edges, loose
vertices, degenerate faces -- because "did this come out clean" is the question that
decides whether the result is usable, and it is not one you can answer by looking at
a triangle count. If the numbers after are worse than the numbers before, raise the
target rather than shipping it.

Why a round trip through Blender is safe here
---------------------------------------------
Re-exporting rewrites the whole file, not just the triangle count, so the loader's
expectations have to survive it. The one convention src/model.cpp depends on is the
`_nocollide` suffix that marks a primitive decorative -- and that lives on the node
name, which Blender keeps in its outliner and writes back out. model.cpp:71 says as
much: the suffix was chosen over a custom `extras` field precisely because it
survives this trip. Verified: node names, material names, image count and the
attribute set all came back byte-identical.

What is NOT guaranteed to survive is anything the exporter re-authors: images may be
re-encoded, and tangents are re-derived rather than passed through. Neither matters
much here -- the loader manufactures tangents when a file omits them -- but check the
file size afterwards, and look at the thing in game before committing it.
"""

from __future__ import annotations

import pathlib
import sys

import bmesh
import bpy


def parse_args(argv: list[str]) -> tuple[pathlib.Path, int, bool]:
    """Reads the arguments after Blender's own `--` separator."""
    if "--" not in argv:
        raise SystemExit("usage: blender -b -P tools/decimate_models.py -- <model.glb> "
                         "--target-tris N [--inplace]")
    args = argv[argv.index("--") + 1:]
    if not args:
        raise SystemExit("no model given")

    source = pathlib.Path(args[0]).resolve()
    if not source.is_file():
        raise SystemExit(f"no such file: {source}")

    target_tris = 5000
    inplace = False
    i = 1
    while i < len(args):
        if args[i] == "--target-tris":
            i += 1
            target_tris = int(args[i])
        elif args[i] == "--inplace":
            inplace = True
        else:
            raise SystemExit(f"unrecognised argument: {args[i]}")
        i += 1
    return source, target_tris, inplace


def meshes() -> list[bpy.types.Object]:
    return [o for o in bpy.data.objects if o.type == "MESH"]


def health() -> dict[str, int]:
    """Counts the defects that make a mesh unusable, across every object.

    Non-manifold edges and degenerate faces are what the tetrahedral cooker chokes
    on and what renders as holes; loose vertices are harmless but a good sign that a
    collapse went wrong.
    """
    totals = {"tris": 0, "nonmanifold": 0, "loose": 0, "degenerate": 0}
    for obj in meshes():
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        totals["tris"] += len(bm.faces)
        totals["nonmanifold"] += sum(1 for e in bm.edges if not e.is_manifold)
        totals["loose"] += sum(1 for v in bm.verts if not v.link_edges)
        totals["degenerate"] += sum(1 for f in bm.faces if f.calc_area() < 1e-12)
        bm.free()
    return totals


def edit_all(operator, **kwargs) -> None:
    """Runs a mesh operator over every object with everything selected."""
    for obj in meshes():
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        operator(**kwargs)
        bpy.ops.object.mode_set(mode="OBJECT")


def clean(merge_distance: float | None) -> None:
    """Merges duplicates (optionally), drops degenerate and orphaned geometry, and
    points the normals outward again."""
    if merge_distance is not None:
        edit_all(bpy.ops.mesh.remove_doubles, threshold=merge_distance)
    edit_all(bpy.ops.mesh.dissolve_degenerate)
    edit_all(bpy.ops.mesh.delete_loose)
    edit_all(bpy.ops.mesh.normals_make_consistent, inside=False)


def report(label: str, h: dict[str, int]) -> None:
    print(f"  {label:9} tris {h['tris']:6}  non-manifold {h['nonmanifold']:5}  "
          f"loose {h['loose']:5}  degenerate {h['degenerate']:5}")


def main() -> None:
    source, target_tris, inplace = parse_args(sys.argv)
    destination = source if inplace else source.with_suffix(".decimated.glb")

    # Blender starts with a default cube, camera and light; none of them belong in
    # the exported file.
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(source))
    if not meshes():
        raise SystemExit(f"{source.name} has no mesh geometry")

    print(f"{source.name}")
    report("imported", health())

    # Weld the surface back together before collapsing it. Downloaded art is often
    # split along every UV seam and every smoothing boundary, which leaves the mesh
    # topologically a pile of disconnected shells: collapse that and the shells pull
    # apart into the holes we saw. The threshold is tight enough to only catch
    # vertices that are meant to be the same point.
    clean(merge_distance=1e-5)
    welded = health()
    report("welded", welded)

    # One ratio for the whole model rather than per object, so the pieces stay in
    # proportion to each other -- decimating a 40-triangle part and a 40,000-triangle
    # part to the same target would dissolve the small one entirely.
    ratio = min(1.0, target_tris / max(1, welded["tris"]))
    for obj in meshes():
        bpy.context.view_layer.objects.active = obj
        modifier = obj.modifiers.new(name="decimate", type="DECIMATE")
        # Collapse is the quadric-error method: it contracts edges in the order that
        # least disturbs the surface, which is what keeps a silhouette recognisable.
        # The alternatives (un-subdivide, planar) want topology this art does not have.
        modifier.decimate_type = "COLLAPSE"
        modifier.ratio = ratio
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    report("collapsed", health())

    # Collapsing leaves slivers behind. These are what the cooker asserts on, so they
    # are swept up before the mesh is written. No merge here: the collapse has already
    # decided which vertices are shared.
    clean(merge_distance=None)
    after = health()
    report("cleaned", after)

    bpy.ops.export_scene.gltf(filepath=str(destination), export_format="GLB")

    verdict = "clean" if after["nonmanifold"] == 0 and after["degenerate"] == 0 else \
              "STILL DEFECTIVE -- raise --target-tris"
    print(f"  -> {destination.name} ({destination.stat().st_size // 1024} KB), "
          f"ratio {ratio:.3f}, {verdict}")


if __name__ == "__main__":
    main()
