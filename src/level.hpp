#pragma once

#include "environment.hpp"
#include "rigid_body.hpp"

#include <DirectXMath.h>

#include <filesystem>
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
    // The sound this body makes on a hard landing, for a dynamic placement (the
    // grill clatters). None -- the default -- for the silent furniture (the cooler)
    // and every static placement, which never reads it.
    ImpactSound impact_sound = ImpactSound::None;

    // Whether this placement radiates heat -- the grill does, and nothing else in
    // the yard yet. When set, Scene gives its dynamic body a HeatSource so the meats
    // set on it cook; the three fields below are how hot and how far. Off by default,
    // so a placement that says nothing about heat is stone cold and the fields are
    // ignored. Heat currently rides only dynamic bodies (Furniture moves the hot zone
    // with the body's pose), so a static placement that asks for heat has none.
    bool emits_heat = false;
    // The air temperature at the heat centre, in degrees Fahrenheit, and how far in
    // metres that heat carries before it fades back to room air. A charcoal grate
    // runs a few hundred degrees; the reach is a hand's-breadth around the grate, so
    // food has to sit on it to cook.
    float heat_temp_f = 400.0f;
    float heat_reach = 1.0f;
    // Where the heat centre sits relative to the model's origin, in the model's own
    // space -- for the grill, up at the grate rather than down at the feet. Carried
    // through the body's pose each frame, so the hot spot stays on the grate even
    // after the grill is knocked over.
    DirectX::XMFLOAT3 heat_offset{0.0f, 0.0f, 0.0f};
};

// Everything that makes one level its own place: a name, where the player starts,
// which way the sun falls, and the things standing in it. Plain data, parsed from a
// `.level` text file by LoadFromFile -- this struct is that format's schema, so a
// field added here is a field the loader (and the files) grow.
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

    // The level's sky and lighting: sun colour, sky gradient, clouds, ambient, fog
    // and shafts. Defaults to the look the shaders once baked in, so a level that
    // names no `[environment]` table (or omits a field) is drawn exactly as before.
    Environment environment = kDefaultEnvironment;

    std::vector<Placement> placements;
};

namespace levels {

// Reads a level from a `.level` TOML file (see assets/levels/backyard.level, which
// documents the format). The top level carries name/spawn/facing/sun; an optional
// `time_of_day` (clock hours) generates a whole sky, which an optional `[environment]`
// table (and the explicit `sun`) then override field by field -- every field falling
// back to the default look; a `box` array and a `prop` array hold the objects, each
// storing the authoring parameters it is placed by (a box's centre/size/yaw/colour, a
// prop's pos/yaw/scale) which the loader recomposes into the same transforms the code
// once built by hand. Boxes are placed before props. Throws std::runtime_error --
// naming the file, and the line for a TOML syntax error -- on anything it cannot parse.
//
// Levels live in files rather than code so a new one is a text edit, not a rebuild;
// the LevelDef struct above is the format's schema, parsed with toml++.
LevelDef LoadFromFile(const std::filesystem::path& path);

} // namespace levels
