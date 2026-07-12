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

// The radiance of the analytic sky along a world-space direction, in linear light.
// A horizon band brightens toward it and darkens into the ground below. This is
// the sky only -- the sun is left out, because the scene's direct term already
// accounts for it and its GGX lobe already broadens with roughness, so adding a
// sun disc here would count it twice.
float3 SampleSky(float3 dir) {
    const float3 zenith = SrgbToLinear(kSkyZenith);
    const float3 horizon = SrgbToLinear(kSkyHorizon);
    const float3 ground = SrgbToLinear(kGroundBounce);
    // Above the horizon fade horizon->zenith; below, fall off to the ground colour
    // over a short span so the reflection has a floor rather than a hard seam.
    const float3 sky = lerp(horizon, zenith, saturate(dir.y));
    return lerp(sky, ground, saturate(-dir.y * 4.0f));
}

#endif // GRILL_COMMON_HLSLI
