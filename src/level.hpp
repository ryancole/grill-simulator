#pragma once

#include <DirectXMath.h>

#include <string>
#include <vector>

// A single placement in a level: one model under one transform. Pure data -- no
// D3D, no PhysX. Scene turns each of these into a draw instance plus either a
// static collider or a dynamic rigid body, exactly as its constructor once did
// inline.
struct Placement {
    // The .glb under assets/models/ to place. An empty name means the shared unit
    // cube built in code (MakeUnitCubeModel) -- the ground, patio and fence are
    // all that cube under a transform, so they carry no file.
    std::string model;
    DirectX::XMFLOAT4X4 transform;
    // Multiplies the material's base colour. A textured .glb wants white here; the
    // untextured cube takes its whole colour from this.
    DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};
    // Metres per checkerboard tile, projected down Y. Zero leaves the surface flat;
    // only the ground and patio use it.
    float checker = 0.0f;
    // A knock-over-able body (grill, cooler) rather than immovable world. When set,
    // Scene diverts this placement's collider boxes into a dynamic PhysX body and
    // reads `mass`/`knock_rating`; otherwise those two are ignored.
    bool dynamic = false;
    float mass = 1.0f;
    float knock_rating = 1.0f;
};

// Everything that makes one level its own place: a name, where the player starts,
// which way the sun falls, and the things standing in it. Deliberately just data
// returned by a function (levels::Backyard) rather than parsed from a file -- the
// struct is the schema, and it can be serialised later once the levels have stopped
// reshaping it.
struct LevelDef {
    std::string name;

    // Where the player's feet start, on the ground (y is the standing height, 0 on
    // a flat yard). The camera adds eye height and drops the capsule here on load.
    DirectX::XMFLOAT3 player_spawn{0.0f, 0.0f, -7.0f};
    // Which way the player faces at spawn, in degrees: 0 looks north (+Z), the way
    // the backyard's grill sits from its south-end spawn.
    float player_facing = 0.0f;

    // The unit vector pointing at the sun, in world space. Drives the shadow map's
    // direction and the scene's direct light; the gradient sky ignores it, so a
    // level can re-angle the sun with no other change. Normalised on use.
    DirectX::XMFLOAT3 sun_direction{0.35f, 0.78f, -0.5f};

    std::vector<Placement> placements;
};

namespace levels {

// Authoring helpers so a level reads the way Scene's constructor used to: a box
// by centre/size/yaw, a prop by model and transform. Each returns one Placement.
Placement Box(DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 size, float yaw_degrees,
              DirectX::XMFLOAT3 color, float checker = 0.0f);
Placement Prop(std::string model, DirectX::FXMMATRIX transform,
               DirectX::XMFLOAT3 tint = {1.0f, 1.0f, 1.0f});
Placement DynamicProp(std::string model, DirectX::FXMMATRIX transform, float mass,
                      float knock_rating, DirectX::XMFLOAT3 tint = {1.0f, 1.0f, 1.0f});

// The original backyard: the one scene the game shipped with, now expressed as
// data. Building a Scene from it is identical to what the old constructor did by
// hand.
LevelDef Backyard();

// A concrete rooftop deck: the same props rearranged on a walled slab, no grass or
// trees, with the sun swung low from the west. The second level, and the proof
// that switching levels works on genuinely different content -- different ground,
// layout, spawn and sun.
LevelDef Rooftop();

} // namespace levels
