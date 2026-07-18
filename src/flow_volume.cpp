#include "flow_volume.hpp"

#include "dx_common.hpp"

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
    for (NvFlowUint i = 0; i < map.dim; ++i) {
        const float u = static_cast<float>(i) / static_cast<float>(map.dim - 1);
        NvFlowFloat4 c;
        if (u < 0.5f) {
            // Cool half: dark smoke fading up from transparent to a dim ember.
            const float t = u / 0.5f;
            c.x = 0.05f + 0.55f * t;
            c.y = 0.05f + 0.20f * t;
            c.z = 0.05f + 0.05f * t;
            c.w = 0.15f + 0.35f * t;
        } else {
            // Hot half: orange through to a bright yellow-white core.
            const float t = (u - 0.5f) / 0.5f;
            c.x = 0.60f + 0.40f * t;
            c.y = 0.25f + 0.70f * t;
            c.z = 0.10f + 0.65f * t;
            c.w = 0.50f + 0.50f * t;
        }
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
    // Centre the simulation box on the backyard grill and give the plume room to rise.
    gridDesc.initialLocation = {0.0f, 2.5f, 5.0f};
    gridDesc.halfSize = {4.0f, 4.0f, 4.0f};
    impl->grid = NvFlowCreateGrid(impl->context, &gridDesc);

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

    // Queue this frame's emitters. Each is a sphere of hot, smoky fuel: the shape places
    // and sizes the injection, the targets set what is injected, and the upward velocity is
    // the draft rising off the coals. The sim applies them on the update below.
    for (const FlowEmitter& e : emitters) {
        NvFlowShapeDesc shape;
        shape.sphere.radius = e.radius;

        NvFlowGridEmitParams params;
        NvFlowGridEmitParamsDefaults(&params);
        params.shapeType = eNvFlowShapeTypeSphere;
        params.shapeRangeOffset = 0;
        params.shapeRangeSize = 1;
        params.deltaTime = dt;
        params.localToWorld =
            ToFlow(XMMatrixTranslation(e.position.x, e.position.y, e.position.z));
        params.bounds = ToFlow(XMMatrixScaling(1.5f * e.radius, 1.5f * e.radius, 1.5f * e.radius) *
                               XMMatrixTranslation(e.position.x, e.position.y, e.position.z));
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

void FlowVolume::Clear() {
    if (!impl_->grid) {
        return;
    }
    NvFlowGridResetDesc reset;
    NvFlowGridResetDescDefaults(&reset);
    reset.initialLocation = {0.0f, 2.5f, 5.0f};
    reset.halfSize = {4.0f, 4.0f, 4.0f};
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
