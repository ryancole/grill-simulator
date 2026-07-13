#include "physics.hpp"

#include "rigid_body.hpp"

#include <PxPhysicsAPI.h>

#include <algorithm>

using namespace physx;

namespace {

constexpr float kGravity = 20.0f;         // m/s^2 down; punchier than 9.81.
constexpr float kSubstep = 1.0f / 120.0f; // fixed physics tick.
constexpr int kMaxSubsteps = 8;           // catch-up cap, so a stall never bursts.

// Contacts below this impulse magnitude are a body settling and jostling in place
// -- a resting patty nudged by its neighbour -- not a real landing, so they raise
// no sound. A drop from table height clears it comfortably. Tuned by ear; the
// units are PhysX impulse (kg*m/s over the substep).
constexpr float kMinImpactImpulse = 0.35f;

// The most contact points pulled from one pair. A box-on-box landing has four at
// most; this is headroom so extractContacts never has to truncate.
constexpr PxU32 kMaxContactPoints = 8;

// The filter shader the scene runs for every overlapping pair. It keeps PhysX's
// usual contact resolution but additionally asks for a touch-found notification
// with contact points, which is what makes the scene call onContact when two
// shapes first meet -- the default shader stays silent, reporting nothing. The
// callback then throws away every pair that has no meat in it, so tagging all
// pairs here costs only that quick check, not a sound.
PxFilterFlags ContactReportFilterShader(PxFilterObjectAttributes attributes0, PxFilterData,
                                        PxFilterObjectAttributes attributes1, PxFilterData,
                                        PxPairFlags& pair_flags, const void*, PxU32) {
    // No triggers are used in this game, but keep the standard guard so one added
    // later behaves as a trigger rather than a solid contact.
    if (PxFilterObjectIsTrigger(attributes0) || PxFilterObjectIsTrigger(attributes1)) {
        pair_flags = PxPairFlag::eTRIGGER_DEFAULT;
        return PxFilterFlag::eDEFAULT;
    }
    pair_flags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eNOTIFY_TOUCH_FOUND |
                 PxPairFlag::eNOTIFY_CONTACT_POINTS;
    return PxFilterFlag::eDEFAULT;
}

// Turns the solver's contact reports into sounding Impacts. The scene owns the sim
// callback pointer; this holds a pointer to Physics::impacts_ (a stable member
// address) and appends to it as pairs land. onContact runs inside fetchResults on
// the same thread that calls Step, so appending to the vector needs no locking.
class ContactReporter : public PxSimulationEventCallback {
public:
    explicit ContactReporter(std::vector<Impact>& out) : out_(out) {}

    void onContact(const PxContactPairHeader& header, const PxContactPair* pairs,
                   PxU32 count) override {
        // If either actor was removed this step its userData is gone; nothing to
        // sound for, and dereferencing it would be a use-after-free.
        if (header.flags &
            (PxContactPairHeaderFlag::eREMOVED_ACTOR_0 | PxContactPairHeaderFlag::eREMOVED_ACTOR_1)) {
            return;
        }
        // The sound belongs to whichever body makes noise -- meat or tongs. If both
        // in the pair do (a steak dropped onto the tongs), the first wins; the
        // static world and furniture leave userData null or None and stay quiet.
        const auto* tag0 = static_cast<const BodyTag*>(header.actors[0]->userData);
        const auto* tag1 = static_cast<const BodyTag*>(header.actors[1]->userData);
        ImpactSound sound = ImpactSound::None;
        if (tag0 != nullptr && tag0->impact_sound != ImpactSound::None) {
            sound = tag0->impact_sound;
        } else if (tag1 != nullptr && tag1->impact_sound != ImpactSound::None) {
            sound = tag1->impact_sound;
        }
        if (sound == ImpactSound::None) {
            return;
        }

        for (PxU32 i = 0; i < count; ++i) {
            const PxContactPair& pair = pairs[i];
            // Only the frame a touch begins, not every step it rests in contact --
            // otherwise a patty lying on the grate would splat continuously.
            if (!(pair.events & PxPairFlag::eNOTIFY_TOUCH_FOUND)) {
                continue;
            }

            PxContactPairPoint points[kMaxContactPoints];
            const PxU32 n = pair.extractContacts(points, kMaxContactPoints);
            if (n == 0) {
                continue;
            }

            // One event per landing, at the mean contact point and carrying the
            // summed impulse -- so a flat face slapping down reads louder than a
            // corner just clipping something.
            PxVec3 position(0.0f, 0.0f, 0.0f);
            float impulse = 0.0f;
            for (PxU32 p = 0; p < n; ++p) {
                position += points[p].position;
                impulse += points[p].impulse.magnitude();
            }
            if (impulse < kMinImpactImpulse) {
                continue;
            }
            position /= static_cast<float>(n);
            out_.push_back(
                Impact{DirectX::XMFLOAT3(position.x, position.y, position.z), impulse, sound});
        }
    }

    // Nothing else in the game listens for these, but they are pure virtual.
    void onTrigger(PxTriggerPair*, PxU32) override {}
    void onConstraintBreak(PxConstraintInfo*, PxU32) override {}
    void onWake(PxActor**, PxU32) override {}
    void onSleep(PxActor**, PxU32) override {}
    void onAdvance(const PxRigidBody* const*, const PxTransform*, PxU32) override {}

private:
    std::vector<Impact>& out_;
};

// The shared surface material. A little friction and a low restitution so the
// props grip when they land; PhysX applies its own restitution-velocity
// threshold, so the settling props still stop bouncing rather than jittering.
constexpr float kStaticFriction = 0.55f;
constexpr float kDynamicFriction = 0.55f;
constexpr float kRestitution = 0.30f;

// The CPU dispatcher's worker count. A handful of small boxes never needs more;
// two threads keep the solve off the render thread without oversubscribing.
constexpr PxU32 kWorkerThreads = 2;

} // namespace

Physics::Physics() {
    allocator_ = new PxDefaultAllocator();
    error_callback_ = new PxDefaultErrorCallback();

    foundation_ = PxCreateFoundation(PX_PHYSICS_VERSION, *allocator_, *error_callback_);

    // No PVD and no allocation tracking: this is a shipping single-player game,
    // not a physics authoring tool.
    physics_ = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation_, PxTolerancesScale());

    // PxRigidBodyExt (the box mass/inertia helper the props will use) and the
    // character-controller module both live in the extensions library.
    PxInitExtensions(*physics_, nullptr);

    dispatcher_ = PxDefaultCpuDispatcherCreate(kWorkerThreads);

    // The reporter is set on the scene below and appends to impacts_ as meat lands.
    // impacts_ is a member, so its address is stable for the reporter's lifetime.
    contact_reporter_ = new ContactReporter(impacts_);

    PxSceneDesc desc(physics_->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -kGravity, 0.0f);
    desc.cpuDispatcher = dispatcher_;
    // A filter shader that asks for touch-found reports, plus the callback that
    // turns them into meat splats; the stock shader would report no contacts.
    desc.filterShader = ContactReportFilterShader;
    desc.simulationEventCallback = contact_reporter_;
    scene_ = physics_->createScene(desc);

    material_ = physics_->createMaterial(kStaticFriction, kDynamicFriction, kRestitution);

    controllers_ = PxCreateControllerManager(*scene_);
}

Physics::~Physics() {
    // Release in the reverse of construction; each is null-checked so a partially
    // constructed Physics (an exception mid-ctor) still tears down cleanly.
    if (controllers_) {
        controllers_->release();
    }
    if (scene_) {
        scene_->release();
    }
    if (material_) {
        material_->release();
    }
    if (dispatcher_) {
        dispatcher_->release();
    }
    if (physics_) {
        PxCloseExtensions();
        physics_->release();
    }
    if (foundation_) {
        foundation_->release();
    }
    // The scene held a pointer to this, so it is freed only after the scene is gone.
    delete contact_reporter_;
    delete error_callback_;
    delete allocator_;
}

void Physics::AddStaticWorld(std::span<const OrientedBox> colliders) {
    for (const OrientedBox& box : colliders) {
        // A collider can be arbitrarily thin (the ground slab, a shelf); PhysX
        // rejects a non-positive box extent, so floor each half to a hair.
        const PxVec3 half(std::max(box.half_extents.x, 1e-3f),
                          std::max(box.half_extents.y, 1e-3f),
                          std::max(box.half_extents.z, 1e-3f));
        const PxTransform pose(
            PxVec3(box.center.x, box.center.y, box.center.z),
            PxQuat(box.orientation.x, box.orientation.y, box.orientation.z, box.orientation.w));
        PxRigidStatic* actor = physics_->createRigidStatic(pose);
        PxRigidActorExt::createExclusiveShape(*actor, PxBoxGeometry(half), *material_);
        scene_->addActor(*actor);
    }
}

PxRigidDynamic* Physics::AddDynamicBody(std::span<const OrientedBox> shapes,
                                        const DirectX::XMFLOAT4X4& initial_pose, float mass) {
    using namespace DirectX;

    // The body spawns at the model-to-world transform. It carries no scale (the
    // shapes are already sized), so the decomposition only wants the translation
    // and rotation; the scale is pulled out and dropped.
    XMVECTOR scale;
    XMVECTOR rotation;
    XMVECTOR translation;
    XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&initial_pose));
    XMFLOAT3 t;
    XMFLOAT4 q;
    XMStoreFloat3(&t, translation);
    XMStoreFloat4(&q, rotation);

    const PxTransform pose(PxVec3(t.x, t.y, t.z), PxQuat(q.x, q.y, q.z, q.w));
    PxRigidDynamic* body = physics_->createRigidDynamic(pose);

    // Each shape sits at its own model-space centre and orientation, so the
    // legs, body, lid and shelf keep their real arrangement and the mass is
    // distributed the way the object actually is -- weight up in the kettle, so
    // it topples off its narrow feet.
    for (const OrientedBox& box : shapes) {
        const PxVec3 half(std::max(box.half_extents.x, 1e-3f),
                          std::max(box.half_extents.y, 1e-3f),
                          std::max(box.half_extents.z, 1e-3f));
        const PxTransform local(
            PxVec3(box.center.x, box.center.y, box.center.z),
            PxQuat(box.orientation.x, box.orientation.y, box.orientation.z, box.orientation.w));
        PxShape* shape = PxRigidActorExt::createExclusiveShape(*body, PxBoxGeometry(half),
                                                               *material_);
        shape->setLocalPose(local);
    }

    // Total mass fixed; PhysX computes the centre of mass and inertia tensor from
    // the shapes so the distribution -- not just the number -- drives the tumble.
    PxRigidBodyExt::setMassAndUpdateInertia(*body, mass);

    // A little damping so a toppled grill rocks to a stop instead of sliding on
    // for ever, and a capped depenetration speed so any small overlap with the
    // patio it spawns on eases out rather than flinging it into the air.
    body->setLinearDamping(0.15f);
    body->setAngularDamping(0.4f);
    body->setMaxDepenetrationVelocity(1.5f);

    scene_->addActor(*body);
    return body;
}

void Physics::Step(float dt) {
    // Last frame's impacts have been sounded; start the step with an empty list so
    // onContact (called from fetchResults below) fills it with only this step's.
    impacts_.clear();

    // Fixed ticks with the leftover time banked for next frame -- a fixed step is
    // what keeps the solver stable. A long stall is spent, not hoarded: once the
    // catch-up cap is hit the remainder is dropped so the sim never sprints to
    // catch up after a hitch.
    accumulator_ += dt;
    int ticks = 0;
    while (accumulator_ >= kSubstep && ticks < kMaxSubsteps) {
        scene_->simulate(kSubstep);
        scene_->fetchResults(true);
        accumulator_ -= kSubstep;
        ++ticks;
    }
    if (ticks == kMaxSubsteps) {
        accumulator_ = 0.0f;
    }
}
