#pragma once

#include "dx_common.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>

// Raw Direct3D 12. No abstraction layer, no engine: the swapchain, the command
// allocators and the fence are all right here on purpose.
class Renderer {
public:
    static constexpr UINT kFrameCount = 2;

    void Initialize(HWND hwnd, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Render();
    void Shutdown();

private:
    void CreateDevice();
    void CreateCommandObjects();
    void CreateSwapChain(HWND hwnd, UINT width, UINT height);
    void CreateRenderTargetViews();
    void CreatePipeline();
    void CreateTriangle();

    // Blocks until the GPU has retired every frame. Only for teardown and
    // resize; the steady-state path is MoveToNextFrame.
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

    ComPtr<ID3D12CommandAllocator> allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> command_list_;

    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> pipeline_state_;

    ComPtr<ID3D12Resource> vertex_buffer_;
    D3D12_VERTEX_BUFFER_VIEW vertex_buffer_view_{};

    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    UINT64 fence_values_[kFrameCount]{};
    UINT frame_index_ = 0;

    D3D12_VIEWPORT viewport_{};
    D3D12_RECT scissor_{};
    UINT width_ = 0;
    UINT height_ = 0;
    bool allow_tearing_ = false;
    bool initialized_ = false;
};
