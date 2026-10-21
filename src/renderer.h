#pragma once

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "dlss.h"
#include "flowerbox.h"
#include "settings.h"

struct ViewportRect {
    float x, y, size;
};

struct GpuInfo {
    bool found = false;
    bool rayTracing = false;
    bool dlss = false;
    std::wstring adapter;
    std::wstring dlssReason;  // why DLSS is not available
};

// Finds the graphics card the screen saver would use and what it supports.
GpuInfo QueryGpu();

class Renderer {
public:
    ~Renderer();

    // hwnd == nullptr renders offscreen (for SaveScreenshot). views gives the
    // floating squares; their sizes stay fixed until the next SetViews.
    bool Init(HWND hwnd, UINT width, UINT height, const Settings& settings, UINT boxVertexCount,
              const std::vector<ViewportRect>& views);
    bool Resize(UINT width, UINT height, const std::vector<ViewportRect>& views);
    // Records and submits one frame. Returns false if the device was lost.
    bool Render(FlowerBox& box, const std::vector<ViewportRect>& views);
    // Returns false if the device was lost; sets occluded when nothing is visible.
    bool Present(bool vsync, bool* occluded);
    bool SaveScreenshot(const wchar_t* path);

    bool RayTracingActive() const { return m_rayTracing; }
    bool DlssActive() const { return m_useDlss; }
    const std::wstring& AdapterName() const { return m_adapterName; }
    const std::wstring& Error() const { return m_error; }
    const std::wstring& DlssStatus() const { return m_dlssStatus; }
    // Warnings and errors from the D3D12 debug layer (FLOWERBOX_D3D_DEBUG set).
    std::wstring DebugMessages();

private:
    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct FrameResources {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12CommandAllocator> buildAllocator;
        ComPtr<ID3D12Resource> vertices;   // upload heap, persistently mapped
        ComPtr<ID3D12Resource> constants;  // upload heap
        ComPtr<ID3D12Resource> instances;  // upload heap
        ComPtr<ID3D12Resource> blas, blasScratch, tlas, tlasScratch;
        Vertex* vertexData = nullptr;
        void* constantData = nullptr;
        D3D12_RAYTRACING_INSTANCE_DESC* instanceData = nullptr;
        UINT64 fence = 0;
    };

    // One floating square rendered at DLSS render size and upscaled.
    struct DlssView {
        UINT outputSize = 0, renderSize = 0;
        ComPtr<ID3D12Resource> color, motion, depth, output;
        NVSDK_NGX_Handle* feature = nullptr;
        UINT jitterIndex = 0;
        bool reset = true;
    };

    bool Fail(const wchar_t* what, HRESULT hr = S_OK);
    bool CreateTargets();
    void ReleaseTargets();
    bool CreatePipelines();
    bool CreateRayTracingResources();
    bool CreateDlssViews(const std::vector<ViewportRect>& views);
    void ReleaseDlssViews();
    ComPtr<ID3D12Resource> CreateTexture(UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                         D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear, UINT samples = 1);
    bool WaitForGpu();
    bool WaitForFence(UINT64 value);
    bool ExecuteAndWait();
    ComPtr<ID3D12Resource> CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                                        D3D12_RESOURCE_STATES state);
    ID3D12Resource* CurrentTarget();
    void BindScene(FrameResources& f, ID3D12PipelineState* pipeline);

    static constexpr UINT kFrameCount = 2;
    static constexpr UINT kMaxViews = 16;

    Settings m_settings;
    HWND m_hwnd = nullptr;
    UINT m_width = 0, m_height = 0;
    UINT m_samples = 1;
    UINT m_boxVertices = 0;
    bool m_rayTracing = false;
    bool m_useDlss = false;
    UINT m_frameNumber = 0;
    float m_prevModel[4][4] = {};
    bool m_havePrevModel = false;
    std::wstring m_adapterName, m_error, m_dlssStatus;

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device5> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12GraphicsCommandList4> m_list;
    ComPtr<ID3D12GraphicsCommandList4> m_buildList;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Resource> m_backBuffers[kFrameCount];
    ComPtr<ID3D12Resource> m_offscreen;
    ComPtr<ID3D12Resource> m_msaaTarget, m_depth;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap, m_dsvHeap;
    UINT m_rtvStride = 0, m_dsvStride = 0;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipeline;    // MSAA path: colour only
    ComPtr<ID3D12PipelineState> m_pipelineMV;  // DLSS path: colour and motion vectors
    ComPtr<ID3D12Resource> m_floorVertices, m_floorBlas;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    FrameResources m_frames[kFrameCount];
    UINT m_frameIndex = 0;
    UINT m_lastFrame = 0;

    Dlss m_dlss;
    std::vector<DlssView> m_views;
};
