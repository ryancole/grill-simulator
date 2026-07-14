// HUD text from a multi-channel signed-distance-field atlas.
//
// The CPU builds one quad per glyph with its corners already in normalized
// device coordinates, so the vertex shader is a pass-through: no projection
// matrix, no per-frame transform. The pixel shader does the real work -- it turns
// the sampled distance field into a crisp, resolution-independent edge.
cbuffer Constants : register(b0) {
    // Straight (non-premultiplied) text colour; .a scales the coverage below.
    float4 g_color;
    // kDistanceRange / atlas dimensions, in texels. Half of it, dotted with the
    // glyph's on-screen size in texels, recovers the distance field's width in
    // screen pixels -- the span the edge is antialiased across. Passing it in
    // spares the shader from knowing the atlas size or the text's on-screen scale.
    float2 g_unit_range;
    float2 g_padding;
    // >0.5 fills the quad flat with g_color and ignores the atlas -- the translucent
    // panel drawn behind the debug overlay. 0 (the default) is the glyph path below.
    float g_solid;
};

Texture2D<float4> g_atlas : register(t0);
SamplerState g_sampler : register(s0);

struct VSInput {
    float2 position : POSITION; // Already in clip space; w is 1, z is 0.
    float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

PSInput VSMain(VSInput input) {
    PSInput output;
    output.position = float4(input.position, 0.0f, 1.0f);
    output.uv = input.uv;
    return output;
}

// The median of the three distance channels is the true signed distance; any one
// channel alone loses the sharp corners the multi-channel encoding exists to keep.
float Median(float3 msd) {
    return max(min(msd.r, msd.g), min(max(msd.r, msd.g), msd.b));
}

float4 PSMain(PSInput input) : SV_TARGET {
    // A solid panel: the quad fills flat with the (translucent) colour, no atlas.
    if (g_solid > 0.5f) {
        return g_color;
    }

    const float3 msd = g_atlas.Sample(g_sampler, input.uv).rgb;
    const float distance = Median(msd) - 0.5f;

    // fwidth gives the texel span one screen pixel covers, so this is the field's
    // width in screen pixels here -- larger text stretches the field across more
    // pixels and softens the edge by exactly the right amount. Clamped to 1 so
    // text a long way off never thins below a single pixel and shimmers.
    const float2 screen_texels = 1.0f / fwidth(input.uv);
    const float screen_px_range = max(0.5f * dot(g_unit_range, screen_texels), 1.0f);

    const float coverage = saturate(distance * screen_px_range + 0.5f);
    return float4(g_color.rgb, g_color.a * coverage);
}
