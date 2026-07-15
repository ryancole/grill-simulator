#include "props.hpp"

#include "actions.hpp"
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

MeshInstance MakeInstance(std::uint32_t model, FXMMATRIX transform, XMFLOAT3 tint = kWhite) {
    MeshInstance instance{};
    instance.model = model;
    XMStoreFloat4x4(&instance.transform, transform);
    instance.tint = tint;
    instance.checker = 0.0f;
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
            spawn.ability);
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

void Props::RebuildTransform(Item& item) {
    // The body frame is the model frame, so its global pose *is* the model-to-world
    // transform, which the RigidBody hands back directly.
    XMStoreFloat4x4(&item.resting, item.rigid.Transform());
}

XMFLOAT3 Props::ItemTint(const Item& item) {
    // Raw meat's tint is white, so this leaves uncooked food and the non-cooking
    // props drawing exactly as their models do; only cooking browns the colour.
    return item.cook ? item.cook->SurfaceTint() : kWhite;
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
                std::optional<ServeDef> serve, Ability ability) {
    Item item{};
    item.stages = std::move(stages);
    item.name = std::move(name);
    item.ability = ability;
    XMStoreFloat4x4(&item.held_local, held_local);
    if (cook) {
        item.cook.emplace(*cook);
    }
    item.serve = serve;
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

    // Bind userData only after the item is settled in items_, since it points at
    // the tag living inside the RigidBody -- items_ is reserved so it never moves.
    items_.push_back(std::move(item));
    items_.back().rigid.Bind();
}

void Props::Update(const XMMATRIX& camera_to_world, const Actions& actions, float dt,
                   std::span<const HeatSource> heat_sources, Objectives& objectives) {
    // The camera-to-world matrix is right, up, forward, eye as its four rows.
    const XMVECTOR eye = camera_to_world.r[3];
    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);

    // Which tray the carried meat hangs over this frame, if any. Each tray's serve zone
    // rides its current pose, so it is rebuilt from the tray each frame -- a tray set
    // down on the bench takes deliveries where it sits. Computed before the Interact
    // press so the same read drives both the serve and the prompt; alongside it, whether
    // this cook would be taken and, if not, what the order still wants, so the prompt can
    // explain a serve about to bounce. Only a meat serves; the tongs carry no cook.
    serve_tray_ = -1;
    serve_ok_ = false;
    serve_need_.clear();
    serve_rejected_type_.clear();
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
        if (serve_tray_ >= 0) {
            const CookInformation::Doneness band = items_[carried_].cook->DonenessBand();
            serve_ok_ = objectives.WouldAccept(items_[carried_].name, band);
            if (!serve_ok_) {
                if (const FoodGoal* order = objectives.NextOrderFor(items_[carried_].name)) {
                    serve_need_ = std::string(DonenessName(order->min)) + " to " +
                                  std::string(DonenessName(order->max));
                }
            }
        }
    }

    // Edge-triggered: one grab per press, so holding Interact does not pick up and
    // drop on alternate frames. Actions latches the press for us, so this is a
    // single event even while the key is held down.
    if (actions.WasPressed(Action::Interact)) {
        if (carried_ >= 0) {
            if (serve_tray_ >= 0) {
                // Over a tray, Interact delivers rather than drops. Serve() accepts the
                // meat only if its cook fills an open order; a rejected cook stays in
                // hand (a no-op, not a drop) -- "reject and keep" -- so a mis-timed press
                // never fumbles food onto the tray. To drop over a tray, step off first.
                // A refusal latches the meat's type for one frame so the HUD can shake
                // the card; the meat's name is read before Serve, which leaves it carried.
                const std::string type = items_[carried_].name;
                if (!Serve(carried_, serve_tray_, objectives)) {
                    serve_rejected_type_ = type;
                }
            } else {
                Drop(camera_to_world);
            }
        } else {
            const int target = PickTarget(eye, forward);
            if (target >= 0) {
                // Lift it out of the simulation while it hangs in the hand: it
                // neither falls nor shoves anything until it is dropped.
                items_[target].rigid.actor()->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, true);
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

    // What the prompt reports this frame: nothing to pick while carrying, else
    // whatever is in reach and looked at. Cheap enough to recompute outright for
    // a handful of items.
    hovered_ = carried_ >= 0 ? -1 : PickTarget(eye, forward);

    // Read each uncarried body's stepped pose back into its render transform.
    // Physics has already advanced the scene this frame, so these are current. A
    // served item is skipped: its body is out of the simulation and its resting pose
    // was set to the counter when it was delivered, so there is nothing to read back.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i != carried_ && !items_[i].served) {
            RebuildTransform(items_[i]);
        }
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

    // Advance the cook on every meat, carried or resting alike. Each cooks against
    // the surrounding air where it sits: room temperature by default, or the hottest
    // temperature any heat source imposes there -- so a steak laid on the grill's
    // grate finally crosses the cook threshold, while one carried away cools back to
    // the yard. The sample point is the item's model origin, which sits on its
    // underside -- exactly the face resting on the grate.
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
        const XMVECTOR point = pose.r[3];
        float ambient_f = CookInformation::kRoomTempF;
        for (const HeatSource& source : heat_sources) {
            ambient_f = std::max(ambient_f, source.TemperatureAt(point));
        }
        item.cook->Update(ambient_f, dt);
    }

    // Rebuild the draw lists from the current state. There are a handful of
    // items, so this is cheaper than tracking which one moved.
    world_.clear();
    held_.clear();
    highlight_.clear();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i == carried_) {
            continue; // drawn in the held pass below
        }
        // A meat served onto the carried tray travels in the hand with it, so it draws
        // in the held pass alongside the tray rather than out in the world.
        if (items_[i].served && items_[i].stuck_to == carried_) {
            continue;
        }
        world_.push_back(MakeInstance(CurrentModel(items_[i]), XMLoadFloat4x4(&items_[i].resting),
                                      ItemTint(items_[i])));
    }
    if (carried_ >= 0) {
        held_.push_back(MakeInstance(CurrentModel(items_[carried_]),
                                     XMLoadFloat4x4(&items_[carried_].held_local) * camera_to_world,
                                     ItemTint(items_[carried_])));
        // The plate of food on a carried tray: each stuck meat's resting was set above to
        // its pose on the (held) tray, so it draws in the hand right where it sits.
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            if (items_[i].served && items_[i].stuck_to == carried_) {
                held_.push_back(MakeInstance(CurrentModel(items_[i]),
                                             XMLoadFloat4x4(&items_[i].resting), ItemTint(items_[i])));
            }
        }
    }
    // The outline draws the hovered item a second time at its resting pose, so
    // it lines up exactly with the world copy above. The outline shader ignores
    // tint, so the browning does not matter here, but pass it for consistency.
    if (hovered_ >= 0) {
        highlight_.push_back(MakeInstance(CurrentModel(items_[hovered_]),
                                          XMLoadFloat4x4(&items_[hovered_].resting),
                                          ItemTint(items_[hovered_])));
    }
}

std::string Props::PromptText() const {
    if (carried_ >= 0) {
        // Over a tray, the carried meat can be delivered. Only offer "[E] Serve" when
        // this cook would actually be taken; otherwise say why nothing happens -- the
        // band it needs, or that no order wants this food -- so a rejected press reads as
        // "not yet", not as broken. Off a tray, Interact is the drop.
        if (serve_tray_ >= 0 && items_[carried_].cook) {
            const Item& meat = items_[carried_];
            const std::string band(meat.cook->DonenessLabel());
            if (serve_ok_) {
                return "[E] Serve " + meat.name + " (" + band + ")";
            }
            if (!serve_need_.empty()) {
                return meat.name + " (" + band + ") -- needs " + serve_need_;
            }
            return meat.name + " (" + band + ") -- no order needs this";
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
        return "[E] Pick up " + item.name;
    }
    return {};
}

std::vector<std::string> Props::MeatDebugLines() const {
    std::vector<std::string> lines;
    for (const Item& item : items_) {
        // Only the food cooks; the tongs and any other non-food carryable carry no
        // CookInformation, so they are not meats and have nothing to report.
        if (!item.cook) {
            continue;
        }
        lines.push_back(item.name + ": " + std::string(item.cook->DonenessLabel()) + " (" +
                        std::to_string(static_cast<int>(item.cook->InternalTempF())) + "F)" +
                        (item.served ? " [served]" : ""));
    }
    return lines;
}

std::optional<Props::MeatReadout> Props::ActiveMeat() const {
    // Prefer the carried item -- what the player is committing to -- and fall back to
    // the one merely looked at while empty-handed. Either way it counts only if it is
    // a meat: a cooking item carries a CookInformation the band is read from, while the
    // tongs and the tray do not and so surface no readout.
    const int index = carried_ >= 0 ? carried_ : hovered_;
    if (index < 0) {
        return std::nullopt;
    }
    const Item& item = items_[static_cast<std::size_t>(index)];
    if (!item.cook) {
        return std::nullopt;
    }
    return MeatReadout{item.name, static_cast<int>(item.cook->DonenessBand())};
}

std::optional<std::string> Props::RejectedServeType() const {
    if (serve_rejected_type_.empty()) {
        return std::nullopt;
    }
    return serve_rejected_type_;
}

int Props::PickTarget(FXMVECTOR eye, FXMVECTOR forward) const {
    XMFLOAT3 origin;
    XMFLOAT3 direction;
    XMStoreFloat3(&origin, eye);
    XMStoreFloat3(&direction, forward);

    // Sweep a small sphere down the gaze and take the nearest prop it meets. The
    // filter restricts the query to prop bodies, so neither the yard's static
    // geometry nor the player's own capsule -- which the sweep starts inside --
    // can shadow the pick. The carried prop, if any, is eDISABLE_SIMULATION and so
    // is out of the query too.
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

void Props::Drop(FXMMATRIX camera_to_world) {
    Item& item = items_[carried_];

    // Let go at the exact pose the object was carried, so it falls out of the
    // hand rather than teleporting to a tidy spot. The held instance is drawn
    // under held_local * camera_to_world, and the body frame is the model frame,
    // so that same matrix is the body's pose.
    const XMMATRIX held_world = XMLoadFloat4x4(&item.held_local) * camera_to_world;
    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);
    const XMVECTOR right = XMVector3Normalize(camera_to_world.r[0]);

    XMFLOAT3 toss;
    XMStoreFloat3(&toss, XMVectorAdd(XMVectorScale(forward, kThrowSpeed),
                                     XMVectorSet(0.0f, -kThrowDrop, 0.0f, 0.0f)));
    XMFLOAT3 spin;
    XMStoreFloat3(&spin, XMVectorScale(right, kThrowSpin));

    // Back into the simulation at the hand pose, with a gentle underarm toss down
    // the gaze and a forward pitch about the player's right so it tumbles over
    // instead of gliding down flat.
    PxRigidDynamic* body = item.rigid.actor();
    body->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, false);
    body->setGlobalPose(ToPxTransform(held_world));
    body->setLinearVelocity(PxVec3(toss.x, toss.y, toss.z));
    body->setAngularVelocity(PxVec3(spin.x, spin.y, spin.z));
    body->wakeUp();
    RebuildTransform(item);

    carried_ = -1;
}

void Props::TriggerAbility(int item, FXMMATRIX camera_to_world) {
    // Dispatch on the carried item's catalog-declared ability. Each carryable type
    // gets its own case as behaviours are added; the camera basis is on hand for one
    // that needs to act along the gaze. None is the placeholder every item starts at.
    (void)camera_to_world;
    switch (items_[item].ability) {
    case Ability::None:
        break; // No behaviour yet -- new abilities dispatch here.
    }
}

bool Props::Serve(int meat, int tray, Objectives& objectives) {
    Item& item = items_[meat];

    // The order decides. A cook it will not take -- undercooked, overcooked, or a type
    // it does not want -- leaves everything untouched and the meat in hand.
    if (!objectives.Serve(item.name, item.cook->DonenessBand())) {
        return false;
    }

    // Accepted. Stick it to the tray: mark it served (so it no longer cooks, is picked
    // up, or is highlighted) and store its pose in the tray's own frame, so it rides the
    // tray wherever it goes. Rest it on the tray's serve surface, nudged into one of a
    // few scatter slots so several delivered meats spread across the face instead of
    // sharing one point.
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

    // Its body was disabled on pick-up and stays out of the simulation -- stuck food
    // neither falls nor is knocked; it follows the tray's pose from here (see Update).
    carried_ = -1;
    return true;
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
