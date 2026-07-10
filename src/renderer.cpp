#include "renderer.hpp"

#include <d3dx12/d3dx12.h>

#include <DirectXMath.h>

#include <cstring>

using namespace DirectX;

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

// The sky the fog fades into, so the horizon has no seam.
constexpr float kSkyColor[] = {0.52f, 0.62f, 0.76f, 1.0f};

// Mirrors the `Constants` cbuffer in shaders/scene.hlsl. Root constants live in
// the command list itself: at 144 bytes per prop there is nothing here worth the
// descriptor heap and the aliasing rules a real constant buffer would cost.
struct Constants {
    XMFLOAT4X4 mvp;
    XMFLOAT4X4 model;
    XMFLOAT3 albedo;
    float checker;
};
static_assert(sizeof(Constants) % sizeof(UINT) == 0);
constexpr UINT kConstantDwords = sizeof(Constants) / sizeof(UINT);
static_assert(kConstantDwords <= 64, "A root signature holds at most 64 DWORDs in total");

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

} // namespace

void Renderer::Initialize(HWND hwnd, UINT width, UINT height, const Scene& scene) {
    width_ = width;
    height_ = height;

    CreateDevice();
    CreateCommandObjects();
    CreateSwapChain(hwnd, width, height);
    CreateRenderTargetViews();
    CreateDepthBuffer();
    CreatePipeline();
    CreateSceneGeometry(scene);

    initialized_ = true;
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

    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.NumDescriptors = kFrameCount;
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap_)),
                  "CreateDescriptorHeap(RTV)");
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
    dsv_desc.NumDescriptors = 1;
    dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device_->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap_)),
                  "CreateDescriptorHeap(DSV)");
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
    const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(
        kDepthFormat, width_, height_, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ThrowIfFailed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear,
                                                   IID_PPV_ARGS(&depth_stencil_)),
                  "CreateCommittedResource(depth buffer)");

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = kDepthFormat;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device_->CreateDepthStencilView(depth_stencil_.Get(), &dsv,
                                    dsv_heap_->GetCPUDescriptorHandleForHeapStart());
}

void Renderer::CreatePipeline() {
    // The per-prop transform and colour arrive as root constants, so there is no
    // descriptor table and nothing to bind but the vertex and index buffers.
    CD3DX12_ROOT_PARAMETER constants;
    constants.InitAsConstants(kConstantDwords, 0);

    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(1, &constants, 0, nullptr,
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

    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

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
    pso.RTVFormats[0] = kBackBufferFormat;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)),
                  "CreateGraphicsPipelineState");
}

void Renderer::CreateSceneGeometry(const Scene& scene) {
    const std::vector<Vertex>& vertices = scene.Vertices();
    const std::vector<std::uint16_t>& indices = scene.Indices();
    index_count_ = static_cast<UINT>(indices.size());

    const UINT64 vertex_bytes = vertices.size() * sizeof(Vertex);
    const UINT64 index_bytes = indices.size() * sizeof(std::uint16_t);

    // The mesh never changes, so it lives in the default heap -- video memory --
    // and is filled once through a staging copy. An upload-heap buffer would sit
    // in system memory and be re-read across PCIe on every draw.
    auto create = [this](UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                         ComPtr<ID3D12Resource>& out, const char* what) {
        const CD3DX12_HEAP_PROPERTIES heap(type);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
        ThrowIfFailed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                       nullptr, IID_PPV_ARGS(&out)),
                      what);
    };

    ComPtr<ID3D12Resource> vertex_staging;
    ComPtr<ID3D12Resource> index_staging;
    create(vertex_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, vertex_buffer_,
           "CreateCommittedResource(vertex buffer)");
    create(index_bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, index_buffer_,
           "CreateCommittedResource(index buffer)");
    create(vertex_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, vertex_staging,
           "CreateCommittedResource(vertex staging)");
    create(index_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, index_staging,
           "CreateCommittedResource(index staging)");

    auto fill = [](ID3D12Resource* buffer, const void* data, UINT64 bytes) {
        void* mapped = nullptr;
        const CD3DX12_RANGE no_read(0, 0);
        ThrowIfFailed(buffer->Map(0, &no_read, &mapped), "Buffer::Map");
        std::memcpy(mapped, data, static_cast<size_t>(bytes));
        buffer->Unmap(0, nullptr);
    };
    fill(vertex_staging.Get(), vertices.data(), vertex_bytes);
    fill(index_staging.Get(), indices.data(), index_bytes);

    ThrowIfFailed(allocators_[frame_index_]->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocators_[frame_index_].Get(), nullptr),
                  "CommandList::Reset");

    command_list_->CopyBufferRegion(vertex_buffer_.Get(), 0, vertex_staging.Get(), 0, vertex_bytes);
    command_list_->CopyBufferRegion(index_buffer_.Get(), 0, index_staging.Get(), 0, index_bytes);

    const CD3DX12_RESOURCE_BARRIER barriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(vertex_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
        CD3DX12_RESOURCE_BARRIER::Transition(index_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                             D3D12_RESOURCE_STATE_INDEX_BUFFER),
    };
    command_list_->ResourceBarrier(_countof(barriers), barriers);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    // The staging buffers are released when this function returns, so the copy
    // has to have landed before it does.
    FlushGpu();

    vertex_buffer_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
    vertex_buffer_view_.StrideInBytes = sizeof(Vertex);
    vertex_buffer_view_.SizeInBytes = static_cast<UINT>(vertex_bytes);

    index_buffer_view_.BufferLocation = index_buffer_->GetGPUVirtualAddress();
    index_buffer_view_.Format = DXGI_FORMAT_R16_UINT;
    index_buffer_view_.SizeInBytes = static_cast<UINT>(index_bytes);
}

void Renderer::Render(const Scene& scene, const XMMATRIX& view_projection) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->RSSetViewports(1, &viewport_);
    command_list_->RSSetScissorRects(1, &scissor_);

    const CD3DX12_RESOURCE_BARRIER to_render_target = CD3DX12_RESOURCE_BARRIER::Transition(
        render_targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_PRESENT,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    command_list_->ResourceBarrier(1, &to_render_target);

    const CD3DX12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart(),
                                            static_cast<INT>(frame_index_), rtv_size_);
    const CD3DX12_CPU_DESCRIPTOR_HANDLE dsv(dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    command_list_->ClearRenderTargetView(rtv, kSkyColor, 0, nullptr);
    command_list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 1, &vertex_buffer_view_);
    command_list_->IASetIndexBuffer(&index_buffer_view_);

    for (const Prop& prop : scene.Props()) {
        const XMMATRIX model = XMLoadFloat4x4(&prop.transform);

        Constants constants{};
        XMStoreFloat4x4(&constants.mvp, model * view_projection);
        constants.model = prop.transform;
        constants.albedo = prop.color;
        constants.checker = prop.checker;

        command_list_->SetGraphicsRoot32BitConstants(0, kConstantDwords, &constants, 0);
        command_list_->DrawIndexedInstanced(index_count_, 1, 0, 0, 0);
    }

    const CD3DX12_RESOURCE_BARRIER to_present = CD3DX12_RESOURCE_BARRIER::Transition(
        render_targets_[frame_index_].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PRESENT);
    command_list_->ResourceBarrier(1, &to_present);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");

    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    ThrowIfFailed(swap_chain_->Present(1, 0), "SwapChain::Present");

    MoveToNextFrame();
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
}

void Renderer::Shutdown() {
    if (initialized_) {
        FlushGpu();
    }
    if (fence_event_) {
        CloseHandle(fence_event_);
        fence_event_ = nullptr;
    }
    initialized_ = false;
}
