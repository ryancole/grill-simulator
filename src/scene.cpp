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
// Only the ground, the patio and the fence are still painted here. Every other
// prop carries its colours in the materials of its own .glb, and the instance
// that places it passes white.
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};
constexpr XMFLOAT3 kGrass{0.24f, 0.36f, 0.18f};
constexpr XMFLOAT3 kConcrete{0.55f, 0.54f, 0.51f};
constexpr XMFLOAT3 kFenceWood{0.45f, 0.32f, 0.21f};

} // namespace

Scene::Scene() {
    cube_ = AddModel(MakeUnitCubeModel());
    const std::uint32_t grill = LoadModel("grill.glb");
    const std::uint32_t tree = LoadModel("tree.glb");
    const std::uint32_t table = LoadModel("table.glb");
    const std::uint32_t crate = LoadModel("crate.glb");
    const std::uint32_t cooler = LoadModel("cooler.glb");

    // The loose props are loaded so the renderer uploads them, but the scene
    // places no instances of them: Props sets out the starting handful and owns
    // every one thereafter.
    prop_models_.tongs = LoadModel("tongs.glb");
    prop_models_.patty = LoadModel("patty.glb");
    prop_models_.steak = LoadModel("steak.glb");

    // The yard. +X is east, +Z is north, and the player spawns at the south end
    // looking at the grill.
    AddBox({0.0f, -0.15f, 0.0f}, {60.0f, 0.3f, 60.0f}, 0.0f, kGrass, 2.0f);
    AddBox({0.0f, 0.03f, 2.5f}, {16.0f, 0.06f, 11.0f}, 0.0f, kConcrete, 1.0f);

    // Fence. Long enough to overlap at the corners.
    AddBox({0.0f, 1.0f, 12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood);
    AddBox({0.0f, 1.0f, -12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood);
    AddBox({12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood);
    AddBox({-12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood);

    // Each model's origin sits on the ground beneath it, so a prop that is not
    // scaled or turned goes where it belongs with a plain translation, and its
    // parts come along as the nodes of one asset.
    AddInstance(grill, XMMatrixTranslation(0.0f, 0.0f, 5.0f), kWhite);
    AddInstance(table, XMMatrixTranslation(-4.5f, 0.0f, 1.5f), kWhite);
    AddInstance(cooler, XMMatrixTranslation(3.6f, 0.0f, 6.5f), kWhite);

    // Two crates, the upper one knocked askew. The smaller is the same box at
    // 0.875 -- an exact scale, unlike the trees', because these two always were
    // the same cube at 0.8 m and 0.7 m. It stands on the lower one, so it is
    // lifted by the lower one's height rather than by an arithmetic centre.
    AddInstance(crate, XMMatrixTranslation(5.8f, 0.0f, -2.0f), kWhite);
    AddInstance(crate,
                XMMatrixScaling(0.875f, 0.875f, 0.875f) *
                    XMMatrixRotationY(XMConvertToRadians(22.0f)) *
                    XMMatrixTranslation(5.8f, 0.8f, -2.0f),
                kWhite);

    // Two of the same tree, told apart by a scale and a turn -- which is the
    // whole reason it is one asset rather than two. The canopy is already yawed
    // 12 degrees inside the model, so neither tree's leaves line up with its own
    // trunk, and the two do not line up with each other.
    //
    // Each canopy starts above head height, so the collider pass skips it and the
    // player can walk underneath. Turning a tree does turn its trunk, whose
    // collider is the world bound of a box no longer axis aligned -- a few
    // centimetres wider than the trunk really is. Nobody has ever noticed a tree
    // being slightly too solid.
    AddInstance(tree,
                XMMatrixRotationY(XMConvertToRadians(25.0f)) *
                    XMMatrixTranslation(-8.0f, 0.0f, 7.5f),
                kWhite);
    AddInstance(tree,
                XMMatrixScaling(0.87f, 0.87f, 0.87f) *
                    XMMatrixRotationY(XMConvertToRadians(-15.0f)) *
                    XMMatrixTranslation(8.5f, 0.0f, -6.0f),
                kWhite);
}

std::uint32_t Scene::LoadModel(const char* file) {
    return AddModel(LoadGltfModel(ExecutableDirectory() / "assets" / "models" / file));
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
    // POSITION accessor. No vertex is ever looked at. Decorative parts -- the
    // cooler's lid, which only laps over a body that is already the whole cooler
    // -- contribute nothing, or the player would stand on them.
    for (const Primitive& primitive : models_[model].primitives) {
        if (!primitive.collides) {
            continue;
        }
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
