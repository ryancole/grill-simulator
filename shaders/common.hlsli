#ifndef GRILL_COMMON_HLSLI
#define GRILL_COMMON_HLSLI

// Shared by the scene pass and the sky background pass, so the sky a metal
// reflects, the sky the horizon fades into, and the sky drawn behind everything
// are one and the same function.

// The analytic sky the specular IBL reflects and the background pass draws: a
// deeper blue overhead fading to a pale band at the horizon, over the same dull
// bounce below. All display-space, linearised where used.
static const float3 kGroundBounce = float3(0.20f, 0.18f, 0.16f);
static const float3 kSkyZenith = float3(0.40f, 0.54f, 0.76f);
static const float3 kSkyHorizon = float3(0.70f, 0.76f, 0.82f);

// The drifting cloud layer laid over that gradient. Sunlit tops, written the way
// they should look on screen and linearised where used, so they read as bright
// white against the blue. kCloudScale sets how large the puffs are; kCloudWind is
// how far the layer scrolls per second; the coverage band turns the noise into
// cloud-or-sky, and the opacity is how solid a fully covered patch gets.
static const float3 kCloudColor = float3(0.96f, 0.97f, 1.0f);
static const float kCloudScale = 0.55f;
static const float2 kCloudWind = float2(0.010f, 0.006f);
static const float kCloudCoverage = 0.52f;
static const float kCloudSoftness = 0.30f;
static const float kCloudOpacity = 0.9f;

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
float CloudCoverage(float3 dir, float time) {
    if (dir.y <= 0.0f) {
        return 0.0f;
    }
    const float2 plane = dir.xz / max(dir.y, 0.15f);
    const float2 uv = plane * kCloudScale + kCloudWind * time;
    const float n = SkyFbm(uv);
    const float cover = smoothstep(kCloudCoverage, kCloudCoverage + kCloudSoftness, n);
    return cover * smoothstep(0.0f, 0.35f, dir.y) * kCloudOpacity;
}

// The radiance of the analytic sky along a world-space direction, in linear light,
// with the drifting cloud layer laid over it. A horizon band brightens toward it
// and darkens into the ground below. This is the sky only -- the sun is left out,
// because the scene's direct term already accounts for it and its GGX lobe already
// broadens with roughness, so adding a sun disc here would count it twice.
float3 SampleSky(float3 dir, float time) {
    const float3 zenith = SrgbToLinear(kSkyZenith);
    const float3 horizon = SrgbToLinear(kSkyHorizon);
    const float3 ground = SrgbToLinear(kGroundBounce);
    // Above the horizon fade horizon->zenith; below, fall off to the ground colour
    // over a short span so the reflection has a floor rather than a hard seam.
    const float3 sky = lerp(horizon, zenith, saturate(dir.y));
    const float3 base = lerp(sky, ground, saturate(-dir.y * 4.0f));
    // The clouds ride on top, brightening the sky toward their sunlit white.
    return lerp(base, SrgbToLinear(kCloudColor), CloudCoverage(dir, time));
}

#endif // GRILL_COMMON_HLSLI
