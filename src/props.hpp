#pragma once

#include "collision.hpp"
#include "cook_information.hpp"
#include "heat_source.hpp"
#include "rigid_body.hpp"
#include "scene.hpp"
#include "serve_zone.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Actions;
class Objectives;
class Physics;

// The loose objects in the yard the player can pick up, carry and set back down:
// a pair of tongs, a steak, a couple of patties. Each is an instance of a prop
// model the Scene loaded, backed by a PhysX rigid body. Props owns the bodies
// and the one-at-a-time "held" state; the yard itself stays static.
//
// The falling, tumbling and stacking are PhysX's job now -- Props just seeds each
// body, drives the carried one, and reads poses back into the draw list. The
// hand-rolled impulse solver that used to live here is gone.
class Props {
public:
    // Takes the whole scene, not just the model ids, because each prop's box
    // collider is derived from its model's vertex bounds (Primitive::bounds), and
    // those live on the Models the scene loaded. `physics` is where the bodies
    // are created and stepped.
    Props(const Scene& scene, Physics& physics);

    // Advances pick-up and drop for one frame and rebuilds the draw lists from the
    // current body poses. The simulation itself is stepped by Physics in the game
    // loop before this runs, so the poses read here are already current.
    // `camera_to_world` carries the eye's position in its fourth row and its gaze
    // in the third, so both the reach test and a carried object's pose come from
    // it. The Interact action, edge-triggered, is what grabs and drops. `dt` is the
    // frame time in seconds, which the cookable meats advance their cook on.
    // `heat_sources` are the yard's hot objects this frame (the grill's grate): each
    // meat cooks against the hottest air any of them imposes at where it sits, or
    // room air when none reaches it. `serve_zones` are the level's delivery counters:
    // pressing Interact while carrying a meat held over one delivers it -- `objectives`
    // decides whether that cook fills an open order, and an accepted meat is set down on
    // the counter as a static display while a rejected one stays in hand.
    void Update(const DirectX::XMMATRIX& camera_to_world, const Actions& actions, float dt,
                std::span<const HeatSource> heat_sources, std::span<const ServeZone> serve_zones,
                Objectives& objectives);

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

    // One line per meat in the yard for the debug overlay: "steak: medium rare
    // (139F)" -- its name, current doneness band, and internal temperature. A meat
    // is any item carrying cooking state; the tongs and other non-food carryables
    // have none and are skipped, so this is exactly the cookable objects, resting or
    // carried. Empty on a level that placed no food.
    std::vector<std::string> MeatDebugLines() const;

private:
    // One loose object, modelled as a single oriented box (an approximation --
    // the meshes are not perfectly boxy, but these things are small).
    //
    // The PhysX body's frame is the model's own frame: its origin sits on the
    // object's underside, and the box shape is offset up to `com_offset`, so the
    // body's global pose is exactly the model-to-world transform the renderer
    // draws under. `resting` is that pose, cached each frame for the draw list,
    // PickTarget and the highlight to share.
    //
    // `held_local` is the pose in eye space while carried, lifted into the world
    // every frame by the camera's basis. A carried body is taken out of the
    // simulation (eDISABLE_SIMULATION) so it neither falls nor shoves anything
    // while it hangs in the hand.
    struct Item {
        // The models this item draws as it cooks, resolved to loaded ids and each
        // tagged with the doneness band it begins at. A tool or single-model food has
        // one stage, so CurrentModel never varies; a chicken has a raw and a cooked
        // stage, and its cook's doneness picks between them each frame. Always
        // non-empty; the first is the base drawn while raw.
        std::vector<CookStage> stages;
        std::string name; // As it reads in the pick-up prompt.

        // Box shape, in the model's own space. `half_extents` are half the box's
        // side lengths; `com_offset` is the centre of the box measured from the
        // model origin, which sits on the object's underside -- so com_offset.y is
        // roughly half the object's height.
        DirectX::XMFLOAT3 half_extents;
        DirectX::XMFLOAT3 com_offset;

        // The shared bumpable body: the PhysX actor (owned by the scene, not by
        // Item) plus the knock tag its userData points at. items_ is reserved up
        // front so that tag address stays stable.
        RigidBody rigid;

        // Rebuilt from the body's pose each frame: the model-to-world transform
        // the renderer draws this item under.
        DirectX::XMFLOAT4X4 resting;

        DirectX::XMFLOAT4X4 held_local;

        // Present only on the meats: their cooking state, ticked each frame and read
        // for the browning tint and the pick-up prompt. The tongs leave it empty --
        // they are not food, so there is nothing to cook.
        std::optional<CookInformation> cook;

        // Set once a meat has been delivered to a serving zone. A served item is done
        // with play: it rests at the counter as a static display -- still drawn, but no
        // longer cooked, picked up, or highlighted -- so its cook is frozen at the band
        // it was served in and its body stays out of the simulation.
        bool served = false;
    };

    // `stages` are the item's cook-stage models (at least one); `base_model` is the
    // model the physics box is cut from -- the base look, since the collider does not
    // resize as the food swaps model. `cook` gives the item a cooking state built from
    // that food's profile; the tongs and any other non-food carryable pass nullopt and
    // simply never cook.
    void Add(std::vector<CookStage> stages, const Model& base_model, std::string name,
             DirectX::XMFLOAT3 position, float yaw_degrees, DirectX::FXMMATRIX held_local,
             float knock_rating, ImpactSound impact_sound, std::optional<CookProfile> cook);
    // Fills an item's box shape (half_extents, com_offset) from the union of its
    // model's primitive bounds. PhysX derives the mass and inertia from the shape.
    static void DeriveBodyShape(Item& item, const Model& model);
    // Creates the PhysX body for `item` at `initial_pose` (a model-to-world
    // transform), attaches its box shape offset to com_offset, and lets PhysX
    // compute the mass properties from the shape. Returns the actor for the caller
    // to hand to the item's RigidBody.
    physx::PxRigidDynamic* CreateBody(const Item& item, DirectX::FXMMATRIX initial_pose);
    // Recomputes `resting` from the body's current global pose, so drawing and
    // picking see where the body actually is.
    static void RebuildTransform(Item& item);
    // The colour to draw an item under: a cooking meat browns with its doneness,
    // everything else stays white (its model's own colours, unchanged).
    static DirectX::XMFLOAT3 ItemTint(const Item& item);
    // The model to draw this frame: the cook stage with the highest `from` band the
    // item has reached (the base while raw, or for a non-food that never cooks). So a
    // chicken shows its raw model until it hits medium, then its cooked one.
    static std::uint32_t CurrentModel(const Item& item);
    // The item the player is looking at within reach, or -1. A sphere swept down
    // the gaze picks the nearest prop it meets; the query is filtered to prop
    // bodies, so the static world and the player's own capsule are ignored.
    int PickTarget(DirectX::FXMVECTOR eye, DirectX::FXMVECTOR forward) const;
    // Releases the carried item back into the simulation: it re-enters at the
    // exact pose it was held, takes a gentle toss along the gaze, and falls from
    // there.
    void Drop(DirectX::FXMMATRIX camera_to_world);
    // Attempts to deliver the carried meat `index` to `zone`, asking `objectives`
    // whether its current doneness fills an open order. On acceptance the item is
    // marked served, set down as a static display on the counter, and released from the
    // hand; on rejection nothing changes and it stays carried. Returns whether it was
    // served, so the caller can fall back to a plain drop when it was not.
    bool Serve(int index, const ServeZone& zone, Objectives& objectives);

    Physics* physics_;
    std::vector<Item> items_;
    int carried_ = -1; // index into items_, or -1
    int hovered_ = -1; // item in reach and looked at this frame, or -1
    // The serve zone the carried meat is held over this frame, or -1. Cached in Update
    // so the (const) prompt can read it -- it drives the "[E] Serve" hint and is what
    // the Interact press serves into.
    int serve_zone_ = -1;
    // Whether the carried meat's current cook would be accepted at that zone this frame,
    // and -- when it would not -- the doneness the outstanding order still wants, as
    // ready-made text ("medium well to well done") or empty if no order needs this type.
    // Both are computed in Update against the Objectives, so the const prompt can explain
    // an about-to-be-rejected serve rather than silently doing nothing.
    bool serve_ok_ = false;
    std::string serve_need_;

    // Rebuilt each Update: every resting item, the carried one, and the one the
    // outline glows around.
    std::vector<MeshInstance> world_;
    std::vector<MeshInstance> held_;
    std::vector<MeshInstance> highlight_;
};
