#ifndef GRILL_COMMON_HLSLI
#define GRILL_COMMON_HLSLI

// Shared by the scene pass and the sky background pass, so the sky a metal
// reflects, the sky the horizon fades into, and the sky drawn behind everything
// are one and the same function.

// The atmosphere the sky is drawn from, handed in rather than baked in: what was
// once a wall of `static const` here is now a struct a caller fills, so a level
// can carry its own sky. SampleSky and CloudCoverage take one, and every pass that
// draws or reflects the sky embeds one in its constant buffer (see FrameConstants
// in scene.hlsl and SkyConstants in sky.hlsl) filled from the C++ Environment.
//
// The gradient is a deeper blue overhead (zenith) fading to a pale band at the
// horizon, over a dull bounce below (ground). The clouds ride on top: cloud_scale
// sets how large the puffs are, cloud_wind how far the layer scrolls per second,
// the coverage band (cloud_coverage/cloud_softness) turns the noise into
// cloud-or-sky, and cloud_opacity how solid a fully covered patch gets. All
// colours are display-space (sRGB), linearised where used, so the cloud tops read
// as bright white against the blue.
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

// How much cloud covers a world-space view direction at time `time`, in [0,1].
// The dome is flattened onto a plane by dividing xz by height, the noise is
// scrolled by the wind, and the coverage band turns it into cloud-or-sky. Clouds
// fade out toward the horizon, where the flattening would smear them into streaks.
float CloudCoverage(float3 dir, float time, SkyEnvironment sky) {
    if (dir.y <= 0.0f) {
        return 0.0f;
    }
    const float2 plane = dir.xz / max(dir.y, 0.15f);
    const float2 uv = plane * sky.cloud_scale + sky.cloud_wind * time;
    const float n = SkyFbm(uv);
    const float cover = smoothstep(sky.cloud_coverage, sky.cloud_coverage + sky.cloud_softness, n);
    return cover * smoothstep(0.0f, 0.35f, dir.y) * sky.cloud_opacity;
}

// The radiance of the analytic sky along a world-space direction, in linear light,
// with the drifting cloud layer laid over it. A horizon band brightens toward it
// and darkens into the ground below. This is the sky only -- the sun is left out,
// because the scene's direct term already accounts for it and its GGX lobe already
// broadens with roughness, so adding a sun disc here would count it twice.
float3 SampleSky(float3 dir, float time, SkyEnvironment sky) {
    const float3 zenith = SrgbToLinear(sky.zenith);
    const float3 horizon = SrgbToLinear(sky.horizon);
    const float3 ground = SrgbToLinear(sky.ground);
    // Above the horizon fade horizon->zenith; below, fall off to the ground colour
    // over a short span so the reflection has a floor rather than a hard seam.
    const float3 gradient = lerp(horizon, zenith, saturate(dir.y));
    const float3 base = lerp(gradient, ground, saturate(-dir.y * 4.0f));
    // The clouds ride on top, brightening the sky toward their sunlit white.
    return lerp(base, SrgbToLinear(sky.cloud_color), CloudCoverage(dir, time, sky));
}

#endif // GRILL_COMMON_HLSLI
