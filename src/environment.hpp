#pragma once

#include <DirectXMath.h>

// The atmosphere of a level: the sky and clouds above, and the sun, ambient, fill,
// fog and light-shaft terms the shaders shade with. Once `static const` baked into
// the shaders, this is now plain data -- a level carries one, the parser fills it,
// and the renderer distributes it into each pass's constant buffer. Shared here so
// both LevelDef (level.hpp) and the Renderer speak the same struct.

// Mirrors the SkyEnvironment struct in shaders/common.hlsli: the sky gradient and
// the drifting cloud layer SampleSky draws from. Laid out to pack into cbuffer
// float4 rows with no straddle -- each XMFLOAT3 is followed by a scalar, and the
// whole is a 16-byte multiple -- so the renderer can embed it straight into a
// constant buffer. Do not reorder one side alone.
struct SkyEnvironment {
    DirectX::XMFLOAT3 zenith;
    float cloud_scale;
    DirectX::XMFLOAT3 horizon;
    float cloud_coverage;
    DirectX::XMFLOAT3 ground;
    float cloud_softness;
    DirectX::XMFLOAT3 cloud_color;
    float cloud_opacity;
    DirectX::XMFLOAT2 cloud_wind;
    float pad0;
    float pad1;
};
static_assert(sizeof(SkyEnvironment) == 80, "SkyEnvironment must mirror the HLSL cbuffer rows");

// The whole atmosphere: the sky/clouds above, plus the sun, ambient, fill, fog and
// light-shaft terms. One source of truth the renderer distributes into every pass;
// see Renderer::SetEnvironment and the ApplyEnvironment overloads. The sun's
// *direction* is not here -- it drives the shadow map's ortho box and the probe
// capture, not just the look, so it stays a LevelDef field of its own.
struct Environment {
    SkyEnvironment sky;
    // The sun's colour and its radiance (a pi folded in; see g_sun_intensity in
    // scene.hlsl, where the diffuse BRDF's 1/pi would otherwise darken everything).
    DirectX::XMFLOAT3 sun_color;
    float sun_intensity;
    // The flat sky tone the diffuse ambient and fill use, and how strong each is.
    DirectX::XMFLOAT3 sky_ambient;
    float ambient_strength;
    float fill_strength;
    // The distance band the yard fades into the sky over.
    float fog_start;
    float fog_end;
    // The volumetric sun shafts: the sunlight they scatter, how strong, and the
    // Henyey-Greenstein forward-scatter asymmetry.
    DirectX::XMFLOAT3 shaft_color;
    float shaft_intensity;
    float shaft_g;
};

// The default sky: the values that were `static const` in common.hlsli, scene.hlsl
// and lightshafts.hlsl before they became data. A level that names no [environment]
// -- or omits any field of one -- gets these, so the look is unchanged until a
// level chooses to differ. constexpr at namespace scope is implicitly inline, so
// every translation unit shares this one definition.
constexpr Environment kDefaultEnvironment{
    /*sky*/ {
        /*zenith*/ {0.40f, 0.54f, 0.76f}, /*cloud_scale*/ 0.55f,
        /*horizon*/ {0.70f, 0.76f, 0.82f}, /*cloud_coverage*/ 0.52f,
        /*ground*/ {0.20f, 0.18f, 0.16f}, /*cloud_softness*/ 0.30f,
        /*cloud_color*/ {0.96f, 0.97f, 1.0f}, /*cloud_opacity*/ 0.9f,
        /*cloud_wind*/ {0.010f, 0.006f}, /*pad*/ 0.0f, 0.0f,
    },
    /*sun_color*/ {1.0f, 0.96f, 0.88f},
    // pi, the same value scene.hlsl's kSunIntensity = kPi carried.
    /*sun_intensity*/ 3.14159265f,
    /*sky_ambient*/ {0.52f, 0.62f, 0.76f},
    /*ambient_strength*/ 0.65f,
    /*fill_strength*/ 0.18f,
    /*fog_start*/ 20.0f,
    /*fog_end*/ 90.0f,
    /*shaft_color*/ {1.0f, 0.96f, 0.88f},
    /*shaft_intensity*/ 0.9f,
    /*shaft_g*/ 0.76f,
};
