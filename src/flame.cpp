#include "flame.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {

// How many specks a second a held flame emits, and how long each burns. Together
// these set how many are alive at once (rate * lifetime, about twenty), which is what
// decides whether the tongue reads as a solid body of fire or as a spatter of dots.
constexpr float kParticlesPerSecond = 170.0f;
constexpr float kLifetime = 0.18f;
constexpr float kLifetimeJitter = 0.05f; // +/- seconds, so they do not die in lockstep.

// A speck leaves the nozzle rising, and keeps accelerating upward: hot gas is buoyant,
// and the climb is what stretches the cloud into a tongue rather than a ball. The
// sideways jitter is small -- enough to give the flame a wobbling edge, not enough to
// spray it -- and the spawn scatter is tighter still, so the base stays pinched at the
// nozzle the way a real flame is.
// Together with the lifetime these decide how tall the tongue stands (about eight
// centimetres, which is a lighter's flame rather than a campfire's): a speck covers
// rise*life plus the buoyant half of the curve before it dies.
constexpr float kRiseSpeed = 0.34f;     // m/s upward at birth.
constexpr float kBuoyancy = 0.9f;       // m/s^2 of continued climb.
constexpr float kDriftSpeed = 0.03f;    // m/s of sideways wander.
constexpr float kSpawnScatter = 0.005f; // metres around the nozzle.

// The cube each speck draws as: biggest at birth and shrinking to nothing, so the
// tongue tapers to a point as it climbs instead of ending in a wall of full-size cubes.
// Small enough that a speck reads as part of a body of fire and not as its own cube --
// which is the whole difference between a flame and a shower of embers.
constexpr float kSideAtBirth = 0.018f;
constexpr float kSideAtDeath = 0.002f;

// The colour a speck burns through: a white-hot yellow at the base, cooling to a deep
// red as it rises and dies. These are the cubes' entire colour -- the unit cube carries
// no material of its own.
constexpr XMFLOAT3 kHotTint{1.0f, 0.83f, 0.42f};
constexpr XMFLOAT3 kCoolTint{0.85f, 0.16f, 0.03f};

// How hard a speck is self-lit. Above one it pushes past the tonemap's shoulder and
// into the bloom's bright-pass, which is what makes the flame glow and cast a haze
// rather than sit there as a flat orange dot. It falls off faster than the colour does
// (the curve below squares it), so a dying speck dims to an ember before it vanishes.
constexpr float kEmissiveAtBirth = 9.0f;
constexpr float kEmissiveAtDeath = 0.6f;

// A cap on the live particles across every fire at once, so a flame held for a whole level
// -- or a fire pit of several burning logs, each a scale-3 fire feeding the same pool --
// cannot grow the vector without bound if a frame ever runs long enough to bank a huge
// emission. Generous enough that a full pit does not visibly starve.
constexpr int kMaxParticles = 1024;

float Range(std::minstd_rand& rng, float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng);
}

} // namespace

Flame::Flame() {
    particles_.reserve(kMaxParticles);
    instances_.reserve(kMaxParticles);
}

void Flame::Emit(XMFLOAT3 origin, float dt, float scale) {
    // A bigger fire emits proportionally more specks to fill its bigger volume, so it does
    // not read as the same sparse handful spread thinner. burn_carry_ banks the fraction a
    // frame could not emit whole; it is shared across every fire, which only shuffles that
    // sub-one remainder between them and never loses or invents a speck.
    burn_carry_ += kParticlesPerSecond * scale * dt;
    const int count = static_cast<int>(burn_carry_);
    if (count <= 0) {
        return;
    }
    burn_carry_ -= static_cast<float>(count);

    const float scatter = kSpawnScatter * scale;
    for (int i = 0; i < count; ++i) {
        if (static_cast<int>(particles_.size()) >= kMaxParticles) {
            break;
        }
        Particle particle{};
        particle.position = XMFLOAT3(origin.x + Range(rng_, -scatter, scatter),
                                     origin.y + Range(rng_, -scatter, scatter),
                                     origin.z + Range(rng_, -scatter, scatter));
        // Rise and drift are scaled at birth; the continued buoyant climb is scaled in
        // Update off `scale`. Together they lift a bigger fire proportionally higher.
        particle.velocity = XMFLOAT3(Range(rng_, -kDriftSpeed, kDriftSpeed) * scale,
                                     kRiseSpeed * scale,
                                     Range(rng_, -kDriftSpeed, kDriftSpeed) * scale);
        particle.lifetime = kLifetime + Range(rng_, -kLifetimeJitter, kLifetimeJitter);
        particle.spin = Range(rng_, 0.0f, XM_2PI);
        particle.scale = scale;
        particles_.push_back(particle);
    }
}

void Flame::Update(float dt) {
    // Age every speck, and drop the ones that have burned out. Erasing as we go would
    // shuffle the tail down repeatedly; partitioning the dead to the end and truncating
    // once is the same result in one pass.
    for (Particle& particle : particles_) {
        particle.age += dt;
        particle.velocity.y += kBuoyancy * particle.scale * dt;
        particle.position.x += particle.velocity.x * dt;
        particle.position.y += particle.velocity.y * dt;
        particle.position.z += particle.velocity.z * dt;
    }
    const auto dead = std::remove_if(particles_.begin(), particles_.end(), [](const Particle& p) {
        return p.age >= p.lifetime;
    });
    particles_.erase(dead, particles_.end());

    // Rebuild the draw list flat each frame, like the fluid's and the props'.
    instances_.clear();
    for (const Particle& particle : particles_) {
        // How far through its life this speck is, 0 at birth and 1 at death. Every curve
        // below is read off it.
        const float t = std::clamp(particle.age / particle.lifetime, 0.0f, 1.0f);

        MeshInstance instance{};
        instance.model = Scene::kCubeModel;
        const float side = std::lerp(kSideAtBirth, kSideAtDeath, t) * particle.scale;
        XMStoreFloat4x4(&instance.transform,
                        XMMatrixScaling(side, side, side) * XMMatrixRotationY(particle.spin) *
                            XMMatrixTranslation(particle.position.x, particle.position.y,
                                                particle.position.z));
        instance.tint = XMFLOAT3(std::lerp(kHotTint.x, kCoolTint.x, t),
                                 std::lerp(kHotTint.y, kCoolTint.y, t),
                                 std::lerp(kHotTint.z, kCoolTint.z, t));
        // Squared so the brightness drops away early in the life and leaves a dim ember
        // trailing, rather than fading evenly with the colour.
        instance.emissive = std::lerp(kEmissiveAtBirth, kEmissiveAtDeath, t * t);
        instance.checker = 0.0f;
        instances_.push_back(instance);
    }
}

void Flame::Clear() {
    particles_.clear();
    instances_.clear();
    burn_carry_ = 0.0f;
}
