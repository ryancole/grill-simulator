#pragma once

#include "collision.hpp"
#include "cook_information.hpp"
#include "flow_volume.hpp"
#include "heat_source.hpp"
#include "ignitable_requirements.hpp"
#include "rigid_body.hpp"
#include "scene.hpp"
#include "serve_zone.hpp"
#include "soft_body.hpp"
#include "wetness_information.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Actions;
class Flame;
class Fluid;
class Objectives;
class Physics;
class Renderer;

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
    // Frees the bodies that were removed from the physics scene (carried, gripped,
    // served, stacked) -- Physics::ClearLevel releases only what is still in the
    // scene, and an out-of-scene actor is otherwise leaked.
    ~Props();

    // Advances pick-up and drop for one frame and rebuilds the draw lists from the
    // current body poses. The simulation itself is stepped by Physics in the game
    // loop before this runs, so the poses read here are already current.
    // `camera_to_world` carries the eye's position in its fourth row and its gaze
    // in the third, so both the reach test and a carried object's pose come from
    // it. The Interact action, edge-triggered, is what grabs and drops. `dt` is the
    // frame time in seconds, which the cookable meats advance their cook on.
    // `heat_sources` are the yard's hot objects this frame (the grill's grate): each
    // meat cooks against the hottest air any of them imposes at where it sits, or room
    // air when none reaches it. Loading is against the serving tray, which is itself a
    // carryable Props owns: pressing Interact while holding a meat over the tray's live
    // serve zone places it -- any meat is accepted and sticks to the tray, unjudged, its
    // cook frozen at that band. The judging happens later: `turn_in` is the level's
    // static delivery zone (null on a sandbox level), and pressing Interact while
    // carrying the loaded tray inside it hands every stuck meat to `objectives` at once
    // and ends the level -- see TurnedIn. `fire_pit` is the level's fire-pit zone (null on
    // a level without one): a log held over it is stacked onto it by the primary action.
    // `fluid` is the session's GPU fluid, which the lighter-fluid can sprays into while the
    // primary action is held; the fluid has no consequence beyond the spray itself today.
    // Every burning log's fire is a volumetric NVIDIA Flow fire (see FlowEmitters). The
    // lighter's own muzzle flame is the exception: it burns as `flame`, a CPU particle
    // system, for as long as the primary action is held -- its little pilot tongue sits too
    // close to the eye for the Flow pass to show. Nothing lights the fire pit yet.
    void Update(const DirectX::XMMATRIX& camera_to_world, const Actions& actions, float dt,
                std::span<const HeatSource> heat_sources, const ServeZone* turn_in,
                const ServeZone* fire_pit, Objectives& objectives, Fluid* fluid, Flame* flame);

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

    // Registers every soft meat's deformable mesh with the renderer and records the
    // handles. Split out of the constructor because Props is built before the renderer
    // has the level's geometry, and a deformable mesh's material runs name primitives of
    // an uploaded model. Call once, after Renderer::LoadScene.
    void RegisterSoftMeshes(Renderer& renderer);

    // The deforming meats this frame: the skinned vertex stream of every soft body,
    // with the same browning tint the rigid draw would carry. Drawn by the scene pass
    // instead of the matching WorldInstances entry, never as well as -- an item with a
    // soft body is absent from world_.
    std::span<const SoftMeshInstance> SoftInstances() const { return soft_; }

    // The fire/smoke sources the burning props hand to NVIDIA Flow this frame -- one per
    // burning carryable: a caught log (an oriented box along the wood) and the lit lighter (a
    // small flame at its muzzle). Empty when nothing is alight. Rebuilt each Update; the
    // caller merges it with the furniture's grate fire before handing the lot to the renderer.
    std::span<const FlowEmitter> FlowEmitters() const { return flow_emitters_; }

    // The world-space positions of every ignitable carryable (the firewood logs), at their
    // current resting poses. The Flow simulation box is sized to include these so a log burns
    // as a Flow fire wherever it can catch -- in its pile as well as the fire pit -- not just
    // inside a box drawn around the pit. Read once on level load.
    std::vector<DirectX::XMFLOAT3> IgnitablePositions() const;

    // The heat the carryables radiate this frame -- a lit log, the struck lighter's flame
    // -- each already placed at its current hot centre by this frame's Update. Handed to
    // Furniture so the grill's grate, held under the lighter, can catch from the same flame
    // that lights the logs: the furniture lights its own bodies against these the way Props
    // lights its items against the furniture's heat. Rebuilt each Update; empty when nothing
    // carryable is hot.
    std::span<const HeatSource> ItemHeats() const { return item_heats_; }

    // The HUD hint for what the E key would do right now: "[E] Pick up tongs"
    // when a loose object is in reach and looked at, "[E] Drop" while carrying,
    // or empty when E would do nothing. Recomputed each Update.
    std::string PromptText() const;

    // Whether a carryable is in hand right now. The furniture's grill-righting reads this
    // to know the hands are busy -- while carrying, Interact is the held item's drop, so
    // no righting is offered. Reflects the state at the last Update.
    bool Carrying() const { return carried_ >= 0; }

    // Whether the loaded tray has been turned in at the level's delivery zone -- the one
    // discrete event that ends the level. Set once, when the player presses Interact while
    // carrying the tray inside the turn-in zone (Update hands the stuck meats to the
    // Objectives at that moment); the game loop polls this to raise the results screen.
    // Always false on a freshly loaded level, since World rebuilds Props from scratch.
    bool TurnedIn() const { return turned_in_; }

    // One entry per meat in the yard for the on-screen "meats" panel. `name` is the food's
    // type (the string Objectives keys on, uppercased for display by the caller), `band`
    // its current doneness band index, `temp_f` its internal temperature in whole degrees
    // Fahrenheit, and `served` whether it has already been handed off. A meat is any item
    // carrying cooking state; non-food carryables are skipped. Empty on a level that placed
    // no food.
    struct MeatStatus {
        std::string name;
        int band = 0;
        int temp_f = 0;
        bool served = false;
    };
    std::vector<MeatStatus> MeatStatuses() const;

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
    // every frame by the camera's basis. A carried body is REMOVED from the physics
    // scene so it neither falls nor shoves anything while it hangs in the hand, and
    // re-added on release. Removal, not eDISABLE_SIMULATION: that flag is not
    // supported by GPU dynamics -- it leaves a stale broadphase entry the GPU
    // pipeline crashes on when something moves through it.
    struct Item {
        // The models this item draws as it cooks, resolved to loaded ids and each
        // tagged with the doneness band it begins at. A tool or single-model food has
        // one stage, so CurrentModel never varies; a chicken has a raw and a cooked
        // stage, and its cook's doneness picks between them each frame. Always
        // non-empty; the first is the base drawn while raw.
        std::vector<CookStage> stages;
        std::string name; // As it reads in the pick-up prompt.

        // What the primary action does with this item in hand, from its catalog type.
        // Dispatched by TriggerAbility while it is the carried item; None does nothing.
        Ability ability = Ability::None;

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

        // Present on a meat that simulates as a deformable volume rather than a rigid
        // box -- so it squashes when it lands instead of merely tumbling. Null on the
        // tools, and null on a meat whose model would not tetrahedralize, which is a
        // property of the asset rather than a failure: such an item simply stays rigid
        // and SoftBody::Status() says why.
        //
        // While `soft` is set the item is drawn from the soft body's skinned vertices
        // (see SoftInstances) and NOT from world_, so it appears in exactly one draw
        // list. The rigid body stays alive beside it as a query-only shell: it is what
        // the pick-up gaze test sweeps and what carries the item's pose, since a
        // deformable volume has no pose to speak of.
        //
        // A food that swaps model as it cooks owns one body PER stage, each cooked from
        // that stage's own model, so the chicken deforms as the raw shape until medium
        // and as the cooked shape after -- a tet mesh cannot be re-skinned to a
        // different model, so the swap has to swap bodies. `soft` and `soft_mesh` name
        // the ACTIVE stage's body and renderer mesh; everything downstream -- the carry,
        // the proxy glue, the draw lists -- reads only those two and never learns the
        // twins exist. Inactive bodies sit parked outside the physics scene (unique_ptr
        // per stage because a SoftBody owns SDK handles and is neither copyable nor
        // movable; a null entry is a stage whose model would not cook).
        SoftBody* soft = nullptr;
        std::uint32_t soft_mesh = 0;
        std::vector<std::unique_ptr<SoftBody>> soft_stage_bodies;
        std::vector<std::uint32_t> soft_stage_meshes;
        std::size_t soft_stage = 0;

        // Set only on the serving tray: the delivery surface it carries. Props reads it
        // to place a live serve zone at the tray's current pose and to rest delivered
        // meat on it. Empty on foods and the tongs.
        std::optional<ServeDef> serve;

        // Set only on a heat-radiating carryable (the firewood log): the heat it gives
        // off, which may be off (unlit) to start. Its origin is refreshed each frame from
        // this item's pose plus `heat_offset` (the hot centre in model space), so the warm
        // zone rides the log wherever it is carried, set down or stacked in the pit.
        std::optional<HeatSource> heat;
        DirectX::XMFLOAT3 heat_offset{0.0f, 0.0f, 0.0f};

        // Set only on a carryable that can be set alight (the firewood log): what the air
        // around it has to do to light it. Empty -- the meats, the tools -- and it simply
        // never catches. There is no "is it lit" flag beside this: an ignited item is one
        // whose own `heat` is switched on, which is the whole point of catching fire.
        std::optional<IgnitableRequirements> ignitable;

        // Set on whatever can be soaked -- today the same ignitable logs, since the only
        // liquid in play (lighter fluid) matters by helping wood catch. Empty on the meats
        // and tools, which nothing wets yet. The spray deposits into this from the droplets
        // that land near the log (see Props::Update), it dries on its own each frame, and
        // the ignition loop reads it to rush a soaked log's catch. Presence marks a thing as
        // wettable, exactly as `ignitable` marks one as lightable; broaden the set by giving
        // more items one.
        std::optional<WetnessInformation> wetness;

        // Seconds this item has been alight, aged each frame while its heat is on. Drives the
        // Flow fire's build-up: a freshly caught log starts as a small flame and grows to a
        // full one over the first few seconds rather than flaring up at full size at once.
        float burn_time = 0.0f;

        // Set once a meat has been delivered onto a tray. A served meat is done with
        // play: its cook is frozen at the band it was served in and its body stays out
        // of the simulation. It is not a static display -- it is stuck to the tray it
        // was served on (`stuck_to`, `stuck_local`) and rides that tray wherever it goes.
        bool served = false;
        // The item index of the tray a served meat is stuck to, or -1. `stuck_local` is
        // its pose in that tray's own frame, so its world pose is stuck_local * tray pose
        // each frame -- delivered meat travels with the tray, resting or carried.
        int stuck_to = -1;
        DirectX::XMFLOAT4X4 stuck_local;

        // Set once a log has been stacked onto the fire pit. Like a served meat it is done
        // with play: its body stays out of the simulation (so nothing knocks it) and its
        // `resting` pose is fixed to the spot in the pit it was placed, so RebuildTransform
        // leaves it alone. Unlike served meat it rides nothing -- the pit does not move.
        bool in_fire_pit = false;

        // Whether the body is currently in the physics scene. False while carried,
        // gripped, served or stacked -- the body is scene-removed for those, and a
        // removed actor is invisible to Physics::ClearLevel's release sweep, so the
        // Props destructor frees whatever this still marks as out.
        bool in_simulation = true;
    };

    // `stages` are the item's cook-stage models (at least one); `pool` is the loaded
    // model pool they index -- the first stage's model cuts the physics box (the base
    // look, since the collider does not resize as the food swaps model), and each
    // stage's model seeds its own soft body. `cook` gives the item a cooking state
    // built from that food's profile; the tongs and any other non-food carryable pass
    // nullopt and simply never cook.
    void Add(std::vector<CookStage> stages, const std::vector<Model>& pool, std::string name,
             DirectX::XMFLOAT3 position, float yaw_degrees, DirectX::FXMMATRIX held_local,
             float knock_rating, ImpactSound impact_sound, std::optional<CookProfile> cook,
             std::optional<ServeDef> serve, Ability ability, std::optional<HeatSource> heat,
             DirectX::XMFLOAT3 heat_offset, std::optional<IgnitableRequirements> ignitable);
    // The air temperature imposed on `point` by everything hot this frame: the level's
    // furniture heat (the grill's grate) and every carryable's own (a lit log, a struck
    // lighter), whichever is hottest, or room air when nothing reaches. The one place the
    // "how hot is it here?" question is answered -- the cook and the ignition both ask it.
    float AmbientAt(DirectX::FXMVECTOR point, std::span<const HeatSource> heat_sources) const;
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
    // How wet an item looks, 0 (dry) to 1 (soaked): its wetness saturation if it can be
    // doused, else zero. The draw passes hand this to the instance so a soaked log shows
    // the wet sheen (see MeshInstance::wetness and scene.hlsl).
    static float ItemWetness(const Item& item);
    // The model to draw this frame: the cook stage with the highest `from` band the
    // item has reached (the base while raw, or for a non-food that never cooks). So a
    // chicken shows its raw model until it hits medium, then its cooked one.
    static std::uint32_t CurrentModel(const Item& item);
    // The index into `stages` behind CurrentModel: the stage with the highest `from`
    // band the cook has reached. Split out so the soft-body swap can compare stages
    // by index rather than by model id.
    static std::size_t CurrentStage(const Item& item);
    // The item the player is looking at within reach, or -1. A sphere swept down
    // the gaze picks the nearest prop it meets; the query is filtered to prop
    // bodies, so the static world and the player's own capsule are ignored.
    int PickTarget(DirectX::FXMVECTOR eye, DirectX::FXMVECTOR forward) const;
    // Releases the carried item back into the simulation: it re-enters at the
    // exact pose it was held, takes a gentle toss along the gaze, and falls from
    // there.
    void Drop(DirectX::FXMMATRIX camera_to_world);
    // Re-enters body `index` into the simulation at `pose_world` (a model-to-world
    // transform) and reads its pose back. With `toss` it leaves the hand along the gaze
    // with a forward tumble (a thrown drop); without it, it is placed in situ with no
    // velocity and simply falls from there -- how the tongs set a meat down exactly where
    // the jaws held it, precise where the hand's throw is not. The shared tail of dropping
    // the carried item and releasing a gripped one; the caller clears whichever slot held it.
    void ReleaseBody(int index, DirectX::FXMMATRIX pose_world, DirectX::FXMMATRIX camera_to_world,
                     bool toss);
    // Fires the carried item's primary-action ability. Dispatches on the item's
    // Ability (from its catalog type): the tongs (GripMeat) clamp or release a meat,
    // None does nothing. `camera_to_world` places a gripped meat and aims a release.
    // Only ever called with a valid carried index.
    void TriggerAbility(int item, DirectX::FXMMATRIX camera_to_world);
    // Loads the carried meat `meat` onto tray item `tray`, unjudged. The meat is marked
    // served (its cook frozen), stuck to the tray -- its pose stored in the tray's frame,
    // scattered across the face so several spread out -- and released from the hand. Any
    // meat is accepted; whether its doneness fills an order is decided later, at turn-in.
    void Load(int meat, int tray);
    // Stacks the carried log `log` onto the fire pit centred at `pit_center`: snaps it to
    // a stacked pose above the pit (nestled over any logs already there), leaves its body
    // out of the simulation so nothing can knock it, marks it placed, and empties the hand.
    void PlaceInFirePit(int log, DirectX::XMFLOAT3 pit_center);
    // Turns in tray item `tray`: hands every meat stuck to it to `objectives` at once
    // (each doneness matched against the open orders) and marks the level turned in. The
    // one place the win condition is evaluated -- called when the loaded tray is carried
    // into the turn-in zone and Interact is pressed.
    void TurnIn(int tray, Objectives& objectives);
    // The world-space model-to-world transform an item is drawn and posed under right
    // now: the held pose while carried, otherwise its resting pose. Used to place a
    // tray's serve zone and to hang served meat off it, resting or in hand.
    DirectX::XMMATRIX CurrentPose(int index, DirectX::FXMMATRIX camera_to_world) const;
    // Where the lighter's flame stands, in the item's own model space: the far face of
    // its box down the model's Z. Derived from the box rather than authored in the
    // catalog because two things need the very same point -- the flame is emitted here
    // and the lighter's heat radiates from here -- and a fire whose heat came from
    // somewhere else would be a lie. Only meaningful for a Flame item: the lighter is a
    // long-barrelled utility one, modelled lying down its Z with the grip at the origin,
    // so its far Z face is the muzzle (its Y is the barrel's centimetre of thickness).
    DirectX::XMVECTOR NozzleLocal(const Item& item) const;
    // Takes item `index`'s body out of the physics scene -- the "held in hand /
    // done with play" state. The GPU-safe replacement for eDISABLE_SIMULATION;
    // ReleaseBody is the inverse.
    void RemoveBodyFromScene(int index);

    Physics* physics_;
    std::vector<Item> items_;
    int carried_ = -1; // index into items_, or -1
    int hovered_ = -1; // item in reach and looked at this frame, or -1
    // The meat clamped in the carried tongs' jaws, or -1. Carried alongside the tongs
    // (both out of the simulation): it rides a grip pose in front of the eye, cooks
    // there, and is let go by the tongs' primary action or when the tongs are dropped.
    int gripped_ = -1;
    // The meat the carried tongs would grip this frame -- a meat in reach and looked at
    // while the jaws are empty -- or -1. Cached in Update so the (const) prompt and the
    // highlight can read it, and so the primary action grips the same one the outline
    // ringed. Only set while carrying the tongs with nothing yet gripped.
    int grip_target_ = -1;
    // The primary action's current key name ("Mouse1"), cached each Update so the const
    // prompt can name the real binding for the tongs' grab/release hint.
    std::string primary_label_;
    // The tray item the carried meat is held over this frame (its serve zone contains
    // the meat), or -1. Cached in Update so the (const) prompt can read it -- it drives
    // the "[E] Place on tray" hint and is what the Interact press loads onto.
    int serve_tray_ = -1;
    // Whether the carried item is the tray and it sits inside the level's turn-in zone
    // this frame. Cached in Update so the (const) prompt can offer "[E] Turn in" and the
    // Interact press knows to hand the tray in rather than drop it.
    bool in_turn_in_ = false;
    // Whether the carried item is a fire-pit log and it sits inside the level's fire-pit
    // zone this frame, with the pit's centre to stack it at. Cached in Update so the same
    // read drives the "[Mouse1] Add log to fire pit" prompt and the primary-action press.
    bool log_over_pit_ = false;
    DirectX::XMFLOAT3 fire_pit_center_{0.0f, 0.0f, 0.0f};
    // Whether the tray has been turned in -- latched true by the turn-in press and read
    // by TurnedIn(). Ends the level; never cleared (the level reloads to play again).
    bool turned_in_ = false;

    // The spray's emission bank: droplets owed but not yet whole at this frame's rate,
    // carried so a fast frame still spits a steady stream. Reset the moment the button
    // is up, so a new hold never starts with a stale fraction.
    float spray_carry_ = 0.0f;

    // Rebuilt each Update: every resting item, the carried one, and the one the
    // outline glows around.
    std::vector<MeshInstance> world_;
    std::vector<MeshInstance> held_;
    std::vector<MeshInstance> highlight_;
    // Rebuilt each Update beside world_: the soft meats' skinned streams. See
    // SoftInstances.
    std::vector<SoftMeshInstance> soft_;
    // Rebuilt each Update: a Flow fire/smoke source for every ignitable carryable currently
    // alight. See FlowEmitters().
    std::vector<FlowEmitter> flow_emitters_;
    // Rebuilt each Update: every carryable's live heat source, each with its hot centre
    // already placed for this frame. Handed to Furniture (see ItemHeats()) so a held flame
    // can light the grill as well as a log. A plain snapshot -- ownership stays with the items.
    std::vector<HeatSource> item_heats_;
};
