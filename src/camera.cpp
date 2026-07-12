#include "camera.hpp"

#include "collision.hpp" // kStepHeight
#include "input.hpp"
#include "physics.hpp"
#include "rigid_body.hpp" // BodyTag

#include <PxPhysicsAPI.h>

#include <algorithm>
#include <cmath>

using namespace DirectX;
using namespace physx;

namespace {

constexpr float kEyeHeight = 1.7f;
constexpr float kBodyRadius = 0.35f;
constexpr float kWalkSpeed = 3.4f;   // metres per second
constexpr float kSprintSpeed = 6.4f; // metres per second

// The capsule: a 0.35 m radius cylinder segment capped by two hemispheres, sized
// so foot-to-crown is exactly the eye height. The eye then rides at the crown.
//   total height = kCapsuleHeight + 2*radius = 1.0 + 0.7 = 1.7 = kEyeHeight
constexpr float kCapsuleHeight = kEyeHeight - 2.0f * kBodyRadius; // cylinder part
// A thin skin PhysX keeps between the capsule and what it touches, so contact is
// detected a hair early rather than after interpenetrating.
constexpr float kContactOffset = 0.05f;

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

// How hard the player shoves what they walk into. The target horizontal speed
// imparted to the object scales with how fast the player is actually moving, so a
// shuffle barely stirs it while a sprint carries real momentum -- enough, struck
// above its centre of mass, to tip a tall thing like the grill rather than only
// sliding it. The impulse is scaled by the object's mass so the *target speed*,
// not the impulse, is what these numbers mean, independent of what is hit.
constexpr float kPushGain = 0.6f;     // fraction of the player's speed imparted
constexpr float kMinPushSpeed = 1.2f; // floor, so a slow bump still scoots a prop
constexpr float kMaxPushSpeed = 3.5f; // cap, so nothing is launched across the yard

// Below this horizontal speed the player is not really running into anything, so
// an object they are merely resting a foot on is left alone rather than kicked.
constexpr float kMinMoveSpeed = 0.5f; // m/s

// Turns an object's 1..10 "hard to knock over" rating (BodyTag::knock_rating) into
// the divisor applied to the shove: rating 1 shrugs nothing off, and each point
// above that stiffens it. Tuned so the planted grill (8) barely stirs at a walk
// and needs a real run to go over, while a middling steak (4) still shifts when
// bumped. An untagged body reads as rating 1 -- full push, as before.
constexpr float kResistancePerPoint = 0.45f;

float KnockResistance(float rating) { return 1.0f + kResistancePerPoint * (rating - 1.0f); }

// Lets the walking player nudge props aside and bowl the grill over. The
// controller collides with dynamic actors on its own but never moves them; this
// applies an impulse at the contact point so a steak or crate scoots and a run
// into the grill knocks it down rather than the object standing there like a wall.
//
// The push goes in the direction of the contact for a side-on hit (a wall, the
// grill's kettle at chest height), but a short object -- a cooler -- is not hit
// side-on at all: the capsule rides up its low top and the controller reports a
// purely downward contact, which used to be dropped as "standing on it", leaving
// the cooler unmoved. So when the contact is vertical we fall back to the player's
// own travel direction and kick the thing along it, as long as the player is
// actually moving -- run over a cooler and it tumbles away instead of being
// silently climbed. A downward push is never applied, so nothing is scooped up.
class ControllerHitReport : public PxUserControllerHitReport {
public:
    // The player's horizontal velocity this frame (world X/Z), set by Camera::Update
    // just before the controller moves, so onShapeHit knows how hard and which way
    // the player is running in.
    PxVec3 player_velocity{0.0f, 0.0f, 0.0f};

    void onShapeHit(const PxControllerShapeHit& hit) override {
        PxRigidDynamic* body = hit.actor->is<PxRigidDynamic>();
        if (body == nullptr || body->getRigidBodyFlags().isSet(PxRigidBodyFlag::eKINEMATIC)) {
            return;
        }

        const float speed = player_velocity.magnitude();

        PxVec3 dir(hit.dir.x, 0.0f, hit.dir.z);
        const float length_sq = dir.magnitudeSquared();
        if (length_sq > 1e-6f) {
            // A side-on contact: shove along it (into the wall/grill), as before.
            dir *= 1.0f / PxSqrt(length_sq);
        } else if (speed > kMinMoveSpeed) {
            // A purely vertical contact and the player is moving: they are running
            // up onto a low object, so kick it along their travel instead.
            dir = player_velocity * (1.0f / speed);
        } else {
            return; // vertical contact, standing still -- just resting on it.
        }

        // The target speed the player's momentum wants to impart, divided down by
        // how hard this particular object is to knock over.
        const auto* tag = static_cast<const BodyTag*>(body->userData);
        const float resistance = KnockResistance(tag != nullptr ? tag->knock_rating : 1.0f);
        const float target =
            std::clamp(kPushGain * speed, kMinPushSpeed, kMaxPushSpeed) / resistance;

        // A contact reports every substep the player leans on the object, so a
        // single unconditional impulse per call would pile up over the contact and
        // bowl over even a "heavy" thing given a second of shoving. Instead treat
        // `target` as a speed the shove brings the object *up to*: only add the
        // shortfall between how fast it is already moving off (its centre-of-mass
        // speed along the push) and `target`, and nothing once it is there. Now a
        // low target genuinely caps how hard the object can ever be shoved, so a
        // sturdy object is nudged but never accelerated enough to go over.
        const float moving_off = body->getLinearVelocity().dot(dir);
        const float add = target - moving_off;
        if (add <= 0.0f) {
            return;
        }
        // At the contact point, not the centre of mass: the offset is what turns a
        // horizontal shove into the torque that topples a tall object.
        PxRigidBodyExt::addForceAtPos(*body, dir * (add * body->getMass()),
                                      toVec3(hit.worldPos), PxForceMode::eIMPULSE);
    }
    void onControllerHit(const PxControllersHit&) override {}
    void onObstacleHit(const PxControllerObstacleHit&) override {}
};

} // namespace

Camera::Camera(Physics& physics) {
    // A capsule stood upright at the spawn point. The controller position is the
    // capsule centre, which sits kCapsuleHeight/2 + radius above the feet.
    PxCapsuleControllerDesc desc;
    desc.radius = kBodyRadius;
    desc.height = kCapsuleHeight;
    desc.upDirection = PxVec3(0.0f, 1.0f, 0.0f);
    desc.slopeLimit = 0.0f; // no slope is too steep; the yard is boxes and flats.
    desc.stepOffset = kStepHeight; // a curb within a step is climbed, not walled off.
    desc.contactOffset = kContactOffset;
    desc.material = &physics.DefaultMaterial();
    report_ = new ControllerHitReport();
    desc.reportCallback = report_;

    const float foot = position_.y - kEyeHeight;
    const float center = foot + 0.5f * kCapsuleHeight + kBodyRadius;
    desc.position = PxExtendedVec3(position_.x, center, position_.z);

    controller_ = physics.Controllers().createController(desc);
}

Camera::~Camera() {
    if (controller_) {
        controller_->release();
    }
    // After the controller, which held the pointer to it. Down to the concrete
    // type: the PhysX base's destructor is protected, deliberately not deletable.
    delete static_cast<ControllerHitReport*>(report_);
}

void Camera::Look(float dx, float dy) {
    yaw_ += dx * kRadiansPerCount;
    // Screen coordinates grow downward, so a downward flick has to lower pitch.
    pitch_ = std::clamp(pitch_ - dy * kRadiansPerCount, -kPitchLimit, kPitchLimit);
    // Fold yaw back into [-pi, pi] so a long session never grinds away its
    // precision by accumulating turns.
    yaw_ = std::remainder(yaw_, XM_2PI);
}

void Camera::Update(const Input& input, float dt) {
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

    vertical_speed_ -= kGravity * dt;

    // The move the controller is asked to make this frame: the horizontal walk
    // plus the vertical fall/jump. It slides this along whatever it hits.
    XMFLOAT3 v;
    XMStoreFloat3(&v, velocity);
    const PxVec3 displacement(v.x * dt, vertical_speed_ * dt, v.z * dt);

    // Hand the hit report the player's horizontal velocity, so the shove it deals
    // to whatever the capsule strikes scales with real momentum and, for a low
    // object climbed rather than hit side-on, follows the direction of travel.
    static_cast<ControllerHitReport*>(report_)->player_velocity = PxVec3(v.x, 0.0f, v.z);

    const PxExtendedVec3 before = controller_->getFootPosition();
    const PxControllerCollisionFlags flags =
        controller_->move(displacement, 0.001f, dt, PxControllerFilters());
    const PxExtendedVec3 after = controller_->getFootPosition();

    // Ride the eye on the capsule's crown, foot plus eye height.
    position_ = {static_cast<float>(after.x), static_cast<float>(after.y) + kEyeHeight,
                 static_cast<float>(after.z)};

    // A contact below is ground: stop falling and allow the next jump. A contact
    // above is a bonked head: kill the climb so the player does not cling there.
    if (flags.isSet(PxControllerCollisionFlag::eCOLLISION_DOWN)) {
        vertical_speed_ = 0.0f;
        grounded_ = true;
    } else {
        grounded_ = false;
    }
    if (flags.isSet(PxControllerCollisionFlag::eCOLLISION_UP) && vertical_speed_ > 0.0f) {
        vertical_speed_ = 0.0f;
    }

    // Whatever the controller refused to let us travel, we were never really
    // carrying. Re-deriving the horizontal velocity from the distance actually
    // covered kills the component pushed into a wall and keeps the component
    // sliding along it, so a held key never builds pressure into a corner.
    if (dt > 1e-5f) {
        const XMVECTOR flat =
            XMVectorSet(static_cast<float>(after.x - before.x) / dt, 0.0f,
                        static_cast<float>(after.z - before.z) / dt, 0.0f);
        const XMVECTOR wanted_flat = XMVectorSet(v.x, 0.0f, v.z, 0.0f);
        if (XMVector3Less(XMVector3LengthSq(flat), XMVector3LengthSq(wanted_flat))) {
            velocity = flat;
        } else {
            velocity = wanted_flat;
        }
    }
    velocity_ = {XMVectorGetX(velocity), XMVectorGetZ(velocity)};
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
