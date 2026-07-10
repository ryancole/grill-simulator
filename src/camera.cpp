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

// How fast the player's velocity converges on the speed the keys are asking for,
// as a rate in 1/seconds: the velocity closes the remaining gap by 1 - e^(-rate *
// dt) each frame, so both numbers are frame-rate independent.
//
// On the ground the response is stiff enough to read as instant while still
// rounding the start and stop of a walk -- releasing a key coasts about 19 cm.
// In the air it is deliberately slack, because a held key can only steer a jump,
// never brake it.
constexpr float kGroundResponse = 18.0f; // settles in ~55 ms
constexpr float kAirResponse = 2.2f;     // settles in ~450 ms

// Exponential decay never truly reaches zero. Below this the player is simply
// stopped, so a released key does not leave them creeping for ever.
constexpr float kRestSpeed = 0.05f;

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

    // The velocity the keys are asking for. With no keys held that is a standstill,
    // which the player then decelerates towards rather than snapping to.
    XMVECTOR wanted = XMVectorZero();
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
        wanted = XMVectorScale(direction, speed);
    }

    XMVECTOR velocity = XMVectorSet(velocity_.x, 0.0f, velocity_.y, 0.0f);
    const bool steering = ahead != 0.0f || side != 0.0f;

    if (grounded_) {
        // Boots on the ground: the player pushes off it, so they may both
        // accelerate and stop, and a released key brings them to rest.
        velocity = XMVectorLerp(velocity, wanted, 1.0f - std::exp(-kGroundResponse * dt));
        if (XMVectorGetX(XMVector3Length(velocity)) < kRestSpeed) {
            velocity = XMVectorZero();
        }
    } else if (steering) {
        // Airborne with a key held: allow a slack steer towards the new heading.
        velocity = XMVectorLerp(velocity, wanted, 1.0f - std::exp(-kAirResponse * dt));
    }
    // Airborne with nothing held: there is nothing to push against, so momentum
    // is conserved exactly. Releasing the keys mid-jump must not stop the player
    // in the air, which is the entire point of holding a velocity at all.

    const XMVECTOR moved_from = XMLoadFloat3(&position_);
    XMStoreFloat3(&position_, XMVectorAdd(moved_from, XMVectorScale(velocity, dt)));

    vertical_speed_ -= kGravity * dt;
    position_.y += vertical_speed_ * dt;

    // Resolved after the vertical step, so that a body which has risen above a
    // crate is free to move across it. There is no ceiling test: jumping into
    // the underside of a tree canopy passes through it.
    position_ = ResolveCollision(position_, kBodyRadius, kEyeHeight, colliders);

    // Whatever the resolver refused to let us travel, we were never really
    // carrying. Re-deriving the velocity from the distance actually covered kills
    // the component pushed into a wall and keeps the component sliding along it.
    // The resolver also *ejects* a body wedged into a corner, and that shove is
    // not momentum, so a speed-up is never adopted.
    if (dt > 1e-5f) {
        const XMVECTOR travelled =
            XMVectorScale(XMVectorSubtract(XMLoadFloat3(&position_), moved_from), 1.0f / dt);
        const XMVECTOR flat = XMVectorSetY(travelled, 0.0f);
        if (XMVector3Less(XMVector3LengthSq(flat), XMVector3LengthSq(velocity))) {
            velocity = flat;
        }
    }
    velocity_ = {XMVectorGetX(velocity), XMVectorGetZ(velocity)};

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
    return XMMatrixLookToLH(XMLoadFloat3(&position_), Forward(),
                            XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
}

XMMATRIX Camera::CameraToWorldMatrix() const {
    // The same frame XMMatrixLookToLH derives, laid out as rows instead of
    // inverted into columns: right, up, forward, then the eye itself. Pitch is
    // clamped short of vertical, so the cross product can never degenerate.
    const XMVECTOR forward = Forward();
    const XMVECTOR right =
        XMVector3Normalize(XMVector3Cross(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), forward));
    const XMVECTOR up = XMVector3Cross(forward, right);
    return XMMATRIX(right, up, forward, XMVectorSetW(XMLoadFloat3(&position_), 1.0f));
}

XMVECTOR Camera::Forward() const {
    const float cos_pitch = std::cos(pitch_);
    return XMVectorSet(std::sin(yaw_) * cos_pitch, std::sin(pitch_), std::cos(yaw_) * cos_pitch,
                       0.0f);
}

XMMATRIX Camera::ProjectionMatrix(float aspect) const {
    return XMMatrixPerspectiveFovLH(kFieldOfView, aspect, kNearPlane, kFarPlane);
}
