// One draw per primitive. Everything the draw needs arrives in root constants
// except the base colour texture, which arrives in a one-entry descriptor table.
//
// DirectXMath is row-major and treats vectors as rows, so the matrices are
// declared row_major and multiplied as `v * M`. That saves a transpose on the
// CPU and keeps the two sides reading the same way.
cbuffer Constants : register(b0) {
    row_major float4x4 g_mvp;
    row_major float4x4 g_model;
    // The rows of transpose(inverse(g_model)), which is what carries a normal
    // from model space to world space when the model matrix scales unevenly.
    // Only .xyz of each is read; w is packing. Spelling this as three float4s
    // rather than a float3x3 makes the C++ mirror's layout unambiguous.
    float4 g_normal_rows[3];
    // Multiplies the base colour texture. Both the glTF material's factor and
    // the instance's tint are folded into it on the CPU.
    float3 g_albedo;
    // Size of one checkerboard tile in metres; zero leaves the surface flat.
    float g_checker;
    // Unit vector pointing at the sun, in world space, already normalized.
    // The yard passes the real one; the viewmodel passes a sun of its own that
    // rides along with the camera, so the player's arms are lit the same way no
    // matter which way they turn.
    float3 g_sun_direction;
    float g_padding;
};

// A material with no texture of its own is pointed at a 1x1 white texel, so
// there is no branch here and no second pipeline state.
Texture2D<float4> g_base_color : register(t0);
SamplerState g_sampler : register(s0);

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
    float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float view_depth : TEXCOORD1;
};

PSInput VSMain(VSInput input) {
    const float4 position = float4(input.position, 1.0f);

    PSInput output;
    output.position = mul(position, g_mvp);
    output.world = mul(position, g_model).xyz;
    // Rows, because `mul(v, M)` treats v as a row and M's rows as its basis.
    const float3x3 normal_matrix =
        float3x3(g_normal_rows[0].xyz, g_normal_rows[1].xyz, g_normal_rows[2].xyz);
    output.normal = mul(input.normal, normal_matrix);
    output.uv = input.uv;
    // Under a left-handed perspective projection the clip-space w is the
    // view-space depth, which is what the fog wants.
    output.view_depth = output.position.w;
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    const float3 normal = normalize(input.normal);

    float3 albedo = g_albedo * g_base_color.Sample(g_sampler, input.uv).rgb;
    if (g_checker > 0.0f) {
        const float2 cell = floor(input.world.xz / g_checker);
        const float tile = frac((cell.x + cell.y) * 0.5f) * 2.0f; // 0 or 1
        albedo *= lerp(0.84f, 1.0f, tile);
    }

    // A hemisphere ambient term stands in for bounced light: sky above, dirt
    // below. Surfaces facing up read cool, undersides read warm and dark.
    const float3 ambient =
        lerp(kGroundBounce, kSkyColor, saturate(normal.y * 0.5f + 0.5f)) * kAmbientStrength;
    const float sun = saturate(dot(normal, g_sun_direction));
    const float fill = saturate(dot(normal, -g_sun_direction)) * kFillStrength;
    const float3 lit = albedo * (ambient + kSunColor * sun + kSkyColor * fill);

    const float fog = saturate((input.view_depth - kFogStart) / (kFogEnd - kFogStart));
    return float4(lerp(lit, kSkyColor, fog * 0.9f), 1.0f);
}
