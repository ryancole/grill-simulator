#include "furniture.hpp"

#include "actions.hpp"
#include "physics.hpp"
#include "scene.hpp"

#include <PxPhysicsAPI.h>

#include <algorithm>

// Furniture now pulls in <PxPhysicsAPI.h>: righting the grill sweeps the scene along
// the gaze and rewrites the base body's pose directly, so it touches PhysX itself.
// Everything else still holds each actor as an opaque handle through its RigidBody.

using namespace DirectX;
using namespace physx;

namespace {

// How far the player can reach to right the grill, and the radius of the sphere swept
// down the gaze to find what they are aiming at -- the same forgiving aim the prop
// pick-up uses, so a large toppled grill need not be pixel-centred.
constexpr float kReach = 2.5f;
constexpr float kPickRadius = 0.25f;

// How close to vertical the grill base's up axis must stay to count as standing: the
// cosine of the tip angle allowed before it reads as knocked over. 0.9 is about 26
// degrees, so a grill merely nudged still stands while a toppled one offers righting.
constexpr float kUprightCos = 0.9f;

// A rotation-and-translation model-to-world matrix as a PhysX rigid transform, for
// dropping the grill base back onto its authored pose. The furniture never scales, so
// the rotation reads straight back out as a quaternion.
PxTransform ToPxTransform(FXMMATRIX m) {
    XMFLOAT3 p;
    XMFLOAT4 q;
    XMStoreFloat3(&p, m.r[3]);
    XMStoreFloat4(&q, XMQuaternionRotationMatrix(m));
    return PxTransform(PxVec3(p.x, p.y, p.z), PxQuat(q.x, q.y, q.z, q.w));
}

// A scene-query filter that keeps only the furniture bodies. Every shovable body
// carries a BodyTag in userData; a carryable prop's has prop_index >= 0, while the
// furniture (grill base, cooler) tags itself with prop_index -1, and the static world
// and the player's own capsule -- which the gaze sweep starts inside -- leave userData
// null. Keeping just the furniture lets the righting sweep find the grill base without
// a prop or the eye's own body shadowing it; the caller checks the nearest hit is the
// base and not the cooler.
struct FurnitureQueryFilter : PxQueryFilterCallback {
    PxQueryHitType::Enum preFilter(const PxFilterData&, const PxShape*, const PxRigidActor* actor,
                                   PxHitFlags&) override {
        const auto* tag = static_cast<const BodyTag*>(actor->userData);
        return (tag != nullptr && tag->prop_index < 0) ? PxQueryHitType::eBLOCK
                                                        : PxQueryHitType::eNONE;
    }
    PxQueryHitType::Enum postFilter(const PxFilterData&, const PxQueryHit&, const PxShape*,
                                    const PxRigidActor*) override {
        return PxQueryHitType::eBLOCK; // Never invoked: ePOSTFILTER is not requested.
    }
};

} // namespace

Furniture::Furniture(Scene& scene, Physics& physics) : scene_(&scene), physics_(&physics) {
    // Reserve so no push_back reallocates: each body's userData points at the tag
    // inside its RigidBody, and that address has to stay put for the session.
    bodies_.reserve(scene.DynamicBodies().size());
    for (const DynamicBody& body : scene.DynamicBodies()) {
        physx::PxRigidDynamic* actor =
            physics.AddDynamicBody(body.shapes, body.initial_transform, body.mass);
        const std::size_t index = bodies_.size();
        Body& stored = bodies_.emplace_back();
        stored.instance = body.instance;
        // prop_index stays -1: furniture is shovable but never carried, so the
        // gaze-pick sweep skips it. The rating rides along for the shove, and the
        // impact sound so the contact report can voice a hard landing (the grill
        // clatters). Bind only now that `stored` sits in its final slot in bodies_.
        stored.body.Adopt(actor, body.knock_rating, -1, body.impact_sound);
        stored.body.Bind();

        // The grill base is the one body the player can stand back up. Remember which it
        // is (tagged by its clatter sound) and the authored pose it spawned at, so a later
        // righting drops it back exactly here rather than merely somewhere upright.
        if (body.impact_sound == ImpactSound::GrillBase) {
            grill_base_ = static_cast<int>(index);
            grill_base_upright_ = body.initial_transform;
        }

        // A body that radiates heat contributes a HeatSource, paired with the index
        // of the body whose pose moves it and the grate offset in that body's space.
        // Update places the origin every frame; it is left unset until then. Its ignition
        // requirements ride a parallel slot -- present on the grill's grate (which starts
        // cold and is lit in play), empty on a source authored already-on -- so
        // UpdateIgnition can warm it toward a held flame and switch it on when it catches.
        if (body.heat) {
            heat_sources_.push_back(*body.heat);
            heat_drives_.push_back({index, body.heat_offset});
            heat_ignitables_.push_back(body.ignitable);
        }
    }
}

void Furniture::Update() {
    // Read each body's stepped pose back into its draw instance; the renderer (and
    // the sun's shadow) then follow the tumble for free.
    for (const Body& stored : bodies_) {
        scene_->SetInstanceTransform(stored.instance, stored.body.Transform());
    }

    // Move each heat source to where its body now sits: the grate offset, in the
    // body's model space, carried through the body's current pose. Knock the grill
    // over and its hot zone tips with it -- the heat is wherever the grate is.
    for (std::size_t i = 0; i < heat_sources_.size(); ++i) {
        const HeatDrive& drive = heat_drives_[i];
        const DirectX::XMMATRIX pose = bodies_[drive.body].body.Transform();
        const DirectX::XMVECTOR origin =
            DirectX::XMVector3Transform(DirectX::XMLoadFloat3(&drive.local_offset), pose);
        heat_sources_[i].SetOrigin(origin);
    }
}

void Furniture::UpdateIgnition(std::span<const HeatSource> external_heats, float dt) {
    // Light any grate the player is holding a flame to. Mirrors Props' log ignition: warm
    // the thing toward the air around it and switch its heat on once its own temperature
    // has climbed past the threshold, so a flame has to be held on it a moment and one
    // taken away too soon lets it cool back. The air here is the hottest *carryable* flame
    // reaching the grate -- the lit lighter, a burning log -- and nothing else: a grate
    // never lights itself, and there is no other furniture heat in the yard to light it.
    for (std::size_t i = 0; i < heat_sources_.size(); ++i) {
        std::optional<IgnitableRequirements>& ignitable = heat_ignitables_[i];
        if (!ignitable || heat_sources_[i].IsOn()) {
            continue;
        }
        // The hot centre Update placed this frame is the point that must get hot -- the
        // same point that will radiate once it catches, which is the fair place to ask.
        const XMFLOAT3 hot_centre = heat_sources_[i].Origin();
        const XMVECTOR origin = XMLoadFloat3(&hot_centre);
        float ambient_f = CookInformation::kRoomTempF;
        for (const HeatSource& flame : external_heats) {
            ambient_f = std::max(ambient_f, flame.TemperatureAt(origin));
        }
        ignitable->Update(ambient_f, dt);
        if (ignitable->Ignited()) {
            heat_sources_[i].SetOn(true);
        }
    }
}

bool Furniture::GrillToppled() const {
    // The base's local up axis (row 1 of its model-to-world pose) against world up: a
    // standing grill has them near-aligned (dot ~1), a toppled one splayed sideways or
    // down (dot below the threshold).
    const XMVECTOR up = XMVector3Normalize(bodies_[grill_base_].body.Transform().r[1]);
    const float upright = XMVectorGetX(XMVector3Dot(up, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    return upright < kUprightCos;
}

bool Furniture::AimingAtGrill(FXMVECTOR eye, FXMVECTOR forward) const {
    XMFLOAT3 origin;
    XMFLOAT3 direction;
    XMStoreFloat3(&origin, eye);
    XMStoreFloat3(&direction, forward);

    // Sweep the aim sphere down the gaze over furniture bodies only, and take the nearest.
    // The static world is not dynamic, so eDYNAMIC drops it; the filter drops the props and
    // the player's own capsule. A hit that is the grill base means it is what the eye rests
    // on -- the cooler, if it is nearer along the gaze, blocks first and is rejected here.
    FurnitureQueryFilter filter;
    const PxQueryFilterData filter_data(PxQueryFlag::eDYNAMIC | PxQueryFlag::ePREFILTER);
    PxSweepBuffer hit;
    const bool blocked = physics_->Scene().sweep(
        PxSphereGeometry(kPickRadius), PxTransform(PxVec3(origin.x, origin.y, origin.z)),
        PxVec3(direction.x, direction.y, direction.z), kReach, hit, PxHitFlag::eDEFAULT,
        filter_data, &filter);
    return blocked && hit.block.actor == bodies_[grill_base_].body.actor();
}

void Furniture::RightGrill() {
    // Drop the base back onto its authored pose, dead still, and wake it so the solver
    // settles it against the ground from there. Update reads this pose into the draw
    // instance and moves the grate heat with it later this frame.
    PxRigidDynamic* actor = bodies_[grill_base_].body.actor();
    actor->setGlobalPose(ToPxTransform(XMLoadFloat4x4(&grill_base_upright_)));
    actor->setLinearVelocity(PxVec3(0.0f, 0.0f, 0.0f));
    actor->setAngularVelocity(PxVec3(0.0f, 0.0f, 0.0f));
    actor->wakeUp();
}

void Furniture::Interact(const XMMATRIX& camera_to_world, const Actions& actions, bool hands_free) {
    right_target_ = -1;
    highlight_.clear();

    // Nothing to right on a level with no grill base, while the hands are busy (E is then
    // the carried item's drop, so the grill offers nothing), or while the grill stands.
    if (grill_base_ < 0 || !hands_free || !GrillToppled()) {
        return;
    }

    // The camera-to-world matrix is right, up, forward, eye as its four rows.
    const XMVECTOR eye = camera_to_world.r[3];
    const XMVECTOR forward = XMVector3Normalize(camera_to_world.r[2]);
    if (!AimingAtGrill(eye, forward)) {
        return;
    }

    right_target_ = grill_base_;

    // Ring the toppled base with the prop pick-up outline at its current pose, so the same
    // glow that marks a grabbable prop marks the grill an E press would stand up. The model
    // id is the base's draw instance; the transform is the body's live pose (which this
    // frame's Update has not yet written back into the instance).
    const Body& base = bodies_[grill_base_];
    MeshInstance instance{};
    instance.model = scene_->Instances()[base.instance].model;
    XMStoreFloat4x4(&instance.transform, base.body.Transform());
    instance.tint = XMFLOAT3{1.0f, 1.0f, 1.0f};
    instance.checker = 0.0f;
    highlight_.push_back(instance);

    // Edge-triggered like the pick-up: one right per press. Props reads the same Interact
    // press, but its gaze sweep skips furniture, so a free-handed E aimed at the grill
    // rights it here and grabs nothing there.
    if (actions.WasPressed(Action::Interact)) {
        RightGrill();
    }
}

std::string Furniture::PromptText() const {
    return right_target_ >= 0 ? "[E] Right the grill" : std::string{};
}
