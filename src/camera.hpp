#pragma once

#include "collision.hpp"

#include <DirectXMath.h>

#include <span>

class Input;

// A first person camera standing at eye height on a left-handed world: +X right,
// +Y up, +Z forward. Yaw and pitch only -- there is no roll, and movement stays
// in the ground plane, so looking at the sky never lifts the player off it.
class Camera {
public:
    // Turns the camera by a raw mouse delta, in mouse counts.
    void Look(float dx, float dy);
    // Walks the camera according to the held keys and slides it out of the
    // scene's colliders.
    void Update(const Input& input, std::span<const Aabb> colliders, float dt);

    DirectX::XMMATRIX ViewMatrix() const;
    DirectX::XMMATRIX ProjectionMatrix(float aspect) const;

    DirectX::XMFLOAT3 Position() const { return position_; }

private:
    DirectX::XMFLOAT3 position_{0.0f, 1.7f, -7.0f};
    // Radians. A yaw of zero looks straight down +Z; positive pitch looks up.
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
};
