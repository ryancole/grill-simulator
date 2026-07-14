#include "scene.hpp"

#include "dx_common.hpp"
#include "level.hpp"

#include <cfloat>
#include <cmath>
#include <string>
#include <unordered_map>

using namespace DirectX;

Scene::Scene(const LevelDef& level) {
    cube_ = AddModel(MakeUnitCubeModel());

    // The carryable props load with every level -- the pick-up/carry system is
    // game-wide, not level content -- so the renderer uploads them once here. The
    // scene places no instances of them: Props sets out the starting handful and
    // owns every one thereafter.
    prop_models_.tongs = LoadModel("tongs-metal.glb");
    prop_models_.patty = LoadModel("burger-raw.glb");
    prop_models_.steak = LoadModel("steak-cooked.glb");

    // Build the level. Each placement becomes a draw instance plus either a static
    // collider (AddInstance) or a knock-over-able dynamic body (AddDynamicInstance),
    // exactly as the constructor used to spell out by hand. Models load on first
    // reference and are reused, so the two crates and the two trees each upload one
    // model; an empty name is the shared unit cube (ground, patio, fence).
    std::unordered_map<std::string, std::uint32_t> loaded;
    for (const Placement& placement : level.placements) {
        std::uint32_t model = cube_;
        if (!placement.model.empty()) {
            const auto [it, inserted] = loaded.try_emplace(placement.model, cube_);
            if (inserted) {
                it->second = LoadModel(placement.model.c_str());
            }
            model = it->second;
        }

        const XMMATRIX transform = XMLoadFloat4x4(&placement.transform);
        if (placement.dynamic) {
            // A placement that radiates heat becomes a HeatSource on its body; a cold
            // one passes nullopt and the body carries no heat. The origin is left
            // unset -- Furniture places it each frame from the body's pose.
            std::optional<HeatSource> heat;
            if (placement.emits_heat) {
                heat.emplace(placement.heat_temp_f, placement.heat_reach);
            }
            AddDynamicInstance(model, transform, placement.tint, placement.mass,
                               placement.knock_rating, placement.impact_sound, heat,
                               placement.heat_offset);
        } else {
            AddInstance(model, transform, placement.tint, placement.checker);
        }
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
