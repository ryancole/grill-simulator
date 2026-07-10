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

Only the standard library is used: zlib writes the PNG, struct writes the GLB.

Coordinates here are glTF's, not the game's
-------------------------------------------
glTF is right-handed with +Y up and -Z forward. The game is left-handed with +Z
north (it draws through XMMatrixPerspectiveFovLH). The two differ by a mirror
through Z, and `Model::Load` undoes it on the way in. So a point the game calls
(x, y, z) is written here as (x, y, -z), which `project()` does for us.

Winding follows the glTF spec: counter-clockwise when seen from outside. That is
the same rule `Scene::BuildUnitCube` states as "cross(v1 - v0, v2 - v0) is the
outward normal", because in a right-handed frame the two say the same thing. The
loader reverses each triangle to pay for the mirror, and the invariant survives.
"""

from __future__ import annotations

import json
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


# --- PNG ----------------------------------------------------------------------


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


def brushed_charcoal(width: int = 128, height: int = 64) -> bytes:
    """A dark, faintly streaked surface for the grill's body.

    The streaks run along X so that they wrap around the barrel horizontally.
    Everything is derived from a tiny integer hash rather than `random`, so the
    committed .glb is byte-for-byte reproducible on any machine.
    """

    def noise(seed: int) -> float:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        seed ^= seed >> 15
        seed = (seed * 2654435761) & 0x7FFFFFFF
        return (seed & 0xFFFF) / 65535.0

    # The old kCharcoal from scene.cpp. Both noise terms average to 1.0, so the
    # texture averages to exactly this and the grill body reads the colour it
    # always did from across the yard -- only up close does the grain appear.
    base = (0.13, 0.13, 0.14)

    pixels = bytearray()
    for y in range(height):
        # One brightness per row is what makes the grain read as brushed metal
        # rather than as sandpaper. The per-pixel term only breaks up the bands.
        row = 0.86 + 0.28 * noise(y * 7919)
        for x in range(width):
            grain = 0.94 + 0.12 * noise(y * 131071 + x * 31)
            shade = row * grain
            pixels += bytes(
                tuple(min(255, round(channel * 255 * shade)) for channel in base) + (255,)
            )
    return write_png(width, height, bytes(pixels))


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


def box(size: tuple[float, float, float]):
    """A box centred on the origin with one flat normal per face.

    24 vertices, not 8: each corner is shared by three faces that disagree about
    the normal. Returns (positions, normals, uvs, indices).
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

        base = len(positions)
        # UVs put the texture's origin at the -u, +v corner: v grows downward in
        # a texture, and upward in the tangent frame.
        for su, sv, uv in ((-1, -1, (0.0, 1.0)), (1, -1, (1.0, 1.0)),
                           (1, 1, (1.0, 0.0)), (-1, 1, (0.0, 0.0))):
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


def add_box_mesh(builder: GlbBuilder, name: str, size, material: int) -> dict:
    positions, normals, uvs, indices = box(size)
    return {
        "name": name,
        "primitives": [
            {
                "attributes": {
                    "POSITION": builder.vec(positions, "VEC3"),
                    "NORMAL": builder.vec(normals, "VEC3"),
                    "TEXCOORD_0": builder.vec(uvs, "VEC2"),
                },
                "indices": builder.indices(indices),
                "material": material,
                "mode": TRIANGLES,
            }
        ],
    }


def build_grill() -> bytes:
    """The kettle grill: four legs, a body, a lid and a side shelf.

    Every part is its own node, which is what gives the loader one bounding box
    per part rather than one around the whole grill. The player can still walk
    under the side shelf.
    """
    builder = GlbBuilder()

    image_view = builder.image_view(brushed_charcoal())

    # The body is the only textured part. The rest carry a flat base colour, so
    # the loader has to handle a material with no texture at all -- which is the
    # case that a 1x1 white default texture exists to serve.
    materials = [
        {
            "name": "body",
            "pbrMetallicRoughness": {
                "baseColorTexture": {"index": 0},
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.85,
            },
            "doubleSided": False,
        },
        {
            "name": "lid",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.62, 0.11, 0.09, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.4,
            },
            "doubleSided": False,
        },
        {
            "name": "leg",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.13, 0.13, 0.14, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 0.9,
            },
            "doubleSided": False,
        },
        {
            "name": "shelf",
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.62, 0.64, 0.67, 1.0],
                "metallicFactor": 1.0,
                "roughnessFactor": 0.35,
            },
            "doubleSided": False,
        },
    ]
    BODY, LID, LEG, SHELF = range(4)

    meshes = [
        add_box_mesh(builder, "body", (1.6, 0.7, 0.9), BODY),
        add_box_mesh(builder, "lid", (1.7, 0.24, 1.0), LID),
        add_box_mesh(builder, "leg", (0.08, 0.25, 0.08), LEG),
        add_box_mesh(builder, "shelf", (0.7, 0.05, 0.7), SHELF),
    ]

    # The grill's own origin sits on the ground between its legs, so the scene
    # places it with a plain translation. Positions below are the game's, in
    # metres, and go through project() on the way into the file.
    parts = [
        ("body", 0, (0.0, 0.6, 0.0)),
        ("lid", 1, (0.0, 1.05, 0.0)),
        ("shelf", 3, (1.15, 0.75, 0.0)),
    ]
    for x in (-0.65, 0.65):
        for z in (-0.35, 0.35):
            parts.append((f"leg_{'e' if x > 0 else 'w'}{'n' if z > 0 else 's'}", 2, (x, 0.125, z)))

    # Node 0 is the root: the four legs and the three panels hang off it, so the
    # whole grill moves as one and the loader has a hierarchy to flatten.
    nodes: list[dict] = [{"name": "Grill", "children": list(range(1, len(parts) + 1))}]
    for name, mesh, translation in parts:
        nodes.append({"name": name, "mesh": mesh, "translation": list(project(*translation))})

    return builder.finish(
        {
            "scene": 0,
            "scenes": [{"nodes": [0]}],
            "nodes": nodes,
            "meshes": meshes,
            "materials": materials,
            "textures": [{"sampler": 0, "source": 0}],
            "images": [{"bufferView": image_view, "mimeType": "image/png"}],
            "samplers": [
                {
                    "magFilter": LINEAR,
                    "minFilter": LINEAR_MIPMAP_LINEAR,
                    "wrapS": REPEAT,
                    "wrapT": REPEAT,
                }
            ],
        }
    )


def main() -> None:
    ASSETS.mkdir(parents=True, exist_ok=True)
    for name, data in (("grill.glb", build_grill()),):
        path = ASSETS / name
        path.write_bytes(data)
        print(f"wrote {path.relative_to(ASSETS.parent.parent)} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
