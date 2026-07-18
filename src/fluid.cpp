#include "fluid.hpp"

#include "physics.hpp"

#include <PxPhysicsAPI.h>
#include <cudamanager/PxCudaContext.h>
#include <cudamanager/PxCudaContextManager.h>

using namespace physx;
using namespace DirectX;

namespace {

// The pool. Fixed and always fully active (idle slots are parked, not removed),
// so the per-frame traffic is two small memcpys, never an allocation. At the
// spray's emission rate and lifetime the pool holds roughly twice the droplets a
// continuous hold can have in flight.
constexpr int kMaxDroplets = 2048;

// How long a droplet lives from nozzle to retirement, seconds. Long enough to
// arc, land and pool visibly; short enough that a sprayed puddle drains away
// rather than accumulating forever.
constexpr float kLifetimeSeconds = 4.0f;

// One droplet weighs five grams (inverse mass, the form PhysX stores). Light
// enough that a stream splashing off the grill nudges nothing.
constexpr float kInvMass = 1.0f / 0.005f;

// The particle sizing, all in metres and mutually constrained (contact >= rest;
// particle contact offset > fluid rest offset). Fluid neighbours settle about
// two fluid-rest-offsets apart, so these make a stream of centimetre droplets.
constexpr float kParticleContactOffset = 0.03f;
constexpr float kRestOffset = 0.02f;
constexpr float kContactOffset = 0.03f;
constexpr float kSolidRestOffset = 0.02f;
constexpr float kFluidRestOffset = 0.018f;

// The PBD material: low friction (it is a liquid on dirt), a little viscosity
// and cohesion so the stream strings and beads like fluid instead of scattering
// like sand, a touch of surface tension for the beading.
constexpr float kFriction = 0.05f;
constexpr float kDamping = 0.05f;
constexpr float kAdhesion = 0.0f;
constexpr float kViscosity = 5.0f;
constexpr float kVorticityConfinement = 0.0f;
constexpr float kSurfaceTension = 0.005f;
constexpr float kCohesion = 5.0f;
constexpr float kLift = 0.0f;
constexpr float kDrag = 0.0f;

// A sanity ceiling so no solver hiccup ever launches a droplet across the map.
constexpr float kMaxVelocity = 50.0f;

// The spray cone's half-angle, expressed as the sideways speed fraction mixed
// into the nozzle direction -- about four degrees of fan.
constexpr float kConeJitter = 0.07f;

// Where the parked block sits: far under every level, spread on a grid much
// coarser than the contact offset so parked neighbours never see each other.
constexpr float kParkY = -120.0f;
constexpr float kParkSpacing = 0.5f;
constexpr int kParkColumns = 64;

// How a droplet draws: a cube of this side, tinted the pale straw of naphtha.
// The cube model carries no material, so the tint is its entire colour.
constexpr float kDropletSide = 0.035f;
constexpr XMFLOAT3 kDropletTint{0.93f, 0.9f, 0.72f};

XMFLOAT3 ParkSpot(int slot) {
    const int col = slot % kParkColumns;
    const int row = slot / kParkColumns;
    return XMFLOAT3(static_cast<float>(col) * kParkSpacing, kParkY,
                    static_cast<float>(row) * kParkSpacing);
}

} // namespace

Fluid::Fluid(Physics& physics) : physics_(&physics) {
    // No CUDA context, no fluid: PhysX 5 has no CPU particle path. Every member
    // stays null/empty and the public surface degrades to no-ops.
    if (!physics.GpuActive()) {
        return;
    }

    PxPhysics& sdk = physics.Sdk();
    material_ = sdk.createPBDMaterial(kFriction, kDamping, kAdhesion, kViscosity,
                                      kVorticityConfinement, kSurfaceTension, kCohesion, kLift,
                                      kDrag);

    system_ = sdk.createPBDParticleSystem(*physics.Cuda());
    // Sizing before the system enters the scene, so the solver's grids are built
    // for these radii from the first step.
    system_->setRestOffset(kRestOffset);
    system_->setContactOffset(kContactOffset);
    system_->setParticleContactOffset(kParticleContactOffset);
    system_->setSolidRestOffset(kSolidRestOffset);
    system_->setFluidRestOffset(kFluidRestOffset);
    system_->setMaxVelocity(kMaxVelocity);
    const PxU32 phase = system_->createPhase(
        material_, PxParticlePhaseFlags(PxParticlePhaseFlag::eParticlePhaseFluid |
                                        PxParticlePhaseFlag::eParticlePhaseSelfCollide));
    physics.Scene().addActor(*system_);

    // The whole pool starts parked. The buffer's device arrays are filled once
    // here, before it is handed to the system, which adopts the data as-is; the
    // phases never change again, so only positions and velocities travel later.
    pos_inv_mass_.resize(kMaxDroplets);
    velocity_.resize(kMaxDroplets, XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f));
    age_.resize(kMaxDroplets, 0.0f);
    for (int i = 0; i < kMaxDroplets; ++i) {
        const XMFLOAT3 park = ParkSpot(i);
        pos_inv_mass_[i] = XMFLOAT4(park.x, park.y, park.z, 0.0f);
    }
    std::vector<PxU32> phases(kMaxDroplets, phase);

    buffer_ = sdk.createParticleBuffer(kMaxDroplets, /*maxVolumes=*/1, physics.Cuda());
    {
        PxScopedCudaLock lock(*physics.Cuda());
        PxCudaContext* ctx = physics.Cuda()->getCudaContext();
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getPositionInvMasses()),
                        pos_inv_mass_.data(), sizeof(PxVec4) * kMaxDroplets);
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getVelocities()),
                        velocity_.data(), sizeof(PxVec4) * kMaxDroplets);
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getPhases()), phases.data(),
                        sizeof(PxU32) * kMaxDroplets);
    }
    buffer_->setNbActiveParticles(kMaxDroplets);
    system_->addParticleBuffer(buffer_);
}

Fluid::~Fluid() {
    if (system_ == nullptr) {
        return;
    }
    // Reverse of construction: detach the buffer, pull the system from the scene,
    // then release. Physics outlives this object (declared before it in Game), so
    // the scene and SDK are still valid here.
    system_->removeParticleBuffer(buffer_);
    physics_->Scene().removeActor(*system_);
    buffer_->release();
    system_->release();
    material_->release();
}

void Fluid::Spray(XMFLOAT3 origin, XMFLOAT3 direction, float speed, int count) {
    if (system_ == nullptr) {
        return;
    }
    std::uniform_real_distribution<float> jitter(-1.0f, 1.0f);
    const XMVECTOR dir = XMVector3Normalize(XMLoadFloat3(&direction));
    // Any two axes perpendicular to the nozzle direction frame the jitter cone.
    const XMVECTOR seed = std::abs(XMVectorGetY(dir)) > 0.9f ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
                                                             : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR side = XMVector3Normalize(XMVector3Cross(seed, dir));
    const XMVECTOR up = XMVector3Cross(dir, side);
    for (int i = 0; i < count; ++i) {
        const XMVECTOR fan = XMVectorAdd(
            dir, XMVectorAdd(XMVectorScale(side, jitter(rng_) * kConeJitter),
                             XMVectorScale(up, jitter(rng_) * kConeJitter)));
        Pending drop;
        drop.position = origin;
        XMStoreFloat3(&drop.velocity, XMVectorScale(XMVector3Normalize(fan), speed));
        pending_.push_back(drop);
    }
}

void Fluid::Park(int slot) {
    const XMFLOAT3 park = ParkSpot(slot);
    pos_inv_mass_[slot] = XMFLOAT4(park.x, park.y, park.z, 0.0f);
    velocity_[slot] = XMFLOAT4(0.0f, 0.0f, 0.0f, 0.0f);
    age_[slot] = 0.0f;
}

void Fluid::Update(float dt) {
    if (system_ == nullptr) {
        return;
    }

    // Pull this step's simulated state down. Both arrays come back so the write-up
    // below (which uploads the whole mirror) never clobbers live sim state with a
    // stale copy. 2048 particles are 32 KB an array; this is nothing.
    PxCudaContext* ctx = physics_->Cuda()->getCudaContext();
    {
        PxScopedCudaLock lock(*physics_->Cuda());
        ctx->memcpyDtoH(pos_inv_mass_.data(),
                        reinterpret_cast<CUdeviceptr>(buffer_->getPositionInvMasses()),
                        sizeof(PxVec4) * kMaxDroplets);
        ctx->memcpyDtoH(velocity_.data(), reinterpret_cast<CUdeviceptr>(buffer_->getVelocities()),
                        sizeof(PxVec4) * kMaxDroplets);
    }

    // Retire droplets past their lifetime back to the parking grid.
    bool dirty = false;
    for (int i = 0; i < kMaxDroplets; ++i) {
        if (age_[i] <= 0.0f) {
            continue;
        }
        age_[i] -= dt;
        if (age_[i] <= 0.0f) {
            Park(i);
            dirty = true;
        }
    }

    // Give the queued sprays their slots, oldest-recycled-first. A queued droplet
    // becomes simulated on the next physics step; it is in this frame's mirrors
    // already, so it draws at the nozzle immediately.
    for (const Pending& drop : pending_) {
        const int slot = cursor_;
        cursor_ = (cursor_ + 1) % kMaxDroplets;
        pos_inv_mass_[slot] = XMFLOAT4(drop.position.x, drop.position.y, drop.position.z, kInvMass);
        velocity_[slot] = XMFLOAT4(drop.velocity.x, drop.velocity.y, drop.velocity.z, 0.0f);
        age_[slot] = kLifetimeSeconds;
        dirty = true;
    }
    pending_.clear();

    if (dirty) {
        PxScopedCudaLock lock(*physics_->Cuda());
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getPositionInvMasses()),
                        pos_inv_mass_.data(), sizeof(PxVec4) * kMaxDroplets);
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getVelocities()),
                        velocity_.data(), sizeof(PxVec4) * kMaxDroplets);
        buffer_->raiseFlags(PxParticleBufferFlag::Enum(PxParticleBufferFlag::eUPDATE_POSITION |
                                                       PxParticleBufferFlag::eUPDATE_VELOCITY));
    }

    // Rebuild what the rest of the game sees: the live droplets' draw instances. A
    // handful of hundreds at most, remade flat each frame like the props' draw lists.
    instances_.clear();
    for (int i = 0; i < kMaxDroplets; ++i) {
        if (age_[i] <= 0.0f) {
            continue;
        }
        const XMFLOAT4& p = pos_inv_mass_[i];

        MeshInstance instance{};
        instance.model = Scene::kCubeModel;
        XMStoreFloat4x4(&instance.transform,
                        XMMatrixScaling(kDropletSide, kDropletSide, kDropletSide) *
                            XMMatrixTranslation(p.x, p.y, p.z));
        instance.tint = kDropletTint;
        instance.checker = 0.0f;
        instances_.push_back(instance);
    }
}

void Fluid::Clear() {
    if (system_ == nullptr) {
        return;
    }
    for (int i = 0; i < kMaxDroplets; ++i) {
        Park(i);
    }
    pending_.clear();
    instances_.clear();
    {
        PxScopedCudaLock lock(*physics_->Cuda());
        PxCudaContext* ctx = physics_->Cuda()->getCudaContext();
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getPositionInvMasses()),
                        pos_inv_mass_.data(), sizeof(PxVec4) * kMaxDroplets);
        ctx->memcpyHtoD(reinterpret_cast<CUdeviceptr>(buffer_->getVelocities()),
                        velocity_.data(), sizeof(PxVec4) * kMaxDroplets);
    }
    buffer_->raiseFlags(PxParticleBufferFlag::Enum(PxParticleBufferFlag::eUPDATE_POSITION |
                                                   PxParticleBufferFlag::eUPDATE_VELOCITY));
}
