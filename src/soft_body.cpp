#include "soft_body.hpp"

#include "model.hpp"
#include "physics.hpp"

#include <PxPhysicsAPI.h>
// PxPhysicsAPI.h does not pull the deformable-volume extension in, so it is named
// here explicitly along with the tet-mesh helper the embedding will want.
#include <extensions/PxDeformableVolumeExt.h>
#include <extensions/PxTetMakerExt.h>
#include <extensions/PxTetrahedronMeshExt.h>

#include <cudamanager/PxCudaContext.h>
#include <cudamanager/PxCudaContextManager.h>

#include <cstring>
#include <string>

using namespace physx;
using namespace DirectX;

namespace {

// The meat's elasticity, in the units a deformable volume takes.
//
// Young's modulus is stiffness in pascals: how hard the material resists being
// stretched. Real muscle sits somewhere in the tens of kilopascals, and that is
// roughly where this is -- low enough that a patty visibly flattens when it lands,
// high enough that it springs back rather than puddling. This is the one knob worth
// turning if the meat reads as rubber (raise it) or as jelly (lower it).
constexpr PxReal kYoungsModulus = 50000.0f;
// Poisson's ratio: how much it bulges sideways when squashed. Meat is nearly
// incompressible, which wants a value near 0.5 -- but the solver stiffens badly as
// it approaches that limit, so this backs off to a value that still bulges without
// making the simulation fight itself.
constexpr PxReal kPoissonsRatio = 0.4f;
constexpr PxReal kDynamicFriction = 0.6f;
// Density in kg/m^3, a shade above water, which is about right for meat and gives a
// patty-sized body a believable few hundred grams.
constexpr PxReal kDensity = 1050.0f;

// Solver iterations per step. Deformables need more than a rigid body to hold their
// shape; too few and the mesh sags under its own weight instead of settling.
constexpr PxU32 kSolverIterations = 30;

// How many triangles the simulated surface is decimated down to before it is filled
// with tetrahedra. This is the single most important number here: the cost of the cook
// and of every step afterwards follows from it, and the drawn mesh does not care, since
// it rides the simulation through the embedding rather than being simulated. A few
// hundred is enough to carry the shape of a squashing patty.
constexpr int kSimTargetTriangles = 400;

// Builds one triangle soup from every primitive of `model`, in model space, with each
// primitive's own transform already applied -- the tet cooker takes a single closed
// surface, where a glTF is a pile of separately placed pieces.
//
// Decorative primitives are skipped: they are the parts the game already refuses to
// collide with (see Primitive::collides), and a loose decorative shell floating inside
// or outside the real surface is exactly the kind of geometry that makes a cook fail.
struct SurfaceSoup {
    std::vector<PxVec3> positions;
    std::vector<PxU32> indices;
};

// Turns the cooker's verdict on a surface into something readable, so a meat that
// refuses to cook can say what is wrong with it rather than just failing.
std::string DescribeAnalysis(PxTriangleMeshAnalysisResults results) {
    struct Named {
        PxTriangleMeshAnalysisResult::Enum flag;
        const char* text;
    };
    static const Named kNamed[] = {
        {PxTriangleMeshAnalysisResult::eZERO_VOLUME, "flat (no volume to fill)"},
        {PxTriangleMeshAnalysisResult::eOPEN_BOUNDARIES, "open boundaries"},
        {PxTriangleMeshAnalysisResult::eSELF_INTERSECTIONS, "self-intersections"},
        {PxTriangleMeshAnalysisResult::eINCONSISTENT_TRIANGLE_ORIENTATION,
         "inconsistent winding"},
        {PxTriangleMeshAnalysisResult::eCONTAINS_ACUTE_ANGLED_TRIANGLES, "acute triangles"},
        {PxTriangleMeshAnalysisResult::eEDGE_SHARED_BY_MORE_THAN_TWO_TRIANGLES,
         "edges shared by 3+ triangles"},
        {PxTriangleMeshAnalysisResult::eCONTAINS_INVALID_POINTS, "invalid points"},
        {PxTriangleMeshAnalysisResult::eTRIANGLE_INDEX_OUT_OF_RANGE, "index out of range"},
    };
    std::string described;
    for (const Named& named : kNamed) {
        if (results & named.flag) {
            if (!described.empty()) {
                described += ", ";
            }
            described += named.text;
        }
    }
    return described.empty() ? "unspecified" : described;
}

SurfaceSoup BuildSurface(const Model& model) {
    SurfaceSoup soup;
    soup.positions.reserve(model.vertices.size());

    // Every primitive indexes the one shared vertex array but places it with its own
    // transform, so a vertex used by two differently-placed primitives lands in two
    // different spots. Emitting fresh vertices per primitive is the only way to keep
    // both; the welding pass in cooking folds the duplicates back together.
    for (const Primitive& primitive : model.primitives) {
        if (!primitive.collides) {
            continue;
        }
        const XMMATRIX to_model = XMLoadFloat4x4(&primitive.transform);
        const PxU32 base = static_cast<PxU32>(soup.positions.size());
        // Remaps a model-wide vertex index to this primitive's freshly emitted copy,
        // so the same source vertex reached twice within one primitive is emitted once.
        std::vector<PxU32> remap(model.vertices.size(), 0xFFFFFFFFu);
        for (std::uint32_t i = 0; i < primitive.index_count; ++i) {
            const std::uint32_t source = model.indices[primitive.first_index + i];
            if (remap[source] == 0xFFFFFFFFu) {
                const XMVECTOR position =
                    XMVector3Transform(XMLoadFloat3(&model.vertices[source].position), to_model);
                XMFLOAT3 placed;
                XMStoreFloat3(&placed, position);
                remap[source] = static_cast<PxU32>(soup.positions.size());
                soup.positions.push_back(PxVec3(placed.x, placed.y, placed.z));
            }
            soup.indices.push_back(remap[source]);
        }
        (void)base;
    }
    return soup;
}

} // namespace

SoftBody::SoftBody(Physics& physics, const Model& model, const XMFLOAT4X4& pose, float scale)
    : physics_(&physics) {
    // Deformable volumes are a GPU-only feature. Without a CUDA context there is
    // nothing to create, and the caller draws the meat rigid instead.
    if (!physics.GpuActive()) {
        return;
    }

    const SurfaceSoup soup = BuildSurface(model);
    if (soup.positions.empty() || soup.indices.size() < 3) {
        return;
    }

    // Decimate before cooking. The drawn model is far denser than anything worth
    // simulating -- a conforming tet cook fills the *inside* of the surface it is given,
    // so a few thousand surface triangles becomes a tetrahedral mesh large enough that
    // both the cook and every subsequent step stop being tractable. (Handing the burger
    // over undecimated does not fail, it simply never finishes.)
    //
    // Coarse is also what is wanted physically: the simulation only has to carry the
    // gross squash of the meat, and the drawn mesh rides along on top of it through the
    // embedding rather than being simulated itself. Disconnected patches are dropped
    // with it, so a model built from several loose shells simulates as its largest
    // piece instead of as a cloud of fragments.
    PxArray<PxVec3> dense_points;
    PxArray<PxU32> dense_indices;
    dense_points.resize(static_cast<PxU32>(soup.positions.size()));
    dense_indices.resize(static_cast<PxU32>(soup.indices.size()));
    std::memcpy(dense_points.begin(), soup.positions.data(),
                soup.positions.size() * sizeof(PxVec3));
    std::memcpy(dense_indices.begin(), soup.indices.data(), soup.indices.size() * sizeof(PxU32));

    PxArray<PxVec3> points;
    PxArray<PxU32> indices;
    PxTetMaker::simplifyTriangleMesh(dense_points, dense_indices, kSimTargetTriangles,
                                     /*maximalEdgeLength=*/0.0f, points, indices,
                                     /*vertexMap=*/nullptr, /*edgeLengthCostWeight=*/0.1f,
                                     /*flatnessDetectionThreshold=*/0.01f,
                                     /*projectSimplifiedPointsOnInputMeshSurface=*/false,
                                     /*outputVertexToInputTriangle=*/nullptr,
                                     /*removeDisconnectedPatches=*/true);
    if (points.size() < 4 || indices.size() < 12) {
        status_ = "decimated away to nothing";
        return;
    }

    PxSimpleTriangleMesh surface;
    surface.points.count = points.size();
    surface.points.stride = sizeof(PxVec3);
    surface.points.data = points.begin();
    surface.triangles.count = indices.size() / 3;
    surface.triangles.stride = 3 * sizeof(PxU32);
    surface.triangles.data = indices.begin();

    // Ask first whether this surface can be tetrahedralized at all. This is not
    // belt-and-braces: the cooker does NOT politely return null on bad input, it walks
    // off the end of it and takes the process down -- so a model that is flat, or
    // inconsistently wound, or full of NaNs has to be turned away here, before it is
    // ever handed over. Which is also exactly the per-asset answer worth having: a meat
    // that will not cook stays rigid, and says why.
    const PxTriangleMeshAnalysisResults analysis = PxTetMaker::validateTriangleMesh(surface);
    if (analysis & PxTriangleMeshAnalysisResult::eMESH_IS_INVALID) {
        status_ = DescribeAnalysis(analysis);
        return;
    }
    // "Problematic" is a warning, not a refusal -- open boundaries get filled, acute
    // triangles just cook into a poorer tet mesh. Worth recording, not worth rejecting.
    status_ = (analysis & PxTriangleMeshAnalysisResult::eMESH_IS_PROBLEMATIC)
                  ? DescribeAnalysis(analysis)
                  : "clean";

    // Welding is what turns the per-primitive duplicates above -- and whatever seams
    // the asset itself carries -- back into the shared corners a solid needs. Without
    // it the surface is not closed and the cook has nothing to fill.
    PxCookingParams params(physics.Sdk().getTolerancesScale());
    params.meshPreprocessParams |= PxMeshPreprocessingFlag::eWELD_VERTICES;
    params.meshWeldTolerance = 1e-4f;
    // The simulation runs on the GPU, and a mesh cooked without this carries none of
    // the data the GPU pipeline needs. It defaults to false, which is the right default
    // for the rigid meshes the rest of the game cooks and the wrong one here.
    params.buildGPUData = true;

    // The conforming cook: the tetrahedra match the surface, so the simulation mesh and
    // the collision mesh are the same thing. The alternative (a coarse voxel simulation
    // mesh wrapped around a finer collision mesh) simulates more cheaply and stably, but
    // it decouples the two meshes and makes the embedding a second problem; a meat is
    // small enough that the simpler shape is worth more here than the cost saved.
    //
    // `validate` on: it makes the cooker inspect the surface and refuse deficient input
    // rather than silently emitting a mesh that explodes on the first step.
    mesh_ = PxDeformableVolumeExt::createDeformableVolumeMeshNoVoxels(
        params, surface, physics.Sdk().getPhysicsInsertionCallback(), 1.5f, /*validate=*/true);
    if (mesh_ == nullptr) {
        return; // The asset would not tetrahedralize; Active() stays false.
    }

    material_ = physics.Sdk().createDeformableVolumeMaterial(kYoungsModulus, kPoissonsRatio,
                                                             kDynamicFriction);
    if (material_ == nullptr) {
        Destroy();
        return;
    }

    const XMVECTOR translation = XMVectorSet(pose._41, pose._42, pose._43, 1.0f);
    XMVECTOR scale_out;
    XMVECTOR rotation;
    XMVECTOR position;
    XMMatrixDecompose(&scale_out, &rotation, &position, XMLoadFloat4x4(&pose));
    XMFLOAT4 quaternion;
    XMStoreFloat4(&quaternion, rotation);
    const PxTransform transform(
        PxVec3(XMVectorGetX(translation), XMVectorGetY(translation), XMVectorGetZ(translation)),
        PxQuat(quaternion.x, quaternion.y, quaternion.z, quaternion.w));

    volume_ = PxDeformableVolumeExt::createDeformableVolumeFromMesh(
        mesh_, transform, *material_, *physics.Cuda(), kDensity, scale);
    if (volume_ == nullptr) {
        Destroy();
        return;
    }
    volume_->setSolverIterationCounts(kSolverIterations);
    // A patty folding onto itself is not a thing worth simulating, and self-collision is
    // the most expensive part of a deformable step.
    volume_->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, true);

    physics.Scene().addActor(*volume_);

    // Stage the body's initial state through pinned host memory: the helper fills these
    // from the shape the cook produced, transform() places and scales them, and
    // copyToDevice pushes the lot to the GPU the simulation runs on. Without this the
    // volume starts at the cooked mesh's own origin, not where the caller asked for it.
    PxVec4* sim_positions = nullptr;
    PxVec4* sim_velocities = nullptr;
    PxVec4* collision_positions = nullptr;
    PxVec4* rest_positions = nullptr;
    PxDeformableVolumeExt::allocateAndInitializeHostMirror(
        *volume_, physics.Cuda(), sim_positions, sim_velocities, collision_positions,
        rest_positions);
    PxDeformableVolumeExt::transform(*volume_, transform, scale, sim_positions, sim_velocities,
                                     collision_positions, rest_positions);
    PxDeformableVolumeExt::updateMass(*volume_, kDensity, /*maxInvMassRatio=*/50.0f,
                                      sim_positions);
    PxDeformableVolumeExt::copyToDevice(*volume_, PxDeformableVolumeDataFlag::eALL, sim_positions,
                                        sim_velocities, collision_positions, rest_positions);
    sim_positions_pinned_ = sim_positions;
    sim_velocities_pinned_ = sim_velocities;
    collision_positions_pinned_ = collision_positions;
    rest_positions_pinned_ = rest_positions;

    // The connectivity, copied out once: deformation moves vertices and never rewires
    // them, so this is fixed for the body's life and the embedding binds against it.
    const PxTetrahedronMesh* simulation = volume_->getSimulationMesh();
    const PxU32 vertex_count = simulation->getNbVertices();
    const PxU32 tetrahedron_count = simulation->getNbTetrahedrons();
    readback_.resize(vertex_count);
    sim_positions_.resize(vertex_count);
    sim_tetrahedra_.resize(static_cast<size_t>(tetrahedron_count) * 4);
    // A tet mesh this small is always 32-bit indexed, but the flag is what says so.
    if (simulation->getTetrahedronMeshFlags() & PxTetrahedronMeshFlag::e16_BIT_INDICES) {
        const auto* source = static_cast<const PxU16*>(simulation->getTetrahedrons());
        for (size_t i = 0; i < sim_tetrahedra_.size(); ++i) {
            sim_tetrahedra_[i] = source[i];
        }
    } else {
        const auto* source = static_cast<const PxU32*>(simulation->getTetrahedrons());
        std::memcpy(sim_tetrahedra_.data(), source, sim_tetrahedra_.size() * sizeof(PxU32));
    }

    // Seed the host mirror from the staged state, so SimPositions() is already the body's
    // starting shape on the frame it is created rather than a block of zeroes.
    Update();
}

SoftBody::~SoftBody() {
    Destroy();
}

void SoftBody::Destroy() {
    // The pinned buffers came from the CUDA allocator, so they go back to it.
    if (physics_ != nullptr && physics_->Cuda() != nullptr) {
        PxCudaContextManager* cuda = physics_->Cuda();
        if (sim_positions_pinned_ != nullptr) {
            cuda->freePinnedHostBuffer(sim_positions_pinned_);
        }
        if (sim_velocities_pinned_ != nullptr) {
            cuda->freePinnedHostBuffer(sim_velocities_pinned_);
        }
        if (collision_positions_pinned_ != nullptr) {
            cuda->freePinnedHostBuffer(collision_positions_pinned_);
        }
        if (rest_positions_pinned_ != nullptr) {
            cuda->freePinnedHostBuffer(rest_positions_pinned_);
        }
    }
    sim_positions_pinned_ = nullptr;
    sim_velocities_pinned_ = nullptr;
    collision_positions_pinned_ = nullptr;
    rest_positions_pinned_ = nullptr;

    if (volume_ != nullptr) {
        // Removing it from the scene first mirrors how the rigid actors are torn down,
        // and keeps the release off a scene that still lists it.
        if (physics_ != nullptr) {
            physics_->Scene().removeActor(*volume_);
        }
        volume_->release();
        volume_ = nullptr;
    }
    if (mesh_ != nullptr) {
        mesh_->release();
        mesh_ = nullptr;
    }
    if (material_ != nullptr) {
        material_->release();
        material_ = nullptr;
    }
}

void SoftBody::Update() {
    if (volume_ == nullptr) {
        return;
    }
    // The simulated positions live on the device. This is the whole per-frame cost of a
    // soft body on the CPU side, and it is deliberately the *simulation* mesh rather
    // than anything render-sized: a few hundred vertices, one copy.
    PxCudaContext* context = physics_->Cuda()->getCudaContext();
    context->memcpyDtoH(readback_.data(),
                        reinterpret_cast<CUdeviceptr>(volume_->getSimPositionInvMassBufferD()),
                        readback_.size() * sizeof(PxVec4));
    for (size_t i = 0; i < readback_.size(); ++i) {
        sim_positions_[i] = XMFLOAT3(readback_[i].x, readback_[i].y, readback_[i].z);
    }
}

XMFLOAT3 SoftBody::Centroid() const {
    if (sim_positions_.empty()) {
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    XMVECTOR sum = XMVectorZero();
    for (const XMFLOAT3& position : sim_positions_) {
        sum = XMVectorAdd(sum, XMLoadFloat3(&position));
    }
    XMFLOAT3 centroid;
    XMStoreFloat3(&centroid, XMVectorScale(sum, 1.0f / static_cast<float>(sim_positions_.size())));
    return centroid;
}
