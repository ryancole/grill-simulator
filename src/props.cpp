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

// One awake body being solved this substep, its rigid state lifted into
// registers, its box's world rotation, and the pseudo-velocity that repairs
// penetration. `contacts` counts how many contacts touched it, so the caller
// knows whether it is resting on something and may be put to sleep.
struct Body {
    int item;                  // index into Props::items_
    DirectX::XMVECTOR position;
    DirectX::XMVECTOR orientation;
    DirectX::XMVECTOR velocity;
    DirectX::XMVECTOR omega;    // world-space angular velocity
    DirectX::XMMATRIX rotation; // world rotation of the box
    DirectX::XMVECTOR inv_inertia;
    float inv_mass;
    DirectX::XMFLOAT3 half_extents;
    DirectX::XMVECTOR pseudo_linear;
    DirectX::XMVECTOR pseudo_angular;
    int contacts;
};

// The oriented box of an uncarried item, whether it is awake or asleep. Awake
// ones are solved; asleep ones are still surfaces an awake body can rest on.
struct BoxFrame {
    DirectX::XMVECTOR center;    // centre of mass, world
    DirectX::XMMATRIX rotation;  // world rotation
    DirectX::XMFLOAT3 half;
};

// Applies a body's inverse inertia tensor to a world-space vector: rotate into
// body axes, scale by the diagonal, rotate back.
XMVECTOR ApplyInvInertia(const Body& b, XMVECTOR world) {
    XMVECTOR local = XMVector3TransformNormal(world, XMMatrixTranspose(b.rotation));
    local = XMVectorMultiply(local, b.inv_inertia);
    return XMVector3TransformNormal(local, b.rotation);
}

// Whether `corner` has penetrated the box (`center`, `rotation`, `half`), and if
// so out which face. `corner_prev` is where the corner sat one substep ago, in
// the box's frame of reference, so a corner is pushed back out the side it came
// from -- see the long note in Step. Returns the world-space outward `normal`
// and the penetration `depth`.
bool BoxContact(FXMVECTOR corner, FXMVECTOR corner_prev, FXMVECTOR center, FXMMATRIX rotation,
                XMFLOAT3 half, XMVECTOR& normal, float& depth) {
    // Into the box's local frame, where it is axis-aligned about the origin.
    const XMMATRIX to_local = XMMatrixTranspose(rotation);
    XMFLOAT3 lc, pv;
    XMStoreFloat3(&lc, XMVector3TransformNormal(XMVectorSubtract(corner, center), to_local));
    XMStoreFloat3(&pv, XMVector3TransformNormal(XMVectorSubtract(corner_prev, center), to_local));

    const float local[3] = {lc.x, lc.y, lc.z};
    const float prev[3] = {pv.x, pv.y, pv.z};
    const float hb[3] = {half.x, half.y, half.z};
    for (int a = 0; a < 3; ++a) {
        if (local[a] <= -hb[a] || local[a] >= hb[a]) {
            return false; // outside on this axis: not penetrating.
        }
    }

    int best_axis = -1;
    float best_depth = FLT_MAX;
    float best_sign = 0.0f;
    for (int a = 0; a < 3; ++a) {
        const float sign = prev[a] >= 0.0f ? 1.0f : -1.0f; // side the corner came from
        const float face_depth = hb[a] - sign * local[a];  // distance out through that face
        if (face_depth < best_depth) {
            best_depth = face_depth;
            best_axis = a;
            best_sign = sign;
        }
    }

    const XMVECTOR local_normal =
        XMVectorSet(best_axis == 0 ? best_sign : 0.0f, best_axis == 1 ? best_sign : 0.0f,
                    best_axis == 2 ? best_sign : 0.0f, 0.0f);
    normal = XMVector3TransformNormal(local_normal, rotation);
    depth = best_depth;
    return true;
}

// One point of a box-box contact manifold: a world position and how far the two
// boxes overlap there.
struct ManifoldPoint {
    DirectX::XMVECTOR point;
    float depth;
};

// The contact manifold between two oriented boxes, by face-axis SAT and face
// clipping. Returns 0 if they are disjoint, else up to four contact points --
// enough for a flat stack to have real support rather than one wobbly point.
// On a hit `normal` points from B into A (the direction that separates A off B).
//
// Only the six face axes are tested, not the nine edge-cross axes; a face
// contact is what stacking needs, and the props are flat enough that a pure
// edge-edge crossing is rare. That keeps this to the reference/incident clip
// below instead of a full 15-axis solver.
int BoxBoxManifold(const BoxFrame& A, const BoxFrame& B, XMVECTOR& normal, ManifoldPoint points[4]) {
    auto dot = [](FXMVECTOR a, FXMVECTOR b) { return XMVectorGetX(XMVector3Dot(a, b)); };

    const XMVECTOR axis_a[3] = {A.rotation.r[0], A.rotation.r[1], A.rotation.r[2]};
    const XMVECTOR axis_b[3] = {B.rotation.r[0], B.rotation.r[1], B.rotation.r[2]};
    const float half_a[3] = {A.half.x, A.half.y, A.half.z};
    const float half_b[3] = {B.half.x, B.half.y, B.half.z};
    const XMVECTOR d = XMVectorSubtract(B.center, A.center); // A -> B

    // Smallest overlap over the six face normals; a gap on any axis means no hit.
    float min_overlap = FLT_MAX;
    XMVECTOR axis_ab = XMVectorZero(); // separating axis, oriented A -> B
    bool ref_is_a = true;
    int ref_index = 0;
    const XMVECTOR* axes[2] = {axis_a, axis_b};
    for (int box = 0; box < 2; ++box) {
        for (int i = 0; i < 3; ++i) {
            const XMVECTOR L = axes[box][i]; // a rotation row, already unit length
            const float radius_a = std::fabs(dot(axis_a[0], L)) * half_a[0] +
                                    std::fabs(dot(axis_a[1], L)) * half_a[1] +
                                    std::fabs(dot(axis_a[2], L)) * half_a[2];
            const float radius_b = std::fabs(dot(axis_b[0], L)) * half_b[0] +
                                    std::fabs(dot(axis_b[1], L)) * half_b[1] +
                                    std::fabs(dot(axis_b[2], L)) * half_b[2];
            const float dist = dot(d, L);
            const float overlap = radius_a + radius_b - std::fabs(dist);
            if (overlap < 0.0f) {
                return 0; // a separating axis
            }
            if (overlap < min_overlap) {
                min_overlap = overlap;
                axis_ab = dist < 0.0f ? XMVectorNegate(L) : L;
                ref_is_a = box == 0;
                ref_index = i;
            }
        }
    }
    normal = XMVectorNegate(axis_ab); // from B into A

    // The reference box owns the separating face; the incident box is the other.
    const BoxFrame& ref = ref_is_a ? A : B;
    const BoxFrame& inc = ref_is_a ? B : A;
    const XMVECTOR* ref_axes = ref_is_a ? axis_a : axis_b;
    const XMVECTOR* inc_axes = ref_is_a ? axis_b : axis_a;
    const float* ref_half = ref_is_a ? half_a : half_b;
    const float* inc_half = ref_is_a ? half_b : half_a;
    const XMVECTOR ref_normal = ref_is_a ? axis_ab : XMVectorNegate(axis_ab); // out toward incident

    // Incident face: the incident box face most anti-parallel to the reference.
    int inc_index = 0;
    float most_anti = FLT_MAX;
    XMVECTOR inc_normal = XMVectorZero();
    for (int i = 0; i < 3; ++i) {
        const float dp = dot(inc_axes[i], ref_normal);
        if (dp < most_anti) {
            most_anti = dp;
            inc_index = i;
            inc_normal = inc_axes[i];
        }
        if (-dp < most_anti) {
            most_anti = -dp;
            inc_index = i;
            inc_normal = XMVectorNegate(inc_axes[i]);
        }
    }

    // Reference face: its centre and the two in-plane axes with their extents.
    const XMVECTOR ref_center = XMVectorAdd(ref.center, XMVectorScale(ref_normal, ref_half[ref_index]));
    const int ru = (ref_index + 1) % 3, rv = (ref_index + 2) % 3;
    const XMVECTOR u_axis = ref_axes[ru];
    const XMVECTOR v_axis = ref_axes[rv];
    const float u_ext = ref_half[ru], v_ext = ref_half[rv];

    // Incident face polygon: four world vertices.
    const XMVECTOR inc_center = XMVectorAdd(inc.center, XMVectorScale(inc_normal, inc_half[inc_index]));
    const int iu = (inc_index + 1) % 3, iv = (inc_index + 2) % 3;
    const XMVECTOR edge_u = XMVectorScale(inc_axes[iu], inc_half[iu]);
    const XMVECTOR edge_v = XMVectorScale(inc_axes[iv], inc_half[iv]);

    struct V {
        float u, v;
        XMVECTOR w;
    };
    V poly[8];
    int count = 0;
    const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int c = 0; c < 4; ++c) {
        const XMVECTOR w = XMVectorAdd(
            inc_center, XMVectorAdd(XMVectorScale(edge_u, corner[c][0]),
                                    XMVectorScale(edge_v, corner[c][1])));
        const XMVECTOR rel = XMVectorSubtract(w, ref_center);
        poly[count++] = {dot(rel, u_axis), dot(rel, v_axis), w};
    }

    // Sutherland-Hodgman clip of the incident face to the reference rectangle.
    auto clip = [](const V* in, int n, int axis, float sign, float ext, V* out) {
        int m = 0;
        for (int i = 0; i < n; ++i) {
            const V& cur = in[i];
            const V& prev = in[(i + n - 1) % n];
            const float cd = (axis == 0 ? cur.u : cur.v) * sign - ext; // > 0 outside
            const float pd = (axis == 0 ? prev.u : prev.v) * sign - ext;
            if ((cd <= 0.0f) != (pd <= 0.0f)) {
                const float t = pd / (pd - cd);
                out[m++] = {prev.u + t * (cur.u - prev.u), prev.v + t * (cur.v - prev.v),
                            XMVectorLerp(prev.w, cur.w, t)};
            }
            if (cd <= 0.0f) {
                out[m++] = cur;
            }
        }
        return m;
    };
    V buf[8];
    int n = clip(poly, count, 0, 1.0f, u_ext, buf);
    n = clip(buf, n, 0, -1.0f, u_ext, poly);
    n = clip(poly, n, 1, 1.0f, v_ext, buf);
    n = clip(buf, n, 1, -1.0f, v_ext, poly);

    // Keep the points that are actually below the reference face (penetrating).
    int out_count = 0;
    for (int i = 0; i < n && out_count < 4; ++i) {
        const float depth = -dot(XMVectorSubtract(poly[i].w, ref_center), ref_normal);
        if (depth > 0.0f) {
            points[out_count].point = poly[i].w;
            points[out_count].depth = depth;
            ++out_count;
        }
    }
    return out_count;
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
        const Aabb bounds = TransformBounds(primitive.bounds, XMLoadFloat4x4(&primitive.transform));
        box_min = XMVectorMin(box_min, XMLoadFloat3(&bounds.min));
        box_max = XMVectorMax(box_max, XMLoadFloat3(&bounds.max));
    }

    // Floor every extent so a perfectly flat mesh (or, defensively, a model with
    // no primitives at all) still gives a non-degenerate box: an invertible
    // inertia tensor and finite collision corners. The stored half-extents use
    // the same floored value, so the collider and the inertia agree.
    XMFLOAT3 h;
    XMStoreFloat3(&h, XMVectorMax(XMVectorScale(XMVectorSubtract(box_max, box_min), 0.5f),
                                  XMVectorReplicate(1e-3f)));
    const XMVECTOR center = XMVectorScale(XMVectorAdd(box_max, box_min), 0.5f);
    item.half_extents = h;
    XMStoreFloat3(&item.com_offset, center);

    // Mass and inertia of a solid box.
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
        Step(kSubstep, colliders);
        physics_accumulator_ -= kSubstep;
        ++ticks;
    }
    // A long stall is spent, not hoarded: dropping the remainder keeps the
    // simulation from sprinting to catch up after a hitch.
    if (ticks == kMaxSubsteps) {
        physics_accumulator_ = 0.0f;
    }

    // Rewrite the render transform of every loose object. Cheap for a handful,
    // and always correct -- including a body that settled and slept this frame.
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (i != carried_) {
            RebuildTransform(items_[i]);
        }
    }
}

void Props::Step(float h, std::span<const Aabb> colliders) {
    const int n = static_cast<int>(items_.size());

    // The oriented box of every uncarried item, awake or asleep. An asleep prop
    // is frozen, but it is still a surface an awake one can rest on, so its box
    // is needed here even though it is not itself solved.
    std::vector<BoxFrame> boxes(n);
    for (int i = 0; i < n; ++i) {
        if (i == carried_) {
            continue;
        }
        boxes[i].center = XMLoadFloat3(&items_[i].position);
        boxes[i].rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&items_[i].orientation));
        boxes[i].half = items_[i].half_extents;
    }

    // The world-space AABB of item i's box, for the broad wake test.
    auto world_aabb = [&](int i, XMFLOAT3& lo, XMFLOAT3& hi) {
        XMVECTOR mn = XMVectorReplicate(FLT_MAX);
        XMVECTOR mx = XMVectorReplicate(-FLT_MAX);
        const XMFLOAT3 hx = boxes[i].half;
        for (int c = 0; c < 8; ++c) {
            const XMVECTOR off = XMVectorSet((c & 1) ? hx.x : -hx.x, (c & 2) ? hx.y : -hx.y,
                                             (c & 4) ? hx.z : -hx.z, 0.0f);
            const XMVECTOR w =
                XMVectorAdd(boxes[i].center, XMVector3TransformNormal(off, boxes[i].rotation));
            mn = XMVectorMin(mn, w);
            mx = XMVectorMax(mx, w);
        }
        XMStoreFloat3(&lo, mn);
        XMStoreFloat3(&hi, mx);
    };

    // --- Wake sleepers a *moving* body overlaps -----------------------------
    // A neighbour barely moving does not disturb a rest -- that is what lets a
    // gently stacked pile settle and stay asleep -- but a thrown or falling prop
    // wakes whatever it lands among.
    for (int i = 0; i < n; ++i) {
        if (i == carried_ || items_[i].asleep) {
            continue;
        }
        const float lin = XMVectorGetX(XMVector3Length(XMLoadFloat3(&items_[i].linear_velocity)));
        const float ang = XMVectorGetX(XMVector3Length(XMLoadFloat3(&items_[i].angular_velocity)));
        if (lin < kLinearSleep && ang < kAngularSleep) {
            continue;
        }
        XMFLOAT3 lo_i, hi_i;
        world_aabb(i, lo_i, hi_i);
        for (int j = 0; j < n; ++j) {
            if (j == carried_ || !items_[j].asleep) {
                continue;
            }
            XMFLOAT3 lo_j, hi_j;
            world_aabb(j, lo_j, hi_j);
            if (lo_i.x <= hi_j.x && hi_i.x >= lo_j.x && lo_i.y <= hi_j.y && hi_i.y >= lo_j.y &&
                lo_i.z <= hi_j.z && hi_i.z >= lo_j.z) {
                items_[j].asleep = false;
                items_[j].rest_timer = 0.0f;
            }
        }
    }

    // --- Build and integrate the active set (awake, uncarried) --------------
    std::vector<int> slot(n, -1); // item index -> active-body slot, or -1
    std::vector<Body> bodies;
    bodies.reserve(n);
    for (int i = 0; i < n; ++i) {
        if (i == carried_ || items_[i].asleep) {
            continue;
        }
        Body b{};
        b.item = i;
        b.velocity = XMLoadFloat3(&items_[i].linear_velocity);
        b.omega = XMLoadFloat3(&items_[i].angular_velocity);
        b.position = XMLoadFloat3(&items_[i].position);
        b.orientation = XMLoadFloat4(&items_[i].orientation);
        b.inv_inertia = XMLoadFloat3(&items_[i].inv_inertia);
        b.inv_mass = items_[i].inv_mass;
        b.half_extents = items_[i].half_extents;
        b.pseudo_linear = XMVectorZero();
        b.pseudo_angular = XMVectorZero();
        b.contacts = 0;

        // Integrate velocity and pose. World-frame spin composes on the right in
        // this row-vector convention.
        b.velocity = XMVectorAdd(b.velocity, XMVectorSet(0.0f, -kGravity * h, 0.0f, 0.0f));
        b.position = XMVectorAdd(b.position, XMVectorScale(b.velocity, h));
        const float spin = XMVectorGetX(XMVector3Length(b.omega));
        if (spin > 1e-6f) {
            const XMVECTOR axis = XMVectorScale(b.omega, 1.0f / spin);
            const XMVECTOR delta = XMQuaternionRotationAxis(axis, spin * h);
            b.orientation = XMQuaternionNormalize(XMQuaternionMultiply(b.orientation, delta));
        }
        b.rotation = XMMatrixRotationQuaternion(b.orientation);
        boxes[i].center = b.position; // keep this body's box current for contacts
        boxes[i].rotation = b.rotation;

        slot[i] = static_cast<int>(bodies.size());
        bodies.push_back(b);
    }

    // --- Gather every contact -----------------------------------------------
    struct Contact {
        int a;           // active body slot; always solved
        int b;           // active body slot, or -1 for a static surface
        XMVECTOR normal; // world, out of the surface into body a
        XMVECTOR ra;     // contact point - bodies[a].position
        XMVECTOR rb;     // contact point - bodies[b].position (unused if b < 0)
        float depth;
        float k; // effective mass along the normal
    };
    std::vector<Contact> contacts;

    // The effective mass at a contact along `nrm`: one body if `sb` is a static
    // surface (< 0), two bodies otherwise.
    auto effective_mass = [&](int sa, int sb, FXMVECTOR nrm, FXMVECTOR ra, FXMVECTOR rb) {
        const Body& A = bodies[sa];
        float k = A.inv_mass + XMVectorGetX(XMVector3Dot(
                                   XMVector3Cross(ApplyInvInertia(A, XMVector3Cross(ra, nrm)), ra),
                                   nrm));
        if (sb >= 0) {
            const Body& B = bodies[sb];
            k += B.inv_mass +
                 XMVectorGetX(XMVector3Dot(
                     XMVector3Cross(ApplyInvInertia(B, XMVector3Cross(rb, nrm)), rb), nrm));
        }
        return k;
    };

    // Each awake body's eight corners against the static world. A corner is
    // pushed back out the face it came in through (its previous position, a
    // substep ago), not the nearest face -- so a thin prop sunk into a thick slab
    // is lifted back out the top it fell through, a rebounding corner is never
    // flung down through the far side, and per-axis resolution stops a prop
    // sliding flat on the floor from being flung out a distant wall.
    for (int sa = 0; sa < static_cast<int>(bodies.size()); ++sa) {
        const XMFLOAT3 hx = bodies[sa].half_extents;
        const XMVECTOR a_pos = bodies[sa].position;
        for (int corner = 0; corner < 8; ++corner) {
            const XMVECTOR off = XMVectorSet((corner & 1) ? hx.x : -hx.x, (corner & 2) ? hx.y : -hx.y,
                                             (corner & 4) ? hx.z : -hx.z, 0.0f);
            const XMVECTOR point =
                XMVectorAdd(a_pos, XMVector3TransformNormal(off, bodies[sa].rotation));
            const XMVECTOR ra = XMVectorSubtract(point, a_pos);
            const XMVECTOR corner_velocity =
                XMVectorAdd(bodies[sa].velocity, XMVector3Cross(bodies[sa].omega, ra));
            const XMVECTOR prev_world = XMVectorSubtract(point, XMVectorScale(corner_velocity, h));

            XMVECTOR normal;
            float depth;
            for (const Aabb& box : colliders) {
                const XMVECTOR lo = XMLoadFloat3(&box.min);
                const XMVECTOR hi = XMLoadFloat3(&box.max);
                XMFLOAT3 half;
                XMStoreFloat3(&half, XMVectorScale(XMVectorSubtract(hi, lo), 0.5f));
                const XMVECTOR center = XMVectorScale(XMVectorAdd(lo, hi), 0.5f);
                if (BoxContact(point, prev_world, center, XMMatrixIdentity(), half, normal, depth)) {
                    const float k = effective_mass(sa, -1, normal, ra, XMVectorZero());
                    contacts.push_back({sa, -1, normal, ra, XMVectorZero(), depth, k});
                    ++bodies[sa].contacts;
                }
            }
        }
    }

    // Prop against prop: a face-clipping manifold, so two boxes of any size stack
    // and rest without interpenetrating -- something the per-corner test cannot
    // do, since neither box's corners need lie inside the other. Each awake body
    // meets every other loose object once; an asleep neighbour reacts to nothing
    // (a static surface, sb = -1) until a moving body wakes it in the pass above.
    for (int sa = 0; sa < static_cast<int>(bodies.size()); ++sa) {
        const int item_a = bodies[sa].item;
        for (int j = 0; j < n; ++j) {
            if (j == item_a || j == carried_) {
                continue;
            }
            const int sb = slot[j];
            if (sb >= 0 && sb <= sa) {
                continue; // this awake pair was already handled with the lower slot as A
            }
            XMVECTOR normal;
            ManifoldPoint pts[4];
            const int np = BoxBoxManifold(boxes[item_a], boxes[j], normal, pts);
            for (int p = 0; p < np; ++p) {
                const XMVECTOR ra = XMVectorSubtract(pts[p].point, bodies[sa].position);
                const XMVECTOR rb = XMVectorSubtract(pts[p].point, boxes[j].center);
                const float k = effective_mass(sa, sb, normal, ra, rb);
                contacts.push_back({sa, sb, normal, ra, rb, pts[p].depth, k});
                ++bodies[sa].contacts;
                if (sb >= 0) {
                    ++bodies[sb].contacts;
                }
            }
        }
    }

    // --- Resolve contact velocities: two-body sequential impulses -----------
    for (int iteration = 0; iteration < kSolverIterations; ++iteration) {
        for (const Contact& c : contacts) {
            if (c.k <= 0.0f) {
                continue;
            }
            const XMVECTOR n = c.normal;
            XMVECTOR va = XMVectorAdd(bodies[c.a].velocity, XMVector3Cross(bodies[c.a].omega, c.ra));
            XMVECTOR vb = XMVectorZero();
            if (c.b >= 0) {
                vb = XMVectorAdd(bodies[c.b].velocity, XMVector3Cross(bodies[c.b].omega, c.rb));
            }
            const float vn = XMVectorGetX(XMVector3Dot(XMVectorSubtract(va, vb), n));

            const float restitution = vn < -kRestitutionThreshold ? kRestitution : 0.0f;
            const float jn = std::max((-(1.0f + restitution) * vn) / c.k, 0.0f);
            const XMVECTOR impulse = XMVectorScale(n, jn);
            bodies[c.a].velocity =
                XMVectorAdd(bodies[c.a].velocity, XMVectorScale(impulse, bodies[c.a].inv_mass));
            bodies[c.a].omega = XMVectorAdd(bodies[c.a].omega,
                                            ApplyInvInertia(bodies[c.a], XMVector3Cross(c.ra, impulse)));
            if (c.b >= 0) {
                bodies[c.b].velocity =
                    XMVectorSubtract(bodies[c.b].velocity, XMVectorScale(impulse, bodies[c.b].inv_mass));
                bodies[c.b].omega = XMVectorSubtract(
                    bodies[c.b].omega, ApplyInvInertia(bodies[c.b], XMVector3Cross(c.rb, impulse)));
            }

            // Friction, clamped to the normal impulse by the Coulomb cone.
            va = XMVectorAdd(bodies[c.a].velocity, XMVector3Cross(bodies[c.a].omega, c.ra));
            vb = XMVectorZero();
            if (c.b >= 0) {
                vb = XMVectorAdd(bodies[c.b].velocity, XMVector3Cross(bodies[c.b].omega, c.rb));
            }
            const XMVECTOR vrel = XMVectorSubtract(va, vb);
            const XMVECTOR tangent_velocity =
                XMVectorSubtract(vrel, XMVectorScale(n, XMVectorGetX(XMVector3Dot(vrel, n))));
            const float tangent_speed = XMVectorGetX(XMVector3Length(tangent_velocity));
            if (tangent_speed > 1e-5f) {
                const XMVECTOR t = XMVectorScale(tangent_velocity, 1.0f / tangent_speed);
                const float kt = effective_mass(c.a, c.b, t, c.ra, c.rb);
                if (kt > 0.0f) {
                    float jt = -tangent_speed / kt;
                    jt = std::clamp(jt, -kFriction * jn, kFriction * jn);
                    const XMVECTOR friction = XMVectorScale(t, jt);
                    bodies[c.a].velocity = XMVectorAdd(bodies[c.a].velocity,
                                                       XMVectorScale(friction, bodies[c.a].inv_mass));
                    bodies[c.a].omega = XMVectorAdd(
                        bodies[c.a].omega, ApplyInvInertia(bodies[c.a], XMVector3Cross(c.ra, friction)));
                    if (c.b >= 0) {
                        bodies[c.b].velocity = XMVectorSubtract(
                            bodies[c.b].velocity, XMVectorScale(friction, bodies[c.b].inv_mass));
                        bodies[c.b].omega = XMVectorSubtract(
                            bodies[c.b].omega,
                            ApplyInvInertia(bodies[c.b], XMVector3Cross(c.rb, friction)));
                    }
                }
            }
        }
    }

    // --- Work off penetration with a separate pseudo-velocity ---------------
    // Split impulse: the same maths driven into a pseudo motion applied to the
    // pose and then discarded, so correcting overlap never becomes real energy.
    for (int iteration = 0; iteration < kSolverIterations; ++iteration) {
        for (const Contact& c : contacts) {
            if (c.k <= 0.0f) {
                continue;
            }
            const XMVECTOR n = c.normal;
            const float target =
                (kBaumgarte / h) * std::max(c.depth - kPenetrationSlop, 0.0f);
            XMVECTOR pa =
                XMVectorAdd(bodies[c.a].pseudo_linear, XMVector3Cross(bodies[c.a].pseudo_angular, c.ra));
            XMVECTOR pb = XMVectorZero();
            if (c.b >= 0) {
                pb = XMVectorAdd(bodies[c.b].pseudo_linear,
                                 XMVector3Cross(bodies[c.b].pseudo_angular, c.rb));
            }
            const float current = XMVectorGetX(XMVector3Dot(XMVectorSubtract(pa, pb), n));
            const float jp = std::max((target - current) / c.k, 0.0f);
            const XMVECTOR impulse = XMVectorScale(n, jp);
            bodies[c.a].pseudo_linear =
                XMVectorAdd(bodies[c.a].pseudo_linear, XMVectorScale(impulse, bodies[c.a].inv_mass));
            bodies[c.a].pseudo_angular = XMVectorAdd(
                bodies[c.a].pseudo_angular, ApplyInvInertia(bodies[c.a], XMVector3Cross(c.ra, impulse)));
            if (c.b >= 0) {
                bodies[c.b].pseudo_linear = XMVectorSubtract(
                    bodies[c.b].pseudo_linear, XMVectorScale(impulse, bodies[c.b].inv_mass));
                bodies[c.b].pseudo_angular = XMVectorSubtract(
                    bodies[c.b].pseudo_angular,
                    ApplyInvInertia(bodies[c.b], XMVector3Cross(c.rb, impulse)));
            }
        }
    }

    // --- Write each body back and put settled ones to sleep -----------------
    for (const Body& b : bodies) {
        XMVECTOR position = XMVectorAdd(b.position, XMVectorScale(b.pseudo_linear, h));
        XMVECTOR orientation = b.orientation;
        const float pseudo_spin = XMVectorGetX(XMVector3Length(b.pseudo_angular));
        if (pseudo_spin > 1e-6f) {
            const XMVECTOR axis = XMVectorScale(b.pseudo_angular, 1.0f / pseudo_spin);
            const XMVECTOR delta = XMQuaternionRotationAxis(axis, pseudo_spin * h);
            orientation = XMQuaternionNormalize(XMQuaternionMultiply(orientation, delta));
        }

        Item& item = items_[b.item];
        XMStoreFloat3(&item.position, position);
        XMStoreFloat4(&item.orientation, orientation);
        XMStoreFloat3(&item.linear_velocity, b.velocity);
        XMStoreFloat3(&item.angular_velocity, b.omega);

        // Park a body that has been slow while touching something. The contact
        // test stops it sleeping at the top of an arc, where speed also passes
        // through zero but nothing is supporting it.
        const float lin = XMVectorGetX(XMVector3Length(b.velocity));
        const float ang = XMVectorGetX(XMVector3Length(b.omega));
        if (b.contacts > 0 && lin < kLinearSleep && ang < kAngularSleep) {
            item.rest_timer += h;
            if (item.rest_timer >= kSleepTime) {
                item.asleep = true;
                item.linear_velocity = {0.0f, 0.0f, 0.0f};
                item.angular_velocity = {0.0f, 0.0f, 0.0f};
            }
        } else {
            item.rest_timer = 0.0f;
        }
    }
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
