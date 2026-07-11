// The glowing rim drawn around the loose object the player is looking at, so it
// is clear which one an E press would grab. It is the classic inverted-hull
// outline: the same mesh is drawn a second time, each vertex pushed out along
// its normal, so the enlarged copy pokes out past the real silhouette as a
// border. The pixel shader paints that border a flat, pulsing colour.
//
// The pass runs with depth testing OFF so the halo is not clipped by whatever
// ground happens to sit between the object and the eye -- that clipping is what
// made an earlier depth-tested version glow only on the faces turned away from
// the player. Drawing over the object is then undone by re-painting the object
// on top, so only the ring outside its silhouette survives.
//
// Like scene.hlsl this is row-major, vectors as rows, multiplied `v * M`.
cbuffer OutlineConstants : register(b0) {
    // View-projection on its own: the copy is grown in world space first, then
    // projected, so the rim keeps an even thickness around the object rather
    // than the uneven one a model-space scale about the origin would give.
    row_major float4x4 g_view_projection;
    row_major float4x4 g_model;
    // Rows of transpose(inverse(g_model)); only .xyz is read. Carries the normal
    // into world space so the push-out is along the true surface normal even
    // when the model matrix scales unevenly. Same trick as scene.hlsl.
    float4 g_normal_rows[3];
    // The rim colour, already pulsed on the CPU. .a is the layer's strength.
    float4 g_color;
    // How far, in metres of world space, each vertex is pushed out along its
    // normal. The wider this is, the further the halo reaches.
    float g_width;
};

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
};

PSInput VSMain(VSInput input) {
    float4 world = mul(float4(input.position, 1.0f), g_model);

    const float3x3 normal_matrix =
        float3x3(g_normal_rows[0].xyz, g_normal_rows[1].xyz, g_normal_rows[2].xyz);
    const float3 normal = normalize(mul(input.normal, normal_matrix));
    world.xyz += normal * g_width;

    PSInput output;
    output.position = mul(world, g_view_projection);
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    return g_color;
}
