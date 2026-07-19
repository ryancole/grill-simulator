#include "catalog.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

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

// An optional [x, y, z], falling back to `fallback` when the key is absent.
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

// An optional impact sound named by string, falling back when absent. An unrecognised
// name is a catalog error, not a silent default.
ImpactSound SoundOr(const toml::node_view<const toml::node>& view, ImpactSound fallback,
                    const std::filesystem::path& path) {
    if (!view) {
        return fallback;
    }
    const auto name = view.value<std::string>();
    if (!name) {
        Fail(path, "sound must be a string");
    }
    if (*name == "meat") return ImpactSound::Meat;
    if (*name == "metal") return ImpactSound::Metal;
    if (*name == "grill_base") return ImpactSound::GrillBase;
    if (*name == "grill_lid") return ImpactSound::GrillLid;
    Fail(path, "unknown sound '" + *name + "' (want meat, metal, grill_base or grill_lid)");
}

// An optional hold style named by string, falling back when absent.
HoldStyle HoldOr(const toml::node_view<const toml::node>& view, HoldStyle fallback,
                 const std::filesystem::path& path) {
    if (!view) {
        return fallback;
    }
    const auto name = view.value<std::string>();
    if (!name) {
        Fail(path, "hold must be a string");
    }
    if (*name == "flat") return HoldStyle::Flat;
    if (*name == "tongs") return HoldStyle::Tongs;
    if (*name == "tray") return HoldStyle::Tray;
    Fail(path, "unknown hold '" + *name + "' (want flat, tongs or tray)");
}

// An optional ability named by string, falling back when absent. An unrecognised name
// is a catalog error, not a silent default. The vocabulary grows as abilities are added
// in code; for now "none" (the default) is the only behaviour there is.
Ability AbilityOr(const toml::node_view<const toml::node>& view, Ability fallback,
                  const std::filesystem::path& path) {
    if (!view) {
        return fallback;
    }
    const auto name = view.value<std::string>();
    if (!name) {
        Fail(path, "ability must be a string");
    }
    if (*name == "none") return Ability::None;
    if (*name == "grip_meat") return Ability::GripMeat;
    if (*name == "stack_in_fire_pit") return Ability::StackInFirePit;
    if (*name == "spray_fluid") return Ability::SprayFluid;
    if (*name == "flame") return Ability::Flame;
    Fail(path, "unknown ability '" + *name +
                   "' (want none, grip_meat, stack_in_fire_pit, spray_fluid or flame)");
}

// The optional heat a type radiates, read from its `heat` (centre temperature) and the
// `heat_reach`/`heat_offset`/`heat_starts_on` beside it. Absent `heat` leaves the type
// cold. Shared by props (the grill) and carryables (the log), which spell heat the same.
std::optional<HeatDef> ReadHeat(const toml::table& entry, const std::filesystem::path& path) {
    const toml::node_view<const toml::node> heat = entry["heat"];
    if (!heat) {
        return std::nullopt;
    }
    HeatDef h;
    h.temp_f = static_cast<float>(AsDouble(*heat.node(), path, "heat"));
    h.reach = NumberOr(entry["heat_reach"], h.reach, path, "heat_reach");
    h.offset = Vec3Or(entry["heat_offset"], h.offset, path, "heat_offset");
    h.starts_on = entry["heat_starts_on"].value_or(h.starts_on);
    return h;
}

// What it takes to light a type, read from its `ignite_temp` (the air temperature that
// sets it alight). Absent, the type simply never catches. Shared by carryables (the log)
// and props (the grill's grate), which spell ignition the same way.
std::optional<IgnitableRequirements> ReadIgnitable(const toml::table& entry,
                                                   const std::filesystem::path& path) {
    const toml::node_view<const toml::node> temp = entry["ignite_temp"];
    if (!temp) {
        return std::nullopt;
    }
    const float ignite_temp = static_cast<float>(AsDouble(*temp.node(), path, "ignite_temp"));
    // How slowly it heats through to that (default matches the struct's own): a couple of
    // these seconds of a flame held on it before it catches. Absent leaves the default.
    const float tau = NumberOr(entry["ignite_tau"], 3.0f, path, "ignite_tau");
    return IgnitableRequirements(ignite_temp, tau);
}

// The model name a type must name, or a catalog error.
std::string ModelOf(const toml::table& entry, const std::string& name,
                    const std::filesystem::path& path) {
    std::string model = entry["model"].value_or(std::string{});
    if (model.empty()) {
        Fail(path, "'" + name + "' needs a model");
    }
    return model;
}

// A doneness band named by string, for a food's model stages. The names match the
// readout labels, with underscores standing in for the spaces in the two-word bands.
CookInformation::Doneness DonenessFromName(const std::string& name,
                                           const std::filesystem::path& path) {
    if (name == "raw") return CookInformation::Doneness::Raw;
    if (name == "rare") return CookInformation::Doneness::Rare;
    if (name == "medium_rare") return CookInformation::Doneness::MediumRare;
    if (name == "medium") return CookInformation::Doneness::Medium;
    if (name == "medium_well") return CookInformation::Doneness::MediumWell;
    if (name == "well_done") return CookInformation::Doneness::WellDone;
    if (name == "burnt") return CookInformation::Doneness::Burnt;
    Fail(path, "unknown doneness '" + name +
                   "' (want raw, rare, medium_rare, medium, medium_well, well_done or burnt)");
}

// The model(s) a carryable draws. Two spellings, mutually exclusive: a single
// `model = "x.glb"` (one stage at Raw -- how every tool and every single-look food is
// written), or a `models = [{model, from}, ...]` list for a food that changes model
// as it cooks (chicken swaps to its cooked look at `from = "medium"`). `from` defaults
// to raw, so the base stage can omit it.
std::vector<CookStageModel> ModelsOf(const toml::table& entry, const std::string& name,
                                     const std::filesystem::path& path) {
    const bool has_single = static_cast<bool>(entry["model"]);
    const toml::array* staged = entry["models"].as_array();
    if (has_single && staged != nullptr) {
        Fail(path, "'" + name + "' gives both model and models -- use one");
    }
    if (staged != nullptr) {
        std::vector<CookStageModel> models;
        for (const toml::node& node : *staged) {
            const toml::table* stage = node.as_table();
            if (stage == nullptr) {
                Fail(path, "each entry of '" + name + "' models must be a table {model, from}");
            }
            CookStageModel model;
            model.model = (*stage)["model"].value_or(std::string{});
            if (model.model.empty()) {
                Fail(path, "a models entry of '" + name + "' needs a model");
            }
            if (const auto from = (*stage)["from"].value<std::string>()) {
                model.from = DonenessFromName(*from, path);
            }
            models.push_back(std::move(model));
        }
        if (models.empty()) {
            Fail(path, "'" + name + "' models must list at least one model");
        }
        return models;
    }
    // The common case: a single required model, drawn from raw on.
    return {CookStageModel{ModelOf(entry, name, path), CookInformation::Doneness::Raw}};
}

// Reads the cook profile from a food entry: each field over the CookProfile default, so
// a food that names none cooks the game's original single way. The six band starts are
// given together as [rare..burnt] so they cannot be set half-and-half.
CookProfile ReadCook(const toml::table& entry, const std::string& name,
                     const std::filesystem::path& path) {
    CookProfile cook;
    cook.temp_time_constant_s =
        NumberOr(entry["cook_tau"], cook.temp_time_constant_s, path, "cook_tau");
    cook.cook_threshold_f =
        NumberOr(entry["cook_threshold_f"], cook.cook_threshold_f, path, "cook_threshold_f");
    cook.doneness_gain = NumberOr(entry["doneness_gain"], cook.doneness_gain, path, "doneness_gain");
    if (const toml::array* bands = entry["bands"].as_array()) {
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
    cook.raw_tint = Vec3Or(entry["raw_tint"], cook.raw_tint, path, "raw_tint");
    cook.seared_tint = Vec3Or(entry["seared_tint"], cook.seared_tint, path, "seared_tint");
    cook.char_tint = Vec3Or(entry["char_tint"], cook.char_tint, path, "char_tint");
    return cook;
}

// Reads the `[<section>.*]` tables of `root` as carryables, cooking or not. Foods carry
// a cook profile; tools leave it empty. Both share model/knock/sound/hold.
void ReadCarryables(const toml::table& root, const char* section, bool is_food,
                    const std::filesystem::path& path,
                    std::unordered_map<std::string, CarryableDef>& out) {
    const toml::table* table = root[section].as_table();
    if (table == nullptr) {
        return;
    }
    for (const auto& [key, node] : *table) {
        const toml::table* entry = node.as_table();
        if (entry == nullptr) {
            Fail(path, std::string("each [") + section + ".*] must be a table");
        }
        const std::string name(key.str());

        CarryableDef def;
        def.models = ModelsOf(*entry, name, path);
        if (is_food) {
            def.cook = ReadCook(*entry, name, path);
        }
        // A serving tray declares `serve` (its ground-plane delivery radius) and
        // optional `serve_offset` (its top surface in model space); other carryables
        // omit it. Mirrors how a prop's heat reads, one level up.
        if (const toml::node_view<const toml::node> serve = (*entry)["serve"]) {
            ServeDef s;
            s.radius = static_cast<float>(AsDouble(*serve.node(), path, "serve"));
            s.offset = Vec3Or((*entry)["serve_offset"], s.offset, path, "serve_offset");
            def.serve = s;
        }
        def.knock_rating = NumberOr((*entry)["knock"], def.knock_rating, path, "knock");
        def.impact_sound = SoundOr((*entry)["sound"], def.impact_sound, path);
        def.hold = HoldOr((*entry)["hold"], def.hold, path);
        def.ability = AbilityOr((*entry)["ability"], def.ability, path);
        // A carryable may radiate heat too (the firewood log). Same spelling as a prop's.
        def.heat = ReadHeat(*entry, path);
        // ...and may be lightable, which is what switches that heat on in play.
        def.ignitable = ReadIgnitable(*entry, path);
        out.emplace(name, std::move(def));
    }
}

} // namespace

namespace catalog {

Catalog Load(const std::filesystem::path& path) {
    toml::table parsed;
    try {
        parsed = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << path.filename().string() << ":" << error.source().begin.line << ": "
                << error.description();
        throw std::runtime_error(message.str());
    }

    Catalog out;

    // Carryables: foods (with a cook profile) and tools (without), both keyed by name
    // into the same map so a level places either with a plain `type`.
    ReadCarryables(parsed, "food", /*is_food=*/true, path, out.carryables);
    ReadCarryables(parsed, "tool", /*is_food=*/false, path, out.carryables);

    // Props: furniture and scenery. A `dynamic` prop reads mass/knock/sound and any
    // heat; a static one ignores them. Heat is opt-in via a `heat` temperature.
    if (const toml::table* props = parsed["prop"].as_table()) {
        for (const auto& [key, node] : *props) {
            const toml::table* entry = node.as_table();
            if (entry == nullptr) {
                Fail(path, "each [prop.*] must be a table");
            }
            const std::string name(key.str());

            PropDef def;
            def.model = ModelOf(*entry, name, path);
            def.dynamic = (*entry)["dynamic"].value_or(false);
            def.mass = NumberOr((*entry)["mass"], def.mass, path, "mass");
            def.knock_rating = NumberOr((*entry)["knock"], def.knock_rating, path, "knock");
            def.impact_sound = SoundOr((*entry)["sound"], def.impact_sound, path);
            def.heat = ReadHeat(*entry, path);
            // ...and may be lightable, which is what switches that heat on in play (the
            // grill's grate, lit with the flame like a log). Only takes effect on a
            // dynamic prop -- Furniture, which owns the bodies, is what ticks it.
            def.ignitable = ReadIgnitable(*entry, path);
            out.props.emplace(name, std::move(def));
        }
    }

    return out;
}

} // namespace catalog
