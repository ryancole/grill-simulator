// Volumetric clouds. Where the old flat CloudCoverage painted a noise band onto the
// sky dome, this marches a real density slab: a horizontal layer between two
// altitudes, sampled with 3D noise, lit by a short march toward the sun so the
// sunward edges glow and the cores stay dark. A fullscreen pass, run after the world
// like the light shafts, compositing the clouds over the gradient sky the sky pass
// already laid down -- but only where the scene depth is the far plane, so the fence,
// the grill and the trees occlude the clouds behind them.
//
// The reflection probe and the scene fog still read the cheap 2D CloudCoverage in
// common.hlsli; this volumetric layer is the on-screen sky only.
#include "common.hlsli"

cbuffer CloudConstants : register(b0) {
    // Clip space back to world, to rebuild each pixel's view ray (and, from the
    // sampled depth, stop it at the surface -- a sky pixel reads depth 1).
    row_major float4x4 g_inv_view_projection;
    float3 g_camera_position;
    // Seconds since start-up, drifting the layer. Matches the clock the sky and the
    // shafts run on.
    float g_time;
    float3 g_sun_direction;
    // The altitudes the slab lives between, in world metres.
    float g_cloud_bottom;
    // The sunlight the clouds scatter and the flat sky fill in their shadows, both in
    // linear light, and the slab's top.
    float3 g_sun_color;
    float g_cloud_top;
    float3 g_sky_ambient;
    // Extinction: how much light a unit of density stops per metre, so how solid the
    // clouds read.
    float g_cloud_density;
    float g_sun_intensity;
    float g_ambient_strength;
    // How hard the higher-frequency noise erodes the cloud edges into billows.
    float g_cloud_detail;
    float g_pad;
    // The shared sky atmosphere: SampleSky's colours plus the cloud coverage/scale/
    // softness/wind the 2D layer uses, reused here for the horizontal shape.
    SkyEnvironment g_sky;
};

// The scene depth (R32 view of the typeless depth buffer). A sky pixel reads 1.
Texture2D<float> g_depth : register(t0);
SamplerState g_sampler : register(s0);

// The view ray is marched in this many steps through the slab; the light march
// toward the sun in this many. Both are dithered to hide the banding a modest count
// would leave. Kept small enough to stay cheap on a fullscreen pass.
static const int kViewSteps = 64;
static const int kLightSteps = 6;

// The cloud_wind in the shared SkyEnvironment was tuned for the flat 2D layer, whose
// noise domain is the normalized view direction; here the noise is indexed by world
// XZ (hundreds of metres), so the same wind drifts a far smaller fraction of a puff
// per second and reads as motionless. This multiplier restores a visible drift for the
// volumetric layer without disturbing the 2D layer the reflections and fog still read.
static const float kWindSpeed = 3.0f;

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// The same no-vertex-buffer fullscreen triangle the other post passes use, clip-y
// flipped into the uv so (0,0) lands at the top-left texel.
VSOutput VSMain(uint id : SV_VertexID) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

// A 3D value-noise hash: a lattice point to a pseudo-random value in [0,1).
float Hash3(float3 p) {
    p = frac(p * 0.3183099f + 0.1f);
    p *= 17.0f;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// 3D value noise: the hash at the eight cube corners, smoothly interpolated.
float Noise3(float3 p) {
    const float3 i = floor(p);
    const float3 f = frac(p);
    const float3 u = f * f * (3.0f - 2.0f * f);
    const float c000 = Hash3(i + float3(0, 0, 0));
    const float c100 = Hash3(i + float3(1, 0, 0));
    const float c010 = Hash3(i + float3(0, 1, 0));
    const float c110 = Hash3(i + float3(1, 1, 0));
    const float c001 = Hash3(i + float3(0, 0, 1));
    const float c101 = Hash3(i + float3(1, 0, 1));
    const float c011 = Hash3(i + float3(0, 1, 1));
    const float c111 = Hash3(i + float3(1, 1, 1));
    const float x00 = lerp(c000, c100, u.x);
    const float x10 = lerp(c010, c110, u.x);
    const float x01 = lerp(c001, c101, u.x);
    const float x11 = lerp(c011, c111, u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}

// Fractal 3D noise: three octaves, each half the amplitude and twice the frequency.
// The billowy detail eroded off the cloud edges.
float Fbm3(float3 p) {
    float value = 0.0f;
    float amplitude = 0.5f;
    [unroll]
    for (int i = 0; i < 3; ++i) {
        value += amplitude * Noise3(p);
        p *= 2.02f;
        amplitude *= 0.5f;
    }
    return value;
}

// The cloud density at a world point, in [0,1]. A 2D coverage field (FBM of the world
// XZ) is extruded through the slab with a rounded vertical profile, then its edges are
// eroded with higher-frequency 3D noise so the billows are three-dimensional rather
// than a flat cut-out. The slab is deliberately thin and wide (see cloud_bottom/top and
// the puff scale below), so the clouds read as a flat deck, not towers.
float CloudDensity(float3 p) {
    const float thickness = max(g_cloud_top - g_cloud_bottom, 1e-3f);
    const float h = saturate((p.y - g_cloud_bottom) / thickness);
    // Rounded vertical profile: fade in off the base, fall off toward the top, so the
    // slab has soft flat-ish bottoms and rounded tops rather than hard lids.
    const float height_grad = smoothstep(0.0f, 0.15f, h) * smoothstep(1.0f, 0.6f, h);
    if (height_grad <= 0.0f) {
        return 0.0f;
    }
    // Horizontal shape: world XZ scaled to a wide puff (a noise cell ~230 m across, so
    // the clouds are far wider than the slab is thick), scrolled by the wind. The fixed
    // offset lifts the sample off the world origin: SkyHash(0,0) is exactly 0, so without
    // it the noise dead-spots directly above the spawn (~0,0) and the sky reads clear
    // straight up no matter the coverage.
    const float2 uv = p.xz * g_sky.cloud_scale * 0.008f + float2(41.3f, 27.7f) +
                      g_sky.cloud_wind * g_time * kWindSpeed;
    const float base = SkyFbm(uv);
    // The coverage band: cloud_coverage sets how much of the sky is cloud, cloud_softness
    // the width of the transition. Tuned so a view ray straight up (which samples a single
    // column) is usually cloud, not a gap -- otherwise the sky reads empty overhead.
    const float cut = g_sky.cloud_coverage;
    float density = smoothstep(cut, cut + g_sky.cloud_softness, base) * height_grad;
    if (density <= 0.0f) {
        return 0.0f;
    }
    // Erode the edges with billowy 3D detail: where the detail noise dips, carve the
    // density back, so the cloud surface breaks into rounded lumps.
    const float3 dp = p * g_sky.cloud_scale * 0.08f + float3(13.7f, 5.1f, 29.3f) +
                      float3(g_sky.cloud_wind.x, 0.0f, g_sky.cloud_wind.y) * g_time * 2.0f * kWindSpeed;
    const float detail = Fbm3(dp);
    density = saturate(density - (1.0f - detail) * g_cloud_detail);
    return density;
}

// Henyey-Greenstein phase: the angular shape of single scattering, cos_theta the
// angle between the view ray and the sun. A forward lobe gives the sunward silver
// lining.
float HenyeyGreenstein(float cos_theta, float g) {
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cos_theta;
    return (1.0f - g2) / (4.0f * 3.14159265f * pow(max(denom, 1e-4f), 1.5f));
}

// March a short way toward the sun from a point inside the cloud, summing the density
// crossed. Returned as an optical-depth-ish sum the caller feeds to Beer's law.
float LightMarch(float3 pos) {
    const float thickness = max(g_cloud_top - g_cloud_bottom, 1e-3f);
    // A fixed reach into the layer -- far enough to shadow the deep cores, short
    // enough to stay cheap.
    const float step_len = thickness * 0.5f / kLightSteps;
    float sum = 0.0f;
    float3 p = pos;
    [loop]
    for (int i = 0; i < kLightSteps; ++i) {
        p += g_sun_direction * step_len;
        sum += CloudDensity(p) * step_len;
    }
    return sum;
}

float4 PSMain(VSOutput input) : SV_TARGET {
    // Only the open sky carries clouds: where the world stands, the depth is nearer
    // than the far plane, and the pass leaves that pixel untouched (transparent).
    const float depth = g_depth.SampleLevel(g_sampler, input.uv, 0.0f);
    if (depth < 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // The world-space view ray through this pixel.
    const float2 ndc = input.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
    float4 world = mul(float4(ndc, 1.0f, 1.0f), g_inv_view_projection);
    world /= world.w;
    const float3 dir = normalize(world.xyz - g_camera_position);

    // Nothing to march looking level or down: the slab is overhead, and a near-flat
    // ray would smear it to the horizon.
    if (dir.y <= 0.02f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // The segment of the ray inside the slab. The eye sits below the layer, so the
    // ray enters at the bottom plane and leaves at the top.
    const float t_near = (g_cloud_bottom - g_camera_position.y) / dir.y;
    const float t_far = (g_cloud_top - g_camera_position.y) / dir.y;
    if (t_far <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float march_start = max(t_near, 0.0f);
    const float seg_len = t_far - march_start;
    if (seg_len <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float step_len = seg_len / kViewSteps;

    // Jitter the start by up to one step so the fixed step count lays down no banded
    // rings; the noise averages out across neighbouring pixels.
    const float dither = SkyHash(input.position.xy);
    float t = march_start + step_len * dither;

    const float cos_theta = dot(dir, g_sun_direction);
    const float phase = HenyeyGreenstein(cos_theta, 0.55f);
    const float3 sun = g_sun_color * g_sun_intensity;
    // A cool sky fill for the shadowed underside, so a full deck reads as bright
    // overcast rather than a dreary grey; still below the sunlit tops so there is
    // some light-to-dark shape.
    const float3 ambient = SrgbToLinear(g_sky_ambient) * g_ambient_strength * 1.1f;
    // The slab is deepest along the ray near the horizon, where it would smear into a
    // milky band; fade the clouds out over the lowest sky only, so the deck still fills
    // most of the dome up to and over the eye.
    const float horizon_fade = smoothstep(0.06f, 0.24f, dir.y);

    float3 scattered = 0.0f;
    float transmittance = 1.0f;
    [loop]
    for (int i = 0; i < kViewSteps; ++i) {
        const float3 pos = g_camera_position + dir * t;
        const float density = CloudDensity(pos);
        if (density > 0.0f) {
            const float sigma = density * g_cloud_density;
            // Beer's law toward the sun for self-shadowing, and a powder term that
            // darkens the deep cores while the thin edges keep the sun's silver lining.
            const float light_depth = LightMarch(pos);
            const float beer = exp(-light_depth * g_cloud_density);
            const float powder = 1.0f - exp(-2.0f * sigma * step_len);
            const float3 luminance = sun * beer * phase + ambient;
            // Front-to-back: what this slice scatters, attenuated by all the cloud in
            // front of it.
            const float slice = 1.0f - exp(-sigma * step_len);
            scattered += transmittance * luminance * slice;
            transmittance *= exp(-sigma * step_len);
            if (transmittance < 0.01f) {
                break;
            }
        }
        t += step_len;
    }

    // Premultiplied over: the colour is already weighted by coverage, so the pipeline
    // blends it with ONE / INV_SRC_ALPHA onto the sky already in the HDR buffer. The
    // horizon fade thins both the colour and the coverage together, so the premultiplied
    // pair stays consistent.
    const float alpha = (1.0f - transmittance) * horizon_fade;
    return float4(scattered * horizon_fade, alpha);
}
