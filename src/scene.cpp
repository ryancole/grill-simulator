#include "scene.hpp"

#include "dx_common.hpp"
#include "level.hpp"

#include <cfloat>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

using namespace DirectX;

namespace {
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};
} // namespace

Scene::Scene(const LevelDef& level) {
    cube_ = AddModel(MakeUnitCubeModel());

    // The catalog says what every placed type is -- a "charcoal_grill", a "steak" --
    // independent of this level. The level names types and where they go; here they
    // are joined. Models load on first reference and are reused, so two crates or two
    // steaks upload one model each.
    const Catalog catalog = catalog::Load(ExecutableDirectory() / "assets" / "catalog.toml");
    std::unordered_map<std::string, std::uint32_t> loaded;
    const auto load = [&](const std::string& file) {
        const auto [it, inserted] = loaded.try_emplace(file, 0u);
        if (inserted) {
            it->second = LoadModel(file.c_str());
        }
        return it->second;
    };

    // Inline cube geometry: the ground, patio and fence. Each is the unit cube under a
    // transform, carrying its own colour and checker.
    for (const BoxPlacement& box : level.boxes) {
        AddInstance(cube_, XMLoadFloat4x4(&box.transform), box.tint, box.checker);
    }

    // Props: furniture and scenery. A dynamic type becomes a knock-over-able body
    // (with a HeatSource if the type radiates heat); a static one becomes immovable
    // world. The prop draws in the glTF's own colours, so its instance is white.
    for (const PropPlacement& placement : level.props) {
        const auto it = catalog.props.find(placement.type);
        if (it == catalog.props.end()) {
            throw std::runtime_error("catalog.toml has no prop named '" + placement.type + "'");
        }
        const PropDef& def = it->second;
        const std::uint32_t model = load(def.model);
        const XMMATRIX transform = XMLoadFloat4x4(&placement.transform);
        if (def.dynamic) {
            std::optional<HeatSource> heat;
            XMFLOAT3 offset{0.0f, 0.0f, 0.0f};
            if (def.heat) {
                heat.emplace(def.heat->temp_f, def.heat->reach);
                offset = def.heat->offset;
            }
            AddDynamicInstance(model, transform, kWhite, def.mass, def.knock_rating,
                               def.impact_sound, heat, offset);
        } else {
            AddInstance(model, transform, kWhite);
        }
    }

    // Carryables: the objects the player starts with. Resolve each placement's type
    // against the catalog and stash a spawn for Props to seed -- Scene draws none of
    // them itself.
    for (const CarryablePlacement& placement : level.carryables) {
        const auto it = catalog.carryables.find(placement.type);
        if (it == catalog.carryables.end()) {
            throw std::runtime_error("catalog.toml has no carryable named '" + placement.type +
                                     "'");
        }
        const CarryableDef& def = it->second;
        CarryableSpawn spawn;
        // Load each cook stage's model (deduped by `load`), carrying the band it kicks
        // in at through unchanged -- Props picks which one to draw as the food cooks.
        for (const CookStageModel& stage : def.models) {
            spawn.models.push_back({load(stage.model), stage.from});
        }
        spawn.name = placement.type;
        spawn.pos = placement.pos;
        spawn.yaw = placement.yaw;
        spawn.hold = def.hold;
        spawn.knock_rating = def.knock_rating;
        spawn.impact_sound = def.impact_sound;
        spawn.cook = def.cook;
        carryables_.push_back(std::move(spawn));
    }
}

std::uint32_t Scene::LoadModel(const char* file) {
    return AddModel(LoadGltfModel(ExecutableDirectory() / "assets" / "models" / file));
}

std::uint32_t Scene::AddModel(Model model) {
    models_.push_back(std::move(model));
    return static_cast<std::uint32_t>(models_.size() - 1);
}

void Scene::AddInstance(std::uint32_t model, FXMMATRIX transform, XMFLOAT3 tint, float checker) {
    MeshInstance instance{};
    instance.model = model;
    XMStoreFloat4x4(&instance.transform, transform);
    instance.tint = tint;
    instance.checker = checker;
    instances_.push_back(instance);

    // One collider per primitive, from the bounds glTF already stored on the
    // POSITION accessor. No vertex is ever looked at. Decorative parts -- the
    // cooler's lid, which only laps over a body that is already the whole cooler
    // -- contribute nothing, or the player would stand on them.
    for (const Primitive& primitive : models_[model].primitives) {
        if (!primitive.collides) {
            continue;
        }
        const XMMATRIX to_world = XMLoadFloat4x4(&primitive.transform) * transform;
        colliders_.push_back(TransformBox(primitive.bounds, to_world));
    }
}

void Scene::AddDynamicInstance(std::uint32_t model, FXMMATRIX transform, XMFLOAT3 tint,
                               float mass, float knock_rating, ImpactSound impact_sound,
                               std::optional<HeatSource> heat, XMFLOAT3 heat_offset) {
    DynamicBody body{};
    body.instance = static_cast<std::uint32_t>(instances_.size());
    XMStoreFloat4x4(&body.initial_transform, transform);
    body.mass = mass;
    body.knock_rating = knock_rating;
    body.impact_sound = impact_sound;
    body.heat = heat;
    body.heat_offset = heat_offset;

    MeshInstance instance{};
    instance.model = model;
    XMStoreFloat4x4(&instance.transform, transform);
    instance.tint = tint;
    instance.checker = 0.0f;
    instances_.push_back(instance);

    // One box per colliding primitive, in the model's own space -- the instance
    // transform is left off here because the body carries it as its spawn pose, and
    // the shapes ride inside that. This is the same box-per-part decomposition
    // AddInstance would have pushed into the static world, only handed to the
    // dynamic body instead so nothing walls the object in place.
    for (const Primitive& primitive : models_[model].primitives) {
        if (!primitive.collides) {
            continue;
        }
        body.shapes.push_back(TransformBox(primitive.bounds, XMLoadFloat4x4(&primitive.transform)));
    }

    dynamic_bodies_.push_back(std::move(body));
}
