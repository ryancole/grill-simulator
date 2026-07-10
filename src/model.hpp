#pragma once

#include "collision.hpp"
#include "image.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct Vertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT2 uv;
};

// The skinning half of a vertex, held in its own array rather than folded into
// Vertex above. Nothing reads it yet -- there is no animation and no skinned
// asset -- but a glTF that carries JOINTS_0 and WEIGHTS_0 parses into it, and
// when rigging lands it becomes a second vertex buffer bound to slot 1. Keeping
// it out of Vertex means the yard, which will never be skinned, pays nothing for
// it: no wasted bytes per vertex, no second input layout for the static path.
struct SkinVertex {
    std::uint16_t joints[4];
    DirectX::XMFLOAT4 weights;
};

// glTF materials describe a full metallic-roughness BRDF. This renderer has a
// hemisphere ambient, one directional light and no specular term at all, so only
// the base colour survives the trip: the factor, and the texture it modulates.
// Everything else in the material -- metallic, roughness, normal maps, emissive
// -- is read past and dropped.
struct Material {
    DirectX::XMFLOAT3 base_color{1.0f, 1.0f, 1.0f};
    // Index into Model::images, or -1 when the material is a flat colour.
    int base_color_image = -1;
};

// One draw. `transform` places the primitive's vertices into model space, having
// already flattened the glTF node hierarchy above it.
struct Primitive {
    DirectX::XMFLOAT4X4 transform;
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    // Index into Model::materials, or -1 for the default white material.
    int material = -1;
    // Index into Model::skins, or -1. Unused until rigging lands.
    int skin = -1;
    // The vertices' own bounds, before `transform`. Taken from the POSITION
    // accessor's min/max, which glTF requires every asset to supply, so no
    // vertex is ever touched to compute it. The caller composes this with
    // `transform` and its own instance transform to get a world-space box.
    Aabb bounds{};
    // False for decorative geometry: drawn, never collided with. See
    // kDecorativeSuffix in model.cpp for how an asset says so, and why it has
    // to be able to.
    bool collides = true;
};

// Unused until rigging lands. `joints` indexes Model::nodes; `inverse_bind`
// takes a vertex from model space into each joint's bind-pose space.
struct Skin {
    std::vector<std::uint32_t> joints;
    std::vector<DirectX::XMFLOAT4X4> inverse_bind;
};

struct Node {
    std::string name;
    // Flattened: node space all the way up to model space.
    DirectX::XMFLOAT4X4 to_model;
    int parent = -1;
};

// A mesh hierarchy with its materials and textures, on the CPU. The renderer
// uploads one of these into vertex, index and texture resources; the scene
// places instances of it.
//
// Indices are always 32 bit, whatever width the file used. A shared unit cube
// wastes 72 bytes by it, which buys one index format, one buffer view and no
// branch anywhere in the renderer.
struct Model {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    // Empty unless some primitive is skinned; otherwise parallel to `vertices`.
    std::vector<SkinVertex> skin_vertices;
    std::vector<Primitive> primitives;
    std::vector<Material> materials;
    std::vector<Image> images;
    std::vector<Skin> skins;
    std::vector<Node> nodes;
};

// Reads a .glb or .gltf.
//
// glTF is right-handed with -Z forward; this game is left-handed with +Z north.
// The two differ by a mirror through Z, and it is undone here -- once, on the
// way in -- so that nothing downstream has to know the file's handedness. See
// model.cpp for what that costs: a sign flip on positions and normals, a
// sandwich around every matrix, and a reversed winding on every triangle.
//
// COM must already be initialised on the calling thread, because the textures go
// through WIC. Throws on a malformed or unreadable asset.
Model LoadGltfModel(const std::filesystem::path& path);

// A cube of side 1 centred on the origin, built in code rather than read from a
// file. The yard's ground, fence, table and trees are all this cube under a
// different transform, and none of them is worth an artist's time.
//
// It has one flat normal per face -- so 24 vertices rather than 8, because the
// corners are shared by three normals -- and no material, which the renderer
// reads as plain white for the instance's tint to colour.
Model MakeUnitCubeModel();
