#pragma once

#include "scene.hpp"

#include <DirectXMath.h>

#include <random>
#include <span>
#include <vector>

// Every flame in the game: the little tongue at the lighter's muzzle, and the fuller fire
// standing on a log that has caught. One shared particle system emits them all -- each
// caller hands it a world point and a size, so the same specks make a pilot flame at
// scale one and a campfire at scale three. A CPU particle system, unlike the lighter fluid
// a flame will one day light -- specks that rise and fade, with nothing to collide against
// and nothing for gameplay to query, so it needs neither PhysX nor the GPU (see Fluid,
// which needs both because its droplets splash and pool).
//
// The particles are drawn as tiny cubes of the shared unit model, like the fluid's
// droplets, but self-lit: each carries an `emissive` above one, so it ignores the
// shading that would otherwise make it read as an orange block in the sun, and blooms
// through the HDR pipeline instead. They are emitted in world space, so a flame carried
// at a walk trails behind the hand rather than dragging along rigidly with it, and a log
// knocked while burning drags its fire with it.
//
// A session-persistent system: the flame is the same object in every level, so main
// owns one and Props points at it. Clear() parks it on a level swap.
class Flame {
public:
    Flame();

    // Emits this frame's particles from `origin` in world space, at `scale` -- one for the
    // lighter's pilot tongue, larger for a caught log's fire, sizing the specks, how high
    // they climb, how wide they spread and how many there are all together. Called every
    // frame a thing is burning, and not at all otherwise: the emission rate is banked
    // across frames internally (see burn_carry_), so a flame looks the same however long a
    // frame ran. Several fires may emit into the one system each frame.
    void Emit(DirectX::XMFLOAT3 origin, float dt, float scale = 1.0f);

    // Ages every live particle by `dt`, drifting it upward and retiring it at the end of
    // its life, then rebuilds Instances(). Runs every frame whether or not the flame is
    // lit this one -- particles already in the air have to finish burning out.
    void Update(float dt);

    // Retires every particle at once: the level-swap reset, so a flame lit in one level
    // does not hang in the air of the next.
    void Clear();

    // The live particles as draw instances -- little self-lit cubes -- for the renderer's
    // world pass. Rebuilt by Update; empty while nothing is burning.
    std::span<const MeshInstance> Instances() const { return instances_; }

private:
    // One speck of fire. Its whole life is `age` climbing to `lifetime`; everything the
    // renderer sees (size, colour, brightness) is a curve read off that ratio, so nothing
    // else has to be stored per particle.
    struct Particle {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 velocity;
        float age = 0.0f;
        float lifetime = 0.0f;
        float spin = 0.0f; // radians of yaw, so the cubes do not all face the same way
        // The size the emitter asked for, kept per speck so a big fire and a small one can
        // burn in the same pool at once: it scales this speck's drawn size and how hard it
        // keeps rising (its birth velocity was already scaled in Emit).
        float scale = 1.0f;
    };

    std::vector<Particle> particles_;

    // Emission banked across frames: the fractional particle a frame did not have the
    // time to emit, carried into the next.
    float burn_carry_ = 0.0f;

    // The jitter's randomness. Nothing depends on the exact path of a speck of fire, so
    // an arbitrarily seeded engine is fine (as in Fluid).
    std::minstd_rand rng_{20260717u};

    // Rebuilt by Update for the renderer.
    std::vector<MeshInstance> instances_;
};
