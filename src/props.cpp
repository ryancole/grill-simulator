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

// The interact key. Left-click is already the mouse-look toggle, so grabbing
// gets its own key, the shooter convention.
constexpr int kInteractKey = 'E';

// Rough density used to turn a box's volume into a mass. The absolute value
// barely matters -- gravity is mass-independent, and a bounce off the static
// world (infinite mass) does not depend on it either -- but it keeps the inertia
// tensor sensibly scaled for when props start knocking into each other.
constexpr float kDensity = 500.0f; // kg/m^3, a bit lighter than water.

// The rigid-body solver. It runs on a fixed substep so the integration and the
// contact impulses stay stable however long a rendered frame took.
constexpr float kGravity = 20.0f;         // m/s^2 down. Punchier than 9.81, to
                                          // match the camera's snappy fall.
constexpr float kSubstep = 1.0f / 120.0f; // fixed physics tick.
constexpr int kMaxSubsteps = 8;           // catch-up cap, so a stall never bursts.
constexpr int kSolverIterations = 10;     // sequential-impulse passes per tick.

constexpr float kRestitution = 0.30f;          // how bouncy a hard hit is.
constexpr float kRestitutionThreshold = 0.6f;  // below this closing speed, no
                                               // bounce -- keeps a settling box
                                               // from buzzing on the surface.
constexpr float kFriction = 0.55f;             // Coulomb coefficient at contacts.
constexpr float kBaumgarte = 0.8f;             // fraction of penetration worked off
                                               // per tick by the pseudo-velocity.
                                               // High is fine -- it is pose-only and
                                               // adds no energy -- and keeps a fast
                                               // impact from clipping through before
                                               // it is pushed back out.
constexpr float kPenetrationSlop = 0.002f;     // overlap left uncorrected, so a
                                               // resting box does not jitter.

// A body slower than these for kSleepTime while touching something is parked, so
// resting props cost nothing and never buzz.
constexpr float kLinearSleep = 0.06f;   // m/s
constexpr float kAngularSleep = 0.20f;  // rad/s
constexpr float kSleepTime = 0.35f;     // seconds

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
    // Advance the simulation in fixed ticks, banking whatever real time is left
    // over for next frame. A fixed step is what keeps the impulse solver stable.
    physics_accumulator_ += dt;
    int ticks = 0;
    while (physics_accumulator_ >= kSubstep && ticks < kMaxSubsteps) {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
            Item& item = items_[i];
            if (item.asleep || i == carried_) {
                continue;
            }
            const int contacts = StepBody(item, kSubstep, colliders);

            // Park a body that has been slow while resting on something. The
            // contact test guards against sleeping at the top of an arc, where
            // speed also passes through zero but nothing is supporting it.
            const float linear = XMVectorGetX(XMVector3Length(XMLoadFloat3(&item.linear_velocity)));
            const float angular =
                XMVectorGetX(XMVector3Length(XMLoadFloat3(&item.angular_velocity)));
            if (contacts > 0 && linear < kLinearSleep && angular < kAngularSleep) {
                item.rest_timer += kSubstep;
                if (item.rest_timer >= kSleepTime) {
                    item.asleep = true;
                    item.linear_velocity = {0.0f, 0.0f, 0.0f};
                    item.angular_velocity = {0.0f, 0.0f, 0.0f};
                }
            } else {
                item.rest_timer = 0.0f;
            }
        }
        physics_accumulator_ -= kSubstep;
        ++ticks;
    }
    // A long stall is spent, not hoarded: dropping the remainder keeps the
    // simulation from sprinting to catch up after a hitch.
    if (ticks == kMaxSubsteps) {
        physics_accumulator_ = 0.0f;
    }

    // Rewrite the render transform of everything that is (or just was) moving.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i != carried_ && !items_[i].asleep) {
            RebuildTransform(items_[i]);
        }
    }
}

int Props::StepBody(Item& item, float h, std::span<const Aabb> colliders) const {
    // --- Integrate velocity and pose ---------------------------------------
    XMVECTOR velocity = XMLoadFloat3(&item.linear_velocity);
    XMVECTOR omega = XMLoadFloat3(&item.angular_velocity); // world space
    XMVECTOR position = XMLoadFloat3(&item.position);
    XMVECTOR orientation = XMLoadFloat4(&item.orientation);

    velocity = XMVectorAdd(velocity, XMVectorSet(0.0f, -kGravity * h, 0.0f, 0.0f));
    position = XMVectorAdd(position, XMVectorScale(velocity, h));

    // Turn the world-space angular velocity into a small rotation and apply it
    // after the current orientation (world-frame spin composes on the right in
    // this row-vector convention).
    const float spin = XMVectorGetX(XMVector3Length(omega));
    if (spin > 1e-6f) {
        const XMVECTOR axis = XMVectorScale(omega, 1.0f / spin);
        const XMVECTOR delta = XMQuaternionRotationAxis(axis, spin * h);
        orientation = XMQuaternionNormalize(XMQuaternionMultiply(orientation, delta));
    }

    const XMMATRIX rotation = XMMatrixRotationQuaternion(orientation);
    const XMVECTOR inv_inertia = XMLoadFloat3(&item.inv_inertia);

    // Maps a world-space vector through the body's inverse inertia tensor: rotate
    // it into body axes, scale by the diagonal, rotate back.
    const XMMATRIX rotation_t = XMMatrixTranspose(rotation);
    auto apply_inv_inertia = [&](XMVECTOR world) {
        XMVECTOR body = XMVector3TransformNormal(world, rotation_t);
        body = XMVectorMultiply(body, inv_inertia);
        return XMVector3TransformNormal(body, rotation);
    };

    // --- Gather contacts: each of the eight box corners against the world -----
    struct Contact {
        XMVECTOR normal; // unit, world; points out of the surface into the body.
        XMVECTOR r;      // contact point minus centre of mass, world.
        float depth;     // penetration, metres.
    };
    Contact contacts[64];
    int contact_count = 0;

    const XMFLOAT3 h_ext = item.half_extents;
    for (int corner = 0; corner < 8 && contact_count < 64; ++corner) {
        const XMVECTOR offset =
            XMVectorSet((corner & 1) ? h_ext.x : -h_ext.x, (corner & 2) ? h_ext.y : -h_ext.y,
                        (corner & 4) ? h_ext.z : -h_ext.z, 0.0f);
        const XMVECTOR point = XMVectorAdd(position, XMVector3TransformNormal(offset, rotation));
        XMFLOAT3 p;
        XMStoreFloat3(&p, point);

        for (const Aabb& box : colliders) {
            // Only a corner strictly inside a solid box is penetrating it.
            if (p.x <= box.min.x || p.x >= box.max.x || p.y <= box.min.y || p.y >= box.max.y ||
                p.z <= box.min.z || p.z >= box.max.z) {
                continue;
            }
            // Pick the face to push the corner out of, one axis at a time, then
            // take the shallowest across the three axes. On each axis the corner
            // is sent back out the face it came IN through -- the one it is moving
            // away from into the box -- not the nearer face. This is what keeps a
            // thin patty that has sunk into the thick patio slab from being shoved
            // out the slab's *bottom*: it fell in through the top, so it is lifted
            // back out the top, however deep it got. Resolving per axis (rather
            // than over all six faces at once) means a prop sliding flat along the
            // floor can never be flung out a far side wall it happens to be moving
            // toward -- only the near, shallow floor face wins.
            const XMVECTOR r = XMVectorSubtract(point, position);
            XMFLOAT3 cv;
            XMStoreFloat3(&cv, XMVectorAdd(velocity, XMVector3Cross(omega, r)));

            const float box_min[3] = {box.min.x, box.min.y, box.min.z};
            const float box_max[3] = {box.max.x, box.max.y, box.max.z};
            const float pc[3] = {p.x, p.y, p.z};
            const float vc[3] = {cv.x, cv.y, cv.z};

            int best_axis = -1;
            float best_depth = FLT_MAX;
            float best_sign = 0.0f;
            for (int a = 0; a < 3; ++a) {
                float sign;
                float depth;
                if (vc[a] < -1e-4f) { // moving toward -a: entered through the +a face
                    sign = 1.0f;
                    depth = box_max[a] - pc[a];
                } else if (vc[a] > 1e-4f) { // moving toward +a: entered through -a
                    sign = -1.0f;
                    depth = pc[a] - box_min[a];
                } else if (box_max[a] - pc[a] < pc[a] - box_min[a]) { // still: nearer face
                    sign = 1.0f;
                    depth = box_max[a] - pc[a];
                } else {
                    sign = -1.0f;
                    depth = pc[a] - box_min[a];
                }
                if (depth < best_depth) {
                    best_depth = depth;
                    best_axis = a;
                    best_sign = sign;
                }
            }

            if (best_axis >= 0 && contact_count < 64) {
                const XMVECTOR normal =
                    XMVectorSet(best_axis == 0 ? best_sign : 0.0f, best_axis == 1 ? best_sign : 0.0f,
                                best_axis == 2 ? best_sign : 0.0f, 0.0f);
                contacts[contact_count++] = {normal, r, best_depth};
            }
        }
    }

    // --- Resolve contacts: sequential impulses with friction -----------------
    for (int iteration = 0; iteration < kSolverIterations; ++iteration) {
        for (int c = 0; c < contact_count; ++c) {
            const XMVECTOR n = contacts[c].normal;
            const XMVECTOR r = contacts[c].r;

            // Velocity of the material point at the contact.
            XMVECTOR point_velocity = XMVectorAdd(velocity, XMVector3Cross(omega, r));
            const float vn = XMVectorGetX(XMVector3Dot(point_velocity, n));

            // Effective mass along the normal.
            const XMVECTOR rn = XMVector3Cross(r, n);
            const XMVECTOR angular_n = XMVector3Cross(apply_inv_inertia(rn), r);
            const float k = item.inv_mass + XMVectorGetX(XMVector3Dot(angular_n, n));
            if (k <= 0.0f) {
                continue;
            }

            // Restitution only for genuine impacts; a slow settle gets none.
            // Penetration is NOT corrected here -- doing that as a velocity would
            // pump energy into a settling box and tip it up onto an edge. It is
            // worked off separately below, as a pseudo-velocity that never touches
            // the real motion.
            const float restitution = vn < -kRestitutionThreshold ? kRestitution : 0.0f;
            float jn = (-(1.0f + restitution) * vn) / k;
            jn = std::max(jn, 0.0f); // contacts push, never pull.

            const XMVECTOR impulse = XMVectorScale(n, jn);
            velocity = XMVectorAdd(velocity, XMVectorScale(impulse, item.inv_mass));
            omega = XMVectorAdd(omega, apply_inv_inertia(XMVector3Cross(r, impulse)));

            // Friction: oppose the tangential motion, clamped to the normal
            // impulse by the Coulomb cone.
            point_velocity = XMVectorAdd(velocity, XMVector3Cross(omega, r));
            const XMVECTOR tangent_velocity = XMVectorSubtract(
                point_velocity, XMVectorScale(n, XMVectorGetX(XMVector3Dot(point_velocity, n))));
            const float tangent_speed = XMVectorGetX(XMVector3Length(tangent_velocity));
            if (tangent_speed > 1e-5f) {
                const XMVECTOR t = XMVectorScale(tangent_velocity, 1.0f / tangent_speed);
                const XMVECTOR rt = XMVector3Cross(r, t);
                const XMVECTOR angular_t = XMVector3Cross(apply_inv_inertia(rt), r);
                const float kt = item.inv_mass + XMVectorGetX(XMVector3Dot(angular_t, t));
                if (kt > 0.0f) {
                    float jt = -tangent_speed / kt;
                    jt = std::clamp(jt, -kFriction * jn, kFriction * jn);
                    const XMVECTOR friction = XMVectorScale(t, jt);
                    velocity = XMVectorAdd(velocity, XMVectorScale(friction, item.inv_mass));
                    omega = XMVectorAdd(omega, apply_inv_inertia(XMVector3Cross(r, friction)));
                }
            }
        }
    }

    // --- Work off penetration with a separate pseudo-velocity ----------------
    // Split-impulse position correction: the same impulse maths, but accumulated
    // into a pseudo motion that is applied to the pose and then thrown away, so it
    // never becomes kinetic energy. This is what lets a box settle instead of
    // bouncing itself onto a corner.
    XMVECTOR pseudo_linear = XMVectorZero();
    XMVECTOR pseudo_angular = XMVectorZero();
    for (int iteration = 0; iteration < kSolverIterations; ++iteration) {
        for (int c = 0; c < contact_count; ++c) {
            const XMVECTOR n = contacts[c].normal;
            const XMVECTOR r = contacts[c].r;

            const XMVECTOR rn = XMVector3Cross(r, n);
            const XMVECTOR angular_n = XMVector3Cross(apply_inv_inertia(rn), r);
            const float k = item.inv_mass + XMVectorGetX(XMVector3Dot(angular_n, n));
            if (k <= 0.0f) {
                continue;
            }

            // Drive the overlap (beyond a small allowed slop) to close over this
            // tick, and no faster.
            const float target =
                (kBaumgarte / h) * std::max(contacts[c].depth - kPenetrationSlop, 0.0f);
            const XMVECTOR pseudo_point =
                XMVectorAdd(pseudo_linear, XMVector3Cross(pseudo_angular, r));
            const float current = XMVectorGetX(XMVector3Dot(pseudo_point, n));
            const float jp = std::max((target - current) / k, 0.0f);

            const XMVECTOR impulse = XMVectorScale(n, jp);
            pseudo_linear = XMVectorAdd(pseudo_linear, XMVectorScale(impulse, item.inv_mass));
            pseudo_angular =
                XMVectorAdd(pseudo_angular, apply_inv_inertia(XMVector3Cross(r, impulse)));
        }
    }
    // Apply the accumulated correction to the pose only.
    position = XMVectorAdd(position, XMVectorScale(pseudo_linear, h));
    const float pseudo_spin = XMVectorGetX(XMVector3Length(pseudo_angular));
    if (pseudo_spin > 1e-6f) {
        const XMVECTOR axis = XMVectorScale(pseudo_angular, 1.0f / pseudo_spin);
        const XMVECTOR delta = XMQuaternionRotationAxis(axis, pseudo_spin * h);
        orientation = XMQuaternionNormalize(XMQuaternionMultiply(orientation, delta));
    }

    XMStoreFloat3(&item.linear_velocity, velocity);
    XMStoreFloat3(&item.angular_velocity, omega);
    XMStoreFloat3(&item.position, position);
    XMStoreFloat4(&item.orientation, orientation);
    return contact_count;
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
            Drop(camera_to_world);
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

void Props::Drop(FXMMATRIX camera_to_world) {
    Item& item = items_[carried_];

    // Let go at the exact pose the object was carried, so it falls out of the
    // hand rather than teleporting to a tidy spot. The held instance is drawn
    // under held_local * camera_to_world, so that same matrix places the body.
    const XMMATRIX held_world = XMLoadFloat4x4(&item.held_local) * camera_to_world;
    const XMVECTOR com_world = XMVector3TransformCoord(XMLoadFloat3(&item.com_offset), held_world);
    const XMVECTOR orientation = XMQuaternionNormalize(XMQuaternionRotationMatrix(held_world));

    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);
    const XMVECTOR right = XMVector3Normalize(camera_to_world.r[0]);

    XMStoreFloat3(&item.position, com_world);
    XMStoreFloat4(&item.orientation, orientation);
    // A gentle underarm toss down the gaze, plus a forward pitch about the
    // player's right so it tumbles over instead of gliding down flat.
    const XMVECTOR toss =
        XMVectorAdd(XMVectorScale(forward, kThrowSpeed), XMVectorSet(0.0f, -kThrowDrop, 0.0f, 0.0f));
    XMStoreFloat3(&item.linear_velocity, toss);
    XMStoreFloat3(&item.angular_velocity, XMVectorScale(right, kThrowSpin));
    item.rest_timer = 0.0f;
    item.asleep = false;
    RebuildTransform(item);

    carried_ = -1;
}
