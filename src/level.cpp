#include "level.hpp"

#include <toml++/toml.hpp>

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace DirectX;

namespace {

// One placement's transform is authored the way the level files spell it: a box by
// its centre, full size and Y turn; a prop by its position, Y turn and uniform
// scale. Both recompose to scale * rotateY * translate -- exactly what the old
// hand-written helpers built, so the levels place bit-for-bit as the code did.
Placement MakeBox(XMFLOAT3 center, XMFLOAT3 size, float yaw_degrees, XMFLOAT3 color,
                  float checker) {
    Placement placement;
    // An empty model name is the shared unit cube.
    XMStoreFloat4x4(&placement.transform,
                    XMMatrixScaling(size.x, size.y, size.z) *
                        XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                        XMMatrixTranslation(center.x, center.y, center.z));
    placement.tint = color;
    placement.checker = checker;
    return placement;
}

Placement MakeProp(std::string model, XMFLOAT3 position, float yaw_degrees, float scale,
                   XMFLOAT3 tint, bool dynamic, float mass, float knock_rating) {
    Placement placement;
    placement.model = std::move(model);
    XMStoreFloat4x4(&placement.transform,
                    XMMatrixScaling(scale, scale, scale) *
                        XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                        XMMatrixTranslation(position.x, position.y, position.z));
    placement.tint = tint;
    placement.dynamic = dynamic;
    placement.mass = mass;
    placement.knock_rating = knock_rating;
    return placement;
}

// A malformed-file error naming the file. TOML syntax errors carry their own line
// (see the parse below); these are the semantic checks on top -- a missing field, a
// vector that is not three numbers -- which toml++ has no opinion about.
[[noreturn]] void Fail(const std::filesystem::path& path, const std::string& what) {
    throw std::runtime_error(path.filename().string() + ": " + what);
}

// A TOML number as double, whether written as a float (1.5) or a bare integer (1):
// toml++ keeps the two types distinct, so a level may write either.
double AsDouble(const toml::node& node, const std::filesystem::path& path, const char* what) {
    if (const auto d = node.value<double>()) {
        return *d;
    }
    if (const auto i = node.value<std::int64_t>()) {
        return static_cast<double>(*i);
    }
    Fail(path, std::string(what) + " must be a number");
}

// A required [x, y, z] array, as a float vector.
XMFLOAT3 Vec3(const toml::node_view<const toml::node>& view, const std::filesystem::path& path,
              const char* what) {
    const toml::array* array = view.as_array();
    if (array == nullptr || array->size() != 3) {
        Fail(path, std::string(what) + " must be an array of three numbers");
    }
    return {static_cast<float>(AsDouble((*array)[0], path, what)),
            static_cast<float>(AsDouble((*array)[1], path, what)),
            static_cast<float>(AsDouble((*array)[2], path, what))};
}

// An optional [x, y, z], falling back to `fallback` when the key is absent.
XMFLOAT3 Vec3Or(const toml::node_view<const toml::node>& view, XMFLOAT3 fallback,
                const std::filesystem::path& path, const char* what) {
    return view ? Vec3(view, path, what) : fallback;
}

// An optional [x, y] (the cloud wind is the only two-vector the format has),
// falling back when the key is absent.
XMFLOAT2 Vec2Or(const toml::node_view<const toml::node>& view, XMFLOAT2 fallback,
                const std::filesystem::path& path, const char* what) {
    if (!view) {
        return fallback;
    }
    const toml::array* array = view.as_array();
    if (array == nullptr || array->size() != 2) {
        Fail(path, std::string(what) + " must be an array of two numbers");
    }
    return {static_cast<float>(AsDouble((*array)[0], path, what)),
            static_cast<float>(AsDouble((*array)[1], path, what))};
}

// An optional scalar number, falling back when the key is absent.
float NumberOr(const toml::node_view<const toml::node>& view, float fallback,
               const std::filesystem::path& path, const char* what) {
    return view ? static_cast<float>(AsDouble(*view.node(), path, what)) : fallback;
}

} // namespace

namespace levels {

LevelDef LoadFromFile(const std::filesystem::path& path) {
    toml::table parsed;
    try {
        parsed = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        // toml++ reports the line and column of the syntax error; surface it with the
        // file name so a broken level names itself.
        std::ostringstream message;
        message << path.filename().string() << ":" << error.source().begin.line << ": "
                << error.description();
        throw std::runtime_error(message.str());
    }
    const toml::table& root = parsed;

    LevelDef level;
    level.name = root["name"].value_or(std::string{});
    level.player_spawn = Vec3Or(root["spawn"], level.player_spawn, path, "spawn");
    level.player_facing = NumberOr(root["facing"], level.player_facing, path, "facing");

    // An optional `time_of_day` (clock hours, 0-24) generates a whole sky at once --
    // the sun's arc position and a matching environment -- as a starting point the
    // explicit `sun` and [environment] below still override. Absent, the level keeps
    // its default sun and default look.
    if (const toml::node_view<const toml::node> tod = root["time_of_day"]) {
        const float hours = static_cast<float>(AsDouble(*tod.node(), path, "time_of_day"));
        level.environment = EnvironmentAtHour(hours, level.sun_direction);
    }

    level.sun_direction = Vec3Or(root["sun"], level.sun_direction, path, "sun");

    // The optional [environment] table: the level's sky and lighting. Every field
    // reads over the default already sitting in level.environment, so anything the
    // table omits (or a level with no table at all) keeps today's look. See
    // Environment in environment.hpp for what each field drives.
    if (const toml::table* e = root["environment"].as_table()) {
        Environment& env = level.environment;
        env.sky.zenith = Vec3Or((*e)["sky_zenith"], env.sky.zenith, path, "sky_zenith");
        env.sky.horizon = Vec3Or((*e)["sky_horizon"], env.sky.horizon, path, "sky_horizon");
        env.sky.ground = Vec3Or((*e)["ground_bounce"], env.sky.ground, path, "ground_bounce");
        env.sky.cloud_color = Vec3Or((*e)["cloud_color"], env.sky.cloud_color, path, "cloud_color");
        env.sky.cloud_scale = NumberOr((*e)["cloud_scale"], env.sky.cloud_scale, path, "cloud_scale");
        env.sky.cloud_wind = Vec2Or((*e)["cloud_wind"], env.sky.cloud_wind, path, "cloud_wind");
        env.sky.cloud_coverage =
            NumberOr((*e)["cloud_coverage"], env.sky.cloud_coverage, path, "cloud_coverage");
        env.sky.cloud_softness =
            NumberOr((*e)["cloud_softness"], env.sky.cloud_softness, path, "cloud_softness");
        env.sky.cloud_opacity =
            NumberOr((*e)["cloud_opacity"], env.sky.cloud_opacity, path, "cloud_opacity");
        env.sun_color = Vec3Or((*e)["sun_color"], env.sun_color, path, "sun_color");
        env.sun_intensity = NumberOr((*e)["sun_intensity"], env.sun_intensity, path, "sun_intensity");
        env.sky_ambient = Vec3Or((*e)["sky_ambient"], env.sky_ambient, path, "sky_ambient");
        env.ambient_strength =
            NumberOr((*e)["ambient_strength"], env.ambient_strength, path, "ambient_strength");
        env.fill_strength = NumberOr((*e)["fill_strength"], env.fill_strength, path, "fill_strength");
        env.fog_start = NumberOr((*e)["fog_start"], env.fog_start, path, "fog_start");
        env.fog_end = NumberOr((*e)["fog_end"], env.fog_end, path, "fog_end");
        env.shaft_color = Vec3Or((*e)["shaft_color"], env.shaft_color, path, "shaft_color");
        env.shaft_intensity =
            NumberOr((*e)["shaft_intensity"], env.shaft_intensity, path, "shaft_intensity");
        env.shaft_g = NumberOr((*e)["shaft_g"], env.shaft_g, path, "shaft_g");
        env.exposure = NumberOr((*e)["exposure"], env.exposure, path, "exposure");
        env.bloom_intensity =
            NumberOr((*e)["bloom_intensity"], env.bloom_intensity, path, "bloom_intensity");
        env.bloom_threshold =
            NumberOr((*e)["bloom_threshold"], env.bloom_threshold, path, "bloom_threshold");
        env.bloom_knee = NumberOr((*e)["bloom_knee"], env.bloom_knee, path, "bloom_knee");
    }

    if (const toml::array* boxes = root["box"].as_array()) {
        for (const toml::node& node : *boxes) {
            const toml::table* box = node.as_table();
            if (box == nullptr) {
                Fail(path, "each box must be a table");
            }
            const XMFLOAT3 center = Vec3((*box)["center"], path, "box center");
            const XMFLOAT3 size = Vec3((*box)["size"], path, "box size");
            const XMFLOAT3 color = Vec3((*box)["color"], path, "box color");
            const float yaw = NumberOr((*box)["yaw"], 0.0f, path, "box yaw");
            const float checker = NumberOr((*box)["checker"], 0.0f, path, "box checker");
            level.placements.push_back(MakeBox(center, size, yaw, color, checker));
        }
    }

    if (const toml::array* props = root["prop"].as_array()) {
        for (const toml::node& node : *props) {
            const toml::table* prop = node.as_table();
            if (prop == nullptr) {
                Fail(path, "each prop must be a table");
            }
            std::string model = (*prop)["model"].value_or(std::string{});
            if (model.empty()) {
                Fail(path, "each prop needs a model");
            }
            const XMFLOAT3 pos = Vec3((*prop)["pos"], path, "prop pos");
            const float yaw = NumberOr((*prop)["yaw"], 0.0f, path, "prop yaw");
            const float scale = NumberOr((*prop)["scale"], 1.0f, path, "prop scale");
            const XMFLOAT3 tint = Vec3Or((*prop)["tint"], XMFLOAT3{1.0f, 1.0f, 1.0f}, path, "prop tint");
            const bool dynamic = (*prop)["dynamic"].value_or(false);
            const float mass = NumberOr((*prop)["mass"], 1.0f, path, "prop mass");
            const float knock = NumberOr((*prop)["knock"], 1.0f, path, "prop knock");
            level.placements.push_back(
                MakeProp(std::move(model), pos, yaw, scale, tint, dynamic, mass, knock));
        }
    }

    return level;
}

} // namespace levels
