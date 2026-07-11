#pragma once

#include "collision.hpp"
#include "scene.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

class Input;

// The loose objects in the yard the player can pick up, carry and set back down:
// a pair of tongs, a steak, a couple of patties. Each is an instance of a prop
// model the Scene loaded; Props owns their transforms and the one-at-a-time
// "held" state, so the yard itself stays static.
//
// There is no collider on any of them. A dropped steak is something to walk
// through, not to trip over, and a carried one must never wedge the player into
// a wall.
class Props {
public:
    // Takes the whole scene, not just the model ids, because each prop's box
    // collider is derived from its model's vertex bounds (Primitive::bounds), and
    // those live on the Models the scene loaded.
    explicit Props(const Scene& scene);

    // Advances pick-up, drop and the falling of loose objects for one frame. `dt`
    // is the frame time the rigid-body step integrates over. `camera_to_world`
    // carries the eye's position in its fourth row and its gaze in the third, so
    // both the reach test and a carried object's pose come from it. `colliders`
    // is what a dropped object falls onto and comes to rest on.
    void Update(const DirectX::XMMATRIX& camera_to_world, const Input& input,
                std::span<const Aabb> colliders, float dt);

    // The objects resting in the yard, drawn in the world pass under the world's
    // sun. Excludes whatever is currently carried.
    std::span<const MeshInstance> WorldInstances() const { return world_; }
    // The carried object, if any -- zero or one instance, already lifted into
    // world space in front of the eye. Drawn in the viewmodel pass over the
    // cleared depth buffer, so a wall never slices it, exactly like the arms.
    std::span<const MeshInstance> HeldInstances() const { return held_; }
    // The object in reach and looked at this frame -- zero or one instance, the
    // same placement it draws at in the world pass. The renderer rings it with a
    // glowing outline so the player sees which one an E press would grab. Empty
    // while carrying, since nothing is being aimed at then.
    std::span<const MeshInstance> HighlightInstances() const { return highlight_; }

    // The HUD hint for what the E key would do right now: "[E] Pick up tongs"
    // when a loose object is in reach and looked at, "[E] Drop" while carrying,
    // or empty when E would do nothing. Recomputed each Update.
    std::string PromptText() const;

private:
    // One loose object, modelled as a single oriented box (an approximation --
    // the meshes are not perfectly boxy, but these things are small). The rigid
    // state is the box centred on its centre of mass; the render transform,
    // `resting`, is rebuilt from it every frame so the draw list, PickTarget and
    // the highlight all read the same pose.
    //
    // `held_local` is the pose in eye space while carried, lifted into the world
    // every frame by the camera's basis -- carrying is kinematic, so the solver
    // leaves a carried item alone.
    struct Item {
        std::uint32_t model;
        std::string name; // As it reads in the pick-up prompt.

        // Box shape, in the model's own space. `half_extents` are half the box's
        // side lengths; `com_offset` is the centre of the box measured from the
        // model origin, which sits on the object's underside -- so com_offset.y is
        // roughly half the object's height.
        DirectX::XMFLOAT3 half_extents;
        DirectX::XMFLOAT3 com_offset;

        // Rigid-body state. `position` is the centre of mass in world space;
        // `orientation` is the box's rotation as a unit quaternion.
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT4 orientation;
        DirectX::XMFLOAT3 linear_velocity{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 angular_velocity{0.0f, 0.0f, 0.0f};

        // Mass and the diagonal of the inverse inertia tensor, both in body space.
        // Precomputed from the box dimensions; the solver in Phase 2 reads them.
        float inv_mass{1.0f};
        DirectX::XMFLOAT3 inv_inertia{0.0f, 0.0f, 0.0f};

        // A settled object is skipped by the solver until something disturbs it,
        // which is both cheaper and what keeps a resting box from jittering.
        bool asleep{true};

        // Rebuilt from position/orientation/com_offset each frame: the model-to-
        // world transform the renderer draws this item under.
        DirectX::XMFLOAT4X4 resting;

        DirectX::XMFLOAT4X4 held_local;
    };

    void Add(std::uint32_t model_id, const Model& model, std::string name,
             DirectX::XMFLOAT3 position, float yaw_degrees, DirectX::FXMMATRIX held_local);
    // Fills an item's box shape (half_extents, com_offset) and mass properties
    // (inv_mass, inv_inertia) from the union of its model's primitive bounds.
    static void DeriveBodyShape(Item& item, const Model& model);
    // Recomputes `resting` from the item's rigid state, so drawing and picking see
    // where the body currently is.
    static void RebuildTransform(Item& item);
    // Steps every awake item forward by dt: gravity, integration, collision
    // against `colliders`. A no-op in Phase 1, where everything spawns asleep.
    void Simulate(float dt, std::span<const Aabb> colliders);
    // The item the player is looking at within reach, or -1. Nearest to the
    // centre of the gaze wins.
    int PickTarget(DirectX::FXMVECTOR eye, DirectX::FXMVECTOR forward) const;
    // Lays the carried item on the ground in front of the player, on whatever
    // surface is under that spot.
    void Drop(DirectX::FXMVECTOR eye, DirectX::FXMVECTOR forward,
              std::span<const Aabb> colliders);

    std::vector<Item> items_;
    int carried_ = -1; // index into items_, or -1
    int hovered_ = -1; // item in reach and looked at this frame, or -1
    bool interact_was_down_ = false;

    // Rebuilt each Update: every resting item, the carried one, and the one the
    // outline glows around.
    std::vector<MeshInstance> world_;
    std::vector<MeshInstance> held_;
    std::vector<MeshInstance> highlight_;
};
