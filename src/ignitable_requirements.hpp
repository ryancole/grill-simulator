#pragma once

// What it takes to set one object alight: the conditions the world around it has to
// meet before it catches fire. Today that is a single number -- how hot the
// surrounding air must get -- but this is the type that grows as ignition gets fussier
// (a dwell time before wood catches, a soaking in lighter fluid, a thing that will not
// light in the rain).
//
// It is the *requirements* side of ignition and nothing else. The other two halves
// already exist and are deliberately left where they are:
//
//   - The supply side is HeatSource: the hot objects that impose a temperature on the
//     air around them (the lighter's flame, a lit log, the grill's grate). This type
//     never asks what is heating it -- only whether the air is hot enough -- so
//     anything hot enough lights anything ignitable, and neither knows the other.
//   - Whether the thing is *currently* burning is its HeatSource's on/off state, not a
//     flag here. An ignitable object's whole point is that it becomes a heat source
//     itself, so it already has a HeatSource, and that HeatSource already answers "is
//     this burning?" (IsOn). Storing it here as well would be two truths to keep in
//     step. Igniting is therefore just switching that heat on -- see Props::Update.
//
// Like CookInformation, this is a plain piece of state an item holds by composition --
// a log *has* IgnitableRequirements; it is not an Ignitable -- not a base class
// anything derives from. And like doneness, catching fire is one-way: nothing here
// puts a fire out, because nothing in the game does yet.
//
// It knows nothing about logs, lighters or fire pits. It is handed a surrounding
// temperature (the same number CookInformation is fed, from the same heat sources) and
// answers one question about it.
class IgnitableRequirements {
public:
    // A default-built one takes the default ignition temperature; the catalog gives each
    // ignitable type its own (see `ignite_temp`).
    IgnitableRequirements() = default;
    explicit IgnitableRequirements(float ignite_temp_f) : ignite_temp_f_(ignite_temp_f) {}

    // Whether air at `ambient_f` degrees Fahrenheit is enough to set this alight. The
    // caller decides what to do about it -- this only reads the thermometer.
    bool MetBy(float ambient_f) const { return ambient_f >= ignite_temp_f_; }

    // How hot the surrounding air must get before this catches, in degrees Fahrenheit.
    // Here for tuning and for a readout; the check itself is MetBy.
    float IgniteTempF() const { return ignite_temp_f_; }

private:
    // Hot enough that nothing in the yard reaches it by accident -- room air, a warm
    // afternoon, even a lit grill's grate all sit well under it -- so a deliberate
    // flame is what lights a fire. Each type overrides it from the catalog.
    float ignite_temp_f_ = 600.0f;
};
