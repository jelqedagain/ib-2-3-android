// Controllers on Windows: XInput (Xbox-style pads), and the legend drawn with GDI.
#include "game/pad.h"
#include "libc/format.h"
#include "uikit/uikit.h"
#include <windows.h>
#include <xinput.h>

namespace game::pad {

const char* const kBackName = "Back";
const char* const kStartName = "Start";

namespace {

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
GetStateFn g_get_state = nullptr;

float axis(SHORT v) { return std::max(-1.0f, v / 32767.0f); }

}  // namespace

bool init() {
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryW(dll)) {
            g_get_state = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
            if (g_get_state) break;
        }
    }
    if (!g_get_state) LOG_WARN("controller: XInput is not available");
    return g_get_state != nullptr;
}

void read(State& p) {
    for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
        XINPUT_STATE st{};
        if (g_get_state(i, &st) != ERROR_SUCCESS) continue;
        const XINPUT_GAMEPAD& g = st.Gamepad;
        p.connected = true;
        p.buttons = g.wButtons;
        p.lx = axis(g.sThumbLX), p.ly = axis(g.sThumbLY);
        p.rx = axis(g.sThumbRX), p.ry = axis(g.sThumbRY);
        p.lt = g.bLeftTrigger / 255.0f, p.rt = g.bRightTrigger / 255.0f;
        break;
    }
}

bool game_in_foreground() { return GetForegroundWindow() == (HWND)uikit::main_window(); }

void render_legend(const std::vector<std::string>& lines, std::vector<u8>& rgba, int& w, int& h) {
    constexpr int kLineH = 40, kPad = 16;
    std::vector<std::wstring> wide;
    for (auto& line : lines) wide.push_back(libc::utf8_to_wide(line));
    HFONT font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    {  // size the panel to the longest line
        HDC measure = CreateCompatibleDC(nullptr);
        HGDIOBJ f = SelectObject(measure, font);
        int widest = 0;
        for (auto& line : wide) {
            RECT r{0, 0, 0, 0};
            DrawTextW(measure, line.c_str(), -1, &r, DT_CALCRECT | DT_SINGLELINE);
            widest = std::max(widest, (int)r.right);
        }
        SelectObject(measure, f);
        DeleteDC(measure);
        w = widest + 2 * kPad + 8;
        h = (int)wide.size() * kLineH + 2 * kPad;
    }
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    RECT all{0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(RGB(14, 20, 34));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(225, 235, 245));
    for (size_t i = 0; i < wide.size(); i++) {
        RECT r{kPad + 4, kPad + (int)i * kLineH, w - kPad, kPad + ((int)i + 1) * kLineH};
        DrawTextW(dc, wide[i].c_str(), -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    }
    GdiFlush();
    rgba.resize((size_t)w * h * 4);
    const u8* src = static_cast<const u8*>(bits);
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = src[i + 2];
        rgba[i + 1] = src[i + 1];
        rgba[i + 2] = src[i];
        rgba[i + 3] = 255;
    }
    SelectObject(dc, old_font);
    SelectObject(dc, old_bmp);
    DeleteObject(font);
    DeleteObject(bmp);
    DeleteDC(dc);
}

}  // namespace game::pad
