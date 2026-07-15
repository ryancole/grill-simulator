// Volumetric grass, generated on the GPU by an amplification + mesh shader pair --
// no vertex or index buffer, no per-blade instance data uploaded. The field is a
// rectangle on the ground split into a grid of cells. The amplification shader
// runs one thread per cell: it frustum- and distance-culls the cell and, for the
// survivors, picks a level of detail (fewer blades farther off), then launches one
// mesh-shader group per surviving cell. Each mesh-shader thread grows one curved,
// wind-swayed blade straight into the pipeline. The pixel shader shades the blades
// the same way the scene pass shades everything else -- the level's sun (with the
// shadow map, so blades shade each other and catch the world's shadows), a
// hemisphere of sky ambient, and the same distance fog.
//
// The same amplification + mesh pair also fills the sun's shadow map: run with the
// light's view-projection in g_view_projection and no pixel shader, it writes the
// grass's depth so the field casts onto the ground and the props.
//
// This is the mesh-shader path the renderer takes only where the device reports
// mesh-shader tier 1 (see Renderer::MeshShadersSupported); hardware without it
// draws no grass rather than falling back, for now.
#include "common.hlsli"

// One blade is a short strip climbing in Y: BLADE_SEGMENTS quads whose width tapers
// to a point at the tip. Each mesh thread emits one, so the per-group vertex and
// primitive counts are BLADES_PER_GROUP times a blade's. Tier 1 caps a group at 256
// vertices and 256 primitives, so the product stays under both.
#define BLADE_SEGMENTS 4
#define VERTS_PER_BLADE (2 * (BLADE_SEGMENTS + 1)) // a left/right pair per ring
#define PRIMS_PER_BLADE (2 * BLADE_SEGMENTS)       // two triangles per quad
#define BLADES_PER_GROUP 16
#define GROUP_VERTS (BLADES_PER_GROUP * VERTS_PER_BLADE) // 160
#define GROUP_PRIMS (BLADES_PER_GROUP * PRIMS_PER_BLADE) // 128

// One amplification group tests this many cells -- one thread each -- and launches a
// mesh group for each that survives the cull. Must match kGrassAsGroup in renderer.cpp,
// which sizes the DispatchMesh grid.
#define AS_GROUP 32

// Everything the field needs that is not per-frame lighting. Root constants, so it
// costs no buffer: the C++ mirror is GrassConstants in renderer.cpp -- keep the two
// layouts in step.
cbuffer GrassConstants : register(b0) {
    // World-to-clip. The on-screen pass sets the camera's; the shadow pass sets the
    // sun's, so the very same shaders fill the shadow map from the light's point of view.
    row_major float4x4 g_view_projection;
    // The field's minimum corner in world space: x and z are the rectangle's near
    // corner, y is the ground the blades stand on (the field is assumed flat).
    float3 g_patch_origin;
    // The world size of one grid cell -- one mesh-shader group's patch of ground.
    float g_cell_size;
    float3 g_grass_camera; // the eye, for facing the blades, the LOD and the fog
    float g_grass_time;    // seconds, for the wind sway (shares the sky's clock)
    float3 g_blade_color;  // the grass base colour, sRGB, jittered per blade
    float g_blade_height;  // a blade's height in metres, before per-blade variation
    float3 g_grass_sun;    // unit vector toward the sun, world space
    float g_blade_width;   // a blade's width at the base in metres
    float2 g_wind;         // world-space wind direction times strength
    uint2 g_grid;          // cells in x and z
    // Level-of-detail distances from the eye, in metres: full blades within .x, half
    // within .y, quarter beyond; a cell past .z (cull) is dropped entirely. The shadow
    // pass passes these all huge, so it keeps every blade -- popping in a shadow reads
    // worse than the fill cost of drawing it.
    float4 g_lod;
};

// The same per-frame lighting the scene pass reads, bound from the same buffer, so
// the grass takes the level's sun, sky, fog and -- through g_light_view_projection --
// its shadow map with no separate plumbing. Layout must match FrameConstants in
// scene.hlsl: it is the very same b1.
cbuffer FrameConstants : register(b1) {
    row_major float4x4 g_light_view_projection;
    float3 g_camera_position;
    float g_time;
    SkyEnvironment g_sky;
    float3 g_sun_color;
    float g_sun_intensity;
    float3 g_sky_ambient;
    float g_ambient_strength;
    float g_fill_strength;
    float g_fog_start;
    float g_fog_end;
    float g_frame_pad;
};

// The sun's depth buffer from the shadow pass, sampled so a blade knows whether the
// sun reaches it. Only the on-screen pixel shader reads it; the shadow-cast pass has
// no pixel shader. Bound at the same fixed slot the scene pass uses.
Texture2D<float> g_grass_shadow_map : register(t0);
SamplerComparisonState g_grass_shadow_sampler : register(s0);

static const float kPi = 3.14159265f;
// Matches kShadowMapSize in renderer.cpp; sizes one shadow texel for the PCF taps.
static const float kGrassShadowMapSize = 2048.0f;
static const float kGrassShadowTexel = 1.0f / kGrassShadowMapSize;

// A small integer hash -> [0,1). The mesh shader has no textures to seed placement,
// so blade position, height, lean and phase all come from hashing the cell and the
// blade index. PCG-ish: cheap and well-distributed enough that the field reads as
// scattered rather than gridded.
float Hash11(uint n) {
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    n = (n >> 22u) ^ n;
    return float(n) * (1.0f / 4294967296.0f);
}

// The blade count for a cell whose centre is `dist` metres from the eye. A cell past
// the cull distance returns 0 and the amplification shader drops it.
uint LodBlades(float dist) {
    if (dist > g_lod.z) {
        return 0;
    }
    if (dist < g_lod.x) {
        return BLADES_PER_GROUP;
    }
    if (dist < g_lod.y) {
        return BLADES_PER_GROUP / 2;
    }
    return BLADES_PER_GROUP / 4;
}

// The payload the amplification shader fills and hands to the mesh groups it spawns:
// one entry per surviving cell, packed tight, so mesh group `i` reads cell[i]/blades[i].
struct Payload {
    uint cell[AS_GROUP];
    uint blades[AS_GROUP];
};

groupshared Payload s_payload;
groupshared uint s_visible;

// Extract the six frustum planes from g_view_projection and keep the cell if its
// bounding sphere is inside all of them. Gribb-Hartmann: the planes are sums and
// differences of the matrix's columns (its transpose's rows), normalised so the
// signed distance is metric. Works for the camera frustum on-screen and the sun's
// orthographic box in the shadow pass, since both arrive as g_view_projection.
bool SphereInFrustum(float3 center, float radius) {
    const float4x4 t = transpose(g_view_projection);
    float4 planes[6];
    planes[0] = t[3] + t[0]; // left
    planes[1] = t[3] - t[0]; // right
    planes[2] = t[3] + t[1]; // bottom
    planes[3] = t[3] - t[1]; // top
    planes[4] = t[2];        // near (z >= 0 in D3D clip space)
    planes[5] = t[3] - t[2]; // far
    [unroll]
    for (int i = 0; i < 6; ++i) {
        const float len = length(planes[i].xyz);
        if (dot(planes[i].xyz, center) + planes[i].w < -radius * len) {
            return false;
        }
    }
    return true;
}

[numthreads(AS_GROUP, 1, 1)]
void ASMain(uint gtid : SV_GroupThreadID, uint gid : SV_GroupID) {
    if (gtid == 0) {
        s_visible = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    // One cell per thread, laid out row-major over the grid.
    const uint cell_index = gid * AS_GROUP + gtid;
    const uint cell_count = g_grid.x * g_grid.y;

    bool keep = false;
    uint blades = 0;
    if (cell_index < cell_count) {
        const uint cx = cell_index % g_grid.x;
        const uint cz = cell_index / g_grid.x;
        const float2 cell_min = g_patch_origin.xz + float2(cx, cz) * g_cell_size;
        const float3 center =
            float3(cell_min.x + g_cell_size * 0.5f, g_patch_origin.y + g_blade_height * 0.5f,
                   cell_min.y + g_cell_size * 0.5f);
        // A sphere over the cell: half its diagonal, plus the tallest a blade reaches.
        const float radius = g_cell_size * 0.70711f + g_blade_height;
        const float dist = distance(center, g_grass_camera);
        blades = LodBlades(dist);
        keep = blades > 0 && SphereInFrustum(center, radius);
    }

    // Compact the survivors so the mesh groups are launched back-to-back with no gaps.
    if (keep) {
        uint slot;
        InterlockedAdd(s_visible, 1, slot);
        s_payload.cell[slot] = cell_index;
        s_payload.blades[slot] = blades;
    }
    GroupMemoryBarrierWithGroupSync();

    // One mesh group per surviving cell. Zero is legal -- an all-culled group draws
    // nothing.
    DispatchMesh(s_visible, 1, 1, s_payload);
}

struct MSOut {
    float4 position : SV_Position;
    float3 world : POSITION;
    float3 normal : NORMAL;
    // 0 at the base of the blade, 1 at the tip: drives the root-to-tip shade and the
    // ambient-occlusion falloff into the sward.
    float height_t : TEXCOORD0;
    float3 tint : TEXCOORD1;
};

[outputtopology("triangle")]
[numthreads(BLADES_PER_GROUP, 1, 1)]
void MSMain(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, in payload Payload pl,
            out vertices MSOut verts[GROUP_VERTS], out indices uint3 tris[GROUP_PRIMS]) {
    // This group's cell and blade count come from the amplification shader's payload.
    const uint cell_index = pl.cell[gid.x];
    const uint blade_count = pl.blades[gid.x];
    // Uniform across the group, so every thread agrees on the counts.
    SetMeshOutputCounts(blade_count * VERTS_PER_BLADE, blade_count * PRIMS_PER_BLADE);

    const uint blade = gtid;
    if (blade >= blade_count) {
        return; // an LOD-thinned group leaves its spare threads idle
    }

    const uint cx = cell_index % g_grid.x;
    const uint cz = cell_index / g_grid.x;

    // Scatter this blade within its cell. The seed folds the cell coordinates and the
    // blade index so no two blades in the field share a placement -- and, keyed on the
    // cell rather than the launch order, a blade stays put as the LOD count changes.
    const uint seed = (cx * 73856093u) ^ (cz * 19349663u) ^ (blade * 83492791u);
    const float r0 = Hash11(seed);
    const float r1 = Hash11(seed ^ 0x9e3779b9u);
    const float r2 = Hash11(seed + 0x68bc21ebu);
    const float r3 = Hash11(seed * 2654435761u + 1u);
    const float r4 = Hash11(seed ^ 0x27d4eb2fu);
    const float r5 = Hash11(seed + 0x165667b1u);
    const float r6 = Hash11(seed * 40503u + 12345u);

    const float2 cell_min = g_patch_origin.xz + float2(cx, cz) * g_cell_size;
    const float2 pos2 = cell_min + float2(r0, r1) * g_cell_size;
    const float3 base = float3(pos2.x, g_patch_origin.y, pos2.y);

    // Per-blade variation, so the sward is not a lawn of clones.
    const float height = g_blade_height * (0.7f + 0.6f * r2);
    const float width = g_blade_width * (0.75f + 0.5f * r3);
    const float yaw = r4 * (2.0f * kPi);
    const float2 wdir = float2(cos(yaw), sin(yaw)); // the width axis, in the ground plane
    const float3 w = float3(wdir.x, 0.0f, wdir.y);
    // The blade leans along the axis across its width; a static lean plus the wind.
    const float2 lean_axis = float2(-wdir.y, wdir.x);
    const float static_lean = 0.12f * height * (r5 * 2.0f - 1.0f);
    // A travelling sine over the field gives gusts that roll across the yard rather
    // than every blade waving in lockstep.
    const float phase = r6 * (2.0f * kPi) + dot(pos2, float2(0.6f, 0.4f));
    const float sway = sin(g_grass_time * 1.7f + phase);
    const float2 lean = lean_axis * static_lean + g_wind * (0.18f + 0.12f * sway);
    const float3 bend = float3(lean.x, 0.0f, lean.y);

    const float3 tint = g_blade_color * (0.72f + 0.5f * r0);
    const float3 to_cam = g_grass_camera - base;

    const uint v_base = blade * VERTS_PER_BLADE;
    const uint p_base = blade * PRIMS_PER_BLADE;

    // Grow the blade ring by ring. The centre climbs in Y and leans more the higher
    // it goes (a quadratic bend, so the base stays planted and the tip curls over);
    // the half-width tapers to zero at the tip.
    [unroll]
    for (uint i = 0; i <= BLADE_SEGMENTS; ++i) {
        const float t = float(i) / float(BLADE_SEGMENTS);
        const float3 centre = base + float3(0.0f, height * t, 0.0f) + bend * (t * t);
        const float half_width = 0.5f * width * (1.0f - t);

        // The tangent up the blade is d(centre)/dt; the normal faces across it, out
        // of the ribbon. Flip it toward the eye so both sides of the thin blade light.
        const float3 tangent = normalize(float3(0.0f, height, 0.0f) + bend * (2.0f * t));
        float3 normal = normalize(cross(w, tangent));
        if (dot(normal, to_cam) < 0.0f) {
            normal = -normal;
        }

        [unroll]
        for (uint side = 0; side < 2; ++side) {
            const float3 world = centre + w * (half_width * (side == 0 ? -1.0f : 1.0f));
            MSOut o;
            o.position = mul(float4(world, 1.0f), g_view_projection);
            o.world = world;
            o.normal = normal;
            o.height_t = t;
            o.tint = tint;
            verts[v_base + i * 2 + side] = o;
        }
    }

    // Two triangles per quad, wound so the front face (the eye side, once the normal
    // flip has picked it) survives the rasterizer -- though the PSO culls neither
    // side, so a blade edge-on never drops out.
    [unroll]
    for (uint q = 0; q < BLADE_SEGMENTS; ++q) {
        const uint l0 = v_base + q * 2; // lower-left
        const uint r0i = l0 + 1;        // lower-right
        const uint l1 = l0 + 2;         // upper-left
        const uint r1i = l0 + 3;        // upper-right
        tris[p_base + q * 2 + 0] = uint3(l0, l1, r0i);
        tris[p_base + q * 2 + 1] = uint3(r0i, l1, r1i);
    }
}

// The fraction of the sun that reaches `world`: 1 fully lit, 0 fully in shadow. The
// same 3x3 PCF projection the scene pass uses (see SunVisibility in scene.hlsl), so a
// blade sits in the same shadows as the ground it grows from and blades shade one
// another through the very map they cast into.
float GrassSunVisibility(float3 world, float3 normal) {
    const float4 light_clip = mul(float4(world + normal * 0.02f, 1.0f), g_light_view_projection);
    const float3 ndc = light_clip.xyz / light_clip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    if (ndc.z > 1.0f || any(uv < 0.0f) || any(uv > 1.0f)) {
        return 1.0f;
    }
    const float depth = ndc.z - 0.0008f;
    float lit = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 tap = uv + float2(x, y) * kGrassShadowTexel;
            lit += g_grass_shadow_map.SampleCmpLevelZero(g_grass_shadow_sampler, tap, depth);
        }
    }
    return lit / 9.0f;
}

float4 PSMain(MSOut input) : SV_TARGET {
    const float3 normal = normalize(input.normal);
    const float3 albedo = SrgbToLinear(input.tint);

    // Direct sun, Lambert only -- a blade is matte, and a specular lobe on grass reads
    // as wet plastic -- gated by the shadow map so blades shade each other and the
    // grill and fence throw shade across the field. The 1/pi matches the scene's
    // diffuse normalisation so the brightness lines up with the ground beneath.
    const float n_dot_l = saturate(dot(normal, g_grass_sun));
    const float shadow = GrassSunVisibility(input.world, normal);
    const float3 sun =
        SrgbToLinear(g_sun_color) * g_sun_intensity * n_dot_l * shadow * (1.0f / kPi);

    // Hemisphere ambient: sky tone from above, ground bounce from below, the same pair
    // the scene's ambient uses.
    const float3 ambient = lerp(SrgbToLinear(g_sky.ground), SrgbToLinear(g_sky_ambient),
                                saturate(normal.y * 0.5f + 0.5f)) *
                           g_ambient_strength;
    // Darken toward the root, where light is buried in the sward, and let the tip catch
    // the most -- the cheap fake of grass self-occlusion.
    const float ao = lerp(0.45f, 1.0f, input.height_t);

    float3 color = albedo * (sun + ambient * ao);

    // Dissolve into the very sky drawn behind, exactly as the scene fog does, so the
    // far edge of the field melts into the horizon instead of ending on a hard line.
    const float3 view_ray = normalize(input.world - g_camera_position);
    const float dist = distance(input.world, g_grass_camera);
    const float fog = saturate((dist - g_fog_start) / (g_fog_end - g_fog_start));
    color = lerp(color, SampleSky(view_ray, g_time, g_sky), fog * 0.9f);

    return float4(color, 1.0f);
}
