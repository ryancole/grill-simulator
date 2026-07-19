#pragma once

#include "cook_information.hpp" // kRoomTempF -- where an unlit thing sits

// What it takes to set one object alight, and how close it currently is to catching.
// Two numbers, the same shape as CookInformation: the temperature the thing must reach
// before it ignites (its fixed character, from the catalog) and the temperature it has
// actually reached right now (its running state). Wood does not catch the instant a flame
// touches it -- it has to heat through first -- so an ignitable warms toward the air
// around it over time and only catches once its own temperature crosses the threshold.
// Hold a flame on a log and it takes a moment; and a log just lit cannot flash its whole
// neighbour alight in one frame, because the neighbour still has to heat up.
//
// It is the *requirements-and-progress* side of ignition. The other two halves live
// elsewhere, deliberately:
//
//   - The supply side is HeatSource: the hot objects that impose a temperature on the
//     air around them (the lighter's flame, a lit log, the grill's grate). This type
//     never asks what is heating it -- only how hot the air is -- so anything hot enough
//     lights anything ignitable, and neither knows the other.
//   - Whether the thing is *currently* burning is its HeatSource's on/off state, not a
//     flag here. An ignitable object's whole point is that it becomes a heat source
//     itself, so it already has a HeatSource, and that HeatSource already answers "is
//     this burning?" (IsOn). Storing it here as well would be two truths to keep in
//     step. Igniting is therefore just switching that heat on -- see Props::Update.
//
// Like CookInformation, this is a plain piece of state an item holds by composition --
// a log *has* IgnitableRequirements; it is not an Ignitable -- not a base class anything
// derives from. Its internal temperature is reversible exactly like a cooking core's --
// it climbs toward a flame and falls back toward room air when the flame leaves before it
// caught -- but catching itself is one-way: nothing here puts a fire out.
//
// It knows nothing about logs, lighters or fire pits. It is fed a surrounding temperature
// (the same number CookInformation is, from the same heat sources) and eases toward it.
class IgnitableRequirements {
public:
    // A default-built one takes the default threshold and heating rate; the catalog gives
    // each ignitable type its own (see `ignite_temp` and `ignite_tau`).
    IgnitableRequirements() = default;
    IgnitableRequirements(float ignite_temp_f, float time_constant_s)
        : ignite_temp_f_(ignite_temp_f), time_constant_s_(time_constant_s) {}

    // Ease this object's own temperature toward the surrounding air by `dt` seconds --
    // Newton's law of heating, the very easing a cooking core follows -- so it warms
    // toward a flame held on it and drifts back down when the flame is taken away before
    // it caught. Reversible, exactly like an internal cook temperature; catching is not.
    void Update(float ambient_f, float dt);

    // Whether it has now reached its ignition temperature. Once it has, the caller lights
    // it and stops heating it (its heat switches on and this is not asked again), so the
    // one-way-ness of catching lives in the caller, not here.
    bool Ignited() const { return internal_temp_f_ >= ignite_temp_f_; }

    // The temperature it must reach to catch, and the one it has reached so far, both in
    // degrees Fahrenheit -- for tuning and for a readout.
    float IgniteTempF() const { return ignite_temp_f_; }
    float InternalTempF() const { return internal_temp_f_; }

private:
    // Hot enough that nothing in the yard reaches it by accident -- room air, a warm
    // afternoon, even a lit grill's grate all sit well under it -- so a deliberate flame
    // is what lights a fire. Each type overrides it from the catalog.
    float ignite_temp_f_ = 600.0f;
    // How slowly the thing heats through toward the surrounding air, as a time constant in
    // seconds: it closes most of the gap to a flame over a few of these. Long enough that
    // catching takes a deliberate hold rather than an instant touch, which is also what
    // stops a stack from flashing over all at once.
    float time_constant_s_ = 3.0f;
    // The temperature it has reached so far. Starts at room air, climbs toward a flame and
    // falls back toward room air when the flame leaves -- until it crosses ignite_temp_f_.
    float internal_temp_f_ = CookInformation::kRoomTempF;
};
