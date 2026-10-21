#include "renderer.h"

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

#include "shader_ps.h"
#include "shader_psmv.h"
#include "shader_psrt.h"
#include "shader_psrtmv.h"
#include "shader_vs.h"

namespace {

constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kMotionFormat = DXGI_FORMAT_R16G16_FLOAT;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32_TYPELESS;
constexpr UINT kFloorVertexCount = 6;
constexpr float kFloorY = -1.15f;
constexpr float kFloorExtent = 15.0f;
constexpr UINT kConstantSlot = 512;

// Flags shared with the shader.
constexpr UINT kFlagTwoSided = 1, kFlagShadows = 2, kFlagAO = 4, kFlagDebug = 8, kFlagTemporal = 16;

struct FrameConstants {
    float viewProj[4][4];
    float model[4][4];
    float prevModel[4][4];
    float eyePos[3];
    float lightRadius;
    float lightPos[3];
    float reflectivity;
    float floorY;
    float floorFade;
    UINT flags;
    UINT samples;
    UINT bounces;
    float aoRadius;
    float floorReflectivity;
    UINT frameIndex;
};
static_assert(sizeof(FrameConstants) <= kConstantSlot, "constants must fit their slot");

struct ViewConstants {
    float jitterNdc[2];
    float viewOrigin[2];
    float viewSize;
    float pad[3];
};
static_assert(sizeof(ViewConstants) == 32, "view constants are eight root constants");

D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p = {};
    p.Type = type;
    p.CreationNodeMask = p.VisibleNodeMask = 1;
    return p;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = r;
    return b;
}

UINT64 Align(UINT64 v, UINT64 a) { return (v + a - 1) & ~(a - 1); }

bool SupportsRayQuery(ID3D12Device5* device) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5)))) return false;
    if (o5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) return false;
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_5};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) return false;
    return sm.HighestShaderModel >= D3D_SHADER_MODEL_6_5;
}

bool SupportsShaderModel6(ID3D12Device5* device) {
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {D3D_SHADER_MODEL_6_0};
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) &&
           sm.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
}

struct DeviceChoice {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    std::wstring name;
    bool rayTracing = false;
};

// Picks the high-performance hardware adapter, preferring one that can ray
// trace; falls back to WARP so the saver still runs without a D3D12 GPU.
bool CreateBestDevice(DeviceChoice& out) {
    using Microsoft::WRL::ComPtr;
    UINT factoryFlags = 0;
    wchar_t debugVar[8] = {};
    if (GetEnvironmentVariableW(L"FLOWERBOX_D3D_DEBUG", debugVar, 8)) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
            ComPtr<ID3D12Debug1> debug1;
            if (debugVar[0] == L'2' && SUCCEEDED(debug.As(&debug1))) debug1->SetEnableGPUBasedValidation(TRUE);
        }
    }
    if (FAILED(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&out.factory)))) return false;

    DeviceChoice fallback;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; out.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        ComPtr<ID3D12Device5> device;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) continue;
        if (!SupportsShaderModel6(device.Get())) continue;
        if (SupportsRayQuery(device.Get())) {
            out.device = device;
            out.name = desc.Description;
            out.rayTracing = true;
            return true;
        }
        if (!fallback.device) {
            fallback.device = device;
            fallback.name = desc.Description;
        }
    }
    if (fallback.device) {
        out.device = fallback.device;
        out.name = fallback.name;
        out.rayTracing = false;
        return true;
    }
    ComPtr<IDXGIAdapter> warp;
    if (SUCCEEDED(out.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))) &&
        SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&out.device))) &&
        SupportsShaderModel6(out.device.Get())) {
        out.name = L"Microsoft Basic Render Driver";
        out.rayTracing = SupportsRayQuery(out.device.Get());
        return true;
    }
    return false;
}

void Multiply(const float a[4][4], const float b[4][4], float out[4][4]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i][k] * b[k][j];
            out[i][j] = s;
        }
}

// The original's camera: gluLookAt from (0, 0, 3.5) towards the origin.
// With the floor shown the camera rises and looks down a little, otherwise
// the box's mirror image would sit below the bottom of the square.
const float kEye[3] = {0, 0, 3.5f}, kTarget[3] = {0, 0, 0};
const float kFloorEye[3] = {0, 1.6f, 3.3f}, kFloorTarget[3] = {0, -0.6f, 0};

// gluPerspective(45, 1, near, far) for D3D depth times gluLookAt(eye, target,
// up = y). The original's far plane of 5 is pushed out so the floor and
// reflections are not clipped.
void ViewProjection(const float eye[3], const float target[3], float out[4][4]) {
    const float zn = 0.5f, zf = 40.0f;
    const float f = 1.0f / std::tan(22.5f * 3.14159265f / 180.0f);
    const float proj[4][4] = {
        {f, 0, 0, 0},
        {0, f, 0, 0},
        {0, 0, zf / (zn - zf), zn * zf / (zn - zf)},
        {0, 0, -1, 0},
    };
    float fw[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float fl = std::sqrt(fw[0] * fw[0] + fw[1] * fw[1] + fw[2] * fw[2]);
    for (float& v : fw) v /= fl;
    // side = forward x up(0, 1, 0), then up = side x forward
    float s[3] = {-fw[2], 0, fw[0]};
    const float sl = std::sqrt(s[0] * s[0] + s[2] * s[2]);
    s[0] /= sl;
    s[2] /= sl;
    const float u[3] = {s[1] * fw[2] - s[2] * fw[1], s[2] * fw[0] - s[0] * fw[2], s[0] * fw[1] - s[1] * fw[0]};
    auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float view[4][4] = {
        {s[0], s[1], s[2], -dot(s, eye)},
        {u[0], u[1], u[2], -dot(u, eye)},
        {-fw[0], -fw[1], -fw[2], dot(fw, eye)},
        {0, 0, 0, 1},
    };
    Multiply(proj, view, out);
}

void FloorVertices(Vertex out[kFloorVertexCount]) {
    const float e = kFloorExtent, y = kFloorY;
    const float corners[kFloorVertexCount][2] = {{-e, -e}, {-e, e}, {e, e}, {-e, -e}, {e, e}, {e, -e}};
    for (UINT i = 0; i < kFloorVertexCount; ++i)
        out[i] = {{corners[i][0], y, corners[i][1]},
                  {0, 1, 0},
                  {0, 1, 0},
                  {0.2f, 0.2f, 0.2f, 1.0f},
                  {corners[i][0], y, corners[i][1]}};
}

float Halton(UINT index, UINT base) {
    float f = 1, r = 0;
    for (; index > 0; index /= base) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
    }
    return r;
}

}  // namespace

GpuInfo QueryGpu() {
    GpuInfo info;
    DeviceChoice choice;
    if (!CreateBestDevice(choice)) return info;
    info.found = true;
    info.rayTracing = choice.rayTracing;
    info.adapter = choice.name;
    Dlss dlss;
    info.dlss = dlss.Init(choice.device.Get());
    if (!info.dlss) info.dlssReason = dlss.Error();
    return info;
}

Renderer::~Renderer() {
    if (m_device) WaitForGpu();
    ReleaseDlssViews();
    m_dlss.Shutdown();
    for (FrameResources& f : m_frames) {
        if (f.vertices && f.vertexData) f.vertices->Unmap(0, nullptr);
        if (f.constants && f.constantData) f.constants->Unmap(0, nullptr);
        if (f.instances && f.instanceData) f.instances->Unmap(0, nullptr);
    }
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool Renderer::Fail(const wchar_t* what, HRESULT hr) {
    wchar_t buf[256];
    swprintf_s(buf, L"%s (0x%08X)", what, static_cast<unsigned>(hr));
    m_error = buf;
    return false;
}

Renderer::ComPtr<ID3D12Resource> Renderer::CreateBuffer(UINT64 size, D3D12_HEAP_TYPE heap,
                                                        D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<UINT64>(size, 256);
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp = HeapProps(heap);
    ComPtr<ID3D12Resource> r;
    if (FAILED(m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

Renderer::ComPtr<ID3D12Resource> Renderer::CreateTexture(UINT width, UINT height, DXGI_FORMAT format,
                                                         D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                                         const D3D12_CLEAR_VALUE* clear, UINT samples) {
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = format;
    td.SampleDesc.Count = samples;
    td.Flags = flags;
    D3D12_HEAP_PROPERTIES def = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> r;
    if (FAILED(m_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, state, clear, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

bool Renderer::Init(HWND hwnd, UINT width, UINT height, const Settings& settings, UINT boxVertexCount,
                    const std::vector<ViewportRect>& views) {
    m_settings = settings;
    m_hwnd = hwnd;
    m_width = std::max(width, 1u);
    m_height = std::max(height, 1u);
    m_boxVertices = boxVertexCount;

    DeviceChoice choice;
    if (!CreateBestDevice(choice)) return Fail(L"No Direct3D 12 graphics device is available");
    m_factory = choice.factory;
    m_device = choice.device;
    m_adapterName = choice.name;
    m_rayTracing = settings.rayTracing && choice.rayTracing;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = m_device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_queue));
    if (FAILED(hr)) return Fail(L"CreateCommandQueue failed", hr);
    for (FrameResources& f : m_frames) {
        hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator));
        if (SUCCEEDED(hr))
            hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.buildAllocator));
        if (FAILED(hr)) return Fail(L"CreateCommandAllocator failed", hr);
    }
    ComPtr<ID3D12GraphicsCommandList4>* lists[] = {&m_list, &m_buildList};
    for (ComPtr<ID3D12GraphicsCommandList4>* list : lists) {
        hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr,
                                         IID_PPV_ARGS(list->ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return Fail(L"CreateCommandList failed", hr);
        (*list)->Close();
    }
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) return Fail(L"CreateFence failed", hr);
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // DLSS is built in and always used when the card supports it.
    // (FLOWERBOX_NO_DLSS turns it off, for comparisons while testing.)
    m_useDlss = !GetEnvironmentVariableW(L"FLOWERBOX_NO_DLSS", nullptr, 0) && m_dlss.Init(m_device.Get());
    m_dlssStatus = m_useDlss ? L"DLSS on" : L"DLSS off: " + m_dlss.Error();

    // Without DLSS: the highest supported MSAA count not above the setting.
    m_samples = 1;
    for (UINT count = m_useDlss ? 1u : static_cast<UINT>(settings.msaa); count > 1; count /= 2) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ql = {kColorFormat, count};
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS dl = {kDepthFormat, count};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &ql, sizeof(ql))) &&
            SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &dl, sizeof(dl))) &&
            ql.NumQualityLevels > 0 && dl.NumQualityLevels > 0) {
            m_samples = count;
            break;
        }
    }

    D3D12_DESCRIPTOR_HEAP_DESC rh = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kFrameCount + 1 + 2 * kMaxViews};
    hr = m_device->CreateDescriptorHeap(&rh, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) return Fail(L"CreateDescriptorHeap failed", hr);
    m_rtvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_DESCRIPTOR_HEAP_DESC dh = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1 + kMaxViews};
    hr = m_device->CreateDescriptorHeap(&dh, IID_PPV_ARGS(&m_dsvHeap));
    if (FAILED(hr)) return Fail(L"CreateDescriptorHeap failed", hr);
    m_dsvStride = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

    if (m_hwnd) {
        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width = m_width;
        sd.Height = m_height;
        sd.Format = kColorFormat;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kFrameCount;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        ComPtr<IDXGISwapChain1> sc;
        hr = m_factory->CreateSwapChainForHwnd(m_queue.Get(), m_hwnd, &sd, nullptr, nullptr, &sc);
        if (FAILED(hr)) return Fail(L"CreateSwapChainForHwnd failed", hr);
        m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        hr = sc.As(&m_swapChain);
        if (FAILED(hr)) return Fail(L"IDXGISwapChain3 is not available", hr);
    }
    if (!CreateTargets()) return false;
    if (!CreatePipelines()) return false;

    const UINT64 vbSize = static_cast<UINT64>(m_boxVertices + kFloorVertexCount) * sizeof(Vertex);
    for (FrameResources& f : m_frames) {
        f.vertices = CreateBuffer(vbSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                  D3D12_RESOURCE_STATE_GENERIC_READ);
        f.constants = CreateBuffer(kConstantSlot, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                                   D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!f.vertices || !f.constants) return Fail(L"Could not allocate frame buffers");
        D3D12_RANGE none = {0, 0};
        f.vertices->Map(0, &none, reinterpret_cast<void**>(&f.vertexData));
        f.constants->Map(0, &none, &f.constantData);
        FloorVertices(f.vertexData + m_boxVertices);
    }

    if (m_rayTracing && !CreateRayTracingResources()) {
        // Ray tracing is optional: carry on with the classic renderer.
        m_rayTracing = false;
        m_error = L"ray tracing unavailable: " + m_error;
        if (!CreatePipelines()) return false;
    }
    if (m_useDlss && !CreateDlssViews(views)) {
        ReleaseDlssViews();
        m_useDlss = false;
        m_dlssStatus = L"DLSS off: " + m_error;
        m_error.clear();
    }
    return true;
}

bool Renderer::CreateTargets() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (m_swapChain) {
        for (UINT i = 0; i < kFrameCount; ++i) {
            HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
            if (FAILED(hr)) return Fail(L"GetBuffer failed", hr);
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, {rtv.ptr + i * m_rtvStride});
        }
    } else {
        D3D12_CLEAR_VALUE black = {kColorFormat, {0, 0, 0, 1}};
        m_offscreen = CreateTexture(m_width, m_height, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                    D3D12_RESOURCE_STATE_COPY_SOURCE, &black);
        if (!m_offscreen) return Fail(L"Could not create the offscreen target");
        m_device->CreateRenderTargetView(m_offscreen.Get(), nullptr, rtv);
    }
    if (m_useDlss) return true;  // each square has its own targets

    if (m_samples > 1) {
        D3D12_CLEAR_VALUE black = {kColorFormat, {0, 0, 0, 1}};
        m_msaaTarget = CreateTexture(m_width, m_height, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_RENDER_TARGET, &black, m_samples);
        if (!m_msaaTarget) return Fail(L"Could not create the multisample target");
        m_device->CreateRenderTargetView(m_msaaTarget.Get(), nullptr, {rtv.ptr + kFrameCount * m_rtvStride});
    }
    D3D12_CLEAR_VALUE depthClear = {};
    depthClear.Format = kDepthFormat;
    depthClear.DepthStencil.Depth = 1.0f;
    m_depth = CreateTexture(m_width, m_height, kDepthFormat,
                            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear, m_samples);
    if (!m_depth) return Fail(L"Could not create the depth buffer");
    m_device->CreateDepthStencilView(m_depth.Get(), nullptr, m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
    return true;
}

void Renderer::ReleaseTargets() {
    for (auto& b : m_backBuffers) b.Reset();
    m_offscreen.Reset();
    m_msaaTarget.Reset();
    m_depth.Reset();
}

bool Renderer::ExecuteAndWait() {
    if (FAILED(m_list->Close())) return false;
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    return WaitForGpu();
}

bool Renderer::CreateDlssViews(const std::vector<ViewportRect>& views) {
    if (views.empty() || views.size() > kMaxViews) return Fail(L"too many screens for DLSS");
    m_views.assign(views.size(), DlssView{});
    if (!WaitForGpu()) return false;
    m_frames[0].allocator->Reset();
    m_list->Reset(m_frames[0].allocator.Get(), nullptr);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvStart = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    for (size_t i = 0; i < views.size(); ++i) {
        DlssView& v = m_views[i];
        v.outputSize = std::max(1u, static_cast<UINT>(views[i].size + 0.5f));
        if (!m_dlss.RenderSize(v.outputSize, &v.renderSize)) return Fail(L"DLSS has no render size for this screen");
        const D3D12_CLEAR_VALUE black = {kColorFormat, {0, 0, 0, 1}};
        const D3D12_CLEAR_VALUE still = {kMotionFormat, {0, 0, 0, 0}};
        D3D12_CLEAR_VALUE depthClear = {};
        depthClear.Format = kDepthFormat;
        depthClear.DepthStencil.Depth = 1.0f;
        v.color = CreateTexture(v.renderSize, v.renderSize, kColorFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                D3D12_RESOURCE_STATE_RENDER_TARGET, &black);
        v.motion = CreateTexture(v.renderSize, v.renderSize, kMotionFormat, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                 D3D12_RESOURCE_STATE_RENDER_TARGET, &still);
        v.depth = CreateTexture(v.renderSize, v.renderSize, kDepthTypeless, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear);
        v.output = CreateTexture(v.outputSize, v.outputSize, kColorFormat,
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                 nullptr);
        if (!v.color || !v.motion || !v.depth || !v.output) return Fail(L"could not allocate DLSS targets");
        m_device->CreateRenderTargetView(v.color.Get(), nullptr,
                                         {rtvStart.ptr + (kFrameCount + 1 + 2 * i) * m_rtvStride});
        m_device->CreateRenderTargetView(v.motion.Get(), nullptr,
                                         {rtvStart.ptr + (kFrameCount + 2 + 2 * i) * m_rtvStride});
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = kDepthFormat;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(v.depth.Get(), &dsv, {dsvStart.ptr + (1 + i) * m_dsvStride});
        v.feature = m_dlss.CreateFeature(m_list.Get(), v.renderSize, v.outputSize);
        if (!v.feature) {
            m_list->Close();
            return Fail(L"DLSS could not start for this screen size");
        }
    }
    return ExecuteAndWait();
}

void Renderer::ReleaseDlssViews() {
    for (DlssView& v : m_views) m_dlss.ReleaseFeature(v.feature);
    m_views.clear();
}

bool Renderer::Resize(UINT width, UINT height, const std::vector<ViewportRect>& views) {
    width = std::max(width, 1u);
    height = std::max(height, 1u);
    if (!WaitForGpu()) return false;
    if (width != m_width || height != m_height) {
        ReleaseTargets();
        m_width = width;
        m_height = height;
        if (m_swapChain) {
            HRESULT hr = m_swapChain->ResizeBuffers(kFrameCount, m_width, m_height, kColorFormat, 0);
            if (FAILED(hr)) return Fail(L"ResizeBuffers failed", hr);
        }
        if (!CreateTargets()) return false;
    }
    if (m_useDlss) {
        ReleaseDlssViews();
        if (!CreateDlssViews(views)) {
            // Too small (or too many squares) for DLSS: switch to MSAA for good.
            ReleaseDlssViews();
            m_useDlss = false;
            m_dlssStatus = L"DLSS off: " + m_error;
            m_error.clear();
            ReleaseTargets();
            m_samples = 1;
            if (!CreateTargets() || !CreatePipelines()) return false;
        }
    }
    return true;
}

bool Renderer::CreatePipelines() {
    // Root signature: b0 frame constants, b1 view constants; with ray
    // tracing also t0 (scene) and t1 (box vertices).
    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 1;
    params[1].Constants.Num32BitValues = sizeof(ViewConstants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[2].Descriptor.ShaderRegister = 0;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[3].Descriptor.ShaderRegister = 1;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = m_rayTracing ? 4 : 2;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr)) return Fail(L"D3D12SerializeRootSignature failed", hr);
    hr = m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                       IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) return Fail(L"CreateRootSignature failed", hr);

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"FACENORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"PREVPOSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 52, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = m_rootSignature.Get();
    pd.VS = {g_VS, sizeof(g_VS)};
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.RenderTarget[1].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;  // facing is decided in the pixel shader
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.InputLayout = {layout, _countof(layout)};
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.DSVFormat = kDepthFormat;

    // MSAA path: colour only.
    pd.PS = m_rayTracing ? D3D12_SHADER_BYTECODE{g_PSRT, sizeof(g_PSRT)} : D3D12_SHADER_BYTECODE{g_PS, sizeof(g_PS)};
    pd.RasterizerState.MultisampleEnable = m_samples > 1;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = kColorFormat;
    pd.SampleDesc.Count = m_samples;
    hr = m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pipeline));
    if (FAILED(hr)) return Fail(L"CreateGraphicsPipelineState failed", hr);

    // DLSS path: colour and motion vectors, no MSAA.
    pd.PS = m_rayTracing ? D3D12_SHADER_BYTECODE{g_PSRTMV, sizeof(g_PSRTMV)}
                         : D3D12_SHADER_BYTECODE{g_PSMV, sizeof(g_PSMV)};
    pd.RasterizerState.MultisampleEnable = FALSE;
    pd.NumRenderTargets = 2;
    pd.RTVFormats[1] = kMotionFormat;
    pd.SampleDesc.Count = 1;
    hr = m_device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_pipelineMV));
    if (FAILED(hr)) return Fail(L"CreateGraphicsPipelineState failed", hr);
    return true;
}

bool Renderer::CreateRayTracingResources() {
    // Static floor BLAS.
    m_floorVertices = CreateBuffer(kFloorVertexCount * sizeof(Vertex), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!m_floorVertices) return Fail(L"Could not allocate the floor");
    Vertex* fv = nullptr;
    D3D12_RANGE none = {0, 0};
    m_floorVertices->Map(0, &none, reinterpret_cast<void**>(&fv));
    FloorVertices(fv);
    m_floorVertices->Unmap(0, nullptr);

    auto geometry = [](D3D12_GPU_VIRTUAL_ADDRESS vb, UINT count, bool opaque) {
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexBuffer.StartAddress = vb;
        g.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = count;
        return g;
    };
    auto inputs = [](const D3D12_RAYTRACING_GEOMETRY_DESC* g) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in = {};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.NumDescs = 1;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.pGeometryDescs = g;
        return in;
    };
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto asState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;

    D3D12_RAYTRACING_GEOMETRY_DESC floorGeom = geometry(m_floorVertices->GetGPUVirtualAddress(), kFloorVertexCount, true);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS floorIn = inputs(&floorGeom);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO pre = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&floorIn, &pre);
    m_floorBlas = CreateBuffer(Align(pre.ResultDataMaxSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT, uav, asState);
    ComPtr<ID3D12Resource> floorScratch = CreateBuffer(Align(pre.ScratchDataSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT,
                                                       uav, D3D12_RESOURCE_STATE_COMMON);
    if (!m_floorBlas || !floorScratch) return Fail(L"Could not allocate the floor acceleration structure");

    // Per-frame box BLAS and TLAS, sized for the full mesh.
    D3D12_RAYTRACING_GEOMETRY_DESC boxGeom =
        geometry(m_frames[0].vertices->GetGPUVirtualAddress(), m_boxVertices, m_settings.twoSided);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS boxIn = inputs(&boxGeom);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO boxPre = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&boxIn, &boxPre);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS topIn = {};
    topIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    topIn.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
    topIn.NumDescs = 2;
    topIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topPre = {};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&topIn, &topPre);

    for (FrameResources& f : m_frames) {
        f.blas = CreateBuffer(Align(boxPre.ResultDataMaxSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT, uav, asState);
        f.blasScratch = CreateBuffer(Align(boxPre.ScratchDataSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT, uav,
                                     D3D12_RESOURCE_STATE_COMMON);
        f.tlas = CreateBuffer(Align(topPre.ResultDataMaxSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT, uav, asState);
        f.tlasScratch = CreateBuffer(Align(topPre.ScratchDataSizeInBytes, 256), D3D12_HEAP_TYPE_DEFAULT, uav,
                                     D3D12_RESOURCE_STATE_COMMON);
        f.instances = CreateBuffer(2 * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD,
                                   D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!f.blas || !f.blasScratch || !f.tlas || !f.tlasScratch || !f.instances)
            return Fail(L"Could not allocate acceleration structures");
        f.instances->Map(0, &none, reinterpret_cast<void**>(&f.instanceData));
    }

    // Build the floor once.
    m_frames[0].allocator->Reset();
    m_list->Reset(m_frames[0].allocator.Get(), nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {};
    build.Inputs = floorIn;
    build.DestAccelerationStructureData = m_floorBlas->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = floorScratch->GetGPUVirtualAddress();
    m_list->BuildRaytracingAccelerationStructure(&build, 0, nullptr);
    D3D12_RESOURCE_BARRIER b = UavBarrier(m_floorBlas.Get());
    m_list->ResourceBarrier(1, &b);
    return ExecuteAndWait();
}

bool Renderer::WaitForFence(UINT64 value) {
    if (m_fence->GetCompletedValue() >= value) return true;
    if (FAILED(m_fence->SetEventOnCompletion(value, m_fenceEvent))) return false;
    WaitForSingleObject(m_fenceEvent, INFINITE);
    return true;
}

bool Renderer::WaitForGpu() {
    if (!m_queue || !m_fence) return false;
    if (FAILED(m_queue->Signal(m_fence.Get(), ++m_fenceValue))) return false;
    return WaitForFence(m_fenceValue);
}

ID3D12Resource* Renderer::CurrentTarget() {
    return m_swapChain ? m_backBuffers[m_swapChain->GetCurrentBackBufferIndex()].Get() : m_offscreen.Get();
}

void Renderer::BindScene(FrameResources& f, ID3D12PipelineState* pipeline) {
    m_list->SetPipelineState(pipeline);
    m_list->SetGraphicsRootSignature(m_rootSignature.Get());
    m_list->SetGraphicsRootConstantBufferView(0, f.constants->GetGPUVirtualAddress());
    const D3D12_GPU_VIRTUAL_ADDRESS vb = f.vertices->GetGPUVirtualAddress();
    if (m_rayTracing) {
        m_list->SetGraphicsRootShaderResourceView(2, f.tlas->GetGPUVirtualAddress());
        m_list->SetGraphicsRootShaderResourceView(3, vb);
    }
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = {vb, static_cast<UINT>((m_boxVertices + kFloorVertexCount) * sizeof(Vertex)),
                                    sizeof(Vertex)};
    m_list->IASetVertexBuffers(0, 1, &vbv);
}

bool Renderer::Render(FlowerBox& box, const std::vector<ViewportRect>& views) {
    FrameResources& f = m_frames[m_frameIndex];
    if (!WaitForFence(f.fence)) return false;

    box.Build(f.vertexData);

    FrameConstants c = {};
    const float* eye = m_settings.floor ? kFloorEye : kEye;
    ViewProjection(eye, m_settings.floor ? kFloorTarget : kTarget, c.viewProj);
    box.ModelMatrix(c.model);
    std::memcpy(c.prevModel, m_havePrevModel ? m_prevModel : c.model, sizeof(c.prevModel));
    std::memcpy(m_prevModel, c.model, sizeof(m_prevModel));
    m_havePrevModel = true;
    std::memcpy(c.eyePos, eye, sizeof(c.eyePos));
    c.lightPos[0] = 2;
    c.lightPos[1] = 2;
    c.lightPos[2] = 10;
    c.lightRadius = 0.6f;
    c.reflectivity = m_settings.rtReflectivity / 100.0f;
    c.floorReflectivity = 0.5f;
    c.floorY = kFloorY;
    c.floorFade = 3.5f;
    c.flags = (m_settings.twoSided ? kFlagTwoSided : 0) | (m_settings.rtShadows ? kFlagShadows : 0) |
              (m_settings.rtAmbientOcclusion ? kFlagAO : 0) | (m_useDlss ? kFlagTemporal : 0);
    if (GetEnvironmentVariableW(L"FLOWERBOX_RT_VIEW", nullptr, 0)) c.flags |= kFlagDebug;
    c.samples = static_cast<UINT>(m_settings.rtSamples);
    c.bounces = static_cast<UINT>(m_settings.rtBounces);
    c.aoRadius = 0.35f;
    c.frameIndex = m_frameNumber++;
    std::memcpy(f.constantData, &c, sizeof(c));

    if (m_rayTracing) {
        // The acceleration structures are built in their own submission: a
        // pixel shader ray query in the same command list as the build saw
        // an empty scene on the NVIDIA driver, barriers notwithstanding.
        if (FAILED(f.buildAllocator->Reset())) return false;
        if (FAILED(m_buildList->Reset(f.buildAllocator.Get(), nullptr))) return false;
        D3D12_RAYTRACING_GEOMETRY_DESC g = {};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = m_settings.twoSided ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
        g.Triangles.VertexBuffer.StartAddress = f.vertices->GetGPUVirtualAddress();
        g.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = m_boxVertices;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottom = {};
        bottom.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottom.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        bottom.Inputs.NumDescs = 1;
        bottom.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottom.Inputs.pGeometryDescs = &g;
        bottom.DestAccelerationStructureData = f.blas->GetGPUVirtualAddress();
        bottom.ScratchAccelerationStructureData = f.blasScratch->GetGPUVirtualAddress();
        m_buildList->BuildRaytracingAccelerationStructure(&bottom, 0, nullptr);
        D3D12_RESOURCE_BARRIER ub = UavBarrier(f.blas.Get());
        m_buildList->ResourceBarrier(1, &ub);

        UINT instanceCount = 0;
        D3D12_RAYTRACING_INSTANCE_DESC boxInst = {};
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 4; ++col) boxInst.Transform[r][col] = c.model[r][col];
        boxInst.InstanceID = 0;
        boxInst.InstanceMask = 0xFF;
        boxInst.AccelerationStructure = f.blas->GetGPUVirtualAddress();
        f.instanceData[instanceCount++] = boxInst;
        if (m_settings.floor) {
            D3D12_RAYTRACING_INSTANCE_DESC floorInst = {};
            floorInst.Transform[0][0] = floorInst.Transform[1][1] = floorInst.Transform[2][2] = 1.0f;
            floorInst.InstanceID = 1;
            floorInst.InstanceMask = 0xFF;
            floorInst.AccelerationStructure = m_floorBlas->GetGPUVirtualAddress();
            f.instanceData[instanceCount++] = floorInst;
        }

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC top = {};
        top.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        top.Inputs.NumDescs = instanceCount;
        top.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.Inputs.InstanceDescs = f.instances->GetGPUVirtualAddress();
        top.DestAccelerationStructureData = f.tlas->GetGPUVirtualAddress();
        top.ScratchAccelerationStructureData = f.tlasScratch->GetGPUVirtualAddress();
        m_buildList->BuildRaytracingAccelerationStructure(&top, 0, nullptr);
        D3D12_RESOURCE_BARRIER tb = UavBarrier(f.tlas.Get());
        m_buildList->ResourceBarrier(1, &tb);
        if (FAILED(m_buildList->Close())) return false;
        ID3D12CommandList* build[] = {m_buildList.Get()};
        m_queue->ExecuteCommandLists(1, build);
    }

    if (FAILED(f.allocator->Reset())) return false;
    if (FAILED(m_list->Reset(f.allocator.Get(), nullptr))) return false;

    ID3D12Resource* target = CurrentTarget();
    const D3D12_RESOURCE_STATES restState =
        m_swapChain ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COPY_SOURCE;
    const D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsvStart = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE targetRtv = {
        rtvStart.ptr + (m_swapChain ? m_swapChain->GetCurrentBackBufferIndex() * m_rtvStride : 0)};
    const float black[4] = {0, 0, 0, 1};
    const UINT drawCount = m_boxVertices + (m_settings.floor ? kFloorVertexCount : 0);

    if (m_useDlss) {
        // Each square: render at DLSS size, upscale, copy into place.
        D3D12_RESOURCE_BARRIER b = Transition(target, restState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_list->ResourceBarrier(1, &b);
        m_list->ClearRenderTargetView(targetRtv, black, 0, nullptr);
        b = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        m_list->ResourceBarrier(1, &b);

        for (size_t i = 0; i < m_views.size() && i < views.size(); ++i) {
            DlssView& v = m_views[i];
            const float ratio = static_cast<float>(v.outputSize) / v.renderSize;
            const UINT phases = std::max(1u, static_cast<UINT>(std::ceil(8.0f * ratio * ratio)));
            const UINT n = v.jitterIndex++ % phases + 1;
            const float jx = Halton(n, 2) - 0.5f, jy = Halton(n, 3) - 0.5f;
            ViewConstants vc = {};
            vc.jitterNdc[0] = 2.0f * jx / v.renderSize;
            vc.jitterNdc[1] = -2.0f * jy / v.renderSize;
            vc.viewSize = static_cast<float>(v.renderSize);

            const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {{rtvStart.ptr + (kFrameCount + 1 + 2 * i) * m_rtvStride},
                                                         {rtvStart.ptr + (kFrameCount + 2 + 2 * i) * m_rtvStride}};
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv = {dsvStart.ptr + (1 + i) * m_dsvStride};
            const float still[4] = {0, 0, 0, 0};
            m_list->ClearRenderTargetView(rtvs[0], black, 0, nullptr);
            m_list->ClearRenderTargetView(rtvs[1], still, 0, nullptr);
            m_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            m_list->OMSetRenderTargets(2, rtvs, FALSE, &dsv);
            BindScene(f, m_pipelineMV.Get());
            m_list->SetGraphicsRoot32BitConstants(1, sizeof(vc) / 4, &vc, 0);
            D3D12_VIEWPORT vp = {0, 0, static_cast<float>(v.renderSize), static_cast<float>(v.renderSize), 0, 1};
            D3D12_RECT sc = {0, 0, static_cast<LONG>(v.renderSize), static_cast<LONG>(v.renderSize)};
            m_list->RSSetViewports(1, &vp);
            m_list->RSSetScissorRects(1, &sc);
            m_list->DrawInstanced(drawCount, 1, 0, 0);

            const auto inputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            D3D12_RESOURCE_BARRIER in[3] = {
                Transition(v.color.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, inputState),
                Transition(v.motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, inputState),
                Transition(v.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, inputState),
            };
            m_list->ResourceBarrier(3, in);
            if (!m_dlss.Evaluate(m_list.Get(), v.feature, v.color.Get(), v.depth.Get(), v.motion.Get(),
                                 v.output.Get(), v.renderSize, jx, jy, v.reset)) {
                m_list->Close();
                return Fail(L"DLSS evaluation failed");
            }
            v.reset = false;
            D3D12_RESOURCE_BARRIER out[4] = {
                Transition(v.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(v.color.Get(), inputState, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(v.motion.Get(), inputState, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(v.depth.Get(), inputState, D3D12_RESOURCE_STATE_DEPTH_WRITE),
            };
            m_list->ResourceBarrier(4, out);

            const LONG x = std::clamp(static_cast<LONG>(std::lround(views[i].x)), 0L, static_cast<LONG>(m_width) - 1);
            const LONG y = std::clamp(static_cast<LONG>(std::lround(views[i].y)), 0L, static_cast<LONG>(m_height) - 1);
            D3D12_BOX srcBox = {0, 0, 0, std::min<UINT>(v.outputSize, m_width - x),
                                std::min<UINT>(v.outputSize, m_height - y), 1};
            D3D12_TEXTURE_COPY_LOCATION dst = {target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            D3D12_TEXTURE_COPY_LOCATION src = {v.output.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
            m_list->CopyTextureRegion(&dst, static_cast<UINT>(x), static_cast<UINT>(y), 0, &src, &srcBox);
            b = Transition(v.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_list->ResourceBarrier(1, &b);
        }
        b = Transition(target, D3D12_RESOURCE_STATE_COPY_DEST, restState);
        m_list->ResourceBarrier(1, &b);
    } else {
        // MSAA path: draw every square straight into the target.
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = targetRtv;
        if (m_samples > 1) {
            rtv.ptr = rtvStart.ptr + kFrameCount * m_rtvStride;
        } else {
            D3D12_RESOURCE_BARRIER b = Transition(target, restState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_list->ResourceBarrier(1, &b);
        }
        m_list->ClearRenderTargetView(rtv, black, 0, nullptr);
        m_list->ClearDepthStencilView(dsvStart, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        m_list->OMSetRenderTargets(1, &rtv, FALSE, &dsvStart);
        BindScene(f, m_pipeline.Get());
        for (const ViewportRect& v : views) {
            ViewConstants vc = {};
            vc.viewOrigin[0] = v.x;
            vc.viewOrigin[1] = v.y;
            vc.viewSize = v.size;
            m_list->SetGraphicsRoot32BitConstants(1, sizeof(vc) / 4, &vc, 0);
            D3D12_VIEWPORT vp = {v.x, v.y, v.size, v.size, 0.0f, 1.0f};
            D3D12_RECT sc = {static_cast<LONG>(v.x), static_cast<LONG>(v.y), static_cast<LONG>(v.x + v.size + 0.5f),
                             static_cast<LONG>(v.y + v.size + 0.5f)};
            m_list->RSSetViewports(1, &vp);
            m_list->RSSetScissorRects(1, &sc);
            m_list->DrawInstanced(drawCount, 1, 0, 0);
        }
        if (m_samples > 1) {
            D3D12_RESOURCE_BARRIER pre[2] = {
                Transition(m_msaaTarget.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_RESOLVE_SOURCE),
                Transition(target, restState, D3D12_RESOURCE_STATE_RESOLVE_DEST),
            };
            m_list->ResourceBarrier(2, pre);
            m_list->ResolveSubresource(target, 0, m_msaaTarget.Get(), 0, kColorFormat);
            D3D12_RESOURCE_BARRIER post[2] = {
                Transition(m_msaaTarget.Get(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(target, D3D12_RESOURCE_STATE_RESOLVE_DEST, restState),
            };
            m_list->ResourceBarrier(2, post);
        } else {
            D3D12_RESOURCE_BARRIER b = Transition(target, D3D12_RESOURCE_STATE_RENDER_TARGET, restState);
            m_list->ResourceBarrier(1, &b);
        }
    }

    if (FAILED(m_list->Close())) return false;
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    m_lastFrame = m_frameIndex;
    if (!m_swapChain) {
        if (FAILED(m_queue->Signal(m_fence.Get(), ++m_fenceValue))) return false;
        f.fence = m_fenceValue;
        m_frameIndex = (m_frameIndex + 1) % kFrameCount;
    }
    return SUCCEEDED(m_device->GetDeviceRemovedReason());
}

bool Renderer::Present(bool vsync, bool* occluded) {
    *occluded = false;
    HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, 0);
    if (hr == DXGI_STATUS_OCCLUDED) *occluded = true;
    else if (FAILED(hr)) return false;
    if (FAILED(m_queue->Signal(m_fence.Get(), ++m_fenceValue))) return false;
    m_frames[m_frameIndex].fence = m_fenceValue;
    m_frameIndex = (m_frameIndex + 1) % kFrameCount;
    return true;
}

std::wstring Renderer::DebugMessages() {
    std::wstring text;
    ComPtr<ID3D12InfoQueue> queue;
    if (!m_device || FAILED(m_device.As(&queue))) return text;
    for (UINT64 i = 0, n = queue->GetNumStoredMessages(); i < n; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> buf(size);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if (FAILED(queue->GetMessage(i, m, &size))) continue;
        if (m->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
        text += L"\n";
        for (SIZE_T c = 0; c < m->DescriptionByteLength && m->pDescription[c]; ++c)
            text += static_cast<wchar_t>(static_cast<unsigned char>(m->pDescription[c]));
    }
    return text;
}

bool Renderer::SaveScreenshot(const wchar_t* path) {
    if (!m_offscreen) return Fail(L"Screenshots need offscreen mode");
    D3D12_RESOURCE_DESC td = m_offscreen->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ComPtr<ID3D12Resource> readback =
        CreateBuffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!readback) return Fail(L"Could not allocate the readback buffer");

    FrameResources& f = m_frames[m_lastFrame];
    if (!WaitForGpu()) return false;
    f.allocator->Reset();
    m_list->Reset(f.allocator.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = m_offscreen.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (!ExecuteAndWait()) return false;

    BYTE* data = nullptr;
    D3D12_RANGE all = {0, static_cast<SIZE_T>(total)};
    if (FAILED(readback->Map(0, &all, reinterpret_cast<void**>(&data)))) return Fail(L"Map failed");

    // The target is RGBA; the PNG encoder takes BGRA.
    std::vector<BYTE> pixels(static_cast<size_t>(fp.Footprint.RowPitch) * m_height);
    std::memcpy(pixels.data(), data + fp.Offset, pixels.size());
    for (UINT yy = 0; yy < m_height; ++yy) {
        BYTE* row = pixels.data() + static_cast<size_t>(yy) * fp.Footprint.RowPitch;
        for (UINT xx = 0; xx < m_width; ++xx) std::swap(row[xx * 4], row[xx * 4 + 2]);
    }
    data = pixels.data();
    fp.Offset = 0;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comStarted = SUCCEEDED(hr);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> wic;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
             SUCCEEDED(wic->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
             SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
             SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
             SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
             SUCCEEDED(frame->SetSize(m_width, m_height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
             IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(frame->WritePixels(m_height, fp.Footprint.RowPitch, fp.Footprint.RowPitch * m_height,
                                          data + fp.Offset)) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }
    readback->Unmap(0, nullptr);
    if (comStarted) CoUninitialize();
    return ok ? true : Fail(L"Could not write the PNG file");
}
