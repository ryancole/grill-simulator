#pragma once

#include <DirectXMath.h>

// A place the player delivers cooked food: a vertical column in the yard, centred on a
// serving surface (a table, a pass). A carried meat brought within `radius` metres of
// the centre in the ground plane can be served there -- see Props::Update, which reads
// the meat's doneness at that moment and hands it to the level's Objectives.
//
// Unlike a HeatSource, a ServeZone is static: its origin is set once from where the
// level placed the serving prop and never moves, so there is no per-frame refresh. It
// is the delivery counterpart of the grill's heat -- the grill *supplies* the cook, the
// serve zone *accepts* the result.
//
// The containment test is horizontal on purpose. A carried meat rides at chest height
// in the hand, well above the plate it is being set on, so a full 3-D proximity test
// would force the player to aim down at the surface. Keyed on the ground plane, the
// zone reads as "stand at the counter with the food" -- which is the playable shape of
// "bring it to the plate".
class ServeZone {
public:
    ServeZone(DirectX::FXMVECTOR origin, float radius);

    // True when `point` lies inside the column: its horizontal (x/z) distance to the
    // centre is within the radius. The vertical coordinate is ignored.
    bool Contains(DirectX::FXMVECTOR point) const;

    // The column's centre in world space -- where served food is set down to rest.
    DirectX::XMFLOAT3 Origin() const { return origin_; }
    float Radius() const { return radius_; }

private:
    DirectX::XMFLOAT3 origin_{0.0f, 0.0f, 0.0f};
    float radius_ = 1.0f;
};
