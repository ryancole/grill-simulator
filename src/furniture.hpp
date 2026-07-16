#pragma once

#include "heat_source.hpp"
#include "rigid_body.hpp"
#include "scene.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

class Physics;
class Actions;

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

    // The player's one interaction with the furniture: standing the grill back up.
    // Aiming at the toppled grill base within reach and pressing Interact rights it --
    // it snaps back to its authored upright pose rather than being picked up (the heavy
    // furniture is never carried). Gaze-picked exactly like a prop pick-up, so this sets
    // the righting target for the prompt and the outline and, on the Interact edge, does
    // the righting. `hands_free` is false while a carryable is held -- E is then that
    // item's drop, so no righting is offered. A no-op on a level with no grill base or
    // while the grill still stands. Call once per frame, before Update reads the poses
    // back, so a right this frame shows this frame.
    void Interact(const DirectX::XMMATRIX& camera_to_world, const Actions& actions,
                  bool hands_free);

    // Reads every body's current pose into its scene instance, and moves each heat
    // source's hot centre to follow the body it rides. Called after Physics::Step
    // advances the simulation and before the frame is drawn -- and before Props
    // updates, so the meats cook against this frame's heat, not last frame's.
    void Update();

    // The heat the furniture radiates this frame -- the grill's grate, its origin
    // already placed at wherever the grill has ended up. Handed to Props so the meats
    // set on the grill cook. Empty on a level with no hot furniture.
    std::span<const HeatSource> HeatSources() const { return heat_sources_; }

    // The HUD hint for the righting action: "[E] Right the grill" while the toppled
    // grill base is looked at within reach and the hands are free, else empty. Recomputed
    // each Interact. Empty on any frame the grill stands or is not aimed at, which is how
    // the caller knows to fall back to the props' own prompt.
    std::string PromptText() const;

    // Zero or one instance -- the grill base at its current pose -- ringed by the same
    // outline the prop pick-up uses while it is the righting target this frame, so a
    // toppled grill an E press would stand up glows just like a grabbable prop. Empty
    // otherwise. Rebuilt each Interact.
    std::span<const MeshInstance> HighlightInstances() const { return highlight_; }

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

    // Whether the grill base is tipped far enough off vertical to count as knocked
    // over -- the condition that gates the righting prompt, since an upright grill has
    // nothing to right. Only called with a grill base present.
    bool GrillToppled() const;
    // Whether the player is aiming at the grill base within reach this frame: a sphere
    // swept down the gaze whose nearest furniture hit is the base. The filter keeps the
    // sweep to furniture bodies, so the static world and the player's own capsule (which
    // it starts inside) are ignored. Only called with a grill base present.
    bool AimingAtGrill(DirectX::FXMVECTOR eye, DirectX::FXMVECTOR forward) const;
    // Snaps the grill base back to its authored upright pose with zeroed velocity, so a
    // toppled grill stands up exactly where the level placed it -- and its grate heat with
    // it, since Update follows the pose. Only called with a grill base present.
    void RightGrill();

    Scene* scene_;
    Physics* physics_;
    std::vector<Body> bodies_;
    std::vector<HeatSource> heat_sources_;
    std::vector<HeatDrive> heat_drives_;

    // The bodies_ index of the grill base -- the one dynamic body the player can stand
    // back up -- or -1 on a level without one. Found in the constructor as the body
    // tagged ImpactSound::GrillBase. `grill_base_upright_` keeps its authored spawn pose
    // so righting restores it exactly.
    int grill_base_ = -1;
    DirectX::XMFLOAT4X4 grill_base_upright_{};

    // The grill base index while it is the righting target this frame (toppled, looked
    // at, hands free), or -1. Set by Interact; read by PromptText. The outline instance
    // below is built alongside it.
    int right_target_ = -1;
    std::vector<MeshInstance> highlight_;
};
