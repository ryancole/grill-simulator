// The lighter-fluid spray, drawn as screen-space sphere impostors. The PhysX PBD
// particle system simulates the droplets; this pass draws them. Each live droplet is
// one point (world centre + radius) in a structured buffer; the vertex shader
// billboards it into a camera-facing quad, and the pixel shader carves that quad into
// a lit sphere -- discarding the corners outside the disc, reconstructing the sphere
// normal, and writing the bulged front depth to SV_Depth so the beads occlude the
// world, and each other, in true 3D. It replaces the flat tinted cubes the fluid used
// to draw.
//
// Matrices are row_major and multiplied as `v * M`, matching the scene pass. Lighting
// is done in view space: the sun direction arrives pre-rotated into view space, so the
// pixel shader needs no inverse-view to shade.

cbuffer FluidConstants : register(b0) {
    row_major float4x4 g_view;  // world -> view (camera) space
    row_major float4x4 g_proj;  // view -> clip space
    // The scene's sun and sky-ambient terms, as the environment stores them (sRGB, with a
    // separate multiplier), so the droplets are lit like the world around them. Converted
    // to linear and scaled in the shader, exactly as the scene pass does.
    float3 g_sun_color;
    float g_sun_intensity;
    float3 g_sky_ambient;
    float g_ambient_strength;
    // The pale-straw naphtha tint the beads take (sRGB), and the sun rotated into view
    // space (a direction, toward the sun) so shading needs no world<->view round trip.
    float3 g_tint;
    float g_pad0;
    float3 g_sun_dir_view;
    float g_pad1;
};

// One entry per live droplet: xyz the world-space centre, w the sphere radius (metres).
StructuredBuffer<float4> g_droplets : register(t0);

// The scene's sRGB->linear, inlined so this pass stays self-contained.
float3 SrgbToLinear(float3 c) {
    return select(c <= 0.04045, c / 12.92, pow((c + 0.055) / 1.055, 2.4));
}

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
    // Face the camera: offset in the view plane, depth flat across the quad (the pixel
    // shader bulges the true front depth per-pixel below).
    const float3 view_pos = float3(view_center.xy + corner * radius, view_center.z);

    VSOut output;
    output.position = mul(float4(view_pos, 1.0), g_proj);
    output.uv = corner;
    output.view_center = view_center;
    output.radius = radius;
    return output;
}

struct PSOut {
    float4 color : SV_Target;
    float depth : SV_Depth;
};

PSOut PSMain(VSOut input) {
    const float d2 = dot(input.uv, input.uv);
    // Outside the unit disc is the quad's corner: not part of the sphere.
    clip(1.0 - d2);

    // The near hemisphere: z toward the camera (view space is left-handed, camera looks
    // down +z, so the surface facing us sits at a smaller z than the centre).
    const float zc = sqrt(1.0 - d2);
    const float3 normal = normalize(float3(input.uv, -zc));
    const float3 front_view =
        float3(input.view_center.xy + input.uv * input.radius, input.view_center.z - zc * input.radius);

    const float3 view_dir = normalize(-front_view);  // toward the camera
    const float3 light_dir = g_sun_dir_view;         // toward the sun

    // The scene's own sun and ambient, in linear light. The sun is divided by pi so the
    // diffuse term is energy-normalized like the scene's PBR -- without it a lit bead
    // exceeds 1 and blows out into a glowing pearl (and trips the bloom threshold).
    const float3 sun = SrgbToLinear(g_sun_color) * (g_sun_intensity / 3.14159265);
    const float3 ambient = SrgbToLinear(g_sky_ambient) * g_ambient_strength;
    const float3 tint = SrgbToLinear(g_tint);

    // Lit straw droplet: sky-ambient fill plus a sun diffuse term, a tight Blinn-Phong
    // specular for the wet glint, and a modest Fresnel rim that lifts the grazing edge the
    // way a rounded bead catches the sky.
    const float ndotl = saturate(dot(normal, light_dir));
    const float3 diffuse = tint * (ambient + sun * ndotl);

    const float3 half_vec = normalize(light_dir + view_dir);
    const float3 specular = sun * pow(saturate(dot(normal, half_vec)), 48.0);

    const float fresnel = pow(1.0 - saturate(dot(normal, view_dir)), 4.0);
    const float3 rim = ambient * (fresnel * 0.5);

    // The bulged front, projected, gives the true per-pixel depth: the beads occlude the
    // world and each other as real spheres, not as flat billboards.
    const float4 clip = mul(float4(front_view, 1.0), g_proj);

    PSOut output;
    output.color = float4(diffuse + specular + rim, 1.0);
    output.depth = clip.z / clip.w;
    return output;
}
