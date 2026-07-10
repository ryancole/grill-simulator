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
    const float ahead =
        static_cast<float>(input.IsKeyDown('W')) - static_cast<float>(input.IsKeyDown('S'));
    const float side =
        static_cast<float>(input.IsKeyDown('D')) - static_cast<float>(input.IsKeyDown('A'));
    if (ahead == 0.0f && side == 0.0f) {
        return;
    }

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
    position_.y = kEyeHeight;
    position_ = ResolveCollision(position_, kBodyRadius, kEyeHeight, colliders);
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
