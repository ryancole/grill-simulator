#include "physics.hpp"

#include <PxPhysicsAPI.h>

#include <algorithm>

using namespace physx;

namespace {

constexpr float kGravity = 20.0f;         // m/s^2 down; punchier than 9.81.
constexpr float kSubstep = 1.0f / 120.0f; // fixed physics tick.
constexpr int kMaxSubsteps = 8;           // catch-up cap, so a stall never bursts.

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

    PxSceneDesc desc(physics_->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -kGravity, 0.0f);
    desc.cpuDispatcher = dispatcher_;
    desc.filterShader = PxDefaultSimulationFilterShader;
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
