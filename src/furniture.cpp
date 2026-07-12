#include "furniture.hpp"

#include "physics.hpp"
#include "scene.hpp"

#include <PxPhysicsAPI.h>

using namespace DirectX;
using namespace physx;

Furniture::Furniture(Scene& scene, Physics& physics) : scene_(&scene) {
    for (const DynamicBody& body : scene.DynamicBodies()) {
        PxRigidDynamic* actor =
            physics.AddDynamicBody(body.shapes, body.initial_transform, body.mass);
        bodies_.push_back({actor, body.instance});
    }
}

void Furniture::Update() {
    // Each body's global pose is that object's model-to-world transform: the shapes
    // were placed in the model's own space and the body spawned at the model
    // origin, so nothing here has to unwind a centre-of-mass offset. Read it back
    // into the draw instance and the renderer follows the tumble for free.
    for (const Body& body : bodies_) {
        const PxTransform pose = body.actor->getGlobalPose();
        const XMMATRIX transform =
            XMMatrixRotationQuaternion(XMVectorSet(pose.q.x, pose.q.y, pose.q.z, pose.q.w)) *
            XMMatrixTranslation(pose.p.x, pose.p.y, pose.p.z);
        scene_->SetInstanceTransform(body.instance, transform);
    }
}
