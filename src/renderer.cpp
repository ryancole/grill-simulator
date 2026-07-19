#include "renderer.hpp"

#include "image.hpp"

#include <d3dx12/d3dx12.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// The scene depth buffer is typeless so it can be read two ways, exactly as the
// shadow map is: a D32 depth view to render into, and an R32_FLOAT resource view
// the light-shaft pass samples to know how far to march each view ray.
constexpr DXGI_FORMAT kDepthResourceFormat = DXGI_FORMAT_R32_TYPELESS;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kDepthSrvFormat = DXGI_FORMAT_R32_FLOAT;

// The HDR scene buffer the whole world renders into, in linear light: a 16-bit
// float per channel, enough range to hold values above 1 for the tonemapper to
// pull back and enough precision to keep the darks from banding.
constexpr DXGI_FORMAT kHdrFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
// The HDR target's render-target view sits just past the swapchain's own RTVs in
// rtv_heap_; the bloom mips' views follow it.
constexpr UINT kHdrRtvIndex = Renderer::kFrameCount;
constexpr UINT kBloomRtvBase = Renderer::kFrameCount + 1;
// The screen-space fluid's three render targets sit past the bloom mips in rtv_heap_:
// the surface depth, its blurred copy, and the thickness.
constexpr UINT kFluidRtvBase = Renderer::kFrameCount + 1 + Renderer::kBloomLevels;

// The screen-space fluid's offscreen formats: a single-channel float for the surface
// depth (view-space z) and its blur, and a half float for the accumulated thickness.
constexpr DXGI_FORMAT kFluidDepthFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kFluidThicknessFormat = DXGI_FORMAT_R16_FLOAT;
// The "no fluid here" depth the surface target is cleared to: far past any real droplet,
// so a MIN blend keeps the nearest actual surface and the blur/composite can tell empty
// pixels apart. Shared with the shaders (their g_sentinel).
constexpr float kFluidSentinel = 1.0e5f;

// The persistent engine SRV heap and its slots. Slot 0 is the HDR buffer the
// tonemap reads; 1 and 2 are the scene depth and a plain (non-comparison) view of
// the shadow map, the two the light-shaft pass marches; then one per bloom mip
// (Renderer::kBloomLevels of them, half the window and down); then the fluid's
// surface depth, its blur, and its thickness.
constexpr UINT kHdrSrvIndex = 0;
constexpr UINT kDepthSrvIndex = 1;
constexpr UINT kShaftShadowSrvIndex = 2;
constexpr UINT kBloomSrvBase = 3;
// Depth and thickness are adjacent so the composite reads them as one contiguous table;
// the blur intermediate follows. The separable blur ends with the smoothed depth back in
// fluid_depth_ (slot 9), which is what the composite samples.
constexpr UINT kFluidDepthSrvIndex = kBloomSrvBase + Renderer::kBloomLevels;      // 9
constexpr UINT kFluidThicknessSrvIndex = kFluidDepthSrvIndex + 1;                 // 10
constexpr UINT kFluidDepthBlurSrvIndex = kFluidDepthSrvIndex + 2;                 // 11
// The depth gap (metres) a blur tap may cross before it is rejected as a different cluster:
// a few droplet radii, wide enough to merge a tight stream, tight enough to keep the
// silhouette. Shared with fluid_blur.hlsl's g_depth_threshold.
constexpr float kFluidBlurDepthThreshold = 0.08f;
// Sized past what is in use so the post-process buffers to come need no realloc.
constexpr UINT kEngineHeapSize = 16;
static_assert(kBloomSrvBase + Renderer::kBloomLevels <= kEngineHeapSize,
              "engine heap too small for bloom");
static_assert(kFluidThicknessSrvIndex < kEngineHeapSize, "engine heap too small for fluid");

// The sky the fog fades into, so the horizon has no seam.
constexpr float kSkyColor[] = {0.52f, 0.62f, 0.76f, 1.0f};

// The HDR buffer's clear colour: the sky tone in linear light. The sky pass fills
// every pixel with depth off, so this is only a formality, but a render target
// must declare a clear value and every clear must match it for the fast path.
constexpr float kHdrClearColor[] = {0.235f, 0.342f, 0.537f, 1.0f};

// The sun is south of the yard, over the player's shoulder at the spawn point,
// so the faces they are looking at are the lit ones.
constexpr XMFLOAT3 kSunDirection{0.35f, 0.78f, -0.5f};

// Mirrors the `Constants` cbuffer in shaders/scene.hlsl. Root constants live in
// the command list itself: at 208 bytes per draw there is nothing here worth the
// descriptor heap and the aliasing rules a real constant buffer would cost.
//
// HLSL packs a cbuffer into float4 rows and never straddles one, so `albedo` and
// `checker` share a row and `sun_direction` and its padding share the next --
// which is exactly the C++ layout below. Do not reorder either side alone.
struct Constants {
    XMFLOAT4X4 mvp;
    XMFLOAT4X4 model;
    // Rows of transpose(inverse(model)). Each occupies a full float4 row in the
    // cbuffer; the w components are never read.
    XMFLOAT4 normal_rows[3];
    XMFLOAT3 albedo;
    float checker;
    XMFLOAT3 sun_direction;
    // 1 for the world, which the sun's shadow map covers; 0 for the viewmodel,
    // lit by its own camera-bolted sun. Mirrors g_shadow_receive in scene.hlsl,
    // and reuses the DWORD that was only padding before shadows existed.
    float shadow_receive;
    // The glTF metallic-roughness factors, each multiplying its channel of the
    // metallic-roughness texture. They open a fresh cbuffer row, which `emissive`
    // then shares, leaving one DWORD of padding to keep the C++ size a whole
    // 16-byte multiple like the HLSL side.
    float metallic;
    float roughness;
    // Self-lit multiple of the base colour, added on top of the shading (see
    // MeshInstance::emissive). This rides the row's spare DWORD deliberately: the
    // signature is already at the 64-DWORD ceiling noted below, so a fresh row --
    // an emissive colour, say, rather than a strength on the tint -- would not fit
    // without merging the descriptor tables.
    float emissive;
    // How wet the surface looks, 0..1 (see MeshInstance::wetness). Rides the DWORD that
    // was only padding, so the wet sheen cost no growth in an already-full signature.
    float wetness;
};

static_assert(sizeof(Constants) % sizeof(UINT) == 0);
constexpr UINT kConstantDwords = sizeof(Constants) / sizeof(UINT);
// Beyond the root constants the scene signature holds the base-colour table (1),
// the frame constant buffer (2), the shadow-map table (1), the normal-map table
// (1), the metallic-roughness table (1), the reflection-probe table (1) and the
// occlusion table (1) -- eight DWORDs, which puts the signature exactly at the
// 64-DWORD ceiling. Another per-draw texture would need the tables merged into one
// contiguous range first.
static_assert(kConstantDwords + 8 <= 64, "A root signature holds at most 64 DWORDs in total");

// The shadow map is square and this many texels on a side. Matches kShadowMapSize
// in scene.hlsl, which sizes one texel for the PCF taps. 2048 gives the fenced
// yard a little over a centimetre per texel, crisp enough for hard-edged props.
constexpr UINT kShadowMapSize = 2048;
// The shadow depth view is the second descriptor in the DSV heap; the scene's own
// depth buffer is the first.
constexpr UINT kShadowDsvIndex = 1;
constexpr DXGI_FORMAT kShadowResourceFormat = DXGI_FORMAT_R32_TYPELESS;
constexpr DXGI_FORMAT kShadowDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kShadowSrvFormat = DXGI_FORMAT_R32_FLOAT;

// Mirrors the ShadowConstants cbuffer in shaders/shadow.hlsl: one matrix, the
// caster's model-to-light-clip transform.
struct ShadowConstants {
    XMFLOAT4X4 light_mvp;
};

constexpr UINT kShadowConstantDwords = sizeof(ShadowConstants) / sizeof(UINT);

// SkyEnvironment, Environment and kDefaultEnvironment now live in environment.hpp
// (via renderer.hpp), shared with the level parser that fills them. What stays here
// are the constant-buffer mirrors that embed a SkyEnvironment, and the
// ApplyEnvironment overloads that stamp an Environment into each.

// Mirrors the SkyConstants cbuffer in shaders/sky.hlsl: clip space back to world,
// the eye the view rays start from, and the level's sky.
struct SkyConstants {
    XMFLOAT4X4 inv_view_projection;
    XMFLOAT3 camera_position;
    // Seconds since start-up, drifting the cloud layer. Mirrors g_time in sky.hlsl.
    float time;
    SkyEnvironment sky;
};

static_assert(sizeof(SkyConstants) % sizeof(UINT) == 0);
constexpr UINT kSkyConstantDwords = sizeof(SkyConstants) / sizeof(UINT);

// Mirrors the FrameConstants cbuffer in shaders/scene.hlsl: state shared by every
// scene draw in a frame. The sun's view-projection never changes; the camera
// position does, which is why this buffer is now written per frame (see
// frame_constants_mapped_) rather than once at startup.
struct FrameConstants {
    XMFLOAT4X4 light_view_projection;
    XMFLOAT3 camera_position;
    // Seconds since start-up, drifting the cloud the fog fades into. Mirrors g_time
    // in scene.hlsl.
    float time;
    // The level's atmosphere, matching the tail of FrameConstants in scene.hlsl:
    // the sky SampleSky reads, then the sun, ambient, fill and fog the shade uses.
    SkyEnvironment sky;
    XMFLOAT3 sun_color;
    float sun_intensity;
    XMFLOAT3 sky_ambient;
    float ambient_strength;
    float fill_strength;
    float fog_start;
    float fog_end;
    float pad;
};

static_assert(sizeof(FrameConstants) == 208, "FrameConstants must mirror the HLSL cbuffer rows");

// Mirrors the LightShaftConstants cbuffer in shaders/lightshafts.hlsl: the inverse
// view-projection that rebuilds a pixel's world point from depth, the sun's view-
// projection the march samples the shadow map through, the eye, and the sun. Each
// float3 opens a fresh cbuffer row, so a pad DWORD follows it, matching the HLSL.
struct LightShaftConstants {
    XMFLOAT4X4 inv_view_projection;
    XMFLOAT4X4 light_view_projection;
    XMFLOAT3 camera_position;
    float pad0;
    XMFLOAT3 sun_direction;
    // The old second pad DWORD now carries the shaft intensity; the colour and
    // asymmetry follow in a fresh row. Matches LightShaftConstants in lightshafts.hlsl.
    float shaft_intensity;
    XMFLOAT3 shaft_color;
    float shaft_g;
};

static_assert(sizeof(LightShaftConstants) % sizeof(UINT) == 0);
constexpr UINT kLightShaftConstantDwords = sizeof(LightShaftConstants) / sizeof(UINT);
static_assert(kLightShaftConstantDwords + 1 <= 64,
              "A root signature holds at most 64 DWORDs in total");

// Mirrors the CloudConstants cbuffer in shaders/clouds.hlsl: the inverse view-
// projection that rebuilds each pixel's view ray, the eye and the clock, the sun and
// the light it scatters, the flat sky fill, and the slab's altitudes/density/detail.
// The shared SkyEnvironment tails it, carrying the coverage/scale/wind the horizontal
// shape reuses from the 2D layer. Each float3 opens a fresh cbuffer row and is
// followed by the scalar that shares it, matching the HLSL with no straddle.
struct CloudConstants {
    XMFLOAT4X4 inv_view_projection;
    XMFLOAT3 camera_position;
    float time;
    XMFLOAT3 sun_direction;
    float cloud_bottom;
    XMFLOAT3 sun_color;
    float cloud_top;
    XMFLOAT3 sky_ambient;
    float cloud_density;
    float sun_intensity;
    float ambient_strength;
    float cloud_detail;
    float pad;
    SkyEnvironment sky;
};

static_assert(sizeof(CloudConstants) % sizeof(UINT) == 0);
constexpr UINT kCloudConstantDwords = sizeof(CloudConstants) / sizeof(UINT);
static_assert(kCloudConstantDwords + 1 <= 64,
              "A root signature holds at most 64 DWORDs in total");

// Mirrors the FluidConstants cbuffer in shaders/fluid.hlsl: just the view and projection
// the impostor pass billboards and depths its droplets with. The lighting moved to the
// composite pass, so the surface pass carries only the transforms.
struct FluidConstants {
    XMFLOAT4X4 view;
    XMFLOAT4X4 proj;
};

static_assert(sizeof(FluidConstants) % sizeof(UINT) == 0);
constexpr UINT kFluidConstantDwords = sizeof(FluidConstants) / sizeof(UINT);
// The root constants plus the droplet buffer's root SRV (two DWORDs).
static_assert(kFluidConstantDwords + 2 <= 64,
              "A root signature holds at most 64 DWORDs in total");

// Mirrors the FluidBlurConstants cbuffer in shaders/fluid_blur.hlsl: the texel step along
// the blur axis, the empty-pixel sentinel, and the depth gap a tap may cross.
struct FluidBlurConstants {
    XMFLOAT2 texel_step;
    float sentinel;
    float depth_threshold;
};

static_assert(sizeof(FluidBlurConstants) % sizeof(UINT) == 0);
constexpr UINT kFluidBlurConstantDwords = sizeof(FluidBlurConstants) / sizeof(UINT);

// Mirrors the FluidCompositeConstants cbuffer in shaders/fluid_composite.hlsl: the
// projection scales and texel size that rebuild view position and normal from depth, the
// scene's sun/ambient (sRGB, with multipliers), the naphtha tint and glint sharpness, and
// the per-channel absorption. Each float3 opens a fresh cbuffer row followed by the scalar
// that shares it, matching the HLSL with no straddle.
struct FluidCompositeConstants {
    float proj00;
    float proj11;
    XMFLOAT2 texel;
    XMFLOAT3 sun_dir_view;
    float sentinel;
    XMFLOAT3 sun_color;
    float sun_intensity;
    XMFLOAT3 sky_ambient;
    float ambient_strength;
    XMFLOAT3 tint;
    float gloss;
    XMFLOAT3 absorption;
    float absorption_strength;
};

static_assert(sizeof(FluidCompositeConstants) % sizeof(UINT) == 0);
constexpr UINT kFluidCompositeConstantDwords = sizeof(FluidCompositeConstants) / sizeof(UINT);
static_assert(kFluidCompositeConstantDwords + 1 <= 64,
              "A root signature holds at most 64 DWORDs in total");

// The pale straw of naphtha the fluid's body takes (sRGB).
constexpr XMFLOAT3 kDropletTint{0.93f, 0.9f, 0.72f};

// How the fluid drinks light per channel (extinction, higher = more absorbed) and overall:
// blue is absorbed most, so a thick pool reads warm amber; thin spray barely tints. The
// sun-glint sharpness rounds out the composite's tuning.
constexpr XMFLOAT3 kFluidAbsorption{1.0f, 1.7f, 3.0f};
constexpr float kFluidAbsorptionStrength = 2.5f;
constexpr float kFluidGloss = 200.0f;

// The droplet pool's ceiling, matching Fluid's kMaxDroplets: the most impostor points the
// per-frame buffer must hold. A live spray never approaches this, but the buffer is sized
// for the worst case so an over-long hold never overruns it.
constexpr UINT kMaxSprayDroplets = 2048;

// Mirrors the GrassConstants cbuffer in shaders/grass.hlsl: the field's placement
// and per-blade parameters the mesh shader grows the grass from, plus the view-
// projection and the eye. Each float3 opens a fresh cbuffer row and is followed by
// the scalar that shares it, so the C++ rows match the HLSL with no straddle. The
// lighting the grass needs (sun colour, sky, fog) rides in the shared FrameConstants
// buffer, bound alongside, not here.
struct GrassConstants {
    XMFLOAT4X4 view_projection;
    XMFLOAT3 patch_origin;
    float cell_size;
    XMFLOAT3 camera_position;
    float time;
    XMFLOAT3 blade_color;
    float blade_height;
    XMFLOAT3 sun_direction;
    float blade_width;
    XMFLOAT2 wind;
    XMUINT2 grid;
    // Level-of-detail distances from the eye, in metres: (full within x, half within
    // y, quarter beyond, cull past z). Mirrors g_lod in grass.hlsl; w is padding.
    XMFLOAT4 lod;
};

static_assert(sizeof(GrassConstants) % sizeof(UINT) == 0);
constexpr UINT kGrassConstantDwords = sizeof(GrassConstants) / sizeof(UINT);
// The root constants plus the frame constant buffer and the obstacle buffer (two root
// CBVs, two DWORDs each) and the shadow-map table (one DWORD).
static_assert(kGrassConstantDwords + 5 <= 64, "A root signature holds at most 64 DWORDs in total");

// The world size of one grass cell -- one mesh-shader group's patch of ground. A
// group grows up to BLADES_PER_GROUP blades (see grass.hlsl), so this sets the peak
// density: a 0.28 m cell with 24 blades is about 300 blades per square metre up close,
// dense enough that the overlapping blades read as a full sward rather than spikes.
constexpr float kGrassCellSize = 0.28f;
// How many cells one amplification group culls, one thread each. Must match AS_GROUP
// in grass.hlsl, which sizes the payload the group hands to its mesh groups.
constexpr UINT kGrassAsGroup = 32;

// The most world footprints the grass keeps clear of at once. Must match
// MAX_GRASS_OBSTACLES in grass.hlsl.
constexpr UINT kMaxGrassObstacles = 64;

// Mirrors the GrassObstacles cbuffer in grass.hlsl: the count of live footprints,
// then a fixed array of XZ rectangles (min.x, min.z, max.x, max.z) the grass avoids.
// A constant buffer bound as a root CBV, so it needs no descriptor heap slot.
struct GrassObstaclesCB {
    UINT count;
    XMFLOAT3 pad;
    XMFLOAT4 boxes[kMaxGrassObstacles];
};

static_assert(sizeof(GrassObstaclesCB) % 16 == 0, "cbuffer must be a whole 16-byte multiple");

// Stamp one Environment into each pass's constant buffer. Each pass reads only the
// slice it needs -- the scene the whole lighting environment, the sky just the
// gradient and clouds, the shafts just their own three fields -- so a single source
// of truth reaches all three without any pass carrying fields it never samples.
void ApplyEnvironment(FrameConstants& frame, const Environment& env) {
    frame.sky = env.sky;
    frame.sun_color = env.sun_color;
    frame.sun_intensity = env.sun_intensity;
    frame.sky_ambient = env.sky_ambient;
    frame.ambient_strength = env.ambient_strength;
    frame.fill_strength = env.fill_strength;
    frame.fog_start = env.fog_start;
    frame.fog_end = env.fog_end;
}

void ApplyEnvironment(SkyConstants& sky, const Environment& env) { sky.sky = env.sky; }

void ApplyEnvironment(LightShaftConstants& shaft, const Environment& env) {
    shaft.shaft_color = env.shaft_color;
    shaft.shaft_intensity = env.shaft_intensity;
    shaft.shaft_g = env.shaft_g;
}

void ApplyEnvironment(CloudConstants& cloud, const Environment& env) {
    cloud.sky = env.sky;
    cloud.sun_color = env.sun_color;
    cloud.sun_intensity = env.sun_intensity;
    cloud.sky_ambient = env.sky_ambient;
    cloud.ambient_strength = env.ambient_strength;
    cloud.cloud_bottom = env.cloud_bottom;
    cloud.cloud_top = env.cloud_top;
    cloud.cloud_density = env.cloud_density;
    cloud.cloud_detail = env.cloud_detail;
}

// Mirrors the BloomConstants cbuffer in shaders/bloom.hlsl: one texel of the source
// mip, and two parameters the two passes read differently -- see the shader.
struct BloomConstants {
    XMFLOAT2 src_texel;
    float param0;
    float param1;
};

static_assert(sizeof(BloomConstants) % sizeof(UINT) == 0);
constexpr UINT kBloomConstantDwords = sizeof(BloomConstants) / sizeof(UINT);

// The bright-pass threshold/knee, the resolve's exposure and how strongly the bloom
// is added back are all per-level now -- see Environment (exposure, bloom_intensity,
// bloom_threshold, bloom_knee), read from environment_ where the passes below set
// their constants. The upsample tent's radius stays a constant: it is filter
// geometry, not part of a level's look.
constexpr float kBloomUpsampleRadius = 1.0f;

// Mirrors the OutlineConstants cbuffer in shaders/outline.hlsl. The view-
// projection stands alone because the mesh is grown in world space before it is
// projected. HLSL packs each float4 into its own row and never straddles one, so
// this C++ order maps 1:1 onto the cbuffer rows -- do not reorder one side.
struct OutlineConstants {
    XMFLOAT4X4 view_projection;
    XMFLOAT4X4 model;
    XMFLOAT4 normal_rows[3];
    XMFLOAT4 color; // rgb, plus this layer's strength in a.
    float width;
};

static_assert(sizeof(OutlineConstants) % sizeof(UINT) == 0);
constexpr UINT kOutlineConstantDwords = sizeof(OutlineConstants) / sizeof(UINT);
static_assert(kOutlineConstantDwords <= 64, "A root signature holds at most 64 DWORDs in total");

// The pick-up glow: a warm amber that reads as heat, breathing so it draws the
// eye without strobing.
//
// It is several concentric enlarged copies of the mesh (see outline.hlsl) blended
// *additively*: a tight bright one at the silhouette out to a wide faint one, so
// where they stack near the object the light is hot and it falls off smoothly
// outward into a soft halo. The pass runs with depth off and the object is
// re-painted over it, so the halo wraps every side the same no matter where the
// player stands. Each copy is one draw, so a handful of layers.
constexpr XMFLOAT3 kOutlineColor{1.0f, 0.66f, 0.24f};
constexpr int kOutlineLayers = 6;
// How far, in metres, the innermost and outermost copies are pushed out along
// the normal. The props are small, so even the outer halo is only centimetres.
constexpr float kOutlineInnerWidth = 0.003f;
constexpr float kOutlineOuterWidth = 0.026f;
// Per-layer additive strength at the inner and outer ends. The inner copy carries
// the most; the outer layers only breathe a faint wash past the edge. Kept low
// because with depth off every layer now reaches all the way around, so their
// sum near the edge is what sets the brightness.
constexpr float kOutlineInnerAlpha = 0.22f;
constexpr float kOutlineOuterAlpha = 0.03f;
constexpr float kOutlinePulseHz = 1.4f;
// The pulse never fully darkens the rim -- it breathes between these.
constexpr float kOutlinePulseMin = 0.65f;
constexpr float kOutlinePulseMax = 1.0f;

// The linear texture format, for data that is not colour: normal maps, the
// metallic-roughness map, the font's distance field, and the 1x1 defaults.
constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// The sRGB texture format, for base-colour images. Sampling through an sRGB view
// decodes the texel to linear light in hardware, which is where the BRDF wants
// it. The scene pixel shader re-encodes to sRGB on the way out (LinearToSrgb),
// and it linearises the flat colours in scene.cpp itself -- so base colour,
// factors and lighting all meet in linear space. The back buffer stays plain
// _UNORM: the encode is done in the shader, which keeps the outline and text
// passes, which still write display-space colour, untouched.
constexpr DXGI_FORMAT kSrgbTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

// The reflection probe cubemap: this many texels on a side, per face, captured
// once from the yard's centre at eye height. Typeless so the same memory renders
// as _UNORM (the capture writes display-space colour, as the scene pass does to
// the back buffer) and samples as _UNORM_SRGB (decoded to linear light, like a
// base-colour texture). The scene's own depth buffer cannot be reused -- it is the
// window's size -- so the capture has a square depth of its own in DSV slot 2.
constexpr UINT kProbeSize = 256;
constexpr XMFLOAT3 kProbePosition{0.0f, 1.5f, 0.0f};
constexpr DXGI_FORMAT kProbeResourceFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
constexpr DXGI_FORMAT kProbeRtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kProbeSrvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr UINT kProbeDsvIndex = 2;

// Slot 0 of the texture heap. A material with no base colour texture points
// here, which spares the shader a branch and the pipeline a second variant.
constexpr UINT kWhiteTexture = 0;

// Slot 1: the flat tangent-space normal (0,0,1), encoded (128,128,255). A
// material with no normal map points here, so the shader always samples a normal
// map and a textureless surface simply reconstructs its geometric normal.
constexpr UINT kFlatNormalTexture = 1;

// One corner of a glyph quad, in the HUD text pass. Position is already in
// normalized device coordinates, so the vertex shader is a pass-through.
struct TextVertex {
    XMFLOAT2 position;
    XMFLOAT2 uv;
};

// Mirrors the Constants cbuffer in shaders/text.hlsl. Eight DWORDs of root
// constants: the colour, the distance-field's texel width over the atlas size,
// and a clip-space nudge the shadow pass rides on.
struct TextConstants {
    XMFLOAT4 color;
    XMFLOAT2 unit_range;
    XMFLOAT2 ndc_offset;
    // >0.5 fills the quad flat with `color` instead of sampling the atlas -- the
    // panel behind the debug overlay. Zero-initialised, so the glyph draws leave it 0.
    float solid = 0.0f;
};

static_assert(sizeof(TextConstants) % sizeof(UINT) == 0);
constexpr UINT kTextConstantDwords = sizeof(TextConstants) / sizeof(UINT);

// The most glyphs one HUD line can draw. A prompt is a few words; this is roomy.
// Enough for the prompt plus the debug overlay's lines packed into one frame region;
// LayoutLine stops filling once this is reached, so an overrun just clips the tail.
constexpr UINT kMaxTextGlyphs = 512;
constexpr UINT kTextVerticesPerGlyph = 6; // Two triangles.
constexpr UINT kTextRegionVertices = kMaxTextGlyphs * kTextVerticesPerGlyph;
constexpr UINT kTextRegionBytes = kTextRegionVertices * sizeof(TextVertex);

// Text height as a fraction of the back buffer's, so the prompt keeps its size
// across resolutions.
constexpr float kTextHeightFraction = 0.040f;
// The shadow's offset from the text, in pixels, down and to the right.
constexpr float kTextShadowPixels = 1.25f;
// The debug overlay reads as tooling, so its lines are smaller than the prompt.
constexpr float kDebugTextHeightFraction = 0.019f;
// Line pitch of the debug overlay, in its own glyph heights.
constexpr float kDebugTextLineFactor = 1.35f;

// The launch menu. A dark neutral backdrop so the warm text reads over it; the
// title sits in the upper third with the entries stacked below the middle. All
// sizes are fractions of the back buffer height, matching kTextHeightFraction, so
// the menu keeps its proportions across resolutions.
constexpr float kMenuClearColor[] = {0.07f, 0.07f, 0.085f, 1.0f};
constexpr float kMenuTitleFraction = 0.11f;  // Title glyph height / back buffer.
constexpr float kMenuEntryFraction = 0.055f; // Entry glyph height / back buffer.
constexpr float kMenuTitleBaselineFraction = 0.32f;  // Title baseline, from the top.
constexpr float kMenuFirstEntryBaselineFraction = 0.52f; // First entry's baseline.
constexpr float kMenuEntrySpacingFactor = 1.8f; // Line pitch, in entry glyph heights.

// The results screen: a title, a block of order-breakdown lines in the middle, then the
// action list anchored low. The breakdown lines are smaller and tighter than menu
// entries so several orders fit; the actions sit at a fixed baseline (independent of how
// many breakdown lines there are) so their hit-test needs only the action count.
constexpr float kResultsTitleFraction = 0.09f;        // Title glyph height / back buffer.
constexpr float kResultsTitleBaselineFraction = 0.24f; // Title baseline, from the top.
constexpr float kResultsLineFraction = 0.038f;         // Breakdown glyph height / buffer.
constexpr float kResultsFirstLineBaselineFraction = 0.40f; // First breakdown baseline.
constexpr float kResultsLineSpacingFactor = 1.5f;      // Breakdown line pitch, in heights.
constexpr float kResultsActionFraction = 0.05f;        // Action glyph height / back buffer.
constexpr float kResultsFirstActionBaselineFraction = 0.78f; // First action's baseline.
constexpr float kResultsActionSpacingFactor = 1.8f;    // Action line pitch, in heights.

// The keybinds screen carries more rows than the launch menu (every gameplay action
// plus Reset and Back), so its title sits higher and its rows are smaller and tighter
// to fit them all. The two columns anchor at fractions of the width: the label's left
// edge and the key value's right edge, leaving a gutter between them.
constexpr float kKeybindTitleFraction = 0.075f;
constexpr float kKeybindTitleBaselineFraction = 0.16f;
constexpr float kKeybindRowFraction = 0.040f;
constexpr float kKeybindFirstRowBaselineFraction = 0.27f;
constexpr float kKeybindRowSpacingFactor = 1.95f;
constexpr float kKeybindLabelLeftFraction = 0.28f;  // Label column's left edge.
constexpr float kKeybindValueRightFraction = 0.72f; // Value column's right edge.
constexpr float kKeybindHintFraction = 0.032f;      // Footer glyph height (smaller).
constexpr float kKeybindHintBaselineFraction = 0.965f; // Footer instructions.

#ifndef NDEBUG
constexpr bool kEnableDebugLayer = true;
#else
constexpr bool kEnableDebugLayer = false;
#endif

// Picks the first hardware adapter that can create a feature level 12.0 device,
// preferring the discrete GPU. WARP is never selected: a software rasterizer
// would silently turn a broken machine into a very slow working one.
ComPtr<IDXGIAdapter1> SelectAdapter(IDXGIFactory6& factory) {
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory.EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                            IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                        __uuidof(ID3D12Device), nullptr))) {
            return adapter;
        }
    }
    throw std::runtime_error("No Direct3D 12 feature level 12.0 hardware adapter found");
}

// The whole-resource subresource range: the sentinel first-mip 0xffffffff selects
// every mip, slice and plane at once, which is what every transition here wants.
constexpr D3D12_BARRIER_SUBRESOURCE_RANGE kAllSubresources{0xffffffff, 0, 0, 0, 0, 0};

// One texture transition as an enhanced barrier: the sync scopes that must drain
// and start, the accesses the resource is leaving and entering, and the layout it
// is changing between. Enhanced barriers spell out all three where a legacy
// transition rolled sync, access and layout into a single before/after state.
void TextureBarrier(ID3D12GraphicsCommandList7* command_list, ID3D12Resource* resource,
                    D3D12_BARRIER_SYNC sync_before, D3D12_BARRIER_ACCESS access_before,
                    D3D12_BARRIER_LAYOUT layout_before, D3D12_BARRIER_SYNC sync_after,
                    D3D12_BARRIER_ACCESS access_after, D3D12_BARRIER_LAYOUT layout_after) {
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = sync_before;
    barrier.SyncAfter = sync_after;
    barrier.AccessBefore = access_before;
    barrier.AccessAfter = access_after;
    barrier.LayoutBefore = layout_before;
    barrier.LayoutAfter = layout_after;
    barrier.pResource = resource;
    barrier.Subresources = kAllSubresources;
    barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
    const D3D12_BARRIER_GROUP group = CD3DX12_BARRIER_GROUP(1, &barrier);
    command_list->Barrier(1, &group);
}

// One buffer transition. Buffers carry no layout -- only sync and access -- so a
// buffer barrier is the same idea without the layout fields.
void BufferBarrier(ID3D12GraphicsCommandList7* command_list, ID3D12Resource* resource,
                   D3D12_BARRIER_SYNC sync_before, D3D12_BARRIER_ACCESS access_before,
                   D3D12_BARRIER_SYNC sync_after, D3D12_BARRIER_ACCESS access_after) {
    D3D12_BUFFER_BARRIER barrier{};
    barrier.SyncBefore = sync_before;
    barrier.SyncAfter = sync_after;
    barrier.AccessBefore = access_before;
    barrier.AccessAfter = access_after;
    barrier.pResource = resource;
    barrier.Offset = 0;
    barrier.Size = UINT64_MAX; // The whole buffer.
    const D3D12_BARRIER_GROUP group = CD3DX12_BARRIER_GROUP(1, &barrier);
    command_list->Barrier(1, &group);
}

} // namespace

void Renderer::Initialize(HWND hwnd, UINT width, UINT height) {
    width_ = width;
    height_ = height;

    CreateDevice();
    CreateCommandObjects();
    CreateSwapChain(hwnd, width, height);
    CreateRenderTargetViews();
    // The engine heap holds the session-level SRVs the depth buffer, HDR target and
    // shadow map each place into it below, so it comes up before any of them.
    CreateEngineDescriptorHeap();
    CreateDepthBuffer();
    CreateHdrTarget();
    CreateBloomTargets();
    CreateFluidTargets();
    CreateShadowMap();
    CreatePipeline();
    CreateSkyPipeline();
    CreateOutlinePipeline();
    CreateShadowPipeline();
    CreateTextPipeline();
    CreateTonemapPipeline();
    CreateLightShaftPipeline();
    CreateCloudPipeline();
    CreateFluidPipeline();
    CreateBloomPipeline();
    // The grass pass only exists where the device grows blades in a mesh shader; on
    // hardware without the tier the pipeline is never built and RenderGrass returns
    // early. CreateDevice set the flag before any of this ran.
    if (mesh_shaders_supported_) {
        CreateGrassPipeline();
    }

    // The Flow fire/smoke sim runs on our own device, recording into our command list and
    // versioning against our fence -- all of which exist now. Its grid and renderer are
    // built lazily on the first RenderFlow, where a recording command list is available.
    flow_.Initialize(device_.Get(), queue_.Get(), fence_.Get());

    // Everything above is the session's, not the level's. The scene geometry, the
    // font atlas and the reflection probe come up in LoadScene, so a level can be
    // swapped out from under all this without rebuilding the device or pipelines.
    initialized_ = true;
}

void Renderer::LoadScene(const Scene& scene) {
    // The shared descriptor heap, every model's buffers and images, the font atlas
    // and the probe cube. Split out of Initialize so a level swap re-runs only this
    // (after ReleaseScene) rather than the whole device bring-up.
    CreateSceneGeometry(scene);
    CaptureReflectionProbe(scene);
}

void Renderer::ReleaseScene() {
    // The GPU may still be reading this level's buffers, textures, heap or probe in
    // the frame in flight, so drain it before any of these ComPtrs let go.
    FlushGpu();

    // A level swap runs through here; empty the Flow grid so a plume lit in one yard does
    // not hang in the air of the next. The grid's allocation is kept for the next level.
    flow_.Clear();

    // Everything CreateSceneGeometry and CaptureReflectionProbe built. The shared
    // heap is torn down whole -- the font atlas and shadow-map SRVs live in it too,
    // so the next LoadScene re-places them (the shadow map resource itself, created
    // once in CreateShadowMap, is left standing). Descriptor slot bookkeeping is
    // recomputed from scratch each load, so nothing here needs resetting but the
    // resources.
    models_.clear();
    textures_.clear();
    texture_heap_.Reset();
    atlas_texture_.Reset();
    mono_atlas_texture_.Reset();
    probe_cube_.Reset();
    probe_rtv_heap_.Reset();
    probe_depth_.Reset();
}

float Renderer::AspectRatio() const {
    if (height_ == 0) {
        return 1.0f;
    }
    return static_cast<float>(width_) / static_cast<float>(height_);
}

void Renderer::CreateDevice() {
    UINT factory_flags = 0;

    if constexpr (kEnableDebugLayer) {
        // Must be enabled before the device is created, otherwise the device is
        // created without validation and this call does nothing.
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }

    ThrowIfFailed(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_)), "CreateDXGIFactory2");

    ComPtr<IDXGIAdapter1> adapter = SelectAdapter(*factory_.Get());
    ThrowIfFailed(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device_)),
                  "D3D12CreateDevice");

    if constexpr (kEnableDebugLayer) {
        ComPtr<ID3D12InfoQueue> info_queue;
        if (SUCCEEDED(device_.As(&info_queue))) {
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
            info_queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        }
    }

    // Every transition in this renderer is an enhanced barrier, and every managed
    // texture is born with an explicit barrier layout. Both need a recent runtime
    // and driver; on the FL 12.0 hardware this targets they are effectively always
    // present, but fail loudly rather than silently mis-synchronise where they are
    // not.
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12{};
    if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &options12,
                                            sizeof(options12))) ||
        !options12.EnhancedBarriersSupported) {
        throw std::runtime_error("This GPU or driver does not support D3D12 enhanced barriers");
    }

    // Mesh shaders are optional: unlike enhanced barriers they are not assumed present
    // on this FL 12.0 target, so record the tier as a capability rather than requiring
    // it. Grass and anything else that wants the amplification/mesh path checks
    // MeshShadersSupported() and falls back to instanced draws where it is absent.
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7,
                                               sizeof(options7)))) {
        mesh_shaders_supported_ = options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1;
    }

    BOOL tearing = FALSE;
    if (SUCCEEDED(factory_->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing,
                                                sizeof(tearing)))) {
        allow_tearing_ = tearing == TRUE;
    }
}

void Renderer::CreateCommandObjects() {
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_)),
                  "CreateCommandQueue");

    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                      IID_PPV_ARGS(&allocators_[i])),
                      "CreateCommandAllocator");
    }

    ThrowIfFailed(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             allocators_[0].Get(), nullptr,
                                             IID_PPV_ARGS(&command_list_)),
                  "CreateCommandList");
    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ThrowIfFailed(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)),
                  "CreateFence");
    fence_values_[0] = 1;

    fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (fence_event_ == nullptr) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateEvent");
    }
}

void Renderer::CreateSwapChain(HWND hwnd, UINT width, UINT height) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.BufferCount = kFrameCount;
    desc.Width = width;
    desc.Height = height;
    desc.Format = kBackBufferFormat;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.Flags = allow_tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;

    ComPtr<IDXGISwapChain1> swap_chain;
    ThrowIfFailed(
        factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &desc, nullptr, nullptr, &swap_chain),
        "CreateSwapChainForHwnd");

    // The game drives its own fullscreen transitions; DXGI's Alt+Enter handling
    // fights with the message loop.
    ThrowIfFailed(factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER),
                  "MakeWindowAssociation");

    ThrowIfFailed(swap_chain.As(&swap_chain_), "IDXGISwapChain3 QueryInterface");
    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    // One RTV per swapchain buffer, one for the HDR scene buffer (kHdrRtvIndex) the
    // whole world renders into, then one per bloom mip, then the fluid's three targets
    // (surface depth, its blur, thickness).
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = kFrameCount + 1 + kBloomLevels + 3;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)),
                  "CreateDescriptorHeap(RTV)");
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Three: the scene's depth buffer, the sun's shadow map, then the reflection
    // probe's square capture depth.
    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
    dsv_desc.NumDescriptors = 3;
    dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap_)),
                  "CreateDescriptorHeap(DSV)");
    dsv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
}

void Renderer::CreateRenderTargetViews() {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < kFrameCount; ++i) {
        ThrowIfFailed(swap_chain_->GetBuffer(i, IID_PPV_ARGS(&render_targets_[i])),
                      "SwapChain::GetBuffer");
        device_->CreateRenderTargetView(render_targets_[i].Get(), nullptr, rtv);
        rtv.Offset(1, rtv_size_);
    }

    viewport_ =
        CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_));
    scissor_ = CD3DX12_RECT(0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_));
}

void Renderer::CreateDepthBuffer() {
    // The clear value has to be declared up front and matched by every
    // ClearDepthStencilView, or the driver loses its fast clear path.
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kDepthFormat;
    clear.DepthStencil.Depth = 1.0f;

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
        kDepthResourceFormat, width_, height_, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    // Born a shader resource, the layout it rests in between frames: Render flips it
    // to a depth target for the world pass and back to a shader resource for the
    // light-shaft pass, which samples it to stop each marched ray at the surface.
    ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear,
                                                    nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&depth_stencil_)),
                  "CreateCommittedResource3(depth buffer)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    // Slot 0 of the DSV heap. The shadow map takes slot 1, written once in
    // CreateShadowMap and untouched by a resize.
    device_->CreateDepthStencilView(depth_stencil_.Get(), &dsv,
                                    dsv_heap_->GetCPUDescriptorHandleForHeapStart());

    // The R32 resource view the light-shaft pass samples, in the persistent engine
    // heap alongside the HDR buffer. Rewritten here on resize, since the resource is.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kDepthSrvFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE handle(engine_heap_->GetCPUDescriptorHandleForHeapStart(),
                                               static_cast<INT>(kDepthSrvIndex), engine_heap_size_);
    device_->CreateShaderResourceView(depth_stencil_.Get(), &srv, handle);
}

void Renderer::CreateEngineDescriptorHeap() {
    // Shader-visible, and -- unlike texture_heap_ -- built once and kept for the
    // whole session, so a level swap never disturbs the SRVs of resources that
    // outlive it. Slot 0 is the HDR scene buffer; the rest are reserved for the
    // post-process buffers to come.
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.NumDescriptors = kEngineHeapSize;
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&engine_heap_)),
                  "CreateDescriptorHeap(engine)");
    engine_heap_size_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void Renderer::CreateHdrTarget() {
    // The clear value must be declared up front and matched by every
    // ClearRenderTargetView, or the driver drops its fast-clear path.
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kHdrFormat;
    std::memcpy(clear.Color, kHdrClearColor, sizeof(clear.Color));

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
        kHdrFormat, width_, height_, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    // Born in the shader-resource layout it rests in between frames: Render's first
    // act is to flip it to a render target, and its last to flip it back, so the
    // resting layout is the one the tonemap pass reads it in.
    ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear,
                                                    nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&hdr_target_)),
                  "CreateCommittedResource3(HDR target)");

    // Its render-target view, in rtv_heap_ just past the swapchain buffers.
    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(kHdrRtvIndex), rtv_size_);
    device_->CreateRenderTargetView(hdr_target_.Get(), nullptr, rtv);

    // Its shader-resource view, in the persistent engine heap the tonemap pass binds.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kHdrFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE srv_handle(engine_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                   static_cast<INT>(kHdrSrvIndex), engine_heap_size_);
    device_->CreateShaderResourceView(hdr_target_.Get(), &srv, srv_handle);
}

void Renderer::CreateBloomTargets() {
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    for (UINT i = 0; i < kBloomLevels; ++i) {
        // Level 0 is half the window; each level halves again, floored at one texel.
        BloomTarget& target = bloom_targets_[i];
        target.width = std::max(1u, width_ >> (i + 1));
        target.height = std::max(1u, height_ >> (i + 1));

        const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
            kHdrFormat, target.width, target.height, 1, 1, 1, 0,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        // Rests as a shader resource, like the HDR buffer: each pass flips its target
        // to a render target and back. No optimized clear value -- the downsample
        // overwrites a level whole and the upsample sums onto it, so it is never
        // cleared.
        ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, nullptr,
                                                        nullptr, 0, nullptr,
                                                        IID_PPV_ARGS(&target.texture)),
                      "CreateCommittedResource3(bloom mip)");

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                static_cast<INT>(kBloomRtvBase + i), rtv_size_);
        device_->CreateRenderTargetView(target.texture.Get(), nullptr, rtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = kHdrFormat;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        const CD3DX12_CPU_DESCRIPTOR_HANDLE srv_handle(
            engine_heap_->GetCPUDescriptorHandleForHeapStart(),
            static_cast<INT>(kBloomSrvBase + i), engine_heap_size_);
        device_->CreateShaderResourceView(target.texture.Get(), &srv, srv_handle);
    }
}

void Renderer::CreateShadowMap() {
    // Typeless, so the same memory can be read two ways: a D32 depth view to
    // render into during the shadow pass, and an R32_FLOAT resource view to
    // sample in the scene pass. The clear value has to be spelled with the
    // concrete depth format or the driver drops its fast-clear path.
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kShadowDepthFormat;
    clear.DepthStencil.Depth = 1.0f;

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC1 desc =
        CD3DX12_RESOURCE_DESC1::Tex2D(kShadowResourceFormat, kShadowMapSize, kShadowMapSize, 1, 1, 1,
                                      0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    // Born a shader resource: the render loop's first act each frame is to
    // transition it to a depth target, so its resting layout between frames is the
    // one the scene pass leaves it in.
    ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear,
                                                    nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&shadow_map_)),
                  "CreateCommittedResource3(shadow map)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kShadowDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE handle(dsv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                               static_cast<INT>(kShadowDsvIndex), dsv_size_);
    device_->CreateDepthStencilView(shadow_map_.Get(), &dsv, handle);

    // A plain (non-comparison) R32 view of the map in the persistent engine heap,
    // for the light-shaft pass to sample and compare by hand. The scene pass keeps
    // its own comparison-sampled view in the per-level texture heap; both look at
    // this one resource. Written once -- the map is fixed size and survives a resize.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kShadowSrvFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE shaft_handle(
        engine_heap_->GetCPUDescriptorHandleForHeapStart(),
        static_cast<INT>(kShaftShadowSrvIndex), engine_heap_size_);
    device_->CreateShaderResourceView(shadow_map_.Get(), &srv, shaft_handle);

    // The shadow pass rasterizes the whole map every frame.
    shadow_viewport_ = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(kShadowMapSize),
                                        static_cast<float>(kShadowMapSize));
    shadow_scissor_ = CD3DX12_RECT(0, 0, static_cast<LONG>(kShadowMapSize),
                                   static_cast<LONG>(kShadowMapSize));
}

void Renderer::CreatePipeline() {
    // The per-draw transform and colour arrive as root constants. The one thing
    // that cannot travel that way is the base colour texture, which needs a
    // descriptor table, and the sampler that reads it is baked into the root
    // signature -- every texture in the game wants the same one.
    const CD3DX12_DESCRIPTOR_RANGE base_color_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    // t1: the shadow map, at a fixed slot in the same heap the base colour lives
    // in, bound once per frame rather than per draw.
    const CD3DX12_DESCRIPTOR_RANGE shadow_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
    // t2, t3: the base colour's normal and metallic-roughness maps, each its own
    // one-entry table bound per draw exactly like the base colour. Separate tables
    // rather than one widened range, so the three need not sit next to each other
    // in the heap.
    const CD3DX12_DESCRIPTOR_RANGE normal_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
    const CD3DX12_DESCRIPTOR_RANGE mr_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);
    // t4: the reflection probe cubemap, bound once per pass rather than per draw --
    // one probe serves the whole yard.
    const CD3DX12_DESCRIPTOR_RANGE probe_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);
    // t5: the ambient-occlusion map, per draw like the other material textures.
    const CD3DX12_DESCRIPTOR_RANGE occlusion_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);

    CD3DX12_ROOT_PARAMETER parameters[8];
    parameters[0].InitAsConstants(kConstantDwords, 0);
    parameters[1].InitAsDescriptorTable(1, &base_color_range, D3D12_SHADER_VISIBILITY_PIXEL);
    // b1: the frame constant buffer holding the sun's view-projection and the eye.
    parameters[2].InitAsConstantBufferView(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
    parameters[3].InitAsDescriptorTable(1, &shadow_range, D3D12_SHADER_VISIBILITY_PIXEL);
    parameters[4].InitAsDescriptorTable(1, &normal_range, D3D12_SHADER_VISIBILITY_PIXEL);
    parameters[5].InitAsDescriptorTable(1, &mr_range, D3D12_SHADER_VISIBILITY_PIXEL);
    parameters[6].InitAsDescriptorTable(1, &probe_range, D3D12_SHADER_VISIBILITY_PIXEL);
    parameters[7].InitAsDescriptorTable(1, &occlusion_range, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC samplers[2];
    // s0: anisotropic because the ground and the patio are seen almost edge on,
    // and trilinear alone turns them to mush a few metres out.
    samplers[0] = CD3DX12_STATIC_SAMPLER_DESC(0, D3D12_FILTER_ANISOTROPIC);
    samplers[0].MaxAnisotropy = 8;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1: the shadow comparison sampler. Linear comparison filtering does the 2x2
    // PCF within each tap; BORDER addressing with a white border means a receiver
    // sampling off the edge of the map compares against depth 1.0 and reads lit.
    samplers[1] = CD3DX12_STATIC_SAMPLER_DESC(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
                                              D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                              D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                              D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, _countof(samplers), samplers,
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&root_signature_)),
                  "CreateRootSignature");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "scene.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "scene.ps.cso");

    // Slot 0 only. When rigging lands, JOINTS_0 and WEIGHTS_0 arrive as a second
    // vertex stream in slot 1, so the yard's unskinned geometry never carries
    // them -- see SkinVertex in model.hpp.
    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    static_assert(sizeof(Vertex) == 48, "The input layout above spells out Vertex's offsets");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {input_layout, _countof(input_layout)};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    // Default rasterizer: cull back faces, clockwise-in-screen-space is front.
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // Default depth state: test and write, pass when nearer.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DSVFormat = kDepthFormat;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // The on-screen scene renders into the linear HDR buffer; the tonemap pass
    // resolves it to the swapchain later.
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)),
                  "CreateGraphicsPipelineState");

    // The probe-capture variant: the same pipeline, same root signature and vertex
    // shader, with the capture pixel shader (analytic sky, no cube read) and the
    // probe's render-target format. It renders the yard into the cubemap faces,
    // which are 8-bit, so it encodes to sRGB where the on-screen variant stays HDR.
    const std::vector<std::byte> capture_ps =
        ReadBinaryFile(shader_dir / "scene_capture.ps.cso");
    pso.PS = {capture_ps.data(), capture_ps.size()};
    pso.RTVFormats[0] = kProbeRtvFormat;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&scene_capture_pipeline_state_)),
        "CreateGraphicsPipelineState(capture)");
}

void Renderer::CreateGrassPipeline() {
    // Three root parameters: the field and per-blade constants the amplification and
    // mesh shaders grow the grass from (b0), the shared frame constant buffer (b1) the
    // pixel shader lights against -- the very buffer the scene pass reads, so the grass
    // takes the level's sun, sky, fog and shadow matrix with no plumbing of its own --
    // and the sun's shadow map (t0), so a blade knows whether the sun reaches it. No
    // input layout: the blades are generated on the GPU, not fetched from a buffer.
    const CD3DX12_DESCRIPTOR_RANGE shadow_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_ROOT_PARAMETER parameters[4];
    parameters[0].InitAsConstants(kGrassConstantDwords, 0);
    parameters[1].InitAsConstantBufferView(1);
    parameters[2].InitAsDescriptorTable(1, &shadow_range, D3D12_SHADER_VISIBILITY_PIXEL);
    // b2: the obstacle rectangles. Read by the amplification shader (whole-cell cull) and
    // the mesh shader (per-blade), so it stays visible to all stages.
    parameters[3].InitAsConstantBufferView(2);

    // The shadow comparison sampler, same as the scene's: linear comparison for 2x2 PCF
    // within a tap, white border so a blade sampling off the map's edge reads lit.
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
                                        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                        D3D12_TEXTURE_ADDRESS_MODE_BORDER,
                                        D3D12_TEXTURE_ADDRESS_MODE_BORDER);
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // No input assembler -- a mesh shader has none -- so unlike the scene signature this
    // omits ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT. Deny the fixed-function geometry stages
    // the pass never uses; the amplification, mesh and pixel stages read the root
    // arguments, so none of them is denied.
    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 1, &sampler,
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&grass_root_signature_)),
                  "CreateRootSignature(grass)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> as = ReadBinaryFile(shader_dir / "grass.as.cso");
    const std::vector<std::byte> ms = ReadBinaryFile(shader_dir / "grass.ms.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "grass.ps.cso");

    // A mesh-shader PSO is built from a pipeline state stream, not the classic graphics
    // description: the amplification and mesh stages replace the input assembler and
    // vertex shader that description assumes, so it has no slot for them. The stream
    // wrapper packs the desc below into the tokenised blob CreatePipelineState wants.
    D3DX12_MESH_SHADER_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = grass_root_signature_.Get();
    pso.AS = {as.data(), as.size()};
    pso.MS = {ms.data(), ms.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // A blade is a thin ribbon: cull neither side, so it never drops out edge-on and
    // both faces light. The mesh shader already turns each normal toward the eye, so the
    // lit side is correct whichever way the triangle happens to wind.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // Default depth: test and write, so the grass occludes and is occluded like the rest
    // of the world, and the shafts and fog that read the depth buffer include it.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DSVFormat = kDepthFormat;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // Into the linear HDR scene buffer, like everything else the world draws.
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleMask = UINT_MAX;
    pso.SampleDesc.Count = 1;

    CD3DX12_PIPELINE_MESH_STATE_STREAM stream(pso);
    const D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{sizeof(stream), &stream};
    ThrowIfFailed(device_->CreatePipelineState(&stream_desc, IID_PPV_ARGS(&grass_pipeline_state_)),
                  "CreatePipelineState(grass)");

    // The shadow-cast variant: the same amplification and mesh shaders with no pixel
    // shader, targeting the shadow map's depth format and no colour, so the field writes
    // its depth from the sun's point of view. The rasterizer carries the same bias the
    // shadow pipeline uses (see CreateShadowPipeline), pushing casters a hair deeper to
    // fight the acne a depth-only pass leaves.
    pso.PS = {};
    pso.RasterizerState.DepthBias = 4000;
    pso.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pso.RasterizerState.DepthBiasClamp = 0.0f;
    pso.DSVFormat = kShadowDepthFormat;
    pso.NumRenderTargets = 0;
    pso.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;

    CD3DX12_PIPELINE_MESH_STATE_STREAM shadow_stream(pso);
    const D3D12_PIPELINE_STATE_STREAM_DESC shadow_stream_desc{sizeof(shadow_stream), &shadow_stream};
    ThrowIfFailed(
        device_->CreatePipelineState(&shadow_stream_desc, IID_PPV_ARGS(&grass_shadow_pipeline_state_)),
        "CreatePipelineState(grass shadow)");

    // The obstacle constant buffer the shaders read to keep clear of the world's
    // footprints. One upload-heap region kept mapped and rewritten by SetGrass on each
    // level load -- the footprints are static within a level, so unlike the frame
    // constants it is not buffered per frame in flight.
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC ob_desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(GrassObstaclesCB));
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &ob_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&grass_obstacles_)),
                  "CreateCommittedResource(grass obstacles)");
    const CD3DX12_RANGE no_read(0, 0);
    void* mapped = nullptr;
    ThrowIfFailed(grass_obstacles_->Map(0, &no_read, &mapped), "GrassObstacles::Map");
    grass_obstacles_mapped_ = static_cast<std::byte*>(mapped);
    grass_obstacles_address_ = grass_obstacles_->GetGPUVirtualAddress();
    // Start empty until a level's SetGrass fills it.
    GrassObstaclesCB empty{};
    std::memcpy(grass_obstacles_mapped_, &empty, sizeof(empty));
}

void Renderer::CreateSkyPipeline() {
    // One root parameter: the inverse view-projection and the eye, as root
    // constants. No input layout (the vertices come from SV_VertexID), no
    // textures, no sampler.
    CD3DX12_ROOT_PARAMETER parameters[1];
    parameters[0].InitAsConstants(kSkyConstantDwords, 0);

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 0, nullptr,
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(sky) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&sky_root_signature_)),
                  "CreateRootSignature(sky)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "sky.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "sky.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {nullptr, 0};
    pso.pRootSignature = sky_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    // The fullscreen triangle's winding is unimportant, so do not risk culling it.
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // No depth at all: the sky is written flat behind the frame, and the geometry
    // that follows paints over it wherever the yard stands.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // On-screen, the sky is the background of the linear HDR buffer.
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sky_pipeline_state_)),
                  "CreateGraphicsPipelineState(sky)");

    // The capture variant: the same pass behind the reflection probe, encoding to
    // sRGB for the 8-bit cube instead of leaving its radiance linear.
    const std::vector<std::byte> capture_ps = ReadBinaryFile(shader_dir / "sky_capture.ps.cso");
    pso.PS = {capture_ps.data(), capture_ps.size()};
    pso.RTVFormats[0] = kProbeRtvFormat;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&sky_capture_pipeline_state_)),
        "CreateGraphicsPipelineState(sky capture)");
}

void Renderer::DrawSky(const XMMATRIX& view_projection, XMFLOAT3 camera_position, float time,
                       bool capture) {
    // The capture variant encodes to sRGB for the 8-bit probe cube; the on-screen
    // one leaves its radiance linear for the HDR buffer.
    command_list_->SetPipelineState(
        capture ? sky_capture_pipeline_state_.Get() : sky_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(sky_root_signature_.Get());
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 0, nullptr);

    SkyConstants constants{};
    XMStoreFloat4x4(&constants.inv_view_projection, XMMatrixInverse(nullptr, view_projection));
    constants.camera_position = camera_position;
    constants.time = time;
    ApplyEnvironment(constants, environment_);
    command_list_->SetGraphicsRoot32BitConstants(0, kSkyConstantDwords, &constants, 0);

    // Three vertices, no buffer: the vertex shader builds the triangle from the id.
    command_list_->DrawInstanced(3, 1, 0, 0);
}

void Renderer::CreateOutlinePipeline() {
    // Everything the outline needs -- the transforms, the rim colour and the
    // push-out width -- travels as root constants. There is no texture, so this
    // root signature has none of the scene's descriptor table or sampler.
    CD3DX12_ROOT_PARAMETER parameters[1];
    parameters[0].InitAsConstants(kOutlineConstantDwords, 0);

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    // The pixel shader reads the rim colour from b0, so pixel access stays open;
    // only the stages the pass never uses are denied.
    root_desc.Init(_countof(parameters), parameters, 0, nullptr,
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(outline) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&outline_root_signature_)),
                  "CreateRootSignature(outline)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "outline.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "outline.ps.cso");

    // The same 48-byte vertex stream the scene uses. The VS only reads POSITION
    // and NORMAL, so the layout lists just what it consumes; the stride comes from
    // the vertex buffer view and steps past the UV and tangent this pass ignores.
    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {input_layout, _countof(input_layout)};
    pso.pRootSignature = outline_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};

    // Cull the front faces: the enlarged copy's near shell would just double the
    // fill, so only the far shell is kept, which paints the enlarged silhouette
    // once. The object re-paint afterward carves the object back out of it.
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;

    // Additive blend: each hull adds its alpha-weighted colour to the frame, so
    // the concentric shells accumulate into a hot core near the object and a soft
    // wash outward -- light being emitted, not a band being painted. The source
    // colour is pre-weighted by its own alpha (SRC_ALPHA) so a layer's strength
    // travels in that alpha; the destination is kept whole (ONE).
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // No depth test at all: the halo pokes a little past the silhouette, and on
    // the near side that margin sits over ground closer to the eye than the
    // object. A depth test would clip it there and nowhere else, so the glow
    // would favour the faces turned away from the player. Off, it wraps evenly;
    // the object re-paint that follows is what keeps it from washing the object.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;

    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // The halo is drawn into the linear HDR scene buffer, alongside the world.
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&outline_pipeline_state_)),
        "CreateGraphicsPipelineState(outline)");
}

void Renderer::CreateTonemapPipeline() {
    // b0: the exposure and bloom-intensity scalars, as root constants. t0: the HDR
    // scene buffer, read by integer Load (the resolve is 1:1). t1: the bloom top mip,
    // in its own table since it is not contiguous with t0 in the engine heap, and
    // sampled through s0 so the hardware upscales it from half resolution.
    const CD3DX12_DESCRIPTOR_RANGE hdr_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    const CD3DX12_DESCRIPTOR_RANGE bloom_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

    CD3DX12_ROOT_PARAMETER parameters[3];
    parameters[0].InitAsConstants(2, 0);
    parameters[1].InitAsDescriptorTable(1, &hdr_range, D3D12_SHADER_VISIBILITY_PIXEL);
    parameters[2].InitAsDescriptorTable(1, &bloom_range, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 1, &sampler,
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(tonemap) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&tonemap_root_signature_)),
                  "CreateRootSignature(tonemap)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "tonemap.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "tonemap.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    // No input layout: the fullscreen triangle comes from SV_VertexID.
    pso.InputLayout = {nullptr, 0};
    pso.pRootSignature = tonemap_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    // The resolve overwrites every pixel of the frame, so no depth at all.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // The one pass that actually writes the swapchain.
    pso.RTVFormats[0] = kBackBufferFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&tonemap_pipeline_state_)),
                  "CreateGraphicsPipelineState(tonemap)");
}

void Renderer::CreateLightShaftPipeline() {
    // b0: the camera and light matrices, the eye and the sun, as root constants.
    // t0, t1: the scene depth and the shadow map, one two-entry table into the
    // engine heap. s0: a point clamp sampler -- the march reads raw stored depths,
    // so there is nothing to filter.
    const CD3DX12_DESCRIPTOR_RANGE srv_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsConstants(kLightShaftConstantDwords, 0);
    parameters[1].InitAsDescriptorTable(1, &srv_range, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 1, &sampler,
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(light shafts) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&light_shaft_root_signature_)),
                  "CreateRootSignature(light shafts)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "lightshafts.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "lightshafts.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    // No input layout: the fullscreen triangle comes from SV_VertexID.
    pso.InputLayout = {nullptr, 0};
    pso.pRootSignature = light_shaft_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Additive: the scattered light is added onto the HDR scene, never covering it.
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // No depth at all: the pass adds light over the whole frame, and the scene depth
    // it needs is bound as a shader resource here, not a depth target.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    // Into the HDR scene buffer, before the resolve.
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&light_shaft_pipeline_state_)),
        "CreateGraphicsPipelineState(light shafts)");
}

void Renderer::CreateCloudPipeline() {
    // b0: the camera, the sun and the sky, as root constants. t0: the scene depth, a
    // one-entry table into the engine heap -- the clouds need no shadow map, only the
    // depth that stops them behind the world. s0: a point clamp sampler, since the
    // march reads the raw stored depth.
    const CD3DX12_DESCRIPTOR_RANGE srv_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsConstants(kCloudConstantDwords, 0);
    parameters[1].InitAsDescriptorTable(1, &srv_range, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 1, &sampler,
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(clouds) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&cloud_root_signature_)),
                  "CreateRootSignature(clouds)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "clouds.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "clouds.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {nullptr, 0};
    pso.pRootSignature = cloud_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Premultiplied-over: the shader weights its colour by coverage and returns the
    // covered fraction as alpha, so ONE / INV_SRC_ALPHA lays the clouds over the sky
    // already in the HDR buffer without double-counting the coverage. A world pixel
    // returns alpha 0 and is left untouched.
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // No depth target: the scene depth it needs is bound as a shader resource, and the
    // pass writes colour over the whole frame with the blend above.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&cloud_pipeline_state_)),
                  "CreateGraphicsPipelineState(clouds)");
}

void Renderer::CreateFluidPipeline() {
    // Serialize and create one root signature, throwing with the compiler's message on a
    // bad layout. A local helper so the three fluid passes below stay readable.
    auto make_root_sig = [&](const CD3DX12_ROOT_SIGNATURE_DESC& desc, const char* name) {
        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature,
                                                 &error);
        if (FAILED(hr)) {
            std::string message =
                error ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                    error->GetBufferSize())
                      : "unknown error";
            throw std::runtime_error(std::string("D3D12SerializeRootSignature(") + name +
                                     ") failed: " + message);
        }
        ComPtr<ID3D12RootSignature> root;
        ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                                   signature->GetBufferSize(), IID_PPV_ARGS(&root)),
                      "CreateRootSignature(fluid)");
        return root;
    };

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    CD3DX12_STATIC_SAMPLER_DESC point_clamp(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
                                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                            D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    point_clamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // --- Pass 1: the impostor surface pass (nearest depth + thickness, MRT) ---
    {
        // b0: the view and projection, as root constants. t0: the per-frame droplet buffer,
        // a root SRV (a StructuredBuffer of world centre + radius) so it needs no descriptor
        // heap slot, like the grass obstacles. The vertex shader reads both, so no stage is
        // denied.
        CD3DX12_ROOT_PARAMETER parameters[2];
        parameters[0].InitAsConstants(kFluidConstantDwords, 0);
        parameters[1].InitAsShaderResourceView(0);
        CD3DX12_ROOT_SIGNATURE_DESC root_desc;
        root_desc.Init(_countof(parameters), parameters, 0, nullptr,
                       D3D12_ROOT_SIGNATURE_FLAG_NONE);
        fluid_surface_root_signature_ = make_root_sig(root_desc, "fluid surface");

        const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "fluid.vs.cso");
        const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "fluid.ps.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.InputLayout = {nullptr, 0};  // no vertex buffer: the quad comes from SV_VertexID
        pso.pRootSignature = fluid_surface_root_signature_.Get();
        pso.VS = {vs.data(), vs.size()};
        pso.PS = {ps.data(), ps.size()};
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        // The billboard faces the camera and the pixel shader discards its corners, so
        // there is no meaningful back face to cull.
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        // Independent per-target blend: RT0 (surface depth) keeps the MINIMUM (nearest z),
        // so overlapping droplets resolve to the front surface with no depth buffer of their
        // own; RT1 (thickness) ADDS, summing each droplet's chord along the ray.
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso.BlendState.IndependentBlendEnable = TRUE;
        D3D12_RENDER_TARGET_BLEND_DESC& depth_blend = pso.BlendState.RenderTarget[0];
        depth_blend.BlendEnable = TRUE;
        depth_blend.SrcBlend = D3D12_BLEND_ONE;
        depth_blend.DestBlend = D3D12_BLEND_ONE;
        depth_blend.BlendOp = D3D12_BLEND_OP_MIN;
        depth_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        depth_blend.DestBlendAlpha = D3D12_BLEND_ONE;
        depth_blend.BlendOpAlpha = D3D12_BLEND_OP_MIN;
        depth_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        D3D12_RENDER_TARGET_BLEND_DESC& thick_blend = pso.BlendState.RenderTarget[1];
        thick_blend.BlendEnable = TRUE;
        thick_blend.SrcBlend = D3D12_BLEND_ONE;
        thick_blend.DestBlend = D3D12_BLEND_ONE;
        thick_blend.BlendOp = D3D12_BLEND_OP_ADD;
        thick_blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        thick_blend.DestBlendAlpha = D3D12_BLEND_ONE;
        thick_blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        thick_blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        // Depth tested read-only against the world (write mask zero, so the scene depth is
        // never touched): a droplet behind the grill is culled, everything in front draws.
        // The MIN/ADD blends -- not a depth buffer of our own -- resolve fluid over fluid.
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pso.DSVFormat = kDepthFormat;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 2;
        pso.RTVFormats[0] = kFluidDepthFormat;
        pso.RTVFormats[1] = kFluidThicknessFormat;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(
            device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&fluid_surface_pipeline_)),
            "CreateGraphicsPipelineState(fluid surface)");
    }

    // --- Pass 2: the separable bilateral depth blur ---
    {
        // b0: the blur axis/sentinel/threshold. t0: the depth to smooth (one engine-heap
        // table). s0: a point clamp sampler -- the blur reads exact stored depths.
        const CD3DX12_DESCRIPTOR_RANGE srv_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        CD3DX12_ROOT_PARAMETER parameters[2];
        parameters[0].InitAsConstants(kFluidBlurConstantDwords, 0);
        parameters[1].InitAsDescriptorTable(1, &srv_range, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_ROOT_SIGNATURE_DESC root_desc;
        root_desc.Init(_countof(parameters), parameters, 1, &point_clamp,
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        fluid_blur_root_signature_ = make_root_sig(root_desc, "fluid blur");

        const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "fluid_blur.vs.cso");
        const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "fluid_blur.ps.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.InputLayout = {nullptr, 0};
        pso.pRootSignature = fluid_blur_root_signature_.Get();
        pso.VS = {vs.data(), vs.size()};
        pso.PS = {ps.data(), ps.size()};
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);  // overwrite
        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kFluidDepthFormat;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(
            device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&fluid_blur_pipeline_)),
            "CreateGraphicsPipelineState(fluid blur)");
    }

    // --- Pass 3: the dual-source composite into the HDR buffer ---
    {
        // b0: the reconstruction/lighting/absorption constants. t0..t1: the blurred depth
        // and the thickness (one two-entry engine-heap table). s0: a point clamp sampler.
        const CD3DX12_DESCRIPTOR_RANGE srv_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);
        CD3DX12_ROOT_PARAMETER parameters[2];
        parameters[0].InitAsConstants(kFluidCompositeConstantDwords, 0);
        parameters[1].InitAsDescriptorTable(1, &srv_range, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_ROOT_SIGNATURE_DESC root_desc;
        root_desc.Init(_countof(parameters), parameters, 1, &point_clamp,
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                           D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);
        fluid_composite_root_signature_ = make_root_sig(root_desc, "fluid composite");

        const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "fluid_composite.vs.cso");
        const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "fluid_composite.ps.cso");

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
        pso.InputLayout = {nullptr, 0};
        pso.pRootSignature = fluid_composite_root_signature_.Get();
        pso.VS = {vs.data(), vs.size()};
        pso.PS = {ps.data(), ps.size()};
        pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

        // Dual-source blend: dst' = surface + dst * transmittance. Output 0 is the additive
        // wet sheen, output 1 the per-channel Beer-Lambert transmittance used as the
        // destination factor -- so the fluid tints and dims the scene behind it with no copy
        // of the scene buffer. An empty pixel returns (0, 1) and leaves the scene untouched.
        pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_SRC1_COLOR;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_SRC1_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso.DepthStencilState.DepthEnable = FALSE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 1;
        pso.RTVFormats[0] = kHdrFormat;
        pso.SampleDesc.Count = 1;
        ThrowIfFailed(
            device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&fluid_composite_pipeline_)),
            "CreateGraphicsPipelineState(fluid composite)");
    }

    // One upload-heap region per frame in flight, mapped once and left mapped, rewritten
    // with this frame's droplet points. MoveToNextFrame retires a frame before its region
    // is reused, so the CPU never overwrites points the GPU is still reading.
    fluid_droplets_stride_ = kMaxSprayDroplets * static_cast<UINT>(sizeof(XMFLOAT4));
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC buffer_desc =
        CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(fluid_droplets_stride_) * kFrameCount);
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&fluid_droplets_)),
                  "CreateCommittedResource(fluid droplets)");

    void* mapped = nullptr;
    const CD3DX12_RANGE no_read(0, 0);
    ThrowIfFailed(fluid_droplets_->Map(0, &no_read, &mapped), "FluidDroplets::Map");
    fluid_droplets_mapped_ = static_cast<std::byte*>(mapped);
}

void Renderer::CreateFluidTargets() {
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);

    // Make one window-sized render target of `format`, born a shader resource (its resting
    // layout between the passes that flip it to a target and back), with its RTV at
    // rtv_heap_ slot `rtv_index` and its SRV at engine_heap_ slot `srv_index`.
    auto make_target = [&](DXGI_FORMAT format, UINT rtv_index, UINT srv_index,
                           ComPtr<ID3D12Resource>& target, const char* name) {
        const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Tex2D(
            format, width_, height_, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                        D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, nullptr,
                                                        nullptr, 0, nullptr, IID_PPV_ARGS(&target)),
                      name);

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                static_cast<INT>(rtv_index), rtv_size_);
        device_->CreateRenderTargetView(target.Get(), nullptr, rtv);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        const CD3DX12_CPU_DESCRIPTOR_HANDLE srv_handle(
            engine_heap_->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(srv_index),
            engine_heap_size_);
        device_->CreateShaderResourceView(target.Get(), &srv, srv_handle);
    };

    make_target(kFluidDepthFormat, kFluidRtvBase + 0, kFluidDepthSrvIndex, fluid_depth_,
                "CreateCommittedResource3(fluid depth)");
    make_target(kFluidDepthFormat, kFluidRtvBase + 1, kFluidDepthBlurSrvIndex, fluid_depth_blur_,
                "CreateCommittedResource3(fluid depth blur)");
    make_target(kFluidThicknessFormat, kFluidRtvBase + 2, kFluidThicknessSrvIndex, fluid_thickness_,
                "CreateCommittedResource3(fluid thickness)");
}

void Renderer::CreateBloomPipeline() {
    // b0: the source texel size and the two pass parameters, as root constants. t0:
    // the source texture (the HDR buffer or a bloom mip), one table into the engine
    // heap. s0: a linear clamp sampler -- the filters lean on bilinear taps.
    const CD3DX12_DESCRIPTOR_RANGE src_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsConstants(kBloomConstantDwords, 0);
    parameters[1].InitAsDescriptorTable(1, &src_range, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 1, &sampler,
                   D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(bloom) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&bloom_root_signature_)),
                  "CreateRootSignature(bloom)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "bloom.vs.cso");
    const std::vector<std::byte> down_ps = ReadBinaryFile(shader_dir / "bloom_down.ps.cso");
    const std::vector<std::byte> up_ps = ReadBinaryFile(shader_dir / "bloom_up.ps.cso");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {nullptr, 0};
    pso.pRootSignature = bloom_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {down_ps.data(), down_ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kHdrFormat;
    pso.SampleDesc.Count = 1;

    // The downsample overwrites its target, so it keeps the default opaque blend.
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&bloom_downsample_pipeline_state_)),
        "CreateGraphicsPipelineState(bloom downsample)");

    // The upsample sums each mip onto the larger one, so it blends additively.
    pso.PS = {up_ps.data(), up_ps.size()};
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_ONE;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
    blend.DestBlendAlpha = D3D12_BLEND_ONE;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    ThrowIfFailed(
        device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&bloom_upsample_pipeline_state_)),
        "CreateGraphicsPipelineState(bloom upsample)");
}

void Renderer::RenderBloom() {
    // Assumes the HDR buffer is a shader resource and the engine heap is bound (the
    // shaft pass leaves both that way). Every bloom mip rests as a shader resource
    // and is flipped to a render target for the one pass that writes it.
    command_list_->SetGraphicsRootSignature(bloom_root_signature_.Get());
    command_list_->IASetVertexBuffers(0, 0, nullptr);

    const D3D12_GPU_DESCRIPTOR_HANDLE engine_base =
        engine_heap_->GetGPUDescriptorHandleForHeapStart();
    const auto srv = [&](UINT slot) {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(engine_base, static_cast<INT>(slot), engine_heap_size_);
    };
    const auto set_target = [&](UINT level) {
        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                static_cast<INT>(kBloomRtvBase + level), rtv_size_);
        command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const CD3DX12_VIEWPORT vp(0.0f, 0.0f, static_cast<float>(bloom_targets_[level].width),
                                  static_cast<float>(bloom_targets_[level].height));
        const CD3DX12_RECT sc(0, 0, static_cast<LONG>(bloom_targets_[level].width),
                              static_cast<LONG>(bloom_targets_[level].height));
        command_list_->RSSetViewports(1, &vp);
        command_list_->RSSetScissorRects(1, &sc);
    };
    const auto to_rt = [&](UINT level) {
        TextureBarrier(command_list_.Get(), bloom_targets_[level].texture.Get(),
                       D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                       D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_SYNC_RENDER_TARGET,
                       D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    };
    const auto to_srv = [&](UINT level) {
        TextureBarrier(command_list_.Get(), bloom_targets_[level].texture.Get(),
                       D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                       D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                       D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    };

    // Downsample down the pyramid: the HDR buffer into level 0 (with the bright-pass
    // knee), then each level into the next. Every step overwrites its whole target.
    command_list_->SetPipelineState(bloom_downsample_pipeline_state_.Get());
    for (UINT i = 0; i < kBloomLevels; ++i) {
        BloomConstants constants{};
        if (i == 0) {
            constants.src_texel = {1.0f / static_cast<float>(width_),
                                   1.0f / static_cast<float>(height_)};
            constants.param0 = environment_.bloom_threshold;
            constants.param1 = environment_.bloom_knee;
        } else {
            constants.src_texel = {1.0f / static_cast<float>(bloom_targets_[i - 1].width),
                                   1.0f / static_cast<float>(bloom_targets_[i - 1].height)};
            constants.param0 = 0.0f; // No threshold past the first level.
            constants.param1 = 0.0f;
        }

        to_rt(i);
        set_target(i);
        command_list_->SetGraphicsRoot32BitConstants(0, kBloomConstantDwords, &constants, 0);
        command_list_->SetGraphicsRootDescriptorTable(1, i == 0 ? srv(kHdrSrvIndex)
                                                                 : srv(kBloomSrvBase + i - 1));
        command_list_->DrawInstanced(3, 1, 0, 0);
        to_srv(i);
    }

    // Upsample back up, summing each smaller mip into the one above it with the tent
    // filter. The additive blend keeps each level's own downsampled content, so the
    // glow accumulates across scales and lands, wide and soft, in level 0.
    command_list_->SetPipelineState(bloom_upsample_pipeline_state_.Get());
    for (int i = static_cast<int>(kBloomLevels) - 2; i >= 0; --i) {
        BloomConstants constants{};
        constants.src_texel = {1.0f / static_cast<float>(bloom_targets_[i + 1].width),
                               1.0f / static_cast<float>(bloom_targets_[i + 1].height)};
        constants.param0 = kBloomUpsampleRadius;
        constants.param1 = 0.0f;

        to_rt(static_cast<UINT>(i));
        set_target(static_cast<UINT>(i));
        command_list_->SetGraphicsRoot32BitConstants(0, kBloomConstantDwords, &constants, 0);
        command_list_->SetGraphicsRootDescriptorTable(1, srv(kBloomSrvBase + i + 1));
        command_list_->DrawInstanced(3, 1, 0, 0);
        to_srv(static_cast<UINT>(i));
    }
}

void Renderer::UpdateShadowProjection() {
    // An orthographic box aimed down the sun's direction and centred on the yard:
    // the fenced area is about 24 m across, and this half-extent leaves the corners
    // and the trees comfortably inside the frame. Ground past it simply falls
    // outside the map and reads as lit, which is right -- nothing out there casts
    // anything the player can see through the fog.
    constexpr float kShadowExtent = 40.0f;   // Full width/height of the ortho box.
    constexpr float kShadowDistance = 50.0f; // How far back along the sun the eye sits.
    constexpr float kShadowNear = 20.0f;
    constexpr float kShadowFar = 80.0f;

    const XMVECTOR sun = XMLoadFloat3(&sun_direction_); // already normalized
    const XMVECTOR target = XMVectorZero();
    const XMVECTOR eye = XMVectorScale(sun, kShadowDistance);
    const XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMMATRIX view = XMMatrixLookAtLH(eye, target, up);
    const XMMATRIX proj =
        XMMatrixOrthographicLH(kShadowExtent, kShadowExtent, kShadowNear, kShadowFar);
    XMStoreFloat4x4(&light_view_projection_, view * proj);
}

void Renderer::SetSunDirection(XMFLOAT3 direction) {
    XMStoreFloat3(&sun_direction_, XMVector3Normalize(XMLoadFloat3(&direction)));
    UpdateShadowProjection();
    // The frame constant regions carry the light matrix; Render rewrites the current
    // one each frame from light_view_projection_, so the new sun lands next frame
    // with no need to restamp the mapped buffer here.
}

void Renderer::SetEnvironment(const Environment& environment) {
    // Just record it. Every pass that draws the sky or shades under it stamps
    // environment_ into its constant buffer as it renders -- Render for the scene
    // and shafts, DrawSky for the background -- so the change lands next frame with
    // nothing to re-upload here. Called before LoadScene, it also decides the sky
    // the reflection-probe capture bakes, since that capture reads the same paths.
    environment_ = environment;
}

void Renderer::SetGrass(const GrassPatch& grass, std::span<const OrientedBox> obstacles) {
    // Record the field; RenderGrass grows it each frame from grass_. On hardware without
    // mesh shaders the pass never runs, so this is remembered but never drawn.
    grass_ = grass;
    grass_active_ = true;

    // Nothing to pack where the pass will not run (the obstacle buffer was never built).
    if (grass_obstacles_mapped_ == nullptr) {
        return;
    }

    // The grass grows on the plane at grass_.center.y and rises about a blade's height.
    // A collider is an obstacle -- a footprint the grass must skip -- only if it rises
    // above that plane and reaches down into the sward: this drops the ground box itself
    // (its top sits at the plane) and high tree canopies (their underside floats above
    // the blades), while keeping the patio, benches, crates, trunks, grill and cooler.
    const float base = grass.center.y;
    const float lift = 0.03f;                              // above the plane to count
    const float layer_top = base + grass.blade_height * 1.4f; // the top of the sward
    const float margin = 0.06f;                           // clear the footprint edges a little

    // The field's own XZ rectangle, so a footprint entirely outside it can be dropped.
    const float half_x = grass.size.x * 0.5f;
    const float half_z = grass.size.y * 0.5f;
    const float field_min_x = grass.center.x - half_x;
    const float field_max_x = grass.center.x + half_x;
    const float field_min_z = grass.center.z - half_z;
    const float field_max_z = grass.center.z + half_z;

    GrassObstaclesCB packed{};
    UINT count = 0;
    for (const OrientedBox& box : obstacles) {
        if (count >= kMaxGrassObstacles) {
            break;
        }
        const float top = box.center.y + box.half_extents.y;
        const float bottom = box.center.y - box.half_extents.y;
        if (top <= base + lift || bottom >= layer_top) {
            continue; // the ground plane, or something floating clear above the blades
        }

        // The world XZ bound of the oriented box: its half-extents projected onto X and Z
        // through the rotation, so a yawed crate clears the true span it turns to cover.
        const XMMATRIX rot = XMMatrixRotationQuaternion(XMLoadFloat4(&box.orientation));
        const XMFLOAT3 h = box.half_extents;
        const float ext_x = std::abs(XMVectorGetX(rot.r[0])) * h.x +
                            std::abs(XMVectorGetX(rot.r[1])) * h.y +
                            std::abs(XMVectorGetX(rot.r[2])) * h.z;
        const float ext_z = std::abs(XMVectorGetZ(rot.r[0])) * h.x +
                            std::abs(XMVectorGetZ(rot.r[1])) * h.y +
                            std::abs(XMVectorGetZ(rot.r[2])) * h.z;
        const float min_x = box.center.x - ext_x - margin;
        const float max_x = box.center.x + ext_x + margin;
        const float min_z = box.center.z - ext_z - margin;
        const float max_z = box.center.z + ext_z + margin;

        // Skip anything that does not touch the field at all.
        if (max_x < field_min_x || min_x > field_max_x || max_z < field_min_z ||
            min_z > field_max_z) {
            continue;
        }

        packed.boxes[count] = {min_x, min_z, max_x, max_z};
        ++count;
    }
    packed.count = count;
    grass_obstacle_count_ = count;
    std::memcpy(grass_obstacles_mapped_, &packed, sizeof(packed));
}

void Renderer::ClearGrass() {
    grass_active_ = false;
}

void Renderer::SetFlowRegion(XMFLOAT3 center, float half_extent) {
    flow_.SetRegion(center, half_extent);
}

void Renderer::CreateShadowPipeline() {
    // The sun starts where the backyard wants it; a loaded level re-aims it through
    // SetSunDirection. Build the light's view-projection from that default now, so
    // the shadow pass has a valid matrix even before the first level loads.
    XMStoreFloat3(&sun_direction_, XMVector3Normalize(XMLoadFloat3(&kSunDirection)));
    UpdateShadowProjection();

    // The per-frame constant buffer that hands that matrix, and the eye position,
    // to the scene pass. One 256-byte-aligned region per frame in flight, kept
    // mapped: Render rewrites the current frame's region and never touches the one
    // the GPU may still be reading for the frame before it. The sun matrix is
    // static, so it is stamped into every region here; only the eye is refreshed.
    frame_constants_stride_ = static_cast<UINT>((sizeof(FrameConstants) + 255) & ~UINT64{255});
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC cb_desc =
        CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(frame_constants_stride_) * kFrameCount);
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &cb_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&frame_constants_)),
                  "CreateCommittedResource(frame constants)");

    const CD3DX12_RANGE no_read(0, 0);
    void* mapped = nullptr;
    ThrowIfFailed(frame_constants_->Map(0, &no_read, &mapped), "FrameConstants::Map");
    frame_constants_mapped_ = static_cast<std::byte*>(mapped);
    for (UINT i = 0; i < kFrameCount; ++i) {
        FrameConstants frame{};
        frame.light_view_projection = light_view_projection_;
        ApplyEnvironment(frame, environment_);
        std::memcpy(frame_constants_mapped_ + static_cast<size_t>(i) * frame_constants_stride_,
                    &frame, sizeof(frame));
    }

    // The pass draws position only, under one root constant: the light MVP. No
    // texture, no sampler, and -- with no pixel shader at all -- pixel access
    // denied alongside the geometry stages the scene never uses.
    CD3DX12_ROOT_PARAMETER parameters[1];
    parameters[0].InitAsConstants(kShadowConstantDwords, 0);

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 0, nullptr,
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&shadow_root_signature_)),
                  "CreateRootSignature(shadow)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "shadow.vs.cso");

    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {input_layout, _countof(input_layout)};
    pso.pRootSignature = shadow_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    // No PS: the rasterizer writes depth on its own.

    // A depth-only pass fights shadow acne at the source with a rasterizer bias.
    // The constant term pushes every caster a hair deeper; the slope-scaled term
    // pushes grazing faces -- where the acne lives -- deeper still. The receiver
    // in scene.hlsl adds a normal offset on top. Front faces are kept (default
    // back-face cull): the yard's slabs are thin, and rendering their far side
    // into the map would let light leak up through the ground.
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.DepthBias = 4000;
    pso.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pso.RasterizerState.DepthBiasClamp = 0.0f;

    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DSVFormat = kShadowDepthFormat;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    // Depth only: no colour targets at all.
    pso.NumRenderTargets = 0;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&shadow_pipeline_state_)),
                  "CreateGraphicsPipelineState(shadow)");
}

ComPtr<ID3D12Resource> Renderer::UploadBuffer(const void* data, UINT64 bytes,
                                              D3D12_BARRIER_ACCESS access_after,
                                              std::vector<ComPtr<ID3D12Resource>>& staging) {
    // The mesh never changes, so it lives in the default heap -- video memory --
    // and is filled once through a staging copy. An upload-heap buffer would sit
    // in system memory and be re-read across PCIe on every draw. Buffers carry no
    // layout, so it is born UNDEFINED; the copy below is its first access and needs
    // no barrier to reach it.
    const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC1 desc = CD3DX12_RESOURCE_DESC1::Buffer(bytes);
    ComPtr<ID3D12Resource> buffer;
    ThrowIfFailed(device_->CreateCommittedResource3(&default_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr,
                                                    0, nullptr, IID_PPV_ARGS(&buffer)),
                  "CreateCommittedResource3(buffer)");

    // The staging buffer sits in the upload heap, permanently CPU-readable, so it
    // is created the legacy way and never needs a barrier of its own.
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload)),
                  "CreateCommittedResource(buffer staging)");

    void* mapped = nullptr;
    const CD3DX12_RANGE no_read(0, 0);
    ThrowIfFailed(upload->Map(0, &no_read, &mapped), "Buffer::Map");
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    upload->Unmap(0, nullptr);

    command_list_->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, bytes);
    // Hand the filled buffer to the draw: the copy must drain, and its access turns
    // from copy-destination into the vertex or index read the caller asked for.
    BufferBarrier(command_list_.Get(), buffer.Get(), D3D12_BARRIER_SYNC_COPY,
                  D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_SYNC_DRAW, access_after);

    staging.push_back(std::move(upload));
    return buffer;
}

ComPtr<ID3D12Resource> Renderer::UploadTexture(const Image& image, UINT descriptor,
                                               DXGI_FORMAT format,
                                               std::vector<ComPtr<ID3D12Resource>>& staging) {
    const auto mip_levels = static_cast<UINT16>(image.levels.size());

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC1 desc =
        CD3DX12_RESOURCE_DESC1::Tex2D(format, image.width, image.height, 1, mip_levels);

    // Born a copy destination for the UpdateSubresources below, then transitioned to
    // a shader resource once the whole mip chain has landed.
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr,
                                                    0, nullptr, IID_PPV_ARGS(&texture)),
                  "CreateCommittedResource3(texture)");

    // One staging buffer for the whole chain. UpdateSubresources knows how to
    // re-pitch each level into it: a texture's rows are aligned to 256 bytes on
    // the GPU, and the tightly packed rows from the decoder are not.
    const UINT64 staging_bytes = GetRequiredIntermediateSize(texture.Get(), 0, mip_levels);
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(staging_bytes);

    ComPtr<ID3D12Resource> upload;
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&upload)),
                  "CreateCommittedResource(texture staging)");

    std::vector<D3D12_SUBRESOURCE_DATA> levels(mip_levels);
    for (UINT16 level = 0; level < mip_levels; ++level) {
        const UINT64 width = std::max(1u, image.width >> level);
        const UINT64 height = std::max(1u, image.height >> level);
        levels[level].pData = image.levels[level].data();
        levels[level].RowPitch = static_cast<LONG_PTR>(width * 4);
        levels[level].SlicePitch = static_cast<LONG_PTR>(width * height * 4);
    }

    UpdateSubresources(command_list_.Get(), texture.Get(), upload.Get(), 0, 0, mip_levels,
                       levels.data());

    // The mip chain has landed; hand the texture to the pixel shader that samples it.
    TextureBarrier(command_list_.Get(), texture.Get(), D3D12_BARRIER_SYNC_COPY,
                   D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                   D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = mip_levels;

    const CD3DX12_CPU_DESCRIPTOR_HANDLE handle(texture_heap_->GetCPUDescriptorHandleForHeapStart(),
                                               static_cast<INT>(descriptor), texture_size_);
    device_->CreateShaderResourceView(texture.Get(), &srv, handle);

    staging.push_back(std::move(upload));
    return texture;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::TextureHandle(UINT descriptor) const {
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(texture_heap_->GetGPUDescriptorHandleForHeapStart(),
                                         static_cast<INT>(descriptor), texture_size_);
}

void Renderer::CreateSceneGeometry(const Scene& scene) {
    const std::vector<Model>& models = scene.Models();

    UINT image_count = 0;
    for (const Model& model : models) {
        image_count += static_cast<UINT>(model.images.size());
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    // The white texture and the flat normal, then every model image, then the two
    // HUD font atlases (Inter + monospace), the sun's shadow map, and the reflection
    // probe cubemap last.
    heap_desc.NumDescriptors = 2 + image_count + 2 + 1 + 1;
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device_->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&texture_heap_)),
                  "CreateDescriptorHeap(SRV)");
    texture_size_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(allocators_[frame_index_]->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocators_[frame_index_].Get(), nullptr),
                  "CommandList::Reset");

    // Every staging resource stays alive until the copies have landed.
    std::vector<ComPtr<ID3D12Resource>> staging;

    // Linear white: it stands in for both a missing base colour (tint only) and a
    // missing metallic-roughness map (the factors act alone, since 1 leaves each
    // channel untouched). It is data, not colour, so it is not sRGB.
    Image white{};
    white.width = 1;
    white.height = 1;
    white.levels.push_back(std::vector<std::byte>(4, std::byte{0xff}));
    textures_.push_back(UploadTexture(white, kWhiteTexture, kTextureFormat, staging));

    // The flat normal: tangent-space (0,0,1) encoded as (128,128,255,255). A
    // material with no normal map samples this and perturbs its normal by nothing.
    Image flat_normal{};
    flat_normal.width = 1;
    flat_normal.height = 1;
    flat_normal.levels.push_back(
        std::vector<std::byte>{std::byte{0x80}, std::byte{0x80}, std::byte{0xff}, std::byte{0xff}});
    textures_.push_back(UploadTexture(flat_normal, kFlatNormalTexture, kTextureFormat, staging));

    UINT next_descriptor = kFlatNormalTexture + 1;
    models_.resize(models.size());

    for (size_t i = 0; i < models.size(); ++i) {
        const Model& model = models[i];
        GpuModel& gpu = models_[i];

        const UINT64 vertex_bytes = model.vertices.size() * sizeof(Vertex);
        const UINT64 index_bytes = model.indices.size() * sizeof(std::uint32_t);

        gpu.vertex_buffer = UploadBuffer(model.vertices.data(), vertex_bytes,
                                         D3D12_BARRIER_ACCESS_VERTEX_BUFFER, staging);
        gpu.vertex_buffer_view.BufferLocation = gpu.vertex_buffer->GetGPUVirtualAddress();
        gpu.vertex_buffer_view.StrideInBytes = sizeof(Vertex);
        gpu.vertex_buffer_view.SizeInBytes = static_cast<UINT>(vertex_bytes);

        gpu.index_buffer = UploadBuffer(model.indices.data(), index_bytes,
                                        D3D12_BARRIER_ACCESS_INDEX_BUFFER, staging);
        gpu.index_buffer_view.BufferLocation = gpu.index_buffer->GetGPUVirtualAddress();
        gpu.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
        gpu.index_buffer_view.SizeInBytes = static_cast<UINT>(index_bytes);

        // An image is sRGB only where a material uses it as base colour; the same
        // bytes read as a normal or metallic-roughness map are linear data. No
        // asset here reuses one image as both, so a single flag per image is safe.
        std::vector<bool> is_srgb(model.images.size(), false);
        for (const Material& material : model.materials) {
            if (material.base_color_image >= 0) {
                is_srgb[material.base_color_image] = true;
            }
        }

        // Where each of this model's images landed in the shared heap.
        std::vector<UINT> descriptors;
        descriptors.reserve(model.images.size());
        for (size_t image_index = 0; image_index < model.images.size(); ++image_index) {
            const DXGI_FORMAT format = is_srgb[image_index] ? kSrgbTextureFormat : kTextureFormat;
            textures_.push_back(
                UploadTexture(model.images[image_index], next_descriptor, format, staging));
            descriptors.push_back(next_descriptor++);
        }

        gpu.primitives.reserve(model.primitives.size());
        for (const Primitive& primitive : model.primitives) {
            DrawPrimitive draw{};
            draw.transform = primitive.transform;
            draw.first_index = primitive.first_index;
            draw.index_count = primitive.index_count;

            // No material means plain white, which leaves the instance's tint as
            // the only thing colouring the draw. That is how every box in the
            // yard gets its colour.
            draw.base_color = {1.0f, 1.0f, 1.0f};
            // Defaults for a primitive with no material at all -- the unit cube.
            // Fully rough and non-metal is a plain matte surface, which is what the
            // yard's flat-coloured boxes want.
            draw.metallic = 0.0f;
            draw.roughness = 1.0f;
            UINT descriptor = kWhiteTexture;
            UINT normal_descriptor = kFlatNormalTexture;
            UINT mr_descriptor = kWhiteTexture;
            UINT occlusion_descriptor = kWhiteTexture;
            if (primitive.material >= 0) {
                const Material& material = model.materials[primitive.material];
                draw.base_color = material.base_color;
                draw.metallic = material.metallic;
                draw.roughness = material.roughness;
                if (material.base_color_image >= 0) {
                    descriptor = descriptors[material.base_color_image];
                }
                if (material.normal_image >= 0) {
                    normal_descriptor = descriptors[material.normal_image];
                }
                if (material.metallic_roughness_image >= 0) {
                    mr_descriptor = descriptors[material.metallic_roughness_image];
                }
                if (material.occlusion_image >= 0) {
                    occlusion_descriptor = descriptors[material.occlusion_image];
                }
            }
            draw.base_color_texture = TextureHandle(descriptor);
            draw.normal_texture = TextureHandle(normal_descriptor);
            draw.metallic_roughness_texture = TextureHandle(mr_descriptor);
            draw.occlusion_texture = TextureHandle(occlusion_descriptor);

            gpu.primitives.push_back(draw);
        }
    }

    // The HUD font atlases take the descriptors after the last model image, and
    // ride the same command list and staging buffers to the GPU. LoadFontAtlas
    // claims two slots: the Inter face here and the monospace face right after it.
    atlas_descriptor_ = next_descriptor;
    LoadFontAtlas(staging);

    // The shadow map's resource view takes the descriptor after the atlases. The
    // texture itself was already created in CreateShadowMap; only its SRV lands
    // here, in the shader-visible heap the scene pass binds. Writing a descriptor
    // is immediate, so it needs no command list of its own.
    shadow_descriptor_ = mono_atlas_descriptor_ + 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC shadow_srv{};
    shadow_srv.Format = kShadowSrvFormat;
    shadow_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    shadow_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shadow_srv.Texture2D.MipLevels = 1;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE shadow_handle(
        texture_heap_->GetCPUDescriptorHandleForHeapStart(),
        static_cast<INT>(shadow_descriptor_), texture_size_);
    device_->CreateShaderResourceView(shadow_map_.Get(), &shadow_srv, shadow_handle);

    // The reflection probe's cube SRV takes the descriptor after the shadow map.
    // The cube itself is created and filled in CaptureReflectionProbe; only the
    // slot is reserved here so the scene root signature can point at it.
    probe_descriptor_ = shadow_descriptor_ + 1;

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    // The staging buffers are released when this function returns, so the copies
    // have to have landed before it does.
    FlushGpu();
}

void Renderer::CaptureReflectionProbe(const Scene& scene) {
    // The cube: six square faces, one mip, typeless so it renders as _UNORM (the
    // capture writes display-space colour, as the scene pass does to the back
    // buffer) and samples as sRGB (decoded to linear light). A render-target
    // resource must declare its clear value up front.
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kProbeRtvFormat;
    clear.Color[0] = kSkyColor[0];
    clear.Color[1] = kSkyColor[1];
    clear.Color[2] = kSkyColor[2];
    clear.Color[3] = 1.0f;

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC1 cube_desc =
        CD3DX12_RESOURCE_DESC1::Tex2D(kProbeResourceFormat, kProbeSize, kProbeSize, 6, 1, 1, 0,
                                      D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    // Born a shader resource, the layout it rests in once the capture below has
    // filled it and the scene pass reflects off it.
    ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &cube_desc,
                                                    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear,
                                                    nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&probe_cube_)),
                  "CreateCommittedResource3(probe cube)");

    // One render-target view per face.
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.NumDescriptors = 6;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&probe_rtv_heap_)),
                  "CreateDescriptorHeap(probe RTV)");
    for (UINT face = 0; face < 6; ++face) {
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = kProbeRtvFormat;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray.FirstArraySlice = face;
        rtv.Texture2DArray.ArraySize = 1;
        rtv.Texture2DArray.MipSlice = 0;
        const CD3DX12_CPU_DESCRIPTOR_HANDLE handle(
            probe_rtv_heap_->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(face),
            rtv_size_);
        device_->CreateRenderTargetView(probe_cube_.Get(), &rtv, handle);
    }

    // The square depth buffer the capture tests against, in DSV slot 2.
    D3D12_CLEAR_VALUE depth_clear{};
    depth_clear.Format = kDepthFormat;
    depth_clear.DepthStencil.Depth = 1.0f;
    const CD3DX12_RESOURCE_DESC1 depth_desc = CD3DX12_RESOURCE_DESC1::Tex2D(
        kDepthFormat, kProbeSize, kProbeSize, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ThrowIfFailed(device_->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &depth_desc,
                                                    D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                                                    &depth_clear, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&probe_depth_)),
                  "CreateCommittedResource3(probe depth)");
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE probe_dsv(dsv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                  static_cast<INT>(kProbeDsvIndex), dsv_size_);
    device_->CreateDepthStencilView(probe_depth_.Get(), &dsv, probe_dsv);

    // The cube's SRV, in the slot CreateSceneGeometry reserved for it.
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kProbeSrvFormat;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.TextureCube.MipLevels = 1;
    const CD3DX12_CPU_DESCRIPTOR_HANDLE srv_handle(texture_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                   static_cast<INT>(probe_descriptor_), texture_size_);
    device_->CreateShaderResourceView(probe_cube_.Get(), &srv, srv_handle);

    // The capture is lit from the eye at the probe centre; write that into frame
    // constants region 0 and aim the scene pass's CBV at it. Nothing is in flight
    // yet, so region 0 is free to borrow.
    FrameConstants frame{};
    frame.light_view_projection = light_view_projection_;
    frame.camera_position = kProbePosition;
    ApplyEnvironment(frame, environment_);
    std::memcpy(frame_constants_mapped_, &frame, sizeof(frame));
    frame_constants_address_ = frame_constants_->GetGPUVirtualAddress();

    // The six face cameras: the standard cube directions and ups, a 90-degree
    // frustum. Each is just another left-handed camera, so the same back-face cull
    // the main view uses renders each face correctly.
    struct Face {
        XMVECTOR dir;
        XMVECTOR up;
    };
    const Face faces[6] = {
        {XMVectorSet(1, 0, 0, 0), XMVectorSet(0, 1, 0, 0)},   // +X
        {XMVectorSet(-1, 0, 0, 0), XMVectorSet(0, 1, 0, 0)},  // -X
        {XMVectorSet(0, 1, 0, 0), XMVectorSet(0, 0, -1, 0)},  // +Y
        {XMVectorSet(0, -1, 0, 0), XMVectorSet(0, 0, 1, 0)},  // -Y
        {XMVectorSet(0, 0, 1, 0), XMVectorSet(0, 1, 0, 0)},   // +Z
        {XMVectorSet(0, 0, -1, 0), XMVectorSet(0, 1, 0, 0)},  // -Z
    };
    const XMVECTOR eye = XMLoadFloat3(&kProbePosition);
    const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, 0.05f, 100.0f);

    // The level's sun (set by SetSunDirection before this capture runs), so the
    // probe bakes reflections lit the same way as the frame that samples it.
    const XMFLOAT3 sun = sun_direction_;

    // Record the capture on the one-shot command list CreateSceneGeometry left
    // closed.
    ThrowIfFailed(allocators_[frame_index_]->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocators_[frame_index_].Get(), nullptr),
                  "CommandList::Reset");

    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    const CD3DX12_VIEWPORT viewport(0.0f, 0.0f, static_cast<float>(kProbeSize),
                                    static_cast<float>(kProbeSize));
    const CD3DX12_RECT scissor(0, 0, static_cast<LONG>(kProbeSize), static_cast<LONG>(kProbeSize));
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);

    // Every face from its resting shader-resource layout to a render target. The
    // cube was just created and nothing has touched it, so there is no prior work to
    // drain -- SYNC_NONE with no access before.
    TextureBarrier(command_list_.Get(), probe_cube_.Get(), D3D12_BARRIER_SYNC_NONE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    for (UINT face = 0; face < 6; ++face) {
        const XMMATRIX view_projection = XMMatrixLookToLH(eye, faces[face].dir, faces[face].up) * proj;

        const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(
            probe_rtv_heap_->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(face),
            rtv_size_);
        command_list_->OMSetRenderTargets(1, &rtv, FALSE, &probe_dsv);
        command_list_->ClearRenderTargetView(rtv, kSkyColor, 0, nullptr);
        command_list_->ClearDepthStencilView(probe_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // The gradient sky behind, then the static yard lit by the analytic sky.
        // Time 0 bakes a still cloudscape into the probe the metals reflect.
        DrawSky(view_projection, kProbePosition, 0.0f, true);
        command_list_->SetGraphicsRootSignature(root_signature_.Get());
        command_list_->SetPipelineState(scene_capture_pipeline_state_.Get());
        command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        DrawInstances(scene.Instances(), view_projection, sun, 0.0f, false);
    }

    // And back to a shader resource for the scene pass to reflect off of: the face
    // renders must drain before any pixel-shader sample of the cube.
    TextureBarrier(command_list_.Get(), probe_cube_.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                   D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);
    FlushGpu();
}

void Renderer::LoadFontFace(const std::filesystem::path& csv, const std::filesystem::path& png,
                            UINT descriptor, Font& font, ComPtr<ID3D12Resource>& texture,
                            UINT& width, UINT& height,
                            std::vector<ComPtr<ID3D12Resource>>& staging) {
    font = LoadFontCsv(csv);

    Image atlas = DecodeImage(ReadBinaryFile(png));
    width = atlas.width;
    height = atlas.height;

    // Deliberately no mip chain: a lower mip of a distance-field atlas averages
    // neighbouring glyphs' distances together, so the edges bleed and the text
    // turns to mush the moment it is minified. DecodeImage built the chain; drop
    // all but the full-resolution level before the upload.
    atlas.levels.resize(1);
    texture = UploadTexture(atlas, descriptor, kTextureFormat, staging);
}

void Renderer::LoadFontAtlas(std::vector<ComPtr<ID3D12Resource>>& staging) {
    const std::filesystem::path dir = ExecutableDirectory() / "assets" / "fonts";
    // The Inter HUD face at atlas_descriptor_, then the monospace debug face in the
    // slot straight after it.
    LoadFontFace(dir / "hud.csv", dir / "hud.png", atlas_descriptor_, font_, atlas_texture_,
                 atlas_width_, atlas_height_, staging);
    mono_atlas_descriptor_ = atlas_descriptor_ + 1;
    LoadFontFace(dir / "mono.csv", dir / "mono.png", mono_atlas_descriptor_, mono_font_,
                 mono_atlas_texture_, mono_atlas_width_, mono_atlas_height_, staging);
}

void Renderer::CreateTextPipeline() {
    // b0: the root constants shared by both stages. t0: the atlas, in the same
    // shader-visible heap the scene binds. s0: a linear clamp sampler, baked in.
    const CD3DX12_DESCRIPTOR_RANGE atlas_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsConstants(kTextConstantDwords, 0);
    parameters[1].InitAsDescriptorTable(1, &atlas_range, D3D12_SHADER_VISIBILITY_PIXEL);

    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                        D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(_countof(parameters), parameters, 1, &sampler,
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                       D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS);

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    HRESULT hr =
        D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error);
    if (FAILED(hr)) {
        std::string message = error
                                  ? std::string(static_cast<const char*>(error->GetBufferPointer()),
                                                error->GetBufferSize())
                                  : "unknown error";
        throw std::runtime_error("D3D12SerializeRootSignature(text) failed: " + message);
    }
    ThrowIfFailed(device_->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&text_root_signature_)),
                  "CreateRootSignature(text)");

    const std::filesystem::path shader_dir = ExecutableDirectory() / "shaders";
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "text.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "text.ps.cso");

    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,
         0},
    };
    static_assert(sizeof(TextVertex) == 16, "The input layout above spells out TextVertex");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {input_layout, _countof(input_layout)};
    pso.pRootSignature = text_root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};

    // The glyph quads have no consistent winding, so cull nothing.
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

    // Straight-alpha blend: the shader's coverage is the source alpha, and the
    // text sits over the finished frame. The colour writes are alpha-weighted;
    // the destination alpha is left as it was.
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // The HUD sits on top of everything, so it neither tests nor writes depth.
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;

    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = kBackBufferFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&text_pipeline_state_)),
                  "CreateGraphicsPipelineState(text)");

    // One upload-heap region per frame in flight, mapped once and left mapped.
    // MoveToNextFrame retires a frame before its region is reused, so the CPU
    // never writes over quads the GPU is still reading.
    const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC buffer_desc =
        CD3DX12_RESOURCE_DESC::Buffer(static_cast<UINT64>(kTextRegionBytes) * kFrameCount);
    ThrowIfFailed(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&text_vertex_buffer_)),
                  "CreateCommittedResource(text vertices)");

    void* mapped = nullptr;
    const CD3DX12_RANGE no_read(0, 0);
    ThrowIfFailed(text_vertex_buffer_->Map(0, &no_read, &mapped), "Text vertices::Map");
    text_vertex_mapped_ = static_cast<std::byte*>(mapped);
}

UINT Renderer::LayoutLine(const FontFace& face, std::string_view text, float baseline, float pixel,
                          UINT first, float left_px) {
    if (text.empty()) {
        return first;
    }

    // Left-anchored lines (left_px >= 0) start at that x; otherwise the line is
    // centred -- its total advance sets the left edge, then the pen walks right.
    float pen_x = left_px;
    if (left_px < 0.0f) {
        float total_width = 0.0f;
        for (const char c : text) {
            if (const Glyph* glyph = face.font->Find(static_cast<unsigned char>(c))) {
                total_width += glyph->advance * pixel;
            }
        }
        pen_x = (static_cast<float>(width_) - total_width) * 0.5f;
    }

    const float atlas_w = static_cast<float>(face.atlas_width);
    const float atlas_h = static_cast<float>(face.atlas_height);

    // Pixel coordinates (origin top-left) into clip space.
    auto to_ndc = [this](float px, float py) {
        return XMFLOAT2{px / static_cast<float>(width_) * 2.0f - 1.0f,
                        1.0f - py / static_cast<float>(height_) * 2.0f};
    };

    auto* vertices = reinterpret_cast<TextVertex*>(text_vertex_mapped_ +
                                                   static_cast<size_t>(frame_index_) *
                                                       kTextRegionBytes);
    UINT count = first;
    for (const char c : text) {
        const Glyph* glyph = face.font->Find(static_cast<unsigned char>(c));
        if (glyph == nullptr) {
            continue;
        }
        if (glyph->visible && count + kTextVerticesPerGlyph <= kTextRegionVertices) {
            // The `b` corners share plane_b and atlas_b, the `t` corners share
            // plane_t and atlas_t; both axes point down, so this lands upright.
            const float xl = pen_x + glyph->plane_l * pixel;
            const float xr = pen_x + glyph->plane_r * pixel;
            const float yb = baseline + glyph->plane_b * pixel;
            const float yt = baseline + glyph->plane_t * pixel;

            const float ul = glyph->atlas_l / atlas_w;
            const float ur = glyph->atlas_r / atlas_w;
            const float vb = glyph->atlas_b / atlas_h;
            const float vt = glyph->atlas_t / atlas_h;

            const TextVertex lb{to_ndc(xl, yb), {ul, vb}};
            const TextVertex rb{to_ndc(xr, yb), {ur, vb}};
            const TextVertex rt{to_ndc(xr, yt), {ur, vt}};
            const TextVertex lt{to_ndc(xl, yt), {ul, vt}};

            vertices[count++] = lb;
            vertices[count++] = rb;
            vertices[count++] = rt;
            vertices[count++] = lb;
            vertices[count++] = rt;
            vertices[count++] = lt;
        }
        pen_x += glyph->advance * pixel;
    }
    return count;
}

void Renderer::BindTextPipeline() {
    command_list_->SetPipelineState(text_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(text_root_signature_.Get());
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->SetGraphicsRootDescriptorTable(1, TextureHandle(atlas_descriptor_));

    // The whole frame region, so a run drawn from any start vertex is in bounds.
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = text_vertex_buffer_->GetGPUVirtualAddress() +
                         static_cast<UINT64>(frame_index_) * kTextRegionBytes;
    vbv.StrideInBytes = sizeof(TextVertex);
    vbv.SizeInBytes = kTextRegionBytes;
    command_list_->IASetVertexBuffers(0, 1, &vbv);
}

void Renderer::DrawTextRun(const FontFace& face, UINT first, UINT count, XMFLOAT4 color) {
    if (count == 0) {
        return;
    }

    // This run's glyphs come from `face`'s atlas, so bind it and size the distance
    // field to its dimensions -- the two faces' atlases differ in size.
    command_list_->SetGraphicsRootDescriptorTable(1, TextureHandle(face.atlas_descriptor));

    TextConstants constants{};
    constants.unit_range = {kDistanceRange / static_cast<float>(face.atlas_width),
                            kDistanceRange / static_cast<float>(face.atlas_height)};

    // A soft drop shadow first, nudged down and right, so the text reads over a
    // bright sky or a dark fence alike; then the text itself over the top. The
    // shadow tracks the fill's alpha so a faded line fades its shadow with it.
    constants.color = {0.0f, 0.0f, 0.0f, 0.75f * color.w};
    constants.ndc_offset = {2.0f * kTextShadowPixels / static_cast<float>(width_),
                            -2.0f * kTextShadowPixels / static_cast<float>(height_)};
    command_list_->SetGraphicsRoot32BitConstants(0, kTextConstantDwords, &constants, 0);
    command_list_->DrawInstanced(count, 1, first, 0);

    constants.color = color;
    constants.ndc_offset = {0.0f, 0.0f};
    command_list_->SetGraphicsRoot32BitConstants(0, kTextConstantDwords, &constants, 0);
    command_list_->DrawInstanced(count, 1, first, 0);
}

UINT Renderer::LayoutSolidQuad(float x0, float y0, float x1, float y1, UINT first) {
    // One rectangle costs the same six vertices a glyph does; bail if it won't fit.
    if (first + kTextVerticesPerGlyph > kTextRegionVertices) {
        return first;
    }

    auto to_ndc = [this](float px, float py) {
        return XMFLOAT2{px / static_cast<float>(width_) * 2.0f - 1.0f,
                        1.0f - py / static_cast<float>(height_) * 2.0f};
    };

    auto* vertices = reinterpret_cast<TextVertex*>(text_vertex_mapped_ +
                                                   static_cast<size_t>(frame_index_) *
                                                       kTextRegionBytes);
    // The uv is unused in solid mode, so any value does.
    const TextVertex lt{to_ndc(x0, y0), {0.0f, 0.0f}};
    const TextVertex rt{to_ndc(x1, y0), {0.0f, 0.0f}};
    const TextVertex rb{to_ndc(x1, y1), {0.0f, 0.0f}};
    const TextVertex lb{to_ndc(x0, y1), {0.0f, 0.0f}};

    UINT count = first;
    vertices[count++] = lt;
    vertices[count++] = rt;
    vertices[count++] = rb;
    vertices[count++] = lt;
    vertices[count++] = rb;
    vertices[count++] = lb;
    return count;
}

void Renderer::DrawSolidRun(UINT first, UINT count, XMFLOAT4 color) {
    if (count == 0) {
        return;
    }
    TextConstants constants{};
    constants.color = color;
    constants.solid = 1.0f;
    command_list_->SetGraphicsRoot32BitConstants(0, kTextConstantDwords, &constants, 0);
    command_list_->DrawInstanced(count, 1, first, 0);
}

void Renderer::DrawHud(std::string_view prompt, std::span<const std::string> debug_lines,
                       std::span<const OrderCard> orders, std::span<const MeatCard> meats) {
    if (width_ == 0 || height_ == 0) {
        return;
    }

    // Pack the panel, the prompt, every debug line and the objective rail into this
    // frame's text region first, remembering each one's vertex range, colour and kind,
    // then draw them all in order -- so the runs never overwrite one another in the
    // shared buffer (as DrawMenu does), and each panel, packed before the glyphs that
    // sit over it, is drawn behind them.
    std::vector<HudRun> runs;
    runs.reserve(debug_lines.size() + 2);
    UINT cursor = 0;

    // The pick-up prompt: one white line centred a little above the bottom.
    if (!prompt.empty()) {
        const float pixel = static_cast<float>(height_) * kTextHeightFraction;
        const float baseline = static_cast<float>(height_) - pixel * 2.2f;
        const UINT first = cursor;
        cursor = LayoutLine(HudFace(), prompt, baseline, pixel, cursor);
        runs.push_back({first, cursor - first, XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f}, false, HudFace()});
    }

    // The debug overlay: white monospace lines in the bottom-left corner, backed by a
    // translucent black panel so they read over any background. Anchored to the bottom so
    // it clears the top-left meats panel: the block stacks downward from first_baseline as
    // before, but first_baseline is placed so the last line lands a small inset above the
    // bottom edge -- the vertical mirror of the top inset it used to sit at.
    const FontFace mono = MonoFace();
    const float debug_pixel = static_cast<float>(height_) * kDebugTextHeightFraction;
    const float left = debug_pixel * 0.6f;      // A small inset from the left edge.
    const float line_pitch = debug_pixel * kDebugTextLineFactor;
    const float last_baseline = static_cast<float>(height_) - debug_pixel * 1.6f;
    const float first_baseline =
        last_baseline -
        (debug_lines.empty() ? 0.0f : static_cast<float>(debug_lines.size() - 1)) * line_pitch;

    if (!debug_lines.empty()) {
        // Size the panel to the widest line, padded, and to the run of baselines --
        // reaching above the tallest cap and below the lowest descender.
        auto line_width = [&mono](const std::string& line, float pixel) {
            float w = 0.0f;
            for (const char c : line) {
                if (const Glyph* glyph = mono.font->Find(static_cast<unsigned char>(c))) {
                    w += glyph->advance * pixel;
                }
            }
            return w;
        };
        float widest = 0.0f;
        for (const std::string& line : debug_lines) {
            widest = std::max(widest, line_width(line, debug_pixel));
        }
        const float pad_x = debug_pixel * 0.5f;
        const float pad_y = debug_pixel * 0.35f;
        const float x0 = left - pad_x;
        const float x1 = left + widest + pad_x;
        const float y0 = first_baseline - debug_pixel * 0.85f - pad_y;
        const float y1 = last_baseline + debug_pixel * 0.30f + pad_y;

        const UINT first = cursor;
        cursor = LayoutSolidQuad(x0, y0, x1, y1, cursor);
        runs.push_back({first, cursor - first, XMFLOAT4{0.0f, 0.0f, 0.0f, 0.5f}, true, mono});
    }

    const XMFLOAT4 debug_color{1.0f, 1.0f, 1.0f, 1.0f};
    float baseline = first_baseline;
    for (const std::string& line : debug_lines) {
        const UINT first = cursor;
        cursor = LayoutLine(mono, line, baseline, debug_pixel, cursor, left);
        runs.push_back({first, cursor - first, debug_color, false, mono});
        baseline += line_pitch;
    }

    // The polished, non-debug half: the orders list down the top-right corner and the
    // meats rail down the top-left, both packed into the same buffer so they draw in the
    // same batch as the prompt and overlay.
    DrawObjectivesRail(orders, runs, cursor);
    DrawMeatRail(meats, runs, cursor);

    if (cursor == 0) {
        return;
    }

    BindTextPipeline();
    for (const HudRun& run : runs) {
        if (run.solid) {
            DrawSolidRun(run.first, run.count, run.color);
        } else {
            DrawTextRun(run.face, run.first, run.count, run.color);
        }
    }
}

float Renderer::TextWidth(const FontFace& face, std::string_view text, float pixel) const {
    float w = 0.0f;
    for (const char c : text) {
        if (const Glyph* glyph = face.font->Find(static_cast<unsigned char>(c))) {
            w += glyph->advance * pixel;
        }
    }
    return w;
}

void Renderer::DrawObjectivesRail(std::span<const OrderCard> orders, std::vector<HudRun>& runs,
                                  UINT& cursor) {
    if (orders.empty()) {
        return;
    }

    const float h = static_cast<float>(height_);
    const float w = static_cast<float>(width_);

    // A plain bulleted list of what to cook -- no progress, no doneness gauge, no
    // success/failure: just the standing orders. Right-anchored in the top-right corner
    // to mirror the meats panel on the left. Every metric is a fraction of the back-buffer
    // height so the list keeps its proportions across resolutions.
    const FontFace face = HudFace();
    const float text_pixel = h * 0.019f;
    const float line_pitch = text_pixel * 1.75f; // Baseline-to-baseline within the list.
    const float pad = text_pixel * 0.9f;         // Inner padding of the panel.
    const float bullet = text_pixel * 0.24f;     // The bullet dot's side.
    const float bullet_gap = text_pixel * 0.7f;  // Dot-to-text gap.
    const float col_gap = text_pixel * 0.8f;     // Name-to-doneness gap on a line.
    const float margin = h * 0.04f;

    // The warm palette the menu and meats panel already use: amber names and bullets, a
    // quiet grey for the doneness, over a translucent panel.
    const XMFLOAT4 panel_color{0.04f, 0.03f, 0.02f, 0.62f};
    const XMFLOAT4 name_color{1.0f, 0.82f, 0.48f, 1.0f};
    const XMFLOAT4 band_color{0.72f, 0.72f, 0.75f, 0.95f};
    const XMFLOAT4 bullet_color{1.0f, 0.70f, 0.30f, 0.95f};

    // Shape each line's two strings once and measure the widest, so the panel hugs the
    // list. The name carries the count when more than one is wanted ("PATTY x2").
    struct Line {
        std::string name;
        std::string band;
        float name_w;
    };
    std::vector<Line> lines;
    lines.reserve(orders.size());
    float widest = 0.0f;
    for (const OrderCard& order : orders) {
        std::string name = order.name;
        if (order.count > 1) {
            name += " x" + std::to_string(order.count);
        }
        const float name_w = TextWidth(face, name, text_pixel);
        const float band_w = TextWidth(face, order.band, text_pixel);
        widest = std::max(widest, name_w + (order.band.empty() ? 0.0f : col_gap + band_w));
        lines.push_back({std::move(name), order.band, name_w});
    }

    const float panel_w = bullet + bullet_gap + widest + 2.0f * pad;
    const float x1 = w - margin;
    const float x0 = x1 - panel_w;
    const float top = margin;

    // The "ORDERS" header, right-anchored over the panel -- the twin of the meats "MEATS".
    {
        const float header_pixel = h * 0.020f;
        const std::string_view header = "ORDERS";
        const float header_w = TextWidth(face, header, header_pixel);
        const float header_baseline = margin - header_pixel * 0.4f;
        const UINT first = cursor;
        cursor = LayoutLine(face, header, header_baseline, header_pixel, cursor, x1 - header_w);
        runs.push_back({first, cursor - first, name_color, false, face});
    }

    // One translucent panel behind the whole list, packed first so the text sits over it.
    const float panel_h =
        2.0f * pad + static_cast<float>(lines.size() - 1) * line_pitch + text_pixel;
    {
        const UINT first = cursor;
        cursor = LayoutSolidQuad(x0, top, x1, top + panel_h, cursor);
        runs.push_back({first, cursor - first, panel_color, true, face});
    }

    const float text_x = x0 + pad + bullet + bullet_gap;
    float baseline = top + pad + text_pixel;
    for (const Line& line : lines) {
        // The bullet: a small amber square centred on the text's x-height.
        const float by = baseline - text_pixel * 0.30f - bullet * 0.5f;
        const float bx = x0 + pad;
        {
            const UINT first = cursor;
            cursor = LayoutSolidQuad(bx, by, bx + bullet, by + bullet, cursor);
            runs.push_back({first, cursor - first, bullet_color, true, face});
        }
        // The meat's name (with its count), amber.
        {
            const UINT first = cursor;
            cursor = LayoutLine(face, line.name, baseline, text_pixel, cursor, text_x);
            runs.push_back({first, cursor - first, name_color, false, face});
        }
        // The wanted doneness, quiet grey, following the name on the same line.
        if (!line.band.empty()) {
            const UINT first = cursor;
            cursor = LayoutLine(face, line.band, baseline, text_pixel, cursor,
                                text_x + line.name_w + col_gap);
            runs.push_back({first, cursor - first, band_color, false, face});
        }
        baseline += line_pitch;
    }
}

void Renderer::DrawMeatRail(std::span<const MeatCard> meats, std::vector<HudRun>& runs,
                            UINT& cursor) {
    if (meats.empty()) {
        return;
    }

    const float h = static_cast<float>(height_);

    // Share the objective rail's metrics so the two rails read as one UI, mirrored to
    // the left. The name is the loud line; the doneness caption below is quieter.
    const FontFace face = HudFace();
    const float name_pixel = h * 0.024f;
    const float caption_pixel = h * 0.016f;
    const float pad = name_pixel * 0.6f;      // Inner padding on all four sides.
    const float card_w = h * 0.28f;           // Uniform card width.
    const float gauge_h = name_pixel * 0.42f; // The doneness bar's thickness.
    const float row_gap = name_pixel * 0.55f; // Vertical gap between a card's rows.
    const float card_gap = name_pixel * 0.6f; // Gap between stacked cards.

    // Left-anchored, inset from the left edge by the same margin the order rail sits
    // in from the right and both sit below the top.
    const float margin = h * 0.04f;
    const float card_h = pad + name_pixel + row_gap + gauge_h + row_gap + caption_pixel + pad;
    const float x0 = margin;
    const float x1 = x0 + card_w;

    // The same warm palette as the order rail: amber for a cooking meat, a calm green
    // once it has been served, and a dim wash for the unreached part of the gauge.
    const XMFLOAT4 panel_color{0.04f, 0.03f, 0.02f, 0.62f};
    const XMFLOAT4 name_color{1.0f, 0.82f, 0.48f, 1.0f};
    const XMFLOAT4 served_color{0.55f, 0.85f, 0.45f, 1.0f};
    const XMFLOAT4 caption_color{0.72f, 0.72f, 0.75f, 0.9f};
    const XMFLOAT4 gauge_on{1.0f, 0.70f, 0.30f, 0.95f};
    const XMFLOAT4 gauge_off{1.0f, 1.0f, 1.0f, 0.12f};

    auto fade = [](XMFLOAT4 c, float a) {
        c.w *= a;
        return c;
    };

    // A header over the stack, the left twin of the ORDERS header on the right.
    {
        const float header_pixel = h * 0.020f;
        const float header_baseline = margin - header_pixel * 0.4f;
        const UINT first = cursor;
        cursor = LayoutLine(face, "MEATS", header_baseline, header_pixel, cursor, x0);
        runs.push_back({first, cursor - first, name_color, false, face});
    }

    float top = margin;
    for (const MeatCard& meat : meats) {
        // A served meat has done its job; fade the whole card back so the eye stays on
        // the ones still cooking.
        const float alpha = meat.served ? 0.5f : 1.0f;

        // The card's translucent backing, packed first so its glyphs and bar sit over it.
        {
            const UINT first = cursor;
            cursor = LayoutSolidQuad(x0, top, x1, top + card_h, cursor);
            runs.push_back({first, cursor - first, fade(panel_color, alpha), true, face});
        }

        // Row 1 -- the meat's name on the left, and once handed off a small green
        // "SERVED" tag on the right where an order card would show its pips.
        const float name_baseline = top + pad + name_pixel;
        {
            const UINT first = cursor;
            cursor = LayoutLine(face, meat.name, name_baseline, name_pixel, cursor, x0 + pad);
            runs.push_back({first, cursor - first,
                            fade(meat.served ? served_color : name_color, alpha), false, face});
        }
        if (meat.served) {
            const std::string_view tag = "SERVED";
            const float tag_w = TextWidth(face, tag, caption_pixel);
            const UINT first = cursor;
            cursor = LayoutLine(face, tag, name_baseline - name_pixel * 0.12f, caption_pixel,
                                cursor, x1 - pad - tag_w);
            runs.push_back({first, cursor - first, fade(served_color, alpha), false, face});
        }

        // Row 2 -- the doneness gauge: one segment per band, filled through the meat's
        // current band so its cook reads as a level rising across the bar, with a bright
        // tick over the current band marking exactly where the cook sits.
        const float gauge_top = name_baseline + row_gap;
        {
            const int bands = std::max(meat.band_count, 1);
            const int band = std::clamp(meat.band_index, 0, bands - 1);
            const float seg_gap = card_w * 0.008f;
            const float inner = card_w - 2.0f * pad;
            const float seg_w =
                (inner - static_cast<float>(bands - 1) * seg_gap) / static_cast<float>(bands);
            float sx = x0 + pad;
            for (int i = 0; i < bands; ++i) {
                const bool lit = i <= band;
                const UINT first = cursor;
                cursor = LayoutSolidQuad(sx, gauge_top, sx + seg_w, gauge_top + gauge_h, cursor);
                runs.push_back(
                    {first, cursor - first, fade(lit ? gauge_on : gauge_off, alpha), true, face});
                sx += seg_w + seg_gap;
            }
            const float cx =
                x0 + pad + static_cast<float>(band) * (seg_w + seg_gap) + seg_w * 0.5f;
            const float half = std::max(card_w * 0.006f, 1.5f);
            const float over = gauge_h * 0.6f;
            const XMFLOAT4 marker_color{1.0f, 0.97f, 0.9f, 0.95f};
            const UINT first = cursor;
            cursor = LayoutSolidQuad(cx - half, gauge_top - over, cx + half,
                                     gauge_top + gauge_h + over, cursor);
            runs.push_back({first, cursor - first, fade(marker_color, alpha), true, face});
        }

        // Row 3 -- the current doneness in words on the left, and the meat's live internal
        // temperature right-anchored beside it, both quiet and small beneath the bar: the
        // word for a glance, the number for a read (as the debug overlay pairs them).
        const float caption_baseline = gauge_top + gauge_h + row_gap + caption_pixel;
        if (!meat.band.empty()) {
            const UINT first = cursor;
            cursor =
                LayoutLine(face, meat.band, caption_baseline, caption_pixel, cursor, x0 + pad);
            runs.push_back({first, cursor - first, fade(caption_color, alpha), false, face});
        }
        if (!meat.temp.empty()) {
            const float temp_w = TextWidth(face, meat.temp, caption_pixel);
            const UINT first = cursor;
            cursor = LayoutLine(face, meat.temp, caption_baseline, caption_pixel, cursor,
                                x1 - pad - temp_w);
            runs.push_back({first, cursor - first, fade(caption_color, alpha), false, face});
        }

        top += card_h + card_gap;
    }
}

void Renderer::DrawMenu(std::string_view title, std::span<const std::string> entries,
                        int selected) {
    if (width_ == 0 || height_ == 0) {
        return;
    }

    const float h = static_cast<float>(height_);
    const float title_pixel = h * kMenuTitleFraction;
    const float entry_pixel = h * kMenuEntryFraction;
    const float spacing = entry_pixel * kMenuEntrySpacingFactor;

    // The warm palette: an amber title, the selected entry the same amber the
    // pick-up glow uses, and the rest a quiet grey. Locals rather than file
    // constants so no XMFLOAT4 needs to be constexpr-constructible.
    const XMFLOAT4 title_color{1.0f, 0.82f, 0.48f, 1.0f};
    const XMFLOAT4 selected_color{1.0f, 0.78f, 0.35f, 1.0f};
    const XMFLOAT4 entry_color{0.72f, 0.72f, 0.75f, 1.0f};

    // Pack every line into this frame's text region first, remembering each one's
    // vertex range and colour, then draw them all -- so the lines do not overwrite
    // one another in the shared buffer.
    struct Run {
        UINT first;
        UINT count;
        XMFLOAT4 color;
    };
    std::vector<Run> runs;
    runs.reserve(entries.size() + 1);

    // The menu draws from the Inter HUD face, like the prompt.
    const FontFace face = HudFace();
    UINT cursor = 0;
    const UINT title_first = cursor;
    cursor = LayoutLine(face, title, h * kMenuTitleBaselineFraction, title_pixel, cursor);
    runs.push_back({title_first, cursor - title_first, title_color});

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const bool is_selected = static_cast<int>(i) == selected;
        // Carets flank the selected entry so the highlight reads even in a still
        // screenshot, not only by its colour.
        const std::string line = is_selected ? "> " + entries[i] + " <" : entries[i];
        const float baseline =
            h * kMenuFirstEntryBaselineFraction + static_cast<float>(i) * spacing;
        const UINT entry_first = cursor;
        cursor = LayoutLine(face, line, baseline, entry_pixel, cursor);
        runs.push_back(
            {entry_first, cursor - entry_first, is_selected ? selected_color : entry_color});
    }

    if (cursor == 0) {
        return;
    }

    BindTextPipeline();
    for (const Run& run : runs) {
        DrawTextRun(face, run.first, run.count, run.color);
    }
}

void Renderer::DrawLoading(std::string_view title, std::string_view subtitle) {
    if (width_ == 0 || height_ == 0) {
        return;
    }

    const float h = static_cast<float>(height_);
    const float title_pixel = h * kMenuTitleFraction;
    const float subtitle_pixel = h * kMenuEntryFraction;

    // The same warm palette the menu uses: an amber title over a quiet grey subtitle.
    const XMFLOAT4 title_color{1.0f, 0.82f, 0.48f, 1.0f};
    const XMFLOAT4 subtitle_color{0.72f, 0.72f, 0.75f, 1.0f};

    struct Run {
        UINT first;
        UINT count;
        XMFLOAT4 color;
    };
    std::vector<Run> runs;

    const FontFace face = HudFace();
    UINT cursor = 0;
    const UINT title_first = cursor;
    cursor = LayoutLine(face, title, h * kMenuTitleBaselineFraction, title_pixel, cursor);
    runs.push_back({title_first, cursor - title_first, title_color});

    if (!subtitle.empty()) {
        const UINT subtitle_first = cursor;
        cursor = LayoutLine(face, subtitle, h * kMenuFirstEntryBaselineFraction, subtitle_pixel,
                            cursor);
        runs.push_back({subtitle_first, cursor - subtitle_first, subtitle_color});
    }

    if (cursor == 0) {
        return;
    }

    BindTextPipeline();
    for (const Run& run : runs) {
        DrawTextRun(face, run.first, run.count, run.color);
    }
}

void Renderer::DrawResults(std::string_view title, bool passed,
                           std::span<const ResultLine> lines,
                           std::span<const std::string> actions, int selected) {
    if (width_ == 0 || height_ == 0) {
        return;
    }

    const float h = static_cast<float>(height_);
    const float title_pixel = h * kResultsTitleFraction;
    const float line_pixel = h * kResultsLineFraction;
    const float line_spacing = line_pixel * kResultsLineSpacingFactor;
    const float action_pixel = h * kResultsActionFraction;
    const float action_spacing = action_pixel * kResultsActionSpacingFactor;

    // A green title on a clear, a red one on a miss; met orders green and missed ones red;
    // the selected action the same warm amber the launch menu picks with, the rest grey.
    const XMFLOAT4 pass_color{0.55f, 0.85f, 0.45f, 1.0f};
    const XMFLOAT4 fail_color{0.92f, 0.42f, 0.38f, 1.0f};
    const XMFLOAT4 met_color{0.60f, 0.82f, 0.52f, 1.0f};
    const XMFLOAT4 missed_color{0.90f, 0.48f, 0.44f, 1.0f};
    const XMFLOAT4 selected_color{1.0f, 0.78f, 0.35f, 1.0f};
    const XMFLOAT4 action_color{0.72f, 0.72f, 0.75f, 1.0f};

    struct Run {
        UINT first;
        UINT count;
        XMFLOAT4 color;
    };
    std::vector<Run> runs;
    runs.reserve(lines.size() + actions.size() + 1);

    const FontFace face = HudFace();
    UINT cursor = 0;
    const auto emit = [&](std::string_view text, float baseline, float pixel, XMFLOAT4 color) {
        const UINT first = cursor;
        cursor = LayoutLine(face, text, baseline, pixel, cursor); // centred (default left)
        if (cursor > first) {
            runs.push_back({first, cursor - first, color});
        }
    };

    // Title, then the order breakdown, then the actions -- each block at its own baseline.
    emit(title, h * kResultsTitleBaselineFraction, title_pixel, passed ? pass_color : fail_color);

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const float baseline =
            h * kResultsFirstLineBaselineFraction + static_cast<float>(i) * line_spacing;
        emit(lines[i].text, baseline, line_pixel, lines[i].met ? met_color : missed_color);
    }

    for (std::size_t i = 0; i < actions.size(); ++i) {
        const bool is_selected = static_cast<int>(i) == selected;
        // Carets flank the selected action so the highlight reads in a still screenshot,
        // not by colour alone -- the same treatment DrawMenu gives its entries.
        const std::string line = is_selected ? "> " + actions[i] + " <" : actions[i];
        const float baseline =
            h * kResultsFirstActionBaselineFraction + static_cast<float>(i) * action_spacing;
        emit(line, baseline, action_pixel, is_selected ? selected_color : action_color);
    }

    if (cursor == 0) {
        return;
    }

    BindTextPipeline();
    for (const Run& run : runs) {
        DrawTextRun(face, run.first, run.count, run.color);
    }
}

void Renderer::DrawKeybinds(std::string_view title, std::span<const std::string> labels,
                            std::span<const std::string> values, int selected, bool capturing) {
    if (width_ == 0 || height_ == 0) {
        return;
    }

    const float h = static_cast<float>(height_);
    const float w = static_cast<float>(width_);
    const float title_pixel = h * kKeybindTitleFraction;
    const float row_pixel = h * kKeybindRowFraction;
    const float spacing = row_pixel * kKeybindRowSpacingFactor;
    const float label_left = w * kKeybindLabelLeftFraction;
    const float value_right = w * kKeybindValueRightFraction;

    // Same warm palette as the launch menu: amber title, amber selected row, quiet grey
    // for the rest, and a dimmer grey for the footer hint.
    const XMFLOAT4 title_color{1.0f, 0.82f, 0.48f, 1.0f};
    const XMFLOAT4 selected_color{1.0f, 0.78f, 0.35f, 1.0f};
    const XMFLOAT4 row_color{0.72f, 0.72f, 0.75f, 1.0f};
    const XMFLOAT4 hint_color{0.5f, 0.5f, 0.55f, 1.0f};

    struct Run {
        UINT first;
        UINT count;
        XMFLOAT4 color;
    };
    std::vector<Run> runs;
    runs.reserve(labels.size() * 3 + 2);

    const FontFace face = HudFace();
    UINT cursor = 0;
    const auto emit = [&](std::string_view text, float baseline, float pixel, float left,
                          XMFLOAT4 color) {
        const UINT first = cursor;
        cursor = LayoutLine(face, text, baseline, pixel, cursor, left);
        if (cursor > first) {
            runs.push_back({first, cursor - first, color});
        }
    };

    // Title, centred (left < 0).
    emit(title, h * kKeybindTitleBaselineFraction, title_pixel, -1.0f, title_color);

    const std::size_t count = std::min(labels.size(), values.size());
    for (std::size_t i = 0; i < count; ++i) {
        const bool is_selected = static_cast<int>(i) == selected;
        const XMFLOAT4 color = is_selected ? selected_color : row_color;
        const float baseline =
            h * kKeybindFirstRowBaselineFraction + static_cast<float>(i) * spacing;

        // A caret to the left marks the selected row so the highlight reads in a still
        // screenshot, not by colour alone.
        if (is_selected) {
            emit(">", baseline, row_pixel, label_left - row_pixel * 1.2f, color);
        }

        // Label, left-anchored at the label column.
        emit(labels[i], baseline, row_pixel, label_left, color);

        // Value, right-anchored at the value column. While this row is capturing, the
        // key is replaced by a prompt; rows with no value (Reset / Back) draw nothing.
        std::string value = is_selected && capturing ? "Press a key..." : values[i];
        if (!value.empty()) {
            const float left = value_right - TextWidth(face, value, row_pixel);
            emit(value, baseline, row_pixel, left, color);
        }
    }

    // Footer hint, centred at the bottom and a touch smaller so it reads as secondary.
    emit("Enter: rebind    Esc: back", h * kKeybindHintBaselineFraction,
         h * kKeybindHintFraction, -1.0f, hint_color);

    if (cursor == 0) {
        return;
    }

    BindTextPipeline();
    for (const Run& run : runs) {
        DrawTextRun(face, run.first, run.count, run.color);
    }
}

void Renderer::DrawInstances(std::span<const MeshInstance> instances,
                             const XMMATRIX& view_projection, XMFLOAT3 sun_direction,
                             float shadow_receive, bool bind_probe) {
    // The per-pass bindings the scene signature adds beyond the per-draw tables:
    // the frame constants (sun matrix + eye), the shadow map, and the reflection
    // probe. Bound here, at the top of every batch, so they survive the
    // root-signature switch the outline pass makes -- setting a root signature
    // clears every argument bound under the old one. The probe is skipped for the
    // capture pass, which is filling that cube and whose shader never reads it.
    command_list_->SetGraphicsRootConstantBufferView(2, frame_constants_address_);
    command_list_->SetGraphicsRootDescriptorTable(3, TextureHandle(shadow_descriptor_));
    if (bind_probe) {
        command_list_->SetGraphicsRootDescriptorTable(6, TextureHandle(probe_descriptor_));
    }

    for (const MeshInstance& instance : instances) {
        const GpuModel& model = models_[instance.model];
        const XMMATRIX instance_to_world = XMLoadFloat4x4(&instance.transform);
        const XMVECTOR tint = XMLoadFloat3(&instance.tint);

        command_list_->IASetVertexBuffers(0, 1, &model.vertex_buffer_view);
        command_list_->IASetIndexBuffer(&model.index_buffer_view);

        for (const DrawPrimitive& primitive : model.primitives) {
            // The primitive's own place in the model, then the model's place in
            // the world.
            const XMMATRIX to_world =
                XMLoadFloat4x4(&primitive.transform) * instance_to_world;

            Constants constants{};
            XMStoreFloat4x4(&constants.mvp, to_world * view_projection);
            XMStoreFloat4x4(&constants.model, to_world);

            // A normal is not carried by the model matrix when that matrix scales
            // unevenly -- and the yard's ground is a cube scaled 60 x 0.3 x 60.
            // The inverse transpose is what carries it instead. Only the upper
            // 3x3 matters, so the translation in the fourth row is left behind.
            const XMMATRIX normal_matrix = XMMatrixTranspose(XMMatrixInverse(nullptr, to_world));
            for (int row = 0; row < 3; ++row) {
                XMStoreFloat4(&constants.normal_rows[row], normal_matrix.r[row]);
            }

            XMStoreFloat3(&constants.albedo,
                          XMVectorMultiply(tint, XMLoadFloat3(&primitive.base_color)));
            constants.checker = instance.checker;
            constants.sun_direction = sun_direction;
            constants.shadow_receive = shadow_receive;
            constants.metallic = primitive.metallic;
            constants.roughness = primitive.roughness;
            constants.emissive = instance.emissive;
            constants.wetness = instance.wetness;

            command_list_->SetGraphicsRoot32BitConstants(0, kConstantDwords, &constants, 0);
            command_list_->SetGraphicsRootDescriptorTable(1, primitive.base_color_texture);
            command_list_->SetGraphicsRootDescriptorTable(4, primitive.normal_texture);
            command_list_->SetGraphicsRootDescriptorTable(5, primitive.metallic_roughness_texture);
            command_list_->SetGraphicsRootDescriptorTable(7, primitive.occlusion_texture);
            command_list_->DrawIndexedInstanced(primitive.index_count, 1, primitive.first_index, 0,
                                                0);
        }
    }
}

void Renderer::DrawShadowCasters(std::span<const MeshInstance> instances) {
    const XMMATRIX light_view_projection = XMLoadFloat4x4(&light_view_projection_);

    for (const MeshInstance& instance : instances) {
        const GpuModel& model = models_[instance.model];
        const XMMATRIX instance_to_world = XMLoadFloat4x4(&instance.transform);

        command_list_->IASetVertexBuffers(0, 1, &model.vertex_buffer_view);
        command_list_->IASetIndexBuffer(&model.index_buffer_view);

        for (const DrawPrimitive& primitive : model.primitives) {
            const XMMATRIX to_world = XMLoadFloat4x4(&primitive.transform) * instance_to_world;

            ShadowConstants constants{};
            XMStoreFloat4x4(&constants.light_mvp, to_world * light_view_projection);

            command_list_->SetGraphicsRoot32BitConstants(0, kShadowConstantDwords, &constants, 0);
            command_list_->DrawIndexedInstanced(primitive.index_count, 1, primitive.first_index, 0,
                                                0);
        }
    }
}

void Renderer::RenderShadowMap(const Scene& scene, std::span<const MeshInstance> props,
                               float time) {
    // Flip the map from the shader resource the scene pass left it as into a depth
    // target to write: the pixel-shader reads of last frame must drain, and it
    // becomes a depth-stencil write.
    TextureBarrier(command_list_.Get(), shadow_map_.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);

    command_list_->RSSetViewports(1, &shadow_viewport_);
    command_list_->RSSetScissorRects(1, &shadow_scissor_);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE shadow_dsv(dsv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                   static_cast<INT>(kShadowDsvIndex), dsv_size_);
    // No colour target: depth only.
    command_list_->OMSetRenderTargets(0, nullptr, FALSE, &shadow_dsv);
    command_list_->ClearDepthStencilView(shadow_dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    command_list_->SetPipelineState(shadow_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(shadow_root_signature_.Get());

    // The world and the resting props cast; the viewmodel and anything carried do
    // not -- they are lit by the camera's own sun and never enter this pass.
    DrawShadowCasters(scene.Instances());
    DrawShadowCasters(props);

    // The grass casts too: run the mesh-shader field from the sun's view into the same
    // depth map, so the blades shadow the ground and one another. It binds its own
    // pipeline and root signature, both replaced by the scene pass that follows. A no-op
    // without grass or mesh-shader support.
    RenderGrassShadow(time);

    // And back to a shader resource for the scene pass to sample: the depth writes
    // must drain before the pixel-shader reads that follow.
    TextureBarrier(command_list_.Get(), shadow_map_.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
}

void Renderer::RenderGrass(const XMMATRIX& view_projection, XMFLOAT3 camera_position, float time) {
    // No pipeline where the device lacks mesh shaders (it was never built), and nothing
    // to grow until a level sets a field.
    if (!mesh_shaders_supported_ || !grass_active_) {
        return;
    }

    // The field is a rectangle centred on grass_.center, laid over a grid of
    // kGrassCellSize cells -- one mesh-shader group each. Round the cell counts so the
    // grown field lands as close to the authored size as a whole number of cells allows.
    const UINT cells_x =
        std::max(1u, static_cast<UINT>(std::lround(grass_.size.x / kGrassCellSize)));
    const UINT cells_z =
        std::max(1u, static_cast<UINT>(std::lround(grass_.size.y / kGrassCellSize)));

    GrassConstants constants{};
    XMStoreFloat4x4(&constants.view_projection, view_projection);
    // The near corner of the field, on the ground: the mesh shader offsets each cell
    // from here, so the blades stay put in the world as the camera moves.
    constants.patch_origin = {grass_.center.x - cells_x * kGrassCellSize * 0.5f, grass_.center.y,
                              grass_.center.z - cells_z * kGrassCellSize * 0.5f};
    constants.cell_size = kGrassCellSize;
    constants.camera_position = camera_position;
    constants.time = time;
    constants.blade_color = grass_.color;
    constants.blade_height = grass_.blade_height;
    // The same sun the world is lit by, so the grass shades in step with the ground.
    constants.sun_direction = sun_direction_;
    constants.blade_width = grass_.blade_width;
    constants.wind = grass_.wind;
    constants.grid = {cells_x, cells_z};
    // Full blades close in, thinning to a quarter by 28 m and culled past 60 -- where
    // the fog has all but swallowed the field anyway. The amplification shader reads
    // these to pick each cell's blade count and to drop the far ones.
    constants.lod = {10.0f, 28.0f, 60.0f, 0.0f};

    command_list_->SetGraphicsRootSignature(grass_root_signature_.Get());
    command_list_->SetPipelineState(grass_pipeline_state_.Get());
    command_list_->SetGraphicsRoot32BitConstants(0, kGrassConstantDwords, &constants, 0);
    // The same per-frame lighting buffer the scene pass binds, for the pixel shader's
    // sun, sky, fog and shadow matrix.
    command_list_->SetGraphicsRootConstantBufferView(1, frame_constants_address_);
    // The sun's shadow map (bound from the texture heap, live as a shader resource by
    // now), so a blade can tell whether the sun reaches it.
    command_list_->SetGraphicsRootDescriptorTable(2, TextureHandle(shadow_descriptor_));
    // The obstacle footprints, so the field keeps clear of the patio, benches and props.
    command_list_->SetGraphicsRootConstantBufferView(3, grass_obstacles_address_);

    // One amplification group per pack of kGrassAsGroup cells; each culls its cells and
    // launches a mesh group for every survivor.
    const UINT as_groups = (cells_x * cells_z + kGrassAsGroup - 1) / kGrassAsGroup;
    command_list_->DispatchMesh(as_groups, 1, 1);
}

void Renderer::RenderGrassShadow(float time) {
    if (!mesh_shaders_supported_ || !grass_active_) {
        return;
    }

    const UINT cells_x =
        std::max(1u, static_cast<UINT>(std::lround(grass_.size.x / kGrassCellSize)));
    const UINT cells_z =
        std::max(1u, static_cast<UINT>(std::lround(grass_.size.y / kGrassCellSize)));

    GrassConstants constants{};
    // From the sun's point of view: the same shaders project by g_view_projection, so
    // handing them the light matrix fills the shadow map instead of the screen. The
    // amplification shader also extracts its cull frustum from this, so it keeps only the
    // grass inside the sun's orthographic box.
    XMStoreFloat4x4(&constants.view_projection, XMLoadFloat4x4(&light_view_projection_));
    constants.patch_origin = {grass_.center.x - cells_x * kGrassCellSize * 0.5f, grass_.center.y,
                              grass_.center.z - cells_z * kGrassCellSize * 0.5f};
    constants.cell_size = kGrassCellSize;
    // Unused for a depth-only pass (it only faces the blade normals and drives the LOD,
    // and the LOD is forced full below); the wind sway keys off time and world position,
    // not the eye, so the shadow's blades bend exactly like the on-screen ones.
    constants.camera_position = grass_.center;
    constants.time = time;
    constants.blade_color = grass_.color;
    constants.blade_height = grass_.blade_height;
    constants.sun_direction = sun_direction_;
    constants.blade_width = grass_.blade_width;
    constants.wind = grass_.wind;
    constants.grid = {cells_x, cells_z};
    // All distances huge: every cell keeps its full blade count and none is distance-
    // culled, so the shadow is complete regardless of where the camera stands.
    constants.lod = {1.0e9f, 1.0e9f, 1.0e9f, 0.0f};

    command_list_->SetGraphicsRootSignature(grass_root_signature_.Get());
    command_list_->SetPipelineState(grass_shadow_pipeline_state_.Get());
    command_list_->SetGraphicsRoot32BitConstants(0, kGrassConstantDwords, &constants, 0);
    // The amplification shader reads the obstacle footprints to skip cells under models;
    // the depth-only pass has no pixel shader, so it binds no shadow map or frame buffer.
    command_list_->SetGraphicsRootConstantBufferView(3, grass_obstacles_address_);

    const UINT as_groups = (cells_x * cells_z + kGrassAsGroup - 1) / kGrassAsGroup;
    command_list_->DispatchMesh(as_groups, 1, 1);
}

void Renderer::DrawOutlines(std::span<const MeshInstance> instances,
                            const XMMATRIX& view_projection, float seconds) {
    if (instances.empty()) {
        return;
    }

    // Breathe the whole rim on a cosine, kept above kOutlinePulseMin so it never
    // vanishes -- it is at full strength the instant an object becomes the target
    // and eases from there rather than starting mid-fade.
    const float breath = 0.5f + 0.5f * std::cos(seconds * kOutlinePulseHz * XM_2PI);
    const float pulse = kOutlinePulseMin + (kOutlinePulseMax - kOutlinePulseMin) * breath;

    command_list_->SetPipelineState(outline_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(outline_root_signature_.Get());

    for (const MeshInstance& instance : instances) {
        const GpuModel& model = models_[instance.model];
        const XMMATRIX instance_to_world = XMLoadFloat4x4(&instance.transform);

        command_list_->IASetVertexBuffers(0, 1, &model.vertex_buffer_view);
        command_list_->IASetIndexBuffer(&model.index_buffer_view);

        for (const DrawPrimitive& primitive : model.primitives) {
            const XMMATRIX to_world = XMLoadFloat4x4(&primitive.transform) * instance_to_world;

            OutlineConstants constants{};
            XMStoreFloat4x4(&constants.view_projection, view_projection);
            XMStoreFloat4x4(&constants.model, to_world);

            // Same inverse-transpose the scene uses, so the push-out follows the
            // true surface normal even where the model matrix scales unevenly.
            const XMMATRIX normal_matrix = XMMatrixTranspose(XMMatrixInverse(nullptr, to_world));
            for (int row = 0; row < 3; ++row) {
                XMStoreFloat4(&constants.normal_rows[row], normal_matrix.r[row]);
            }

            // Concentric enlarged copies from the tight bright inner rim out to
            // the wide faint halo. Additive blending sums them, so the overlap
            // near the edge is the brightest and the light falls off outward.
            for (int layer = 0; layer < kOutlineLayers; ++layer) {
                const float t = kOutlineLayers > 1
                                    ? static_cast<float>(layer) / (kOutlineLayers - 1)
                                    : 0.0f;
                constants.width = kOutlineInnerWidth + (kOutlineOuterWidth - kOutlineInnerWidth) * t;
                const float alpha =
                    (kOutlineInnerAlpha + (kOutlineOuterAlpha - kOutlineInnerAlpha) * t) * pulse;
                constants.color = {kOutlineColor.x, kOutlineColor.y, kOutlineColor.z, alpha};

                command_list_->SetGraphicsRoot32BitConstants(0, kOutlineConstantDwords, &constants,
                                                             0);
                command_list_->DrawIndexedInstanced(primitive.index_count, 1, primitive.first_index,
                                                     0, 0);
            }
        }
    }
}

void Renderer::RenderFlow(const XMMATRIX& view, const XMMATRIX& projection, float dt,
                          std::span<const FlowEmitter> emitters) {
    // Hand Flow our HDR scene buffer as its composite target and our scene depth for the
    // ray-march's depth test. Both are described explicitly because Flow re-creates the
    // views on its side; the states named here are the ones the world pass left them in at
    // this point -- HDR a render target, depth a depth target -- which is exactly what Flow
    // expects (it issues its own transitions to read depth and restores them).
    FlowTarget target = {};

    const CD3DX12_CPU_DESCRIPTOR_HANDLE hdr_rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                static_cast<INT>(kHdrRtvIndex), rtv_size_);
    target.color_resource = hdr_target_.Get();
    target.color_rtv = hdr_rtv;
    target.color_rtv_desc.Format = kHdrFormat;
    target.color_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

    target.depth_resource = depth_stencil_.Get();
    target.depth_dsv = dsv_heap_->GetCPUDescriptorHandleForHeapStart();
    target.depth_dsv_desc.Format = kDepthFormat;
    target.depth_dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;

    target.depth_srv_desc.Format = kDepthSrvFormat;
    target.depth_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    target.depth_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    target.depth_srv_desc.Texture2D.MipLevels = 1;

    target.viewport = viewport_;
    target.scissor = scissor_;

    // The fence values that bracket this frame's command list: the queue's completed value
    // now, and the value it will be signalled with once this list retires (MoveToNextFrame
    // signals fence_values_[frame_index_] before bumping it). Flow frees internal buffers
    // whose release value has passed the completed one.
    const UINT64 last_fence_completed = fence_->GetCompletedValue();
    const UINT64 next_fence_value = fence_values_[frame_index_];

    // Bridging enhanced barriers (this renderer) to legacy ones (Flow's internals) has two
    // rules to satisfy at once. First, D3D12 forbids a legacy barrier on a resource an
    // enhanced barrier last moved unless it is parked in the common layout -- so decay the
    // HDR buffer and the scene depth to COMMON with enhanced barriers. Second, Flow assumes
    // its render target and depth sit in *concrete* legacy states, not COMMON: a COMMON
    // resource is implicitly promoted on first access, after which Flow's own before=COMMON
    // barrier mismatches. So immediately re-transition, with legacy barriers, into the
    // render-target and depth-write states Flow's demo hands it. The whole thing reverses
    // after the call.
    TextureBarrier(command_list_.Get(), hdr_target_.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_LAYOUT_COMMON);
    TextureBarrier(command_list_.Get(), depth_stencil_.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_LAYOUT_COMMON);

    const D3D12_RESOURCE_BARRIER to_flow[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(hdr_target_.Get(), D3D12_RESOURCE_STATE_COMMON,
                                             D3D12_RESOURCE_STATE_RENDER_TARGET),
        CD3DX12_RESOURCE_BARRIER::Transition(depth_stencil_.Get(), D3D12_RESOURCE_STATE_COMMON,
                                             D3D12_RESOURCE_STATE_DEPTH_WRITE),
    };
    command_list_->ResourceBarrier(_countof(to_flow), to_flow);

    flow_.Render(command_list_.Get(), last_fence_completed, next_fence_value, dt, emitters, target,
                 view, projection);

    // Flow leaves the resources in the concrete states it was told (its transitions restore
    // to currentState). Return them to COMMON with legacy barriers, then promote back to the
    // working layouts with enhanced barriers for the cloud pass and arms that follow.
    const D3D12_RESOURCE_BARRIER from_flow[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(hdr_target_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                             D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(depth_stencil_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                             D3D12_RESOURCE_STATE_COMMON),
    };
    command_list_->ResourceBarrier(_countof(from_flow), from_flow);

    TextureBarrier(command_list_.Get(), hdr_target_.Get(), D3D12_BARRIER_SYNC_ALL,
                   D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_LAYOUT_COMMON,
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    TextureBarrier(command_list_.Get(), depth_stencil_.Get(), D3D12_BARRIER_SYNC_ALL,
                   D3D12_BARRIER_ACCESS_COMMON, D3D12_BARRIER_LAYOUT_COMMON,
                   D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
}

void Renderer::RenderFluidSpray(std::span<const XMFLOAT4> droplets, const XMMATRIX& view,
                                const XMMATRIX& projection) {
    if (droplets.empty()) {
        return;
    }
    const UINT count = std::min(static_cast<UINT>(droplets.size()), kMaxSprayDroplets);

    // This frame's slice of the droplet buffer, so the write cannot race the GPU still
    // reading the previous frame's.
    std::byte* region =
        fluid_droplets_mapped_ + static_cast<size_t>(frame_index_) * fluid_droplets_stride_;
    std::memcpy(region, droplets.data(), static_cast<size_t>(count) * sizeof(XMFLOAT4));
    const D3D12_GPU_VIRTUAL_ADDRESS address =
        fluid_droplets_->GetGPUVirtualAddress() +
        static_cast<UINT64>(frame_index_) * fluid_droplets_stride_;

    // Handles into the heaps the passes bind, resolved once.
    const CD3DX12_CPU_DESCRIPTOR_HANDLE fluid_depth_rtv(
        rtv_heap_->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(kFluidRtvBase + 0),
        rtv_size_);
    const CD3DX12_CPU_DESCRIPTOR_HANDLE fluid_depth_blur_rtv(
        rtv_heap_->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(kFluidRtvBase + 1),
        rtv_size_);
    const CD3DX12_CPU_DESCRIPTOR_HANDLE fluid_thickness_rtv(
        rtv_heap_->GetCPUDescriptorHandleForHeapStart(), static_cast<INT>(kFluidRtvBase + 2),
        rtv_size_);
    const CD3DX12_CPU_DESCRIPTOR_HANDLE hdr_rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                static_cast<INT>(kHdrRtvIndex), rtv_size_);
    const CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    auto engine_srv = [&](UINT slot) {
        return CD3DX12_GPU_DESCRIPTOR_HANDLE(engine_heap_->GetGPUDescriptorHandleForHeapStart(),
                                             static_cast<INT>(slot), engine_heap_size_);
    };

    // --- Pass 1: surface depth + thickness. The impostors draw into the two offscreen
    // targets, depth-tested read-only against the world so the yard occludes them; MIN
    // blend keeps the nearest surface, additive blend sums the thickness. ---
    TextureBarrier(command_list_.Get(), fluid_depth_.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    TextureBarrier(command_list_.Get(), fluid_thickness_.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    const D3D12_CPU_DESCRIPTOR_HANDLE surface_rtvs[] = {fluid_depth_rtv, fluid_thickness_rtv};
    command_list_->OMSetRenderTargets(2, surface_rtvs, FALSE, &dsv);
    const float sentinel_clear[] = {kFluidSentinel, kFluidSentinel, kFluidSentinel, kFluidSentinel};
    const float zero_clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_list_->ClearRenderTargetView(fluid_depth_rtv, sentinel_clear, 0, nullptr);
    command_list_->ClearRenderTargetView(fluid_thickness_rtv, zero_clear, 0, nullptr);

    FluidConstants surface{};
    XMStoreFloat4x4(&surface.view, view);
    XMStoreFloat4x4(&surface.proj, projection);
    command_list_->SetGraphicsRootSignature(fluid_surface_root_signature_.Get());
    command_list_->SetPipelineState(fluid_surface_pipeline_.Get());
    command_list_->SetGraphicsRoot32BitConstants(0, kFluidConstantDwords, &surface, 0);
    command_list_->SetGraphicsRootShaderResourceView(1, address);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 0, nullptr);
    command_list_->DrawInstanced(6, count, 0, 0);

    TextureBarrier(command_list_.Get(), fluid_depth_.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                   D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    TextureBarrier(command_list_.Get(), fluid_thickness_.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                   D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);

    // --- Pass 2: separable bilateral blur, horizontal then vertical, ping-ponging between
    // the depth and its blur target so the smoothed depth ends back in fluid_depth_. Beads
    // in a tight cluster melt into one surface; the bilateral weighting keeps blobs apart. ---
    ID3D12DescriptorHeap* engine_heaps[] = {engine_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(engine_heaps), engine_heaps);
    command_list_->SetGraphicsRootSignature(fluid_blur_root_signature_.Get());
    command_list_->SetPipelineState(fluid_blur_pipeline_.Get());

    auto blur = [&](const CD3DX12_CPU_DESCRIPTOR_HANDLE& dst_rtv, ID3D12Resource* dst,
                    UINT src_srv_slot, XMFLOAT2 texel_step) {
        TextureBarrier(command_list_.Get(), dst, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                       D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                       D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                       D3D12_BARRIER_LAYOUT_RENDER_TARGET);
        command_list_->OMSetRenderTargets(1, &dst_rtv, FALSE, nullptr);
        FluidBlurConstants blur_constants{};
        blur_constants.texel_step = texel_step;
        blur_constants.sentinel = kFluidSentinel;
        blur_constants.depth_threshold = kFluidBlurDepthThreshold;
        command_list_->SetGraphicsRoot32BitConstants(0, kFluidBlurConstantDwords, &blur_constants, 0);
        command_list_->SetGraphicsRootDescriptorTable(1, engine_srv(src_srv_slot));
        command_list_->DrawInstanced(3, 1, 0, 0);
        TextureBarrier(command_list_.Get(), dst, D3D12_BARRIER_SYNC_RENDER_TARGET,
                       D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                       D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                       D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    };
    const float inv_w = 1.0f / static_cast<float>(width_);
    const float inv_h = 1.0f / static_cast<float>(height_);
    // Horizontal: depth -> blur target.
    blur(fluid_depth_blur_rtv, fluid_depth_blur_.Get(), kFluidDepthSrvIndex, XMFLOAT2(inv_w, 0.0f));
    // Vertical: blur target -> back into depth, where the composite reads it.
    blur(fluid_depth_rtv, fluid_depth_.Get(), kFluidDepthBlurSrvIndex, XMFLOAT2(0.0f, inv_h));

    // --- Pass 3: composite. Reconstruct the surface from the blurred depth and composite it
    // into the HDR buffer with a dual-source blend: additive sheen plus per-channel
    // Beer-Lambert absorption of the scene behind, no scene copy. ---
    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, nullptr);
    command_list_->SetGraphicsRootSignature(fluid_composite_root_signature_.Get());
    command_list_->SetPipelineState(fluid_composite_pipeline_.Get());

    FluidCompositeConstants composite{};
    XMFLOAT4X4 proj4x4;
    XMStoreFloat4x4(&proj4x4, projection);
    composite.proj00 = proj4x4._11;
    composite.proj11 = proj4x4._22;
    composite.texel = XMFLOAT2(inv_w, inv_h);
    XMStoreFloat3(&composite.sun_dir_view,
                  XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&sun_direction_), view)));
    composite.sentinel = kFluidSentinel;
    composite.sun_color = environment_.sun_color;
    composite.sun_intensity = environment_.sun_intensity;
    composite.sky_ambient = environment_.sky_ambient;
    composite.ambient_strength = environment_.ambient_strength;
    composite.tint = kDropletTint;
    composite.gloss = kFluidGloss;
    composite.absorption = kFluidAbsorption;
    composite.absorption_strength = kFluidAbsorptionStrength;
    command_list_->SetGraphicsRoot32BitConstants(0, kFluidCompositeConstantDwords, &composite, 0);
    command_list_->SetGraphicsRootDescriptorTable(1, engine_srv(kFluidDepthSrvIndex));
    command_list_->DrawInstanced(3, 1, 0, 0);

    // Leave the world pass as it was found: the scene's texture heap and the HDR target with
    // its depth bound. The caller restores the scene root signature and pipeline.
    ID3D12DescriptorHeap* scene_heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(scene_heaps), scene_heaps);
    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, &dsv);
}

void Renderer::Render(const Scene& scene, std::span<const MeshInstance> props,
                      std::span<const MeshInstance> highlight, const ViewmodelPose& viewmodel,
                      std::span<const MeshInstance> held_props, const XMMATRIX& view_projection,
                      XMFLOAT3 camera_position, std::string_view hud_prompt,
                      std::span<const std::string> debug_lines,
                      std::span<const OrderCard> orders, std::span<const MeatCard> meats,
                      const XMMATRIX& view, const XMMATRIX& projection, float flow_dt,
                      std::span<const FlowEmitter> flow_emitters,
                      std::span<const XMFLOAT4> droplets) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    // Wall-clock seconds since start-up, on one clock for the whole frame: the sky's
    // drifting clouds, the fog that fades into them, and the outline's pulse all read
    // from it.
    const float seconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - start_time_).count();

    // Refresh this frame's slice of the frame constants -- the eye moved -- and
    // point the scene pass's CBV at it. Its own region, so the write cannot race
    // the GPU still reading the previous frame's.
    std::byte* frame_region =
        frame_constants_mapped_ + static_cast<size_t>(frame_index_) * frame_constants_stride_;
    FrameConstants frame{};
    frame.light_view_projection = light_view_projection_;
    frame.camera_position = camera_position;
    frame.time = seconds;
    ApplyEnvironment(frame, environment_);
    std::memcpy(frame_region, &frame, sizeof(frame));
    frame_constants_address_ =
        frame_constants_->GetGPUVirtualAddress() +
        static_cast<UINT64>(frame_index_) * frame_constants_stride_;

    // The only heap the game binds, and it must be bound before any root
    // descriptor table that points into it.
    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // The shadow map comes first: the scene pass samples it, so every caster has
    // to be drawn into it before a single lit pixel is shaded. It binds its own
    // pipeline, root signature and viewport, all replaced below for the main pass.
    RenderShadowMap(scene, props, seconds);

    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->SetPipelineState(pipeline_state_.Get());
    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    // The whole world renders into the linear HDR scene buffer; the tonemap pass
    // resolves it to the swapchain afterward. Flip the buffer from the shader
    // resource the previous frame's resolve left it as into a render target: that
    // pixel-shader read must drain before this frame writes it.
    TextureBarrier(command_list_.Get(), hdr_target_.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    // The depth buffer rests as a shader resource for the light-shaft pass; flip it
    // to a depth target for the world pass about to write it.
    TextureBarrier(command_list_.Get(), depth_stencil_.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE hdr_rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                                static_cast<INT>(kHdrRtvIndex), rtv_size_);
    // Slot 0 of the DSV heap: the scene's depth buffer, not the shadow map's.
    const CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, &dsv);

    command_list_->ClearRenderTargetView(hdr_rtv, kHdrClearColor, 0, nullptr);
    command_list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // The gradient sky, filling the frame behind the yard. Depth is off, so the
    // geometry that follows paints over it. DrawSky binds its own pipeline and root
    // signature, so the scene's are restored before the first instance is drawn.
    DrawSky(view_projection, camera_position, seconds, false);
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->SetPipelineState(pipeline_state_.Get());

    // The loaded level's sun, already normalized. Matches the direction baked into
    // light_view_projection_, so the shadows the world casts line up with its shade.
    const XMFLOAT3 sun = sun_direction_;
    // The world and the resting props receive the sun's shadow.
    DrawInstances(scene.Instances(), view_projection, sun, 1.0f, true);
    // The resting props take the yard's sun too: they are part of the world, and
    // only pass into the near pass once the player lifts them.
    DrawInstances(props, view_projection, sun, 1.0f, true);

    // The grass grows over the ground, into the same HDR buffer and depth, so it
    // occludes and fogs with the world and the shafts below march through it. It binds
    // its own mesh-shader pipeline and root signature; restore the scene's, since the
    // outline pass early-returns with nothing highlighted and the viewmodel draw that
    // follows assumes the scene's are still in force. A no-op without mesh-shader support.
    RenderGrass(view_projection, camera_position, seconds);
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->SetPipelineState(pipeline_state_.Get());

    // The lighter-fluid spray: a screen-space fluid, composited into the HDR buffer as
    // smooth, tinted, translucent liquid. It captures the droplets' surface depth and
    // thickness (depth-tested against the yard the world pass just laid down, so the world
    // occludes them), bilaterally smooths the depth, and composites the lit surface -- all
    // here, while the scene depth still holds the world and before the arms clear it. It
    // rebinds the scene's texture heap and render targets on the way out; restore the scene
    // root signature and pipeline for the outline pass and the draws that follow.
    RenderFluidSpray(droplets, view, projection);
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->SetPipelineState(pipeline_state_.Get());

    // Paint the glowing halo around the object the player is aiming at. It runs
    // with depth off, so it washes across the object as well as the ground around
    // it; re-painting the object on top afterward leaves only the ring outside
    // its silhouette, and does so evenly on every side. It pulses on `seconds`,
    // the same clock the clouds drift on.
    DrawOutlines(highlight, view_projection, seconds);
    if (!highlight.empty()) {
        // DrawOutlines left its own PSO and root signature bound; restore the
        // scene's, both to re-paint the object here and for the passes that
        // follow, which are all DrawInstances and assume the scene's are in force.
        command_list_->SetPipelineState(pipeline_state_.Get());
        command_list_->SetGraphicsRootSignature(root_signature_.Get());
        DrawInstances(highlight, view_projection, sun, 1.0f, true);
    }

    // The volumetric clouds, composited over the sky *before* the arms are drawn --
    // crucially while the depth buffer still holds the yard, so the world occludes
    // them: a pixel on the tree or the fence reads a near depth and the pass leaves it
    // alone. The arms pass below clears depth to draw the viewmodel on top of
    // everything, so the clouds cannot ride alongside the shafts after it -- by then
    // only the arms are in the depth buffer, and the clouds would spill over the
    // treetops. Depth flips to a shader resource for the march, then back to a target
    // for the arms.
    TextureBarrier(command_list_.Get(), depth_stencil_.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, nullptr);
    ID3D12DescriptorHeap* cloud_heaps[] = {engine_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(cloud_heaps), cloud_heaps);
    command_list_->SetGraphicsRootSignature(cloud_root_signature_.Get());
    command_list_->SetPipelineState(cloud_pipeline_state_.Get());
    CloudConstants cloud{};
    XMStoreFloat4x4(&cloud.inv_view_projection, XMMatrixInverse(nullptr, view_projection));
    cloud.camera_position = camera_position;
    cloud.time = seconds;
    cloud.sun_direction = sun_direction_;
    ApplyEnvironment(cloud, environment_);
    command_list_->SetGraphicsRoot32BitConstants(0, kCloudConstantDwords, &cloud, 0);
    const CD3DX12_GPU_DESCRIPTOR_HANDLE cloud_depth_srv(
        engine_heap_->GetGPUDescriptorHandleForHeapStart(), static_cast<INT>(kDepthSrvIndex),
        engine_heap_size_);
    command_list_->SetGraphicsRootDescriptorTable(1, cloud_depth_srv);
    command_list_->IASetVertexBuffers(0, 0, nullptr);
    command_list_->DrawInstanced(3, 1, 0, 0);

    // Depth back to a write target, and the scene pipeline the arms draw with. The
    // cloud pass swapped the engine heap in for its depth SRV; rebind the texture heap
    // the scene's draws sample, or the arms would read their materials from the wrong
    // heap.
    TextureBarrier(command_list_.Get(), depth_stencil_.Get(), D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
                   D3D12_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, &dsv);
    ID3D12DescriptorHeap* scene_heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(scene_heaps), scene_heaps);
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->SetPipelineState(pipeline_state_.Get());

    // The grill's fire and smoke, ray-marched into the HDR buffer *after* the clouds -- so a
    // plume rising against the sky stands in front of them, not behind -- but before the arms
    // clear depth, while the scene depth still holds the yard so the world occludes the smoke.
    // Flow leaves its own heap, root signature, PSO and targets bound, so restore the scene's
    // for the arms that follow.
    RenderFlow(view, projection, flow_dt, flow_emitters);
    ID3D12DescriptorHeap* post_flow_heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(post_flow_heaps), post_flow_heaps);
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->SetPipelineState(pipeline_state_.Get());
    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, &dsv);
    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    // The arms live about half a metre from the eye, close enough that any wall
    // the player leans against would be drawn in front of them. Throwing the
    // depth buffer away first is the usual answer: it costs one clear, and the
    // arms still occlude each other because they keep writing depth as they go.
    command_list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // The arms and anything carried do not receive the sun's shadow: they are lit
    // by a sun bolted to the camera, which the shadow map, built from the world's
    // fixed sun, says nothing about. Hence 0 -- their sun term stays unshadowed.
    DrawInstances(viewmodel.instances, view_projection, viewmodel.sun_direction, 0.0f, true);
    // A carried object rides in the same pass as the arms, under the same key
    // light bolted to the eye, so it is lit like something in the hand and never
    // clipped by the wall the player is facing.
    DrawInstances(held_props, view_projection, viewmodel.sun_direction, 0.0f, true);

    // The volumetric sun shafts, added into the HDR scene. The world's depth writes
    // must drain, then the depth buffer becomes a shader resource the march reads to
    // stop each ray at the surface. It is unbound as a depth target for this pass --
    // the shafts render into the HDR buffer alone, additively, no depth.
    TextureBarrier(command_list_.Get(), depth_stencil_.Get(), D3D12_BARRIER_SYNC_DEPTH_STENCIL,
                   D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE,
                   D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_SYNC_PIXEL_SHADING,
                   D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);

    command_list_->OMSetRenderTargets(1, &hdr_rtv, FALSE, nullptr);
    // The shaft pass reads the depth and shadow SRVs from the persistent engine heap,
    // which the tonemap pass below also binds -- so it stays bound through both.
    ID3D12DescriptorHeap* engine_heaps[] = {engine_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(engine_heaps), engine_heaps);

    command_list_->SetGraphicsRootSignature(light_shaft_root_signature_.Get());
    command_list_->SetPipelineState(light_shaft_pipeline_state_.Get());

    LightShaftConstants shaft{};
    XMStoreFloat4x4(&shaft.inv_view_projection, XMMatrixInverse(nullptr, view_projection));
    shaft.light_view_projection = light_view_projection_;
    shaft.camera_position = camera_position;
    shaft.sun_direction = sun_direction_;
    ApplyEnvironment(shaft, environment_);
    command_list_->SetGraphicsRoot32BitConstants(0, kLightShaftConstantDwords, &shaft, 0);
    const CD3DX12_GPU_DESCRIPTOR_HANDLE shaft_srv(
        engine_heap_->GetGPUDescriptorHandleForHeapStart(), static_cast<INT>(kDepthSrvIndex),
        engine_heap_size_);
    command_list_->SetGraphicsRootDescriptorTable(1, shaft_srv);
    command_list_->IASetVertexBuffers(0, 0, nullptr);
    command_list_->DrawInstanced(3, 1, 0, 0);

    // The world and its shafts are finished. Flip the HDR buffer to a shader resource
    // for the tonemap pass: the render-target writes must drain before the resolve
    // samples it.
    TextureBarrier(command_list_.Get(), hdr_target_.Get(), D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET,
                   D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                   D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);

    // Bloom: build the glow pyramid from the finished HDR scene, leaving it in bloom
    // level 0 for the resolve to add in. It runs its own per-mip viewports, so the
    // full-window one is restored afterward for the resolve and HUD.
    RenderBloom();
    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    // Bring the swapchain buffer up as the target the resolve and the HUD write. The
    // resolve overwrites every pixel, so its prior contents are discarded -- layout
    // UNDEFINED before, with nothing to wait on.
    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(frame_index_), rtv_size_);
    // No depth: the resolve and the HUD both write flat over the frame.
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    // The resolve pass: tonemap the HDR scene plus the bloom onto the swapchain. Both
    // SRVs live in the engine heap, still bound from the bloom pass; the fullscreen
    // triangle comes from SV_VertexID, so no vertex buffer.
    command_list_->SetGraphicsRootSignature(tonemap_root_signature_.Get());
    command_list_->SetPipelineState(tonemap_pipeline_state_.Get());
    const float tonemap_constants[] = {environment_.exposure, environment_.bloom_intensity};
    command_list_->SetGraphicsRoot32BitConstants(0, _countof(tonemap_constants), tonemap_constants,
                                                 0);
    const D3D12_GPU_DESCRIPTOR_HANDLE engine_base =
        engine_heap_->GetGPUDescriptorHandleForHeapStart();
    command_list_->SetGraphicsRootDescriptorTable(
        1, CD3DX12_GPU_DESCRIPTOR_HANDLE(engine_base, static_cast<INT>(kHdrSrvIndex),
                                         engine_heap_size_));
    command_list_->SetGraphicsRootDescriptorTable(
        2, CD3DX12_GPU_DESCRIPTOR_HANDLE(engine_base, static_cast<INT>(kBloomSrvBase),
                                         engine_heap_size_));
    command_list_->IASetVertexBuffers(0, 0, nullptr);
    command_list_->DrawInstanced(3, 1, 0, 0);

    // The HUD goes on last, blended over the resolved frame in display space -- so
    // it is deliberately not tonemapped. It samples the font atlas from the
    // per-level texture heap, so bind that back before drawing.
    ID3D12DescriptorHeap* text_heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(text_heaps), text_heaps);
    DrawHud(hud_prompt, debug_lines, orders, meats);

    // The frame is complete; hand the swapchain buffer back for presentation.
    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_PRESENT);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    ThrowIfFailed(swap_chain_->Present(1, 0), "SwapChain::Present");

    MoveToNextFrame();
}

void Renderer::RenderMenu(std::string_view title, std::span<const std::string> entries,
                          int selected) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    // The font atlas SRV lives in the per-level texture heap; the menu text samples
    // it, so bind that heap. A level is loaded behind the menu (its geometry just is
    // not drawn), so the heap and the atlas are present.
    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    // Bring the swapchain buffer up as a render target and clear it to the menu
    // backdrop. The clear overwrites every pixel, so its prior contents are
    // discarded -- layout UNDEFINED before, with nothing to wait on.
    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(frame_index_), rtv_size_);
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    command_list_->ClearRenderTargetView(rtv, kMenuClearColor, 0, nullptr);

    DrawMenu(title, entries, selected);

    // The frame is complete; hand the swapchain buffer back for presentation.
    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_PRESENT);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    ThrowIfFailed(swap_chain_->Present(1, 0), "SwapChain::Present");

    MoveToNextFrame();
}

void Renderer::RenderLoading(std::string_view title, std::string_view subtitle) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    // Same setup as RenderMenu: the font atlas rides the per-level texture heap (the
    // outgoing level is still resident), so bind it, then own the swapchain buffer from
    // clear to present over the menu backdrop.
    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(frame_index_), rtv_size_);
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    command_list_->ClearRenderTargetView(rtv, kMenuClearColor, 0, nullptr);

    DrawLoading(title, subtitle);

    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_PRESENT);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    ThrowIfFailed(swap_chain_->Present(1, 0), "SwapChain::Present");

    MoveToNextFrame();
}

void Renderer::RenderResults(std::string_view title, bool passed,
                             std::span<const ResultLine> lines,
                             std::span<const std::string> actions, int selected) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    // Same setup as RenderMenu: the font atlas rides the per-level texture heap, so bind
    // it, then own the swapchain buffer from clear to present over the menu backdrop.
    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(frame_index_), rtv_size_);
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    command_list_->ClearRenderTargetView(rtv, kMenuClearColor, 0, nullptr);

    DrawResults(title, passed, lines, actions, selected);

    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_PRESENT);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    ThrowIfFailed(swap_chain_->Present(1, 0), "SwapChain::Present");

    MoveToNextFrame();
}

void Renderer::RenderKeybinds(std::string_view title, std::span<const std::string> labels,
                              std::span<const std::string> values, int selected, bool capturing) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    // Same setup as RenderMenu: the font atlas rides the per-level texture heap, so bind
    // it, then own the swapchain buffer from clear to present.
    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_NO_ACCESS,
                   D3D12_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_SYNC_RENDER_TARGET,
                   D3D12_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(frame_index_), rtv_size_);
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    command_list_->ClearRenderTargetView(rtv, kMenuClearColor, 0, nullptr);

    DrawKeybinds(title, labels, values, selected, capturing);

    TextureBarrier(command_list_.Get(), render_targets_[frame_index_].Get(),
                   D3D12_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET,
                   D3D12_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_SYNC_NONE,
                   D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_LAYOUT_PRESENT);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    ThrowIfFailed(swap_chain_->Present(1, 0), "SwapChain::Present");

    MoveToNextFrame();
}

int Renderer::MenuEntryAt(int x, int y, int entry_count) const {
    if (height_ == 0 || entry_count <= 0) {
        return -1;
    }
    (void)x; // Rows span the full width, so only the vertical position matters.

    const float h = static_cast<float>(height_);
    const float entry_pixel = h * kMenuEntryFraction;
    const float spacing = entry_pixel * kMenuEntrySpacingFactor;
    const float fy = static_cast<float>(y);

    for (int i = 0; i < entry_count; ++i) {
        const float baseline =
            h * kMenuFirstEntryBaselineFraction + static_cast<float>(i) * spacing;
        // The band is centred on the line's visual middle (the baseline sits near
        // the glyphs' feet) and is one line-pitch tall, so consecutive rows tile
        // with no gaps between them.
        const float center = baseline - entry_pixel * 0.3f;
        if (fy >= center - spacing * 0.5f && fy < center + spacing * 0.5f) {
            return i;
        }
    }
    return -1;
}

int Renderer::ResultsActionAt(int x, int y, int action_count) const {
    if (height_ == 0 || action_count <= 0) {
        return -1;
    }
    (void)x; // Rows span the full width, so only the vertical position matters.

    const float h = static_cast<float>(height_);
    const float action_pixel = h * kResultsActionFraction;
    const float spacing = action_pixel * kResultsActionSpacingFactor;
    const float fy = static_cast<float>(y);

    for (int i = 0; i < action_count; ++i) {
        const float baseline =
            h * kResultsFirstActionBaselineFraction + static_cast<float>(i) * spacing;
        // Same tiling as MenuEntryAt: a line-pitch-tall band centred on each action's
        // visual middle, so consecutive actions tile with no gaps.
        const float center = baseline - action_pixel * 0.3f;
        if (fy >= center - spacing * 0.5f && fy < center + spacing * 0.5f) {
            return i;
        }
    }
    return -1;
}

int Renderer::KeybindRowAt(int x, int y, int row_count) const {
    if (height_ == 0 || row_count <= 0) {
        return -1;
    }
    (void)x; // Rows span the full width, so only the vertical position matters.

    const float h = static_cast<float>(height_);
    const float row_pixel = h * kKeybindRowFraction;
    const float spacing = row_pixel * kKeybindRowSpacingFactor;
    const float fy = static_cast<float>(y);

    for (int i = 0; i < row_count; ++i) {
        const float baseline =
            h * kKeybindFirstRowBaselineFraction + static_cast<float>(i) * spacing;
        // Same tiling as MenuEntryAt: a line-pitch-tall band centred on each row's
        // glyphs, so adjacent rows meet with no gap.
        const float center = baseline - row_pixel * 0.3f;
        if (fy >= center - spacing * 0.5f && fy < center + spacing * 0.5f) {
            return i;
        }
    }
    return -1;
}

void Renderer::MoveToNextFrame() {
    const UINT64 current = fence_values_[frame_index_];
    ThrowIfFailed(queue_->Signal(fence_.Get(), current), "CommandQueue::Signal");

    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    // Only stall if the GPU has not finished the last frame that used this
    // allocator. With two frames in flight this is usually a no-op.
    if (fence_->GetCompletedValue() < fence_values_[frame_index_]) {
        ThrowIfFailed(fence_->SetEventOnCompletion(fence_values_[frame_index_], fence_event_),
                      "Fence::SetEventOnCompletion");
        WaitForSingleObjectEx(fence_event_, INFINITE, FALSE);
    }

    fence_values_[frame_index_] = current + 1;
}

void Renderer::FlushGpu() {
    if (!queue_ || !fence_ || !fence_event_) {
        return;
    }

    const UINT64 target = fence_values_[frame_index_];
    ThrowIfFailed(queue_->Signal(fence_.Get(), target), "CommandQueue::Signal");

    if (fence_->GetCompletedValue() < target) {
        ThrowIfFailed(fence_->SetEventOnCompletion(target, fence_event_),
                      "Fence::SetEventOnCompletion");
        WaitForSingleObjectEx(fence_event_, INFINITE, FALSE);
    }

    ++fence_values_[frame_index_];
}

void Renderer::Resize(UINT width, UINT height) {
    if (!initialized_ || width == 0 || height == 0 || (width == width_ && height == height_)) {
        return;
    }

    // The swapchain cannot resize while the GPU still references its buffers.
    FlushGpu();

    for (UINT i = 0; i < kFrameCount; ++i) {
        render_targets_[i].Reset();
        // Every frame is now retired, so they all share one fence value.
        fence_values_[i] = fence_values_[frame_index_];
    }
    depth_stencil_.Reset();
    // The HDR scene buffer and the bloom mips are window-sized too, so they are
    // rebuilt with the rest. The engine heap they are viewed through persists;
    // CreateHdrTarget and CreateBloomTargets just rewrite the descriptors.
    hdr_target_.Reset();
    for (BloomTarget& target : bloom_targets_) {
        target.texture.Reset();
    }
    // The fluid's offscreen targets are window-sized as well; their SRVs in the engine
    // heap are simply rewritten by CreateFluidTargets below.
    fluid_depth_.Reset();
    fluid_depth_blur_.Reset();
    fluid_thickness_.Reset();

    DXGI_SWAP_CHAIN_DESC desc{};
    ThrowIfFailed(swap_chain_->GetDesc(&desc), "SwapChain::GetDesc");
    ThrowIfFailed(
        swap_chain_->ResizeBuffers(kFrameCount, width, height, desc.BufferDesc.Format, desc.Flags),
        "SwapChain::ResizeBuffers");

    width_ = width;
    height_ = height;
    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    CreateRenderTargetViews();
    CreateDepthBuffer();
    CreateHdrTarget();
    CreateBloomTargets();
    CreateFluidTargets();
}

void Renderer::Shutdown() {
    if (initialized_) {
        FlushGpu();
    }
    // With the GPU idle, release the Flow objects before the device goes away.
    flow_.Shutdown();
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    initialized_ = false;
}
