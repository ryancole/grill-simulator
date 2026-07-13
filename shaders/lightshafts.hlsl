// Volumetric sun shafts (crepuscular rays). A fullscreen pass that marches each
// view ray from the eye to the first surface, sampling the sun's shadow map at
// every step: the fraction of the ray that stands in sunlight is scattered light,
// weighted toward the sun by a phase function. The result is added into the HDR
// scene buffer before the tonemap pass, so shafts stream through the gaps between
// the fence, the grill and the trees exactly where the world is lit.
//
// It reuses the shadow map the scene already casts, so there is no new geometry
// pass -- only a march. The scene depth stops each ray at the surface, so a shaft
// never bleeds in front of a wall.
#include "common.hlsli"

cbuffer LightShaftConstants : register(b0) {
    // Clip space back to world, to rebuild each pixel's surface point from depth.
    row_major float4x4 g_inv_view_projection;
    // World to the sun's clip space, the same transform the shadow pass rasterized
    // with, so a marched point projects into the map the way a receiver does.
    row_major float4x4 g_light_view_projection;
    float3 g_camera_position;
    float g_pad0;
    // Unit vector toward the sun, for the phase function.
    float3 g_sun_direction;
    // How strong the effect is -- part of the level's atmosphere now, alongside the
    // colour and asymmetry below, filled from the C++ Environment.
    float g_shaft_intensity;
    // The sunlight the shafts scatter, in linear light, and the Henyey-Greenstein
    // asymmetry: >0 scatters forward, so shafts blaze when the eye looks toward the
    // sun and fade looking away.
    float3 g_shaft_color;
    float g_shaft_g;
};

// The scene depth (R32 view of the typeless depth buffer) and a plain view of the
// sun's shadow map -- sampled and compared by hand rather than through the scene's
// comparison sampler, since a march wants the raw stored depth.
Texture2D<float> g_depth : register(t0);
Texture2D<float> g_shadow : register(t1);
SamplerState g_sampler : register(s0);

// How many steps each ray is marched. More steps is smoother but costlier; the
// per-pixel dither below hides the banding a modest count would otherwise leave.
static const int kSteps = 48;
// The march is capped at this many metres: past the yard the shadow map does not
// reach anyway, and a sky pixel's ray would otherwise run to the far plane.
static const float kMaxDistance = 60.0f;
// A hair of slack in the shadow compare, so a lit sample does not shadow itself.
static const float kShaftBias = 0.0015f;

struct VSOutput {
    float4 position : SV_POSITION;
    // Texture coordinate, (0,0) at the top-left, for sampling depth and rebuilding
    // the view ray.
    float2 uv : TEXCOORD0;
};

// The same no-vertex-buffer fullscreen triangle the other post passes use, with
// the clip-space y flipped into the uv so (0,0) lands at the top-left texel.
VSOutput VSMain(uint id : SV_VertexID) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

// Henyey-Greenstein phase: the angular shape of single scattering, cos_theta being
// the angle between the view ray and the sun.
float HenyeyGreenstein(float cos_theta, float g) {
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cos_theta;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(denom, 1e-4f), 1.5f));
}

float4 PSMain(VSOutput input) : SV_TARGET {
    // Rebuild the surface point this pixel sees: its NDC, with the sampled depth,
    // carried back to world. A sky pixel reads depth 1 and rebuilds a point on the
    // far plane, so its ray marches the full distance -- which is where open shafts
    // are brightest.
    const float depth = g_depth.SampleLevel(g_sampler, input.uv, 0.0f);
    const float2 ndc = input.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(float4(ndc, depth, 1.0f), g_inv_view_projection);
    world /= world.w;

    const float3 ray = world.xyz - g_camera_position;
    const float ray_len = length(ray);
    const float3 ray_dir = ray / max(ray_len, 1e-4f);

    // March from the eye toward the surface, but no further than the shadowed yard.
    const float march_len = min(ray_len, kMaxDistance);
    const float step_len = march_len / kSteps;
    // Jitter each ray's start by up to one step, so the fixed step count does not
    // lay down banded rings; the noise averages out across neighbouring pixels.
    const float dither = SkyHash(input.position.xy);
    float3 pos = g_camera_position + ray_dir * step_len * dither;

    float lit_sum = 0.0f;
    [loop]
    for (int i = 0; i < kSteps; ++i) {
        const float4 light_clip = mul(float4(pos, 1.0f), g_light_view_projection);
        const float3 light_ndc = light_clip.xyz / light_clip.w;
        const float2 light_uv = light_ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        // Outside the map, or beyond its far plane, nothing casts -- count it lit,
        // which is what keeps the open sky full of scattered light.
        float lit = 1.0f;
        if (all(light_uv >= 0.0f) && all(light_uv <= 1.0f) && light_ndc.z <= 1.0f) {
            const float stored = g_shadow.SampleLevel(g_sampler, light_uv, 0.0f);
            lit = (light_ndc.z - kShaftBias <= stored) ? 1.0f : 0.0f;
        }
        lit_sum += lit;
        pos += ray_dir * step_len;
    }

    const float scattered = lit_sum / kSteps;
    const float phase = HenyeyGreenstein(dot(ray_dir, g_sun_direction), g_shaft_g);
    // Additive: the pass is blended onto the HDR scene, so it only ever adds light.
    return float4(g_shaft_color * scattered * phase * g_shaft_intensity, 1.0f);
}
