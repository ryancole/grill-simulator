#include "cook_information.hpp"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace {

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
    internal_temp_f_ +=
        (ambient_f - internal_temp_f_) * -std::expm1(-dt / profile_.temp_time_constant_s);

    // Bank this frame's cooking: only the heat carried above the threshold counts,
    // so food merely warming in the air banks nothing. Doneness never falls.
    const float over = internal_temp_f_ - profile_.cook_threshold_f;
    if (over > 0.0f) {
        doneness_ += over * profile_.doneness_gain * dt;
    }
}

CookInformation::Doneness CookInformation::DonenessBand() const {
    if (doneness_ >= profile_.burnt) return Doneness::Burnt;
    if (doneness_ >= profile_.well_done) return Doneness::WellDone;
    if (doneness_ >= profile_.medium_well) return Doneness::MediumWell;
    if (doneness_ >= profile_.medium) return Doneness::Medium;
    if (doneness_ >= profile_.medium_rare) return Doneness::MediumRare;
    if (doneness_ >= profile_.rare) return Doneness::Rare;
    return Doneness::Raw;
}

std::string_view CookInformation::DonenessLabel() const { return DonenessName(DonenessBand()); }

std::string_view DonenessName(CookInformation::Doneness band) {
    using Doneness = CookInformation::Doneness;
    switch (band) {
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

std::optional<CookInformation::Doneness> ParseDoneness(std::string_view token) {
    using Doneness = CookInformation::Doneness;
    if (token == "raw") return Doneness::Raw;
    if (token == "rare") return Doneness::Rare;
    if (token == "medium_rare" || token == "medium rare") return Doneness::MediumRare;
    if (token == "medium") return Doneness::Medium;
    if (token == "medium_well" || token == "medium well") return Doneness::MediumWell;
    if (token == "well_done" || token == "well done") return Doneness::WellDone;
    if (token == "burnt") return Doneness::Burnt;
    return std::nullopt;
}

XMFLOAT3 CookInformation::SurfaceTint() const {
    // Two segments: raw -> seared across the cook up to well-done, then seared ->
    // char as it runs on into burnt. Clamped at both ends so the colour holds once
    // fully charred.
    if (doneness_ <= profile_.well_done) {
        const float t = std::clamp(doneness_ / profile_.well_done, 0.0f, 1.0f);
        return Lerp(profile_.raw_tint, profile_.seared_tint, t);
    }
    const float t =
        std::clamp((doneness_ - profile_.well_done) / (profile_.burnt - profile_.well_done), 0.0f,
                   1.0f);
    return Lerp(profile_.seared_tint, profile_.char_tint, t);
}
