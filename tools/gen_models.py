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


def brushed_charcoal() -> bytes:
    """The grill's body: dark, faintly streaked along X so it wraps the barrel."""

    def shade(x: int, y: int) -> float:
        # One brightness per row is what makes the grain read as brushed metal
        # rather than as sandpaper. The per-pixel term only breaks up the bands.
        row = 0.86 + 0.28 * noise(y * 7919)
        return row * (0.94 + 0.12 * noise(y * 131071 + x * 31))

    return shaded_texture(128, 64, (0.13, 0.13, 0.14), shade)


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


def planks(bands: int = 4) -> bytes:
    """The picnic table. Boards run along the texture's U, seams along its V.

    `box()` is asked to tile this every 0.8 m, so four bands across the tile put
    a seam every 20 cm -- which is about what a picnic table's boards are.
    """
    height = 64
    band_height = height // bands

    def shade(x: int, y: int) -> float:
        board = 0.88 + 0.24 * noise((y // band_height) * 7919)
        # The dark line where two boards meet.
        seam = 0.55 if y % band_height == 0 else 1.0
        # Grain runs the length of the board, so it varies fast across V and
        # slowly along U.
        grain = 0.95 + 0.10 * noise(y * 31 + (x // 16) * 131071)
        return board * seam * grain

    return shaded_texture(64, height, (0.60, 0.44, 0.28), shade)


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


def assemble(builder: GlbBuilder, nodes: list[dict], meshes: list[dict], materials: list[dict],
             images: list[int]) -> bytes:
    """One scene, one node tree, one texture per image. Node 0 is the root."""
    return builder.finish(
        {
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": nodes,
            "meshes": meshes,
            "materials": materials,
            "textures": [{"sampler": 0, "source": i} for i in range(len(images))],
            "images": [{"bufferView": view, "mimeType": "image/png"} for view in images],
            "samplers": [sampler()],
        }
    )


# --- The models ---------------------------------------------------------------


def build_grill() -> bytes:
    """The kettle grill: four legs, a body, a lid and a side shelf.

    Every part is its own node, which is what gives the loader one bounding box
    per part rather than one around the whole grill.
    """
    builder = GlbBuilder()
    charcoal_image = builder.image_view(brushed_charcoal())

    # The body is the only textured part. The rest carry a flat base colour, so
    # the loader has to handle a material with no texture at all -- which is the
    # case that a 1x1 white default texture exists to serve.
    materials = [
        material("body", texture=0),
        material("lid", color=(0.62, 0.11, 0.09), roughness=0.4),
        material("leg", color=(0.13, 0.13, 0.14), roughness=0.9),
        material("shelf", color=(0.62, 0.64, 0.67), metallic=1.0, roughness=0.35),
    ]
    BODY, LID, LEG, SHELF = range(4)

    meshes = [
        builder.box_mesh("body", (1.6, 0.7, 0.9), BODY),
        builder.box_mesh("lid", (1.7, 0.24, 1.0), LID),
        builder.box_mesh("leg", (0.08, 0.25, 0.08), LEG),
        builder.box_mesh("shelf", (0.7, 0.05, 0.7), SHELF),
    ]

    # The grill's own origin sits on the ground between its legs, so the scene
    # places it with a plain translation.
    parts = [("body", 0, (0.0, 0.6, 0.0)), ("lid", 1, (0.0, 1.05, 0.0)),
             ("shelf", 3, (1.15, 0.75, 0.0))]
    for x in (-0.65, 0.65):
        for z in (-0.35, 0.35):
            parts.append((f"leg_{'e' if x > 0 else 'w'}{'n' if z > 0 else 's'}", 2, (x, 0.125, z)))

    nodes: list[dict] = [{"name": "Grill", "children": list(range(1, len(parts) + 1))}]
    for name, mesh, translation in parts:
        nodes.append({"name": name, "mesh": mesh, "translation": list(project(*translation))})

    return assemble(builder, nodes, meshes, materials, [charcoal_image])


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

    nodes = [
        {"name": "Tree", "children": [1, 2]},
        {"name": "trunk", "mesh": 0, "translation": list(project(0.0, 1.5, 0.0))},
        {
            "name": "canopy",
            "mesh": 1,
            "translation": list(project(0.0, 3.7, 0.0)),
            "rotation": yaw_quaternion(12.0),
        },
    ]

    return assemble(builder, nodes, meshes, materials, [bark_image, leaves_image])


def build_table() -> bytes:
    """A picnic table: a top, four legs and two benches, all the same wood."""
    builder = GlbBuilder()
    planks_image = builder.image_view(planks())

    materials = [material("wood", texture=0, roughness=0.8)]
    WOOD = 0

    # 0.8 m of texture per tile, so the boards read at the same size on the top,
    # the benches and the legs.
    meshes = [
        builder.box_mesh("top", (2.6, 0.1, 1.2), WOOD, tile=0.8),
        builder.box_mesh("leg", (0.12, 0.7, 0.12), WOOD, tile=0.8),
        builder.box_mesh("bench", (2.6, 0.08, 0.4), WOOD, tile=0.8),
    ]

    # The table's origin sits on the ground at the centre of its top.
    parts = [("top", 0, (0.0, 0.75, 0.0)),
             ("bench_s", 2, (0.0, 0.45, -0.95)),
             ("bench_n", 2, (0.0, 0.45, 0.95))]
    for x in (-1.15, 1.15):
        for z in (-0.45, 0.45):
            parts.append((f"leg_{'e' if x > 0 else 'w'}{'n' if z > 0 else 's'}", 1, (x, 0.35, z)))

    nodes: list[dict] = [{"name": "Table", "children": list(range(1, len(parts) + 1))}]
    for name, mesh, translation in parts:
        nodes.append({"name": name, "mesh": mesh, "translation": list(project(*translation))})

    return assemble(builder, nodes, meshes, materials, [planks_image])


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    models = (("grill.glb", build_grill()), ("tree.glb", build_tree()),
              ("table.glb", build_table()))
    for name, data in models:
        path = ASSETS / name
        path.write_bytes(data)
        print(f"wrote {path.relative_to(ASSETS.parent.parent)} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
