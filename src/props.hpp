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
    explicit Props(const PropModels& models);

    // Advances pick-up and drop for one frame from the E key. `camera_to_world`
    // carries the eye's position in its fourth row and its gaze in the third, so
    // both the reach test and a carried object's pose come from it. `colliders`
    // is what a dropped object comes to rest on.
    void Update(const DirectX::XMMATRIX& camera_to_world, const Input& input,
                std::span<const Aabb> colliders);

    // The objects resting in the yard, drawn in the world pass under the world's
    // sun. Excludes whatever is currently carried.
    std::span<const MeshInstance> WorldInstances() const { return world_; }
    // The carried object, if any -- zero or one instance, already lifted into
    // world space in front of the eye. Drawn in the viewmodel pass over the
    // cleared depth buffer, so a wall never slices it, exactly like the arms.
    std::span<const MeshInstance> HeldInstances() const { return held_; }

    // The HUD hint for what the E key would do right now: "[E] Pick up tongs"
    // when a loose object is in reach and looked at, "[E] Drop" while carrying,
    // or empty when E would do nothing. Recomputed each Update.
    std::string PromptText() const;

private:
    // One loose object. `resting` is its world transform when set down;
    // `held_local` is its pose in eye space while carried, lifted into the world
    // every frame by the camera's basis.
    struct Item {
        std::uint32_t model;
        DirectX::XMFLOAT4X4 resting;
        DirectX::XMFLOAT4X4 held_local;
        std::string name; // As it reads in the pick-up prompt.
    };

    void Add(std::uint32_t model, std::string name, DirectX::XMFLOAT3 position, float yaw_degrees,
             DirectX::FXMMATRIX held_local);
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

    // Rebuilt each Update: every resting item, and the carried one.
    std::vector<MeshInstance> world_;
    std::vector<MeshInstance> held_;
};
