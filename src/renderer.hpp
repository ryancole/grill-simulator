#pragma once

#include "dx_common.hpp"
#include "font.hpp"
#include "scene.hpp"
#include "viewmodel.hpp"

#include <DirectXMath.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <chrono>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// Raw Direct3D 12. No abstraction layer, no engine: the swapchain, the command
// allocators and the fence are all right here on purpose.
class Renderer {
public:
    static constexpr UINT kFrameCount = 2;

    void Initialize(HWND hwnd, UINT width, UINT height, const Scene& scene);
    void Resize(UINT width, UINT height);
    // `props` are the loose objects resting in the yard, drawn with the scene.
    // `highlight` is the one the player is aiming at, ringed with a glowing
    // outline; empty rings nothing. `viewmodel` and `held_props` are drawn last,
    // over a cleared depth buffer, so the player's arms and whatever they carry
    // are never sliced open by the wall they are standing against. `hud_prompt`
    // is the one line of HUD text laid over the finished frame; empty draws
    // nothing.
    void Render(const Scene& scene, std::span<const MeshInstance> props,
                std::span<const MeshInstance> highlight, const ViewmodelPose& viewmodel,
                std::span<const MeshInstance> held_props,
                const DirectX::XMMATRIX& view_projection, std::string_view hud_prompt);
    void Shutdown();

    float AspectRatio() const;

private:
    // One draw: a slice of its model's index buffer, the base colour the glTF
    // material asked for, and the descriptor of the texture that modulates it.
    struct DrawPrimitive {
        DirectX::XMFLOAT4X4 transform;
        UINT first_index;
        UINT index_count;
        DirectX::XMFLOAT3 base_color;
        D3D12_GPU_DESCRIPTOR_HANDLE base_color_texture;
    };

    // A Model, uploaded. The CPU-side Model that produced it is not needed again.
    struct GpuModel {
        ComPtr<ID3D12Resource> vertex_buffer;
        D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view{};
        ComPtr<ID3D12Resource> index_buffer;
        D3D12_INDEX_BUFFER_VIEW index_buffer_view{};
        std::vector<DrawPrimitive> primitives;
    };

    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, UINT width, UINT height);
    void CreateRenderTargetViews();
    void CreateDepthBuffer();
    void CreatePipeline();
    // The inverted-hull outline pass: its own root signature and PSO that grow a
    // mesh along its normals, cull the near faces and paint the far shell a flat
    // glowing colour. Depth-tests against the world but writes no depth.
    void CreateOutlinePipeline();
    // Uploads every model the scene holds, plus the 1x1 white texture that
    // stands in for a material with no texture of its own.
    void CreateSceneGeometry(const Scene& scene);

    // The second, tiny pipeline: the alpha-blended, depth-free pass that draws
    // HUD text from the MSDF atlas. Builds its root signature, PSO and the
    // per-frame dynamic vertex buffer the glyph quads are written into.
    void CreateTextPipeline();
    // Loads the font's glyph metrics and uploads its atlas into the shared
    // texture heap. Shares the scene upload's open command list and staging list.
    void LoadFontAtlas(std::vector<ComPtr<ID3D12Resource>>& staging);
    // Lays `text` out into a centred line near the bottom of the screen and draws
    // it -- a dark shadow pass, then the text -- into the current frame. A no-op
    // for empty text. Assumes the render target is already bound.
    void DrawText(std::string_view text);

    // Fills a default-heap buffer through a staging copy. The staging resource is
    // appended to `staging`, which the caller must keep alive until the GPU has
    // retired the copy.
    ComPtr<ID3D12Resource> UploadBuffer(const void* data, UINT64 bytes,
                                        D3D12_RESOURCE_STATES final_state,
                                        std::vector<ComPtr<ID3D12Resource>>& staging);
    // Uploads a mip chain and writes its shader resource view into slot
    // `descriptor` of the texture heap.
    ComPtr<ID3D12Resource> UploadTexture(const Image& image, UINT descriptor,
                                         std::vector<ComPtr<ID3D12Resource>>& staging);
    D3D12_GPU_DESCRIPTOR_HANDLE TextureHandle(UINT descriptor) const;

    // One draw per primitive of each instance's model, each under its own root
    // constants.
    void DrawInstances(std::span<const MeshInstance> instances,
                       const DirectX::XMMATRIX& view_projection, DirectX::XMFLOAT3 sun_direction);

    // Paints the glowing halo around each instance, with depth off so it wraps
    // every side evenly; `seconds` drives the pulse. The caller re-paints the
    // instance afterward to carve it back out of the halo. Assumes the render
    // target is still bound.
    void DrawOutlines(std::span<const MeshInstance> instances,
                      const DirectX::XMMATRIX& view_projection, float seconds);

    // Blocks until the GPU has retired every frame. Only for teardown, resize
    // and the one-off geometry upload; the steady-state path is MoveToNextFrame.
    void FlushGpu();
    // Signals the current frame and waits only if the frame we are about to
    // reuse is still in flight.
    void MoveToNextFrame();

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;

    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_size_ = 0;
    ComPtr<ID3D12Resource> render_targets_[kFrameCount];

    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12Resource> depth_stencil_;

    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> command_list_;

    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_state_;

    // The pick-up outline pass. Shares the scene's vertex buffers and input
    // layout; only the root signature and PSO differ.
    ComPtr<ID3D12RootSignature> outline_root_signature_;
    ComPtr<ID3D12PipelineState> outline_pipeline_state_;

    // The HUD text pass. The atlas SRV lives in the shared texture_heap_ at
    // atlas_descriptor_; the vertex buffer is one upload-heap region per frame in
    // flight, kept mapped, rewritten with the frame's glyph quads.
    ComPtr<ID3D12RootSignature> text_root_signature_;
    ComPtr<ID3D12PipelineState> text_pipeline_state_;
    ComPtr<ID3D12Resource> atlas_texture_;
    UINT atlas_descriptor_ = 0;
    UINT atlas_width_ = 0;
    UINT atlas_height_ = 0;
    Font font_;
    ComPtr<ID3D12Resource> text_vertex_buffer_;
    std::byte* text_vertex_mapped_ = nullptr;

    // Shader-visible, and the only descriptor heap the game binds. Slot 0 is the
    // white texture; every glTF image follows it.
    ComPtr<ID3D12DescriptorHeap> texture_heap_;
    UINT texture_size_ = 0;
    std::vector<ComPtr<ID3D12Resource>> textures_;

    std::vector<GpuModel> models_;

    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_values_[kFrameCount]{};
    UINT frame_index_ = 0;

    // When the renderer came up, so the outline's glow can pulse against a
    // wall-clock rather than needing a delta threaded through Render.
    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    UINT width_ = 0;
    UINT height_ = 0;
    bool allow_tearing_ = false;
    bool initialized_ = false;
};
