#pragma once

#include "furniture.hpp"
#include "objectives.hpp"
#include "props.hpp"
#include "scene.hpp"
#include "serve_zone.hpp"

#include <optional>

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
    Objectives& objectives() { return objectives_; }
    const Objectives& objectives() const { return objectives_; }
    // The level's turn-in zone, or nullptr on a level that set no `turn_in`. Props tests
    // the carried tray against it each frame; a null zone means the level cannot be turned
    // in (a sandbox). Stable for the level's lifetime, so a borrowed pointer is safe.
    const ServeZone* turn_in_zone() const { return turn_in_ ? &*turn_in_ : nullptr; }

private:
    // Declared before props_/furniture_ so it is built first (they take a Scene&)
    // and destroyed last.
    Scene scene_;
    Props props_;
    Furniture furniture_;
    // The level's win-condition tracker, seeded from its goals. Ordinary per-level
    // state -- it borrows nothing, so its position among the members is free.
    Objectives objectives_;
    // The level's turn-in zone, built from its `turn_in`, or empty on a sandbox level.
    std::optional<ServeZone> turn_in_;

    // The persistent systems this level's content was loaded into, so the
    // destructor can hand it all back.
    Renderer* renderer_;
    Physics* physics_;
};
