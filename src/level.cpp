#include "level.hpp"

#include <utility>

using namespace DirectX;

namespace {

// The colours for the parts that are still bare boxes. Every other prop carries
// its colours in the materials of its own .glb and is placed white. Written the
// way they should look on screen -- nothing here is converted to linear light and
// the back buffer is not sRGB, so the shader's arithmetic happens in whatever
// space these are, which is fine for flat colours.
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};
constexpr XMFLOAT3 kGrass{0.24f, 0.36f, 0.18f};
constexpr XMFLOAT3 kConcrete{0.55f, 0.54f, 0.51f};
constexpr XMFLOAT3 kFenceWood{0.45f, 0.32f, 0.21f};

} // namespace

namespace levels {

Placement Box(XMFLOAT3 center, XMFLOAT3 size, float yaw_degrees, XMFLOAT3 color, float checker) {
    Placement placement;
    // An empty model name is the shared unit cube. This is the same
    // scale * rotate * translate the old Scene::AddBox composed.
    XMStoreFloat4x4(&placement.transform,
                    XMMatrixScaling(size.x, size.y, size.z) *
                        XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                        XMMatrixTranslation(center.x, center.y, center.z));
    placement.tint = color;
    placement.checker = checker;
    return placement;
}

Placement Prop(std::string model, FXMMATRIX transform, XMFLOAT3 tint) {
    Placement placement;
    placement.model = std::move(model);
    XMStoreFloat4x4(&placement.transform, transform);
    placement.tint = tint;
    return placement;
}

Placement DynamicProp(std::string model, FXMMATRIX transform, float mass, float knock_rating,
                      XMFLOAT3 tint) {
    Placement placement = Prop(std::move(model), transform, tint);
    placement.dynamic = true;
    placement.mass = mass;
    placement.knock_rating = knock_rating;
    return placement;
}

LevelDef Backyard() {
    LevelDef level;
    level.name = "Backyard";

    // The yard. +X is east, +Z is north, and the player spawns at the south end
    // looking at the grill.
    //
    // Each model's origin sits on the ground beneath it, so a prop that is not
    // scaled or turned goes where it belongs with a plain translation, and its
    // parts come along as the nodes of one asset.
    level.placements = {
        Box({0.0f, -0.15f, 0.0f}, {60.0f, 0.3f, 60.0f}, 0.0f, kGrass, 2.0f),
        Box({0.0f, 0.03f, 2.5f}, {16.0f, 0.06f, 11.0f}, 0.0f, kConcrete, 1.0f),

        // Fence. Long enough to overlap at the corners.
        Box({0.0f, 1.0f, 12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood),
        Box({0.0f, 1.0f, -12.0f}, {24.5f, 2.0f, 0.25f}, 0.0f, kFenceWood),
        Box({12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood),
        Box({-12.0f, 1.0f, 0.0f}, {0.25f, 2.0f, 24.5f}, 0.0f, kFenceWood),

        // The grill and cooler are dynamic bodies, not part of the static world:
        // run into one and it topples or slides. DynamicProp draws it like any
        // other prop but hands its colliders to Physics as one rigid piece rather
        // than nailing them down. The grill is light and top-heavy so it goes over;
        // the cooler is heavier and low, so it mostly gets shoved -- which is how a
        // cooler behaves. The knock rating is 1..10 "hard to knock over": the grill
        // sits planted (8), the cooler nearly as stubborn (7).
        DynamicProp("grill-basic.glb", XMMatrixTranslation(0.0f, 0.0f, 5.0f), 12.0f, 8.0f, kWhite),
        Prop("bench-basic.glb", XMMatrixTranslation(-4.5f, 0.0f, 1.5f), kWhite),
        DynamicProp("cooler-basic.glb", XMMatrixTranslation(3.6f, 0.0f, 6.5f), 14.0f, 7.0f, kWhite),

        // Two crates, the upper one knocked askew. The smaller is the same box at
        // 0.875 -- an exact scale, unlike the trees', because these two always were
        // the same cube at 0.8 m and 0.7 m. It stands on the lower one, so it is
        // lifted by the lower one's height rather than by an arithmetic centre.
        Prop("crate.glb", XMMatrixTranslation(5.8f, 0.0f, -2.0f), kWhite),
        Prop("crate.glb",
             XMMatrixScaling(0.875f, 0.875f, 0.875f) *
                 XMMatrixRotationY(XMConvertToRadians(22.0f)) *
                 XMMatrixTranslation(5.8f, 0.8f, -2.0f),
             kWhite),

        // Two of the same tree, told apart by a scale and a turn -- which is the
        // whole reason it is one asset rather than two. The canopy is already yawed
        // 12 degrees inside the model, so neither tree's leaves line up with its own
        // trunk, and the two do not line up with each other.
        //
        // Each canopy starts above head height, so the collider pass skips it and
        // the player can walk underneath. Turning a tree turns its trunk's collider
        // with it -- the static actor is an oriented box, so the trunk is as solid
        // as it looks rather than a few centimetres wider at the corners.
        Prop("tree.glb",
             XMMatrixRotationY(XMConvertToRadians(25.0f)) *
                 XMMatrixTranslation(-8.0f, 0.0f, 7.5f),
             kWhite),
        Prop("tree.glb",
             XMMatrixScaling(0.87f, 0.87f, 0.87f) *
                 XMMatrixRotationY(XMConvertToRadians(-15.0f)) *
                 XMMatrixTranslation(8.5f, 0.0f, -6.0f),
             kWhite),
    };

    return level;
}

} // namespace levels
