#pragma once

#include <DirectXMath.h>
#include <d3d12.h>

#include <cstdint>
#include <memory>
#include <span>

// NVIDIA Flow -- the sparse-voxel GPU fluid sim that grows the grill's smoke and the
// coal fire (see cmake/Flow.cmake for why this is the old GameWorks 1.0.1 SDK and not
// Flow 2). A thin wrapper that owns Flow's simulation grid, its volume ray-marcher and
// its render material, and drives them each frame off our own D3D12 device: Flow records
// its compute and its composite straight into the command list we hand it, and ray-marches
// the fire into our HDR scene buffer, depth-tested against our scene depth so the smoke
// is occluded by the grill and the world exactly as any solid object would be.
//
// Every NvFlow* type is confined to flow_volume.cpp -- the SDK's headers declare their
// entry points __declspec(dllexport), which is wrong to see from a consumer, so this
// class is a PIMPL and nothing else in the build includes them. What crosses the boundary
// here is plain D3D12 and DirectXMath.
//
// Session-persistent, like Flame and Fluid: one instance lives for the whole run and
// Clear() empties the grid on a level swap so a plume lit in one yard does not hang in
// the air of the next.

// One fire source handed to the sim this frame, in world space. A lit grill grate or a
// burning log is one of these: hot fuel injected into the grid at a point, which the sim
// turns into rising, cooling, dissipating smoke and flame.
struct FlowEmitter {
    DirectX::XMFLOAT3 position; // World-space centre of the emit region.
    float radius;               // Radius of the emit sphere, metres.
    float temperature;          // Target temperature injected (drives buoyancy + fire colour).
    float smoke;                // Target smoke density injected (the visible soot).
    float fuel;                 // Target fuel injected (burns to sustain the flame).
    float velocity_up;          // Initial upward velocity imparted, m/s (the draft off the coals).
};

// Everything Flow needs to composite into our frame, filled by the renderer from its own
// resources. All plain D3D12: the HDR buffer as a render target, the scene depth as both
// a depth view (in case Flow writes nominal depth) and a shader-resource view (which it
// samples to stop each ray at the first surface). The states are what these resources are
// *currently* in when Render is called -- Flow issues its own barriers relative to them
// and restores them, so the caller hands them over in the layouts named here and gets them
// back unchanged. See renderer.cpp's Flow insertion point.
struct FlowTarget {
    // The HDR scene buffer, in D3D12_RESOURCE_STATE_RENDER_TARGET.
    ID3D12Resource* color_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE color_rtv;
    D3D12_RENDER_TARGET_VIEW_DESC color_rtv_desc;

    // The scene depth buffer, in D3D12_RESOURCE_STATE_DEPTH_WRITE. Flow reads it through
    // the SRV desc/handle for depth testing; it only touches the DSV if asked to write
    // nominal depth (it is not, here).
    ID3D12Resource* depth_resource;
    D3D12_CPU_DESCRIPTOR_HANDLE depth_dsv;
    D3D12_DEPTH_STENCIL_VIEW_DESC depth_dsv_desc;
    D3D12_SHADER_RESOURCE_VIEW_DESC depth_srv_desc;

    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
};

class FlowVolume {
public:
    FlowVolume();
    ~FlowVolume();
    FlowVolume(const FlowVolume&) = delete;
    FlowVolume& operator=(const FlowVolume&) = delete;

    // Brings up the Flow context on our device and creates the grid, the volume renderer
    // and the fire material. `queue` and `fence` are the renderer's own -- Flow versions
    // its internal upload buffers against that fence, and records onto that queue's command
    // lists. One-time; call once the device exists, before the first Render.
    void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence);

    // Steps the sim `dt` forward from this frame's `emitters` and ray-marches the result
    // into `target`, recording into `command_list`. `last_fence_completed` is the queue
    // fence's completed value and `next_fence_value` the value that will be signalled once
    // this command list retires -- together they tell Flow which internal buffers are free
    // to reuse. `view` and `projection` are the camera's, row-major as DirectXMath stores
    // them (Flow's own convention); the grid lives in world space, so the model matrix is
    // identity. A no-op before Initialize.
    void Render(ID3D12GraphicsCommandList* command_list, std::uint64_t last_fence_completed,
                std::uint64_t next_fence_value, float dt, std::span<const FlowEmitter> emitters,
                const FlowTarget& target, const DirectX::XMMATRIX& view,
                const DirectX::XMMATRIX& projection);

    // Positions the simulation box for the current level: `center` is its middle in world
    // space and `half_extent` its half-size in metres (the box is that wide in x and z, and
    // a little taller so the plume has headroom). Fire outside the box is not simulated, so
    // a level sets this around wherever its fires can burn -- the grill grate, the fire pit.
    // Takes effect immediately if the grid already exists (it is repositioned and cleared),
    // and is remembered for when the grid is first built otherwise. Call on level load.
    void SetRegion(DirectX::XMFLOAT3 center, float half_extent);

    // Empties the grid, preserving its allocation -- the level-swap reset, so smoke from
    // one level does not carry into the next. Mirrors Flame::Clear.
    void Clear();

    // Releases the Flow context and every object built on it. Call before the device goes
    // away at shutdown.
    void Shutdown();

    // Opaque state, defined in flow_volume.cpp so the NvFlow headers stay out of this one.
    // Named publicly only so the file's Flow-side helper functions (and the C descriptor-
    // reservation callback Flow calls back through) can reach it; nothing outside the .cpp
    // can do anything with an incomplete type.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
