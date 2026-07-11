#pragma once

#include <DirectXMath.h>

#include <span>

// World-space axis-aligned box. Every prop in the scene is a box, so this is
// both the bound of the render primitive and the only collider shape the game
// needs.
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
// player walk onto the patio slab instead of being stopped by its 6 cm lip.
inline constexpr float kStepHeight = 0.25f;

// Pushes a vertical cylinder out of every box it overlaps and returns the eye
// position it may actually occupy. `eye` is the position the player wants to
// move to; the body spans [eye.y - eye_height, eye.y].
//
// Resolving one box can push the player into another, so this iterates.
DirectX::XMFLOAT3 ResolveCollision(DirectX::XMFLOAT3 eye, float radius, float eye_height,
                                   std::span<const Aabb> boxes);

// The height of the highest surface the cylinder is standing over, ignoring
// anything above `ceiling`. Returns -FLT_MAX when the cylinder is over nothing at
// all, which the caller reads as "keep falling".
//
// `ceiling` is what separates a floor from a wall: pass the feet plus a step and
// a curb becomes something to stand on, pass the feet exactly and it does not.
float HighestSupportUnder(float x, float z, float radius, float ceiling,
                          std::span<const Aabb> boxes);
