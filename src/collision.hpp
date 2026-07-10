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

// Pushes a vertical cylinder out of every box it overlaps and returns the eye
// position it may actually occupy. `eye` is the position the player wants to
// move to; the body spans [eye.y - eye_height, eye.y].
//
// Resolving one box can push the player into another, so this iterates.
DirectX::XMFLOAT3 ResolveCollision(DirectX::XMFLOAT3 eye, float radius, float eye_height,
                                   std::span<const Aabb> boxes);
