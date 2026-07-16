#ifndef GRILL_COMMON_HLSLI
#define GRILL_COMMON_HLSLI

// Shared by the scene pass and the sky background pass, so the sky a metal
// reflects, the sky the horizon fades into, and the sky drawn behind everything
// are one and the same function.

// The atmosphere the sky is drawn from, handed in rather than baked in: what was
// once a wall of `static const` here is now a struct a caller fills, so a level
// can carry its own sky. SampleSky takes one, and every pass that draws or reflects
// the sky embeds one in its constant buffer (see FrameConstants in scene.hlsl and
// SkyConstants in sky.hlsl) filled from the C++ Environment.
//
// The gradient is a deeper blue overhead (zenith) fading to a pale band at the
// horizon, over a dull bounce below (ground). The clouds are no longer drawn here --
// the sky is just the gradient now, and the volumetric cloud pass (shaders/clouds.hlsl)
// raymarches the clouds over it on screen. The cloud_* fields still describe that
// layer and are consumed there: cloud_scale sizes the puffs, cloud_wind scrolls them,
// cloud_coverage/cloud_softness set the coverage band, cloud_color their lit tone.
// cloud_opacity is no longer read by anything, kept only so the row stays packed. All
// colours are display-space (sRGB), linearised where used.
//
// Laid out to pack into cbuffer float4 rows with no straddle -- each float3 is
// followed by a scalar, and the whole is a 16-byte multiple -- so the C++ mirror
// (SkyEnvironment in renderer.cpp) can embed it anywhere. Do not reorder one side
// alone.
struct SkyEnvironment {
    float3 zenith;        float cloud_scale;
    float3 horizon;       float cloud_coverage;
    float3 ground;        float cloud_softness;
    float3 cloud_color;   float cloud_opacity;
    float2 cloud_wind;    float2 pad;
};

// Colour constants in this project are written the way they should look on screen,
// i.e. in sRGB. Lighting happens in linear light, so they are decoded on the way
// in and the frame is encoded again on the way out. Base-colour textures are
// decoded in hardware by their sRGB view; these do it by hand.
float3 SrgbToLinear(float3 c) {
    return select(c <= 0.04045f, c / 12.92f, pow((c + 0.055f) / 1.055f, 2.4f));
}

float3 LinearToSrgb(float3 c) {
    return select(c <= 0.0031308f, c * 12.92f, 1.055f * pow(c, 1.0f / 2.4f) - 0.055f);
}

// A cheap value-noise hash: a 2D point to a pseudo-random value in [0,1). No
// texture, so the sky costs nothing to feed.
float SkyHash(float2 p) {
    p = frac(p * float2(123.34f, 345.45f));
    p += dot(p, p + 34.345f);
    return frac(p.x * p.y);
}

// Value noise: the hash at the four lattice corners, smoothly interpolated. One
// octave of soft blobs.
float SkyValueNoise(float2 p) {
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float a = SkyHash(i + float2(0.0f, 0.0f));
    const float b = SkyHash(i + float2(1.0f, 0.0f));
    const float c = SkyHash(i + float2(0.0f, 1.0f));
    const float d = SkyHash(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// Fractal noise: four octaves of value noise, each half the amplitude and twice
// the frequency, which is what turns smooth blobs into cloud-like detail.
float SkyFbm(float2 p) {
    float value = 0.0f;
    float amplitude = 0.5f;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        value += amplitude * SkyValueNoise(p);
        p *= 2.0f;
        amplitude *= 0.5f;
    }
    return value;
}

// The radiance of the analytic sky along a world-space direction, in linear light: a
// plain gradient, a horizon band brightening toward it and darkening into the ground
// below. The clouds are drawn separately now, by the volumetric pass over the frame,
// so nothing cloud-like is laid on here -- which is what keeps the sky the reflections
// and the fog sample cheap. The sun is left out too, because the scene's direct term
// already accounts for it and its GGX lobe already broadens with roughness, so adding
// a sun disc here would count it twice.
float3 SampleSky(float3 dir, SkyEnvironment sky) {
    const float3 zenith = SrgbToLinear(sky.zenith);
    const float3 horizon = SrgbToLinear(sky.horizon);
    const float3 ground = SrgbToLinear(sky.ground);
    // Above the horizon fade horizon->zenith; below, fall off to the ground colour
    // over a short span so the reflection has a floor rather than a hard seam.
    const float3 gradient = lerp(horizon, zenith, saturate(dir.y));
    return lerp(gradient, ground, saturate(-dir.y * 4.0f));
}

#endif // GRILL_COMMON_HLSLI
