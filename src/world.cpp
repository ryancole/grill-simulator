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
      renderer_(&renderer),
      physics_(&physics) {
    // Hand the freshly built Scene to the persistent systems. Aim the sun first, so
    // the reflection probe LoadScene captures is lit the level's way; then upload
    // the geometry, and drop the immovable colliders into the physics scene the
    // props and furniture already registered their bodies with.
    renderer.SetSunDirection(level.sun_direction);
    renderer.LoadScene(scene_);
    physics.AddStaticWorld(scene_.Colliders());
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
