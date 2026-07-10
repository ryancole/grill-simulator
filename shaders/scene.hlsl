// One draw per prop. Everything the draw needs arrives in root constants, so
// there are no descriptor heaps and no constant buffer resources at all.
//
// DirectXMath is row-major and treats vectors as rows, so the matrices are
// declared row_major and multiplied as `v * M`. That saves a transpose on the
// CPU and keeps the two sides reading the same way.
cbuffer Constants : register(b0) {
    row_major float4x4 g_mvp;
    row_major float4x4 g_model;
    float3 g_albedo;
    // Size of one checkerboard tile in metres; zero leaves the surface flat.
    float g_checker;
};

// The sun is south of the yard, over the player's shoulder at the spawn point,
// so the faces they are looking at are the lit ones.
static const float3 kSunDirection = normalize(float3(0.35f, 0.78f, -0.5f));
static const float3 kSunColor = float3(1.0f, 0.96f, 0.88f);
static const float3 kSkyColor = float3(0.52f, 0.62f, 0.76f);
static const float3 kGroundBounce = float3(0.20f, 0.18f, 0.16f);
// Without a shadow term the only thing keeping unlit faces off pure black is
// the ambient, so it carries more weight here than it would with real bounce.
static const float kAmbientStrength = 0.65f;
// A dim light from behind the sun. Nothing in the world casts it -- it exists so
// that a wall turned away from the sun reads as brown rather than as a hole.
static const float kFillStrength = 0.18f;

// Matched to the fence, 12 m out: the yard stays crisp and the grass beyond it
// fades, which is the only cue the player has that the world keeps going.
static const float kFogStart = 20.0f;
static const float kFogEnd = 90.0f;

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float view_depth : TEXCOORD0;
};

PSInput VSMain(VSInput input) {
    const float4 position = float4(input.position, 1.0f);

    PSInput output;
    output.position = mul(position, g_mvp);
    output.world = mul(position, g_model).xyz;
    // Every prop transform is an axis-aligned scale, then a yaw, then a
    // translation. A box's normals are axis aligned, so that combination only
    // changes their length -- no inverse transpose is needed to fix them up.
    output.normal = mul(float4(input.normal, 0.0f), g_model).xyz;
    // Under a left-handed perspective projection the clip-space w is the
    // view-space depth, which is what the fog wants.
    output.view_depth = output.position.w;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    const float3 normal = normalize(input.normal);

    float3 albedo = g_albedo;
    if (g_checker > 0.0f) {
        const float2 cell = floor(input.world.xz / g_checker);
        const float tile = frac((cell.x + cell.y) * 0.5f) * 2.0f; // 0 or 1
        albedo *= lerp(0.84f, 1.0f, tile);
    }

    // A hemisphere ambient term stands in for bounced light: sky above, dirt
    // below. Surfaces facing up read cool, undersides read warm and dark.
    const float3 ambient =
        lerp(kGroundBounce, kSkyColor, saturate(normal.y * 0.5f + 0.5f)) * kAmbientStrength;
    const float sun = saturate(dot(normal, kSunDirection));
    const float fill = saturate(dot(normal, -kSunDirection)) * kFillStrength;
    const float3 lit = albedo * (ambient + kSunColor * sun + kSkyColor * fill);

    const float fog = saturate((input.view_depth - kFogStart) / (kFogEnd - kFogStart));
    return float4(lerp(lit, kSkyColor, fog * 0.9f), 1.0f);
}
