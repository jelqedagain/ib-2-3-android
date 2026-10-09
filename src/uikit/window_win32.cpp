// The Win32 window behind the game's screen: mouse-to-touch input, keys, fullscreen, FPS title.
#include "uikit/uikit.h"
#include "foundation/runloop.h"
#include "gles/gl.h"
#include "libc/vfs.h"
#include "settings.h"
#include "win/image.h"
#include <windowsx.h>
#include <algorithm>

namespace uikit {

namespace {

HWND g_hwnd = nullptr;

CGPoint client_to_points(int cx, int cy) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    double cw = rc.right, ch = rc.bottom;
    double aspect = g_device.width_pt / g_device.height_pt;
    double ow = cw, oh = cw / aspect;
    if (oh > ch) oh = ch, ow = ch * aspect;
    double ox = (cw - ow) / 2, oy = (ch - oh) / 2;
    double nx = std::clamp((cx - ox) / ow, 0.0, 0.999), ny = std::clamp((cy - oy) / oh, 0.0, 0.999);
    return {nx * g_device.width_pt, ny * g_device.height_pt};
}

bool g_fullscreen = false;
WINDOWPLACEMENT g_windowed_placement{sizeof(WINDOWPLACEMENT)};

void toggle_fullscreen(HWND h) {
    DWORD style = GetWindowLongW(h, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof(mi)};
        if (!GetWindowPlacement(h, &g_windowed_placement) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongW(h, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_windowed_placement);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
}

// Turns generic Shift/Ctrl/Alt into their left/right variants.
int resolve_vk(WPARAM wp, LPARAM lp) {
    bool extended = (lp >> 24) & 1;
    switch (wp) {
    case VK_SHIFT: return (int)MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
    case VK_CONTROL: return extended ? VK_RCONTROL : VK_LCONTROL;
    case VK_MENU: return extended ? VK_RMENU : VK_LMENU;
    default: return (int)wp;
    }
}

constexpr UINT WM_APP_FPS = WM_APP + 10;  // FPS for the title bar (settings: ShowFPS)

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(h);
        touch_down(0, client_to_points(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) touch_move(0, client_to_points(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        touch_up(0, client_to_points(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)));
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (msg == WM_KEYDOWN && wp == VK_F11) {
            toggle_fullscreen(h);
            return 0;
        }
        if (msg == WM_KEYDOWN && (lp & (1 << 30))) return 0;  // ignore auto-repeat
        if (g_key_handler) g_key_handler(resolve_vk(wp, lp), msg == WM_KEYDOWN);
        return 0;
    case WM_KILLFOCUS:
        if (g_key_handler) g_key_handler(-1, false);  // release everything held
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
            toggle_fullscreen(h);
            return 0;
        }
        if (wp == VK_F4) break;  // keep Alt+F4
        if (msg == WM_SYSKEYDOWN && (lp & (1 << 30))) return 0;
        if (g_key_handler) g_key_handler(resolve_vk(wp, lp), msg == WM_SYSKEYDOWN);
        return 0;  // Alt / F10 must not activate the window menu
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;  // no beep on Alt+Enter
        break;
    case WM_APP_FPS: {
        wchar_t title[96];
        swprintf(title, 96, L"IB3 - %u FPS", (unsigned)wp);
        SetWindowTextW(h, title);
        return 0;
    }
    case WM_CLOSE:
        LOG_INFO("window closed; exiting");
        app_will_terminate();
        ExitProcess(0);
    case WM_SIZE:
        resize_boot_window(LOWORD(lp), HIWORD(lp));
        break;
    case WM_ERASEBKGND:
        if (!gles::window_presented()) {  // black, not white, until the first frame
            RECT rc;
            GetClientRect(h, &rc);
            FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        }
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void create_window() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"IB3Window";
    wc.hIcon = win::load_icon(vfs::to_host_w((std::string(vfs::kBundlePath) + "/Icon-60@2x.png").c_str()));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    const auto& st = settings::get();
    RECT r{0, 0, g_test_mode ? 1280 : st.window_width, g_test_mode ? 720 : st.window_height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowW(L"IB3Window", L"IB3", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_test_mode) {  // test runs render to a hidden window
        ShowWindow(g_hwnd, SW_SHOW);
        if (st.fullscreen) toggle_fullscreen(g_hwnd);
    }
    UpdateWindow(g_hwnd);
    ns::g_main_pump = [](DWORD timeout, HANDLE wake) {
        MsgWaitForMultipleObjects(1, &wake, FALSE, timeout, QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    };
}

void* main_window() { return g_hwnd; }

void show_fps(unsigned fps) { PostMessageW(g_hwnd, WM_APP_FPS, (WPARAM)fps, 0); }  // never send from the render thread

}  // namespace uikit
