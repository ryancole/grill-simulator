#pragma once

#include <DirectXMath.h>

namespace physx {
class PxRigidDynamic;
}

// Hung off every dynamic body the player can bump, via PxActor::userData, so the
// one generic shove in the character controller can tell bodies apart without
// knowing about Props or Furniture.
//
// `knock_rating` is a designer's 1..10 dial for how hard the object is to knock
// over: 1 shrugs nothing off, higher values divide the player's shove down. See
// the controller hit report in camera.cpp for how a rating becomes a resistance.
//
// `prop_index` is >= 0 only for a carryable prop -- its index in Props::items_ --
// which is how the gaze-pick sweep tells a grabbable prop (meat, tongs) from the
// heavy furniture (grill, cooler), which is dynamic and shovable but not carried.
//
// `meat` marks the steak and patties, so the physics contact report can single
// them out and play a wet splat when one lands on something -- the tongs, grill
// and cooler carry the same tag but leave this false, so they stay silent.
struct BodyTag {
    int prop_index = -1;
    float knock_rating = 1.0f;
    bool meat = false;
};

// The shared core of everything the player can bump: a handle to a dynamic PhysX
// body that carries a knock tag (so the generic shove treats it right) and maps to
// a render transform. Props embed one in each carryable item, Furniture embeds one
// in each toppleable object, and anything bumpable added later does the same --
// this is where "it has bump physics" lives, once.
//
// It is a non-owning handle: the PxScene owns the actor and releases it on
// teardown, exactly like every other PhysX object in this project. The tag,
// however, lives *inside* the RigidBody, and its address is what userData points
// at -- so a RigidBody must already sit in its final storage slot before Bind() is
// called, and must not be moved or copied afterwards. The owning containers
// reserve() up front for precisely this reason.
class RigidBody {
public:
    // Take ownership of the tag values for an actor the caller just created in the
    // scene. Does not touch userData yet -- see Bind.
    void Adopt(physx::PxRigidDynamic* actor, float knock_rating, int prop_index = -1,
               bool meat = false);
    // Point the actor's userData at our tag. Call once, after this RigidBody is in
    // the storage slot it will live in for the session.
    void Bind();

    physx::PxRigidDynamic* actor() const { return actor_; }

    // The body's model-to-world transform, read from its current global pose. The
    // body frame is the model frame throughout this project, so this is exactly
    // what the renderer draws the object under -- wherever it has been shoved to.
    DirectX::XMMATRIX Transform() const;

private:
    physx::PxRigidDynamic* actor_ = nullptr;
    BodyTag tag_;
};
