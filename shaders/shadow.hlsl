// The shadow pass: depth only, from the sun's point of view. Every caster in the
// yard is drawn once into a depth buffer that records, at each texel, how far the
// nearest surface is from the sun. scene.hlsl then reads that buffer back to ask,
// for each lit pixel, whether something stood between it and the sun.
//
// There is no pixel shader -- the rasterizer writes depth on its own -- so this
// file is a vertex shader and nothing else. Row-major, vectors as rows, `v * M`,
// the same convention as the rest of the renderer.
cbuffer ShadowConstants : register(b0) {
    // Model space straight to the sun's clip space: the caster's world transform
    // folded into the sun's orthographic view-projection on the CPU.
    row_major float4x4 g_light_mvp;
};

// Only the position is read. The scene's vertex buffer carries a normal and a UV
// after it, but the input layout for this pass names position alone, so the rest
// is never fetched.
float4 VSMain(float3 position : POSITION) : SV_POSITION {
    return mul(float4(position, 1.0f), g_light_mvp);
}
