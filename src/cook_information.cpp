#include "cook_information.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {

// How fast the core chases the surrounding temperature, as a time constant in
// seconds: the core closes most of the gap to the air (or the grate) over a few of
// these. A thick cut is slow to heat and slow to cool, so this is deliberately long
// -- a couple of minutes -- rather than snapping to the surroundings each frame.
constexpr float kTempTimeConstant = 90.0f;

// Cooking only really begins once the core is hot: below this the food is warming
// but not yet changing, so no doneness banks. Roughly where meat starts to firm up.
constexpr float kCookThresholdF = 130.0f;

// Doneness banked per degree the core sits above the threshold, per second. Tuned
// so that a core held around a serving temperature walks from raw to well-done over
// a few minutes. These will firm up once a real heat source drives the core; for
// now, with the food sitting in room air, nothing crosses the threshold at all.
constexpr float kDonenessGain = 0.0002f;

// The doneness value at which each band begins. Cut so the middle bands each span a
// similar slice and Burnt only arrives well after well-done -- the player has to
// leave food on the heat to ruin it, not merely reach it.
constexpr float kRare = 0.12f;
constexpr float kMediumRare = 0.28f;
constexpr float kMedium = 0.45f;
constexpr float kMediumWell = 0.62f;
constexpr float kWellDone = 0.80f;
constexpr float kBurnt = 1.15f;

// The surface tints doneness lerps between: no change while raw, a seared brown by
// well-done, and a near-black char once burnt. Multiplied onto the meat's own
// colour, so these darken and brown it rather than repaint it.
constexpr XMFLOAT3 kRawTint{1.0f, 1.0f, 1.0f};
constexpr XMFLOAT3 kSearedTint{0.50f, 0.34f, 0.24f};
constexpr XMFLOAT3 kCharTint{0.13f, 0.11f, 0.10f};

XMFLOAT3 Lerp(const XMFLOAT3& a, const XMFLOAT3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

} // namespace

void CookInformation::Update(float ambient_f, float dt) {
    if (dt <= 0.0f) {
        return;
    }

    // Newton's law of heating: the core closes an exp-shaped fraction of its gap to
    // the surroundings this frame. Written with expm1 so it stays stable and correct
    // for any dt, where a bare Euler step could overshoot on a long frame.
    internal_temp_f_ += (ambient_f - internal_temp_f_) * -std::expm1(-dt / kTempTimeConstant);

    // Bank this frame's cooking: only the heat carried above the threshold counts,
    // so food merely warming in the air banks nothing. Doneness never falls.
    const float over = internal_temp_f_ - kCookThresholdF;
    if (over > 0.0f) {
        doneness_ += over * kDonenessGain * dt;
    }
}

CookInformation::Doneness CookInformation::DonenessBand() const {
    if (doneness_ >= kBurnt) return Doneness::Burnt;
    if (doneness_ >= kWellDone) return Doneness::WellDone;
    if (doneness_ >= kMediumWell) return Doneness::MediumWell;
    if (doneness_ >= kMedium) return Doneness::Medium;
    if (doneness_ >= kMediumRare) return Doneness::MediumRare;
    if (doneness_ >= kRare) return Doneness::Rare;
    return Doneness::Raw;
}

std::string_view CookInformation::DonenessLabel() const {
    switch (DonenessBand()) {
    case Doneness::Raw: return "raw";
    case Doneness::Rare: return "rare";
    case Doneness::MediumRare: return "medium rare";
    case Doneness::Medium: return "medium";
    case Doneness::MediumWell: return "medium well";
    case Doneness::WellDone: return "well done";
    case Doneness::Burnt: return "burnt";
    }
    return "raw";
}

XMFLOAT3 CookInformation::SurfaceTint() const {
    // Two segments: raw -> seared across the cook up to well-done, then seared ->
    // char as it runs on into burnt. Clamped at both ends so the colour holds once
    // fully charred.
    if (doneness_ <= kWellDone) {
        const float t = std::clamp(doneness_ / kWellDone, 0.0f, 1.0f);
        return Lerp(kRawTint, kSearedTint, t);
    }
    const float t = std::clamp((doneness_ - kWellDone) / (kBurnt - kWellDone), 0.0f, 1.0f);
    return Lerp(kSearedTint, kCharTint, t);
}
