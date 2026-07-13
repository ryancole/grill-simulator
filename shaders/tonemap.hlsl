// The resolve pass. The whole world -- sky, scene, outlines, viewmodel -- is drawn
// into an R16G16B16A16_FLOAT scene buffer in linear light; this fullscreen pass is
// the single place that linear buffer is tonemapped and encoded to sRGB for the
// 8-bit swapchain. No scene shader carries the encode any more. The HUD text is
// drawn afterward straight onto the swapchain, so it is deliberately not tonemapped.
#include "common.hlsli"

cbuffer TonemapConstants : register(b0) {
    // A scalar on the linear scene before tonemapping. 1.0 leaves it untouched; it
    // is the hook a manual EV control or auto-exposure would drive later.
    float g_exposure;
    // How strongly the bloom is added back into the frame.
    float g_bloom_intensity;
};

// The HDR scene, one linear texel per output pixel. Read by integer Load rather
// than a filtered sample: the resolve is 1:1, so there is nothing to interpolate.
Texture2D<float4> g_hdr : register(t0);
// The bloom pyramid's top mip, at half resolution. Sampled (not loaded) so the
// hardware bilinearly upscales it to full res, which is also what softens it.
Texture2D<float4> g_bloom : register(t1);
SamplerState g_sampler : register(s0);

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// The same no-vertex-buffer fullscreen triangle the other post passes use, with the
// clip-space y flipped into the uv so (0,0) lands at the top-left texel.
VSOutput VSMain(uint id : SV_VertexID) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

// The Khronos PBR Neutral tone mapper. It holds hue and saturation where ACES
// would drift them, compresses only the highlights, and desaturates just the very
// brightest values toward white. Operates in linear light and returns linear,
// range [0,1].
float3 PbrNeutralToneMap(float3 color) {
    const float kStartCompression = 0.8f - 0.04f;
    const float kDesaturation = 0.15f;

    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    color -= offset;

    const float peak = max(color.r, max(color.g, color.b));
    if (peak < kStartCompression) {
        return color;
    }

    const float d = 1.0f - kStartCompression;
    const float new_peak = 1.0f - d * d / (peak + d - kStartCompression);
    color *= new_peak / peak;

    const float g = 1.0f - 1.0f / (kDesaturation * (peak - new_peak) + 1.0f);
    return lerp(color, new_peak.xxx, g);
}

float4 PSMain(VSOutput input) : SV_TARGET {
    const int3 texel = int3(int2(input.position.xy), 0);
    const float3 hdr = g_hdr.Load(texel).rgb;
    // The bloom is added into the scene before tonemapping, so its glow is rolled
    // off with the highlights that cast it rather than clipping to white.
    const float3 bloom = g_bloom.SampleLevel(g_sampler, input.uv, 0.0f).rgb;
    const float3 mapped = PbrNeutralToneMap((hdr + bloom * g_bloom_intensity) * g_exposure);
    return float4(LinearToSrgb(mapped), 1.0f);
}
