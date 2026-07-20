#include "props.hpp"

#include "renderer.hpp"

#include "actions.hpp"
#include "flame.hpp"
#include "fluid.hpp"
#include "objectives.hpp"
#include "physics.hpp"

#include <PxPhysicsAPI.h>

#include <algorithm>
#include <cfloat>

using namespace DirectX;
using namespace physx;

namespace {

// The prop models carry their own colours, so the instance that places one wants
// white -- exactly as the yard's glTF furniture does.
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};

// How far the player can reach to grab, and the radius of the sphere swept down
// the gaze to find what they are aiming at. The sphere is a forgiving aim -- a
// small object on the ground need not be pixel-centred -- standing in for the old
// alignment cone.
constexpr float kReach = 2.4f;
constexpr float kPickRadius = 0.2f;

// Rough density used to turn a box's volume into a mass. The absolute value
// barely matters -- gravity is mass-independent, and a rest on the infinite-mass
// world does not depend on it either -- but it keeps the inertia sensibly scaled
// for when props knock into each other.
constexpr float kDensity = 500.0f; // kg/m^3, a bit lighter than water.

// The toss imparted when an object leaves the hand: forward along the gaze, a
// touch down, and a little pitch so it turns over instead of sliding flat.
constexpr float kThrowSpeed = 1.5f;  // m/s along the gaze.
constexpr float kThrowDrop = 0.5f;   // m/s downward bias.
constexpr float kThrowSpin = 2.5f;   // rad/s of forward tumble.

// The lighter fluid's stream: how many droplets a second of held spray emits, and
// how fast each leaves the nozzle. The rate is banked across frames (spray_carry_),
// so it holds at any frame time; the speed gives the stream a metre or two of
// throw before gravity takes it.
constexpr float kSprayPerSecond = 260.0f;
constexpr float kSpraySpeed = 7.5f; // m/s out of the nozzle.

// How the sprayed fluid soaks what it lands on. A droplet counts as wetting an ignitable
// when its centre falls within kWetRadius metres of that thing's hot centre -- roughly a
// log's own girth, so the stream has to actually be on the log, not merely in the yard.
// Every such droplet deposits kWetPerDropletSecond of saturation per second it lingers
// there, so a stream held on a log soaks it over a second or two while a glancing pass
// barely dampens it; WetnessInformation caps the total at fully soaked. Tuning-sensitive
// against the spray rate and droplet lifetime -- a feel pass will want to revisit both.
constexpr float kWetRadius = 0.3f;
constexpr float kWetPerDropletSecond = 0.02f;

// A lit log's tint: multiplied into the log model's own browns, it pushes the wood
// toward glowing ember orange -- the visible difference between a cold stack and a
// burning one. The red channel rides above one so the HDR pipeline blooms it a touch.
constexpr XMFLOAT3 kEmberTint{1.35f, 0.62f, 0.28f};

// A caught log's Flow fire builds up rather than flaring to full size the instant it
// catches: over these seconds its emitter grows from a small starting flame to the full
// fire, so ignition reads as a fire taking hold, not an immediate bonfire.
constexpr float kFireBuildupSeconds = 6.0f;

// A carried object hangs by the right hand -- see the viewmodel's wrist, which
// sits near (0.27, -0.35, 0.78) in this same eye space. Flat things are tipped
// up so the player sees a face rather than an edge; the tongs are turned to
// point away down the gaze.
XMMATRIX FlatInHand() {
    return XMMatrixRotationX(-0.6f) * XMMatrixTranslation(0.22f, -0.34f, 0.82f);
}

XMMATRIX TongsInHand() {
    return XMMatrixRotationY(-XM_PIDIV2) * XMMatrixRotationX(-0.15f) *
           XMMatrixTranslation(0.16f, -0.30f, 0.52f);
}

// A tray is carried out in front, a little lower and further than the food pose and
// tipped toward the eye (like the flat food, but gentler) so its top face -- and
// anything served onto it -- reads rather than showing only its thin edge. Kept within
// the downward view angle so it sits in frame, not below the screen. Tuned by eye; nudge
// if the plate rides too high, too low, or too steep.
XMMATRIX TrayInHand() {
    return XMMatrixRotationX(-0.4f) * XMMatrixTranslation(0.0f, -0.38f, 0.85f);
}

// Where a meat clamped in the tongs rides: just past the jaws, out along the gaze and
// tipped up so a face reads, a touch ahead of where TongsInHand puts the tips. Shared by
// the draw, the cook sample and the release, so the meat is drawn, cooked and let go at
// one consistent spot. Tuned by eye like the hand poses above; nudge if the meat floats
// off the tips.
XMMATRIX TongsGripPose() {
    return XMMatrixRotationX(-0.5f) * XMMatrixTranslation(0.17f, -0.32f, 0.78f);
}

// The in-hand pose a carryable's catalog hold style asks for. Props owns the two
// poses; the catalog just names which one.
XMMATRIX HoldFor(HoldStyle hold) {
    switch (hold) {
    case HoldStyle::Tongs:
        return TongsInHand();
    case HoldStyle::Tray:
        return TrayInHand();
    case HoldStyle::Flat:
        break;
    }
    return FlatInHand();
}

MeshInstance MakeInstance(std::uint32_t model, FXMMATRIX transform, XMFLOAT3 tint = kWhite,
                          float wetness = 0.0f) {
    MeshInstance instance{};
    instance.model = model;
    XMStoreFloat4x4(&instance.transform, transform);
    instance.tint = tint;
    instance.checker = 0.0f;
    instance.wetness = wetness;
    return instance;
}

// A rotation-and-translation model-to-world matrix as a PhysX rigid transform.
// The props never scale, so the rotation is read straight back out as a
// quaternion.
PxTransform ToPxTransform(FXMMATRIX m) {
    XMFLOAT3 p;
    XMFLOAT4 q;
    XMStoreFloat3(&p, m.r[3]);
    XMStoreFloat4(&q, XMQuaternionRotationMatrix(m));
    return PxTransform(PxVec3(p.x, p.y, p.z), PxQuat(q.x, q.y, q.z, q.w));
}

// A scene-query filter that keeps only the carryable props. Every shovable body
// carries a BodyTag in userData, but only a prop's has prop_index >= 0; the yard's
// static actors and the player's own capsule -- which the gaze sweep starts
// *inside*, so it would otherwise be the nearest hit every frame -- leave userData
// null, and the heavy furniture (grill, cooler) tags itself with prop_index -1.
// Both are rejected here, so the sweep reports the first grabbable prop along the
// gaze rather than the body the eye sits in or a cooler in the way.
struct PropQueryFilter : PxQueryFilterCallback {
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape*, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        const auto* tag = static_cast<const BodyTag*>(actor->userData);
        return (tag != nullptr && tag->prop_index >= 0) ? PxQueryHitType::eBLOCK
                                                        : PxQueryHitType::eNONE;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*,
                                    const PxRigidActor*) override {
        return PxQueryHitType::eBLOCK; // Never invoked: ePOSTFILTER is not requested.
    }
};

} // namespace

Props::Props(const Scene& scene, Physics& physics) : physics_(&physics) {
    const std::vector<Model>& pool = scene.Models();
    const std::vector<CarryableSpawn>& spawns = scene.Carryables();

    // Reserve so no push_back reallocates: each body's userData points at the tag
    // stored inside its Item, and that address has to stay put for the session.
    items_.reserve(spawns.size());

    // The starting objects are the level's carryables, already joined to their catalog
    // types by Scene: each hands over its loaded model, where it starts, how it is
    // held, how it lands, and -- for a food -- how it cooks (nullopt leaves the tongs
    // inert). Placed a hair above the ground in the files so physics settles them flat.
    for (const CarryableSpawn& spawn : spawns) {
        // The base (raw) model cuts the physics box; the whole stage list rides along
        // so the item can swap look as it cooks.
        Add(spawn.models, pool[spawn.models.front().model], spawn.name, spawn.pos, spawn.yaw,
            HoldFor(spawn.hold), spawn.knock_rating, spawn.impact_sound, spawn.cook, spawn.serve,
            spawn.ability, spawn.heat, spawn.heat_offset, spawn.ignitable);
    }
}

void Props::DeriveBodyShape(Item& item, const Model& model) {
    // Union every primitive's vertex bounds, each carried through its own node
    // transform, into one model-space box. glTF hands us those bounds directly
    // (Primitive::bounds), so no vertex is touched here.
    XMVECTOR box_min = XMVectorReplicate(FLT_MAX);
    XMVECTOR box_max = XMVectorReplicate(-FLT_MAX);
    for (const Primitive& primitive : model.primitives) {
        const Aabb bounds = TransformBounds(primitive.bounds, XMLoadFloat4x4(&primitive.transform));
        box_min = XMVectorMin(box_min, XMLoadFloat3(&bounds.min));
        box_max = XMVectorMax(box_max, XMLoadFloat3(&bounds.max));
    }

    // Floor every extent so a perfectly flat mesh (or, defensively, a model with
    // no primitives at all) still gives a non-degenerate box: a valid PhysX shape
    // and finite mass. The stored half-extents use the same floored value, so the
    // collider and the mass PhysX derives from it agree.
    XMFLOAT3 h;
    XMStoreFloat3(&h, XMVectorMax(XMVectorScale(XMVectorSubtract(box_max, box_min), 0.5f),
                                  XMVectorReplicate(1e-3f)));
    const XMVECTOR center = XMVectorScale(XMVectorAdd(box_max, box_min), 0.5f);
    item.half_extents = h;
    XMStoreFloat3(&item.com_offset, center);
}

PxRigidDynamic* Props::CreateBody(const Item& item, FXMMATRIX initial_pose) {
    PxPhysics& sdk = physics_->Sdk();

    PxRigidDynamic* body = sdk.createRigidDynamic(ToPxTransform(initial_pose));
    PxShape* shape = PxRigidActorExt::createExclusiveShape(
        *body, PxBoxGeometry(item.half_extents.x, item.half_extents.y, item.half_extents.z),
        physics_->DefaultMaterial());
    // The model origin sits on the underside; lift the box to com_offset so the
    // shape wraps the object and PhysX places the centre of mass there. The body
    // frame then coincides with the model frame.
    shape->setLocalPose(PxTransform(PxVec3(item.com_offset.x, item.com_offset.y, item.com_offset.z)));
    PxRigidBodyExt::updateMassAndInertia(*body, kDensity);

    physics_->Scene().addActor(*body);
    return body;
}

Props::~Props() {
    // The bodies still in the physics scene are the scene's to release (see
    // Physics::ClearLevel); the ones removed for carrying, gripping, serving or
    // stacking are invisible to that sweep and freed here.
    for (Item& item : items_) {
        if (!item.in_simulation && item.rigid.actor() != nullptr) {
            item.rigid.actor()->release();
        }
    }
}

void Props::RemoveBodyFromScene(int index) {
    // Out of the scene entirely: no gravity, no collisions, no query hits, and --
    // unlike PxActorFlag::eDISABLE_SIMULATION, which GPU dynamics does not support
    // (a stale broadphase entry lingers and the GPU pipeline access-violates when
    // something moves through it) -- safe on both pipelines. ReleaseBody re-adds.
    physics_->Scene().removeActor(*items_[index].rigid.actor());
    items_[index].in_simulation = false;
}

void Props::RebuildTransform(Item& item) {
    // The body frame is the model frame, so its global pose *is* the model-to-world
    // transform, which the RigidBody hands back directly.
    XMStoreFloat4x4(&item.resting, item.rigid.Transform());
}

XMFLOAT3 Props::ItemTint(const Item& item) {
    // Anything that has caught fire glows ember orange -- the one visible cue a thing is
    // burning, wherever it lies. Keyed on being ignitable rather than on lying in the pit:
    // a log lit on the grass is just as alight as one in the fire, and a log is not the
    // only thing here whose heat is on -- the lighter's is too, while it burns, and a
    // lighter is the thing doing the lighting, not a thing that caught.
    if (item.ignitable && item.heat && item.heat->IsOn()) {
        return kEmberTint;
    }
    // Raw meat's tint is white, so this leaves uncooked food and the non-cooking
    // props drawing exactly as their models do; only cooking browns the colour.
    return item.cook ? item.cook->SurfaceTint() : kWhite;
}

float Props::ItemWetness(const Item& item) {
    return item.wetness ? item.wetness->Wetness() : 0.0f;
}

std::uint32_t Props::CurrentModel(const Item& item) {
    // Pick the stage with the highest `from` band the cook has reached. A non-food has
    // no cook, so it reads as Raw and shows its single base stage. The scan does not
    // assume the stages are sorted -- it takes the greatest matching band outright --
    // so the browning tint still layers on top of whichever model this returns.
    const int band = static_cast<int>(item.cook ? item.cook->DonenessBand()
                                                 : CookInformation::Doneness::Raw);
    std::uint32_t model = item.stages.front().model;
    int best = -1;
    for (const CookStage& stage : item.stages) {
        const int from = static_cast<int>(stage.from);
        if (from <= band && from > best) {
            best = from;
            model = stage.model;
        }
    }
    return model;
}

void Props::Add(std::vector<CookStage> stages, const Model& base_model, std::string name,
                XMFLOAT3 position, float yaw_degrees, FXMMATRIX held_local, float knock_rating,
                ImpactSound impact_sound, std::optional<CookProfile> cook,
                std::optional<ServeDef> serve, Ability ability, std::optional<HeatSource> heat,
                XMFLOAT3 heat_offset, std::optional<IgnitableRequirements> ignitable) {
    Item item{};
    item.stages = std::move(stages);
    item.name = std::move(name);
    item.ability = ability;
    XMStoreFloat4x4(&item.held_local, held_local);
    if (cook) {
        item.cook.emplace(*cook);
    }
    item.serve = serve;
    item.heat = heat;
    item.heat_offset = heat_offset;
    item.ignitable = ignitable;
    // A thing that can be lit can be doused: give every ignitable a wetness so the spray has
    // something to soak and the ignition below has something to read. It starts bone dry.
    if (ignitable) {
        item.wetness.emplace();
    }
    DeriveBodyShape(item, base_model);

    // Seed the body so the model origin lands at `position`, yawed -- the same
    // placement the old yaw-and-translate gave.
    const XMMATRIX pose = XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                          XMMatrixTranslation(position.x, position.y, position.z);
    // Adopt the new actor with this item's index (so a gaze-sweep hit decodes back
    // to an item) and its knock rating (so the shove knows how hard it is to move).
    const int index = static_cast<int>(items_.size());
    item.rigid.Adopt(CreateBody(item, pose), knock_rating, index, impact_sound);
    RebuildTransform(item);

    // A meat also simulates as a deformable volume, so it squashes where a rigid box
    // could only tumble. It is cooked from the same model the rigid draw uses and seeded
    // at the same pose, and it may quietly fail -- an asset that will not tetrahedralize
    // leaves this null and the item stays rigid, which is a property of the model rather
    // than an error worth stopping a level load for. The rigid body stays either way:
    // it is what the gaze test sweeps and what carries the item's pose.
    if (item.cook) {
        XMFLOAT4X4 seed;
        XMStoreFloat4x4(&seed, pose);
        auto body = std::make_unique<SoftBody>(*physics_, base_model, seed, 1.0f);
        if (body->Active()) {
            item.soft = std::move(body);
        }
    }

    // Bind userData only after the item is settled in items_, since it points at
    // the tag living inside the RigidBody -- items_ is reserved so it never moves.
    items_.push_back(std::move(item));
    items_.back().rigid.Bind();
}

void Props::RegisterSoftMeshes(Renderer& renderer) {
    for (Item& item : items_) {
        if (item.soft == nullptr) {
            continue;
        }
        // The runs name primitives of the item's base model, which is the one the soft
        // body was cooked from -- a food that swaps mesh as it cooks keeps deforming the
        // shape it started as.
        item.soft_mesh = renderer.CreateDeformableMesh(
            item.stages.front().model, item.soft->SkinnedIndices(), item.soft->SkinnedPrimitives(),
            static_cast<UINT>(item.soft->SkinnedVertices().size()));
    }
}

void Props::Update(const XMMATRIX& camera_to_world, const Actions& actions, float dt,
                   std::span<const HeatSource> heat_sources, const ServeZone* turn_in,
                   const ServeZone* fire_pit, Objectives& objectives, Fluid* fluid,
                   Flame* flame) {
    // The camera-to-world matrix is right, up, forward, eye as its four rows.
    const XMVECTOR eye = camera_to_world.r[3];
    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);

    // Which tray the carried meat hangs over this frame, if any. Each tray's serve zone
    // rides its current pose, so it is rebuilt from the tray each frame -- a tray set
    // down on the bench takes food where it sits. Computed before the Interact press so
    // the same read drives both the load and the prompt. Any meat loads (the cook is not
    // judged here, only at turn-in), so no acceptance is precomputed. Only a meat loads;
    // the tongs carry no cook.
    serve_tray_ = -1;
    if (carried_ >= 0 && items_[carried_].cook) {
        const XMVECTOR held = (XMLoadFloat4x4(&items_[carried_].held_local) * camera_to_world).r[3];
        for (int t = 0; t < static_cast<int>(items_.size()); ++t) {
            if (!items_[t].serve || t == carried_) {
                continue;
            }
            const XMMATRIX tray = CurrentPose(t, camera_to_world);
            const XMVECTOR center = XMVector3Transform(XMLoadFloat3(&items_[t].serve->offset), tray);
            if (ServeZone(center, items_[t].serve->radius).Contains(held)) {
                serve_tray_ = t;
                break;
            }
        }
    }

    // Whether the carried item is the tray and it sits inside the level's static turn-in
    // zone this frame -- the delivery point. Tested at the tray's own origin (the zone is
    // a horizontal column, so its height is irrelevant), and only when a turn-in zone
    // exists. Cached so the same read drives the "[E] Turn in" prompt and the press below.
    in_turn_in_ = false;
    if (turn_in != nullptr && carried_ >= 0 && items_[carried_].serve) {
        const XMVECTOR tray_origin = CurrentPose(carried_, camera_to_world).r[3];
        in_turn_in_ = turn_in->Contains(tray_origin);
    }

    // Whether the carried item is a fire-pit log inside the level's fire-pit zone this
    // frame -- tested at the held log's position, like the turn-in above, and only when a
    // pit exists and the log's ability is to stack there. Its centre is cached alongside so
    // the primary-action press stacks onto the pit without re-reading the zone.
    log_over_pit_ = false;
    if (fire_pit != nullptr && carried_ >= 0 &&
        items_[carried_].ability == Ability::StackInFirePit) {
        const XMVECTOR held = (XMLoadFloat4x4(&items_[carried_].held_local) * camera_to_world).r[3];
        if (fire_pit->Contains(held)) {
            log_over_pit_ = true;
            fire_pit_center_ = fire_pit->Origin();
        }
    }

    // The meat the tongs would grip this frame: with the tongs in hand and their jaws
    // empty, the meat in reach and looked at. Only a meat qualifies -- a served one, the
    // tray or another tool is skipped -- so the primary action clamps food and nothing
    // else. Computed before the press so the same read drives the grip, the prompt and
    // the outline. The gaze sweep already excludes the disabled carried/gripped bodies.
    grip_target_ = -1;
    if (carried_ >= 0 && items_[carried_].ability == Ability::GripMeat && gripped_ < 0) {
        const int target = PickTarget(eye, forward);
        if (target >= 0 && target != carried_ && items_[target].cook && !items_[target].served) {
            grip_target_ = target;
        }
    }
    // The primary action's key name, cached for the (const) prompt so its grab/release
    // hint names the real binding rather than a hardcoded key.
    primary_label_ = actions.KeyName(Action::PrimaryAction);

    // Edge-triggered: one grab per press, so holding Interact does not pick up and
    // drop on alternate frames. Actions latches the press for us, so this is a
    // single event even while the key is held down.
    if (actions.WasPressed(Action::Interact)) {
        if (carried_ >= 0) {
            if (serve_tray_ >= 0) {
                // Holding a meat over a tray, Interact loads rather than drops. Any meat
                // is accepted, unjudged; it sticks to the tray and the hand empties. To
                // drop a meat over a tray, step off the tray first.
                Load(carried_, serve_tray_);
            } else if (in_turn_in_) {
                // Carrying the loaded tray inside the turn-in zone, Interact hands it in:
                // every stuck meat is matched against the orders and the level ends. To
                // set the tray down here without turning in, step out of the zone first.
                TurnIn(carried_, objectives);
            } else {
                Drop(camera_to_world);
            }
        } else {
            const int target = PickTarget(eye, forward);
            if (target >= 0) {
                // Lift it out of the simulation while it hangs in the hand: it
                // neither falls nor shoves anything until it is dropped.
                RemoveBodyFromScene(target);
                carried_ = target;
            }
        }
    }

    // The primary action fires the held item's ability -- what left mouse does depends
    // on what is in hand. Edge-triggered like the grab, and gated on carrying, so an
    // empty hand does nothing and a held button fires once. Separate from Interact, so
    // acting on an item and dropping it are distinct presses.
    if (actions.WasPressed(Action::PrimaryAction) && carried_ >= 0) {
        TriggerAbility(carried_, camera_to_world);
    }

    // The lighter fluid sprays for as long as the primary action is held -- a stream,
    // not the edge-triggered single fire above. Droplets leave the top of the held can
    // and fly down the gaze; the emission is banked fractionally so the stream's rate
    // is steady whatever the frame time. Nothing sprays without the GPU fluid (see
    // Fluid::Active) -- the can still clicks, it just squirts nothing.
    if (fluid != nullptr && fluid->Active() && carried_ >= 0 &&
        items_[carried_].ability == Ability::SprayFluid &&
        actions.IsActive(Action::PrimaryAction)) {
        spray_carry_ += kSprayPerSecond * dt;
        const int count = static_cast<int>(spray_carry_);
        if (count > 0) {
            spray_carry_ -= static_cast<float>(count);
            // The nozzle sits at the top of the can: the held pose's origin lifted up
            // the can's own Y by its height (the model origin is on the underside and
            // com_offset.y is half the height), nudged a touch down the gaze so the
            // stream clears the can's body.
            const XMMATRIX held =
                XMLoadFloat4x4(&items_[carried_].held_local) * camera_to_world;
            const XMVECTOR nozzle =
                XMVectorAdd(XMVectorAdd(held.r[3],
                                        XMVectorScale(XMVector3Normalize(held.r[1]),
                                                      items_[carried_].com_offset.y * 2.0f)),
                            XMVectorScale(forward, 0.06f));
            XMFLOAT3 origin;
            XMFLOAT3 aim;
            XMStoreFloat3(&origin, nozzle);
            XMStoreFloat3(&aim, forward);
            fluid->Spray(origin, aim, kSpraySpeed, count);
        }
    } else {
        spray_carry_ = 0.0f;
    }

    // The lighter is hot for as long as the primary action is held, like the can's spray
    // above and unlike the edge-triggered abilities: its heat switches on with the flame
    // and off when the button is released, so it warms (and lights) what it is held to only
    // while it burns. Only the flag is set here -- the fire itself is drawn below, in the
    // one pass that shows every burning thing. Set before the ignition check further down,
    // so a log held under the flame feels the lighter's heat this frame. Every Flame item
    // is visited, not just the carried one, so a lighter dropped mid-burn goes cold rather
    // than lying in the dirt radiating.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Item& item = items_[i];
        if (item.ability != Ability::Flame || !item.heat) {
            continue;
        }
        const bool burning = i == carried_ && actions.IsActive(Action::PrimaryAction);
        item.heat->SetOn(burning);
    }

    // What the prompt reports this frame: nothing to pick while carrying, else
    // whatever is in reach and looked at. Cheap enough to recompute outright for
    // a handful of items.
    hovered_ = carried_ >= 0 ? -1 : PickTarget(eye, forward);

    // Read each uncarried body's stepped pose back into its render transform.
    // Physics has already advanced the scene this frame, so these are current. A
    // served item is skipped: its body is out of the simulation and its resting pose
    // was set to the counter when it was delivered, so there is nothing to read back.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i != carried_ && i != gripped_ && !items_[i].served && !items_[i].in_fire_pit) {
            RebuildTransform(items_[i]);
        }
    }

    // The meat clamped in the tongs rides the grip pose in front of the eye. Keep its
    // resting current from there (its body is out of the simulation, so nothing else
    // moves it), so the cook samples heat where the jaws hold it and any pose read is
    // right. Done alongside the served-rides-tray step below, before the cook loop.
    if (gripped_ >= 0) {
        XMStoreFloat4x4(&items_[gripped_].resting, TongsGripPose() * camera_to_world);
    }

    // Served meat rides the tray it was delivered onto: its world pose is its stored
    // pose in the tray's frame times wherever that tray is now. Done after the transforms
    // above so the tray's resting pose is current, and CurrentPose covers a tray that is
    // itself being carried -- so a plate of served food travels in the hand with it.
    for (Item& item : items_) {
        if (item.served && item.stuck_to >= 0) {
            const XMMATRIX tray = CurrentPose(item.stuck_to, camera_to_world);
            XMStoreFloat4x4(&item.resting, XMLoadFloat4x4(&item.stuck_local) * tray);
        }
    }

    // Move each heat-radiating carryable's hot centre to where it sits this frame -- its
    // current pose (held, or its resting/stacked pose) times the offset up into the log --
    // so the warm zone rides the log as it is carried, set down or stacked in the pit. Like
    // the furniture's grill, done before the cook loop so meats read this frame's positions.
    item_heats_.clear();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (items_[i].heat) {
            const XMMATRIX pose = CurrentPose(i, camera_to_world);
            // The lighter is the exception to the authored offset: its heat comes off its
            // flame, which stands at the muzzle the model itself defines (see
            // NozzleLocal), so the two cannot be spelled separately without drifting
            // apart. Everything else radiates from the point its catalog entry names.
            const XMVECTOR local = items_[i].ability == Ability::Flame
                                       ? NozzleLocal(items_[i])
                                       : XMLoadFloat3(&items_[i].heat_offset);
            items_[i].heat->SetOrigin(XMVector3Transform(local, pose));
            // Snapshot it, hot centre and all, for Furniture to light the grill against
            // (see ItemHeats()) -- the same flame that catches a log catches the grate.
            item_heats_.push_back(*items_[i].heat);
        }
    }

    // Soak whatever the spray is landing on, and dry everything wet a little. A droplet wets
    // an ignitable when it comes to rest near that thing's hot centre -- the same point the
    // ignition below warms -- so aiming the stream at a log is what douses it, and where the
    // fluid actually falls is what gets wet (a stream that overshoots into the dirt soaks
    // nothing). Drying runs every frame regardless of the spray, so a volatile puddle left
    // unlit evaporates on its own. Placed after the hot-centre refresh above, so the target
    // point is where the log sits this frame, and before the ignition below, so this frame's
    // soaking feeds this frame's catch.
    const std::span<const XMFLOAT4> droplets =
        (fluid != nullptr && fluid->Active()) ? fluid->Points() : std::span<const XMFLOAT4>{};
    const float wet_radius_sq = kWetRadius * kWetRadius;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Item& item = items_[i];
        if (!item.wetness) {
            continue;
        }
        // Dry first, then take this frame's deposit, so a droplet arriving now is not
        // evaporated the instant it lands.
        item.wetness->Update(dt);
        if (droplets.empty()) {
            continue;
        }
        // The wet centre is the log's hot centre when it has one (already placed this frame),
        // else its body origin -- so this holds for anything wettable, not just the logs.
        XMVECTOR centre;
        if (item.heat) {
            const XMFLOAT3 origin = item.heat->Origin();
            centre = XMLoadFloat3(&origin);
        } else {
            centre = CurrentPose(i, camera_to_world).r[3];
        }
        int nearby = 0;
        for (const XMFLOAT4& drop : droplets) {
            const XMVECTOR delta = XMVectorSubtract(XMVectorSet(drop.x, drop.y, drop.z, 0.0f), centre);
            if (XMVectorGetX(XMVector3LengthSq(delta)) <= wet_radius_sq) {
                ++nearby;
            }
        }
        if (nearby > 0) {
            item.wetness->Wet(WetAgent::LighterFluid,
                              kWetPerDropletSecond * static_cast<float>(nearby) * dt);
        }
    }

    // Light anything ignitable the air has got hot enough around: hold the lighter's flame
    // to a log and it catches, and once caught it is a heat source itself -- which is the
    // whole point, since that is what cooks the food over it and what lights the next log.
    // Igniting is one-way and needs no flag of its own: an item is alight exactly when its
    // own heat is on, so a lit one is skipped and nothing here ever puts a fire out.
    //
    // After the origin refresh above (so every source is where it is this frame) and before
    // the cook below, so a log that catches this frame warms the food over it this frame.
    // Fire still spreads -- a lit log is a heat source the next one reads -- but no longer
    // in a single pass: each log has to heat through to its own ignition temperature first
    // (see IgnitableRequirements), so a stack catches progressively over seconds rather
    // than flashing over the instant the first log lights.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Item& item = items_[i];
        if (!item.ignitable || !item.heat || item.heat->IsOn()) {
            continue;
        }
        // Warm it toward the air at its own heat centre -- the middle of the round, which
        // the origin refresh above has already placed in the world. The point that will
        // radiate once it catches is the point that has to get hot, which is both the
        // symmetric rule and the fair one: an item's model origin sits on its *underside*,
        // so asking there would measure the air at the spot furthest from a flame held
        // over it. It catches once its own temperature has climbed past the threshold, not
        // the instant the air does -- so the flame has to be held on it, and a flame taken
        // away too soon lets it cool back down.
        const XMFLOAT3 hot_centre = item.heat->Origin();
        const XMVECTOR point = XMLoadFloat3(&hot_centre);
        const float ambient = AmbientAt(point, heat_sources);
        // Lighter fluid soaked in is an accelerant with no patience: the instant a flame hot
        // enough to catch this wood reaches a doused log, it goes up at once -- no slow
        // heat-through -- and straight to a full flame, skipping the ember-to-fire build-up
        // (burn_time below), the way fluid-soaked wood whooshes alight. It still only lights
        // off heat genuinely hot enough to catch it (ambient at or past the ignite temp), so
        // a soaked log by the too-cool grate still never catches and nothing self-ignites.
        // Water, when it exists, would read the same wetness and slow the catch instead.
        const bool fluid_soaked = item.wetness && item.wetness->IsWet() &&
                                  item.wetness->Agent() == WetAgent::LighterFluid;
        if (fluid_soaked && ambient >= item.ignitable->IgniteTempF()) {
            item.heat->SetOn(true);
            item.burn_time = kFireBuildupSeconds; // full flame from frame one, no build-up
        } else {
            item.ignitable->Update(ambient, dt);
            if (item.ignitable->Ignited()) {
                item.heat->SetOn(true);
            }
        }
    }

    // Every burning thing this frame is a volumetric NVIDIA Flow fire -- one emitter per
    // alight carryable, keyed on the heat being on (the single truth of "is this on fire"):
    // a caught log gets an oriented box that runs along the wood, the lit lighter a small
    // flame at its muzzle. The heat that cooks and spreads is the HeatSource, already handled.
    flow_emitters_.clear();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Item& item = items_[i];
        if (!item.heat || !item.heat->IsOn()) {
            item.burn_time = 0.0f; // Not alight (or gone cold): reset the build-up.
            continue;
        }
        if (item.ignitable) {
            // A caught log's fire is shaped to the wood: an oriented box turned with the log's
            // own pose and centred on its hot middle, so the flame runs along its length rather
            // than balling up as a sphere. It builds -- the box grows from a small patch to (a
            // little under) the log's footprint and the intensity from ember to full over
            // kFireBuildupSeconds, eased so it takes hold rather than flaring up at once. A
            // single log stays a modest campfire; a stack sums several boxes, so more wood
            // naturally makes a bigger fire.
            item.burn_time += dt;
            const float t = std::min(item.burn_time / kFireBuildupSeconds, 1.0f);
            const float g = t * t * (3.0f - 2.0f * t); // smoothstep ease
            const auto ramp = [g](float start, float full) { return start + (full - start) * g; };

            XMMATRIX pose = CurrentPose(i, camera_to_world);
            pose.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f); // keep orientation, drop position
            const XMFLOAT3 centre = item.heat->Origin();
            XMFLOAT4X4 box;
            XMStoreFloat4x4(&box, pose * XMMatrixTranslation(centre.x, centre.y, centre.z));
            const XMFLOAT3 he = item.half_extents;
            flow_emitters_.push_back({box,
                                      {ramp(0.06f, 0.9f * he.x), ramp(0.05f, 0.6f * he.y),
                                       ramp(0.06f, 0.9f * he.z)},
                                      ramp(0.20f, 0.62f),   // temperature
                                      ramp(0.03f, 0.16f),   // smoke
                                      ramp(0.05f, 0.14f),   // fuel
                                      ramp(0.13f, 0.40f)}); // upward draft (m/s)
        } else if (item.ability == Ability::Flame && flame != nullptr) {
            // The lit lighter's pilot tongue stays a CPU particle flame at its muzzle
            // (heat->Origin() sits there), not a Flow fire: the muzzle rides ~1 m from the
            // eye, too close for the world-depth-tested Flow pass to show (it gets culled
            // against the near ground), so the little flame is emitted as self-lit specks
            // that draw in the world pass instead. The fuller fires -- caught logs, the
            // grill grate -- are Flow, above and in main.
            flame->Emit(item.heat->Origin(), dt, 1.0f);
        }
    }

    // Advance the cook on every meat, carried or resting alike. Each cooks against
    // the surrounding air where it sits: room temperature by default, or the hottest
    // temperature any heat source imposes there -- the yard's furniture heat (the grill's
    // grate) or a lit log's -- so a steak laid on the grate or beside the fire finally
    // crosses the cook threshold, while one carried away cools back to the yard. The sample
    // point is the item's model origin, which sits on its underside -- the face on the grate.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Item& item = items_[i];
        // Non-food never cooks; a served meat is frozen at the band it was delivered in,
        // so it stops cooking the moment it leaves the hand for the counter.
        if (!item.cook || item.served) {
            continue;
        }
        const XMMATRIX pose = i == carried_
                                  ? XMLoadFloat4x4(&item.held_local) * camera_to_world
                                  : XMLoadFloat4x4(&item.resting);
        item.cook->Update(AmbientAt(pose.r[3], heat_sources), dt);
    }

    // Rebuild the draw lists from the current state. There are a handful of
    // items, so this is cheaper than tracking which one moved.
    // Read every soft meat back from the GPU before the draw lists are built: the
    // skinned vertices this produces are what the deforming ones are drawn from.
    soft_.clear();
    for (Item& item : items_) {
        if (item.soft != nullptr) {
            item.soft->Update();
        }
    }

    // Then hand every held soft meat its kinematic target for the next step. `in_simulation`
    // is exactly the right test: it is false precisely while an item is out of the rigid
    // scene -- carried, clamped in the tongs, served onto a tray, stacked in the pit -- so
    // this one loop covers every pick-up and put-down without any of those call sites
    // knowing about soft bodies. A deformable volume cannot simply leave the scene the way
    // the rigid body does (it would stop being read back, and the meat would draw frozen
    // where it was), so instead it is driven to where the hand holds it: still colliding,
    // still shoving what it meets, and on release carrying on from that exact shape.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Item& item = items_[i];
        if (item.soft == nullptr) {
            continue;
        }
        if (item.in_simulation) {
            item.soft->EndKinematic(); // A no-op unless it was being carried.
            continue;
        }
        // CurrentPose covers carried and resting-or-stuck; the tongs' jaws are their own
        // pose, which it does not know about.
        const XMMATRIX pose =
            i == gripped_ ? TongsGripPose() * camera_to_world : CurrentPose(i, camera_to_world);
        XMFLOAT4X4 held;
        XMStoreFloat4x4(&held, pose);
        if (item.soft->Kinematic()) {
            item.soft->UpdateKinematic(held);
        } else {
            item.soft->BeginKinematic(held);
        }
    }

    world_.clear();
    held_.clear();
    highlight_.clear();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        // A deforming meat is drawn from its skinned vertices instead of as a rigid
        // instance -- one draw list or the other, never both. The vertices are already
        // in world space, so unlike the rigid path there is no pose to hand over; the
        // browning tint and the wet sheen carry across unchanged. This comes before the
        // held checks because it holds *wherever* the meat is: carried and gripped ones
        // are being driven to the hand (see the kinematic loop above), so their vertices
        // are already at the held pose and they draw here rather than in the held pass.
        // They lose that pass's cleared depth, but a driven meat is a real collider and
        // so cannot be inside the wall that would have sliced it.
        if (items_[i].soft != nullptr) {
            SoftMeshInstance draw;
            draw.mesh = items_[i].soft_mesh;
            draw.vertices = items_[i].soft->SkinnedVertices();
            draw.tint = ItemTint(items_[i]);
            draw.wetness = ItemWetness(items_[i]);
            soft_.push_back(draw);
            continue;
        }
        if (i == carried_ || i == gripped_) {
            continue; // drawn in the held pass below (the tongs, and the meat in them)
        }
        // A meat served onto the carried tray travels in the hand with it, so it draws
        // in the held pass alongside the tray rather than out in the world.
        if (items_[i].served && items_[i].stuck_to == carried_) {
            continue;
        }
        world_.push_back(MakeInstance(CurrentModel(items_[i]), XMLoadFloat4x4(&items_[i].resting),
                                      ItemTint(items_[i]), ItemWetness(items_[i])));
    }
    // A soft meat is never in the held pass -- it went into soft_ above, at the pose it is
    // being driven to -- so both of these skip one.
    if (carried_ >= 0 && items_[carried_].soft == nullptr) {
        held_.push_back(MakeInstance(CurrentModel(items_[carried_]),
                                     XMLoadFloat4x4(&items_[carried_].held_local) * camera_to_world,
                                     ItemTint(items_[carried_]), ItemWetness(items_[carried_])));
    }
    if (carried_ >= 0) {
        // The plate of food on a carried tray: each stuck meat's resting was set above to
        // its pose on the (held) tray, so it draws in the hand right where it sits.
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[i].served && items_[i].stuck_to == carried_ && items_[i].soft == nullptr) {
                held_.push_back(MakeInstance(CurrentModel(items_[i]),
                                             XMLoadFloat4x4(&items_[i].resting), ItemTint(items_[i])));
            }
        }
    }
    // The meat clamped in the tongs draws in the hand at the grip pose, over the cleared
    // depth like the tongs themselves, so a wall never slices it.
    if (gripped_ >= 0 && items_[gripped_].soft == nullptr) {
        held_.push_back(MakeInstance(CurrentModel(items_[gripped_]),
                                     TongsGripPose() * camera_to_world, ItemTint(items_[gripped_])));
    }
    // The outline draws the hovered item a second time at its resting pose, so
    // it lines up exactly with the world copy above. The outline shader ignores
    // tint, so the browning does not matter here, but pass it for consistency.
    if (hovered_ >= 0) {
        highlight_.push_back(MakeInstance(CurrentModel(items_[hovered_]),
                                          XMLoadFloat4x4(&items_[hovered_].resting),
                                          ItemTint(items_[hovered_])));
    }
    // While the tongs are held with empty jaws, ring the meat they would grip -- the
    // same outline the E-pick uses, so the mouse grab reads the same way. hovered_ is
    // always -1 while carrying, so this never double-rings with the block above.
    if (grip_target_ >= 0) {
        highlight_.push_back(MakeInstance(CurrentModel(items_[grip_target_]),
                                          XMLoadFloat4x4(&items_[grip_target_].resting),
                                          ItemTint(items_[grip_target_])));
    }
}

std::string Props::PromptText() const {
    if (carried_ >= 0) {
        // The tongs grip meat on the primary action. Offer grab (a meat looked at) or
        // release (one already in the jaws) before the generic drop, so the mouse action
        // is discoverable; the label names the real binding. Off both, it falls through
        // to "[E] Drop", which lets go of the tongs (and any meat) together.
        if (items_[carried_].ability == Ability::GripMeat) {
            if (gripped_ >= 0) {
                const Item& meat = items_[gripped_];
                return "[" + primary_label_ + "] Release " + meat.name + " (" +
                       std::string(meat.cook->DonenessLabel()) + ")";
            }
            if (grip_target_ >= 0) {
                const Item& meat = items_[grip_target_];
                return "[" + primary_label_ + "] Grab " + meat.name + " (" +
                       std::string(meat.cook->DonenessLabel()) + ")";
            }
            return "[E] Drop";
        }
        // Holding a meat over a tray, Interact loads it -- any meat, unjudged, so the hint
        // just names the food and its current cook. Carrying the loaded tray in the
        // turn-in zone, Interact hands it in. Off both, Interact is the drop.
        if (serve_tray_ >= 0 && items_[carried_].cook) {
            const Item& meat = items_[carried_];
            return "[E] Place " + meat.name + " (" +
                   std::string(meat.cook->DonenessLabel()) + ") on tray";
        }
        if (in_turn_in_) {
            return "[E] Turn in";
        }
        // Holding a log over the fire pit, the primary action stacks it on -- name the real
        // binding, like the tongs' hint. Off it, the log's primary action does nothing and
        // Interact is the drop.
        if (log_over_pit_) {
            return "[" + primary_label_ + "] Add log to fire pit";
        }
        // The lighter fluid squirts on the primary action -- always available while it is
        // held, so its button shows the whole time (Interact still drops it).
        if (items_[carried_].ability == Ability::SprayFluid) {
            return "[" + primary_label_ + "] Spray lighter fluid";
        }
        // The lighter strikes a flame on the primary action -- like the fluid's squirt,
        // available the whole time it is held, so its button shows throughout.
        if (items_[carried_].ability == Ability::Flame) {
            return "[" + primary_label_ + "] Flame";
        }
        return "[E] Drop";
    }
    if (hovered_ >= 0) {
        const Item& item = items_[hovered_];
        // Name the meats' doneness in the prompt so the cook is legible at a glance:
        // "[E] Pick up steak (raw)". The tongs, having no cook, read plainly.
        if (item.cook) {
            return "[E] Pick up " + item.name + " (" +
                   std::string(item.cook->DonenessLabel()) + ")";
        }
        // A doused log reads its soaking, so the player can see the spray took before they
        // bring a flame to it: "[E] Pick up log (soaked in lighter fluid)".
        if (item.wetness && item.wetness->IsWet()) {
            return "[E] Pick up " + item.name + " (soaked in " +
                   std::string(WetAgentInfo(item.wetness->Agent()).name) + ")";
        }
        return "[E] Pick up " + item.name;
    }
    return {};
}

std::vector<Props::MeatStatus> Props::MeatStatuses() const {
    std::vector<MeatStatus> statuses;
    for (const Item& item : items_) {
        // Only the food cooks; the tongs and any other non-food carryable carry no
        // CookInformation, so they are not meats and belong on no meats panel.
        if (!item.cook) {
            continue;
        }
        statuses.push_back(MeatStatus{item.name, static_cast<int>(item.cook->DonenessBand()),
                                      static_cast<int>(item.cook->InternalTempF()), item.served});
    }
    return statuses;
}

std::vector<XMFLOAT3> Props::IgnitablePositions() const {
    std::vector<XMFLOAT3> positions;
    for (const Item& item : items_) {
        if (item.ignitable) {
            // The translation of the resting model-to-world -- where the log sits right now.
            positions.push_back({item.resting._41, item.resting._42, item.resting._43});
        }
    }
    return positions;
}

int Props::PickTarget(FXMVECTOR eye, FXMVECTOR forward) const {
    XMFLOAT3 origin;
    XMFLOAT3 direction;
    XMStoreFloat3(&origin, eye);
    XMStoreFloat3(&direction, forward);

    // Sweep a small sphere down the gaze and take the nearest prop it meets. The
    // filter restricts the query to prop bodies, so neither the yard's static
    // geometry nor the player's own capsule -- which the sweep starts inside --
    // can shadow the pick. The carried prop, if any, was removed from the scene and
    // so is out of the query too.
    PropQueryFilter filter;
    const PxQueryFilterData filter_data(PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    PxSweepBuffer hit;
    const bool blocked = physics_->Scene().sweep(
        PxSphereGeometry(kPickRadius), PxTransform(PxVec3(origin.x, origin.y, origin.z)),
        PxVec3(direction.x, direction.y, direction.z), kReach, hit, PxHitFlag::eDEFAULT,
        filter_data, &filter);
    if (!blocked) {
        return -1;
    }

    // Only carryable props pass the filter, and each carries its item index in its
    // BodyTag -- so the nearest hit decodes straight back to an item.
    return static_cast<const BodyTag*>(hit.block.actor->userData)->prop_index;
}

void Props::ReleaseBody(int index, FXMMATRIX pose_world, FXMMATRIX camera_to_world, bool toss) {
    Item& item = items_[index];

    // A thrown drop leaves the hand with a gentle underarm toss down the gaze and a
    // forward pitch about the player's right, so it tumbles over instead of gliding down
    // flat. A placed drop (the tongs) gets neither: zeroed velocity means it falls
    // straight from the pose it was held at, landing exactly where the jaws were.
    XMFLOAT3 linear{0.0f, 0.0f, 0.0f};
    XMFLOAT3 spin{0.0f, 0.0f, 0.0f};
    if (toss) {
        const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);
        const XMVECTOR right = XMVector3Normalize(camera_to_world.r[0]);
        XMStoreFloat3(&linear, XMVectorAdd(XMVectorScale(forward, kThrowSpeed),
                                           XMVectorSet(0.0f, -kThrowDrop, 0.0f, 0.0f)));
        XMStoreFloat3(&spin, XMVectorScale(right, kThrowSpin));
    }

    // Re-enter the scene at the release pose: pose first (so the actor arrives
    // where the hand let go, not at its stale pre-carry spot), then the add, then
    // the velocities and wake -- the inverse of RemoveBodyFromScene.
    PxRigidDynamic* body = item.rigid.actor();
    body->setGlobalPose(ToPxTransform(pose_world));
    physics_->Scene().addActor(*body);
    item.in_simulation = true;
    body->setLinearVelocity(PxVec3(linear.x, linear.y, linear.z));
    body->setAngularVelocity(PxVec3(spin.x, spin.y, spin.z));
    body->wakeUp();
    RebuildTransform(item);
}

void Props::Drop(FXMMATRIX camera_to_world) {
    // If the tongs are gripping a meat, set it down first -- placed in the jaws' spot
    // with no toss, so it drops straight rather than being flung as the tongs let go.
    if (gripped_ >= 0) {
        ReleaseBody(gripped_, TongsGripPose() * camera_to_world, camera_to_world, /*toss=*/false);
        gripped_ = -1;
    }

    // Let go at the exact pose the object was carried, so it falls out of the hand
    // rather than teleporting to a tidy spot. The held instance is drawn under
    // held_local * camera_to_world, and the body frame is the model frame, so that same
    // matrix is the body's pose. The hand throws -- tongs are the precise placer.
    ReleaseBody(carried_, XMLoadFloat4x4(&items_[carried_].held_local) * camera_to_world,
                camera_to_world, /*toss=*/true);
    carried_ = -1;
}

void Props::TriggerAbility(int item, FXMMATRIX camera_to_world) {
    // Dispatch on the carried item's catalog-declared ability. None is the placeholder
    // every item starts at; new behaviours get their own case here.
    switch (items_[item].ability) {
    case Ability::None:
        break; // No behaviour yet.
    case Ability::GripMeat:
        // The tongs. With a meat in the jaws, set it down exactly where they hold it --
        // placed with no toss, so it drops straight onto whatever is below (the grate),
        // more precise than the hand's throw. Otherwise clamp the meat looked at --
        // grip_target_, already filtered to a meat in reach -- out of the simulation.
        if (gripped_ >= 0) {
            ReleaseBody(gripped_, TongsGripPose() * camera_to_world, camera_to_world,
                        /*toss=*/false);
            gripped_ = -1;
        } else if (grip_target_ >= 0) {
            RemoveBodyFromScene(grip_target_);
            gripped_ = grip_target_;
            grip_target_ = -1;
        }
        break;
    case Ability::StackInFirePit:
        // The firewood log. Held over the fire pit, the primary action stacks it onto the
        // pit and takes it out of play; held anywhere else it does nothing (step over the
        // pit to place it). log_over_pit_ and the pit centre were cached this frame above.
        if (log_over_pit_) {
            PlaceInFirePit(item, fire_pit_center_);
        }
        break;
    case Ability::SprayFluid:
        // The lighter fluid sprays continuously while the button is held, which the
        // held-spray block in Update drives (see kSprayPerSecond) -- an edge fire here
        // would spit a single droplet, so the press itself does nothing extra.
        break;
    case Ability::Flame:
        break; // The lighter's flame: wired, but no behaviour yet.
    }
}

void Props::Load(int meat, int tray) {
    Item& item = items_[meat];

    // Any meat loads, unjudged -- the orders are not consulted here (turn-in decides).
    // Stick it to the tray: mark it served (so it no longer cooks, is picked up, or is
    // highlighted) and store its pose in the tray's own frame, so it rides the tray
    // wherever it goes. Rest it on the tray's serve surface, nudged into one of a few
    // scatter slots so several loaded meats spread across the face instead of sharing one
    // point.
    item.served = true;
    item.stuck_to = tray;
    int placed = 0;
    for (const Item& other : items_) {
        if (&other != &item && other.served && other.stuck_to == tray) {
            ++placed;
        }
    }
    constexpr float kSlot = 0.06f; // metres between scatter slots on the tray face
    const XMFLOAT3 surface = items_[tray].serve->offset; // tray-local top centre
    const float sx = ((placed % 2) == 0 ? -1.0f : 1.0f) * kSlot;
    const float sz = (((placed / 2) % 2) == 0 ? -1.0f : 1.0f) * kSlot;
    XMStoreFloat4x4(&item.stuck_local,
                    XMMatrixTranslation(surface.x + sx, surface.y, surface.z + sz));

    // Its body left the scene on pick-up and stays out of the simulation -- loaded food
    // neither falls nor is knocked; it follows the tray's pose from here (see Update).
    carried_ = -1;
}

void Props::PlaceInFirePit(int log, XMFLOAT3 pit_center) {
    Item& item = items_[log];

    // Where in the pit this log lands: logs stack two to a layer, each layer turned a
    // quarter so they cross like laid firewood. Count the logs already in the pit to pick
    // the slot -- 0,1 fill the bottom layer side by side, 2,3 the next layer crossing it,
    // and so on up. The offsets are in the log's own half-extents, so a layer's pair sits
    // just touching and each layer rests a full log-height on the one below.
    int placed = 0;
    for (const Item& other : items_) {
        if (&other != &item && other.in_fire_pit) {
            ++placed;
        }
    }
    const float half_w = item.half_extents.x; // half a log's width
    const float half_h = item.half_extents.y; // half a log's height
    const int row = placed / 2;
    const int col = placed % 2;
    const float lateral = (col == 0 ? -half_w : half_w);
    const bool cross = (row % 2) != 0; // alternate layers lie the other way
    const float dx = cross ? 0.0f : lateral;
    const float dz = cross ? lateral : 0.0f;
    const float dy = static_cast<float>(row) * (2.0f * half_h);
    const float yaw = XMConvertToRadians((cross ? 96.0f : 6.0f) + static_cast<float>(placed) * 7.0f);

    // The log's body frame is its model frame (origin on the underside), so this rotate-
    // then-translate is exactly the pose the renderer draws it under, and the underside
    // lands on the pit floor (dy 0) or atop the layer below.
    const XMMATRIX pose = XMMatrixRotationY(yaw) *
                          XMMatrixTranslation(pit_center.x + dx, pit_center.y + dy, pit_center.z + dz);
    XMStoreFloat4x4(&item.resting, pose);

    // Its body left the scene on pick-up and stays out of the simulation -- a placed log
    // neither falls nor is knocked, and is out of the pick sweep too (like carried and
    // served bodies). in_fire_pit fixes `resting` here (RebuildTransform skips it), so the
    // log simply draws at this pose from now on. Mark it placed and empty the hand.
    item.in_fire_pit = true;
    carried_ = -1;
}

void Props::TurnIn(int tray, Objectives& objectives) {
    // Hand every meat loaded on this tray to the orders at once. Objectives::Serve fills
    // the first open order the type and band fit and rejects the rest, so a tray that
    // matches the ticket completes it and a wrong or short one leaves orders unfilled --
    // the results screen reads Objectives to show which. A meat that fits no order is
    // simply not counted; nothing is undone. Filled counts start at zero (this is the
    // only caller), so the order the meats are visited in does not change the tally.
    for (const Item& item : items_) {
        if (item.served && item.stuck_to == tray && item.cook) {
            objectives.Serve(item.name, item.cook->DonenessBand());
        }
    }
    turned_in_ = true;
}

XMMATRIX Props::CurrentPose(int index, FXMMATRIX camera_to_world) const {
    // A carried item hangs at its held pose in front of the eye; anything else sits at
    // the resting pose last read from its body (or, for stuck food, computed onto its
    // tray). This is the one transform an item is drawn and reasoned about under.
    if (index == carried_) {
        return XMLoadFloat4x4(&items_[index].held_local) * camera_to_world;
    }
    return XMLoadFloat4x4(&items_[index].resting);
}

float Props::AmbientAt(FXMVECTOR point, std::span<const HeatSource> heat_sources) const {
    // The hottest thing wins: heat does not stack here, so standing a log in a campfire
    // and a lit grill at once is as hot as the hotter of the two, not their sum.
    float ambient_f = CookInformation::kRoomTempF;
    for (const HeatSource& source : heat_sources) {
        ambient_f = std::max(ambient_f, source.TemperatureAt(point));
    }
    // A carryable's own heat -- a lit log, a struck lighter -- warms what is near it too.
    // An off source returns room air, so an unlit stack costs a call and changes nothing.
    // An item's own heat is included: harmless, since a thing is only hot once it is lit,
    // and once lit there is nothing left to light.
    for (const Item& other : items_) {
        if (other.heat) {
            ambient_f = std::max(ambient_f, other.heat->TemperatureAt(point));
        }
    }
    return ambient_f;
}

XMVECTOR Props::NozzleLocal(const Item& item) const {
    // The box's far face down Z, on the model's own axis. A point, so w is 1: the
    // callers carry it into the world through the item's pose, which has to translate it.
    return XMVectorSet(0.0f, 0.0f, item.com_offset.z + item.half_extents.z, 1.0f);
}
