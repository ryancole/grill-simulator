#include "camera.hpp"

#include "input.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {

constexpr float kEyeHeight = 1.7f;
constexpr float kBodyRadius = 0.35f;
constexpr float kWalkSpeed = 3.4f;   // metres per second
constexpr float kSprintSpeed = 6.4f; // metres per second

// Real gravity makes a jump feel like a moon landing: the player hangs at the
// apex for far too long. Games solve this by falling much harder than 9.8, and
// launching harder to keep the height. 7 m/s against 22 m/s^2 clears 1.1 m --
// enough to reach the crates and the picnic table, not the grill lid.
constexpr float kGravity = 22.0f;
constexpr float kJumpSpeed = 7.0f;

// A 400 CPI mouse moved 10 cm turns roughly 200 degrees. Standard shooter feel.
constexpr float kRadiansPerCount = 0.0022f;

// Straight up is a singularity for a yaw/pitch camera: the view direction would
// become parallel to the up vector and the view matrix would be degenerate.
constexpr float kPitchLimit = XM_PIDIV2 - 0.01f;

constexpr float kFieldOfView = XMConvertToRadians(70.0f);
constexpr float kNearPlane = 0.05f;
constexpr float kFarPlane = 250.0f;

} // namespace

void Camera::Look(float dx, float dy) {
    yaw_ += dx * kRadiansPerCount;
    // Screen coordinates grow downward, so a downward flick has to lower pitch.
    pitch_ = std::clamp(pitch_ - dy * kRadiansPerCount, -kPitchLimit, kPitchLimit);
    // Fold yaw back into [-pi, pi] so a long session never grinds away its
    // precision by accumulating turns.
    yaw_ = std::remainder(yaw_, XM_2PI);
}

void Camera::Update(const Input& input, std::span<const Aabb> colliders, float dt) {
    // Where the feet started the frame. The landing test measures against this
    // rather than against where they end up, so a fast fall lands on the highest
    // surface it crossed instead of dropping straight through it.
    const float feet_before = position_.y - kEyeHeight;

    if (grounded_ && input.IsKeyDown(VK_SPACE)) {
        vertical_speed_ = kJumpSpeed;
        grounded_ = false;
    }

    const float ahead =
        static_cast<float>(input.IsKeyDown('W')) - static_cast<float>(input.IsKeyDown('S'));
    const float side =
        static_cast<float>(input.IsKeyDown('D')) - static_cast<float>(input.IsKeyDown('A'));

    if (ahead != 0.0f || side != 0.0f) {
        const float sin_yaw = std::sin(yaw_);
        const float cos_yaw = std::cos(yaw_);
        // Pitch is deliberately absent: it aims the view, never the feet.
        const XMVECTOR forward = XMVectorSet(sin_yaw, 0.0f, cos_yaw, 0.0f);
        const XMVECTOR right = XMVectorSet(cos_yaw, 0.0f, -sin_yaw, 0.0f);

        // Normalising keeps a diagonal from being faster than a straight line.
        const XMVECTOR direction = XMVector3Normalize(
            XMVectorAdd(XMVectorScale(forward, ahead), XMVectorScale(right, side)));
        const float speed = input.IsKeyDown(VK_SHIFT) ? kSprintSpeed : kWalkSpeed;

        XMStoreFloat3(&position_,
                      XMVectorAdd(XMLoadFloat3(&position_), XMVectorScale(direction, speed * dt)));
    }

    vertical_speed_ -= kGravity * dt;
    position_.y += vertical_speed_ * dt;

    // Resolved after the vertical step, so that a body which has risen above a
    // crate is free to move across it. There is no ceiling test: jumping into
    // the underside of a tree canopy passes through it.
    position_ = ResolveCollision(position_, kBodyRadius, kEyeHeight, colliders);

    // A surface within a step of where the feet began is a floor to stand on --
    // which is also what turns a curb into a step rather than a wall.
    const float support = HighestSupportUnder(position_.x, position_.z, kBodyRadius,
                                              feet_before + kStepHeight, colliders);
    const bool landed = vertical_speed_ <= 0.0f && position_.y - kEyeHeight <= support;
    if (landed) {
        position_.y = support + kEyeHeight;
        vertical_speed_ = 0.0f;
    }
    grounded_ = landed;
}

XMMATRIX Camera::ViewMatrix() const {
    const float cos_pitch = std::cos(pitch_);
    const XMVECTOR direction =
        XMVectorSet(std::sin(yaw_) * cos_pitch, std::sin(pitch_), std::cos(yaw_) * cos_pitch, 0.0f);
    return XMMatrixLookToLH(XMLoadFloat3(&position_), direction, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

XMMATRIX Camera::ProjectionMatrix(float aspect) const {
    return XMMatrixPerspectiveFovLH(kFieldOfView, aspect, kNearPlane, kFarPlane);
}
