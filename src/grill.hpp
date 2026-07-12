#pragma once

#include <cstdint>

namespace physx {
class PxRigidDynamic;
}
class Scene;
class Physics;

// The kettle grill, promoted from a static prop to a single dynamic PhysX body so
// the player can knock it over by running into it. It is built from the grill
// model's own collider boxes -- one shape per leg, body, lid and shelf -- so it
// tips and tumbles as one rigid object, weight up in the kettle and feet narrow.
//
// The scene still owns the grill's draw instance; Grill just owns its body and
// reads the body's pose back into that instance every frame, so wherever the
// grill has fallen is where it draws (and casts its shadow). It is not carryable
// and never enters Props' pick query -- its body carries no userData, which is
// exactly what that query filters on.
class Grill {
public:
    // Builds the dynamic body in the physics scene from Scene::Grill(), spawned at
    // the grill instance's authored transform.
    Grill(Scene& scene, Physics& physics);

    // Reads the body's current pose into the scene's grill instance. Called after
    // Physics::Step advances the simulation and before the frame is drawn.
    void Update();

private:
    Scene* scene_;
    physx::PxRigidDynamic* body_ = nullptr;
    std::uint32_t instance_ = 0; // index into Scene::Instances() of the grill
};
