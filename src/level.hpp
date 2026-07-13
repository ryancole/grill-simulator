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

// Everything that makes one level its own place: a name and the things standing
// in it. Deliberately just data returned by a function (levels::Backyard) rather
// than parsed from a file -- the struct is the schema, and it can be serialised
// later once a second and third level have stopped reshaping it.
struct LevelDef {
    std::string name;
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

} // namespace levels
