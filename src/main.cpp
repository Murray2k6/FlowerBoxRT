// 3D FlowerBox RT: the Windows NT OpenGL "3D FlowerBox" screen saver rebuilt
// for Windows 11 on Direct3D 12, with optional ray tracing.
//
//   /s            run full screen
//   /p <hwnd>     run in the Screen Saver Settings preview
//   /c[:<hwnd>]   settings (also when started without arguments)
//   /snapshot <file.png> <seconds> <width> <height> [Name=value ...]
//                 render one frame offscreen to a PNG (for testing)

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <random>
#include <string>
#include <vector>

#include "config.h"
#include "flowerbox.h"
#include "renderer.h"
#include "resource.h"
#include "settings.h"

namespace {

// The original advanced one animation step per 16 ms WM_TIMER message. On
// the standard 15.6 ms system clock a 16 ms timer fires every second tick,
// so it ran at 32 steps per second; Speed 100% reproduces that pace. (When
// another program raised the timer resolution the original sped up to 62.5
// steps per second, which is why its timing drifts on modern Windows.)
constexpr double kStepsPerSecond = 1000.0 / 31.25;

const wchar_t kWindowClass[] = L"FlowerBoxRTWindow";

enum class Mode { Config, Saver, Preview, Snapshot };

// The image floats around its monitor in a square viewport, as the original's
// floating window did: side = (w + h) / 2 * (0.25 + size / 200), speed
// 0.4 to 1 times side / 200 pixels per step.
class Floater {
public:
    void Init(const RECT& area, int imageSize, std::mt19937& rng, bool centered) {
        m_left = static_cast<float>(area.left);
        m_top = static_cast<float>(area.top);
        m_width = static_cast<float>(area.right - area.left);
        m_height = static_cast<float>(area.bottom - area.top);
        float side = (m_width + m_height) * 0.5f * (imageSize * 0.005f + 0.25f);
        m_size = std::clamp(side, 1.0f, std::max(1.0f, std::min(m_width, m_height)));
        m_vmax = std::max(1.0f, m_size * 0.005f);
        m_vmin = 0.4f * m_vmax;
        if (centered) {
            m_x = (m_width - m_size) * 0.5f;
            m_y = (m_height - m_size) * 0.5f;
        } else {
            m_x = std::uniform_real_distribution<float>(0.0f, std::max(0.0f, m_width - m_size))(rng);
            m_y = std::uniform_real_distribution<float>(0.0f, std::max(0.0f, m_height - m_size))(rng);
        }
        m_vx = Speed(rng) * (rng() & 1 ? 1.0f : -1.0f);
        m_vy = Speed(rng) * (rng() & 1 ? 1.0f : -1.0f);
    }

    void Advance(double steps, std::mt19937& rng) {
        Move(m_x, m_vx, m_width - m_size, steps, rng);
        Move(m_y, m_vy, m_height - m_size, steps, rng);
    }

    ViewportRect View() const { return {m_left + m_x, m_top + m_y, m_size}; }

private:
    float Speed(std::mt19937& rng) { return std::uniform_real_distribution<float>(m_vmin, m_vmax)(rng); }

    void Move(float& pos, float& vel, float limit, double steps, std::mt19937& rng) {
        if (limit <= 0) {
            pos = 0;
            return;
        }
        pos += static_cast<float>(vel * steps);
        if (pos < 0) {
            pos = std::min(-pos, limit);
            vel = Speed(rng);
        } else if (pos > limit) {
            pos = std::max(2 * limit - pos, 0.0f);
            vel = -Speed(rng);
        }
    }

    float m_left = 0, m_top = 0, m_width = 1, m_height = 1, m_size = 1;
    float m_x = 0, m_y = 0, m_vx = 0, m_vy = 0, m_vmin = 0, m_vmax = 1;
};

struct App {
    Mode mode = Mode::Saver;
    HWND hwnd = nullptr;
    Settings settings;
    FlowerBox box;
    Renderer renderer;
    bool rendererReady = false;
    std::vector<RECT> areas;
    std::vector<Floater> floaters;
    std::mt19937 rng{std::random_device{}()};
    bool haveMouse = false;
    POINT mouse = {};
    ULONGLONG startTick = 0;
    UINT width = 0, height = 0;
    bool resized = false;
    unsigned frames = 0;
};

App* g_app = nullptr;

void CloseSaver(HWND hwnd) {
    if (g_app && g_app->mode == Mode::Saver) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App* app = g_app;
    const bool saver = app && app->mode == Mode::Saver;
    switch (msg) {
        case WM_SETCURSOR:
            if (saver) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            if (saver) {
                POINT p = {static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
                if (!app->haveMouse || GetTickCount64() - app->startTick < 500) {
                    app->mouse = p;
                    app->haveMouse = true;
                } else if (std::abs(p.x - app->mouse.x) > 4 || std::abs(p.y - app->mouse.y) > 4) {
                    CloseSaver(hwnd);
                }
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_XBUTTONDOWN:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            CloseSaver(hwnd);
            if (saver) return 0;
            break;
        case WM_ACTIVATEAPP:
            if (saver && !wp && GetTickCount64() - app->startTick > 1000) CloseSaver(hwnd);
            break;
        case WM_SYSCOMMAND:
            if (saver && ((wp & 0xFFF0) == SC_SCREENSAVE || (wp & 0xFFF0) == SC_MONITORPOWER)) return 0;
            break;
        case WM_SIZE:
            if (app) {
                app->width = LOWORD(lp);
                app->height = HIWORD(lp);
                app->resized = true;
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (!app || !app->rendererReady) FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL CALLBACK CollectMonitor(HMONITOR, HDC, LPRECT rc, LPARAM lp) {
    reinterpret_cast<std::vector<RECT>*>(lp)->push_back(*rc);
    return TRUE;
}

void InitFloaters(App& app, bool centered) {
    app.floaters.assign(app.areas.size(), Floater{});
    for (size_t i = 0; i < app.areas.size(); ++i)
        app.floaters[i].Init(app.areas[i], app.settings.imageSize, app.rng, centered);
}

std::vector<ViewportRect> Views(const App& app) {
    std::vector<ViewportRect> views;
    for (const Floater& f : app.floaters) views.push_back(f.View());
    return views;
}

// Waits until the deadline while still handling window messages.
void WaitUntil(HANDLE timer, LONGLONG deadline, LONGLONG freq) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG remaining = deadline - now.QuadPart;
    if (remaining <= 0) return;
    LARGE_INTEGER due;
    due.QuadPart = -std::max<LONGLONG>(1, remaining * 10000000 / freq);
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
        while (MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage(static_cast<int>(msg.wParam));
                    return;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    } else {
        Sleep(static_cast<DWORD>(remaining * 1000 / freq));
    }
}

int RunAnimation(App& app) {
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

    const bool vsync = app.settings.frameRate == kFrameRateVSync;
    double fps = app.settings.frameRate == kFrameRate30 ? 30.0 : 60.0;
    if (app.mode == Mode::Preview) fps = 30.0;
    const LONGLONG period = static_cast<LONGLONG>(freq.QuadPart / fps);
    LONGLONG deadline = last.QuadPart;
    const double stepsPerSecond = kStepsPerSecond * app.settings.speed / 100.0;

    MSG msg = {};
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                if (timer) CloseHandle(timer);
                return static_cast<int>(msg.wParam);
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!app.rendererReady) {
            WaitMessage();
            continue;
        }
        if (app.resized) {
            app.resized = false;
            if (app.mode == Mode::Preview && app.width > 0 && app.height > 0) {
                app.areas = {RECT{0, 0, static_cast<LONG>(app.width), static_cast<LONG>(app.height)}};
                InitFloaters(app, false);
                if (!app.renderer.Resize(app.width, app.height, Views(app))) break;
            }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double dt = std::min(0.1, static_cast<double>(now.QuadPart - last.QuadPart) / freq.QuadPart);
        last = now;
        const double steps = dt * stepsPerSecond;
        app.box.Advance(steps);
        for (Floater& f : app.floaters) f.Advance(steps, app.rng);

        bool occluded = false;
        if (!app.renderer.Render(app.box, Views(app)) || !app.renderer.Present(vsync, &occluded)) {
            // Device lost (driver update, GPU reset): stop drawing, keep the window black.
            app.rendererReady = false;
            InvalidateRect(app.hwnd, nullptr, TRUE);
            continue;
        }
        ++app.frames;
        if (occluded) {
            Sleep(100);
            QueryPerformanceCounter(&last);
            deadline = last.QuadPart;
            continue;
        }
        if (!vsync) {
            deadline += period;
            QueryPerformanceCounter(&now);
            if (now.QuadPart - deadline > period) deadline = now.QuadPart;
            WaitUntil(timer, deadline, freq.QuadPart);
        }
    }
    if (timer) CloseHandle(timer);
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

bool RegisterWindowClass(HINSTANCE inst) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) != 0;
}

int RunSaver(HINSTANCE inst, Mode mode, HWND parent) {
    App app;
    app.mode = mode;
    app.settings.Load();
    g_app = &app;
    if (!RegisterWindowClass(inst)) return 1;

    if (mode == Mode::Saver) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        app.width = static_cast<UINT>(vw);
        app.height = static_cast<UINT>(vh);
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&app.areas));
        for (RECT& r : app.areas) OffsetRect(&r, -vx, -vy);
        if (app.areas.empty()) app.areas.push_back({0, 0, vw, vh});
        app.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kWindowClass, L"3D FlowerBox RT",
                                   WS_POPUP | WS_VISIBLE, vx, vy, vw, vh, nullptr, nullptr, inst, nullptr);
    } else {
        RECT rc;
        if (!IsWindow(parent) || !GetClientRect(parent, &rc)) return 1;
        app.width = static_cast<UINT>(std::max<LONG>(rc.right, 1));
        app.height = static_cast<UINT>(std::max<LONG>(rc.bottom, 1));
        app.areas.push_back({0, 0, static_cast<LONG>(app.width), static_cast<LONG>(app.height)});
        app.hwnd = CreateWindowExW(0, kWindowClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, app.width, app.height, parent,
                                   nullptr, inst, nullptr);
    }
    if (!app.hwnd) return 1;
    app.resized = false;
    app.startTick = GetTickCount64();
    if (mode == Mode::Saver) SetForegroundWindow(app.hwnd);

    app.box.Init(app.settings);
    InitFloaters(app, false);
    app.rendererReady = app.renderer.Init(app.hwnd, app.width, app.height, app.settings, app.box.VertexCount(), Views(app));
    if (!app.rendererReady) InvalidateRect(app.hwnd, nullptr, TRUE);
    int result = RunAnimation(app);
    // FLOWERBOX_LOG=<file> records how the run went, for testing.
    wchar_t logPath[MAX_PATH];
    if (GetEnvironmentVariableW(L"FLOWERBOX_LOG", logPath, MAX_PATH)) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, logPath, L"w, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"%s, ray tracing %s, %s, ready %d, %ux%u, %zu squares, %u frames\n%s%s\n",
                     app.renderer.AdapterName().c_str(), app.renderer.RayTracingActive() ? L"on" : L"off",
                     app.renderer.DlssStatus().c_str(), app.rendererReady ? 1 : 0, app.width, app.height,
                     app.floaters.size(), app.frames, app.renderer.Error().c_str(), app.renderer.DebugMessages().c_str());
            fclose(f);
        }
    }
    g_app = nullptr;
    return result;
}

int RunSnapshot(const std::vector<std::wstring>& args) {
    // args: <file.png> <seconds> <width> <height> [Name=value ...]
    if (args.size() < 4) return 2;
    App app;
    app.mode = Mode::Snapshot;
    app.settings.Load();
    for (size_t i = 4; i < args.size(); ++i) {
        size_t eq = args[i].find(L'=');
        if (eq == std::wstring::npos || !app.settings.Set(args[i].substr(0, eq).c_str(), _wtoi(args[i].c_str() + eq + 1)))
            return 2;
    }
    app.settings.Clamp();
    const double seconds = _wtof(args[1].c_str());
    app.width = static_cast<UINT>(std::max(1, _wtoi(args[2].c_str())));
    app.height = static_cast<UINT>(std::max(1, _wtoi(args[3].c_str())));
    app.areas.push_back({0, 0, static_cast<LONG>(app.width), static_cast<LONG>(app.height)});
    app.box.Init(app.settings);
    InitFloaters(app, true);
    // Renders a run of frames 1/60 s apart, ending <seconds> into the
    // animation, so DLSS has a history; the last frame is saved.
    wchar_t framesVar[16] = L"32";
    GetEnvironmentVariableW(L"FLOWERBOX_SNAPSHOT_FRAMES", framesVar, 16);
    const int frames = std::max(1, _wtoi(framesVar));
    const double stepsPerSecond = kStepsPerSecond * app.settings.speed / 100.0;
    const double stepsPerFrame = stepsPerSecond / 60.0;
    // Advance in single steps, as the original's timer did.
    double steps = std::max(0.0, seconds * stepsPerSecond - stepsPerFrame * (frames - 1));
    for (; steps >= 1.0; steps -= 1.0) app.box.Advance(1.0);
    app.box.Advance(steps);
    // The status goes next to the image, since a GUI program has no console.
    auto report = [&](const std::wstring& text) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, (args[0] + L".txt").c_str(), L"w, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"%s\n", text.c_str());
            fclose(f);
        }
    };
    bool ok = app.renderer.Init(nullptr, app.width, app.height, app.settings, app.box.VertexCount(), Views(app));
    for (int i = 0; ok && i < frames; ++i) {
        if (i > 0) app.box.Advance(stepsPerFrame);
        ok = app.renderer.Render(app.box, Views(app));
    }
    if (!ok || !app.renderer.SaveScreenshot(args[0].c_str())) {
        report(L"error: " + app.renderer.Error() + app.renderer.DebugMessages());
        return 1;
    }
    std::wstring status = app.renderer.AdapterName() + L", ray tracing " +
                          (app.renderer.RayTracingActive() ? L"on" : L"off") + L", " + app.renderer.DlssStatus() +
                          L", " + std::to_wstring(app.box.VertexCount()) + L" vertices";
    if (!app.renderer.Error().empty()) status += L", note: " + app.renderer.Error();
    report(status + app.renderer.DebugMessages());
    return 0;
}

HWND ParseHwnd(const std::wstring& s) {
    return reinterpret_cast<HWND>(static_cast<UINT_PTR>(wcstoull(s.c_str(), nullptr, 10)));
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args(argv + 1, argv + argc);
    LocalFree(argv);

    Mode mode = Mode::Config;
    HWND window = nullptr;
    if (!args.empty()) {
        std::wstring a = args[0];
        for (wchar_t& ch : a) ch = static_cast<wchar_t>(std::towlower(ch));
        if (a == L"/snapshot" || a == L"-snapshot") {
            mode = Mode::Snapshot;
        } else if (a.size() >= 2 && (a[0] == L'/' || a[0] == L'-')) {
            const wchar_t kind = a[1];
            std::wstring value = a.size() > 3 && a[2] == L':' ? a.substr(3) : (args.size() > 1 ? args[1] : L"");
            if (kind == L's') mode = Mode::Saver;
            else if (kind == L'p' || kind == L'l') {
                mode = Mode::Preview;
                window = ParseHwnd(value);
            } else if (kind == L'c') {
                mode = Mode::Config;
                window = a.size() > 3 && a[2] == L':' ? ParseHwnd(a.substr(3)) : GetForegroundWindow();
            }
        }
    }

    switch (mode) {
        case Mode::Saver: return RunSaver(inst, Mode::Saver, nullptr);
        case Mode::Preview: return RunSaver(inst, Mode::Preview, window);
        case Mode::Snapshot: return RunSnapshot(std::vector<std::wstring>(args.begin() + 1, args.end()));
        default: RunConfigDialog(inst, window); return 0;
    }
}
