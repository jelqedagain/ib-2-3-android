// Boot screen. On iOS the launch image is shown, and the startup movie plays, before UE3 has
// created its GL view; the game's own frames only start ~25 s later here. Until the game presents
// its first frame, a child window covers the game's window and draws the launch image or the
// playing movie with GDI. (GL can't be used for this: ANGLE's D3D11 backend is not safe with a
// second thread drawing while the game's render thread is already working.)
#include "audio/video.h"
#include "gles/gl.h"
#include "uikit/uikit.h"
#include "win/image.h"
#include <atomic>
#include <mutex>
#include <vector>
#include <windows.h>

namespace uikit {

namespace {

HWND g_boot = nullptr;
std::atomic<bool> g_boot_ended{false};
std::vector<u8> g_launch;  // BGRA
int g_launch_w = 0, g_launch_h = 0;
std::vector<u8> g_frame;  // BGRA of the last movie frame
int g_frame_w = 0, g_frame_h = 0;
u64 g_frame_serial = ~0ull;

constexpr UINT_PTR kRepaintTimer = 1;

void rgba_to_bgra(const u8* src, size_t pixels, std::vector<u8>& out) {
    out.resize(pixels * 4);
    for (size_t i = 0; i < pixels; i++) {
        out[i * 4 + 0] = src[i * 4 + 2];
        out[i * 4 + 1] = src[i * 4 + 1];
        out[i * 4 + 2] = src[i * 4 + 0];
        out[i * 4 + 3] = 255;
    }
}

void paint(HWND h, HDC dc) {
    const u8* rgba;
    int w, hgt;
    u64 serial;
    bool movie = video::current_frame(rgba, w, hgt, serial);
    if (movie) {
        if (serial != g_frame_serial) {
            rgba_to_bgra(rgba, (size_t)w * hgt, g_frame);
            g_frame_w = w, g_frame_h = hgt, g_frame_serial = serial;
        }
        video::release_frame();
    }
    const std::vector<u8>* img = movie ? &g_frame : &g_launch;
    int iw = movie ? g_frame_w : g_launch_w, ih = movie ? g_frame_h : g_launch_h;
    RECT rc;
    GetClientRect(h, &rc);
    int cw = rc.right, ch = rc.bottom;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP back = CreateCompatibleBitmap(dc, std::max(cw, 1), std::max(ch, 1));
    HGDIOBJ old = SelectObject(mem, back);
    FillRect(mem, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (!img->empty() && iw > 0 && ih > 0) {
        double s = std::min((double)cw / iw, (double)ch / ih);
        int ow = (int)(iw * s), oh = (int)(ih * s);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), iw, -ih, 1, 32, BI_RGB};
        SetStretchBltMode(mem, HALFTONE);
        StretchDIBits(mem, (cw - ow) / 2, (ch - oh) / 2, ow, oh, 0, 0, iw, ih, img->data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
    // Test screenshots while the boot screen is up.
    std::string shot;
    if (gles::take_screenshot_request(shot)) {
        std::vector<u8> px((size_t)cw * ch * 4);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), cw, -ch, 1, 32, BI_RGB};
        GetDIBits(mem, back, 0, ch, px.data(), &bi, DIB_RGB_COLORS);
        for (size_t i = 0; i < px.size(); i += 4) {
            std::swap(px[i], px[i + 2]);
            px[i + 3] = 255;
        }
        gles::write_png(shot.c_str(), px.data(), cw, ch);
    }
    SelectObject(mem, old);
    DeleteObject(back);
    DeleteDC(mem);
}

LRESULT CALLBACK boot_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (g_boot_ended) {
            DestroyWindow(h);
            return 0;
        }
        if (HDC dc = GetDC(h)) {  // paint directly: hidden (test) windows get no WM_PAINT
            paint(h, dc);
            ReleaseDC(h, dc);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        paint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_NCHITTEST:
        return HTTRANSPARENT;  // clicks go to the game window underneath
    case WM_DESTROY:
        g_boot = nullptr;
        LOG_INFO("boot screen closed");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void create_boot_window(void* parent_hwnd, const std::wstring& launch_image) {
    HWND parent = (HWND)parent_hwnd;
    std::vector<u8> rgba;
    if (win::load_image_rgba(launch_image, rgba, g_launch_w, g_launch_h))
        rgba_to_bgra(rgba.data(), (size_t)g_launch_w * g_launch_h, g_launch);
    else
        LOG_WARN("launch image not loaded");
    WNDCLASSW wc{};
    wc.lpfnWndProc = boot_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"IB3Boot";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassW(&wc);
    RECT rc;
    GetClientRect(parent, &rc);
    g_boot = CreateWindowW(L"IB3Boot", L"", WS_CHILD | WS_VISIBLE, 0, 0, rc.right, rc.bottom, parent, nullptr,
                           wc.hInstance, nullptr);
    SetTimer(g_boot, kRepaintTimer, 33, nullptr);
    UpdateWindow(g_boot);
}

void resize_boot_window(int w, int h) {
    if (g_boot) MoveWindow(g_boot, 0, 0, w, h, TRUE);
}

void end_boot_window() { g_boot_ended = true; }  // the window closes itself on its next timer tick

}  // namespace uikit
