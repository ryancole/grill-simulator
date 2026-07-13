#pragma once

#include <DirectXMath.h>

namespace physx {
class PxController;
class PxUserControllerHitReport;
}
class Actions;
class Physics;

// A first person camera standing at eye height on a left-handed world: +X right,
// +Y up, +Z forward. Yaw and pitch only -- there is no roll, and horizontal
// movement ignores pitch, so looking at the sky never walks the player upward.
//
// The body is a PhysX capsule character controller: the walk/jump/air-control
// feel is still owned here, but the collide-and-slide against the world is the
// controller's job. The eye rides on top of the capsule.
class Camera {
public:
    // Creates the capsule controller in the physics scene at the spawn point.
    explicit Camera(Physics& physics);
    ~Camera();

    Camera(const Camera&) = delete;
    Camera& operator=(const Camera&) = delete;

    // Turns the camera by a raw mouse delta, in mouse counts.
    void Look(float dx, float dy);
    // Walks, jumps and falls the player by moving the capsule controller, which
    // slides it along the world's surfaces, then rides the eye on top. Reads the
    // move/jump/sprint actions rather than raw keys, so the walk answers to
    // whatever controls.toml binds them to.
    void Update(const Actions& actions, float dt);

    // Drops the player at a level's entrance and clears their motion, for when a
    // level is (re)loaded. `foot` is where the feet stand on the ground and `facing`
    // is the heading in degrees (0 looks north, +Z); the eye rides an eye-height
    // above the feet. The controller persists across the swap (it belongs to the
    // session, not the level), so this just repositions and re-aims it.
    void Respawn(DirectX::XMFLOAT3 foot, float facing_degrees);

    DirectX::XMMATRIX ViewMatrix() const;
    DirectX::XMMATRIX ProjectionMatrix(float aspect) const;
    // The inverse of the view matrix: it carries a point expressed relative to
    // the eye out into the world. Anything bolted to the player's face -- their
    // arms, for now -- is placed with this.
    DirectX::XMMATRIX CameraToWorldMatrix() const;

    DirectX::XMFLOAT3 Position() const { return position_; }

private:
    // The unit vector the eye is looking down, yaw and pitch applied.
    DirectX::XMVECTOR Forward() const;

    // The capsule in the physics scene. Owned by the controller manager (in
    // Physics); released here, before that manager tears down.
    physx::PxController* controller_ = nullptr;
    // Applies the shove to a prop the capsule walks into. Owned here; must outlive
    // the controller that calls it.
    physx::PxUserControllerHitReport* report_ = nullptr;

    // The eye, in world space: read back from the controller's foot each frame.
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
