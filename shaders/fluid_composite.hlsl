// The lighter-fluid spray, pass 3: composite the smoothed surface into the scene as lit,
// tinted, translucent liquid. From the blurred surface depth it rebuilds each pixel's
// view-space position and, from the neighbouring depths, its surface normal -- then shades
// a thin naphtha film:
//
//   out0 (surface, additive):  a Fresnel-weighted sky reflection plus a sharp sun glint
//        and a faint body colour -- the wet sheen sitting on top of the background.
//   out1 (transmittance):      Beer-Lambert absorption from the accumulated thickness, per
//        channel (naphtha drinks a little more blue than red, so a thick pool reads amber).
//
// A dual-source blend composites these as `dst' = surface + dst * transmittance`, so the
// fluid tints and dims the scene behind it *per channel* with no copy of the scene buffer
// -- the background is still in the HDR target being written. Empty pixels return
// (surface 0, transmittance 1), leaving the scene untouched.
//
// Lighting is done in view space: the sun arrives pre-rotated into it, as in the impostor
// pass, so no world<->view round trip is needed.

cbuffer FluidCompositeConstants : register(b0) {
    // The perspective projection's x and y scales (proj._11, proj._22): view.x = ndc.x *
    // z / proj00, view.y = ndc.y * z / proj11, to rebuild view position from depth.
    float g_proj00;
    float g_proj11;
    float2 g_texel;  // one texel (1/width, 1/height), for the normal-reconstruction taps

    float3 g_sun_dir_view;  // toward the sun, view space, normalized
    float g_sentinel;       // the "no fluid" depth value

    float3 g_sun_color;  // sRGB, with its multiplier below
    float g_sun_intensity;

    float3 g_sky_ambient;  // sRGB, the reflected environment and body fill
    float g_ambient_strength;

    float3 g_tint;  // the naphtha body colour (sRGB)
    float g_gloss;  // sun-glint sharpness (specular exponent)

    // Per-channel extinction (higher = more absorbed) and an overall strength, so a
    // centimetre of fluid barely tints and a deep pool goes amber.
    float3 g_absorption;
    float g_absorption_strength;
};

Texture2D<float> g_depth : register(t0);      // blurred surface depth (view space z)
Texture2D<float> g_thickness : register(t1);  // accumulated fluid thickness (metres)
SamplerState g_sampler : register(s0);

float3 SrgbToLinear(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID) {
    VSOut output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// Rebuild the view-space position of the surface at `uv` given its depth `z`.
float3 ViewPosition(float2 uv, float z) {
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return float3(ndc.x * z / g_proj00, ndc.y * z / g_proj11, z);
}

struct PSOut {
    float4 surface : SV_Target0;
    float4 transmittance : SV_Target1;
};

PSOut PSMain(VSOut input) {
    PSOut output;

    const float z = g_depth.SampleLevel(g_sampler, input.uv, 0);
    // No fluid: leave the background exactly as it is (add nothing, absorb nothing).
    if (z >= g_sentinel * 0.5) {
        output.surface = float4(0.0, 0.0, 0.0, 0.0);
        output.transmittance = float4(1.0, 1.0, 1.0, 1.0);
        return output;
    }

    const float3 position = ViewPosition(input.uv, z);

    // Reconstruct the normal from the depth of the four neighbours. An empty or far-off
    // neighbour (a different cluster) falls back to the centre depth, so the silhouette
    // does not smear a normal across the gap. The nearer of each opposing pair is used, the
    // usual trick for a clean normal at depth discontinuities.
    const float zl = g_depth.SampleLevel(g_sampler, input.uv - float2(g_texel.x, 0.0), 0);
    const float zr = g_depth.SampleLevel(g_sampler, input.uv + float2(g_texel.x, 0.0), 0);
    const float zu = g_depth.SampleLevel(g_sampler, input.uv - float2(0.0, g_texel.y), 0);
    const float zd = g_depth.SampleLevel(g_sampler, input.uv + float2(0.0, g_texel.y), 0);

    const float3 pl = ViewPosition(input.uv - float2(g_texel.x, 0.0), zl < g_sentinel * 0.5 ? zl : z);
    const float3 pr = ViewPosition(input.uv + float2(g_texel.x, 0.0), zr < g_sentinel * 0.5 ? zr : z);
    const float3 pu = ViewPosition(input.uv - float2(0.0, g_texel.y), zu < g_sentinel * 0.5 ? zu : z);
    const float3 pd = ViewPosition(input.uv + float2(0.0, g_texel.y), zd < g_sentinel * 0.5 ? zd : z);

    const float3 dx = abs(pr.z - z) < abs(z - pl.z) ? (pr - position) : (position - pl);
    const float3 dy = abs(pd.z - z) < abs(z - pu.z) ? (pd - position) : (position - pu);
    float3 normal = normalize(cross(dx, dy));
    if (normal.z > 0.0) {
        normal = -normal;  // face the camera (view space -z)
    }

    const float3 view_dir = normalize(-position);
    const float3 light_dir = g_sun_dir_view;

    const float3 sun = SrgbToLinear(g_sun_color) * g_sun_intensity;
    const float3 ambient = SrgbToLinear(g_sky_ambient) * g_ambient_strength;
    const float3 tint = SrgbToLinear(g_tint);

    // Fresnel: a clear liquid is barely reflective head-on and mirror-like at grazing.
    const float f0 = 0.02;
    const float fresnel = f0 + (1.0 - f0) * pow(1.0 - saturate(dot(normal, view_dir)), 5.0);

    // The wet sheen laid on top of the background: the sky reflected by Fresnel, a tight
    // sun glint, and a faint body colour so the film is not pure glass.
    const float3 reflection = ambient * fresnel;
    const float3 half_vec = normalize(light_dir + view_dir);
    const float3 glint = sun * pow(saturate(dot(normal, half_vec)), g_gloss);
    const float3 body = tint * ambient * 0.15;

    // Beer-Lambert: how much of the background survives the fluid, per channel.
    const float thickness = g_thickness.SampleLevel(g_sampler, input.uv, 0);
    const float3 transmittance = exp(-thickness * g_absorption * g_absorption_strength);

    output.surface = float4(reflection + glint + body, 1.0);
    output.transmittance = float4(transmittance, 1.0);
    return output;
}
