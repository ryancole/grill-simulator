#pragma once

#include <DirectXMath.h>

// World-space axis-aligned box. Every prop in the scene is a box, so this is
// both the bound of the render primitive and the collider shape the game hands
// to PhysX (as a static box actor, see Physics::AddStaticWorld).
struct Aabb {
    DirectX::XMFLOAT3 min;
    DirectX::XMFLOAT3 max;
};

// The axis-aligned bound of `bounds` carried through `transform`. All eight
// corners are transformed and re-bounded, so a rotated box gives a loose but
// correct fit. The scene builds its colliders with this; the props measure their
// own box shape with it.
Aabb TransformBounds(const Aabb& bounds, DirectX::FXMMATRIX transform);

// How far above its feet the player can rise without jumping. A box whose top is
// within a step is climbed rather than collided with, which is what lets the
// player walk onto the patio slab instead of being stopped by its 6 cm lip. Fed
// to the capsule controller as its step offset.
inline constexpr float kStepHeight = 0.25f;
