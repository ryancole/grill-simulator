#include "furniture.hpp"

#include "physics.hpp"
#include "scene.hpp"

// No <PxPhysicsAPI.h>: with the body wrapped in a RigidBody, Furniture only ever
// holds the actor as an opaque handle -- creating it, adopting it, reading its
// transform -- and never calls a PhysX method itself.

Furniture::Furniture(Scene& scene, Physics& physics) : scene_(&scene) {
    // Reserve so no push_back reallocates: each body's userData points at the tag
    // inside its RigidBody, and that address has to stay put for the session.
    bodies_.reserve(scene.DynamicBodies().size());
    for (const DynamicBody& body : scene.DynamicBodies()) {
        physx::PxRigidDynamic* actor =
            physics.AddDynamicBody(body.shapes, body.initial_transform, body.mass);
        Body& stored = bodies_.emplace_back();
        stored.instance = body.instance;
        // prop_index stays -1: furniture is shovable but never carried, so the
        // gaze-pick sweep skips it. The rating rides along for the shove, and the
        // impact sound so the contact report can voice a hard landing (the grill
        // clatters). Bind only now that `stored` sits in its final slot in bodies_.
        stored.body.Adopt(actor, body.knock_rating, -1, body.impact_sound);
        stored.body.Bind();
    }
}

void Furniture::Update() {
    // Read each body's stepped pose back into its draw instance; the renderer (and
    // the sun's shadow) then follow the tumble for free.
    for (const Body& stored : bodies_) {
        scene_->SetInstanceTransform(stored.instance, stored.body.Transform());
    }
}
