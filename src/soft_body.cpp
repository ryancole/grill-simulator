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

#include <algorithm>
#include <cfloat>
#include <cstring>
#include <string>

using namespace physx;
using namespace DirectX;

namespace {

// The meat's elasticity, in the units a deformable volume takes.
//
// Young's modulus is stiffness in pascals: how hard the material resists being
// stretched. Real muscle sits in the tens of kilopascals; this is above that, which
// suits a body being watched rather than measured.
//
// Worth knowing before reaching for this knob: it barely moves the shape the body
// settles into. Raising it sixfold changed the resting height of a dropped patty by
// about a percent, because most of the difference between the cooked rest shape and
// the settled one is not elastic at all -- the voxel simulation mesh circumscribes the
// model, so it starts fractionally larger than the surface it stands in for and gives
// that back on the first contact. This is the knob for how the meat *responds* -- how
// far it gives on impact and how briskly it comes back -- not for where it ends up.
constexpr PxReal kYoungsModulus = 150000.0f;
// Poisson's ratio: how much it bulges sideways when squashed. Meat is nearly
// incompressible, which wants a value near 0.5 -- but the solver stiffens badly as
// it approaches that limit, so this backs off to a value that still bulges without
// making the simulation fight itself.
constexpr PxReal kPoissonsRatio = 0.4f;
constexpr PxReal kDynamicFriction = 0.6f;
// Density in kg/m^3, a shade above water, which is about right for meat and gives a
// patty-sized body a believable few hundred grams.
constexpr PxReal kDensity = 1050.0f;

// Solver iterations per step, and by a wide margin the most expensive number here.
// Measured, for one patty, in Release (physics milliseconds per frame, and the height
// the body settles to):
//
//      1 -> 3.35 ms, 0.0508      8 ->  5.21 ms, 0.0513
//      2 -> 2.97 ms, 0.0507     16 -> 12.68 ms, 0.0518
//      4 -> 3.21 ms, 0.0506     30 -> 78.71 ms  (13 fps)
//
// Two things fall out of that. The cost floors at or below four -- the 1/2/4 readings are
// not monotonic, so that is noise around a floor where something other than iterations
// dominates. And above eight it climbs far faster than the count does, because the scene
// steps on a fixed 1/120 clock with an eight-step catch-up: once a frame overruns, every
// frame runs all eight substeps, so an expensive step makes a slow frame makes more
// expensive steps. Thirty sat at the bottom of that spiral.
//
// Four rather than two: they cost the same within noise, and four keeps some headroom.
// Note what the numbers above do NOT cover -- they are a body at rest. Too few iterations
// shows up under impact (jitter, penetration, a landing that squashes further than it
// should), and a single drop settling in the grass does not exercise that. If a thrown
// meat ever behaves oddly on contact, this is the first number to raise.
constexpr PxU32 kSolverIterations = 4;

// How many triangles the simulated surface is decimated down to before it is filled
// with tetrahedra. This is the single most important number here: the cost of the cook
// and of every step afterwards follows from it, and the drawn mesh does not care, since
// it rides the simulation through the embedding rather than being simulated. A few
// hundred is enough to carry the shape of a squashing patty.
constexpr int kSimTargetTriangles = 400;

// The fewest triangles worth handing to the tetrahedral cooker. Below this there is not
// enough surface to enclose a volume, and the cooker's response to that is an assertion
// rather than an error return -- so this floor is what keeps a lean model from taking
// the process down.
constexpr size_t kMinSimTriangles = 64;

// How many voxels the simulation mesh spans along the model's longest axis. This sets
// how finely the body can bend: too few and a patty squashes as one rigid-ish lump, too
// many and the step cost climbs for detail the eye will not read on something this
// small. It is a resolution knob, not a shape knob -- the silhouette comes from the
// collision mesh and the drawn model, neither of which this touches.
constexpr PxU32 kSimVoxelsAlongLongestAxis = 8;

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

// Flattens the whole model -- every primitive, not just the collidable ones -- into one
// vertex array with each primitive's transform already baked into its positions, normals
// and tangents. The rasterized path applies that transform per draw; a skinned buffer
// cannot, because by then the vertices have been moved by the simulation and there is no
// rigid transform left to speak of. So it is applied once, here, up front.
//
// The runs are recorded as they are emitted (see SoftBodyPrimitive) so the renderer can
// still draw each piece with its own material.
void FlattenForSkinning(const Model& model, std::vector<Vertex>& vertices,
                        std::vector<std::uint32_t>& indices,
                        std::vector<SoftBodyPrimitive>& primitives) {
    for (size_t p = 0; p < model.primitives.size(); ++p) {
        const Primitive& primitive = model.primitives[p];
        const XMMATRIX to_model = XMLoadFloat4x4(&primitive.transform);
        // A normal is carried by the inverse transpose, not the matrix itself, so a
        // primitive placed with a non-uniform scale keeps its lighting right.
        const XMMATRIX normal_matrix =
            XMMatrixTranspose(XMMatrixInverse(nullptr, to_model));

        SoftBodyPrimitive run;
        run.first_index = static_cast<std::uint32_t>(indices.size());
        run.source_primitive = static_cast<std::uint32_t>(p);

        std::vector<std::uint32_t> remap(model.vertices.size(), 0xFFFFFFFFu);
        for (std::uint32_t i = 0; i < primitive.index_count; ++i) {
            const std::uint32_t source = model.indices[primitive.first_index + i];
            if (remap[source] == 0xFFFFFFFFu) {
                const Vertex& in = model.vertices[source];
                Vertex out = in; // uv rides through untouched
                XMStoreFloat3(&out.position,
                              XMVector3Transform(XMLoadFloat3(&in.position), to_model));
                XMStoreFloat3(&out.normal, XMVector3Normalize(XMVector3TransformNormal(
                                               XMLoadFloat3(&in.normal), normal_matrix)));
                // The tangent lies in the surface and stretches with it, so it goes
                // through the matrix itself; w is the bitangent handedness and is kept.
                const XMVECTOR tangent = XMVector3Normalize(
                    XMVector3TransformNormal(XMLoadFloat3(&reinterpret_cast<const XMFLOAT3&>(
                                                 in.tangent)),
                                             to_model));
                XMStoreFloat3(&reinterpret_cast<XMFLOAT3&>(out.tangent), tangent);
                out.tangent.w = in.tangent.w;
                remap[source] = static_cast<std::uint32_t>(vertices.size());
                vertices.push_back(out);
            }
            indices.push_back(remap[source]);
        }
        run.index_count = static_cast<std::uint32_t>(indices.size()) - run.first_index;
        if (run.index_count > 0) {
            primitives.push_back(run);
        }
    }
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
    if (soup.indices.size() / 3 <= static_cast<size_t>(kSimTargetTriangles)) {
        // Already lean enough. Worth checking rather than simplifying unconditionally,
        // because the simplifier does not stop at the target it is given (see below) and
        // running it on a mesh that is already small enough is how a usable surface
        // becomes an unusable one.
        points = std::move(dense_points);
        indices = std::move(dense_indices);
    } else {
        // The target below is a request, not a promise: this simplifier collapses well
        // past it. Asked for 400 triangles it returned 153 from a 19,628-triangle burger
        // and 24 from a 2,000-triangle one -- about one percent either way, regardless of
        // what was asked. The reason is that its flatness heuristic works in absolute
        // units while a meat is three centimetres across, so at the default threshold the
        // entire model reads as flat and collapses. Handing it a large threshold turns
        // that heuristic off (the SDK documents a large value as having no effect), which
        // is what makes the target mean something.
        PxTetMaker::simplifyTriangleMesh(dense_points, dense_indices, kSimTargetTriangles,
                                         /*maximalEdgeLength=*/0.0f, points, indices,
                                         /*vertexMap=*/nullptr, /*edgeLengthCostWeight=*/0.1f,
                                         /*flatnessDetectionThreshold=*/1e30f,
                                         /*projectSimplifiedPointsOnInputMeshSurface=*/false,
                                         /*outputVertexToInputTriangle=*/nullptr,
                                         /*removeDisconnectedPatches=*/true);
    }

    // Too little surface left to fill with tetrahedra. The cooker does not report this,
    // it asserts and takes the process down, so the floor is enforced here. Falling back
    // to the undecimated soup is better than refusing: a model small enough to collapse
    // this far is small enough to simulate whole.
    if (indices.size() / 3 < kMinSimTriangles) {
        points = std::move(dense_points);
        indices = std::move(dense_indices);
    }
    if (points.size() < 4 || indices.size() / 3 < kMinSimTriangles) {
        status_ = "too little geometry to simulate";
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

    // The voxel cook, not the conforming one. Both fill the surface with tetrahedra, but
    // they differ in a way that decides whether the body holds its shape at all:
    //
    // A conforming cook makes the tetrahedra follow the surface exactly, which around a
    // filled hole or a decimated crease means slivers -- tetrahedra so flat they have
    // almost no volume. A sliver has almost no resistance to being squashed along its
    // thin axis, so a mesh full of them collapses under its own weight however stiff the
    // material claims to be. That is what a conforming cook of the burger did: it settled
    // at a fifth of its height, which is the solver giving up rather than meat squashing.
    //
    // The voxel cook instead lays the simulation mesh out on a regular grid, so every
    // tetrahedron is well proportioned by construction and none of them can collapse.
    // The surface-matching mesh is still cooked and kept, as the *collision* mesh -- so
    // the body still collides with its real shape while simulating on the sane one. The
    // two meshes then differ, which matters downstream: the embedding below binds the
    // drawn vertices to the simulation mesh, since that is the one that is read back.
    //
    // `validate` on: it makes the cooker inspect the surface and refuse deficient input
    // rather than silently emitting a mesh that explodes on the first step.
    mesh_ = PxDeformableVolumeExt::createDeformableVolumeMesh(
        params, surface, kSimVoxelsAlongLongestAxis, physics.Sdk().getPhysicsInsertionCallback(),
        /*validate=*/true);
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

    // --- The embedding: bind the drawn mesh into the simulation mesh. ---
    //
    // Both sides have to be in the same space for this, and the space is the cooked rest
    // pose in model coordinates: the simulation mesh's own vertices as the cooker laid
    // them out (the transform above moved the *device* copy, not these), against the
    // flattened model vertices, which are in model space too. What comes back is, per
    // drawn vertex, a tetrahedron and four weights inside it.
    //
    // From then on the pose is implicit. Skinning reads the *deformed, world-space* tet
    // corners and applies these same weights, so the result is already in world space --
    // the placement, the rotation and the deformation all arrive together, and the draw
    // needs no model matrix at all.
    FlattenForSkinning(model, skinned_, skinned_indices_, skinned_primitives_);

    PxArray<PxVec3> tet_vertices;
    tet_vertices.resize(vertex_count);
    std::memcpy(tet_vertices.begin(), simulation->getVertices(), vertex_count * sizeof(PxVec3));
    PxArray<PxU32> tet_indices;
    tet_indices.resize(static_cast<PxU32>(sim_tetrahedra_.size()));
    std::memcpy(tet_indices.begin(), sim_tetrahedra_.data(),
                sim_tetrahedra_.size() * sizeof(PxU32));

    PxArray<PxVec3> embed_points;
    embed_points.resize(static_cast<PxU32>(skinned_.size()));
    for (size_t i = 0; i < skinned_.size(); ++i) {
        const XMFLOAT3& p = skinned_[i].position;
        embed_points[static_cast<PxU32>(i)] = PxVec3(p.x, p.y, p.z);
    }

    PxArray<PxVec4> weights;
    PxArray<PxU32> links;
    PxTetrahedronMeshExt::createPointsToTetrahedronMap(tet_vertices, tet_indices, embed_points,
                                                       weights, links);

    embed_tet_.resize(skinned_.size());
    embed_weights_.resize(skinned_.size());
    for (size_t i = 0; i < skinned_.size(); ++i) {
        embed_tet_[i] = links[static_cast<PxU32>(i)];
        const PxVec4& w = weights[static_cast<PxU32>(i)];
        embed_weights_[i] = XMFLOAT4(w.x, w.y, w.z, w.w);
    }

    // Seed the host mirror from the staged state, so SimPositions() is already the body's
    // starting shape on the frame it is created rather than a block of zeroes -- and
    // skin once off it, so SkinnedVertices() is drawable before the first step.
    Update();
}

SoftBody::~SoftBody() {
    Destroy();
}

void SoftBody::Destroy() {
    // The pinned buffers came from the CUDA allocator, so they go back to it.
    if (physics_ != nullptr && physics_->Cuda() != nullptr) {
        PxCudaContextManager* cuda = physics_->Cuda();
        kinematic_ = false;
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
        // and keeps the release off a scene that still lists it. A parked body already
        // left the scene, and removing it twice is an SDK error.
        if (physics_ != nullptr && !parked_) {
            physics_->Scene().removeActor(*volume_);
        }
        volume_->release();
        volume_ = nullptr;
    }
    parked_ = false;
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
    Skin();
}

namespace {

// How much of the shape the grip claims: vertices in the bottom slab of the carry-frame
// bounding box, within a radius of its centre axis. Bottom-centre reads as "resting in
// the palm" for the hand and "on the lower jaw" for the tongs, and it leaves the rim and
// top free to sag -- pin too much and the meat carries rigid, pin too little and it
// dangles off a point like a rag. Fractions of the shape's own extents so one pair of
// numbers fits a patty and a drumstick alike.
constexpr float kGripHeightFraction = 0.35f;
constexpr float kGripRadiusFraction = 0.55f;

} // namespace

void SoftBody::BeginKinematic(const XMFLOAT4X4& pose) {
    if (volume_ == nullptr || kinematic_ || sim_positions_.empty()) {
        return;
    }

    // Work in the carry pose's frame, from the shape the body is in right now. Live
    // positions rather than the cooked rest shape, so a meat picked up mid-squash is
    // carried mid-squash instead of snapping back to a fresh patty.
    const XMMATRIX to_world = XMLoadFloat4x4(&pose);
    XMVECTOR determinant;
    const XMMATRIX to_local = XMMatrixInverse(&determinant, to_world);
    std::vector<XMFLOAT3> local(sim_positions_.size());
    XMFLOAT3 lo{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < sim_positions_.size(); ++i) {
        XMStoreFloat3(&local[i], XMVector3Transform(XMLoadFloat3(&sim_positions_[i]), to_local));
        lo = {std::min(lo.x, local[i].x), std::min(lo.y, local[i].y), std::min(lo.z, local[i].z)};
        hi = {std::max(hi.x, local[i].x), std::max(hi.y, local[i].y), std::max(hi.z, local[i].z)};
    }

    // The pinned patch: the bottom-centre of the carry-frame bounds. Their original
    // inverse masses come out of the live readback (position xyz + inverse mass in w),
    // recorded before the grip zeroes them so release can put them back.
    //
    // The locals are stored relative to the bounds' bottom-centre `anchor`, not to where
    // the shape happens to be: the anchor is where the model origin sits (the underside
    // -- see DeriveBodyShape), so mapping anchor-relative locals through the carry pose
    // carries the meat AT the hand. Without the subtraction the shape's world offset at
    // grab time would be baked in, and a meat grabbed at arm's reach would be carried at
    // arm's reach for ever.
    const float grip_ceiling = lo.y + (hi.y - lo.y) * kGripHeightFraction;
    const float cx = (lo.x + hi.x) * 0.5f;
    const float cz = (lo.z + hi.z) * 0.5f;
    const XMFLOAT3 anchor{cx, lo.y, cz};
    const float max_radius =
        0.5f * std::max(hi.x - lo.x, hi.z - lo.z) * kGripRadiusFraction;
    pinned_.clear();
    pinned_local_.clear();
    pinned_inv_mass_.clear();
    for (size_t i = 0; i < local.size(); ++i) {
        const float dx = local[i].x - cx;
        const float dz = local[i].z - cz;
        if (local[i].y <= grip_ceiling && dx * dx + dz * dz <= max_radius * max_radius) {
            pinned_.push_back(static_cast<std::uint32_t>(i));
            pinned_local_.push_back(
                XMFLOAT3(local[i].x - anchor.x, local[i].y - anchor.y, local[i].z - anchor.z));
            pinned_inv_mass_.push_back(readback_[i].w);
        }
    }
    if (pinned_.empty()) {
        // A degenerate shape (all mass outside the grip window) -- grab the lowest vertex
        // rather than carrying nothing at all.
        std::uint32_t lowest = 0;
        for (std::uint32_t i = 1; i < local.size(); ++i) {
            if (local[i].y < local[lowest].y) {
                lowest = i;
            }
        }
        pinned_.push_back(lowest);
        pinned_local_.push_back(XMFLOAT3(local[lowest].x - anchor.x, local[lowest].y - anchor.y,
                                         local[lowest].z - anchor.z));
        pinned_inv_mass_.push_back(readback_[lowest].w);
    }

    // Move the WHOLE body to the hand rigidly, stilled, and only then raise the grip.
    // Teleporting just the pinned patch and letting elasticity drag the rest in reads
    // like a nice springy yank on paper, but at this game's 4 solver iterations it is
    // taffy in practice: a patty grabbed from arm's reach stretched into a metre-long
    // ribbon and took seconds to recover. A rigid teleport writes no strain at all --
    // the shape arrives in the hand exactly as it lay -- and from here the carry only
    // ever strains it by one frame's worth of hand motion, which is the sag-and-swing
    // regime the solver is comfortable in.
    PxVec4* positions = static_cast<PxVec4*>(sim_positions_pinned_);
    PxVec4* velocities = static_cast<PxVec4*>(sim_velocities_pinned_);
    for (size_t i = 0; i < local.size(); ++i) {
        const XMVECTOR anchored = XMVectorSet(local[i].x - anchor.x, local[i].y - anchor.y,
                                              local[i].z - anchor.z, 1.0f);
        XMFLOAT3 world;
        XMStoreFloat3(&world, XMVector3Transform(anchored, to_world));
        positions[i] = PxVec4(world.x, world.y, world.z, readback_[i].w);
        velocities[i] = PxVec4(0.0f);
    }
    for (size_t p = 0; p < pinned_.size(); ++p) {
        positions[pinned_[p]].w = 0.0f;
    }

    // The collision mesh comes along for the ride. A deformable volume simulates TWO
    // vertex sets -- the simulation (voxel) mesh above and a finer collision mesh
    // coupled to it -- and teleporting only the first leaves the second behind, still
    // coupled: measured as the grabbed patty tethering itself to the tray and
    // stretching into a ribbon across the yard as the player walked. The constructor's
    // placement (PxDeformableVolumeExt::transform) moves every buffer for the same
    // reason. The teleport is the pure translation that carries the grip anchor to the
    // pose origin, so one vertex's before/after difference is the whole story; the
    // collision mesh's live positions are read back first, since the host mirror still
    // holds the cook-time state.
    PxVec4* collision_positions = static_cast<PxVec4*>(collision_positions_pinned_);
    const PxU32 collision_count = volume_->getCollisionMesh()->getNbVertices();
    {
        PxScopedCudaLock lock(*physics_->Cuda());
        physics_->Cuda()->getCudaContext()->memcpyDtoH(
            collision_positions,
            reinterpret_cast<CUdeviceptr>(volume_->getPositionInvMassBufferD()),
            collision_count * sizeof(PxVec4));
    }
    const PxVec4 delta(positions[0].x - readback_[0].x, positions[0].y - readback_[0].y,
                       positions[0].z - readback_[0].z, 0.0f);
    for (PxU32 j = 0; j < collision_count; ++j) {
        collision_positions[j] += delta;
    }

    PxDeformableVolumeExt::copyToDevice(
        *volume_,
        PxDeformableVolumeDataFlags(PxDeformableVolumeDataFlag::eSIM_POSITION_INVMASS |
                                    PxDeformableVolumeDataFlag::eSIM_VELOCITY |
                                    PxDeformableVolumeDataFlag::ePOSITION_INVMASS),
        positions, velocities, collision_positions,
        static_cast<PxVec4*>(rest_positions_pinned_));
    kinematic_ = true;
}

void SoftBody::UpdateKinematic(const XMFLOAT4X4& pose) {
    if (!kinematic_) {
        return;
    }
    // Refresh BOTH host mirrors from the live state -- the positions from the readback
    // Update just brought down, the velocities straight off the device -- then overwrite
    // only the pinned patch (positions to the carry pose, velocities to zero, inverse
    // mass zero). The velocity echo is not optional: uploading positions alone freezes
    // the free vertices solid, because the upload's dirty-processing resets the solver's
    // velocity state every frame -- one substep of gravity, then a reset, for ever
    // (measured: a held patty's free rim hovered within half a millimetre for three
    // seconds instead of sagging). Handing the solver back its own velocities makes the
    // reconcile an identity for everything but the grip, so gravity accumulates and the
    // free part genuinely dangles.
    PxVec4* positions = static_cast<PxVec4*>(sim_positions_pinned_);
    PxVec4* velocities = static_cast<PxVec4*>(sim_velocities_pinned_);
    {
        PxScopedCudaLock lock(*physics_->Cuda());
        physics_->Cuda()->getCudaContext()->memcpyDtoH(
            velocities, reinterpret_cast<CUdeviceptr>(volume_->getSimVelocityBufferD()),
            readback_.size() * sizeof(PxVec4));
    }
    std::memcpy(positions, readback_.data(), readback_.size() * sizeof(PxVec4));
    const XMMATRIX to_world = XMLoadFloat4x4(&pose);
    for (size_t p = 0; p < pinned_.size(); ++p) {
        XMFLOAT3 world;
        XMStoreFloat3(&world, XMVector3Transform(XMLoadFloat3(&pinned_local_[p]), to_world));
        positions[pinned_[p]] = PxVec4(world.x, world.y, world.z, 0.0f);
        velocities[pinned_[p]] = PxVec4(0.0f);
    }
    // A gripped body must never sleep: its pinned vertices sit at zero velocity, which
    // is indistinguishable from settling, and a sleeping volume ignores the solver
    // entirely -- elasticity, gravity, everything. (The meats doze off on the tray long
    // before the first grab, which is exactly how this was found: every buffer upload
    // above landed in a body the solver was skipping, and the "carry" was this code's
    // own writes echoing back through the readback.)
    volume_->setWakeCounter(1.0f);
    // Safe here only because Physics::Step runs simulate/fetchResults synchronously and
    // has already returned by the time Props drives the carry: these buffers must not be
    // written while the solver is running.
    PxDeformableVolumeExt::copyToDevice(
        *volume_,
        PxDeformableVolumeDataFlags(PxDeformableVolumeDataFlag::eSIM_POSITION_INVMASS |
                                    PxDeformableVolumeDataFlag::eSIM_VELOCITY),
        positions, velocities, static_cast<PxVec4*>(collision_positions_pinned_),
        static_cast<PxVec4*>(rest_positions_pinned_));
}

void SoftBody::EndKinematic(const XMFLOAT3& velocity) {
    if (volume_ == nullptr || !kinematic_) {
        return;
    }

    // Put the grip's inverse masses back and send the body off with the release
    // velocity -- the hand's toss, or zero for a placed drop. The free vertices keep
    // the swing they had on top of it: their live velocities are read back first, so
    // the whole-buffer upload does not zero momentum the solver still owns, and the
    // toss is added rather than assigned. The formerly pinned vertices get the toss
    // exactly -- their buffered velocities are stale, since a zero-inverse-mass vertex
    // is never integrated. Positions ride along refreshed from the readback with the
    // restored masses in their w.
    PxVec4* positions = static_cast<PxVec4*>(sim_positions_pinned_);
    PxVec4* velocities = static_cast<PxVec4*>(sim_velocities_pinned_);
    {
        PxScopedCudaLock lock(*physics_->Cuda());
        physics_->Cuda()->getCudaContext()->memcpyDtoH(
            velocities, reinterpret_cast<CUdeviceptr>(volume_->getSimVelocityBufferD()),
            readback_.size() * sizeof(PxVec4));
    }
    std::memcpy(positions, readback_.data(), readback_.size() * sizeof(PxVec4));
    const PxVec4 toss(velocity.x, velocity.y, velocity.z, 0.0f);
    for (size_t i = 0; i < readback_.size(); ++i) {
        velocities[i] += toss;
    }
    for (size_t p = 0; p < pinned_.size(); ++p) {
        // The readback saw the pinned vertices with the zeroed mass the grip gave them;
        // the recorded original goes back in its place.
        positions[pinned_[p]].w = pinned_inv_mass_[p];
        velocities[pinned_[p]] = toss;
    }
    PxDeformableVolumeExt::copyToDevice(
        *volume_,
        PxDeformableVolumeDataFlags(PxDeformableVolumeDataFlag::eSIM_POSITION_INVMASS |
                                    PxDeformableVolumeDataFlag::eSIM_VELOCITY),
        positions, velocities, static_cast<PxVec4*>(collision_positions_pinned_),
        static_cast<PxVec4*>(rest_positions_pinned_));
    // Awake on release, or a meat dropped from a sleeping grip would hang in the air
    // exactly as it hung in the hand.
    volume_->setWakeCounter(1.0f);
    kinematic_ = false;
}

void SoftBody::Park() {
    if (volume_ == nullptr || parked_) {
        return;
    }
    physics_->Scene().removeActor(*volume_);
    parked_ = true;
}

void SoftBody::Unpark(const XMFLOAT4X4& pose) {
    if (volume_ == nullptr || !parked_) {
        return;
    }

    // The shape comes from the host mirrors, not the readback: a parked-since-birth
    // twin has never been stepped, so its readback is empty while the mirrors hold the
    // placed rest state the constructor staged. (A body parked mid-life would return in
    // whatever shape its mirrors last held -- fine, since doneness is monotonic and
    // nothing swaps back today.) The same bottom-centre-anchor arithmetic as the grip:
    // the anchor is where the model origin sits, so planting it at the pose origin puts
    // the swap-in exactly where the swap-out stood.
    PxVec4* positions = static_cast<PxVec4*>(sim_positions_pinned_);
    PxVec4* velocities = static_cast<PxVec4*>(sim_velocities_pinned_);
    const size_t count = readback_.size();
    const XMMATRIX to_world = XMLoadFloat4x4(&pose);
    XMVECTOR determinant;
    const XMMATRIX to_local = XMMatrixInverse(&determinant, to_world);
    std::vector<XMFLOAT3> local(count);
    XMFLOAT3 lo{FLT_MAX, FLT_MAX, FLT_MAX};
    XMFLOAT3 hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < count; ++i) {
        const XMVECTOR world = XMVectorSet(positions[i].x, positions[i].y, positions[i].z, 1.0f);
        XMStoreFloat3(&local[i], XMVector3Transform(world, to_local));
        lo = {std::min(lo.x, local[i].x), std::min(lo.y, local[i].y), std::min(lo.z, local[i].z)};
        hi = {std::max(hi.x, local[i].x), std::max(hi.y, local[i].y), std::max(hi.z, local[i].z)};
    }
    const XMFLOAT3 anchor{(lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f};
    const PxVec4 before = positions[0];
    for (size_t i = 0; i < count; ++i) {
        const XMVECTOR anchored = XMVectorSet(local[i].x - anchor.x, local[i].y - anchor.y,
                                              local[i].z - anchor.z, 1.0f);
        XMFLOAT3 world;
        XMStoreFloat3(&world, XMVector3Transform(anchored, to_world));
        positions[i] = PxVec4(world.x, world.y, world.z, positions[i].w);
        velocities[i] = PxVec4(0.0f);
    }

    // The collision mesh rides along by the same translation, exactly as in
    // BeginKinematic -- its mirror still holds the constructor's placement, which is
    // the same placement the sim mirror held, so the one delta moves both coherently.
    PxVec4* collision_positions = static_cast<PxVec4*>(collision_positions_pinned_);
    const PxU32 collision_count = volume_->getCollisionMesh()->getNbVertices();
    const PxVec4 delta = positions[0] - before;
    for (PxU32 j = 0; j < collision_count; ++j) {
        collision_positions[j] += PxVec4(delta.x, delta.y, delta.z, 0.0f);
    }

    PxDeformableVolumeExt::copyToDevice(
        *volume_,
        PxDeformableVolumeDataFlags(PxDeformableVolumeDataFlag::eSIM_POSITION_INVMASS |
                                    PxDeformableVolumeDataFlag::eSIM_VELOCITY |
                                    PxDeformableVolumeDataFlag::ePOSITION_INVMASS),
        positions, velocities, collision_positions, static_cast<PxVec4*>(rest_positions_pinned_));

    physics_->Scene().addActor(*volume_);
    parked_ = false;
    // Awake on arrival: an unparked body must settle onto whatever it was planted
    // over, not hang where it was placed the way the sleeping grip once did.
    volume_->setWakeCounter(1.0f);
}

void SoftBody::Skin() {
    if (skinned_.empty()) {
        return;
    }

    // Each drawn vertex is its four tet corners, blended by the weights the embedding
    // fixed at rest. The corners are already in world space, so this lands in world
    // space -- the deformation and the body's placement in one step.
    const size_t tet_count = sim_tetrahedra_.size() / 4;
    for (size_t i = 0; i < skinned_.size(); ++i) {
        const std::uint32_t tet = embed_tet_[i];
        if (tet >= tet_count) {
            continue; // Unembedded: left at its rest position (see below).
        }
        const XMFLOAT4& w = embed_weights_[i];
        const std::uint32_t* corner = &sim_tetrahedra_[tet * 4];
        XMVECTOR position = XMVectorScale(XMLoadFloat3(&sim_positions_[corner[0]]), w.x);
        position = XMVectorAdd(position,
                               XMVectorScale(XMLoadFloat3(&sim_positions_[corner[1]]), w.y));
        position = XMVectorAdd(position,
                               XMVectorScale(XMLoadFloat3(&sim_positions_[corner[2]]), w.z));
        position = XMVectorAdd(position,
                               XMVectorScale(XMLoadFloat3(&sim_positions_[corner[3]]), w.w));
        XMStoreFloat3(&skinned_[i].position, position);
    }

    // Normals have to be rebuilt from the deformed surface -- the rest normals describe a
    // shape that no longer exists, and a squashed patty lit by its unsquashed normals
    // reads as a flat decal. Area-weighted, which falls out of using the raw cross
    // product rather than a normalized one: a big triangle should have more say in a
    // shared vertex's normal than a sliver does.
    //
    // The tangents are deliberately left at rest. They matter far less (they only orient
    // the normal map's tangent space, and the deformations here are gentle), and
    // rebuilding them properly needs the texture coordinates as well as the positions --
    // more work per frame than the difference would show.
    for (Vertex& vertex : skinned_) {
        vertex.normal = XMFLOAT3(0.0f, 0.0f, 0.0f);
    }
    for (size_t i = 0; i + 2 < skinned_indices_.size(); i += 3) {
        const std::uint32_t i0 = skinned_indices_[i];
        const std::uint32_t i1 = skinned_indices_[i + 1];
        const std::uint32_t i2 = skinned_indices_[i + 2];
        const XMVECTOR p0 = XMLoadFloat3(&skinned_[i0].position);
        const XMVECTOR p1 = XMLoadFloat3(&skinned_[i1].position);
        const XMVECTOR p2 = XMLoadFloat3(&skinned_[i2].position);
        const XMVECTOR face = XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0));
        for (const std::uint32_t index : {i0, i1, i2}) {
            XMStoreFloat3(&skinned_[index].normal,
                          XMVectorAdd(XMLoadFloat3(&skinned_[index].normal), face));
        }
    }
    for (Vertex& vertex : skinned_) {
        const XMVECTOR normal = XMLoadFloat3(&vertex.normal);
        // A vertex no triangle reached, or one whose triangles cancelled out, would
        // normalize to a NaN and paint the whole surface black.
        if (XMVectorGetX(XMVector3LengthSq(normal)) > 1e-20f) {
            XMStoreFloat3(&vertex.normal, XMVector3Normalize(normal));
        } else {
            vertex.normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
        }
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
