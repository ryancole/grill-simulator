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
        const std::size_t index = bodies_.size();
        Body& stored = bodies_.emplace_back();
        stored.instance = body.instance;
        // prop_index stays -1: furniture is shovable but never carried, so the
        // gaze-pick sweep skips it. The rating rides along for the shove, and the
        // impact sound so the contact report can voice a hard landing (the grill
        // clatters). Bind only now that `stored` sits in its final slot in bodies_.
        stored.body.Adopt(actor, body.knock_rating, -1, body.impact_sound);
        stored.body.Bind();

        // A body that radiates heat contributes a HeatSource, paired with the index
        // of the body whose pose moves it and the grate offset in that body's space.
        // Update places the origin every frame; it is left unset until then.
        if (body.heat) {
            heat_sources_.push_back(*body.heat);
            heat_drives_.push_back({index, body.heat_offset});
        }
    }
}

void Furniture::Update() {
    // Read each body's stepped pose back into its draw instance; the renderer (and
    // the sun's shadow) then follow the tumble for free.
    for (const Body& stored : bodies_) {
        scene_->SetInstanceTransform(stored.instance, stored.body.Transform());
    }

    // Move each heat source to where its body now sits: the grate offset, in the
    // body's model space, carried through the body's current pose. Knock the grill
    // over and its hot zone tips with it -- the heat is wherever the grate is.
    for (std::size_t i = 0; i < heat_sources_.size(); ++i) {
        const HeatDrive& drive = heat_drives_[i];
        const DirectX::XMMATRIX pose = bodies_[drive.body].body.Transform();
        const DirectX::XMVECTOR origin =
            DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&drive.local_offset), pose);
        heat_sources_[i].SetOrigin(origin);
    }
}
