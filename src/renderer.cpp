#include "renderer.hpp"

#include "image.hpp"

#include <d3dx12/d3dx12.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cstring>

using namespace DirectX;

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

// The sky the fog fades into, so the horizon has no seam.
constexpr float kSkyColor[] = {0.52f, 0.62f, 0.76f, 1.0f};

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
    float padding;
};

static_assert(sizeof(Constants) % sizeof(UINT) == 0);
constexpr UINT kConstantDwords = sizeof(Constants) / sizeof(UINT);
// The descriptor table for the base colour texture costs one more DWORD.
static_assert(kConstantDwords + 1 <= 64, "A root signature holds at most 64 DWORDs in total");

// _UNORM rather than _UNORM_SRGB, and deliberately. Nothing in this renderer
// converts to linear light: the flat colours in scene.cpp, the constants in the
// shader, the box filter that builds the mips and the back buffer are all in
// whatever space they were written. Decoding only the textures would leave them
// the one linear thing in the frame, reading darker than the flat colour beside
// them. Fixing it is a single change that has to touch all five.
constexpr DXGI_FORMAT kTextureFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

// Slot 0 of the texture heap. A material with no base colour texture points
// here, which spares the shader a branch and the pipeline a second variant.
constexpr UINT kWhiteTexture = 0;

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
};

static_assert(sizeof(TextConstants) % sizeof(UINT) == 0);
constexpr UINT kTextConstantDwords = sizeof(TextConstants) / sizeof(UINT);

// The most glyphs one HUD line can draw. A prompt is a few words; this is roomy.
constexpr UINT kMaxTextGlyphs = 256;
constexpr UINT kTextVerticesPerGlyph = 6; // Two triangles.
constexpr UINT kTextRegionVertices = kMaxTextGlyphs * kTextVerticesPerGlyph;
constexpr UINT kTextRegionBytes = kTextRegionVertices * sizeof(TextVertex);

// Text height as a fraction of the back buffer's, so the prompt keeps its size
// across resolutions.
constexpr float kTextHeightFraction = 0.040f;
// The shadow's offset from the text, in pixels, down and to the right.
constexpr float kTextShadowPixels = 1.25f;

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
    CreateTextPipeline();

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
    // The per-draw transform and colour arrive as root constants. The one thing
    // that cannot travel that way is the base colour texture, which needs a
    // descriptor table, and the sampler that reads it is baked into the root
    // signature -- every texture in the game wants the same one.
    const CD3DX12_DESCRIPTOR_RANGE base_color_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsConstants(kConstantDwords, 0);
    parameters[1].InitAsDescriptorTable(1, &base_color_range, D3D12_SHADER_VISIBILITY_PIXEL);

    // Anisotropic because the ground and the patio are seen almost edge on, and
    // trilinear alone turns them to mush a few metres out.
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_ANISOTROPIC);
    sampler.MaxAnisotropy = 8;
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
    };
    static_assert(sizeof(Vertex) == 32, "The input layout above spells out Vertex's offsets");

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

ComPtr<ID3D12Resource> Renderer::UploadBuffer(const void* data, UINT64 bytes,
                                              D3D12_RESOURCE_STATES final_state,
                                              std::vector<ComPtr<ID3D12Resource>>& staging) {
    auto create = [this](UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state,
                         const char* what) {
        const CD3DX12_HEAP_PROPERTIES heap(type);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size);
        ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                       nullptr, IID_PPV_ARGS(&resource)),
                      what);
        return resource;
    };

    // The mesh never changes, so it lives in the default heap -- video memory --
    // and is filled once through a staging copy. An upload-heap buffer would sit
    // in system memory and be re-read across PCIe on every draw.
    ComPtr<ID3D12Resource> buffer = create(bytes, D3D12_HEAP_TYPE_DEFAULT,
                                           D3D12_RESOURCE_STATE_COPY_DEST,
                                           "CreateCommittedResource(buffer)");
    ComPtr<ID3D12Resource> upload = create(bytes, D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ,
                                           "CreateCommittedResource(buffer staging)");

    void* mapped = nullptr;
    const CD3DX12_RANGE no_read(0, 0);
    ThrowIfFailed(upload->Map(0, &no_read, &mapped), "Buffer::Map");
    std::memcpy(mapped, data, static_cast<size_t>(bytes));
    upload->Unmap(0, nullptr);

    command_list_->CopyBufferRegion(buffer.Get(), 0, upload.Get(), 0, bytes);
    const CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, final_state);
    command_list_->ResourceBarrier(1, &barrier);

    staging.push_back(std::move(upload));
    return buffer;
}

ComPtr<ID3D12Resource> Renderer::UploadTexture(const Image& image, UINT descriptor,
                                               std::vector<ComPtr<ID3D12Resource>>& staging) {
    const auto mip_levels = static_cast<UINT16>(image.levels.size());

    const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
    const CD3DX12_RESOURCE_DESC desc =
        CD3DX12_RESOURCE_DESC::Tex2D(kTextureFormat, image.width, image.height, 1, mip_levels);

    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&texture)),
                  "CreateCommittedResource(texture)");

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

    const CD3DX12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    command_list_->ResourceBarrier(1, &barrier);

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = kTextureFormat;
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
    // The white texture, then every model image, then the HUD font atlas last.
    heap_desc.NumDescriptors = 1 + image_count + 1;
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

    Image white{};
    white.width = 1;
    white.height = 1;
    white.levels.push_back(std::vector<std::byte>(4, std::byte{0xff}));
    textures_.push_back(UploadTexture(white, kWhiteTexture, staging));

    UINT next_descriptor = kWhiteTexture + 1;
    models_.resize(models.size());

    for (size_t i = 0; i < models.size(); ++i) {
        const Model& model = models[i];
        GpuModel& gpu = models_[i];

        const UINT64 vertex_bytes = model.vertices.size() * sizeof(Vertex);
        const UINT64 index_bytes = model.indices.size() * sizeof(std::uint32_t);

        gpu.vertex_buffer = UploadBuffer(model.vertices.data(), vertex_bytes,
                                         D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, staging);
        gpu.vertex_buffer_view.BufferLocation = gpu.vertex_buffer->GetGPUVirtualAddress();
        gpu.vertex_buffer_view.StrideInBytes = sizeof(Vertex);
        gpu.vertex_buffer_view.SizeInBytes = static_cast<UINT>(vertex_bytes);

        gpu.index_buffer = UploadBuffer(model.indices.data(), index_bytes,
                                        D3D12_RESOURCE_STATE_INDEX_BUFFER, staging);
        gpu.index_buffer_view.BufferLocation = gpu.index_buffer->GetGPUVirtualAddress();
        gpu.index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
        gpu.index_buffer_view.SizeInBytes = static_cast<UINT>(index_bytes);

        // Where each of this model's images landed in the shared heap.
        std::vector<UINT> descriptors;
        descriptors.reserve(model.images.size());
        for (const Image& image : model.images) {
            textures_.push_back(UploadTexture(image, next_descriptor, staging));
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
            UINT descriptor = kWhiteTexture;
            if (primitive.material >= 0) {
                const Material& material = model.materials[primitive.material];
                draw.base_color = material.base_color;
                if (material.base_color_image >= 0) {
                    descriptor = descriptors[material.base_color_image];
                }
            }
            draw.base_color_texture = TextureHandle(descriptor);

            gpu.primitives.push_back(draw);
        }
    }

    // The HUD font atlas takes the descriptor after the last model image, and
    // rides the same command list and staging buffers to the GPU.
    atlas_descriptor_ = next_descriptor;
    LoadFontAtlas(staging);

    ThrowIfFailed(command_list_->Close(), "CommandList::Close");
    ID3D12CommandList* lists[] = {command_list_.Get()};
    queue_->ExecuteCommandLists(_countof(lists), lists);

    // The staging buffers are released when this function returns, so the copies
    // have to have landed before it does.
    FlushGpu();
}

void Renderer::LoadFontAtlas(std::vector<ComPtr<ID3D12Resource>>& staging) {
    const std::filesystem::path dir = ExecutableDirectory() / "assets" / "fonts";
    font_ = LoadFontCsv(dir / "hud.csv");

    const std::vector<std::byte> png = ReadBinaryFile(dir / "hud.png");
    Image atlas = DecodeImage(png);
    atlas_width_ = atlas.width;
    atlas_height_ = atlas.height;

    // Deliberately no mip chain: a lower mip of a distance-field atlas averages
    // neighbouring glyphs' distances together, so the edges bleed and the text
    // turns to mush the moment it is minified. DecodeImage built the chain; drop
    // all but the full-resolution level before the upload.
    atlas.levels.resize(1);
    atlas_texture_ = UploadTexture(atlas, atlas_descriptor_, staging);
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

void Renderer::DrawText(std::string_view text) {
    if (text.empty() || width_ == 0 || height_ == 0) {
        return;
    }

    const float pixel = static_cast<float>(height_) * kTextHeightFraction;

    // Centre the line: total advance sets the left edge, and the baseline sits a
    // little above the bottom of the screen.
    float total_width = 0.0f;
    for (const char c : text) {
        if (const Glyph* glyph = font_.Find(static_cast<unsigned char>(c))) {
            total_width += glyph->advance * pixel;
        }
    }
    float pen_x = (static_cast<float>(width_) - total_width) * 0.5f;
    const float baseline = static_cast<float>(height_) - pixel * 2.2f;

    const float atlas_w = static_cast<float>(atlas_width_);
    const float atlas_h = static_cast<float>(atlas_height_);

    // Pixel coordinates (origin top-left) into clip space.
    auto to_ndc = [this](float px, float py) {
        return XMFLOAT2{px / static_cast<float>(width_) * 2.0f - 1.0f,
                        1.0f - py / static_cast<float>(height_) * 2.0f};
    };

    auto* vertices = reinterpret_cast<TextVertex*>(text_vertex_mapped_ +
                                                   static_cast<size_t>(frame_index_) *
                                                       kTextRegionBytes);
    UINT count = 0;
    for (const char c : text) {
        const Glyph* glyph = font_.Find(static_cast<unsigned char>(c));
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
    if (count == 0) {
        return;
    }

    command_list_->SetPipelineState(text_pipeline_state_.Get());
    command_list_->SetGraphicsRootSignature(text_root_signature_.Get());
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list_->SetGraphicsRootDescriptorTable(1, TextureHandle(atlas_descriptor_));

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = text_vertex_buffer_->GetGPUVirtualAddress() +
                         static_cast<UINT64>(frame_index_) * kTextRegionBytes;
    vbv.StrideInBytes = sizeof(TextVertex);
    vbv.SizeInBytes = count * sizeof(TextVertex);
    command_list_->IASetVertexBuffers(0, 1, &vbv);

    TextConstants constants{};
    constants.unit_range = {kDistanceRange / static_cast<float>(atlas_width_),
                            kDistanceRange / static_cast<float>(atlas_height_)};

    // A soft drop shadow first, nudged down and right, so the text reads over a
    // bright sky or a dark fence alike; then the text itself over the top.
    constants.color = {0.0f, 0.0f, 0.0f, 0.75f};
    constants.ndc_offset = {2.0f * kTextShadowPixels / static_cast<float>(width_),
                            -2.0f * kTextShadowPixels / static_cast<float>(height_)};
    command_list_->SetGraphicsRoot32BitConstants(0, kTextConstantDwords, &constants, 0);
    command_list_->DrawInstanced(count, 1, 0, 0);

    constants.color = {1.0f, 1.0f, 1.0f, 1.0f};
    constants.ndc_offset = {0.0f, 0.0f};
    command_list_->SetGraphicsRoot32BitConstants(0, kTextConstantDwords, &constants, 0);
    command_list_->DrawInstanced(count, 1, 0, 0);
}

void Renderer::DrawInstances(std::span<const MeshInstance> instances,
                             const XMMATRIX& view_projection, XMFLOAT3 sun_direction) {
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

            command_list_->SetGraphicsRoot32BitConstants(0, kConstantDwords, &constants, 0);
            command_list_->SetGraphicsRootDescriptorTable(1, primitive.base_color_texture);
            command_list_->DrawIndexedInstanced(primitive.index_count, 1, primitive.first_index, 0,
                                                0);
        }
    }
}

void Renderer::Render(const Scene& scene, std::span<const MeshInstance> props,
                      const ViewmodelPose& viewmodel, std::span<const MeshInstance> held_props,
                      const XMMATRIX& view_projection, std::string_view hud_prompt) {
    ID3D12CommandAllocator* allocator = allocators_[frame_index_].Get();
    ThrowIfFailed(allocator->Reset(), "CommandAllocator::Reset");
    ThrowIfFailed(command_list_->Reset(allocator, pipeline_state_.Get()), "CommandList::Reset");

    command_list_->SetGraphicsRootSignature(root_signature_.Get());

    // The only heap the game binds, and it must be bound before any root
    // descriptor table that points into it.
    ID3D12DescriptorHeap* heaps[] = {texture_heap_.Get()};
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

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

    XMFLOAT3 sun{};
    XMStoreFloat3(&sun, XMVector3Normalize(XMLoadFloat3(&kSunDirection)));
    DrawInstances(scene.Instances(), view_projection, sun);
    // The resting props take the yard's sun too: they are part of the world, and
    // only pass into the near pass once the player lifts them.
    DrawInstances(props, view_projection, sun);

    // The arms live about half a metre from the eye, close enough that any wall
    // the player leans against would be drawn in front of them. Throwing the
    // depth buffer away first is the usual answer: it costs one clear, and the
    // arms still occlude each other because they keep writing depth as they go.
    command_list_->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    DrawInstances(viewmodel.instances, view_projection, viewmodel.sun_direction);
    // A carried object rides in the same pass as the arms, under the same key
    // light bolted to the eye, so it is lit like something in the hand and never
    // clipped by the wall the player is facing.
    DrawInstances(held_props, view_projection, viewmodel.sun_direction);

    // The HUD goes on last, blended over the finished frame with depth off.
    DrawText(hud_prompt);

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
