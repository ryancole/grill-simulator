// Bloom: the soft glow that bright parts of the HDR scene bleed into their
// surroundings. Built as a small pyramid -- the HDR buffer is progressively
// downsampled into ever-smaller mips with a wide 13-tap filter, then those mips
// are upsampled and summed back with a tent filter, so one bright pixel spreads
// into a large, soft, artefact-free halo. The top of the chain is added back into
// the frame by the tonemap resolve. This is the Jimenez / "Next Generation Post
// Processing in Call of Duty" scheme.

cbuffer BloomConstants : register(b0) {
    // One texel of the *source* texture this pass reads: 1 / its dimensions.
    float2 g_src_texel;
    // Downsample: the soft-knee threshold, applied on level 0 only (<=0 disables it
    // for the deeper levels). Upsample: reused as the tent-filter radius.
    float g_param0;
    // Downsample: the soft-knee width. Unused by the upsample.
    float g_param1;
    // The slot of the source texture (the HDR buffer or a bloom mip) in the one bound
    // heap, fetched bindlessly rather than through a table -- it changes every pass as
    // the pyramid walks up and down.
    uint g_source_index;
};

SamplerState g_sampler : register(s0);

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

// The shared fullscreen triangle, uv (0,0) at the top-left.
VSOutput VSMain(uint id : SV_VertexID) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    output.uv = uv;
    return output;
}

// The 13-tap downsample: five overlapping 2x2 boxes, the centre one double-
// weighted. A wide low-pass that halves the resolution without the blocky aliasing
// a naive 2x2 average would leave, and the reason the bloom stays stable rather
// than flickering as the camera moves.
float3 Downsample(float2 uv) {
    const Texture2D<float4> g_source = ResourceDescriptorHeap[g_source_index];
    const float2 t = g_src_texel;
    const float3 a = g_source.SampleLevel(g_sampler, uv + t * float2(-2, 2), 0).rgb;
    const float3 b = g_source.SampleLevel(g_sampler, uv + t * float2(0, 2), 0).rgb;
    const float3 c = g_source.SampleLevel(g_sampler, uv + t * float2(2, 2), 0).rgb;
    const float3 d = g_source.SampleLevel(g_sampler, uv + t * float2(-2, 0), 0).rgb;
    const float3 e = g_source.SampleLevel(g_sampler, uv, 0).rgb;
    const float3 f = g_source.SampleLevel(g_sampler, uv + t * float2(2, 0), 0).rgb;
    const float3 g = g_source.SampleLevel(g_sampler, uv + t * float2(-2, -2), 0).rgb;
    const float3 h = g_source.SampleLevel(g_sampler, uv + t * float2(0, -2), 0).rgb;
    const float3 i = g_source.SampleLevel(g_sampler, uv + t * float2(2, -2), 0).rgb;
    const float3 j = g_source.SampleLevel(g_sampler, uv + t * float2(-1, 1), 0).rgb;
    const float3 k = g_source.SampleLevel(g_sampler, uv + t * float2(1, 1), 0).rgb;
    const float3 l = g_source.SampleLevel(g_sampler, uv + t * float2(-1, -1), 0).rgb;
    const float3 m = g_source.SampleLevel(g_sampler, uv + t * float2(1, -1), 0).rgb;
    float3 result = e * 0.125f;
    result += (a + c + g + i) * 0.03125f;
    result += (b + d + f + h) * 0.0625f;
    result += (j + k + l + m) * 0.125f;
    return result;
}

float4 PSDownsample(VSOutput input) : SV_TARGET {
    float3 color = Downsample(input.uv);
    // Level 0 keeps only what is brighter than the knee, so the bloom grows from the
    // highlights -- the sunlit clouds, the shafts -- and not from the whole frame.
    if (g_param0 > 0.0f) {
        const float threshold = g_param0;
        const float knee = max(g_param1, 1e-4f);
        const float brightness = max(color.r, max(color.g, color.b));
        float soft = clamp(brightness - threshold + knee, 0.0f, 2.0f * knee);
        soft = soft * soft / (4.0f * knee);
        const float contribution = max(soft, brightness - threshold) / max(brightness, 1e-4f);
        color *= contribution;
    }
    return float4(color, 1.0f);
}

// The 3x3 tent filter, spread by the radius, used to grow each mip back up as it is
// summed into the one above it.
float3 Upsample(float2 uv) {
    const Texture2D<float4> g_source = ResourceDescriptorHeap[g_source_index];
    const float2 t = g_src_texel * g_param0;
    const float3 a = g_source.SampleLevel(g_sampler, uv + t * float2(-1, 1), 0).rgb;
    const float3 b = g_source.SampleLevel(g_sampler, uv + t * float2(0, 1), 0).rgb;
    const float3 c = g_source.SampleLevel(g_sampler, uv + t * float2(1, 1), 0).rgb;
    const float3 d = g_source.SampleLevel(g_sampler, uv + t * float2(-1, 0), 0).rgb;
    const float3 e = g_source.SampleLevel(g_sampler, uv, 0).rgb;
    const float3 f = g_source.SampleLevel(g_sampler, uv + t * float2(1, 0), 0).rgb;
    const float3 g = g_source.SampleLevel(g_sampler, uv + t * float2(-1, -1), 0).rgb;
    const float3 h = g_source.SampleLevel(g_sampler, uv + t * float2(0, -1), 0).rgb;
    const float3 i = g_source.SampleLevel(g_sampler, uv + t * float2(1, -1), 0).rgb;
    return (e * 4.0f + (b + d + f + h) * 2.0f + (a + c + g + i)) * (1.0f / 16.0f);
}

float4 PSUpsample(VSOutput input) : SV_TARGET {
    // The additive blend in the PSO sums this onto the larger mip already there.
    return float4(Upsample(input.uv), 1.0f);
}
