#include "props.hpp"

#include "input.hpp"

#include <cfloat>
#include <cmath>

using namespace DirectX;

namespace {

// The prop models carry their own colours, so the instance that places one wants
// white -- exactly as the yard's glTF furniture does.
constexpr XMFLOAT3 kWhite{1.0f, 1.0f, 1.0f};

// How far the player can reach to grab, and how far off the centre of the gaze a
// prop may sit and still be grabbed. 0.80 is about 37 degrees, forgiving enough
// that a small object on the ground does not have to be pixel-centred.
constexpr float kReach = 2.4f;
constexpr float kMinAlignment = 0.80f;

// A dropped object is set down this far in front of the eye, along the gaze
// flattened to the ground. The radius is the footprint the support test uses to
// find the surface under it -- small, because these objects are.
constexpr float kDropDistance = 1.15f;
constexpr float kDropRadius = 0.06f;

// The interact key. Left-click is already the mouse-look toggle, so grabbing
// gets its own key, the shooter convention.
constexpr int kInteractKey = 'E';

// Rough density used to turn a box's volume into a mass. The absolute value
// barely matters -- gravity is mass-independent, and a bounce off the static
// world (infinite mass) does not depend on it either -- but it keeps the inertia
// tensor sensibly scaled for when props start knocking into each other.
constexpr float kDensity = 500.0f; // kg/m^3, a bit lighter than water.

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

} // namespace

Props::Props(const Scene& scene) {
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
        const XMMATRIX transform = XMLoadFloat4x4(&primitive.transform);
        const XMVECTOR lo = XMLoadFloat3(&primitive.bounds.min);
        const XMVECTOR hi = XMLoadFloat3(&primitive.bounds.max);
        // A node transform can rotate the bounds, so all eight corners are carried
        // across and re-bounded rather than just min and max.
        for (int corner = 0; corner < 8; ++corner) {
            const XMVECTOR point = XMVectorSelect(lo, hi, XMVectorSelectControl(corner & 1,
                                                                                (corner >> 1) & 1,
                                                                                (corner >> 2) & 1, 0));
            const XMVECTOR world = XMVector3TransformCoord(point, transform);
            box_min = XMVectorMin(box_min, world);
            box_max = XMVectorMax(box_max, world);
        }
    }

    const XMVECTOR half = XMVectorScale(XMVectorSubtract(box_max, box_min), 0.5f);
    const XMVECTOR center = XMVectorScale(XMVectorAdd(box_max, box_min), 0.5f);
    XMStoreFloat3(&item.half_extents, half);
    XMStoreFloat3(&item.com_offset, center);

    // Mass and inertia of a solid box. A degenerate extent (a perfectly flat
    // patty) is floored so the tensor stays invertible.
    XMFLOAT3 h;
    XMStoreFloat3(&h, XMVectorMax(half, XMVectorReplicate(1e-3f)));
    const float mass = kDensity * (2.0f * h.x) * (2.0f * h.y) * (2.0f * h.z);
    item.inv_mass = 1.0f / mass;
    // Solid box: I = (1/3) m (h_a^2 + h_b^2) about each axis, in half-extents.
    item.inv_inertia = {
        1.0f / ((mass / 3.0f) * (h.y * h.y + h.z * h.z)),
        1.0f / ((mass / 3.0f) * (h.x * h.x + h.z * h.z)),
        1.0f / ((mass / 3.0f) * (h.x * h.x + h.y * h.y)),
    };
}

void Props::RebuildTransform(Item& item) {
    // The body is stored about its centre of mass, but the model draws about its
    // own origin (on the underside). Undo the COM offset, rotate, then place:
    //   world(v) = (v - com_offset) * R(orientation) + position.
    const XMVECTOR com = XMLoadFloat3(&item.com_offset);
    const XMVECTOR q = XMLoadFloat4(&item.orientation);
    const XMVECTOR position = XMLoadFloat3(&item.position);
    const XMMATRIX m = XMMatrixTranslationFromVector(XMVectorNegate(com)) *
                       XMMatrixRotationQuaternion(q) *
                       XMMatrixTranslationFromVector(position);
    XMStoreFloat4x4(&item.resting, m);
}

void Props::Add(std::uint32_t model_id, const Model& model, std::string name, XMFLOAT3 position,
                float yaw_degrees, FXMMATRIX held_local) {
    Item item{};
    item.model = model_id;
    item.name = std::move(name);
    XMStoreFloat4x4(&item.held_local, held_local);
    DeriveBodyShape(item, model);

    // Seed the rigid state to match the old yaw-and-translate placement exactly:
    // the model origin lands at `position`, so the centre of mass lands at
    // com_offset carried through the yaw.
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(0.0f, XMConvertToRadians(yaw_degrees), 0.0f);
    const XMVECTOR com = XMLoadFloat3(&item.com_offset);
    const XMVECTOR origin = XMVectorSet(position.x, position.y, position.z, 0.0f);
    const XMVECTOR com_world =
        XMVectorAdd(XMVector3TransformNormal(com, XMMatrixRotationQuaternion(q)), origin);
    XMStoreFloat4(&item.orientation, q);
    XMStoreFloat3(&item.position, com_world);
    item.asleep = true;
    RebuildTransform(item);

    items_.push_back(std::move(item));
}

void Props::Simulate(float dt, std::span<const Aabb> colliders) {
    // Phase 1: every item spawns, and is dropped, asleep -- so the solver has
    // nothing to advance yet. Phase 2 replaces this body with gravity,
    // integration and contact resolution against `colliders`, waking an item when
    // it is dropped.
    (void)dt;
    (void)colliders;
}

void Props::Update(const XMMATRIX& camera_to_world, const Input& input,
                   std::span<const Aabb> colliders, float dt) {
    // The camera-to-world matrix is right, up, forward, eye as its four rows.
    const XMVECTOR eye = camera_to_world.r[3];
    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);

    // Edge-triggered: one grab per press, so holding E does not pick up and drop
    // on alternate frames. Key auto-repeat keeps the key "down", so this stays a
    // single event.
    const bool down = input.IsKeyDown(kInteractKey);
    if (down && !interact_was_down_) {
        if (carried_ >= 0) {
            Drop(eye, forward, colliders);
        } else {
            carried_ = PickTarget(eye, forward);
        }
    }
    interact_was_down_ = down;

    // What the prompt reports this frame: nothing to pick while carrying, else
    // whatever is in reach and looked at. Cheap enough to recompute outright for
    // a handful of items.
    hovered_ = carried_ >= 0 ? -1 : PickTarget(eye, forward);

    // Fall, bounce and settle any loose object that is in motion. Runs after the
    // grab so a just-dropped item is already awake, and before the draw lists so
    // they read its stepped pose.
    Simulate(dt, colliders);

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

void Props::Drop(FXMVECTOR eye, FXMVECTOR forward, std::span<const Aabb> colliders) {
    // The drop spot follows the gaze flattened to the ground, so looking down
    // sets the object at the player's feet rather than burying it underfoot, and
    // looking up still lays it out in front.
    XMVECTOR flat = XMVectorSetY(forward, 0.0f);
    if (XMVectorGetX(XMVector3LengthSq(flat)) < 1e-6f) {
        flat = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f); // Gaze was dead vertical.
    }
    flat = XMVector3Normalize(flat);

    const XMVECTOR spot = XMVectorAdd(eye, XMVectorScale(flat, kDropDistance));
    const float x = XMVectorGetX(spot);
    const float z = XMVectorGetZ(spot);

    // Rest on whatever is under the spot, ignoring anything above eye height, so
    // a steak can be set on the table or the grill lid but not pushed through a
    // fence. Nothing there means it goes to the ground.
    float y = HighestSupportUnder(x, z, kDropRadius, XMVectorGetY(eye), colliders);
    if (y == -FLT_MAX) {
        y = 0.0f;
    }

    // Face the object along the gaze, upright, so it lands the way it was held.
    const float yaw = std::atan2(XMVectorGetX(flat), XMVectorGetZ(flat));

    // Seed the rigid state so the model origin lands at (x, y, z), matching the
    // old snap exactly. Phase 1 leaves it asleep and motionless; Phase 2 will
    // wake it here with a small toss so it can fall and tumble.
    Item& item = items_[carried_];
    const XMVECTOR q = XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f);
    const XMVECTOR com = XMLoadFloat3(&item.com_offset);
    const XMVECTOR origin = XMVectorSet(x, y, z, 0.0f);
    const XMVECTOR com_world =
        XMVectorAdd(XMVector3TransformNormal(com, XMMatrixRotationQuaternion(q)), origin);
    XMStoreFloat4(&item.orientation, q);
    XMStoreFloat3(&item.position, com_world);
    item.linear_velocity = {0.0f, 0.0f, 0.0f};
    item.angular_velocity = {0.0f, 0.0f, 0.0f};
    item.asleep = true;
    RebuildTransform(item);

    carried_ = -1;
}
