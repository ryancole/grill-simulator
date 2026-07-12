#!/usr/bin/env python3
"""Generates the .glb models under assets/models/ from box primitives.

This is a stopgap: the game has no artist, and hand-editing binary glTF is not a
thing anyone should do. Every model here is still a pile of boxes -- the same
boxes the code used to build at runtime -- but they now travel through the real
asset pipeline, so replacing one with something exported from Blender is a file
copy rather than a code change.

The script is deliberately *not* wired into CMake. Building a C++ game should not
require a Python interpreter. Run it by hand and commit what it writes:

    python tools/gen_models.py

Only the standard library is used: zlib writes the PNGs, struct writes the GLBs.

Coordinates here are glTF's, not the game's
-------------------------------------------
glTF is right-handed with +Y up and -Z forward. The game is left-handed with +Z
north (it draws through XMMatrixPerspectiveFovLH). The two differ by a mirror
through Z, and `Model::Load` undoes it on the way in. So a point the game calls
(x, y, z) is written here as (x, y, -z), which `project()` does for us; a yaw the
game calls +t is written as -t, which `yaw_quaternion()` does.

Winding follows the glTF spec: counter-clockwise when seen from outside. That is
the same rule `MakeUnitCubeModel` states as "cross(v1 - v0, v2 - v0) is the
outward normal", because in a right-handed frame the two say the same thing. The
loader reverses each triangle to pay for the mirror, and the invariant survives.
"""

from __future__ import annotations

import json
import math
import pathlib
import struct
import zlib

ASSETS = pathlib.Path(__file__).resolve().parent.parent / "assets" / "models"

# Component types.
UNSIGNED_SHORT = 5123
FLOAT = 5126

# Sampler filters and wrap modes.
LINEAR = 9729
LINEAR_MIPMAP_LINEAR = 9987
REPEAT = 10497

TRIANGLES = 4


def project(x: float, y: float, z: float) -> tuple[float, float, float]:
    """Game space -> glTF space. An involution: applying it twice is identity."""
    return (x, y, -z)


def yaw_quaternion(degrees: float) -> list[float]:
    """A rotation about +Y of `degrees` *as the game will see it*.

    The mirror through Z turns a glTF yaw of t into a game yaw of -t, so the
    angle is negated on the way into the file.
    """
    half = math.radians(-degrees) / 2.0
    return [0.0, math.sin(half), 0.0, math.cos(half)]


# --- Textures -----------------------------------------------------------------


def noise(seed: int) -> float:
    """A deterministic hash in [0, 1).

    Not `random`: the committed .glb files must be byte-for-byte reproducible on
    any machine and any Python build.
    """
    seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
    seed ^= seed >> 15
    seed = (seed * 2654435761) & 0x7FFFFFFF
    return (seed & 0xFFFF) / 65535.0


def smooth_noise(x: int, y: int, width: int, height: int, period: int, seed: int) -> float:
    """Value noise on a lattice of `period`-pixel cells, smoothly interpolated.

    Sampling `noise()` per cell and leaving it at that gives hard square blocks --
    which is what the canopy looked like on the first attempt. Interpolating the
    four surrounding lattice points with a smoothstep rounds them off.

    The lattice wraps every `width // period` cells, so the result tiles: the
    canopy repeats its texture twice across each face and must not show a seam.
    """
    assert width % period == 0 and height % period == 0

    cells_x, cells_y = width // period, height // period

    def lattice(ix: int, iy: int) -> float:
        return noise((ix % cells_x) * 7919 + (iy % cells_y) * 104729 + seed * 15485863)

    gx, gy = x / period, y / period
    ix, iy = int(gx), int(gy)
    fx, fy = gx - ix, gy - iy
    # Smoothstep, so the lattice points are not visible as creases.
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)

    top = lattice(ix, iy) * (1 - sx) + lattice(ix + 1, iy) * sx
    bottom = lattice(ix, iy + 1) * (1 - sx) + lattice(ix + 1, iy + 1) * sx
    return top * (1 - sy) + bottom * sy


def write_png(width: int, height: int, rgba: bytes) -> bytes:
    """A minimal 8-bit RGBA PNG. No interlacing, one IDAT, filter type 0."""
    assert len(rgba) == width * height * 4

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    # Each scanline is prefixed with its filter byte, which is 0 -- "None".
    raw = b"".join(
        b"\x00" + rgba[row * width * 4 : (row + 1) * width * 4] for row in range(height)
    )

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def shaded_texture(width: int, height: int, base: tuple[float, float, float], shade) -> bytes:
    """A texture that averages to exactly `base`.

    `shade(x, y)` returns a brightness multiplier of no particular scale; the
    whole field is then divided by its own mean. That normalisation is the point
    of this function: every texture here replaces a flat colour that the yard
    used to be painted with, and the prop has to read the same colour from across
    the yard. Only up close does the grain appear.
    """
    shades = [shade(x, y) for y in range(height) for x in range(width)]
    mean = sum(shades) / len(shades)

    pixels = bytearray()
    for value in shades:
        level = value / mean
        pixels += bytes(tuple(min(255, round(c * 255 * level)) for c in base) + (255,))
    return write_png(width, height, bytes(pixels))


def bark() -> bytes:
    """The tree's trunk: fissures running the length of it.

    They run down the texture's V because `box()` puts V along +Y on all four
    side faces of a box. The lid and the underside get horizontal fissures, and
    the canopy hides both.
    """

    def shade(x: int, y: int) -> float:
        column = 0.80 + 0.40 * noise(x * 7919)
        # Fissures wander a little rather than running dead straight.
        return column * (0.93 + 0.14 * noise(x * 131071 + (y // 3) * 31))

    return shaded_texture(64, 64, (0.33, 0.24, 0.16), shade)


def leaves() -> bytes:
    """The tree's canopy: clumps of foliage, not a slab of flat green.

    Three octaves of smooth value noise. Without an alpha channel the canopy is a
    solid box whatever we do, so the texture's whole job is to break the box up --
    which means the biggest clumps have to be big enough to see from across the
    yard, and the smallest small enough to survive standing under it.
    """
    size = 64

    def shade(x: int, y: int) -> float:
        clump = smooth_noise(x, y, size, size, 16, seed=1)
        leaf = smooth_noise(x, y, size, size, 8, seed=2)
        fleck = smooth_noise(x, y, size, size, 4, seed=3)
        return 0.62 + 0.76 * (0.60 * clump + 0.28 * leaf + 0.12 * fleck)

    return shaded_texture(size, size, (0.20, 0.42, 0.18), shade)


def cardboard() -> bytes:
    """A packing box: paper fibre over corrugation.

    The flutes show faintly through the liner as evenly spaced ridges. They run
    across the texture's U, so on the sides of a box they stand upright -- which
    is which way up corrugation goes when a box is meant to be stacked, and the
    yard has one crate stacked on another.
    """
    size = 64
    flutes = 8

    def shade(x: int, y: int) -> float:
        ridge = 0.97 + 0.06 * math.sin(x * 2.0 * math.pi * flutes / size)
        fibre = 0.10 * smooth_noise(x, y, size, size, 8, seed=5)
        speck = 0.06 * noise(x * 31 + y * 131071)
        return ridge * (0.92 + fibre + speck)

    return shaded_texture(size, size, (0.52, 0.38, 0.24), shade)


# --- Geometry -----------------------------------------------------------------

# The six face normals of a box, in glTF space.
FACE_NORMALS = [
    (1.0, 0.0, 0.0),
    (-1.0, 0.0, 0.0),
    (0.0, 1.0, 0.0),
    (0.0, -1.0, 0.0),
    (0.0, 0.0, 1.0),
    (0.0, 0.0, -1.0),
]


def cross(a, b):
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def normalize(v):
    length = sum(c * c for c in v) ** 0.5
    return tuple(c / length for c in v)


def box(size: tuple[float, float, float], tile: float | None = None):
    """A box centred on the origin with one flat normal per face.

    24 vertices, not 8: each corner is shared by three faces that disagree about
    the normal. Returns (positions, normals, uvs, indices).

    `tile` is the size in metres of one repeat of the texture. Without it each
    face gets the unit square, which stretches a texture across whatever the face
    happens to measure; with it the texel density is the same on every face of
    every box, which is what stops the table's legs looking like a different wood
    from its top.
    """
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    uvs: list[tuple[float, float]] = []
    indices: list[int] = []

    half = tuple(c * 0.5 for c in size)

    for normal in FACE_NORMALS:
        # Any vector not parallel to the normal seeds the face's tangent frame.
        # Build u and v so that cross(u, v) == normal, which is what puts the
        # corners below in counter-clockwise order as seen from outside.
        seed = (0.0, 0.0, 1.0) if abs(normal[1]) > 0.5 else (0.0, 1.0, 0.0)
        u = normalize(cross(seed, normal))
        v = cross(normal, u)

        # The face's centre, and the two half-extents that span it. Scaling by
        # `half` after building the unit frame keeps u and v axis-aligned, so
        # this picks out the box's own dimensions along each.
        center = tuple(normal[i] * half[i] for i in range(3))
        du = tuple(u[i] * half[i] for i in range(3))
        dv = tuple(v[i] * half[i] for i in range(3))

        if tile:
            # u and v are axis-aligned, so the face's extent along each is just
            # the box's own dimension on that axis.
            span_u = sum(abs(u[i]) * size[i] for i in range(3)) / tile
            span_v = sum(abs(v[i]) * size[i] for i in range(3)) / tile
        else:
            span_u = span_v = 1.0

        base = len(positions)
        # UVs put the texture's origin at the -u, +v corner: v grows downward in
        # a texture, and upward in the tangent frame.
        corners = (
            (-1, -1, (0.0, span_v)),
            (1, -1, (span_u, span_v)),
            (1, 1, (span_u, 0.0)),
            (-1, 1, (0.0, 0.0)),
        )
        for su, sv, uv in corners:
            positions.append(tuple(center[i] + su * du[i] + sv * dv[i] for i in range(3)))
            normals.append(normal)
            uvs.append(uv)

        indices += [base + o for o in (0, 1, 2, 0, 2, 3)]

    return positions, normals, uvs, indices


# --- GLB assembly -------------------------------------------------------------


class GlbBuilder:
    """Accumulates bufferViews into one binary chunk and emits the .glb."""

    def __init__(self) -> None:
        self.blob = bytearray()
        self.buffer_views: list[dict] = []
        self.accessors: list[dict] = []

    def _view(self, data: bytes, target: int | None = None) -> int:
        # Every accessor's byteOffset must be a multiple of its component size,
        # and glTF requires bufferViews to start on a 4-byte boundary anyway.
        while len(self.blob) % 4:
            self.blob.append(0)

        view = {"buffer": 0, "byteOffset": len(self.blob), "byteLength": len(data)}
        if target is not None:
            view["target"] = target
        self.blob += data
        self.buffer_views.append(view)
        return len(self.buffer_views) - 1

    def image_view(self, data: bytes) -> int:
        return self._view(data)

    def vec(self, values, kind: str) -> int:
        """A float accessor. `kind` is glTF's VEC2 / VEC3."""
        count = {"VEC2": 2, "VEC3": 3}[kind]
        flat = [c for value in values for c in value]
        # 34962 == ARRAY_BUFFER
        view = self._view(struct.pack(f"<{len(flat)}f", *flat), target=34962)

        self.accessors.append(
            {
                "bufferView": view,
                "componentType": FLOAT,
                "count": len(values),
                "type": kind,
                "min": [min(v[i] for v in values) for i in range(count)],
                "max": [max(v[i] for v in values) for i in range(count)],
            }
        )
        return len(self.accessors) - 1

    def indices(self, values: list[int]) -> int:
        assert max(values) < 65536
        # 34963 == ELEMENT_ARRAY_BUFFER
        view = self._view(struct.pack(f"<{len(values)}H", *values), target=34963)
        self.accessors.append(
            {
                "bufferView": view,
                "componentType": UNSIGNED_SHORT,
                "count": len(values),
                "type": "SCALAR",
            }
        )
        return len(self.accessors) - 1

    def box_mesh(self, name: str, size, material: int, tile: float | None = None) -> dict:
        positions, normals, uvs, indices = box(size, tile)
        return {
            "name": name,
            "primitives": [
                {
                    "attributes": {
                        "POSITION": self.vec(positions, "VEC3"),
                        "NORMAL": self.vec(normals, "VEC3"),
                        "TEXCOORD_0": self.vec(uvs, "VEC2"),
                    },
                    "indices": self.indices(indices),
                    "material": material,
                    "mode": TRIANGLES,
                }
            ],
        }

    def finish(self, gltf: dict) -> bytes:
        gltf["asset"] = {"version": "2.0", "generator": "grill-simulator tools/gen_models.py"}
        gltf["bufferViews"] = self.buffer_views
        gltf["accessors"] = self.accessors
        gltf["buffers"] = [{"byteLength": len(self.blob)}]

        # Both chunks are padded to 4 bytes: JSON with spaces, BIN with zeros.
        # The spec is explicit that the padding differs, because a JSON parser
        # would choke on a NUL and a buffer reader must not see stray 0x20.
        json_chunk = json.dumps(gltf, separators=(",", ":")).encode()
        json_chunk += b" " * (-len(json_chunk) % 4)
        bin_chunk = bytes(self.blob) + b"\x00" * (-len(self.blob) % 4)

        length = 12 + 8 + len(json_chunk) + 8 + len(bin_chunk)
        return (
            struct.pack("<4sII", b"glTF", 2, length)
            + struct.pack("<I4s", len(json_chunk), b"JSON")
            + json_chunk
            + struct.pack("<I4s", len(bin_chunk), b"BIN\x00")
            + bin_chunk
        )


def material(name: str, texture: int | None = None, color=(1.0, 1.0, 1.0),
             metallic: float = 0.0, roughness: float = 0.85) -> dict:
    pbr: dict = {
        "baseColorFactor": [*color, 1.0],
        "metallicFactor": metallic,
        "roughnessFactor": roughness,
    }
    if texture is not None:
        pbr["baseColorTexture"] = {"index": texture}
    return {"name": name, "pbrMetallicRoughness": pbr, "doubleSided": False}


def sampler() -> dict:
    return {
        "magFilter": LINEAR,
        "minFilter": LINEAR_MIPMAP_LINEAR,
        "wrapS": REPEAT,
        "wrapT": REPEAT,
    }


def part_nodes(root: str, parts) -> list[dict]:
    """A root transform node with one child per part.

    `parts` are (name, mesh, translation) or (name, mesh, translation, yaw), both
    in the game's space. Every model here hangs its parts off a root so that the
    loader flattens a real hierarchy, and so that each part yields its own
    collider rather than one loose box around the whole prop.
    """
    nodes: list[dict] = [{"name": root, "children": list(range(1, len(parts) + 1))}]
    for part in parts:
        name, mesh, translation = part[0], part[1], part[2]
        node = {"name": name, "mesh": mesh, "translation": list(project(*translation))}
        if len(part) > 3 and part[3]:
            node["rotation"] = yaw_quaternion(part[3])
        nodes.append(node)
    return nodes


def assemble(builder: GlbBuilder, nodes: list[dict], meshes: list[dict], materials: list[dict],
             images: list[int]) -> bytes:
    """One scene, one node tree, one texture per image. Node 0 is the root.

    A prop with no textures at all -- the flat-coloured tongs and meat -- omits
    the image, texture and sampler arrays entirely rather than emitting them
    empty: glTF requires each of those arrays to hold at least one element if it
    is present, so an empty `"images": []` is malformed.
    """
    gltf = {
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": nodes,
        "meshes": meshes,
        "materials": materials,
    }
    if images:
        gltf["textures"] = [{"sampler": 0, "source": i} for i in range(len(images))]
        gltf["images"] = [{"bufferView": view, "mimeType": "image/png"} for view in images]
        gltf["samplers"] = [sampler()]
    return builder.finish(gltf)


# --- The models ---------------------------------------------------------------


def build_tree() -> bytes:
    """One tree: a trunk and a canopy above head height.

    Authored at full size. The scene places it twice, at different scales and
    yaws, which is the whole reason it is one asset and not two.

    The canopy is yawed inside the model as well. That is not decoration: it is
    the only rotation in any of these files, and it is what exercises the
    loader's TRS-rotation path -- including the fact that a yaw survives the
    mirror through Z with its sign flipped.
    """
    builder = GlbBuilder()
    bark_image = builder.image_view(bark())
    leaves_image = builder.image_view(leaves())

    materials = [material("bark", texture=0, roughness=0.95),
                 material("leaves", texture=1, roughness=1.0)]
    BARK, LEAVES = range(2)

    meshes = [
        builder.box_mesh("trunk", (0.5, 3.0, 0.5), BARK, tile=1.0),
        builder.box_mesh("canopy", (3.0, 1.8, 3.0), LEAVES, tile=1.5),
    ]

    parts = [("trunk", 0, (0.0, 1.5, 0.0)), ("canopy", 1, (0.0, 3.7, 0.0), 12.0)]

    return assemble(builder, part_nodes("Tree", parts), meshes, materials,
                    [bark_image, leaves_image])


def build_crate() -> bytes:
    """A cardboard packing box, 80 cm on a side.

    The scene stacks two of them, the upper one scaled to 0.875 and knocked
    askew. Unlike the tree, that scale is exact: the two crates always were the
    same cube at 0.8 m and 0.7 m.
    """
    builder = GlbBuilder()
    cardboard_image = builder.image_view(cardboard())

    materials = [material("cardboard", texture=0, roughness=0.95)]
    meshes = [builder.box_mesh("box", (0.8, 0.8, 0.8), 0, tile=0.4)]

    # The origin sits on the ground under the crate, so stacking one on another
    # is a translation by the lower one's height.
    return assemble(builder, part_nodes("Crate", [("box", 0, (0.0, 0.4, 0.0))]), meshes,
                    materials, [cardboard_image])


# --- The props ----------------------------------------------------------------
#
# Unlike the yard's furniture, these are loose objects the player carries. They
# are flat-coloured -- no textures -- because they are small and mostly seen in
# the hand, and because it exercises the loader's texture-less material path that
# the grill's flat lid and legs only share a file with. Each is one node, so it
# still yields a bounding box, but the game gives none of them a collider: a
# dropped steak is something to walk through, not to trip over.
#
# Every prop's origin sits at the centre of its underside, so the game sets one
# down with a plain translation to whatever surface height it is resting on --
# the same convention the crate and cooler already use.


def build_tongs() -> bytes:
    """A pair of spring tongs: two steel arms splaying from a hinge.

    They lie flat along +X with the hinge at the origin, so the game can lay them
    across the grill's shelf with a yaw and a translation.
    """
    builder = GlbBuilder()
    materials = [material("steel", color=(0.72, 0.74, 0.78), metallic=1.0, roughness=0.3)]
    STEEL = 0

    meshes = [
        builder.box_mesh("arm", (0.34, 0.02, 0.03), STEEL),
        builder.box_mesh("hinge", (0.05, 0.03, 0.08), STEEL),
    ]

    # The arms' boxes are centred, so a translation of 0.19 in X puts each one's
    # near end just past the hinge; the yaw splays them apart along Z into the
    # open V of a pair of tongs.
    parts = [
        ("hinge", 1, (0.0, 0.015, 0.0)),
        ("arm_n", 0, (0.19, 0.01, 0.04), 8.0),
        ("arm_s", 0, (0.19, 0.01, -0.04), -8.0),
    ]

    return assemble(builder, part_nodes("Tongs", parts), meshes, materials, [])


def build_patty() -> bytes:
    """A raw burger patty: a low, dark disc stood in for by a flat box."""
    builder = GlbBuilder()
    materials = [material("patty", color=(0.36, 0.20, 0.14), roughness=0.9)]
    meshes = [builder.box_mesh("patty", (0.20, 0.035, 0.20), 0)]
    return assemble(builder, part_nodes("Patty", [("patty", 0, (0.0, 0.0175, 0.0))]),
                    meshes, materials, [])


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    models = (("tree.glb", build_tree()),
              ("crate.glb", build_crate()),
              ("tongs.glb", build_tongs()),
              ("patty.glb", build_patty()))
    for name, data in models:
        path = ASSETS / name
        path.write_bytes(data)
        print(f"wrote {path.relative_to(ASSETS.parent.parent)} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
