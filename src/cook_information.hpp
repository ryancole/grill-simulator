#pragma once

#include <DirectXMath.h>

#include <optional>
#include <string_view>

// The parameters that make one kind of food cook the way it does: how fast its core
// chases the surrounding heat, how hot it must get before cooking begins, how quickly
// doneness banks, where the doneness bands fall, and the colours it browns through.
//
// Split out of CookInformation so a steak and a chicken need not cook alike -- each
// food type carries its own profile (loaded from the catalog, see catalog.hpp), and
// the cook reads its numbers from there rather than from constants baked into code.
// Every field defaults to the original single set of constants, so a CookInformation
// built without a profile -- or a catalog entry that omits a field -- cooks exactly as
// the game did when there was only one way to cook.
struct CookProfile {
    // How fast the core chases the surrounding temperature, as a time constant in
    // seconds: the core closes most of the gap to the air (or the grate) over a few of
    // these. A thick cut is slow to heat and slow to cool, so this is deliberately
    // long -- a couple of minutes -- rather than snapping to the surroundings.
    float temp_time_constant_s = 90.0f;
    // Cooking only really begins once the core is hot: below this the food is warming
    // but not yet changing, so no doneness banks. Roughly where meat starts to firm up.
    float cook_threshold_f = 130.0f;
    // Doneness banked per degree the core sits above the threshold, per second. Tuned
    // so a core held around a serving temperature walks from raw to well-done over a
    // few minutes.
    float doneness_gain = 0.0002f;

    // The doneness value at which each band begins, in the usual steakhouse order. Cut
    // so the middle bands each span a similar slice and Burnt only arrives well after
    // well-done -- the player has to leave food on the heat to ruin it.
    float rare = 0.12f;
    float medium_rare = 0.28f;
    float medium = 0.45f;
    float medium_well = 0.62f;
    float well_done = 0.80f;
    float burnt = 1.15f;

    // The surface tints doneness lerps between: no change while raw, a seared brown by
    // well-done, and a near-black char once burnt. Multiplied onto the food's own
    // colour, so these darken and brown it rather than repaint it.
    DirectX::XMFLOAT3 raw_tint{1.0f, 1.0f, 1.0f};
    DirectX::XMFLOAT3 seared_tint{0.50f, 0.34f, 0.24f};
    DirectX::XMFLOAT3 char_tint{0.13f, 0.11f, 0.10f};
};

// The cooking state of one piece of food -- a steak, a patty. CookInformation owns
// the two numbers that together answer "how cooked is this?": the internal
// temperature it has reached right now, and the doneness that has banked from
// sitting at cooking temperatures over time. How those numbers evolve is set by the
// CookProfile it holds -- the food type's own, so different foods cook differently.
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
// This type knows nothing about grills. It is handed a surrounding temperature each
// frame -- room air when the food is off the heat, or a hot grate's temperature when
// it is on one (see heat_source.hpp) -- and Newton's law of heating eases the internal
// temperature toward it, while whatever heat the core carries above the cooking
// threshold is integrated into doneness.
class CookInformation {
public:
    // A default-built cook uses the profile's defaults (the original single set of
    // constants); giving it a food type's profile makes it cook that type's way.
    CookInformation() = default;
    explicit CookInformation(const CookProfile& profile) : profile_(profile) {}

    // Room temperature, in degrees Fahrenheit: where food starts, and where it
    // relaxes back to once it is off any heat. A U.S. backyard grill game thinks in
    // Fahrenheit, so the whole type does. Environmental, not per-food, so it stays a
    // shared constant (the heat sources read it too).
    static constexpr float kRoomTempF = 68.0f;

    // The doneness bands, in the usual steakhouse order. Raw is food that has not
    // cooked yet; Burnt is food cooked well past well-done, where the surface chars.
    // Derived from the accumulated doneness value, not read straight off temperature.
    enum class Doneness { Raw, Rare, MediumRare, Medium, MediumWell, WellDone, Burnt };

    // Advances the cook by `dt` seconds while the food is surrounded by `ambient_f`
    // degrees Fahrenheit -- room air, or the grate beneath it. The internal
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
    CookProfile profile_{};
    float internal_temp_f_ = kRoomTempF;
    float doneness_ = 0.0f;
};

// A doneness band's readout label -- the spaced form "medium rare", the same text the
// member DonenessLabel() returns (it is DonenessName(DonenessBand())). Free so callers
// holding a bare band -- a win-condition goal, not a cooking meat -- can name it too.
std::string_view DonenessName(CookInformation::Doneness band);

// The reverse: a band from the token the level and catalog files spell it with. Accepts
// the underscore form ("medium_rare") those files use and the spaced readout form, and
// returns nullopt on anything unknown so a loader can name the bad value rather than
// guess. The two-word bands are the only ones the two spellings differ on.
std::optional<CookInformation::Doneness> ParseDoneness(std::string_view token);
