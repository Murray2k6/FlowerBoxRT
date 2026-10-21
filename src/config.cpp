#include "config.h"

#include <commctrl.h>

#include <cwchar>

#include "renderer.h"
#include "resource.h"

namespace {

struct DialogState {
    Settings* settings;
    GpuInfo gpu;
};

const int kMsaaValues[] = {1, 2, 4, 8};
const wchar_t* const kMsaaNames[] = {L"Off", L"2x", L"4x", L"8x"};
const wchar_t* const kFrameRateNames[] = {L"30 fps", L"60 fps", L"Match display"};

void SetupSlider(HWND dlg, int id, int lo, int hi, int freq) {
    HWND h = GetDlgItem(dlg, id);
    SendMessageW(h, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessageW(h, TBM_SETTICFREQ, freq, 0);
    SendMessageW(h, TBM_SETPAGESIZE, 0, freq);
}

int Slider(HWND dlg, int id) { return static_cast<int>(SendDlgItemMessageW(dlg, id, TBM_GETPOS, 0, 0)); }
void SetSlider(HWND dlg, int id, int v) { SendDlgItemMessageW(dlg, id, TBM_SETPOS, TRUE, v); }
bool Checked(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }
void Check(HWND dlg, int id, bool on) { CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED); }

void UpdateLabels(HWND dlg) {
    wchar_t buf[64];
    swprintf_s(buf, L"%d%%", Slider(dlg, IDC_SPEED));
    SetDlgItemTextW(dlg, IDC_SPEED_LABEL, buf);
    swprintf_s(buf, L"Samples per pixel: %d", Slider(dlg, IDC_RT_SAMPLES));
    SetDlgItemTextW(dlg, IDC_RT_SAMPLES_LABEL, buf);
    int bounces = Slider(dlg, IDC_RT_BOUNCES);
    if (bounces == 0) swprintf_s(buf, L"Reflection bounces: off");
    else swprintf_s(buf, L"Reflection bounces: %d", bounces);
    SetDlgItemTextW(dlg, IDC_RT_BOUNCES_LABEL, buf);
    swprintf_s(buf, L"Reflectivity: %d%%", Slider(dlg, IDC_RT_REFLECT));
    SetDlgItemTextW(dlg, IDC_RT_REFLECT_LABEL, buf);
}

void UpdateEnabled(HWND dlg, const DialogState& state) {
    EnableWindow(GetDlgItem(dlg, IDC_RT_ENABLE), state.gpu.rayTracing);
    const bool rt = state.gpu.rayTracing && Checked(dlg, IDC_RT_ENABLE);
    for (int id : {IDC_RT_SHADOWS, IDC_RT_AO, IDC_RT_SAMPLES_LABEL, IDC_RT_SAMPLES, IDC_RT_BOUNCES_LABEL,
                   IDC_RT_BOUNCES})
        EnableWindow(GetDlgItem(dlg, id), rt);
    const bool reflect = rt && Slider(dlg, IDC_RT_BOUNCES) > 0;
    EnableWindow(GetDlgItem(dlg, IDC_RT_REFLECT_LABEL), reflect);
    EnableWindow(GetDlgItem(dlg, IDC_RT_REFLECT), reflect);
    // DLSS is built in: when it runs it does the anti-aliasing.
    EnableWindow(GetDlgItem(dlg, IDC_MSAA), !state.gpu.dlss);
}

void ToControls(HWND dlg, const Settings& s, const DialogState& state) {
    CheckRadioButton(dlg, IDC_CHECKER, IDC_ONECOLOR, IDC_CHECKER + s.colorMode);
    Check(dlg, IDC_SMOOTH, s.smooth);
    Check(dlg, IDC_SLANTED, s.slanted);
    Check(dlg, IDC_CYCLE, s.cycle);
    Check(dlg, IDC_SPIN, s.spin);
    Check(dlg, IDC_BLOOM, s.bloom);
    Check(dlg, IDC_TWOSIDED, s.twoSided);
    SendDlgItemMessageW(dlg, IDC_SHAPE, CB_SETCURSEL, s.shape, 0);
    SetSlider(dlg, IDC_COMPLEXITY, s.subdiv);
    SetSlider(dlg, IDC_IMAGESIZE, s.imageSize);
    SetSlider(dlg, IDC_SPEED, s.speed);
    Check(dlg, IDC_RT_ENABLE, s.rayTracing && state.gpu.rayTracing);
    Check(dlg, IDC_RT_SHADOWS, s.rtShadows);
    Check(dlg, IDC_RT_AO, s.rtAmbientOcclusion);
    SetSlider(dlg, IDC_RT_SAMPLES, s.rtSamples);
    SetSlider(dlg, IDC_RT_BOUNCES, s.rtBounces);
    SetSlider(dlg, IDC_RT_REFLECT, s.rtReflectivity);
    int msaa = 2;
    for (int i = 0; i < 4; ++i)
        if (kMsaaValues[i] == s.msaa) msaa = i;
    SendDlgItemMessageW(dlg, IDC_MSAA, CB_SETCURSEL, msaa, 0);
    SendDlgItemMessageW(dlg, IDC_FPS, CB_SETCURSEL, s.frameRate, 0);
    Check(dlg, IDC_FLOOR, s.floor);
    UpdateLabels(dlg);
    UpdateEnabled(dlg, state);
}

void FromControls(HWND dlg, Settings& s, const DialogState& state) {
    s.colorMode = Checked(dlg, IDC_CHECKER) ? kColorChecker : Checked(dlg, IDC_ONECOLOR) ? kColorOne : kColorPerSide;
    s.smooth = Checked(dlg, IDC_SMOOTH);
    s.slanted = Checked(dlg, IDC_SLANTED);
    s.cycle = Checked(dlg, IDC_CYCLE);
    s.spin = Checked(dlg, IDC_SPIN);
    s.bloom = Checked(dlg, IDC_BLOOM);
    s.twoSided = Checked(dlg, IDC_TWOSIDED);
    s.shape = static_cast<int>(SendDlgItemMessageW(dlg, IDC_SHAPE, CB_GETCURSEL, 0, 0));
    s.subdiv = Slider(dlg, IDC_COMPLEXITY);
    s.imageSize = Slider(dlg, IDC_IMAGESIZE);
    s.speed = Slider(dlg, IDC_SPEED);
    // An unsupported card keeps the saved choice, so moving the settings to
    // a ray-tracing PC later still works.
    if (state.gpu.rayTracing) s.rayTracing = Checked(dlg, IDC_RT_ENABLE);
    s.rtShadows = Checked(dlg, IDC_RT_SHADOWS);
    s.rtAmbientOcclusion = Checked(dlg, IDC_RT_AO);
    s.rtSamples = Slider(dlg, IDC_RT_SAMPLES);
    s.rtBounces = Slider(dlg, IDC_RT_BOUNCES);
    s.rtReflectivity = Slider(dlg, IDC_RT_REFLECT);
    s.msaa = kMsaaValues[SendDlgItemMessageW(dlg, IDC_MSAA, CB_GETCURSEL, 0, 0) & 3];
    s.frameRate = static_cast<int>(SendDlgItemMessageW(dlg, IDC_FPS, CB_GETCURSEL, 0, 0));
    s.floor = Checked(dlg, IDC_FLOOR);
    s.Clamp();
}

INT_PTR CALLBACK ConfigProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    DialogState* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(dlg, DWLP_USER));
    switch (msg) {
        case WM_INITDIALOG: {
            state = reinterpret_cast<DialogState*>(lp);
            SetWindowLongPtrW(dlg, DWLP_USER, lp);
            HINSTANCE inst = GetModuleHandleW(nullptr);
            HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
            SendMessageW(dlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(dlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            for (int i = 0; i < kShapeCount; ++i) {
                wchar_t name[64];
                LoadStringW(inst, IDS_SHAPE_CUBE + i, name, 64);
                SendDlgItemMessageW(dlg, IDC_SHAPE, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
            }
            for (const wchar_t* n : kMsaaNames)
                SendDlgItemMessageW(dlg, IDC_MSAA, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
            for (const wchar_t* n : kFrameRateNames)
                SendDlgItemMessageW(dlg, IDC_FPS, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
            SetupSlider(dlg, IDC_COMPLEXITY, kSubdivMin, kSubdivMax, 2);
            SetupSlider(dlg, IDC_IMAGESIZE, kImageSizeMin, kImageSizeMax, 10);
            SetupSlider(dlg, IDC_SPEED, kSpeedMin, kSpeedMax, 10);
            SetupSlider(dlg, IDC_RT_SAMPLES, kSamplesMin, kSamplesMax, 1);
            SetupSlider(dlg, IDC_RT_BOUNCES, kBouncesMin, kBouncesMax, 1);
            SetupSlider(dlg, IDC_RT_REFLECT, kReflectivityMin, kReflectivityMax, 10);

            std::wstring status;
            if (!state->gpu.found) status = L"No DirectX 12 graphics card found.";
            else if (state->gpu.rayTracing) status = L"Supported on " + state->gpu.adapter + L".";
            else status = L"Not supported on " + state->gpu.adapter + L". The classic renderer is used.";
            SetDlgItemTextW(dlg, IDC_RT_STATUS, status.c_str());
            const std::wstring dlss = state->gpu.dlss
                                          ? L"NVIDIA DLSS (Quality) is on and does the anti-aliasing."
                                          : L"DLSS is off: " + state->gpu.dlssReason + L". Anti-aliasing uses MSAA.";
            SetDlgItemTextW(dlg, IDC_DLSS_STATUS, dlss.c_str());

            ToControls(dlg, *state->settings, *state);
            return TRUE;
        }
        case WM_HSCROLL:
            if (state) {
                UpdateLabels(dlg);
                UpdateEnabled(dlg, *state);
            }
            return TRUE;
        case WM_COMMAND:
            if (!state) break;
            switch (LOWORD(wp)) {
                case IDC_RT_ENABLE:
                    UpdateEnabled(dlg, *state);
                    return TRUE;
                case IDC_DEFAULTS:
                    ToControls(dlg, Settings{}, *state);
                    return TRUE;
                case IDOK:
                    FromControls(dlg, *state->settings, *state);
                    state->settings->Save();
                    EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

}  // namespace

void RunConfigDialog(HINSTANCE instance, HWND parent) {
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Settings settings;
    settings.Load();
    DialogState state = {&settings, QueryGpu()};
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), parent, ConfigProc, reinterpret_cast<LPARAM>(&state));
}
