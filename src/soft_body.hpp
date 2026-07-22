#pragma once

#include "model.hpp" // Vertex: the skinned mesh handed back is made of these

#include <DirectXMath.h>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

class Physics;

// One run of the skinned mesh that came from a single primitive of the source model.
// The mesh is flattened -- every primitive's vertices are emitted with that primitive's
// own transform already applied, because a skinned buffer has no per-draw transform
// left to apply -- so this is what puts the pieces back: which slice of the index
// buffer belongs to which of the model's primitives, and therefore which material.
struct SoftBodyPrimitive {
    std::uint32_t first_index = 0;
    std::uint32_t index_count = 0;
    // Index into the source Model::primitives, for the material the renderer draws it
    // with. The soft body does not care what a material is; it only has to not lose
    // which one this run wants.
    std::uint32_t source_primitive = 0;
};

// One deformable mesh, as it stands this frame. `mesh` is a handle from
// Renderer::CreateDeformableMesh and `vertices` is that frame's skinned vertex stream,
// straight from SoftBody::SkinnedVertices() -- already in world space, so unlike a
// MeshInstance this carries no transform. The rest is what a MeshInstance carries for
// look: the browning tint, and the wet sheen.
struct SoftMeshInstance {
    std::uint32_t mesh = 0;
    std::span<const Vertex> vertices;
    DirectX::XMFLOAT3 tint{1.0f, 1.0f, 1.0f};
    float emissive = 0.0f;
    float wetness = 0.0f;
};

// Forward-declared so this header stays free of <PxPhysicsAPI.h>, exactly as
// physics.hpp is; only soft_body.cpp pulls the SDK in. Everything the SDK hands
// back is a raw pointer with an SDK-managed lifetime.
namespace physx {
class PxDeformableVolume;
class PxDeformableVolumeMaterial;
class PxDeformableVolumeMesh;
} // namespace physx

// One meat, simulated as a deformable volume: a finite-element body that squashes
// and springs back where a rigid body could only slide and tumble. This is the
// game's second GPU-side simulation, after the lighter-fluid spray (see Fluid) --
// PhysX 5's deformable volumes are GPU-only, so like the fluid the whole class
// degrades to inert stubs on a machine with no CUDA device: Active() is false, the
// spans stay empty, and the caller falls back to drawing the meat rigid.
//
// The simulation does not run on the model the game draws. A deformable volume
// wants a *tetrahedral* mesh -- a solid packed with tets, not a hollow shell of
// triangles -- so the constructor cooks one from the model's surface. What comes
// back has its own, entirely different vertices, which is why nothing here is
// enough to render with on its own; SimPositions() is the raw simulated state, and
// carrying the drawn mesh along on top of it is the next step (the embedding, which
// binds each of the model's vertices into a tet).
//
// Cooking is the fragile part and it is allowed to fail: a model that is not
// watertight, or that has self-intersections or stray loose shells, can refuse to
// tetrahedralize or produce a mesh too degenerate to simulate. That is a property
// of the asset, not a bug to work around, so a failure leaves Active() false and
// says so rather than throwing -- one uncookable meat must not take the level down.
class SoftBody {
public:
    // Cooks `model` into a tetrahedral mesh and drops a deformable volume for it into
    // the physics scene at `pose` (a translation/rotation; `scale` sizes it, since the
    // cooked mesh is in the model's own units). Leaves the body inert if there is no
    // GPU or the cook fails -- check Active() before relying on it.
    SoftBody(Physics& physics, const Model& model, const DirectX::XMFLOAT4X4& pose,
             float scale = 1.0f);
    ~SoftBody();

    SoftBody(const SoftBody&) = delete;
    SoftBody& operator=(const SoftBody&) = delete;

    // Whether a volume actually exists -- a CUDA context came up AND the model cooked.
    // When false every other call is a cheap no-op and the spans are empty.
    bool Active() const { return volume_ != nullptr; }

    // Why the body is in the state it is: "clean" when the model cooked without
    // complaint, otherwise what the cooker found wrong with it ("flat (no volume to
    // fill)", "inconsistent winding", ...). Worth surfacing rather than swallowing --
    // whether a given meat can be soft at all is a property of its asset, and this is
    // the only thing that says so.
    const std::string& Status() const { return status_; }

    // Once per frame, after Physics::Step: copies the simulated vertex positions back
    // from the GPU into the host mirror behind SimPositions(). The read is small -- a
    // tet mesh has hundreds of vertices where the drawn model has thousands -- which
    // is what makes doing the skinning on the CPU afterwards cheap.
    void Update();

    // The simulation mesh's vertices, in world space, as of the last Update. These are
    // tet corners, not the model's vertices: useful for driving the embedding and for
    // seeing that the body is moving at all, not for drawing.
    std::span<const DirectX::XMFLOAT3> SimPositions() const { return sim_positions_; }

    // The tetrahedra, four vertex indices each, indexing SimPositions(). Fixed for the
    // body's lifetime -- deformation moves the vertices, never the connectivity -- so
    // the embedding is computed against this once.
    std::span<const std::uint32_t> SimTetrahedra() const { return sim_tetrahedra_; }

    // The average of SimPositions(): where the body is, roughly, without asking what
    // shape it is in. Cheap enough to call every frame.
    DirectX::XMFLOAT3 Centroid() const;

    // The drawn mesh, deformed: the source model's own vertices -- its texture
    // coordinates, its tangents, its silhouette -- with positions and normals carried
    // along by the simulation. This is what the renderer uploads and draws, in world
    // space and needing no model transform, and it is the whole point of the exercise:
    // the meat squashes without becoming a different-looking object.
    //
    // Rebuilt by Update. Flattened across primitives (see SoftBodyPrimitive), so the
    // indices here are the soft body's own, not the source model's.
    std::span<const Vertex> SkinnedVertices() const { return skinned_; }
    std::span<const std::uint32_t> SkinnedIndices() const { return skinned_indices_; }
    std::span<const SoftBodyPrimitive> SkinnedPrimitives() const { return skinned_primitives_; }

    // --- Carrying ---------------------------------------------------------------
    //
    // A carried meat is gripped, not frozen. At pick-up a small patch of simulation
    // vertices around the underside of the grip -- the part resting in the hand or
    // clamped in the jaws -- has its inverse mass zeroed and is written to the carry
    // pose every frame; the solver treats zero-inverse-mass vertices as immovable, so
    // the rest of the meat stays fully simulated and hangs off the pinned patch through
    // the same FEM elasticity that makes it squash. The free part sags, swings when the
    // player turns, and keeps colliding with the world; on release the inverse masses
    // are restored and the body falls from exactly the shape it was dangling in.
    //
    // The writes go through PxDeformableVolumeExt::copyToDevice -- the same path the
    // constructor stages the body's initial placement through -- refreshed each frame
    // from the live readback so the solver's own state is never clobbered with stale
    // data. This deliberately does NOT use PxDeformableVolume's kinematic target buffer:
    // that API is silently ignored unless the scene raises eENABLE_DIRECT_GPU_API, a
    // flag this game cannot afford (it kills every CPU-side pose read; see
    // Props::RebuildTransform and the contact-report audio). Learned the hard way --
    // targets tracked the hand perfectly while the body never moved.
    //
    // Note this also does NOT go through the rigid path's scene-remove (see
    // Props::RemoveBodyFromScene): a deformable volume must stay in the scene to keep
    // simulating its free part, and eDISABLE_SIMULATION is fatal on GPU anyway.

    // Grips the body at `pose`: selects the pinned patch from the shape it is in right
    // now (so a meat picked up mid-squash is carried mid-squash) and drives that patch
    // to the pose. No-op if inert or already gripped.
    void BeginKinematic(const DirectX::XMFLOAT4X4& pose);
    // Moves the gripped patch to `pose`. Call once a frame while carried, after
    // Physics::Step and after Update() -- the buffers must not be written while the
    // solver runs, and the refresh reads the positions Update just brought back.
    void UpdateKinematic(const DirectX::XMFLOAT4X4& pose);
    // Lets go: restores the pinned vertices' inverse masses and hands the body
    // `velocity` -- the hand's toss for a throw, zero for a placed drop -- so a thrown
    // meat leaves the hand moving instead of dying at the release and falling limp.
    // The free vertices keep whatever swing they had on top of it. No-op if not gripped.
    void EndKinematic(const DirectX::XMFLOAT3& velocity = DirectX::XMFLOAT3{0.0f, 0.0f, 0.0f});
    bool Kinematic() const { return kinematic_; }

    // --- Parking ----------------------------------------------------------------
    //
    // A body cooked for a later cook stage -- the chicken's pre-cooked twin -- waits
    // outside the physics scene until its stage arrives: no cost, no collisions, no
    // readback. Scene removal, NOT eDISABLE_SIMULATION, which is fatal on the GPU
    // pipeline (see Props::RemoveBodyFromScene for the same rule on the rigids).

    // Takes the body out of the physics scene. No-op if inert or already parked.
    void Park();
    // Plants the body at `pose` -- its rest shape, bounds' bottom-centre at the pose
    // origin, stilled -- and puts it back in the scene, awake. The whole point is that
    // the swap-in lands exactly where the swapped-out body stood. No-op if inert or
    // not parked.
    void Unpark(const DirectX::XMFLOAT4X4& pose);
    bool Parked() const { return parked_; }

private:
    // Rebuilds SkinnedVertices() from the freshly read-back simulation positions:
    // blends each drawn vertex from its embedding, then rebuilds the normals off the
    // deformed surface. Called by Update.
    void Skin();

    // Frees the volume and its cooked mesh, and returns the object to the inert state.
    // Used both by the destructor and to back out of a half-built body when the cook
    // produces something the simulation will not accept.
    void Destroy();

    Physics* physics_ = nullptr;
    physx::PxDeformableVolume* volume_ = nullptr;
    physx::PxDeformableVolumeMaterial* material_ = nullptr;
    physx::PxDeformableVolumeMesh* mesh_ = nullptr;

    // The pinned host buffers PhysX wants for staging a body's initial state to the
    // device. Allocated through the CUDA context manager, so they are freed through it
    // too -- plain delete[] would corrupt the heap.
    //
    // Held as void* because these are really PxVec4*, and PxVec4 is a typedef of a
    // template (PxVec4T<float>) rather than a class -- so it cannot be forward-declared
    // the way the pointer types above can, and naming it here would drag the whole SDK
    // into this header. soft_body.cpp casts them back.
    void* sim_positions_pinned_ = nullptr;
    void* sim_velocities_pinned_ = nullptr;
    void* collision_positions_pinned_ = nullptr;
    void* rest_positions_pinned_ = nullptr;

    // The device buffer's layout is position xyz + inverse mass in w; the readback
    // lands here and is unpacked into sim_positions_, which is what callers want. Held
    // as XMFLOAT4 rather than PxVec4 -- same sixteen bytes, and it keeps the SDK's
    // definition out of this header.
    std::vector<DirectX::XMFLOAT4> readback_;
    std::vector<DirectX::XMFLOAT3> sim_positions_;
    std::vector<std::uint32_t> sim_tetrahedra_;

    // The embedding: for each skinned vertex, which tetrahedron of the simulation mesh
    // it sits inside, and where inside it (barycentric weights over that tet's four
    // corners). Computed once, against the cooked rest pose; from then on a vertex's
    // position is just those weights applied to wherever the four corners have moved.
    // That is the whole trick -- it makes the drawn mesh follow the simulation without
    // the simulation ever having heard of it.
    std::vector<std::uint32_t> embed_tet_;
    std::vector<DirectX::XMFLOAT4> embed_weights_;

    // The drawn mesh: rest state (the flattened source model, kept to restore texture
    // coordinates and tangents) and the live skinned copy Update rewrites.
    std::vector<Vertex> skinned_;
    std::vector<std::uint32_t> skinned_indices_;
    std::vector<SoftBodyPrimitive> skinned_primitives_;

    // Carrying state (see BeginKinematic). The pinned patch: which simulation vertices
    // are gripped, where each sits in the carry pose's frame (fixed at pick-up, so the
    // grip is rigid while the rest swings), and the inverse mass each had before the
    // grip zeroed it, restored on release.
    bool kinematic_ = false;
    std::vector<std::uint32_t> pinned_;
    std::vector<DirectX::XMFLOAT3> pinned_local_;
    std::vector<float> pinned_inv_mass_;

    // Whether the volume is currently out of the physics scene (see Park). The
    // destructor reads it too: a parked body must not be scene-removed twice.
    bool parked_ = false;

    // See Status(). Set as soon as the cook has an opinion; "no gpu" when there was
    // never a CUDA context to try on.
    std::string status_ = "no gpu";
};
