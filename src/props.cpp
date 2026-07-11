#include "props.hpp"

#include "input.hpp"
#include "physics.hpp"

#include <PxPhysicsAPI.h>

#include <cfloat>
#include <cmath>

using namespace DirectX;
using namespace physx;

namespace {

// The prop models carry their own colours, so the instance that places one wants
// white -- exactly as the yard's glTF furniture does.
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};

// How far the player can reach to grab, and how far off the centre of the gaze a
// prop may sit and still be grabbed. 0.80 is about 37 degrees, forgiving enough
// that a small object on the ground does not have to be pixel-centred.
constexpr float kReach = 2.4f;
constexpr float kMinAlignment = 0.80f;

// The interact key. Left-click is already the mouse-look toggle, so grabbing
// gets its own key, the shooter convention.
constexpr int kInteractKey = 'E';

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

MeshInstance MakeInstance(std::uint32_t model, FXMMATRIX transform) {
    MeshInstance instance{};
    instance.model = model;
    XMStoreFloat4x4(&instance.transform, transform);
    instance.tint = kWhite;
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

} // namespace

Props::Props(const Scene& scene, Physics& physics) : physics_(&physics) {
    const PropModels& models = scene.PropModelIds();
    const std::vector<Model>& pool = scene.Models();

    // The tongs lie across the grill's side shelf; the meat waits on the picnic
    // table. All four sit in the player's view from the spawn point. Each carries
    // the Model it was loaded from so its box collider can be measured off the
    // mesh bounds.
    Add(models.tongs, pool[models.tongs], "tongs", {1.15f, 0.78f, 5.0f}, 90.0f, TongsInHand());
    Add(models.steak, pool[models.steak], "steak", {-4.55f, 0.80f, 1.70f}, 18.0f, FlatInHand());
    Add(models.patty, pool[models.patty], "patty", {-4.25f, 0.80f, 1.35f}, 0.0f, FlatInHand());
    Add(models.patty, pool[models.patty], "patty", {-4.80f, 0.80f, 1.45f}, -24.0f, FlatInHand());
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

void Props::CreateBody(Item& item, FXMMATRIX initial_pose) {
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
    item.body = body;
}

void Props::RebuildTransform(Item& item) {
    // The body frame is the model frame, so its global pose *is* the model-to-
    // world transform: rotate by the quaternion, then translate.
    const PxTransform pose = item.body->getGlobalPose();
    const XMVECTOR q = XMVectorSet(pose.q.x, pose.q.y, pose.q.z, pose.q.w);
    const XMVECTOR t = XMVectorSet(pose.p.x, pose.p.y, pose.p.z, 0.0f);
    const XMMATRIX m = XMMatrixRotationQuaternion(q) * XMMatrixTranslationFromVector(t);
    XMStoreFloat4x4(&item.resting, m);
}

void Props::Add(std::uint32_t model_id, const Model& model, std::string name, XMFLOAT3 position,
                float yaw_degrees, FXMMATRIX held_local) {
    Item item{};
    item.model = model_id;
    item.name = std::move(name);
    XMStoreFloat4x4(&item.held_local, held_local);
    DeriveBodyShape(item, model);

    // Seed the body so the model origin lands at `position`, yawed -- the same
    // placement the old yaw-and-translate gave.
    const XMMATRIX pose = XMMatrixRotationY(XMConvertToRadians(yaw_degrees)) *
                          XMMatrixTranslation(position.x, position.y, position.z);
    CreateBody(item, pose);
    RebuildTransform(item);

    items_.push_back(std::move(item));
}

void Props::Update(const XMMATRIX& camera_to_world, const Input& input) {
    // The camera-to-world matrix is right, up, forward, eye as its four rows.
    const XMVECTOR eye = camera_to_world.r[3];
    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);

    // Edge-triggered: one grab per press, so holding E does not pick up and drop
    // on alternate frames. Key auto-repeat keeps the key "down", so this stays a
    // single event.
    const bool down = input.IsKeyDown(kInteractKey);
    if (down && !interact_was_down_) {
        if (carried_ >= 0) {
            Drop(camera_to_world);
        } else {
            const int target = PickTarget(eye, forward);
            if (target >= 0) {
                // Lift it out of the simulation while it hangs in the hand: it
                // neither falls nor shoves anything until it is dropped.
                items_[target].body->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, true);
                carried_ = target;
            }
        }
    }
    interact_was_down_ = down;

    // What the prompt reports this frame: nothing to pick while carrying, else
    // whatever is in reach and looked at. Cheap enough to recompute outright for
    // a handful of items.
    hovered_ = carried_ >= 0 ? -1 : PickTarget(eye, forward);

    // Read each uncarried body's stepped pose back into its render transform.
    // Physics has already advanced the scene this frame, so these are current.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i != carried_) {
            RebuildTransform(items_[i]);
        }
    }

    // Rebuild the draw lists from the current state. There are a handful of
    // items, so this is cheaper than tracking which one moved.
    world_.clear();
    held_.clear();
    highlight_.clear();
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i == carried_) {
            continue;
        }
        world_.push_back(MakeInstance(items_[i].model, XMLoadFloat4x4(&items_[i].resting)));
    }
    if (carried_ >= 0) {
        held_.push_back(MakeInstance(items_[carried_].model,
                                     XMLoadFloat4x4(&items_[carried_].held_local) *
                                         camera_to_world));
    }
    // The outline draws the hovered item a second time at its resting pose, so
    // it lines up exactly with the world copy above.
    if (hovered_ >= 0) {
        highlight_.push_back(
            MakeInstance(items_[hovered_].model, XMLoadFloat4x4(&items_[hovered_].resting)));
    }
}

std::string Props::PromptText() const {
    if (carried_ >= 0) {
        return "[E] Drop";
    }
    if (hovered_ >= 0) {
        return "[E] Pick up " + items_[hovered_].name;
    }
    return {};
}

int Props::PickTarget(FXMVECTOR eye, FXMVECTOR forward) const {
    int best = -1;
    float best_alignment = kMinAlignment;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        // The object's origin sits on its underside; lift the aim point a little
        // so a thing lying on the ground is not looked straight past.
        const XMVECTOR origin = XMLoadFloat4x4(&items_[i].resting).r[3];
        const XMVECTOR target = XMVectorAdd(origin, XMVectorSet(0.0f, 0.06f, 0.0f, 0.0f));
        const XMVECTOR to_target = XMVectorSubtract(target, eye);

        const float distance = XMVectorGetX(XMVector3Length(to_target));
        if (distance > kReach || distance < 1e-3f) {
            continue;
        }
        const float alignment =
            XMVectorGetX(XMVector3Dot(XMVector3Normalize(to_target), forward));
        if (alignment > best_alignment) {
            best_alignment = alignment;
            best = i;
        }
    }
    return best;
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
    PxRigidDynamic* body = item.body;
    body->setActorFlag(PxActorFlag::eDISABLE_SIMULATION, false);
    body->setGlobalPose(ToPxTransform(held_world));
    body->setLinearVelocity(PxVec3(toss.x, toss.y, toss.z));
    body->setAngularVelocity(PxVec3(spin.x, spin.y, spin.z));
    body->wakeUp();
    RebuildTransform(item);

    carried_ = -1;
}
