#pragma once

#include "cook_information.hpp"
#include "environment.hpp"

#include <DirectXMath.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// A coloured box: the shared unit cube (MakeUnitCubeModel) under a transform. The
// ground, patio and fence are all this -- geometry authored inline in the level rather
// than a model from disk, so it carries its own colour and an optional checker rather
// than referencing a catalog type.
struct BoxPlacement {
    DirectX::XMFLOAT4X4 transform;
    // The cube's whole colour (it has no material of its own).
    DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};
    // Metres per checkerboard tile, projected down Y. Zero leaves the surface flat;
    // only the ground and patio use it.
    float checker = 0.0f;
};

// One placed prop -- furniture or scenery -- naming a catalog [prop.*] type and where
// it stands. What the prop *is* (model, physics, heat) lives in the catalog; this says
// only which type and its transform, so the same grill type places in every level from
// one definition.
struct PropPlacement {
    std::string type;
    DirectX::XMFLOAT4X4 transform;
};

// One placed carryable -- a food or a tool -- naming a catalog [food.*]/[tool.*] type
// and where it starts. Props seeds it from its position and yaw; the type supplies the
// model, cook and hold. Levels place these so the starting meats are level content, not
// baked into Props.
struct CarryablePlacement {
    std::string type;
    DirectX::XMFLOAT3 pos;
    float yaw = 0.0f;
};

// One line of a level's win condition: a number of a food `type` (a catalog food name,
// as the level places it) the player must deliver to a serving zone cooked into the
// doneness band range [min, max], inclusive. Because doneness only ever rises, `min` is
// "at least this cooked" (an undercooked serve is rejected) and `max` is "no more than
// this" (max = WellDone rejects a burnt serve). The defaults span a forgiving middle --
// at least rare, no more than well done, one needed -- so a terse goal is just a type.
struct FoodGoal {
    std::string type;
    int count = 1;
    CookInformation::Doneness min = CookInformation::Doneness::Rare;
    CookInformation::Doneness max = CookInformation::Doneness::WellDone;
};

// The level's turn-in spot: where the player carries the loaded tray to end the level. A
// horizontal circle (`radius` metres around `pos` in the ground plane, height ignored)
// that Props tests the carried tray against -- pressing Interact inside it hands the tray
// in and its meats are judged against the goals. Present only on a level that sets a
// `turn_in`; a level without one is a sandbox with no way to win (like an empty `goal`).
struct TurnInDef {
    DirectX::XMFLOAT3 pos{0.0f, 0.0f, 0.0f};
    float radius = 1.0f;
};

// The level's grass field: a flat rectangle of GPU-grown blades over the ground.
// `center` is its middle (y the ground height the blades stand on) and `size` its
// extent in metres (x by z). The rest tune the look: `color` the base blade colour
// (sRGB), `blade_height`/`blade_width` a blade's dimensions before per-blade variation,
// and `wind` the breeze direction and strength the blades sway in. Present only on a
// level with a `grass` table; a level without one grows none. Drawn only where the
// device supports mesh shaders, so on older hardware the field is simply absent.
struct GrassDef {
    DirectX::XMFLOAT3 center{0.0f, 0.0f, 0.0f};
    DirectX::XMFLOAT2 size{20.0f, 20.0f};
    DirectX::XMFLOAT3 color{0.33f, 0.5f, 0.18f};
    float blade_height = 0.35f;
    float blade_width = 0.03f;
    DirectX::XMFLOAT2 wind{0.15f, 0.05f};
};

// Everything that makes one level its own place: a name, where the player starts,
// which way the sun falls, and the things standing in it. Plain data, parsed from a
// level's TOML file by LoadFromFile -- this struct is that format's schema, so a
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

    // The things standing in the level: inline cube geometry, catalog props (furniture
    // and scenery), and the carryables the player starts with. Placed in that order.
    std::vector<BoxPlacement> boxes;
    std::vector<PropPlacement> props;
    std::vector<CarryablePlacement> carryables;

    // The level's win condition: the orders the player must deliver on the tray, each
    // cooked into its band range. Empty on a level that sets no `goal` array, which is
    // simply a level with nothing to win (the sandbox levels leave it so).
    std::vector<FoodGoal> goals;

    // Where the player turns the loaded tray in to end the level. Unset on a level with
    // no `turn_in` table, which then has no delivery point (a sandbox).
    std::optional<TurnInDef> turn_in;

    // The level's grass field, if it set a `grass` table. Unset on a level with none,
    // which grows no grass.
    std::optional<GrassDef> grass;
};

namespace levels {

// Reads a level from a TOML file (see assets/levels/backyard.toml, which documents the
// format). The top level carries name/spawn/facing/sun; an optional
// `time_of_day` (clock hours) generates a whole sky, which an optional `[environment]`
// table (and the explicit `sun`) then override field by field -- every field falling
// back to the default look. A `box` array holds inline cube geometry (centre/size/yaw/
// colour/checker), a `prop` array places catalog props by `type` (pos/yaw/scale), and a
// `carryable` array places catalog foods and tools by `type` (pos/yaw) -- the loader
// recomposes the authoring parameters into transforms. Throws std::runtime_error --
// naming the file, and the line for a TOML syntax error -- on anything it cannot parse.
//
// Levels live in files rather than code so a new one is a text edit, not a rebuild; the
// props and carryables they place are catalog types (see catalog.hpp), so a level names
// a "charcoal_grill" or a "steak" and the catalog says what those are.
LevelDef LoadFromFile(const std::filesystem::path& path);

} // namespace levels
