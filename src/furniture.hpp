#pragma once

#include "heat_source.hpp"
#include "rigid_body.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
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

    // Reads every body's current pose into its scene instance, and moves each heat
    // source's hot centre to follow the body it rides. Called after Physics::Step
    // advances the simulation and before the frame is drawn -- and before Props
    // updates, so the meats cook against this frame's heat, not last frame's.
    void Update();

    // The heat the furniture radiates this frame -- the grill's grate, its origin
    // already placed at wherever the grill has ended up. Handed to Props so the meats
    // set on the grill cook. Empty on a level with no hot furniture.
    std::span<const HeatSource> HeatSources() const { return heat_sources_; }

private:
    // One dynamic object: the shared bumpable body and the scene draw instance its
    // pose is written back into. bodies_ is reserved up front because the body's
    // tag is what its userData points at, and that address must stay valid.
    struct Body {
        RigidBody body;
        std::uint32_t instance;
    };

    // One heat emitter and what drives it: the HeatSource itself lives in the
    // contiguous heat_sources_ vector below (so HeatSources() can hand it out as a
    // span), while the body index and the grate's body-space offset that move its
    // origin each frame ride here in a parallel vector. Split so the exposed side
    // stays a clean run of HeatSource and the bookkeeping stays private.
    struct HeatDrive {
        std::size_t body;             // index into bodies_ whose pose carries the heat
        DirectX::XMFLOAT3 local_offset; // grate centre in that body's model space
    };

    Scene* scene_;
    std::vector<Body> bodies_;
    std::vector<HeatSource> heat_sources_;
    std::vector<HeatDrive> heat_drives_;
};
