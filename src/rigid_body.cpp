#include "rigid_body.hpp"

#include <PxPhysicsAPI.h>

using namespace DirectX;
using namespace physx;

void RigidBody::Adopt(PxRigidDynamic* actor, float knock_rating, int prop_index,
                      ImpactSound impact_sound) {
    actor_ = actor;
    tag_ = BodyTag{prop_index, knock_rating, impact_sound};
}

void RigidBody::Bind() {
    actor_->userData = &tag_;
}

XMMATRIX RigidBody::Transform() const {
    // The global pose *is* the model-to-world transform: the collider shapes were
    // placed in the model's own space and the body spawned at the model origin, so
    // nothing here has to unwind a centre-of-mass offset. Rotate by the quaternion,
    // then translate.
    const PxTransform pose = actor_->getGlobalPose();
    return XMMatrixRotationQuaternion(XMVectorSet(pose.q.x, pose.q.y, pose.q.z, pose.q.w)) *
           XMMatrixTranslation(pose.p.x, pose.p.y, pose.p.z);
}
