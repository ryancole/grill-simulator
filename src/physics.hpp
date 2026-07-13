#pragma once

#include "collision.hpp"
#include "rigid_body.hpp"

#include <DirectXMath.h>

#include <span>
#include <vector>

// PhysX owns the simulation. This class brings the SDK up, holds the one scene
// every rigid body lives in, and steps it on a fixed clock. The props (dynamic
// bodies), the yard (static box actors) and the player (a capsule character
// controller) all register with the scene it holds.
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
class PxRigidDynamic;
class PxSimulationEventCallback;
} // namespace physx

// A sounding collision the solver reported this step, for the audio to play.
// `position` is where the contact landed in world space, so the sound pans and
// attenuates from the right spot; `strength` is the contact impulse magnitude,
// which the mixer maps to volume -- a dropped patty thuds softer than one hurled
// at a wall; `sound` is which clip to play, from the body that made the noise.
struct Impact {
    DirectX::XMFLOAT3 position;
    float strength;
    ImpactSound sound;
};

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
    // remainder for next frame -- a fixed clock is what keeps the contact solve
    // stable however long a rendered frame took. Contacts that a meat body begins
    // this step are gathered into the impact list, which Impacts() hands back.
    void Step(float dt);

    // The meat impacts the solver reported during the most recent Step, for the
    // audio to sound. Cleared at the top of each Step, so read it after Step and
    // before the next one; empty on any frame no meat landed on anything.
    std::span<const Impact> Impacts() const { return impacts_; }

    // Builds the immovable world: one static box actor per collider, sized,
    // placed and turned to match. Called once after the scene loads; these actors
    // never move and live for the rest of the session -- they are what a dropped
    // prop falls onto and what the player's controller slides along.
    void AddStaticWorld(std::span<const OrientedBox> colliders);

    // Creates one dynamic rigid body from a set of box shapes, so a large object
    // (the grill) can be shoved and toppled as a single rigid piece. `shapes` are
    // in the body's own model space -- one per leg, body, lid and shelf -- and
    // `initial_pose` is the model-to-world transform the body spawns at (a pure
    // translation/rotation, no scale). PhysX derives the centre of mass and
    // inertia from the shapes at the given total `mass`, which is what makes a
    // top-heavy, narrow-based object tip rather than merely slide. The returned
    // body is owned by the scene; the caller reads its pose back each frame.
    physx::PxRigidDynamic* AddDynamicBody(std::span<const OrientedBox> shapes,
                                          const DirectX::XMFLOAT4X4& initial_pose, float mass);

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

    // Receives the solver's contact reports and appends meat impacts to impacts_.
    // Owned here (raw new/delete like the SDK objects) and set on the scene, so it
    // must outlive the scene -- released after it in the destructor.
    physx::PxSimulationEventCallback* contact_reporter_ = nullptr;

    // Meat impacts gathered during the current Step's substeps, handed to the audio
    // by Impacts(). Cleared at the top of each Step.
    std::vector<Impact> impacts_;

    // Real time not yet consumed by a fixed substep, carried to next frame.
    float accumulator_ = 0.0f;
};
