#include "viewmodel.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {

constexpr XMFLOAT3 kSkin{0.72f, 0.53f, 0.40f};
constexpr XMFLOAT3 kSleeve{0.36f, 0.44f, 0.50f};

// The arms carry their own key light, bolted to the camera: over the player's
// head and a little behind it, so the tops of the forearms and the backs of the
// hands catch it. Lighting them with the yard's sun instead -- which is what the
// code did first -- turns them into flat brown slabs whenever the player faces
// into it. Shooters have always anchored the viewmodel's key to the view for
// exactly this reason. The ambient stays in world space, so the arms still
// darken when the player walks under a tree.
//
// The X component is zero, and has to be. The two arms are the same arm mirrored
// through X, which maps the inner face of one onto the inner face of the other
// with the normal's X negated. Any sideways lean in the light therefore adds to
// one arm's inner face exactly what it takes from the other's -- and those inner
// faces are most of what the player sees. At 0.35 across, the value the yard's
// sun uses, the left arm came out half the brightness of the right and the pair
// read as two different materials.
constexpr XMFLOAT3 kEyeSunDirection{0.0f, 0.82f, -0.57f};

// The right arm, in eye space and in metres. The shoulder sits behind the near
// plane and the elbow below the bottom of the frame, so what the player actually
// sees is a forearm entering from the corner -- which is the whole trick.
//
// An arm this far forward is nowhere near where a real one would be: a hand 78 cm
// from the eye is at the end of a straightened arm, not a relaxed one. It has to
// be. The world is drawn at a 70 degree field of view, and anything held at a
// true arm's length inside that frustum is close enough to the eye that
// perspective inflates it until it swallows the screen. Shooters normally answer
// this with a second, narrower projection just for the viewmodel; pushing the
// arms out buys the same look for none of the machinery.
constexpr XMFLOAT3 kShoulder{0.36f, -0.70f, -0.05f};
constexpr XMFLOAT3 kElbow{0.42f, -0.52f, 0.42f};
constexpr XMFLOAT3 kWrist{0.27f, -0.35f, 0.78f};
constexpr XMFLOAT3 kKnuckles{0.21f, -0.31f, 0.94f};
// The thumb leaves the inner edge of the palm, just past the wrist, and stays
// roughly in the plane of the palm. Angling it upward instead reads as a spur
// growing out of the wrist rather than as a thumb.
constexpr XMFLOAT3 kThumbBase{0.22f, -0.335f, 0.83f};
constexpr XMFLOAT3 kThumbTip{0.15f, -0.325f, 0.90f};

XMFLOAT3 Mirror(XMFLOAT3 point, float side) {
    return {point.x * side, point.y, point.z};
}

} // namespace

Viewmodel::Viewmodel(std::uint32_t cube_model)
    : cube_(cube_model) {
    AddArm(1.0f);
    AddArm(-1.0f);
    world_space_.resize(eye_space_.size());
}

void Viewmodel::AddArm(float side) {
    AddLimb(Mirror(kShoulder, side), Mirror(kElbow, side), {0.135f, 0.135f}, kSleeve);
    AddLimb(Mirror(kElbow, side), Mirror(kWrist, side), {0.105f, 0.105f}, kSkin);
    // The hand is wider than it is tall: a flat palm turned down, not a fist.
    AddLimb(Mirror(kWrist, side), Mirror(kKnuckles, side), {0.105f, 0.062f}, kSkin);
    AddLimb(Mirror(kThumbBase, side), Mirror(kThumbTip, side), {0.045f, 0.045f}, kSkin);
}

void Viewmodel::AddLimb(XMFLOAT3 start, XMFLOAT3 end, XMFLOAT2 thickness, XMFLOAT3 color) {
    const XMVECTOR from = XMLoadFloat3(&start);
    const XMVECTOR to = XMLoadFloat3(&end);
    const XMVECTOR along = XMVectorSubtract(to, from);

    // The unit cube's +Z face is pushed onto the limb's axis, and the other two
    // axes complete a right-handed frame around it. Rebuilding the frame from the
    // endpoints -- rather than negating X to mirror the arm -- is what keeps the
    // determinant positive: a mirrored transform would reverse the winding of
    // every triangle and the left arm would be culled inside out.
    const XMVECTOR forward = XMVector3Normalize(along);
    const XMVECTOR hint = std::abs(XMVectorGetY(forward)) > 0.9f
                              ? XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f)
                              : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(hint, forward));
    const XMVECTOR up = XMVector3Cross(forward, right);

    // Two boxes that merely touch at a joint leave a wedge of daylight on the
    // outside of the bend. Each limb is grown backwards past `start` by half its
    // own thickness so it buries its base inside the limb before it. The far end
    // stays exactly on `end`, which is what keeps the fingertips where they were
    // authored.
    const float overlap = 0.5f * std::max(thickness.x, thickness.y);
    const float length = XMVectorGetX(XMVector3Length(along)) + overlap;
    const XMVECTOR midpoint = XMVectorSubtract(XMVectorScale(XMVectorAdd(from, to), 0.5f),
                                               XMVectorScale(forward, overlap * 0.5f));

    const XMMATRIX transform(XMVectorScale(right, thickness.x), XMVectorScale(up, thickness.y),
                             XMVectorScale(forward, length), XMVectorSetW(midpoint, 1.0f));

    MeshInstance limb{};
    limb.model = cube_;
    XMStoreFloat4x4(&limb.transform, transform);
    limb.tint = color;
    limb.checker = 0.0f;
    eye_space_.push_back(limb);
}

ViewmodelPose Viewmodel::Pose(const XMMATRIX& camera_to_world) {
    for (size_t i = 0; i < eye_space_.size(); ++i) {
        const XMMATRIX local = XMLoadFloat4x4(&eye_space_[i].transform);
        world_space_[i] = eye_space_[i];
        XMStoreFloat4x4(&world_space_[i].transform, local * camera_to_world);
    }

    // Rotating the eye-space sun by the camera's basis is what pins it to the
    // player's head. TransformNormal drops the translation, which is what a
    // direction wants, and the basis is orthonormal, so it stays unit length.
    ViewmodelPose pose{};
    pose.instances = world_space_;
    XMStoreFloat3(&pose.sun_direction,
                  XMVector3TransformNormal(XMVector3Normalize(XMLoadFloat3(&kEyeSunDirection)),
                                           camera_to_world));
    return pose;
}
