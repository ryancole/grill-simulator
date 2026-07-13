// One draw per primitive. Everything the draw needs arrives in root constants
// except the base colour texture, which arrives in a one-entry descriptor table.
//
// DirectXMath is row-major and treats vectors as rows, so the matrices are
// declared row_major and multiplied as `v * M`. That saves a transpose on the
// CPU and keeps the two sides reading the same way.
#include "common.hlsli"

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
    // The glTF metallic-roughness factors, each scaling its channel of the
    // metallic-roughness map. Padded out to a full row to match the C++ mirror.
    float g_metallic;
    float g_roughness;
    float g_pad0;
    float g_pad1;
};

// Per-frame, shared by every scene draw: the sun's world-to-clip matrix, the one
// the shadow lookup projects a receiver into. A constant buffer rather than root
// constants because it never changes within a frame and the root-constant budget
// is nearly full.
cbuffer FrameConstants : register(b1) {
    row_major float4x4 g_light_view_projection;
    // The eye, in world space, for the view vector the specular term needs.
    float3 g_camera_position;
    float g_frame_pad0;
};

// A material with no texture of its own is pointed at a 1x1 white texel, so
// there is no branch here and no second pipeline state.
Texture2D<float4> g_base_color : register(t0);
// The sun's depth buffer from the shadow pass: one channel, the light-space depth
// of the nearest caster at each texel.
Texture2D<float> g_shadow_map : register(t1);
// The material's tangent-space normal map. A material with none is pointed at a
// flat 1x1 (0,0,1), so there is no branch and no second pipeline state -- exactly
// as the base colour handles a textureless material with a 1x1 white.
Texture2D<float4> g_normal_map : register(t2);
// The metallic-roughness map: glTF packs roughness in G and metallic in B. A
// material with none samples a 1x1 white, so the factors above stand alone.
Texture2D<float4> g_metallic_roughness : register(t3);
// The reflection probe: the yard captured into a cubemap once at startup, so a
// metal reflects the fence and the trees, not just the analytic sky. Sampled by
// PSMain; PSMainCapture, which fills this very cube, must not read it, so the
// sample is behind a compile-time flag that folds away for the capture variant.
TextureCube<float4> g_reflection_probe : register(t4);
// The ambient-occlusion map: glTF stores how exposed each texel is to ambient
// light in the R channel. A material with none samples a 1x1 white, so nothing is
// occluded. It is linear data, not colour, so it is sampled without sRGB decode.
Texture2D<float4> g_occlusion : register(t5);
SamplerState g_sampler : register(s0);
// Hardware PCF. SampleCmp compares the receiver's depth against the stored one
// and bilinearly filters the 0/1 results, so a single tap already softens across
// four texels; the 3x3 grid below widens that to a gentle penumbra.
SamplerComparisonState g_shadow_sampler : register(s1);

static const float3 kSunColor = float3(1.0f, 0.96f, 0.88f);
// The sky tone the diffuse ambient uses. The specular reflection and the
// background use the zenith/horizon gradient in common.hlsli; this one flat value
// stands in for the sky's cosine-convolved irradiance.
static const float3 kSkyColor = float3(0.52f, 0.62f, 0.76f);
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
    // xyz is the surface tangent; w is the handedness of the bitangent.
    float4 tangent : TANGENT;
};

struct PSInput {
    float4 position : SV_POSITION;
    float3 world : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float view_depth : TEXCOORD1;
    // World-space tangent; w carries the handedness through untouched.
    float4 tangent : TANGENT;
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
    // A tangent is carried by the model matrix itself, not its inverse transpose:
    // it lies in the surface and stretches with it, where the normal stays
    // perpendicular. The handedness in w rides along unchanged.
    output.tangent = float4(mul(input.tangent.xyz, (float3x3)g_model), input.tangent.w);
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

static const float kPi = 3.14159265f;
// The dielectric reflectance at normal incidence: about 4% for everything that is
// not a metal. Metals have no such fixed value -- their F0 is their base colour.
static const float3 kDielectricF0 = float3(0.04f, 0.04f, 0.04f);

// The sun's radiance. The diffuse BRDF carries a 1/pi that a real light's
// intensity would swallow; folding a pi back into the sun here keeps a lit matte
// surface about as bright as it was before the BRDF, and gives the speculars room
// to punch above it.
static const float kSunIntensity = kPi;

// The GGX/Trowbridge-Reitz normal distribution: how much of the surface's
// microfacets face exactly the half vector. `a` is the roughness squared.
float DistributionGGX(float n_dot_h, float a) {
    const float a2 = a * a;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
    return a2 / max(kPi * d * d, 1e-7f);
}

// Height-correlated Smith visibility: the geometry (self-shadowing/masking) term
// with the specular denominator 1 / (4 n.l n.v) already folded in, so the caller
// multiplies D * Vis * F and nothing else.
float VisibilitySmith(float n_dot_v, float n_dot_l, float a) {
    const float a2 = a * a;
    const float v = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0f - a2) + a2);
    const float l = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0f - a2) + a2);
    return 0.5f / max(v + l, 1e-5f);
}

// Schlick's approximation of Fresnel: reflectance climbing from f0 at face-on to
// white at grazing.
float3 FresnelSchlick(float v_dot_h, float3 f0) {
    return f0 + (1.0f - f0) * pow(saturate(1.0f - v_dot_h), 5.0f);
}

// The sky's rough average -- what a fully blurred reflection tends toward, and the
// floor the probe reflection fades into as roughness climbs.
float3 SkyAverage() {
    return lerp(SrgbToLinear(kSkyHorizon), SrgbToLinear(kSkyZenith), 0.5f);
}

// The prefiltered analytic environment: a rough surface reflects a blurred sky,
// which the real split-sum approximation bakes into mip levels of a cubemap. With
// an analytic sky there is nothing to prebake, so the blur is faked by fading the
// sharp reflection toward the sky's rough average as roughness climbs.
float3 PrefilteredSky(float3 r, float roughness) {
    return lerp(SampleSky(r), SkyAverage(), roughness);
}

// The prefiltered *environment* along a reflection vector: the captured cubemap
// when `use_probe`, so a metal reflects the real yard, and the analytic sky
// otherwise -- the path the capture pass itself takes, which is why it must not
// read the cube it is filling. The single-mip cube cannot blur, so roughness fades
// its reflection toward the sky average, the same floor PrefilteredSky uses.
float3 SpecularEnvironment(float3 r, float roughness, bool use_probe) {
    if (use_probe) {
        const float3 sharp = g_reflection_probe.SampleLevel(g_sampler, r, 0.0f).rgb;
        return lerp(sharp, SkyAverage(), roughness);
    }
    return PrefilteredSky(r, roughness);
}

// The environment half of the split-sum: the integral of the specular BRDF over
// the hemisphere, as a scale and bias on f0. Karis's analytic fit, so no
// precomputed BRDF lookup texture is needed.
float2 EnvBRDFApprox(float roughness, float n_dot_v) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * n_dot_v)) * r.x + r.y;
    return float2(-1.04f, 1.04f) * a004 + r.zw;
}

// The whole surface shade, in linear light. `use_probe` picks the specular
// environment: the captured cubemap for the on-screen pass, the analytic sky for
// the capture pass that fills that cube. It is a compile-time literal at each
// entry point below, so the cube sample folds away entirely for the capture
// variant and no feedback is possible.
float4 ShadeScene(PSInput input, bool use_probe) {
    // Rebuild the tangent frame and perturb the geometric normal by the map. The
    // tangent is re-orthogonalized against the interpolated normal (Gram-Schmidt),
    // which absorbs the small drift interpolation leaves between them; the
    // bitangent's handedness comes from the w the vertex carried. A flat (0,0,1)
    // sample -- what a material with no map reads -- leaves the normal untouched.
    const float3 geometric_normal = normalize(input.normal);
    const float3 tangent =
        normalize(input.tangent.xyz - geometric_normal * dot(geometric_normal, input.tangent.xyz));
    const float3 bitangent = cross(geometric_normal, tangent) * input.tangent.w;
    const float3 tangent_normal = g_normal_map.Sample(g_sampler, input.uv).xyz * 2.0f - 1.0f;
    const float3 normal = normalize(tangent_normal.x * tangent + tangent_normal.y * bitangent +
                                    tangent_normal.z * geometric_normal);

    // The base colour, in linear light: the texture is decoded by its sRGB view,
    // the flat factor/tint in g_albedo is decoded here.
    float3 base_color = SrgbToLinear(g_albedo) * g_base_color.Sample(g_sampler, input.uv).rgb;
    if (g_checker > 0.0f) {
        const float2 cell = floor(input.world.xz / g_checker);
        const float tile = frac((cell.x + cell.y) * 0.5f) * 2.0f; // 0 or 1
        base_color *= lerp(0.84f, 1.0f, tile);
    }

    // glTF packs roughness in G and metallic in B; the factors scale each. A floor
    // on roughness keeps the specular lobe from collapsing to a point the size of
    // one pixel, which aliases into a crawling sparkle as the camera moves.
    const float2 mr = g_metallic_roughness.Sample(g_sampler, input.uv).gb;
    const float roughness = clamp(g_roughness * mr.x, 0.045f, 1.0f);
    const float metallic = saturate(g_metallic * mr.y);
    const float a = roughness * roughness;

    // Metals have no diffuse and reflect their own colour; dielectrics diffuse
    // their base colour and reflect a dim white.
    const float3 diffuse_color = base_color * (1.0f - metallic);
    const float3 f0 = lerp(kDielectricF0, base_color, metallic);

    const float3 view = normalize(g_camera_position - input.world);
    const float n_dot_v = saturate(dot(normal, view)) + 1e-5f;

    // The direct sun, through the Cook-Torrance specular BRDF plus a Lambert
    // diffuse. Only the sun is shadowed; the branch is on a root constant, uniform
    // across the draw, so it is coherent and free.
    const float3 half_vector = normalize(view + g_sun_direction);
    const float n_dot_l = saturate(dot(normal, g_sun_direction));
    const float n_dot_h = saturate(dot(normal, half_vector));
    const float v_dot_h = saturate(dot(view, half_vector));

    const float3 fresnel = FresnelSchlick(v_dot_h, f0);
    const float3 specular =
        DistributionGGX(n_dot_h, a) * VisibilitySmith(n_dot_v, n_dot_l, a) * fresnel;
    // Energy the specular reflection did not take is left for the diffuse.
    const float3 diffuse = (1.0f - fresnel) * diffuse_color * (1.0f / kPi);

    float shadow = 1.0f;
    if (g_shadow_receive > 0.5f) {
        shadow = SunVisibility(input.world, normal);
    }
    const float3 sun =
        (diffuse + specular) * n_dot_l * SrgbToLinear(kSunColor) * kSunIntensity * shadow;

    // Image-based ambient, split into its two halves. The diffuse half is a
    // hemisphere term standing in for the sky's cosine-convolved irradiance: sky
    // above, dirt below, lighting the diffuse albedo.
    const float3 irradiance =
        lerp(SrgbToLinear(kGroundBounce), SrgbToLinear(kSkyColor),
             saturate(normal.y * 0.5f + 0.5f)) * kAmbientStrength;
    const float3 ambient_diffuse = irradiance * diffuse_color;

    // The specular half reflects the sky itself: sampled along the reflection
    // vector so it swings with the view, blurred by roughness, and weighted by the
    // split-sum environment BRDF -- which brightens the rim at grazing angles and
    // tints the reflection by f0. This is what makes a metal read as metal even
    // where the sun is not glinting off it.
    const float3 reflection = reflect(-view, normal);
    const float2 env_brdf = EnvBRDFApprox(roughness, n_dot_v);
    const float3 ambient_specular =
        SpecularEnvironment(reflection, roughness, use_probe) * (f0 * env_brdf.x + env_brdf.y);

    // Ambient occlusion darkens only the indirect light -- the sky's diffuse and
    // its reflection -- in the crevices and contact points the map records. The
    // direct sun is untouched, since a surface in a crevice is still lit if the sun
    // reaches it. A material with no map reads 1 and nothing changes.
    const float ao = g_occlusion.Sample(g_sampler, input.uv).r;
    const float3 ambient = (ambient_diffuse + ambient_specular) * ao;

    // A dim fill from behind the sun, diffuse only, so faces turned away read brown
    // rather than as holes.
    const float3 fill = SrgbToLinear(kSkyColor) * saturate(dot(normal, -g_sun_direction)) *
                        kFillStrength * diffuse_color;

    const float3 lit = ambient + sun + fill;

    // Fade into the very sky drawn behind this pixel, sampled along the view ray,
    // so distant geometry dissolves into the gradient rather than into a flat tone
    // that would seam against it at the horizon.
    const float3 view_ray = normalize(input.world - g_camera_position);
    const float fog = saturate((input.view_depth - kFogStart) / (kFogEnd - kFogStart));
    const float3 color = lerp(lit, SampleSky(view_ray), fog * 0.9f);
    // Left in linear light: the whole world renders into the HDR scene buffer,
    // and the tonemap pass (tonemap.hlsl) is the one place the linear->sRGB encode
    // happens. The capture path below, which writes an 8-bit cube instead, encodes
    // for itself.
    return float4(color, 1.0f);
}

// The on-screen pass: reflect the captured probe. Its target is the linear HDR
// scene buffer, so the shade is written as-is and the tonemap pass encodes it.
float4 PSMain(PSInput input) : SV_TARGET {
    return ShadeScene(input, true);
}

// The capture pass that fills the probe: reflect the analytic sky instead, so it
// never reads the cube it is writing. Its target is the _UNORM probe cube, sampled
// back through an sRGB view, so the linear shade is encoded here.
float4 PSMainCapture(PSInput input) : SV_TARGET {
    const float4 shade = ShadeScene(input, false);
    return float4(LinearToSrgb(shade.rgb), shade.a);
}
