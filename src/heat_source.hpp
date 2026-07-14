#pragma once

#include <DirectXMath.h>

// The heat one hot object radiates into the air around it -- a lit grill's grate is
// the only thing in the yard that emits any today. HeatSource owns the two numbers
// that describe that heat: how hot it is right at the source, and how far the heat
// carries before it fades back into room air.
//
// It is the other half of the cook. CookInformation asks, every frame, "what
// temperature surrounds this food?" and eases toward it; a HeatSource is one answer
// to that question -- the air temperature it imposes on a point some distance away.
// The two types meet at that single number and nowhere else, so neither has to know
// the other exists: give a meat a hotter surrounding and it cooks, whatever produced
// the heat.
//
// Like CookInformation this is a plain piece of state an object holds by composition
// -- the grill *has* a HeatSource; it is not one -- not a base class anything derives
// from. Where CookInformation deliberately knows nothing about grills, though, a
// HeatSource does know where it currently sits: its origin is runtime state the owner
// refreshes each frame from the (possibly knocked-over) body it rides, so the hot
// zone travels with the grill when it is shoved. The emitting temperature and reach
// are its fixed character; the origin is where that character happens to be right now.
class HeatSource {
public:
    HeatSource() = default;
    // `emitter_temp_f` is the air temperature at the very centre of the source, in
    // degrees Fahrenheit; `reach` is how far in metres that heat carries before it
    // has faded entirely back to room air.
    HeatSource(float emitter_temp_f, float reach)
        : emitter_temp_f_(emitter_temp_f), reach_(reach) {}

    // Move the hot centre to `origin` in world space. The owner calls this each frame
    // from the body's current pose, so the heat follows the object as it is knocked
    // around the yard.
    void SetOrigin(DirectX::FXMVECTOR origin) { DirectX::XMStoreFloat3(&origin_, origin); }
    DirectX::XMFLOAT3 Origin() const { return origin_; }

    // The air temperature at the very centre of the source, in degrees Fahrenheit --
    // the emitting temperature that is its fixed character, before any distance
    // falloff. A debug readout of "how hot is this heat source" reads this.
    float EmitterTempF() const { return emitter_temp_f_; }

    // The air temperature this source imposes at `point`, in degrees Fahrenheit: the
    // full emitter temperature at the centre, easing smoothly down to room temperature
    // by `reach` metres away and holding at room temperature beyond it. Never returns
    // less than room temperature -- a heat source only ever warms the air, it never
    // draws warmth out of it, so food set beside a cold source is left exactly as the
    // air already found it.
    float TemperatureAt(DirectX::FXMVECTOR point) const;

private:
    float emitter_temp_f_ = 0.0f;
    float reach_ = 1.0f;
    DirectX::XMFLOAT3 origin_{0.0f, 0.0f, 0.0f};
};
