#pragma once

#include "cook_information.hpp"
#include "rigid_body.hpp" // ImpactSound

#include <DirectXMath.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// How a carried object sits in the player's hand -- the two poses Props knows how to
// hold. Flat things (food) are tipped up to show a face; the tongs point away down the
// gaze. Named by the catalog so a carryable picks its pose without Props knowing the
// object.
enum class HoldStyle { Flat, Tongs };

// The heat a prop radiates, as pure data: the air temperature at the centre, how far
// (metres) it carries before fading to room air, and where the hot centre sits in the
// model's own space (up at a grate, not down at the feet). Scene turns this into a
// HeatSource on the prop's dynamic body. Heat rides dynamic props only.
struct HeatDef {
    float temp_f = 400.0f;
    float reach = 1.0f;
    DirectX::XMFLOAT3 offset{0.0f, 0.0f, 0.0f};
};

// One model in a carryable's cook progression: the .glb to draw once the food's
// doneness has reached `from` (and until a later stage's band overtakes it). A tool
// or a single-model food is one stage at Raw, so it never changes; chicken adds a
// "cooked" model at Medium so it visibly turns over on the grill. The models are
// resolved (loaded) by Scene; this is just the name and the band it kicks in at.
struct CookStageModel {
    std::string model;
    CookInformation::Doneness from = CookInformation::Doneness::Raw;
};

// A carryable type -- a food or a tool -- as the catalog spells it, before its models
// are loaded. Foods carry a cook profile (they grill); tools leave it empty (the tongs
// are not food). Everything a carried item needs that does not depend on where it sits
// in a level: its model(s), how it cooks, how it lands, and how it is held.
struct CarryableDef {
    // The models this carryable draws as it cooks, always at least one (the base, at
    // Raw). One entry for a tool or a single-model food; several for a food that
    // swaps model with doneness. Ordering is not assumed -- the drawn model is picked
    // by band each frame (see Props::CurrentModel).
    std::vector<CookStageModel> models;
    std::optional<CookProfile> cook; // set for foods, empty for tools
    float knock_rating = 4.0f;
    ImpactSound impact_sound = ImpactSound::Meat;
    HoldStyle hold = HoldStyle::Flat;
};

// A placeable prop type -- furniture or scenery -- as the catalog spells it. `dynamic`
// makes it a knock-over-able body (reading mass/knock/sound and any heat); left off, it
// is immovable world and those are ignored. `heat` is present only on the hot props
// (the grill), and only takes effect on a dynamic one.
struct PropDef {
    std::string model;
    bool dynamic = false;
    float mass = 1.0f;
    float knock_rating = 1.0f;
    ImpactSound impact_sound = ImpactSound::None;
    std::optional<HeatDef> heat;
};

// The whole catalog: the game's object archetypes, keyed by the name a level places
// them under. Carryables (foods + tools, owned by Props) and props (furniture +
// scenery, owned by Scene/Furniture). Placement -- where each sits -- lives in the
// level files; this says what each *is*, independent of any level.
struct Catalog {
    std::unordered_map<std::string, CarryableDef> carryables;
    std::unordered_map<std::string, PropDef> props;
};

namespace catalog {

// Reads assets/catalog.toml into a Catalog: `[food.<name>]` and `[tool.<name>]` become
// carryables (a food with a cook profile, a tool without), and `[prop.<name>]` become
// props. Every field but `model` is optional and defaults to the values baked in the
// structs above, so a terse entry is just a model. Throws std::runtime_error -- naming
// the file, and the line for a TOML syntax error -- on anything it cannot parse. The
// catalog is text parsed at runtime, like the levels, so a new type is an edit and a
// re-stage, not a rebuild.
Catalog Load(const std::filesystem::path& path);

} // namespace catalog
