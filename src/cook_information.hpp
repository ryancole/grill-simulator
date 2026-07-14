#pragma once

#include <DirectXMath.h>

#include <string_view>

// The cooking state of one piece of food -- a steak, a patty. CookInformation owns
// the two numbers that together answer "how cooked is this?": the internal
// temperature it has reached right now, and the doneness that has banked from
// sitting at cooking temperatures over time.
//
// It is a plain piece of state a food item holds (by composition -- a meat has a
// CookInformation; it is not a Cookable), not a base class anything derives from.
//
// The two numbers behave differently on purpose. Temperature is instantaneous and
// reversible: set food on heat and its core climbs toward that heat; take it off
// and the core drifts back down toward the surrounding air. Doneness is one-way:
// it only ever rises, because cooking is a chemical change that does not undo -- a
// steak does not un-sear as it cools. Doneness is what the "rare / medium / well
// done" readout reads from, and what browns and finally chars the surface.
//
// This type knows nothing about grills. It is handed a surrounding temperature
// each frame -- room air today, a hot grate tomorrow -- and Newton's law of
// heating eases the internal temperature toward it, while whatever heat the core
// carries above a cooking threshold is integrated into doneness. The heat source
// that would supply a grate temperature is the next thing to build; until then the
// meat simply sits in room air and never cooks.
class CookInformation {
public:
    // Room temperature, in degrees Fahrenheit: where food starts, and where it
    // relaxes back to once it is off any heat. A U.S. backyard grill game thinks in
    // Fahrenheit, so the whole type does.
    static constexpr float kRoomTempF = 68.0f;

    // The doneness bands, in the usual steakhouse order. Raw is food that has not
    // cooked yet; Burnt is food cooked well past well-done, where the surface chars.
    // Derived from the accumulated doneness value, not read straight off temperature.
    enum class Doneness { Raw, Rare, MediumRare, Medium, MediumWell, WellDone, Burnt };

    // Advances the cook by `dt` seconds while the food is surrounded by `ambient_f`
    // degrees Fahrenheit -- room air, or (soon) the grate beneath it. The internal
    // temperature eases toward `ambient_f`; doneness banks whatever cooking the
    // current core temperature buys this frame and never gives it back.
    void Update(float ambient_f, float dt);

    // The current core temperature, in degrees Fahrenheit.
    float InternalTempF() const { return internal_temp_f_; }
    // The accumulated cook: 0 while raw, climbing without bound as the food cooks
    // and then chars. Monotonic -- it never decreases. The bands below are cut from
    // it; the raw value is here for tuning and for anything that wants a smooth
    // progress rather than a band.
    float DonenessValue() const { return doneness_; }
    Doneness DonenessBand() const;
    // The band as text for a prompt or readout: "raw", "medium rare", "burnt", ...
    std::string_view DonenessLabel() const;

    // A colour to multiply the food's base colour by, browning it as it cooks:
    // white (no change) while raw, through searing browns, to near-black once burnt.
    // Meant to feed MeshInstance::tint, so raw meat draws exactly as the model does.
    DirectX::XMFLOAT3 SurfaceTint() const;

private:
    float internal_temp_f_ = kRoomTempF;
    float doneness_ = 0.0f;
};
