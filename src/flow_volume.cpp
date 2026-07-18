#include "flow_volume.hpp"

#include "dx_common.hpp"

#include <algorithm>
#include <cstring>

// NVIDIA Flow's headers hardcode every entry point as `extern "C" __declspec(dllexport)`
// with no override hook -- wrong for a consumer, which wants an import, but harmless: the
// import library resolves the calls and the exe simply re-exports the symbols. Isolating
// the includes to this one translation unit keeps that quirk (and its link-time warnings)
// out of the rest of the build. <d3d12.h> must precede the D3D12 context header.
#include <d3d12.h>
#pragma warning(push)
#pragma warning(disable : 4190) // extern "C" returning a C++ type (NvFlow*Handle by value)
#include "NvFlow.h"
#include "NvFlowContext.h"
#include "NvFlowContextD3D12.h"
#pragma warning(pop)

using namespace DirectX;

namespace {

// A shader-visible CBV/SRV/UAV heap Flow draws scratch descriptors from while it records
// into our command list -- Flow asks for a contiguous block through the reserveDescriptors
// callback, and we hand out slots linearly, wrapping at the end. Sized large enough (as in
// Flow's own demo) that a block handed out is long consumed by the GPU before the cursor
// laps back onto it, so no explicit fencing is needed.
constexpr UINT kFlowDescriptorHeapSize = 8192;

// The colour map is a 1D temperature -> RGBA ramp the ray-march samples: cool low
// temperatures read as dark, thin smoke, hot ones as bright orange-white fire. 64 entries
// is Flow's suggested default resolution.
constexpr UINT kColorMapResolution = 64;

// The simulation grid's virtual resolution and resident fraction, chosen together so a
// level-sized box (~24 m) still has ~2 cm voxels while the resident-voxel budget -- and thus
// the memory -- stays near a small default grid's. See CreateFlowObjects.
constexpr NvFlowUint kGridVirtualDim = 1536;
constexpr float kGridResidentScale = 0.00036f;

NvFlowFloat4x4 ToFlow(FXMMATRIX m) {
    XMFLOAT4X4 f;
    XMStoreFloat4x4(&f, m);
    NvFlowFloat4x4 out;
    static_assert(sizeof(out) == sizeof(f), "NvFlowFloat4x4 must match XMFLOAT4X4 layout");
    std::memcpy(&out, &f, sizeof(out));
    return out;
}

} // namespace

struct FlowVolume::Impl {
    // The renderer's own device objects. Flow records onto command lists from `queue`,
    // versions its internal buffers against `fence`, and allocates on `device`.
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Fence* fence = nullptr;

    // The Flow objects, all created lazily on the first Render (when a recording command
    // list first exists) and torn down in Shutdown.
    NvFlowContext* context = nullptr;
    NvFlowGrid* grid = nullptr;
    NvFlowRenderMaterialPool* material_pool = nullptr;
    NvFlowVolumeRender* volume_render = nullptr;
    NvFlowDepthStencilView* flow_dsv = nullptr;
    NvFlowRenderTargetView* flow_rtv = nullptr;
    bool created = false;

    // The scratch descriptor ring Flow reserves from (see reserveDescriptors below).
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    UINT descriptor_size = 0;
    UINT descriptor_cursor = 0;

    // A one-slot CPU-only (non-shader-visible) heap holding an SRV of the scene depth. Flow
    // copies from the srvHandle we give it, and a copy source must live in a non-shader-
    // visible heap -- so the engine heap's shader-visible depth SRV cannot be handed over.
    ComPtr<ID3D12DescriptorHeap> depth_srv_cpu_heap;

    // Per-frame context inputs, refreshed by FillContextDesc each Render.
    ID3D12GraphicsCommandList* command_list = nullptr;
    UINT64 last_fence_completed = 0;
    UINT64 next_fence_value = 1;

    // The current level's simulation box (SetRegion). The grid is built and reset to this,
    // so a level's fires can sit anywhere -- the backyard grill, the campsite fire pit. The
    // default is the backyard grill, used until the first SetRegion.
    XMFLOAT3 region_center{0.0f, 2.5f, 5.0f};
    float region_half = 4.0f;
};

namespace {

// Flow's descriptor-ring callback: hand back a contiguous block of `num` descriptors from
// the shared heap, advancing (and wrapping) the cursor. The fence arguments are unused --
// the heap is oversized so blocks are never reclaimed while still in flight, matching the
// approach in Flow's DemoAppD3D12.
NvFlowDescriptorReserveHandleD3D12 FlowReserveDescriptors(void* userdata, NvFlowUint num,
                                                          NvFlowUint64 /*lastFenceCompleted*/,
                                                          NvFlowUint64 /*nextFenceValue*/) {
    auto* impl = static_cast<FlowVolume::Impl*>(userdata);
    UINT start = impl->descriptor_cursor;
    if (start + num >= kFlowDescriptorHeapSize) {
        start = 0;
    }
    impl->descriptor_cursor = start + num;

    D3D12_CPU_DESCRIPTOR_HANDLE cpu = impl->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = impl->descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(start) * impl->descriptor_size;
    gpu.ptr += static_cast<UINT64>(start) * impl->descriptor_size;

    NvFlowDescriptorReserveHandleD3D12 handle = {};
    handle.heap = impl->descriptor_heap.Get();
    handle.descriptorSize = impl->descriptor_size;
    handle.cpuHandle = cpu;
    handle.gpuHandle = gpu;
    return handle;
}

} // namespace

FlowVolume::FlowVolume() : impl_(std::make_unique<Impl>()) {}
FlowVolume::~FlowVolume() { Shutdown(); }

void FlowVolume::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Fence* fence) {
    impl_->device = device;
    impl_->queue = queue;
    impl_->fence = fence;

    // The shared scratch ring and the CPU-only depth SRV heap can be built up front; the
    // Flow context and everything hung off it wait for the first Render, where a recording
    // command list exists for the resource-creation uploads.
    D3D12_DESCRIPTOR_HEAP_DESC ring = {};
    ring.NumDescriptors = kFlowDescriptorHeapSize;
    ring.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ring.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(&ring, IID_PPV_ARGS(&impl_->descriptor_heap)),
                  "CreateDescriptorHeap(flow ring)");
    impl_->descriptor_size =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_DESCRIPTOR_HEAP_DESC cpu = {};
    cpu.NumDescriptors = 1;
    cpu.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cpu.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&cpu, IID_PPV_ARGS(&impl_->depth_srv_cpu_heap)),
                  "CreateDescriptorHeap(flow depth srv)");
}

namespace {

// Fills the D3D12 context description Flow needs each frame: our device, queue, fence and
// the frame's command list plus the fence values that bracket it, and the descriptor-ring
// callback. Mirrors NvFlowInteropUpdateContextDesc in Flow's demo.
void FillContextDesc(NvFlowContextDescD3D12& desc, FlowVolume::Impl* impl) {
    desc.device = impl->device;
    desc.commandQueue = impl->queue;
    desc.commandQueueFence = impl->fence;
    desc.commandList = impl->command_list;
    desc.lastFenceCompleted = impl->last_fence_completed;
    desc.nextFenceValue = impl->next_fence_value;
    desc.dynamicHeapCbvSrvUav.userdata = impl;
    desc.dynamicHeapCbvSrvUav.reserveDescriptors = FlowReserveDescriptors;
}

// Writes a plain fire ramp into the material's colour map: dark, near-transparent soot at
// low temperature climbing to opaque orange then hot yellow-white. Refined in Phase 2.
void FillFireColorMap(NvFlowContext* context, NvFlowRenderMaterialHandle material) {
    NvFlowColorMapData map = NvFlowRenderMaterialColorMap(context, material);
    if (!map.data || map.dim == 0) {
        return;
    }
    struct Point {
        float pos;
        NvFlowFloat4 rgba;
    };
    static const Point kCurve[] = {
        {0.00f, {0.090000f, 0.090000f, 0.100000f, 0.35f}}, // cool grey smoke
        {0.35f, {0.240000f, 0.170000f, 0.130000f, 0.55f}}, // warm soot
        {0.65f, {0.835294f, 0.392157f, 0.117647f, 0.80f}}, // orange (demo)
        {0.85f, {1.270000f, 1.200000f, 0.390000f, 0.85f}}, // yellow, HDR (demo)
        {1.00f, {1.500000f, 1.500000f, 1.500000f, 0.80f}}, // white-hot, HDR (demo)
    };
    constexpr int kCount = static_cast<int>(sizeof(kCurve) / sizeof(kCurve[0]));
    for (NvFlowUint i = 0; i < map.dim; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(map.dim - 1);
        int seg = 0;
        while (seg + 2 < kCount && u > kCurve[seg + 1].pos) {
            ++seg;
        }
        const Point& a = kCurve[seg];
        const Point& b = kCurve[seg + 1];
        const float t = b.pos > a.pos ? (u - a.pos) / (b.pos - a.pos) : 0.0f;
        NvFlowFloat4 c;
        c.x = a.rgba.x + (b.rgba.x - a.rgba.x) * t;
        c.y = a.rgba.y + (b.rgba.y - a.rgba.y) * t;
        c.z = a.rgba.z + (b.rgba.z - a.rgba.z) * t;
        c.w = a.rgba.w + (b.rgba.w - a.rgba.w) * t;
        map.data[i] = c;
    }
    NvFlowRenderMaterialColorUnmap(context, material);
}

// Builds the Flow context, grid, material pool + fire material, and defers the volume
// renderer (which needs a grid export) to the caller. Runs once, on the first frame.
void CreateFlowObjects(FlowVolume::Impl* impl) {
    NvFlowContextDescD3D12 contextDesc = {};
    FillContextDesc(contextDesc, impl);
    impl->context = NvFlowCreateContextD3D12(NV_FLOW_VERSION, &contextDesc);

    NvFlowGridDesc gridDesc;
    NvFlowGridDescDefaults(&gridDesc);
    // Centre the simulation box on the level's fire region (SetRegion), giving the plume room
    // to rise. Defaults to the backyard grill until a level sets it.
    gridDesc.initialLocation = {impl->region_center.x, impl->region_center.y, impl->region_center.z};
    gridDesc.halfSize = {impl->region_half, impl->region_half, impl->region_half};
    // The box is level-sized (world.cpp), which would make the default 512^3 grid's voxels
    // huge and the fire coarse and faint. Raise the virtual resolution so voxels stay ~2 cm
    // across the big box, and drop residentScale to match: memory tracks the *resident*
    // voxels (only where smoke is, a small fraction of the box), so keeping that budget fixed
    // holds the footprint near the default's ~400 MB despite the far higher virtual resolution.
    gridDesc.virtualDim = {kGridVirtualDim, kGridVirtualDim, kGridVirtualDim};
    gridDesc.residentScale = kGridResidentScale;
    impl->grid = NvFlowCreateGrid(impl->context, &gridDesc);

    // Tune how the fire moves: vorticity adds the turbulent curl that makes flame and smoke
    // billow rather than stand as a smooth jet, and a stronger buoyancy lifts the hot column.
    // Values from Flow's own simple-flame demo. Applied to the grid's default material, which
    // the default emitter (emitMaterialIndex 0) feeds.
    NvFlowGridMaterialParams gridMat;
    NvFlowGridMaterialParamsDefaults(&gridMat);
    gridMat.vorticityStrength = 5.0f;
    gridMat.vorticityVelocityMask = 0.0f;
    gridMat.vorticityConstantMask = 1.0f;
    gridMat.smoke.macCormackBlendFactor = 0.75f;
    gridMat.buoyancyPerTemp *= 2.0f;
    NvFlowGridSetMaterialParams(impl->grid, NvFlowGridGetDefaultMaterial(impl->grid), &gridMat);

    NvFlowRenderMaterialPoolDesc poolDesc = {};
    poolDesc.colorMapResolution = kColorMapResolution;
    impl->material_pool = NvFlowCreateRenderMaterialPool(impl->context, &poolDesc);

    NvFlowRenderMaterialHandle material = NvFlowGetDefaultRenderMaterial(impl->material_pool);
    FillFireColorMap(impl->context, material);
}

} // namespace

void FlowVolume::Render(ID3D12GraphicsCommandList* command_list, std::uint64_t last_fence_completed,
                        std::uint64_t next_fence_value, float dt,
                        std::span<const FlowEmitter> emitters, const FlowTarget& target,
                        const XMMATRIX& view, const XMMATRIX& projection) {
    if (!impl_->device) {
        return; // Not initialized.
    }

    impl_->command_list = command_list;
    impl_->last_fence_completed = last_fence_completed;
    impl_->next_fence_value = next_fence_value;

    if (!impl_->created) {
        CreateFlowObjects(impl_.get());
        impl_->created = true;
    } else {
        NvFlowContextDescD3D12 contextDesc = {};
        FillContextDesc(contextDesc, impl_.get());
        NvFlowUpdateContextD3D12(impl_->context, &contextDesc);
    }

    // Queue this frame's emitters. Each injects hot, smoky fuel over an oriented box shaped
    // to the burning object, with an upward draft; the sim applies them on the update below.
    for (const FlowEmitter& e : emitters) {
        const XMMATRIX transform = XMLoadFloat4x4(&e.transform);

        NvFlowShapeDesc shape;
        shape.box.halfSize = {e.half_extents.x, e.half_extents.y, e.half_extents.z};

        NvFlowGridEmitParams params;
        NvFlowGridEmitParamsDefaults(&params);
        params.shapeType = eNvFlowShapeTypeBox;
        params.shapeRangeOffset = 0;
        params.shapeRangeSize = 1;
        params.deltaTime = dt;
        params.localToWorld = ToFlow(transform);
        // Allocate blocks around the box (its world position rides the transform's last row),
        // sized generously so the rising plume has room to grow into.
        const float px = e.transform._41, py = e.transform._42, pz = e.transform._43;
        const float span = std::max({e.half_extents.x, e.half_extents.y, e.half_extents.z, 0.15f});
        const float bound = 2.0f * span;
        params.bounds =
            ToFlow(XMMatrixScaling(bound, bound, bound) * XMMatrixTranslation(px, py, pz));
        params.allocationScale = {1.0f, 1.0f, 1.0f};
        params.velocityLinear = {0.0f, e.velocity_up, 0.0f};
        params.smoke = e.smoke;
        params.smokeCoupleRate = 8.0f;
        params.temperature = e.temperature;
        params.temperatureCoupleRate = 8.0f;
        params.fuel = e.fuel;
        params.fuelCoupleRate = 8.0f;

        NvFlowGridEmit(impl_->grid, &shape, 1, &params, 1);
    }

    // Step the simulation forward.
    NvFlowGridUpdate(impl_->grid, impl_->context, dt);

    NvFlowGridExport* gridExport = NvFlowGridGetGridExport(impl_->context, impl_->grid);
    if (!impl_->volume_render) {
        NvFlowVolumeRenderDesc renderDesc = {};
        renderDesc.gridExport = gridExport;
        impl_->volume_render = NvFlowCreateVolumeRender(impl_->context, &renderDesc);
    }

    // Refresh the non-shader-visible depth SRV Flow samples for depth testing.
    const D3D12_CPU_DESCRIPTOR_HANDLE depthSrvCpu =
        impl_->depth_srv_cpu_heap->GetCPUDescriptorHandleForHeapStart();
    impl_->device->CreateShaderResourceView(target.depth_resource, &target.depth_srv_desc,
                                             depthSrvCpu);

    // Describe our HDR render target and scene depth to Flow, in the states the renderer
    // hands them over in (see FlowTarget). Created on the first frame, updated after.
    NvFlowRenderTargetViewDescD3D12 rtvDesc = {};
    rtvDesc.rtvHandle = target.color_rtv;
    rtvDesc.rtvDesc = target.color_rtv_desc;
    rtvDesc.resource = target.color_resource;
    // The concrete states the caller has legacy-transitioned these into just before this
    // call: a render target for the composite, a depth target for the ray-march's depth
    // read (RenderFlow brackets the whole thing through the common layout so the renderer's
    // enhanced barriers and Flow's legacy ones interoperate). Flow assumes concrete states,
    // not COMMON -- handing it COMMON lets D3D12 implicitly promote a resource on first read
    // and then Flow's own before=COMMON barrier mismatches.
    rtvDesc.currentState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    rtvDesc.viewport = target.viewport;
    rtvDesc.scissor = target.scissor;

    NvFlowDepthStencilViewDescD3D12 dsvDesc = {};
    dsvDesc.dsvHandle = target.depth_dsv;
    dsvDesc.dsvDesc = target.depth_dsv_desc;
    dsvDesc.dsvResource = target.depth_resource;
    dsvDesc.dsvCurrentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    dsvDesc.srvHandle = depthSrvCpu;
    dsvDesc.srvDesc = target.depth_srv_desc;
    dsvDesc.srvResource = target.depth_resource;
    dsvDesc.srvCurrentState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    dsvDesc.viewport = target.viewport;

    if (!impl_->flow_rtv) {
        impl_->flow_rtv = NvFlowCreateRenderTargetViewD3D12(impl_->context, &rtvDesc);
        impl_->flow_dsv = NvFlowCreateDepthStencilViewD3D12(impl_->context, &dsvDesc);
    } else {
        NvFlowUpdateRenderTargetViewD3D12(impl_->context, impl_->flow_rtv, &rtvDesc);
        NvFlowUpdateDepthStencilViewD3D12(impl_->context, impl_->flow_dsv, &dsvDesc);
    }

    NvFlowVolumeRenderParams renderParams;
    NvFlowVolumeRenderParamsDefaults(&renderParams);
    renderParams.projectionMatrix = ToFlow(projection);
    renderParams.viewMatrix = ToFlow(view);
    renderParams.modelMatrix = ToFlow(XMMatrixIdentity());
    renderParams.depthStencilView = impl_->flow_dsv;
    renderParams.renderTargetView = impl_->flow_rtv;
    renderParams.materialPool = impl_->material_pool;
    renderParams.renderMode = eNvFlowVolumeRenderMode_colormap;
    renderParams.renderChannel = eNvFlowGridTextureChannelDensity;

    NvFlowVolumeRenderGridExport(impl_->volume_render, impl_->context, gridExport, &renderParams);
}

void FlowVolume::SetRegion(XMFLOAT3 center, float half_extent) {
    impl_->region_center = center;
    impl_->region_half = half_extent;
    // If the grid is already up, move (and clear) it to the new box now; otherwise the stored
    // values are picked up when it is first built. Reset repositions and empties in one go.
    if (impl_->grid) {
        NvFlowGridResetDesc reset;
        NvFlowGridResetDescDefaults(&reset);
        reset.initialLocation = {center.x, center.y, center.z};
        reset.halfSize = {half_extent, half_extent, half_extent};
        NvFlowGridReset(impl_->grid, &reset);
    }
}

void FlowVolume::Clear() {
    if (!impl_->grid) {
        return;
    }
    NvFlowGridResetDesc reset;
    NvFlowGridResetDescDefaults(&reset);
    reset.initialLocation = {impl_->region_center.x, impl_->region_center.y, impl_->region_center.z};
    reset.halfSize = {impl_->region_half, impl_->region_half, impl_->region_half};
    NvFlowGridReset(impl_->grid, &reset);
}

void FlowVolume::Shutdown() {
    if (!impl_) {
        return;
    }
    if (impl_->flow_rtv) {
        NvFlowReleaseRenderTargetView(impl_->flow_rtv);
        impl_->flow_rtv = nullptr;
    }
    if (impl_->flow_dsv) {
        NvFlowReleaseDepthStencilView(impl_->flow_dsv);
        impl_->flow_dsv = nullptr;
    }
    if (impl_->volume_render) {
        NvFlowReleaseVolumeRender(impl_->volume_render);
        impl_->volume_render = nullptr;
    }
    if (impl_->material_pool) {
        NvFlowReleaseRenderMaterialPool(impl_->material_pool);
        impl_->material_pool = nullptr;
    }
    if (impl_->grid) {
        NvFlowReleaseGrid(impl_->grid);
        impl_->grid = nullptr;
    }
    if (impl_->context) {
        NvFlowReleaseContext(impl_->context);
        impl_->context = nullptr;
    }
    impl_->created = false;
}
