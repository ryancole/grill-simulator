#include "ignitable_requirements.hpp"

#include <cmath>

void IgnitableRequirements::Update(float ambient_f, float dt) {
    if (dt <= 0.0f) {
        return;
    }

    // The same easing CookInformation gives a cooking core: the temperature closes an
    // exp-shaped fraction of its gap to the surroundings this frame. Written with expm1 so
    // it stays stable and correct for any dt, where a bare Euler step could overshoot on a
    // long frame. Symmetric in direction -- a positive gap warms it, a negative one cools
    // it -- so a flame taken away before the thing caught lets it drift back toward room
    // air, and only a sustained hold carries it across the ignition threshold.
    internal_temp_f_ += (ambient_f - internal_temp_f_) * -std::expm1(-dt / time_constant_s_);
}
