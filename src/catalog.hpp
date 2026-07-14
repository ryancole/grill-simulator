#pragma once

#include "cook_information.hpp"
#include "rigid_body.hpp" // ImpactSound

#include <filesystem>
#include <string>
#include <unordered_map>

// One food type as the catalog spells it, before its model is loaded: the .glb that
// draws it, how it lands (its knock rating and the sound it makes), and how it cooks
// (its CookProfile). This is the archetype -- what a "steak" is -- independent of any
// level: a level places these by name, and Scene loads the model and resolves the def
// into a spawnable food.
//
// Pure data, like a Placement. Parsed from assets/catalog.toml by LoadFoods; the model
// is still a filename here (Scene turns it into a loaded model id).
struct FoodDef {
    std::string model;
    CookProfile cook;
    // How hard the food is to knock about (1..10) and the clip it plays on a hard
    // landing -- carried onto the item's body exactly as a level prop's would be.
    float knock_rating = 4.0f;
    ImpactSound impact_sound = ImpactSound::Meat;
};

namespace catalog {

// Reads the `[food.<name>]` tables of assets/catalog.toml into name -> FoodDef. Every
// field but the model is optional, defaulting to the CookProfile defaults (the game's
// original single cook), so a terse entry is just a model. Throws std::runtime_error
// -- naming the file, and the line for a TOML syntax error -- on anything it cannot
// parse. The catalog lives in a text file, parsed at runtime, for the same reason the
// levels do: a new food is an edit and a re-stage, not a rebuild.
std::unordered_map<std::string, FoodDef> LoadFoods(const std::filesystem::path& path);

} // namespace catalog
