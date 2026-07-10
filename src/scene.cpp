#include "scene.hpp"

#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace {

// Every colour is written the way it should look on screen. Nothing here is
// converted to linear light and the back buffer is not sRGB, so the shader's
// arithmetic happens in whatever space these are -- fine for flat colours.
constexpr XMFLOAT3 kGrass{0.24f, 0.36f, 0.18f};
constexpr XMFLOAT3 kConcrete{0.55f, 0.54f, 0.51f};
constexpr XMFLOAT3 kFenceWood{0.45f, 0.32f, 0.21f};
constexpr XMFLOAT3 kCharcoal{0.13f, 0.13f, 0.14f};
constexpr XMFLOAT3 kGrillRed{0.62f, 0.11f, 0.09f};
constexpr XMFLOAT3 kSteel{0.62f, 0.64f, 0.67f};
constexpr XMFLOAT3 kTableWood{0.60f, 0.44f, 0.28f};
constexpr XMFLOAT3 kCoolerBlue{0.16f, 0.44f, 0.62f};
constexpr XMFLOAT3 kCrate{0.52f, 0.38f, 0.24f};
constexpr XMFLOAT3 kTrunk{0.33f, 0.24f, 0.16f};
constexpr XMFLOAT3 kLeaves{0.20f, 0.42f, 0.18f};

} // namespace

Scene::Scene() {
    BuildUnitCube();

    // The yard. +X is east, +Z is north, and the player spawns at the south end
    // looking at the grill.
    AddBox({0.0f, -0.15f, 0.0f}, {60.0f, 0.3f, 60.0f}, 0.0f, kGrass, 2.0f);
    AddBox({0.0f, 0.03f, 2.5f}, {16.0f, 0.06f, 11.0f}, 0.0f, kConcrete, 1.0f);

    // Fence. Long enough to overlap at the corners.
    AddBox({0.0f, 1.0f, 12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood);
    AddBox({0.0f, 1.0f, -12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood);
    AddBox({12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood);
    AddBox({-12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood);

    // The grill itself: four legs, a body, a lid and a side shelf.
    for (const float x : {-0.65f, 0.65f}) {
        for (const float z : {4.65f, 5.35f}) {
            AddBox({x, 0.125f, z}, {0.08f, 0.25f, 0.08f}, 0.0f, kCharcoal);
        }
    }
    AddBox({0.0f, 0.6f, 5.0f}, {1.6f, 0.7f, 0.9f}, 0.0f, kCharcoal);
    AddBox({0.0f, 1.05f, 5.0f}, {1.7f, 0.24f, 1.0f}, 0.0f, kGrillRed);
    AddBox({1.15f, 0.75f, 5.0f}, {0.7f, 0.05f, 0.7f}, 0.0f, kSteel);

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

// A cube of side 1 centred on the origin, with one flat normal per face -- so 24
// vertices rather than 8, because the corners are shared by three normals.
void Scene::BuildUnitCube() {
    constexpr XMFLOAT3 kFaceNormals[] = {{1.0f, 0.0f, 0.0f},  {-1.0f, 0.0f, 0.0f},
                                         {0.0f, 1.0f, 0.0f},  {0.0f, -1.0f, 0.0f},
                                         {0.0f, 0.0f, 1.0f},  {0.0f, 0.0f, -1.0f}};

    for (const XMFLOAT3& face_normal : kFaceNormals) {
        const XMVECTOR normal = XMLoadFloat3(&face_normal);

        // Any vector that is not parallel to the normal seeds the face's tangent
        // frame. Build u and v so that cross(u, v) == normal.
        const XMVECTOR seed = std::abs(face_normal.y) > 0.5f ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                                                             : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        const XMVECTOR u = XMVector3Normalize(XMVector3Cross(seed, normal));
        const XMVECTOR v = XMVector3Cross(normal, u);

        // Direct3D's default rasteriser treats a triangle as front facing when
        // cross(v1 - v0, v2 - v0) points at the camera. Emitting the corners in
        // this order makes that cross product the outward normal, so every face
        // of the cube is front facing from outside and back-face culling works.
        const XMVECTOR center = XMVectorScale(normal, 0.5f);
        const XMVECTOR half_u = XMVectorScale(u, 0.5f);
        const XMVECTOR half_v = XMVectorScale(v, 0.5f);
        const XMVECTOR corners[] = {
            XMVectorSubtract(XMVectorSubtract(center, half_u), half_v),
            XMVectorSubtract(XMVectorAdd(center, half_u), half_v),
            XMVectorAdd(XMVectorAdd(center, half_u), half_v),
            XMVectorAdd(XMVectorSubtract(center, half_u), half_v),
        };

        const auto base = static_cast<std::uint16_t>(vertices_.size());
        for (const XMVECTOR& corner : corners) {
            Vertex vertex{};
            XMStoreFloat3(&vertex.position, corner);
            vertex.normal = face_normal;
            vertices_.push_back(vertex);
        }

        for (const int offset : {0, 1, 2, 0, 2, 3}) {
            indices_.push_back(static_cast<std::uint16_t>(base + offset));
        }
    }
}

void Scene::AddBox(XMFLOAT3 center, XMFLOAT3 size, float yaw_degrees, XMFLOAT3 color,
                   float checker) {
    const XMMATRIX transform = XMMatrixScaling(size.x, size.y, size.z) *
                               XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                               XMMatrixTranslation(center.x, center.y, center.z);

    Prop prop{};
    XMStoreFloat4x4(&prop.transform, transform);
    prop.color = color;
    prop.checker = checker;
    props_.push_back(prop);

    // The collider is the world-space bound of the transformed cube. For a
    // rotated box that is a loose fit, which for a tree canopy nobody can reach
    // is not worth a separate oriented-box test.
    XMVECTOR minimum = XMVectorReplicate(FLT_MAX);
    XMVECTOR maximum = XMVectorReplicate(-FLT_MAX);
    for (const float x : {-0.5f, 0.5f}) {
        for (const float y : {-0.5f, 0.5f}) {
            for (const float z : {-0.5f, 0.5f}) {
                const XMVECTOR corner =
                    XMVector3Transform(XMVectorSet(x, y, z, 1.0f), transform);
                minimum = XMVectorMin(minimum, corner);
                maximum = XMVectorMax(maximum, corner);
            }
        }
    }

    Aabb bounds{};
    XMStoreFloat3(&bounds.min, minimum);
    XMStoreFloat3(&bounds.max, maximum);
    colliders_.push_back(bounds);
}
