#include "renderer.hpp"

#include <d3dx12/d3dx12.h>

#include <DirectXMath.h>

#include <cstring>

using namespace DirectX;

namespace {

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT4 color;
};

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

void Renderer::Initialize(HWND hwnd, UINT width, UINT height) {
    width_ = width;
    height_ = height;

    CreateDevice();
    CreateCommandObjects();
    CreateSwapChain(hwnd, width, height);
    CreateRenderTargetViews();
    CreatePipeline();
    CreateTriangle();

    initialized_ = true;
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
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
                  "CreateDescriptorHeap");
    rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
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

void Renderer::CreatePipeline() {
    // An empty root signature: the triangle's colour comes from the vertex
    // buffer, so the shaders bind nothing.
    CD3DX12_ROOT_SIGNATURE_DESC root_desc;
    root_desc.Init(0, nullptr, 0, nullptr,
                   D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

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
    const std::vector<std::byte> vs = ReadBinaryFile(shader_dir / "triangle.vs.cso");
    const std::vector<std::byte> ps = ReadBinaryFile(shader_dir / "triangle.ps.cso");

    const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.InputLayout = {input_layout, _countof(input_layout)};
    pso.pRootSignature = root_signature_.Get();
    pso.VS = {vs.data(), vs.size()};
    pso.PS = {ps.data(), ps.size()};
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso.SampleDesc.Count = 1;

    ThrowIfFailed(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline_state_)),
                  "CreateGraphicsPipelineState");
}

void Renderer::CreateTriangle() {
    const Vertex vertices[] = {
        {{0.0f, 0.5f, 0.0f}, {1.0f, 0.2f, 0.1f, 1.0f}},
        {{0.5f, -0.5f, 0.0f}, {1.0f, 0.6f, 0.0f, 1.0f}},
        {{-0.5f, -0.5f, 0.0f}, {0.9f, 0.1f, 0.0f, 1.0f}},
    };

    // An upload-heap vertex buffer is the wrong choice for real geometry -- it
    // lives in system memory and is read over PCIe every draw. For three
    // vertices it avoids a staging copy and a second resource.
    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC buffer = CD3DX12_RESOURCE_DESC::Buffer(sizeof(vertices));
    ThrowIfFailed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&vertex_buffer_)),
                  "CreateCommittedResource(vertex buffer)");

    void* mapped = nullptr;
    const CD3DX12_RANGE no_read(0, 0);
    ThrowIfFailed(vertex_buffer_->Map(0, &no_read, &mapped), "VertexBuffer::Map");
    std::memcpy(mapped, vertices, sizeof(vertices));
    vertex_buffer_->Unmap(0, nullptr);

    vertex_buffer_view_.BufferLocation = vertex_buffer_->GetGPUVirtualAddress();
    vertex_buffer_view_.StrideInBytes = sizeof(Vertex);
    vertex_buffer_view_.SizeInBytes = sizeof(vertices);
}

void Renderer::Render() {
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
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    constexpr float kCharcoal[] = {0.06f, 0.06f, 0.07f, 1.0f};
    command_list_->ClearRenderTargetView(rtv, kCharcoal, 0, nullptr);

    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->IASetVertexBuffers(0, 1, &vertex_buffer_view_);
    command_list_->DrawInstanced(3, 1, 0, 0);

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

    DXGI_SWAP_CHAIN_DESC desc{};
    ThrowIfFailed(swap_chain_->GetDesc(&desc), "SwapChain::GetDesc");
    ThrowIfFailed(
        swap_chain_->ResizeBuffers(kFrameCount, width, height, desc.BufferDesc.Format, desc.Flags),
        "SwapChain::ResizeBuffers");

    width_ = width;
    height_ = height;
    frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

    CreateRenderTargetViews();
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
