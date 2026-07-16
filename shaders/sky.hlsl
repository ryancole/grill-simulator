// The gradient sky drawn behind everything, so the sky the frame shows and the sky
// the metals reflect are the one SampleSky in common.hlsli. A single fullscreen
// triangle with no vertex buffer; the pixel shader turns each pixel back into a
// world-space view ray and asks the sky what colour lies that way.
#include "common.hlsli"

cbuffer SkyConstants : register(b0) {
    // Clip space back to world: the inverse of the camera's view-projection.
    row_major float4x4 g_inv_view_projection;
    float3 g_camera_position;
    // Vestigial: once the seconds that drifted the flat cloud layer drawn over the
    // sky here. The clouds are volumetric now (shaders/clouds.hlsl), drawn in their
    // own pass, so the background is a static gradient and nothing reads this -- it
    // stays only to keep this buffer's layout in step with its C++ mirror.
    float g_time;
    // The level's atmosphere SampleSky draws from -- the same struct the scene pass
    // carries, so the background and the sky a metal reflects stay one sky. Filled
    // from the C++ Environment; see SkyConstants in renderer.cpp.
    SkyEnvironment g_sky;
};

struct VSOutput {
    float4 position : SV_POSITION;
    // The pixel's clip-space xy, carried across so the pixel shader can shoot a
    // ray through it. w is 1 everywhere, so this interpolates linearly.
    float2 clip : TEXCOORD0;
};

// Emits the three corners of a triangle large enough to cover the screen, picked
// by vertex id so no vertex buffer is bound: id 0,1,2 -> (0,0),(2,0),(0,2) in uv,
// which is (-1,-1),(3,-1),(-1,3) in clip space.
VSOutput VSMain(uint id : SV_VertexID) {
    const float2 uv = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.clip = uv * 2.0f - 1.0f;
    // z = 1 puts it on the far plane; the pass runs with depth off regardless.
    output.position = float4(output.clip, 1.0f, 1.0f);
    return output;
}

// The linear radiance of the sky along this pixel's view ray. A far-plane point in
// clip space, carried back to world, gives the direction from the eye through the
// pixel.
float3 SkyRadiance(VSOutput input) {
    float4 world = mul(float4(input.clip, 1.0f, 1.0f), g_inv_view_projection);
    world /= world.w;
    const float3 dir = normalize(world.xyz - g_camera_position);
    return SampleSky(dir, g_sky);
}

// The on-screen background: into the linear HDR scene buffer, so no encode here --
// the tonemap pass does it, once, for the whole frame.
float4 PSMain(VSOutput input) : SV_TARGET {
    return float4(SkyRadiance(input), 1.0f);
}

// The background behind the reflection-probe capture: into the _UNORM cube sampled
// through an sRGB view, so the linear radiance is encoded here, exactly as the
// scene capture pass does.
float4 PSMainCapture(VSOutput input) : SV_TARGET {
    return float4(LinearToSrgb(SkyRadiance(input)), 1.0f);
}
