#pragma once

#include "scene.hpp"

#include <DirectXMath.h>

#include <random>
#include <span>
#include <vector>

class Physics;

namespace physx {
class PxPBDParticleSystem;
class PxPBDMaterial;
class PxParticleBuffer;
} // namespace physx

// The lighter-fluid spray, as a real PhysX PBD fluid: a pool of GPU particles the
// spray ability squirts from the can's nozzle. The droplets arc under the scene's
// gravity, collide with the world (they splash off the grill and pool on the dirt)
// and carry a little viscosity and cohesion so a stream reads as liquid rather
// than as gravel. This is the game's first (and so far only) GPU-side simulation;
// see Physics::GpuActive.
//
// PhysX 5's particle systems are GPU-only, so the whole class degrades to inert
// stubs on a machine with no CUDA device: Active() is false, Spray does nothing,
// the spans stay empty, and the lighter fluid simply squirts nothing.
//
// A session-persistent system, like Physics -- the particle system rides the one
// physics scene. A level swap calls Clear() to park every droplet; the per-level
// consequences of the fluid (the fire pit's wetness) live in Props, not here.
//
// The particle pool is fixed. Every slot is always active in the simulation; an
// idle slot is "parked": pinned (zero inverse mass, so it never moves or falls)
// far below any level, spaced out so the parked block costs the neighborhood grid
// nothing. Spraying recycles the oldest slot, so the can never runs dry -- an
// over-long hold just retires its earliest droplets first.
class Fluid {
public:
    explicit Fluid(Physics& physics);
    ~Fluid();

    Fluid(const Fluid&) = delete;
    Fluid& operator=(const Fluid&) = delete;

    // Whether the GPU fluid exists at all (a CUDA context came up -- see
    // Physics::GpuActive). When false every other call is a cheap no-op.
    bool Active() const { return system_ != nullptr; }

    // Queues `count` droplets leaving `origin` along `direction` (unit length) at
    // `speed` metres per second, each nudged by a small cone jitter so the stream
    // fans like a sprayed liquid. They enter the simulation on the next Update.
    void Spray(DirectX::XMFLOAT3 origin, DirectX::XMFLOAT3 direction, float speed, int count);

    // Once per frame, after Physics::Step: reads the simulated positions back from
    // the GPU, retires droplets past their lifetime, injects the sprays queued
    // since the last frame, and rebuilds Positions()/Instances(). The injected
    // droplets take effect on the next physics step, but they appear in this
    // frame's draw list already -- at the nozzle, where they were queued.
    void Update(float dt);

    // Parks every droplet at once: the level-swap reset, so no puddle survives
    // into (or re-primes the fire pit of) a freshly loaded level.
    void Clear();

    // The live droplets' world positions this frame, for gameplay to test (the
    // fire pit's wetness). Rebuilt by Update; empty while nothing is in flight.
    std::span<const DirectX::XMFLOAT3> Positions() const { return positions_; }

    // The live droplets as draw instances -- little tinted cubes of the shared
    // unit-cube model -- for the renderer's world pass. Rebuilt by Update.
    std::span<const MeshInstance> Instances() const { return instances_; }

private:
    // Pins slot `i` at its parking spot: zero inverse mass far under the world.
    void Park(int slot);

    Physics* physics_ = nullptr;
    physx::PxPBDParticleSystem* system_ = nullptr;
    physx::PxPBDMaterial* material_ = nullptr;
    physx::PxParticleBuffer* buffer_ = nullptr;

    // Host mirrors of the GPU particle buffer, refreshed from the device each
    // Update and written back when a spray or an expiry dirtied them. Layout
    // matches PhysX's: position xyz + inverse mass in w; velocity xyz, w unused.
    std::vector<DirectX::XMFLOAT4> pos_inv_mass_;
    std::vector<DirectX::XMFLOAT4> velocity_;
    // Seconds each slot has left to live; zero or below means parked.
    std::vector<float> age_;

    // Sprays queued since the last Update, waiting for slots.
    struct Pending {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 velocity;
    };
    std::vector<Pending> pending_;

    // The recycling cursor: the next slot a queued droplet takes, oldest first.
    int cursor_ = 0;

    // The cone jitter's randomness. Gameplay never depends on exact droplet
    // paths, so an arbitrarily seeded engine is fine.
    std::minstd_rand rng_{20260716u};

    // Rebuilt by Update for the renderer and for gameplay queries.
    std::vector<DirectX::XMFLOAT3> positions_;
    std::vector<MeshInstance> instances_;
};
