#include "wetness_information.hpp"

#include <algorithm>
#include <cmath>

const WetAgentTraits& WetAgentInfo(WetAgent agent) {
    // The fixed character of each liquid. Drying times are constants, not rates: a liquid
    // sheds most of its wetness over a few of these. Lighter fluid is a volatile solvent
    // and flashes off in well under a minute; water lingers for several.
    static constexpr WetAgentTraits kLighterFluid{"lighter fluid", 25.0f};
    static constexpr WetAgentTraits kWater{"water", 180.0f};

    switch (agent) {
    case WetAgent::LighterFluid:
        return kLighterFluid;
    case WetAgent::Water:
        return kWater;
    }
    return kLighterFluid;
}

void WetnessInformation::Wet(WetAgent agent, float amount) {
    if (amount <= 0.0f) {
        return;
    }

    // The most recent liquid wins: a squirt of fluid onto a water-damp thing makes it
    // fluid-wet. Set the agent before adding, so the saturation that carries it is this
    // liquid's, then cap at fully soaked -- an over-long spray puddles, it does not stack
    // past 1.
    agent_ = agent;
    wetness_ = std::min(1.0f, wetness_ + amount);
}

void WetnessInformation::Update(float dt) {
    if (dt <= 0.0f || wetness_ <= 0.0f) {
        return;
    }

    // Evaporation: the wetness closes an exp-shaped fraction of its gap to bone-dry this
    // frame, at the current liquid's time constant. The same easing CookInformation and
    // IgnitableRequirements use for temperature, aimed at 0 -- written with expm1 so it
    // stays stable and correct for any dt where a bare Euler step could overshoot past dry
    // on a long frame.
    const float tau = WetAgentInfo(agent_).dry_time_constant_s;
    wetness_ += (0.0f - wetness_) * -std::expm1(-dt / tau);
    if (wetness_ < 0.0f) {
        wetness_ = 0.0f;
    }
}
