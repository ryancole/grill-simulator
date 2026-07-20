// The lighter-fluid spray, pass 2: a separable bilateral blur of the surface depth. The
// impostor pass leaves a bumpy field of sphere fronts; smoothing it melts a tight cluster
// of beads into one continuous liquid surface, while the bilateral weighting keeps the
// silhouette crisp -- taps whose depth differs from the centre by more than a threshold
// (a different droplet cluster, or empty space) are rejected, so distinct blobs do not
// bleed into one another.
//
// Run twice, horizontal then vertical (g_texel_step carries the axis and pixel size),
// ping-ponging between the depth and the blur target. Empty pixels -- those at the far
// sentinel the impostor pass cleared to -- stay empty.

cbuffer FluidBlurConstants : register(b0) {
    // One texel along the blur axis: (1/width, 0) horizontal, (0, 1/height) vertical.
    float2 g_texel_step;
    // The "no fluid here" value the depth target was cleared to; anything at or above half
    // of it is empty. Depths nearer than a whole cluster stay well below it.
    float g_sentinel;
    // The largest depth gap, in metres, a tap may differ from the centre and still be
    // blended in: wide enough to merge neighbouring beads, tight enough to keep separate
    // clusters (and the silhouette against empty space) apart.
    float g_depth_threshold;
    // The slot of the depth to smooth in the one bound heap, fetched bindlessly rather
    // than through a table -- it ping-pongs between the two fluid depth targets.
    uint g_depth_index;
};

SamplerState g_sampler : register(s0);

// The blur half-width in taps, and the spatial falloff. A radius of eight is plenty to
// close the gaps between centimetre beads at these draw sizes.
static const int kRadius = 8;
static const float kSigma = 4.0;

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vertex_id : SV_VertexID) {
    // A single triangle that covers the screen, from the vertex id.
    VSOut output;
    output.uv = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float PSMain(VSOut input) : SV_Target {
    const Texture2D<float> g_depth = ResourceDescriptorHeap[g_depth_index];
    const float center = g_depth.SampleLevel(g_sampler, input.uv, 0);
    // Empty stays empty: no surface to smooth, and blending would drag the sentinel in.
    if (center >= g_sentinel * 0.5) {
        return g_sentinel;
    }

    float sum = center;  // the centre tap, weight 1
    float weight = 1.0;
    for (int i = -kRadius; i <= kRadius; ++i) {
        if (i == 0) {
            continue;
        }
        const float2 uv = input.uv + g_texel_step * (float)i;
        const float tap = g_depth.SampleLevel(g_sampler, uv, 0);
        // Reject empty taps and taps across a depth discontinuity (a different cluster).
        if (tap >= g_sentinel * 0.5 || abs(tap - center) > g_depth_threshold) {
            continue;
        }
        const float w = exp(-(float)(i * i) / (2.0 * kSigma * kSigma));
        sum += tap * w;
        weight += w;
    }
    return sum / weight;
}
