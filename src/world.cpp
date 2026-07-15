#include "world.hpp"

#include "level.hpp"
#include "physics.hpp"
#include "renderer.hpp"

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
        renderer.SetGrass({g.center, g.size, g.color, g.blade_height, g.blade_width, g.wind});
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
