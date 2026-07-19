#pragma once

#include <string_view>

// What has soaked one object, and how wet it currently is. WetnessInformation is a plain
// piece of state a thing holds by composition -- a log *has* a wetness; it is not a
// Wettable -- exactly like CookInformation (how cooked) and IgnitableRequirements (how
// close to catching). Where those two answer "how hot / how cooked", this answers "how wet,
// and wet with what".
//
// Two facts, deliberately paired:
//
//   - A saturation, 0 (bone dry) to 1 (soaked). It rises when something wets the object and
//     eases back toward dry on its own as the liquid evaporates -- reversible, the way a
//     cooking core's temperature is and unlike the one-way doneness beside it.
//   - The agent: *what* wet it. Different liquids do different things -- lighter fluid is an
//     accelerant, water (later) a retardant -- so knowing how wet a thing is means nothing
//     without knowing what with. Only one agent characterises an object at a time: the most
//     recent liquid to soak it wins (see Wet), which is the simple, legible rule; a fuller
//     model that blends several soaks at once can come later if the game ever needs it.
//
// This type is the shared *state and bookkeeping* of wetness. It owns none of the gameplay:
// it never lowers an ignition threshold, flares a fire or dampens one. Those effects live in
// the systems that read it, which dispatch on Agent() -- lighter fluid helps a log catch,
// water would stop it -- exactly as CookInformation "knows nothing about grills" and the
// cook loop reads its numbers. Keeping the divergent impacts out here is what lets a new
// liquid be added as one more WetAgent plus the handful of places that care, rather than a
// change threaded through this class.
//
// How fast a given liquid dries, and what to call it, *are* intrinsic to the substance, so
// those do live here -- in WetAgentTraits, the per-agent profile, the same split
// CookProfile makes for cooking. A volatile solvent like lighter fluid flashes off in
// seconds; water lingers for minutes.

// The liquids that can wet a thing. Extend this to add a new one -- water is sketched in
// already so the shape is clear -- then give it traits below and teach whichever systems
// care how to react to it.
enum class WetAgent {
    LighterFluid, // the spray can: a volatile accelerant
    Water,        // not yet placed in any level; here to show the system is not fluid-only
};

// The fixed character of one liquid -- the numbers that make lighter fluid behave unlike
// water regardless of what it has soaked. Split out of WetnessInformation so the state
// (how wet, right now) stays separate from the substance's nature (how it dries, what it is
// called), the same way CookProfile is split out of CookInformation.
struct WetAgentTraits {
    // A name for a prompt or readout: "lighter fluid", "water".
    std::string_view name;
    // How quickly the liquid evaporates, as a time constant in seconds: the wetness closes
    // most of its gap to bone-dry over a few of these. Short for a volatile solvent (lighter
    // fluid flashes off), long for water. This is the whole of "why lighter fluid dries
    // faster than water" -- one number per liquid.
    float dry_time_constant_s;
};

// The traits of one liquid. A pure function of the agent -- the substance's fixed nature,
// not per-object state -- so it lives beside the enum rather than inside the state.
const WetAgentTraits& WetAgentInfo(WetAgent agent);

class WetnessInformation {
public:
    // A default-built one is bone dry, with no agent yet -- an object nothing has touched.
    // The agent only becomes meaningful once something wets it (see Wet); while dry, IsWet
    // is false and Agent should not be consulted.
    WetnessInformation() = default;

    // Soak this object with `agent` by `amount` of saturation (0..1 units), clamping the
    // total at fully soaked. The applied agent becomes the one that characterises the
    // object -- the most recent liquid wins outright, so a squirt of fluid onto a
    // water-damp thing makes it fluid-wet. A caller spraying over time passes a rate times
    // the frame's dt; a single splash passes a lump. A non-positive amount does nothing and
    // leaves the current agent alone.
    void Wet(WetAgent agent, float amount);

    // Advance drying by `dt` seconds: the saturation eases toward bone-dry at the current
    // agent's evaporation rate. A no-op once already dry. Reversible in the sense that Wet
    // pushes it back up -- this is only the downward half, the liquid leaving on its own.
    void Update(float dt);

    // How wet, 0 (dry) to 1 (soaked). The raw value, for anything that wants a smooth amount
    // rather than the yes/no below.
    float Wetness() const { return wetness_; }

    // Whether there is enough liquid on it to matter -- above a hair off zero, so a thing
    // all but evaporated reads dry. The guard a reader uses before it trusts Agent().
    bool IsWet() const { return wetness_ > kDryThreshold; }

    // What it is wet with: the liquid that last soaked it. Meaningful only while IsWet() --
    // a dry object's agent is stale, whatever last touched it before it evaporated.
    WetAgent Agent() const { return agent_; }

private:
    // Below this saturation the object counts as dry: the last sliver evaporates without a
    // gameplay effect, and Agent() stops being trusted. Small enough that a real soak is
    // unambiguously "wet".
    static constexpr float kDryThreshold = 0.01f;

    // The most recent liquid to soak it. Only trusted while IsWet(); the default is
    // arbitrary and never read on a thing that has never been wet.
    WetAgent agent_ = WetAgent::LighterFluid;
    // The current saturation, 0..1. Rises with Wet, eases toward 0 in Update.
    float wetness_ = 0.0f;
};
