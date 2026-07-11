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
    // 1 for surfaces that receive the sun's shadow -- the whole yard -- and 0 for
    // the viewmodel, which is lit by a camera-bolted sun the shadow map knows
    // nothing about. Kept a float so it folds into the arithmetic without a cast.
    float g_shadow_receive;
};

// Per-frame, shared by every scene draw: the sun's world-to-clip matrix, the one
// the shadow lookup projects a receiver into. A constant buffer rather than root
// constants because it never changes within a frame and the root-constant budget
// is nearly full.
cbuffer FrameConstants : register(b1) {
    row_major float4x4 g_light_view_projection;
};

// A material with no texture of its own is pointed at a 1x1 white texel, so
// there is no branch here and no second pipeline state.
Texture2D<float4> g_base_color : register(t0);
// The sun's depth buffer from the shadow pass: one channel, the light-space depth
// of the nearest caster at each texel.
Texture2D<float> g_shadow_map : register(t1);
SamplerState g_sampler : register(s0);
// Hardware PCF. SampleCmp compares the receiver's depth against the stored one
// and bilinearly filters the 0/1 results, so a single tap already softens across
// four texels; the 3x3 grid below widens that to a gentle penumbra.
SamplerComparisonState g_shadow_sampler : register(s1);

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

// The shadow map is square; these size one texel of it. Must match the resource
// the renderer creates -- see kShadowMapSize in renderer.cpp.
static const float kShadowMapSize = 2048.0f;
static const float kShadowTexel = 1.0f / kShadowMapSize;
// Before the depth compare, slide the receiver this far along its own normal, in
// metres. It lifts a surface off its own shadow, which is what kills the acne a
// flat depth bias leaves on faces grazing the sun -- a couple of shadow texels'
// worth here. Too large and thin objects start to float free of their shadows.
static const float kNormalOffset = 0.035f;
// A last small nudge in light-clip depth, on top of the rasterizer's slope-scaled
// bias, for the acne the normal offset does not catch.
static const float kDepthBias = 0.0006f;

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

// The fraction of the sun that reaches `world`: 1 fully lit, 0 fully in shadow.
// Projects the receiver into the sun's view and 3x3-PCF-compares its depth
// against the shadow map. The yard is smaller than the sun's frame, so anything
// falling outside the map -- the far grass -- reads as lit rather than clamped
// dark at the border.
float SunVisibility(float3 world, float3 normal) {
    const float4 light_clip =
        mul(float4(world + normal * kNormalOffset, 1.0f), g_light_view_projection);
    // Orthographic, so w is 1; the divide is a formality kept for clarity.
    const float3 ndc = light_clip.xyz / light_clip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (ndc.z > 1.0f || any(uv < 0.0f) || any(uv > 1.0f)) {
        return 1.0f;
    }

    const float depth = ndc.z - kDepthBias;
    float lit = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 tap = uv + float2(x, y) * kShadowTexel;
            lit += g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, tap, depth);
        }
    }
    return lit / 9.0f;
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
    // Only the direct sun is shadowed; the ambient and the fill stand in for
    // bounced light and keep unlit faces off pure black. The branch is on a root
    // constant, uniform across the draw, so it is coherent and free -- and it
    // spares the viewmodel a shadow lookup it would only throw away.
    float shadow = 1.0f;
    if (g_shadow_receive > 0.5f) {
        shadow = SunVisibility(input.world, normal);
    }
    const float fill = saturate(dot(normal, -g_sun_direction)) * kFillStrength;
    const float3 lit = albedo * (ambient + kSunColor * sun * shadow + kSkyColor * fill);

    const float fog = saturate((input.view_depth - kFogStart) / (kFogEnd - kFogStart));
    return float4(lerp(lit, kSkyColor, fog * 0.9f), 1.0f);
}
