#include "scene.hpp"

#include "dx_common.hpp"

#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace {

// Every colour is written the way it should look on screen. Nothing here is
// converted to linear light and the back buffer is not sRGB, so the shader's
// arithmetic happens in whatever space these are -- fine for flat colours.
//
// The grill's colours are no longer among them: they live in the materials of
// assets/models/grill.glb, and the instance that places it passes white.
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};
constexpr XMFLOAT3 kGrass{0.24f, 0.36f, 0.18f};
constexpr XMFLOAT3 kConcrete{0.55f, 0.54f, 0.51f};
constexpr XMFLOAT3 kFenceWood{0.45f, 0.32f, 0.21f};
constexpr XMFLOAT3 kTableWood{0.60f, 0.44f, 0.28f};
constexpr XMFLOAT3 kCoolerBlue{0.16f, 0.44f, 0.62f};
constexpr XMFLOAT3 kCrate{0.52f, 0.38f, 0.24f};
constexpr XMFLOAT3 kTrunk{0.33f, 0.24f, 0.16f};
constexpr XMFLOAT3 kLeaves{0.20f, 0.42f, 0.18f};

// The world-space bound of a transformed box. For a rotated one that is a loose
// fit, which for a tree canopy nobody can reach is not worth a separate
// oriented-box test.
Aabb TransformBounds(const Aabb& bounds, FXMMATRIX transform) {
    XMVECTOR minimum = XMVectorReplicate(FLT_MAX);
    XMVECTOR maximum = XMVectorReplicate(-FLT_MAX);

    for (const float x : {bounds.min.x, bounds.max.x}) {
        for (const float y : {bounds.min.y, bounds.max.y}) {
            for (const float z : {bounds.min.z, bounds.max.z}) {
                const XMVECTOR corner = XMVector3Transform(XMVectorSet(x, y, z, 1.0f), transform);
                minimum = XMVectorMin(minimum, corner);
                maximum = XMVectorMax(maximum, corner);
            }
        }
    }

    Aabb result{};
    XMStoreFloat3(&result.min, minimum);
    XMStoreFloat3(&result.max, maximum);
    return result;
}

} // namespace

Scene::Scene() {
    cube_ = AddModel(MakeUnitCubeModel());
    const std::uint32_t grill =
        AddModel(LoadGltfModel(ExecutableDirectory() / "assets" / "models" / "grill.glb"));

    // The yard. +X is east, +Z is north, and the player spawns at the south end
    // looking at the grill.
    AddBox({0.0f, -0.15f, 0.0f}, {60.0f, 0.3f, 60.0f}, 0.0f, kGrass, 2.0f);
    AddBox({0.0f, 0.03f, 2.5f}, {16.0f, 0.06f, 11.0f}, 0.0f, kConcrete, 1.0f);

    // Fence. Long enough to overlap at the corners.
    AddBox({0.0f, 1.0f, 12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood);
    AddBox({0.0f, 1.0f, -12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood);
    AddBox({12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood);
    AddBox({-12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood);

    // The grill. Its own origin sits on the ground between its legs, so it goes
    // where it belongs with a plain translation, and its legs, body, lid and
    // shelf come along as the nodes of one asset.
    AddInstance(grill, XMMatrixTranslation(0.0f, 0.0f, 5.0f), kWhite);

    // Picnic table with two benches.
    AddBox({-4.5f, 0.75f, 1.5f}, {2.6f, 0.1f, 1.2f}, 0.0f, kTableWood);
    for (const float x : {-5.65f, -3.35f}) {
        for (const float z : {1.05f, 1.95f}) {
            AddBox({x, 0.35f, z}, {0.12f, 0.7f, 0.12f}, 0.0f, kTableWood);
        }
    }
    AddBox({-4.5f, 0.45f, 0.55f}, {2.6f, 0.08f, 0.4f}, 0.0f, kTableWood);
    AddBox({-4.5f, 0.45f, 2.45f}, {2.6f, 0.08f, 0.4f}, 0.0f, kTableWood);

    AddBox({3.6f, 0.3f, 6.5f}, {0.9f, 0.6f, 0.6f}, 0.0f, kCoolerBlue);

    // Two crates, the upper one knocked askew.
    AddBox({5.8f, 0.4f, -2.0f}, {0.8f, 0.8f, 0.8f}, 0.0f, kCrate);
    AddBox({5.8f, 1.15f, -2.0f}, {0.7f, 0.7f, 0.7f}, 22.0f, kCrate);

    // Trees. Each canopy starts above head height, so the collider pass skips it
    // and the player can walk underneath.
    AddBox({-8.0f, 1.5f, 7.5f}, {0.5f, 3.0f, 0.5f}, 0.0f, kTrunk);
    AddBox({-8.0f, 3.7f, 7.5f}, {3.0f, 1.8f, 3.0f}, 25.0f, kLeaves);
    AddBox({8.5f, 1.3f, -6.0f}, {0.45f, 2.6f, 0.45f}, 0.0f, kTrunk);
    AddBox({8.5f, 3.2f, -6.0f}, {2.6f, 1.6f, 2.6f}, -15.0f, kLeaves);
}

std::uint32_t Scene::AddModel(Model model) {
    models_.push_back(std::move(model));
    return static_cast<std::uint32_t>(models_.size() - 1);
}

void Scene::AddInstance(std::uint32_t model, FXMMATRIX transform, XMFLOAT3 tint, float checker) {
    MeshInstance instance{};
    instance.model = model;
    XMStoreFloat4x4(&instance.transform, transform);
    instance.tint = tint;
    instance.checker = checker;
    instances_.push_back(instance);

    // One collider per primitive, from the bounds glTF already stored on the
    // POSITION accessor. No vertex is ever looked at.
    for (const Primitive& primitive : models_[model].primitives) {
        const XMMATRIX to_world = XMLoadFloat4x4(&primitive.transform) * transform;
        colliders_.push_back(TransformBounds(primitive.bounds, to_world));
    }
}

void Scene::AddBox(XMFLOAT3 center, XMFLOAT3 size, float yaw_degrees, XMFLOAT3 color,
                   float checker) {
    AddInstance(cube_,
                XMMatrixScaling(size.x, size.y, size.z) *
                    XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                    XMMatrixTranslation(center.x, center.y, center.z),
                color, checker);
}
