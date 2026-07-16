#include "heat_source.hpp"

#include "cook_information.hpp"

using namespace DirectX;

float HeatSource::TemperatureAt(FXMVECTOR point) const {
    // An off (unlit) source emits nothing: the air everywhere is just room air, so the
    // cook sees no heat from it until it is switched on.
    if (!on_) {
        return CookInformation::kRoomTempF;
    }

    const float distance =
        XMVectorGetX(XMVector3Length(XMVectorSubtract(point, XMLoadFloat3(&origin_))));

    // Past the reach the source contributes nothing: the air is whatever it already
    // was, which for the cook is room temperature. Kept as an early out so the common
    // case -- a meat nowhere near the grill -- costs a single distance and returns.
    if (distance >= reach_) {
        return CookInformation::kRoomTempF;
    }

    // Smooth falloff across the reach: full heat at the centre, room air at the edge,
    // with an eased shoulder (smoothstep of the normalised distance, inverted) so the
    // hot zone has a soft rim rather than a hard ring the food would cross in one
    // step. The result rides above room temperature by the source's excess, scaled by
    // how central the point is.
    const float t = distance / reach_;
    const float falloff = 1.0f - (t * t * (3.0f - 2.0f * t));
    return CookInformation::kRoomTempF + (emitter_temp_f_ - CookInformation::kRoomTempF) * falloff;
}
