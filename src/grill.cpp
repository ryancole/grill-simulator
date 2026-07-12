#include "grill.hpp"

#include "physics.hpp"
#include "scene.hpp"

#include <PxPhysicsAPI.h>

using namespace DirectX;
using namespace physx;

namespace {

// A kettle grill is light and top-heavy: a shallow steel bowl on three thin legs.
// Twelve kilos keeps it easy to shove and quick to tip, which is the whole point.
constexpr float kGrillMass = 12.0f;

} // namespace

Grill::Grill(Scene& scene, Physics& physics) : scene_(&scene) {
    const DynamicBody& grill = scene.Grill();
    instance_ = grill.instance;
    body_ = physics.AddDynamicBody(grill.shapes, grill.initial_transform, kGrillMass);
}

void Grill::Update() {
    // The body's global pose is the grill's model-to-world transform: the shapes
    // were placed in the model's own space and the body spawned at the model
    // origin, so nothing here has to unwind a centre-of-mass offset. Read it back
    // into the draw instance and the renderer follows the tumble for free.
    const PxTransform pose = body_->getGlobalPose();
    const XMMATRIX transform =
        XMMatrixRotationQuaternion(XMVectorSet(pose.q.x, pose.q.y, pose.q.z, pose.q.w)) *
        XMMatrixTranslation(pose.p.x, pose.p.y, pose.p.z);
    scene_->SetInstanceTransform(instance_, transform);
}
