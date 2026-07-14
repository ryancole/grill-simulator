#pragma once

#include "collision.hpp"
#include "heat_source.hpp"
#include "model.hpp"
#include "rigid_body.hpp"

#include <DirectXMath.h>

#include <cstdint>
#include <optional>
#include <vector>

struct LevelDef;

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

// A world object the player can knock over -- the grill, the cooler -- given its
// own dynamic rigid body instead of a clutch of immovable static colliders. The
// scene draws it like any other instance, but hands its collider boxes here (in
// the model's own space, one per colliding primitive) so Physics can build a
// single dynamic body that tips and slides as one piece. `instance` is the draw
// instance the body's pose is read back into each frame, `initial_transform` is
// where it spawns, and `mass` (kg) is fixed while PhysX derives the centre of
// mass and inertia from the shapes -- so a top-heavy grill topples where a low,
// wide cooler mostly shoves.
struct DynamicBody {
    std::uint32_t instance = 0;
    DirectX::XMFLOAT4X4 initial_transform;
    std::vector<OrientedBox> shapes;
    float mass = 1.0f;
    // 1..10, how hard the player finds it to knock over (see BodyTag::knock_rating).
    float knock_rating = 1.0f;
    // The sound the body makes on a hard landing, carried to its BodyTag so the
    // contact report can voice it (the grill clatters; the cooler stays None).
    ImpactSound impact_sound = ImpactSound::None;

    // The heat this body radiates, if any -- present on the grill, empty on the
    // cooler. Its origin is unset here (it is world state Furniture refreshes each
    // frame from the body's pose); this carries only the fixed temperature and reach.
    std::optional<HeatSource> heat;
    // Where the heat centre sits in the model's own space -- up at the grate. Carried
    // separately from the HeatSource because it is fixed body-space data Furniture
    // transforms through the pose to place the source's world origin each frame.
    DirectX::XMFLOAT3 heat_offset{0.0f, 0.0f, 0.0f};
};

// The runtime side of a level: it takes a LevelDef (pure data -- see level.hpp)
// and builds the GPU models, draw instances, static colliders and dynamic bodies
// the rest of the game reads. The level says what stands where; the Scene is what
// that turns into once models are loaded and colliders are cut.
class Scene {
public:
    explicit Scene(const LevelDef& level);

    const std::vector<Model>& Models() const { return models_; }
    const std::vector<MeshInstance>& Instances() const { return instances_; }
    const std::vector<OrientedBox>& Colliders() const { return colliders_; }

    // The knock-over-able world objects, as dynamic bodies: each one's model-space
    // collider shapes, mass and spawn pose. Handed to Physics::AddDynamicBody so
    // they topple or slide when run into.
    const std::vector<DynamicBody>& DynamicBodies() const { return dynamic_bodies_; }
    // Rewrites one instance's model-to-world transform. Furniture reads each
    // toppling body's pose back into its instance through this every frame, so the
    // renderer (and the sun's shadow) draw it wherever it has fallen.
    void SetInstanceTransform(std::uint32_t index, DirectX::FXMMATRIX transform) {
        DirectX::XMStoreFloat4x4(&instances_[index].transform, transform);
    }

    // The shared unit cube is always the first model every level builds, so its
    // index is fixed. The viewmodel builds its arms out of it and names this in
    // the instances it hands the renderer -- and because the index never varies,
    // the (persistent) viewmodel can reference it without holding a live Scene.
    static constexpr std::uint32_t kCubeModel = 0;
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
    // Places a model like AddInstance, but diverts its collider boxes into a
    // dynamic body (see DynamicBodies) instead of the immovable static world -- so
    // it draws in the world pass yet is free to be knocked over. The boxes are
    // recorded in the model's own space; the instance transform rides separately as
    // where the body spawns, and `mass` sets how heavy it is to shove.
    void AddDynamicInstance(std::uint32_t model, DirectX::FXMMATRIX transform,
                            DirectX::XMFLOAT3 tint, float mass, float knock_rating,
                            ImpactSound impact_sound, std::optional<HeatSource> heat,
                            DirectX::XMFLOAT3 heat_offset);

    std::vector<Model> models_;
    std::vector<MeshInstance> instances_;
    std::vector<OrientedBox> colliders_;
    std::vector<DynamicBody> dynamic_bodies_;

    std::uint32_t cube_ = 0;
    PropModels prop_models_{};
};
