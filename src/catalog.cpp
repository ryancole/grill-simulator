#include "catalog.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace DirectX;

namespace {

// A malformed-catalog error naming the file. TOML syntax errors carry their own line
// (see the parse below); these are the semantic checks on top. Mirrors level.cpp's
// helper deliberately -- the two files parse the same TOML the same way.
[[noreturn]] void Fail(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error(path.filename().string() + ": " + what);
}

// A TOML number as double, whether written as a float (1.5) or a bare integer (1).
double AsDouble(const toml::node& node, const std::filesystem::path& path, const char* what) {
    if (const auto d = node.value<double>()) {
        return *d;
    }
    if (const auto i = node.value<std::int64_t>()) {
        return static_cast<double>(*i);
    }
    Fail(path, std::string(what) + " must be a number");
}

// An optional scalar number, falling back when the key is absent.
float NumberOr(const toml::node_view<const toml::node>& view, float fallback,
               const std::filesystem::path& path, const char* what) {
    return view ? static_cast<float>(AsDouble(*view.node(), path, what)) : fallback;
}

// An optional [r, g, b], falling back to `fallback` when the key is absent.
XMFLOAT3 Vec3Or(const toml::node_view<const toml::node>& view, XMFLOAT3 fallback,
                const std::filesystem::path& path, const char* what) {
    if (!view) {
        return fallback;
    }
    const toml::array* array = view.as_array();
    if (array == nullptr || array->size() != 3) {
        Fail(path, std::string(what) + " must be an array of three numbers");
    }
    return {static_cast<float>(AsDouble((*array)[0], path, what)),
            static_cast<float>(AsDouble((*array)[1], path, what)),
            static_cast<float>(AsDouble((*array)[2], path, what))};
}

// An optional impact sound named by string, falling back to `fallback` when absent. An
// unrecognised name is a catalog error, not a silent default.
ImpactSound SoundOr(const toml::node_view<const toml::node>& view, ImpactSound fallback,
                    const std::filesystem::path& path) {
    if (!view) {
        return fallback;
    }
    const auto name = view.value<std::string>();
    if (!name) {
        Fail(path, "food sound must be a string");
    }
    if (*name == "meat") return ImpactSound::Meat;
    if (*name == "metal") return ImpactSound::Metal;
    if (*name == "grill_base") return ImpactSound::GrillBase;
    if (*name == "grill_lid") return ImpactSound::GrillLid;
    Fail(path, "unknown food sound '" + *name + "' (want meat, metal, grill_base or grill_lid)");
}

} // namespace

namespace catalog {

std::unordered_map<std::string, FoodDef> LoadFoods(const std::filesystem::path& path) {
    toml::table parsed;
    try {
        parsed = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << path.filename().string() << ":" << error.source().begin.line << ": "
                << error.description();
        throw std::runtime_error(message.str());
    }

    std::unordered_map<std::string, FoodDef> foods;
    const toml::table* table = parsed["food"].as_table();
    if (table == nullptr) {
        return foods; // No foods is not an error; the game just has nothing to grill.
    }

    for (const auto& [key, node] : *table) {
        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            Fail(path, "each [food.*] must be a table");
        }
        const std::string name(key.str());

        FoodDef def;
        def.model = (*entry)["model"].value_or(std::string{});
        if (def.model.empty()) {
            Fail(path, "food '" + name + "' needs a model");
        }

        // The cook profile: each field reads over the CookProfile default already in
        // `def.cook`, so a food that names none cooks the game's original single way.
        CookProfile& cook = def.cook;
        cook.temp_time_constant_s =
            NumberOr((*entry)["cook_tau"], cook.temp_time_constant_s, path, "cook_tau");
        cook.cook_threshold_f =
            NumberOr((*entry)["cook_threshold_f"], cook.cook_threshold_f, path, "cook_threshold_f");
        cook.doneness_gain =
            NumberOr((*entry)["doneness_gain"], cook.doneness_gain, path, "doneness_gain");

        // The six band starts, given together as [rare, medium_rare, ... burnt] so the
        // order is legible and they cannot be set half-and-half. Omit to keep them all.
        if (const toml::array* bands = (*entry)["bands"].as_array()) {
            if (bands->size() != 6) {
                Fail(path, "food '" + name + "' bands must be six numbers [rare..burnt]");
            }
            cook.rare = static_cast<float>(AsDouble((*bands)[0], path, "bands"));
            cook.medium_rare = static_cast<float>(AsDouble((*bands)[1], path, "bands"));
            cook.medium = static_cast<float>(AsDouble((*bands)[2], path, "bands"));
            cook.medium_well = static_cast<float>(AsDouble((*bands)[3], path, "bands"));
            cook.well_done = static_cast<float>(AsDouble((*bands)[4], path, "bands"));
            cook.burnt = static_cast<float>(AsDouble((*bands)[5], path, "bands"));
        }

        cook.raw_tint = Vec3Or((*entry)["raw_tint"], cook.raw_tint, path, "raw_tint");
        cook.seared_tint = Vec3Or((*entry)["seared_tint"], cook.seared_tint, path, "seared_tint");
        cook.char_tint = Vec3Or((*entry)["char_tint"], cook.char_tint, path, "char_tint");

        def.knock_rating = NumberOr((*entry)["knock"], def.knock_rating, path, "knock");
        def.impact_sound = SoundOr((*entry)["sound"], def.impact_sound, path);

        foods.emplace(name, std::move(def));
    }

    return foods;
}

} // namespace catalog
