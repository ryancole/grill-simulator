#pragma once

#include "collision.hpp"
#include "model.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <vector>

// One placement of one model. The renderer draws every primitive of `model`
// under `transform`, and the scene never says anything about how the model is
// built -- a glTF from disk and the unit cube built in code place identically.
struct MeshInstance {
    // Index into Scene::Models().
    std::uint32_t model = 0;
    DirectX::XMFLOAT4X4 transform;
    // Multiplies the material's own base colour. A glTF model carries its
    // colours in its materials and wants white here; the unit cube has no
    // material at all, so this is the only thing that gives it a colour.
    DirectX::XMFLOAT3 tint;
    // Size in metres of one checkerboard tile, projected down the Y axis. Zero
    // leaves the surface flat -- it is only there to give the ground a texture
    // to move against.
    float checker;
};

// The models of the loose objects the player carries. The scene loads them like
// any other prop so the renderer uploads them once, but it places no instances
// of its own -- Props owns their placements and their movement.
struct PropModels {
    std::uint32_t tongs = 0;
    std::uint32_t patty = 0;
    std::uint32_t steak = 0;
};

// A world object heavy enough to want its own dynamic rigid body rather than a
// clutch of immovable static colliders: the grill. The scene draws it like any
// other instance, but hands its collider boxes here -- in the model's own space,
// one per colliding primitive -- so Physics can build a single dynamic body that
// tips as one piece. `instance` is the draw instance the body's pose is read back
// into each frame; `initial_transform` is where that body spawns.
struct DynamicBody {
    std::uint32_t instance = 0;
    DirectX::XMFLOAT4X4 initial_transform;
    std::vector<OrientedBox> shapes;
};

// A backyard: a grill loaded from glTF, and everything else still a box.
class Scene {
public:
    Scene();

    const std::vector<Model>& Models() const { return models_; }
    const std::vector<MeshInstance>& Instances() const { return instances_; }
    const std::vector<OrientedBox>& Colliders() const { return colliders_; }

    // The grill, as a dynamic body: its model-space collider shapes and where it
    // spawns. Handed to Physics::AddDynamicBody so the grill topples when run into.
    const DynamicBody& Grill() const { return grill_; }
    // Rewrites one instance's model-to-world transform. Grill reads the toppling
    // body's pose back into the grill instance through this each frame, so the
    // renderer (and the sun's shadow) draw it wherever it has fallen.
    void SetInstanceTransform(std::uint32_t index, DirectX::FXMMATRIX transform) {
        DirectX::XMStoreFloat4x4(&instances_[index].transform, transform);
    }

    // The shared unit cube. The viewmodel builds its arms out of it too, and
    // needs to name it in the instances it hands the renderer.
    std::uint32_t CubeModel() const { return cube_; }

    // The models Props places and moves. Loaded here, drawn there.
    const PropModels& PropModelIds() const { return prop_models_; }

private:
    std::uint32_t AddModel(Model model);
    // `file` names a .glb under the executable's assets/models/.
    std::uint32_t LoadModel(const char* file);

    // Places one model and gives the collider pass one box per primitive of it.
    // For the cube that is a single box, exactly as if it had been added by hand.
    // For the grill it is a box per leg, body, lid and shelf, rather than one
    // loose box around the whole thing -- which is what keeps the legs from
    // walling off the ground between them.
    void AddInstance(std::uint32_t model, DirectX::FXMMATRIX transform, DirectX::XMFLOAT3 tint,
                     float checker = 0.0f);
    // Places the grill like AddInstance, but diverts its collider boxes into a
    // dynamic body (Grill) instead of the immovable static world -- so it draws in
    // the world pass yet is free to be knocked over. The boxes are recorded in the
    // grill's own model space; the instance transform rides separately as where the
    // body spawns.
    void AddGrill(std::uint32_t model, DirectX::FXMMATRIX transform, DirectX::XMFLOAT3 tint);
    void AddBox(DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 size, float yaw_degrees,
                DirectX::XMFLOAT3 color, float checker = 0.0f);

    std::vector<Model> models_;
    std::vector<MeshInstance> instances_;
    std::vector<OrientedBox> colliders_;
    DynamicBody grill_;

    std::uint32_t cube_ = 0;
    PropModels prop_models_{};
};
