#pragma once

#include <windows.h>
#include <d3d12.h>

#include <string>

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

// NVIDIA DLSS Super Resolution (Quality mode) through NGX. The DLSS runtime
// is embedded in the screen saver and unpacked to
// %LOCALAPPDATA%\FlowerBoxRT\ngx the first time it is needed.
class Dlss {
public:
    ~Dlss();

    // Returns false when DLSS cannot run here; Error() says why.
    bool Init(ID3D12Device* device);
    void Shutdown();
    bool Available() const { return m_params != nullptr; }
    const std::wstring& Error() const { return m_error; }

    // Square render size DLSS Quality asks for, for a square output.
    bool RenderSize(UINT outputSize, UINT* renderSize);
    NVSDK_NGX_Handle* CreateFeature(ID3D12GraphicsCommandList* list, UINT renderSize, UINT outputSize);
    void ReleaseFeature(NVSDK_NGX_Handle* feature);
    bool Evaluate(ID3D12GraphicsCommandList* list, NVSDK_NGX_Handle* feature, ID3D12Resource* color,
                  ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* output, UINT renderSize,
                  float jitterX, float jitterY, bool reset);

private:
    ID3D12Device* m_device = nullptr;
    NVSDK_NGX_Parameter* m_params = nullptr;
    bool m_initialized = false;
    std::wstring m_error;
};
