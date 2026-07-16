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
    // The post-process resolve. `exposure` scales the linear scene before tonemapping
    // (1.0 leaves it be); `bloom_intensity` is how strongly the glow is added back.
    // `bloom_threshold`/`bloom_knee` are the soft-knee bright-pass: only what is
    // brighter than the threshold blooms, easing in over the knee. Unlike the fields
    // above these ride to the tonemap and bloom passes, not the per-draw scene
    // constants, so they are not part of any cbuffer mirror -- plain Environment data.
    float exposure;
    float bloom_intensity;
    float bloom_threshold;
    float bloom_knee;
    // The volumetric cloud pass (shaders/clouds.hlsl). The slab lives between
    // `cloud_bottom` and `cloud_top` metres; `cloud_density` is its extinction (how
    // solid it reads); `cloud_detail` how hard the 3D noise erodes the edges into
    // billows. The horizontal shape, colour, coverage and wind come from `sky` above,
    // shared with the flat 2D layer -- these four are the volumetric-only knobs, and
    // like the shaft/bloom fields they reach only the cloud pass, not the SkyEnvironment
    // mirror, so they stay plain Environment data.
    float cloud_bottom;
    float cloud_top;
    float cloud_density;
    float cloud_detail;
};

// The default sky: the values that were `static const` in common.hlsli, scene.hlsl
// and lightshafts.hlsl before they became data. A level that names no [environment]
// -- or omits any field of one -- gets these, so the look is unchanged until a
// level chooses to differ. constexpr at namespace scope is implicitly inline, so
// every translation unit shares this one definition.
constexpr Environment kDefaultEnvironment{
    /*sky*/ {
        /*zenith*/ {0.40f, 0.54f, 0.76f}, /*cloud_scale*/ 0.55f,
        /*horizon*/ {0.70f, 0.76f, 0.82f}, /*cloud_coverage*/ 0.30f,
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
    /*exposure*/ 1.0f,
    /*bloom_intensity*/ 0.7f,
    /*bloom_threshold*/ 0.9f,
    /*bloom_knee*/ 0.4f,
    /*cloud_bottom*/ 160.0f,
    /*cloud_top*/ 210.0f,
    /*cloud_density*/ 0.24f,
    /*cloud_detail*/ 0.45f,
};

// Expands a time of day into a whole sky. `hours` is a clock time in [0,24) (values
// outside wrap, so 25 is 1am); the result is the sun's direction on its daily arc --
// rising in the east, overhead at noon, setting in the west, a dim moon at night --
// written into `sun_direction`, plus the matching Environment returned: warm and dim
// near the horizon, bright blue at noon (exactly kDefaultEnvironment at 12:00), dark
// and cool at midnight. Interpolated between a handful of keyframe moods.
//
// It is a convenience for authoring, not a runtime concept: the level parser calls
// it when a level names `time_of_day`, bakes the result into the level's fixed
// sun_direction and environment, and the renderer never knows a clock was involved.
// A level may still spell out `sun` or individual [environment] fields to override
// what this generates. Implemented in environment.cpp.
Environment EnvironmentAtHour(float hours, DirectX::XMFLOAT3& sun_direction);
