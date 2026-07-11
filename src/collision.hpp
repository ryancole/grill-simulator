#pragma once

#include <DirectXMath.h>

// World-space axis-aligned box. Every prop's vertex bounds come to us as one of
// these (glTF stores them on the POSITION accessor), and a model-space union of
// them still measures a prop's own box shape.
struct Aabb {
    DirectX::XMFLOAT3 min;
    DirectX::XMFLOAT3 max;
};

// World-space oriented box: a centre, half-extents along its own axes, and the
// rotation that orients those axes. This is the collider shape the game hands to
// PhysX (as a static box actor, see Physics::AddStaticWorld). An identity
// orientation is just an AABB; a yawed crate or tree trunk keeps its true
// footprint here instead of the looser axis-aligned bound.
struct OrientedBox {
    DirectX::XMFLOAT3 center;
    DirectX::XMFLOAT3 half_extents;
    DirectX::XMFLOAT4 orientation; // quaternion (x, y, z, w)
};

// The axis-aligned bound of `bounds` carried through `transform`. All eight
// corners are transformed and re-bounded, so a rotated box gives a loose but
// correct fit. The props measure their own (unturned, model-space) box shape
// with it, and TransformBox falls back to it for a transform it cannot cleanly
// decompose.
Aabb TransformBounds(const Aabb& bounds, DirectX::FXMMATRIX transform);

// The oriented box of `bounds` carried through `transform`. When `transform` is
// a translation, rotation and (possibly non-uniform) scale with no shear -- which
// is every instance the yard places -- the box keeps its true orientation, so a
// turned crate collides as the box it looks like rather than its bloated
// axis-aligned bound. If the matrix will not decompose that cleanly (a
// non-uniform scale folded under a rotation shears a box into a shape no box can
// represent), it falls back to TransformBounds: the loose AABB the scene used
// everywhere before, with an identity orientation.
OrientedBox TransformBox(const Aabb& bounds, DirectX::FXMMATRIX transform);

// How far above its feet the player can rise without jumping. A box whose top is
// within a step is climbed rather than collided with, which is what lets the
// player walk onto the patio slab instead of being stopped by its 6 cm lip. Fed
// to the capsule controller as its step offset.
inline constexpr float kStepHeight = 0.25f;
