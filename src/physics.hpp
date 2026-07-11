#pragma once

#include "collision.hpp"

#include <span>

// PhysX owns the simulation from here on. This class brings the SDK up, holds
// the one scene every rigid body lives in, and steps it on a fixed clock. The
// hand-rolled sequential-impulse solver in props.cpp and the AABB character
// sweep in collision.cpp are both being retired onto it; this is the empty shell
// they will hang off (step 1 of that migration -- nothing is added to the scene
// yet).
//
// PhysX types are forward-declared so this header stays cheap to include: only a
// translation unit that actually touches the scene, a material or an actor pulls
// in <PxPhysicsAPI.h>. Everything the SDK hands back is a raw pointer with an
// SDK-managed lifetime, so the members are raw pointers too and the destructor
// releases them in reverse.

namespace physx {
class PxDefaultAllocator;
class PxDefaultErrorCallback;
class PxFoundation;
class PxPhysics;
class PxDefaultCpuDispatcher;
class PxScene;
class PxMaterial;
class PxControllerManager;
} // namespace physx

// Brings PhysX up in the constructor and tears it back down with the object's
// lifetime. One of these lives in Game, constructed once near the top so it
// outlives everything that will register bodies with it.
class Physics {
public:
    Physics();
    ~Physics();

    Physics(const Physics&) = delete;
    Physics& operator=(const Physics&) = delete;

    // Advances the simulation over `dt` seconds in fixed substeps, banking the
    // remainder for next frame -- the same fixed-clock discipline the old solver
    // used (see Props::Simulate), which is what keeps the contact solve stable
    // however long a rendered frame took.
    void Step(float dt);

    // Builds the immovable world: one static box actor per collider, sized and
    // placed to match. Called once after the scene loads; these actors never
    // move and live for the rest of the session -- they are what a dropped prop
    // falls onto and what the player's controller slides along.
    void AddStaticWorld(std::span<const Aabb> colliders);

    // The scene every actor is added to, the factory that creates them, the
    // shared surface material, and the manager that will own the player's
    // character controller. Callers pull in <PxPhysicsAPI.h> themselves.
    physx::PxScene& Scene() const { return *scene_; }
    physx::PxPhysics& Sdk() const { return *physics_; }
    physx::PxMaterial& DefaultMaterial() const { return *material_; }
    physx::PxControllerManager& Controllers() const { return *controllers_; }

private:
    // The allocator and error callback must outlive the foundation that refers
    // to them, so they are owned here and released last.
    physx::PxDefaultAllocator* allocator_ = nullptr;
    physx::PxDefaultErrorCallback* error_callback_ = nullptr;
    physx::PxFoundation* foundation_ = nullptr;
    physx::PxPhysics* physics_ = nullptr;
    physx::PxDefaultCpuDispatcher* dispatcher_ = nullptr;
    physx::PxScene* scene_ = nullptr;
    physx::PxMaterial* material_ = nullptr;
    physx::PxControllerManager* controllers_ = nullptr;

    // Real time not yet consumed by a fixed substep, carried to next frame.
    float accumulator_ = 0.0f;
};
