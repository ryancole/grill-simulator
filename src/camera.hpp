#pragma once

#include "collision.hpp"

#include <DirectXMath.h>

#include <span>

class Input;

// A first person camera standing at eye height on a left-handed world: +X right,
// +Y up, +Z forward. Yaw and pitch only -- there is no roll, and horizontal
// movement ignores pitch, so looking at the sky never walks the player upward.
class Camera {
public:
    // Turns the camera by a raw mouse delta, in mouse counts.
    void Look(float dx, float dy);
    // Walks, jumps and falls the camera, then slides it out of the scene's
    // colliders.
    void Update(const Input& input, std::span<const Aabb> colliders, float dt);

    DirectX::XMMATRIX ViewMatrix() const;
    DirectX::XMMATRIX ProjectionMatrix(float aspect) const;

    DirectX::XMFLOAT3 Position() const { return position_; }

private:
    DirectX::XMFLOAT3 position_{0.0f, 1.7f, -7.0f};
    // Radians. A yaw of zero looks straight down +Z; positive pitch looks up.
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;

    // Metres per second across the ground plane; `y` here is the world's Z. The
    // player accelerates towards the speed the keys ask for rather than snapping
    // to it, which is what carries a jump forward after the keys are released.
    DirectX::XMFLOAT2 velocity_{0.0f, 0.0f};
    // Metres per second, positive up. Gravity owns this between jumps.
    float vertical_speed_ = 0.0f;
    // Whether the player was standing on something at the end of the last frame,
    // which is the only state a jump is allowed to start from.
    bool grounded_ = true;
};
