#pragma once

#include "rigid_body.hpp"

#include <cstdint>
#include <vector>

class Scene;
class Physics;

// The backyard's knock-over-able world objects -- the grill and the cooler --
// each a single dynamic PhysX rigid body so the player can shove or topple it by
// running into it. Before this they were part of the immovable static world.
//
// Each object is built from its model's own collider boxes so it tips and slides
// as one rigid piece: the grill is light and top-heavy (narrow feet, so it goes
// over), the cooler heavier and low (so it mostly gets pushed). The scene still
// owns their draw instances; Furniture owns the bodies and, every frame, reads
// each body's pose back into its instance so it renders and shadows wherever it
// has ended up. None of these are carryable, and their bodies carry no userData,
// which is what keeps Props' pick query from ever grabbing one.
class Furniture {
public:
    // Builds one dynamic body per Scene::DynamicBodies() entry, each spawned at the
    // instance's authored transform with its declared mass.
    Furniture(Scene& scene, Physics& physics);

    // Reads every body's current pose into its scene instance. Called after
    // Physics::Step advances the simulation and before the frame is drawn.
    void Update();

private:
    // One dynamic object: the shared bumpable body and the scene draw instance its
    // pose is written back into. bodies_ is reserved up front because the body's
    // tag is what its userData points at, and that address must stay valid.
    struct Body {
        RigidBody body;
        std::uint32_t instance;
    };

    Scene* scene_;
    std::vector<Body> bodies_;
};
