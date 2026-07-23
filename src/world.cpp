#include "world.hpp"

#include "level.hpp"
#include "physics.hpp"
#include "renderer.hpp"

#include <algorithm>
#include <limits>

namespace {
// How the loading bar splits across the stages with per-item granularity, weighted
// by measured wall time (Debug, both levels): the Scene's model loading takes about
// a quarter of a load, and the Props seeding -- where every meat's FEM meshes cook --
// about two thirds. The GPU stages reported from the body are quick and share the tail.
constexpr float kProgressSceneEnd = 0.25f;
constexpr float kProgressPropsEnd = 0.9f;
} // namespace

World::World(const LevelDef& level, Renderer& renderer, Physics& physics,
             const std::function<void(float)>& progress)
    // Scene first: Props and Furniture both read its models and instances. Building
    // them here (in the member list) mirrors the old startup order, where the props
    // and furniture bodies were created before the static world was added below.
    // The Scene's per-model fractions and the Props' per-carryable ones are folded
    // down into their spans of the overall load; the body reports the stages after.
    : scene_(level,
             [&progress](float fraction) {
                 if (progress) {
                     progress(fraction * kProgressSceneEnd);
                 }
             }),
      props_(scene_, physics,
             [&progress](float fraction) {
                 if (progress) {
                     progress(kProgressSceneEnd +
                              fraction * (kProgressPropsEnd - kProgressSceneEnd));
                 }
             }),
      furniture_(scene_, physics),
      objectives_(level.goals),
      renderer_(&renderer),
      physics_(&physics) {
    const auto report = [&](float fraction) {
        if (progress) {
            progress(fraction);
        }
    };
    // Hand the freshly built Scene to the persistent systems. Aim the sun and set
    // the sky first, so the first frame is lit and coloured the level's way; then
    // upload the geometry, and drop the immovable colliders
    // into the physics scene the props and furniture already registered their bodies
    // with.
    renderer.SetSunDirection(level.sun_direction);
    renderer.SetEnvironment(level.environment);
    // Hand over the level's grass field, or clear any the last level left, so the grass
    // pass grows this level's -- and only this level's. A no-op in effect where the
    // device has no mesh shaders.
    if (level.grass) {
        const GrassDef& g = *level.grass;
        // The footprints the grass must skip so it does not poke up through anything: the
        // static world's colliders (the patio, fence, benches, crates and tree trunks are
        // all here) plus the knock-over bodies at their spawn pose (the grill and cooler).
        // The renderer filters these by height -- the ground plane and the tree canopies
        // fall out -- so hand it everything and let it keep what reaches into the sward.
        std::vector<OrientedBox> obstacles = scene_.Colliders();
        for (const DynamicBody& body : scene_.DynamicBodies()) {
            const DirectX::XMMATRIX pose = DirectX::XMLoadFloat4x4(&body.initial_transform);
            for (const OrientedBox& shape : body.shapes) {
                // The body's collider shapes are in its own space; carry each to where the
                // body spawns. They are authored axis-aligned, so its model-space bound is
                // its centre +/- half-extents, which TransformBox turns into a world box.
                const Aabb local{{shape.center.x - shape.half_extents.x,
                                  shape.center.y - shape.half_extents.y,
                                  shape.center.z - shape.half_extents.z},
                                 {shape.center.x + shape.half_extents.x,
                                  shape.center.y + shape.half_extents.y,
                                  shape.center.z + shape.half_extents.z}};
                obstacles.push_back(TransformBox(local, pose));
            }
        }
        renderer.SetGrass({g.center, g.size, g.color, g.blade_height, g.blade_width, g.wind},
                          obstacles);
    } else {
        renderer.ClearGrass();
    }
    renderer.LoadScene(scene_);
    report(0.95f); // The whole GPU upload, BLASes and TLAS included.
    // The meats' deformable meshes name primitives of the models just uploaded, so they
    // are registered only now, once the renderer has them.
    props_.RegisterSoftMeshes(renderer);
    // And only now can the raytracing geometry be closed out: those meshes carry index
    // buffers of their own, which the reflection hit shader needs entries for.
    renderer.FinalizeRaytracingGeometry();
    report(0.98f); // What follows is CPU-cheap: colliders, zones, the Flow box.
    physics.AddStaticWorld(scene_.Colliders());

    // Build the turn-in zone from the level's `turn_in`, if it set one. A static column
    // like a ServeZone, it never moves, so it is made once here from the placed position.
    if (level.turn_in) {
        turn_in_.emplace(DirectX::XMLoadFloat3(&level.turn_in->pos), level.turn_in->radius);
    }
    // Likewise the fire-pit zone: a static column over the pit where logs are stacked.
    if (level.fire_pit) {
        fire_pit_.emplace(DirectX::XMLoadFloat3(&level.fire_pit->pos), level.fire_pit->radius);
    }

    // Box the Flow smoke simulation around this level's fires. They can burn at the furniture
    // grates (the grill) and the fire pit (stacked logs), which sit in different spots from
    // one level to the next -- the backyard grill at the centre, the campsite pit off to the
    // side -- so the sim box is placed here rather than baked into the renderer. Update the
    // furniture first so its heat origins are at their spawn poses, not the origin.
    furniture_.Update();
    float min_x = std::numeric_limits<float>::max(), max_x = std::numeric_limits<float>::lowest();
    float min_z = min_x, max_z = max_x;
    bool any_fire = false;
    const auto include = [&](float x, float z) {
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_z = std::min(min_z, z);
        max_z = std::max(max_z, z);
        any_fire = true;
    };
    for (const HeatSource& hot : furniture_.HeatSources()) {
        const DirectX::XMFLOAT3 origin = hot.Origin();
        include(origin.x, origin.z);
    }
    if (level.fire_pit) {
        include(level.fire_pit->pos.x, level.fire_pit->pos.z);
    }
    // The ignitable carryables (the firewood logs) too: a log can catch and burn where it
    // lies -- in its pile, not just once stacked in the pit -- and its fire is a Flow fire,
    // so the sim box has to reach the pile as well or the log would burn invisibly there.
    for (const DirectX::XMFLOAT3& log : props_.IgnitablePositions()) {
        include(log.x, log.z);
    }
    if (any_fire) {
        // One box per level, centred on the fires' spread and sized to take in the whole
        // level around them, so a fire -- or the carried lighter -- shows wherever it is,
        // not just inside a box drawn around the pit. A generous minimum half keeps even a
        // single-fire level (the backyard grill) roomy; the grid's high virtual resolution
        // (see flow_volume.cpp) keeps the fire fine despite the large box. Lifted in y so the
        // box runs from the ground up to where the smoke thins.
        const float half = std::max(0.5f * std::max(max_x - min_x, max_z - min_z) + 2.0f, 12.0f);
        renderer.SetFlowRegion({0.5f * (min_x + max_x), 2.5f, 0.5f * (min_z + max_z)}, half);
    }
    report(1.0f);
}

World::~World() {
    // Give the level's content back to the systems that outlive it, in the reverse
    // of loading: the physics actors first (a CPU-side strip), then the renderer's
    // geometry (which drains the GPU before freeing). The Scene, Props and Furniture
    // members then destruct after this body -- their RigidBody handles are non-owning,
    // so ClearLevel having already released the actors is correct, not a double free.
    physics_->ClearLevel();
    renderer_->ReleaseScene();
}
