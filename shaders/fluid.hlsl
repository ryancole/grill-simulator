// The lighter-fluid spray, pass 1 of the screen-space fluid pipeline: the surface
// depth and thickness. The PhysX PBD particle system simulates the droplets; this pass
// draws each live droplet (world centre + radius, from a structured buffer) as a
// camera-facing sphere impostor and writes two things about it:
//
//   RT0 (R32F, MIN blend): the nearest fluid surface's eye-space depth. MIN blend keeps
//        the smallest (nearest) z per pixel, so overlapping droplets resolve to the front
//        surface without a depth buffer of their own.
//   RT1 (R16F, additive):  the total fluid thickness along the view ray -- each droplet
//        adds its chord length -- which the composite turns into Beer-Lambert absorption.
//
// Both are depth-tested read-only against the world's depth buffer (write mask zero), so
// the yard occludes droplets behind it while all droplets in front accumulate. A later
// pass smooths RT0 and composites the lit, tinted, translucent surface into the scene.
//
// Matrices are row_major and multiplied as `v * M`, matching the scene pass.

cbuffer FluidConstants : register(b0) {
    row_major float4x4 g_view;  // world -> view (camera) space
    row_major float4x4 g_proj;  // view -> clip space
};

// One entry per live droplet: xyz the world-space centre, w the sphere radius (metres).
StructuredBuffer<float4> g_droplets : register(t0);

struct VSOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;          // quad corner in [-1,1], the disc's local coords
    float3 view_center : TEXCOORD1; // droplet centre in view space
    float radius : TEXCOORD2;
};

// The two triangles of the billboard quad, as corner offsets.
static const float2 kCorners[6] = {
    float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
    float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0),
};

VSOut VSMain(uint vertex_id : SV_VertexID, uint instance_id : SV_InstanceID) {
    const float4 droplet = g_droplets[instance_id];
    const float3 view_center = mul(float4(droplet.xyz, 1.0), g_view).xyz;
    const float radius = droplet.w;

    const float2 corner = kCorners[vertex_id];
    const float3 view_pos = float3(view_center.xy + corner * radius, view_center.z);

    VSOut output;
    output.position = mul(float4(view_pos, 1.0), g_proj);
    output.uv = corner;
    output.view_center = view_center;
    output.radius = radius;
    return output;
}

struct PSOut {
    float eye_z : SV_Target0;      // nearest fluid surface depth (view space z)
    float thickness : SV_Target1;  // this droplet's chord length along the ray
};

PSOut PSMain(VSOut input) {
    const float d2 = dot(input.uv, input.uv);
    // Outside the unit disc is the quad's corner: not part of the sphere.
    clip(1.0 - d2);

    // The near hemisphere's offset from the centre, in radii (view space is left-handed,
    // camera down +z, so the surface facing us sits at a smaller z than the centre).
    const float zc = sqrt(1.0 - d2);

    PSOut output;
    output.eye_z = input.view_center.z - zc * input.radius;
    // The chord through the sphere at this pixel: front to back is 2 * zc * radius.
    output.thickness = 2.0 * zc * input.radius;
    return output;
}
