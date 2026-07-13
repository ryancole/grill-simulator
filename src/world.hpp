#pragma once

#include "furniture.hpp"
#include "props.hpp"
#include "scene.hpp"

struct LevelDef;
class Physics;
class Renderer;

// One loaded level, alive for exactly as long as that level is current. It owns
// the per-level trio -- the Scene (what stands where), the Props (loose carryables)
// and the Furniture (knock-over-able bodies) -- and, on either side of its own
// lifetime, the level-shaped state that actually lives in the persistent systems:
// the renderer's uploaded geometry and the physics scene's actors.
//
// Constructing a World loads a level: it builds the Scene from the LevelDef,
// uploads it to the renderer, drops the static colliders into the physics scene,
// and seeds the props and furniture. Destroying one unloads it: the renderer frees
// that geometry and the physics scene is stripped of the level's actors. So a level
// swap is just `world.reset()` then a fresh World -- the Renderer, Physics, Camera
// and the rest of the session outlive it untouched.
//
// The player's controller is not in here: it belongs to the session (Camera owns
// it, Physics::ClearLevel spares it). A caller respawns the player separately after
// loading.
class World {
public:
    World(const LevelDef& level, Renderer& renderer, Physics& physics);
    ~World();

    World(const World&) = delete;
    World& operator=(const World&) = delete;

    Scene& scene() { return scene_; }
    const Scene& scene() const { return scene_; }
    Props& props() { return props_; }
    Furniture& furniture() { return furniture_; }

private:
    // Declared before props_/furniture_ so it is built first (they take a Scene&)
    // and destroyed last.
    Scene scene_;
    Props props_;
    Furniture furniture_;

    // The persistent systems this level's content was loaded into, so the
    // destructor can hand it all back.
    Renderer* renderer_;
    Physics* physics_;
};
