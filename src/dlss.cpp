// This software contains source code provided by NVIDIA Corporation.
#include "dlss.h"

#include <shlobj.h>

#include <cstdio>
#include <vector>

#include "nvsdk_ngx_helpers.h"
#include "resource.h"

namespace {

// NGX project identifier for this application (NVSDK_NGX_ENGINE_TYPE_CUSTOM).
const char kProjectId[] = "6f0d3a52-8c41-4b7e-9e2a-3d5f1b7c9a64";

std::wstring LocalDataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = base;
    CoTaskMemFree(base);
    if (dir.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        dir = tmp;
    }
    dir += L"\\FlowerBoxRT";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

unsigned long long Fnv1a(const BYTE* data, size_t size) {
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

bool ReadMarker(const std::wstring& path, unsigned long long* value) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    bool ok = fread(value, sizeof(*value), 1, f) == 1;
    fclose(f);
    return ok;
}

// Unpacks the embedded nvngx_dlss.dll into dir unless an identical copy is there.
bool ExtractRuntime(const std::wstring& dir, std::wstring* error) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(IDR_DLSS_RUNTIME), RT_RCDATA);
    HGLOBAL mem = res ? LoadResource(self, res) : nullptr;
    const BYTE* data = mem ? static_cast<const BYTE*>(LockResource(mem)) : nullptr;
    const DWORD size = res ? SizeofResource(self, res) : 0;
    if (!data || !size) {
        *error = L"the DLSS runtime is missing from the screen saver";
        return false;
    }
    const unsigned long long hash = Fnv1a(data, size) ^ size;
    const std::wstring dll = dir + L"\\nvngx_dlss.dll", marker = dir + L"\\nvngx_dlss.id";

    unsigned long long existing = 0;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (ReadMarker(marker, &existing) && existing == hash &&
        GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &attr) && attr.nFileSizeHigh == 0 &&
        attr.nFileSizeLow == size)
        return true;

    const std::wstring tmp = dll + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *error = L"could not unpack the DLSS runtime";
        return false;
    }
    DWORD written = 0;
    const bool wrote = WriteFile(h, data, size, &written, nullptr) && written == size;
    CloseHandle(h);
    if (!wrote || !MoveFileExW(tmp.c_str(), dll.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        // Another running copy may hold the old DLL; it is still usable if present.
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            *error = L"could not unpack the DLSS runtime";
            return false;
        }
        return true;
    }
    FILE* f = nullptr;
    if (_wfopen_s(&f, marker.c_str(), L"wb") == 0 && f) {
        fwrite(&hash, sizeof(hash), 1, f);
        fclose(f);
    }
    return true;
}

}  // namespace

Dlss::~Dlss() { Shutdown(); }

bool Dlss::Init(ID3D12Device* device) {
    Shutdown();
    m_device = device;
    const std::wstring dataDir = LocalDataDir();
    const std::wstring runtimeDir = dataDir + L"\\ngx";
    CreateDirectoryW(runtimeDir.c_str(), nullptr);
    if (!ExtractRuntime(runtimeDir, &m_error)) return false;

    const wchar_t* paths[] = {runtimeDir.c_str()};
    NVSDK_NGX_FeatureCommonInfo info = {};
    info.PathListInfo.Path = paths;
    info.PathListInfo.Length = 1;
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                             dataDir.c_str(), device, &info);
    if (NVSDK_NGX_FAILED(r)) {
        m_error = L"NVIDIA NGX is not available on this graphics card";
        return false;
    }
    m_initialized = true;
    r = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_params);
    if (NVSDK_NGX_FAILED(r) || !m_params) {
        m_params = nullptr;
        m_error = L"NVIDIA NGX did not report its capabilities";
        Shutdown();
        return false;
    }
    int available = 0, needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(m_params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (!available) {
        m_error = needsDriver ? L"DLSS needs a newer NVIDIA driver" : L"DLSS is not supported on this graphics card";
        Shutdown();
        return false;
    }
    return true;
}

void Dlss::Shutdown() {
    if (m_params) NVSDK_NGX_D3D12_DestroyParameters(m_params);
    m_params = nullptr;
    if (m_initialized) NVSDK_NGX_D3D12_Shutdown1(m_device);
    m_initialized = false;
}

bool Dlss::RenderSize(UINT outputSize, UINT* renderSize) {
    unsigned int w = 0, h = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0;
    NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS(m_params, outputSize, outputSize,
                                                       NVSDK_NGX_PerfQuality_Value_MaxQuality, &w, &h, &maxW, &maxH,
                                                       &minW, &minH, &sharpness);
    if (NVSDK_NGX_FAILED(r) || w == 0 || h == 0) return false;
    *renderSize = w < h ? w : h;
    return true;
}

NVSDK_NGX_Handle* Dlss::CreateFeature(ID3D12GraphicsCommandList* list, UINT renderSize, UINT outputSize) {
    NVSDK_NGX_DLSS_Create_Params p = {};
    p.Feature.InWidth = renderSize;
    p.Feature.InHeight = renderSize;
    p.Feature.InTargetWidth = outputSize;
    p.Feature.InTargetHeight = outputSize;
    p.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    NVSDK_NGX_Handle* handle = nullptr;
    if (NVSDK_NGX_FAILED(NGX_D3D12_CREATE_DLSS_EXT(list, 1, 1, &handle, m_params, &p))) return nullptr;
    return handle;
}

void Dlss::ReleaseFeature(NVSDK_NGX_Handle* feature) {
    if (feature) NVSDK_NGX_D3D12_ReleaseFeature(feature);
}

bool Dlss::Evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle* feature, ID3D12Resource* color,
                    ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, UINT renderSize,
                    float jitterX, float jitterY, bool reset) {
    NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
    e.Feature.pInColor = color;
    e.Feature.pInOutput = output;
    e.pInDepth = depth;
    e.pInMotionVectors = motion;
    e.InJitterOffsetX = jitterX;
    e.InJitterOffsetY = jitterY;
    e.InRenderSubrectDimensions.Width = renderSize;
    e.InRenderSubrectDimensions.Height = renderSize;
    e.InReset = reset ? 1 : 0;
    // Motion vectors are written in UV units; DLSS wants render pixels.
    e.InMVScaleX = static_cast<float>(renderSize);
    e.InMVScaleY = static_cast<float>(renderSize);
    return NVSDK_NGX_SUCCEED(NGX_D3D12_EVALUATE_DLSS_EXT(list, feature, m_params, &e));
}
