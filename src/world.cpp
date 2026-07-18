#include "world.hpp"

#include "level.hpp"
#include "physics.hpp"
#include "renderer.hpp"

#include <algorithm>
#include <limits>

World::World(const LevelDef& level, Renderer& renderer, Physics& physics)
    // Scene first: Props and Furniture both read its models and instances. Building
    // them here (in the member list) mirrors the old startup order, where the props
    // and furniture bodies were created before the static world was added below.
    : scene_(level),
      props_(scene_, physics),
      furniture_(scene_, physics),
      objectives_(level.goals),
      renderer_(&renderer),
      physics_(&physics) {
    // Hand the freshly built Scene to the persistent systems. Aim the sun and set
    // the sky first, so the reflection probe LoadScene captures is lit and coloured
    // the level's way; then upload the geometry, and drop the immovable colliders
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
    if (any_fire) {
        // Centre the box on the fires' spread, sized to hold it with a couple of metres of
        // margin, never below a minimum so a single fire still gets a roomy box for its
        // plume. Lifted in y so the box runs from the ground up to where the smoke thins.
        const float half = std::max(0.5f * std::max(max_x - min_x, max_z - min_z) + 2.0f, 4.0f);
        renderer.SetFlowRegion({0.5f * (min_x + max_x), 2.5f, 0.5f * (min_z + max_z)}, half);
    }
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
