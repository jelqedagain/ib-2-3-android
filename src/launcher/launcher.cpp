// The launcher: shown when the executable is started on its own. First run installs the game
// from the player's .ipa; afterwards it edits settings.ini (display, graphics, audio, key
// bindings) and starts the game (the same executable with -play).
#ifndef UNICODE
#define UNICODE
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10 (per-monitor DPI APIs)
#include "game/actions.h"
#include "launcher/install.h"
#include "launcher/launcher.h"
#include "settings.h"
#include "gles/gl.h"
#include <atomic>
#include <thread>
#include <vector>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

namespace launcher {

namespace {

enum : int {
    IDC_MODE = 100, IDC_WINSIZE, IDC_RES, IDC_FPS, IDC_SHOWFPS, IDC_AA, IDC_ANISO, IDC_SHADOWS, IDC_HISHADOWS,
    IDC_SHAFTS, IDC_BLOOM, IDC_DOF, IDC_MUSIC, IDC_EFFECTS, IDC_MUSIC_VAL, IDC_EFFECTS_VAL, IDC_KEYS, IDC_STATUS,
    IDC_PROGRESS, IDC_SAVES, IDC_PLAY,
};
constexpr UINT WM_INSTALL_PROGRESS = WM_APP + 1, WM_INSTALL_DONE = WM_APP + 2;

const COLORREF kHeaderBg = RGB(14, 20, 34), kHeaderText = RGB(235, 242, 250), kHeaderSub = RGB(140, 170, 205);

struct WinSize {
    int w, h;
};
const WinSize kWinSizes[] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
const int kRenderHeights[] = {720, 1080, 1440, 2160};
const int kAniso[] = {1, 2, 4, 8, 16};

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_title_font = nullptr, g_sub_font = nullptr;
HICON g_icon = nullptr;
HBRUSH g_header_brush = nullptr;
int g_dpi = 96;
std::atomic<bool> g_installing{false};
std::wstring g_install_error;

int S(int v) { return MulDiv(v, g_dpi, 96); }

HWND ctl(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
    HWND h_ = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd,
                              (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h_, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h_;
}
HWND label(const wchar_t* text, int x, int y, int w, int id = -1) { return ctl(L"STATIC", text, 0, x, y + 3, w, 20, id); }
HWND combo(int x, int y, int w, int id, std::initializer_list<const wchar_t*> items, int sel) {
    HWND c = ctl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 200, id);
    for (auto* s : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)s);
    SendMessageW(c, CB_SETCURSEL, sel, 0);
    return c;
}
HWND check(const wchar_t* text, int x, int y, int w, int id, bool on) {
    HWND c = ctl(L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}
HWND item(int id) { return GetDlgItem(g_wnd, id); }
int sel(int id) { return (int)SendMessageW(item(id), CB_GETCURSEL, 0, 0); }
bool checked(int id) { return SendMessageW(item(id), BM_GETCHECK, 0, 0) == BST_CHECKED; }
int slider(int id) { return (int)SendMessageW(item(id), TBM_GETPOS, 0, 0); }

template <class T, size_t N>
int index_of(const T (&arr)[N], const T& v, int fallback) {
    for (size_t i = 0; i < N; i++)
        if (arr[i] == v) return (int)i;
    return fallback;
}

void update_volume_labels() {
    SetWindowTextW(item(IDC_MUSIC_VAL), (std::to_wstring(slider(IDC_MUSIC)) + L"%").c_str());
    SetWindowTextW(item(IDC_EFFECTS_VAL), (std::to_wstring(slider(IDC_EFFECTS)) + L"%").c_str());
}

void read_controls() {
    auto& s = settings::get();
    s.fullscreen = sel(IDC_MODE) == 1;
    int ws = sel(IDC_WINSIZE);
    if (ws >= 0) s.window_width = kWinSizes[ws].w, s.window_height = kWinSizes[ws].h;
    s.render_height = kRenderHeights[std::max(0, sel(IDC_RES))];
    s.max_fps = sel(IDC_FPS) == 1 ? 60 : 30;
    s.show_fps = checked(IDC_SHOWFPS);
    s.anti_aliasing = std::max(0, sel(IDC_AA));
    s.anisotropy = kAniso[std::max(0, sel(IDC_ANISO))];
    s.dynamic_shadows = checked(IDC_SHADOWS);
    s.high_res_shadows = checked(IDC_HISHADOWS);
    s.light_shafts = checked(IDC_SHAFTS);
    s.bloom = checked(IDC_BLOOM);
    s.depth_of_field = checked(IDC_DOF);
    s.music_volume = slider(IDC_MUSIC);
    s.effects_volume = slider(IDC_EFFECTS);
}

void set_status(const std::wstring& text) { SetWindowTextW(item(IDC_STATUS), text.c_str()); }

void refresh_install_state() {
    bool installed = game_installed();
    SetWindowTextW(item(IDC_PLAY), installed ? L"Play" : L"Install game...");
    if (installed) {
        std::string v = installed_version();
        std::wstring msg = L"Infinity Blade III " + widen(v.empty() ? "(unknown version)" : v) + L" is installed.";
        if (!v.empty() && v != "1.4.4") msg += L" This port was made for version 1.4.4; other versions may not work.";
        set_status(msg);
    } else {
        set_status(L"Game files not installed yet. Click \"Install game...\" and choose your Infinity Blade III .ipa.");
    }
}

// --- Key bindings window ----------------------------------------------------------------

struct KeysState {
    HWND wnd = nullptr, list = nullptr;
    std::vector<std::string> keys;  // per game::kActions entry
    int capturing = -1;
};
KeysState g_keys;

void keys_refresh_row(int i) {
    std::wstring key = i == g_keys.capturing ? L"Press a key..." : widen(game::key_display_name(g_keys.keys[i]));
    ListView_SetItemText(g_keys.list, i, 1, key.data());
}

void keys_assign(int i, const std::string& key) {
    // A key can only do one thing: the action that had it gets this action's old key.
    for (size_t j = 0; j < g_keys.keys.size(); j++) {
        if ((int)j != i && g_keys.keys[j] == key) {
            g_keys.keys[j] = g_keys.keys[i];
            keys_refresh_row((int)j);
        }
    }
    g_keys.keys[i] = key;
}

LRESULT CALLBACK keys_list_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (g_keys.capturing >= 0 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        int vk = (int)wp;
        if (vk == VK_SHIFT) vk = (int)MapVirtualKeyW((lp >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
        else if (vk == VK_CONTROL) vk = (lp >> 24) & 1 ? VK_RCONTROL : VK_LCONTROL;
        else if (vk == VK_MENU) vk = (lp >> 24) & 1 ? VK_RMENU : VK_LMENU;
        std::string key = game::key_name_for_vk(vk);
        if (key.empty()) return 0;  // unsupported key: keep waiting
        int i = g_keys.capturing;
        g_keys.capturing = -1;
        keys_assign(i, key);
        keys_refresh_row(i);
        return 0;
    }
    if (g_keys.capturing >= 0 && (msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_KEYUP || msg == WM_SYSKEYUP)) return 0;
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return DefSubclassProc(h, msg, wp, lp);
}

void keys_start_capture() {
    int i = ListView_GetNextItem(g_keys.list, -1, LVNI_SELECTED);
    if (i < 0) return;
    int prev = g_keys.capturing;
    g_keys.capturing = i;
    if (prev >= 0) keys_refresh_row(prev);
    keys_refresh_row(i);
    SetFocus(g_keys.list);
}

LRESULT CALLBACK keys_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 1:  // Change
            keys_start_capture();
            return 0;
        case 2:  // Reset
            g_keys.capturing = -1;
            for (size_t i = 0; i < std::size(game::kActions); i++) {
                g_keys.keys[i] = game::kActions[i].default_key;
                keys_refresh_row((int)i);
            }
            return 0;
        case 3:  // Done
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->hwndFrom == g_keys.list && nm->code == NM_DBLCLK) keys_start_capture();
        break;
    }
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        for (size_t i = 0; i < std::size(game::kActions); i++) settings::set_key(game::kActions[i].id, g_keys.keys[i]);
        EnableWindow(g_wnd, TRUE);
        SetForegroundWindow(g_wnd);
        g_keys.wnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void open_key_bindings() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = keys_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"IB3Keys";
        wc.hIcon = g_icon;
        RegisterClassW(&wc);
        registered = true;
    }
    g_keys.keys.clear();
    for (auto& a : game::kActions) g_keys.keys.push_back(settings::key_for(a.id, a.default_key));
    g_keys.capturing = -1;
    RECT r{0, 0, S(460), S(520)};
    AdjustWindowRectExForDpi(&r, WS_CAPTION | WS_SYSMENU, FALSE, 0, g_dpi);
    RECT pr;
    GetWindowRect(g_wnd, &pr);
    g_keys.wnd = CreateWindowExW(0, L"IB3Keys", L"Key bindings", WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                 pr.left + S(40), pr.top + S(40), r.right - r.left, r.bottom - r.top, g_wnd, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
    auto mk = [](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_keys.wnd,
                                 (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        return c;
    };
    mk(L"STATIC", L"Select an action and click Change (or double-click it), then press the new key.", 0, 12, 10, 436,
       20, -1);
    g_keys.list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 12,
                     36, 436, 420, 10);
    ListView_SetExtendedListViewStyle(g_keys.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.cx = S(270);
    col.pszText = (LPWSTR)L"Action";
    ListView_InsertColumn(g_keys.list, 0, &col);
    col.cx = S(140);
    col.pszText = (LPWSTR)L"Key";
    ListView_InsertColumn(g_keys.list, 1, &col);
    for (size_t i = 0; i < std::size(game::kActions); i++) {
        std::wstring label_text = widen(game::kActions[i].label);
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = label_text.data();
        ListView_InsertItem(g_keys.list, &it);
        keys_refresh_row((int)i);
    }
    SetWindowSubclass(g_keys.list, keys_list_proc, 1, 0);
    mk(L"BUTTON", L"Change", BS_PUSHBUTTON | WS_TABSTOP, 12, 468, 100, 30, 1);
    mk(L"BUTTON", L"Reset to defaults", BS_PUSHBUTTON | WS_TABSTOP, 120, 468, 140, 30, 2);
    mk(L"BUTTON", L"Done", BS_DEFPUSHBUTTON | WS_TABSTOP, 348, 468, 100, 30, 3);
    EnableWindow(g_wnd, FALSE);
}

// --- Install / play -------------------------------------------------------------------------

std::wstring find_ipa_next_to_exe() {
    std::vector<std::wstring> found;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((settings::exe_dir() + L"*.ipa").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do found.push_back(settings::exe_dir() + fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return found.size() == 1 ? found[0] : L"";
}

std::wstring choose_ipa() {
    std::wstring ipa = find_ipa_next_to_exe();
    if (!ipa.empty()) {
        std::wstring q = L"Install Infinity Blade III from\n" + ipa + L" ?";
        int r = MessageBoxW(g_wnd, q.c_str(), L"Install game", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDYES) return ipa;
        if (r == IDCANCEL) return L"";
    }
    wchar_t file[MAX_PATH * 2] = {};
    std::wstring dir = settings::exe_dir();
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Infinity Blade III app (*.ipa)\0*.ipa\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.lpstrInitialDir = dir.c_str();
    ofn.lpstrTitle = L"Choose your Infinity Blade III .ipa";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn) ? file : L"";
}

void set_busy(bool busy) {
    for (int id : {IDC_PLAY, IDC_SAVES, IDC_KEYS}) EnableWindow(item(id), !busy);
    ShowWindow(item(IDC_PROGRESS), busy ? SW_SHOW : SW_HIDE);
}

void start_install() {
    std::wstring ipa = choose_ipa();
    if (ipa.empty()) return;
    g_installing = true;
    set_busy(true);
    SendMessageW(item(IDC_PROGRESS), PBM_SETPOS, 0, 0);
    set_status(L"Installing game files from the .ipa... (this takes a minute)");
    std::thread([ipa] {
        std::wstring error;
        bool ok = install_from_ipa(ipa, [](double f) { PostMessageW(g_wnd, WM_INSTALL_PROGRESS, (WPARAM)(f * 1000), 0); },
                                   error);
        g_install_error = error;
        PostMessageW(g_wnd, WM_INSTALL_DONE, ok, 0);
    }).detach();
}

void play() {
    read_controls();
    settings::save();
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" -play";
    std::wstring dir = settings::exe_dir();
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        MessageBoxW(g_wnd, L"Could not start the game.", L"Infinity Blade III", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    DestroyWindow(g_wnd);
}

void open_saves() {
    std::wstring dir = settings::exe_dir() + L"userdata\\Documents\\SAVE";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    ShellExecuteW(g_wnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// --- Main window --------------------------------------------------------------------------

constexpr int kWidth = 640, kHeaderH = 84;

void create_controls() {
    const auto& s = settings::get();
    int L = 20, R = 336, y0 = kHeaderH + 16;
    // Display
    ctl(L"BUTTON", L"Display", BS_GROUPBOX, L - 8, y0, 300, 200, -1);
    int y = y0 + 26;
    label(L"Window mode", L, y, 110);
    combo(L + 120, y, 160, IDC_MODE, {L"Windowed", L"Fullscreen"}, s.fullscreen ? 1 : 0);
    y += 32;
    label(L"Window size", L, y, 110);
    int ws = 0;
    for (int i = 0; i < (int)std::size(kWinSizes); i++)
        if (kWinSizes[i].w == s.window_width && kWinSizes[i].h == s.window_height) ws = i;
    combo(L + 120, y, 160, IDC_WINSIZE, {L"1280 x 720", L"1600 x 900", L"1920 x 1080", L"2560 x 1440"}, ws);
    y += 32;
    label(L"Resolution", L, y, 110);
    combo(L + 120, y, 160, IDC_RES, {L"720p", L"1080p (original)", L"1440p", L"4K (2160p)"},
          index_of(kRenderHeights, s.render_height, 1));
    y += 32;
    label(L"Frame rate", L, y, 110);
    combo(L + 120, y, 160, IDC_FPS, {L"30 FPS (original)", L"60 FPS (experimental)"}, s.max_fps >= 60 ? 1 : 0);
    y += 32;
    check(L"Show FPS in the title bar", L, y, 260, IDC_SHOWFPS, s.show_fps);

    // Audio
    int ya = y0 + 212;
    ctl(L"BUTTON", L"Audio", BS_GROUPBOX, L - 8, ya, 300, 96, -1);
    y = ya + 26;
    label(L"Music", L, y, 80);
    HWND m = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, L + 80, y, 160, 26, IDC_MUSIC);
    SendMessageW(m, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(m, TBM_SETPOS, TRUE, s.music_volume);
    label(L"", L + 244, y, 44, IDC_MUSIC_VAL);
    y += 32;
    label(L"Effects", L, y, 80);
    HWND e = ctl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, L + 80, y, 160, 26, IDC_EFFECTS);
    SendMessageW(e, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(e, TBM_SETPOS, TRUE, s.effects_volume);
    label(L"", L + 244, y, 44, IDC_EFFECTS_VAL);
    update_volume_labels();

    // Graphics
    ctl(L"BUTTON", L"Graphics", BS_GROUPBOX, R - 8, y0, 300, 236, -1);
    y = y0 + 26;
    label(L"Anti-aliasing", R, y, 110);
    combo(R + 120, y, 160, IDC_AA, {L"Off", L"FXAA (original)", L"MSAA 4x"}, s.anti_aliasing);
    y += 32;
    label(L"Texture filtering", R, y, 110);
    combo(R + 120, y, 160, IDC_ANISO, {L"1x", L"2x", L"4x", L"8x", L"16x"}, index_of(kAniso, s.anisotropy, 2));
    y += 34;
    check(L"Dynamic shadows", R, y, 280, IDC_SHADOWS, s.dynamic_shadows);
    y += 26;
    check(L"High-resolution shadows", R, y, 280, IDC_HISHADOWS, s.high_res_shadows);
    y += 26;
    check(L"Light shafts (god rays)", R, y, 280, IDC_SHAFTS, s.light_shafts);
    y += 26;
    check(L"Bloom", R, y, 280, IDC_BLOOM, s.bloom);
    y += 26;
    check(L"Depth of field", R, y, 280, IDC_DOF, s.depth_of_field);

    // Controls
    int yc = y0 + 248;
    ctl(L"BUTTON", L"Controls", BS_GROUPBOX, R - 8, yc, 300, 60, -1);
    ctl(L"BUTTON", L"Key bindings...", BS_PUSHBUTTON | WS_TABSTOP, R, yc + 22, 140, 28, IDC_KEYS);
    label(L"Mouse = touch", R + 152, yc + 24, 130);

    // Bottom
    int yb = y0 + 324;
    ctl(L"STATIC", L"", SS_LEFT, L - 8, yb, kWidth - 24, 36, IDC_STATUS);
    HWND pb = ctl(PROGRESS_CLASSW, L"", 0, L - 8, yb + 40, kWidth - 24, 16, IDC_PROGRESS);
    SendMessageW(pb, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
    ShowWindow(pb, SW_HIDE);
    ctl(L"BUTTON", L"Open save folder", BS_PUSHBUTTON | WS_TABSTOP, L - 8, yb + 64, 140, 34, IDC_SAVES);
    ctl(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, kWidth - 16 - 180, yb + 64, 180, 34, IDC_PLAY);
    refresh_install_state();
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PLAY:
            if (g_installing) return 0;
            if (game_installed()) play();
            else start_install();
            return 0;
        case IDC_KEYS:
            open_key_bindings();
            return 0;
        case IDC_SAVES:
            open_saves();
            return 0;
        }
        break;
    case WM_HSCROLL:
        update_volume_labels();
        return 0;
    case WM_INSTALL_PROGRESS:
        SendMessageW(item(IDC_PROGRESS), PBM_SETPOS, wp, 0);
        return 0;
    case WM_INSTALL_DONE:
        g_installing = false;
        set_busy(false);
        if (wp) {
            refresh_install_state();
            MessageBoxW(h, L"Infinity Blade III is installed. Click Play to start.", L"Infinity Blade III",
                        MB_ICONINFORMATION);
        } else {
            refresh_install_state();
            MessageBoxW(h, (L"Installing failed:\n" + g_install_error).c_str(), L"Infinity Blade III", MB_ICONERROR);
        }
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        RECT header{0, 0, rc.right, S(kHeaderH)};
        FillRect(dc, &header, g_header_brush);
        DrawIconEx(dc, S(18), S(14), g_icon, S(56), S(56), 0, nullptr, DI_NORMAL);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kHeaderText);
        HGDIOBJ old = SelectObject(dc, g_title_font);
        TextOutW(dc, S(88), S(14), L"Infinity Blade III", 18);
        SelectObject(dc, g_sub_font);
        SetTextColor(dc, kHeaderSub);
        const wchar_t* sub = L"PC Port";
        TextOutW(dc, S(90), S(50), sub, (int)wcslen(sub));
        SelectObject(dc, old);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (g_installing) {
            MessageBoxW(h, L"Please wait until the game files are installed.", L"Infinity Blade III", MB_ICONINFORMATION);
            return 0;
        }
        read_controls();
        settings::save();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HFONT make_font(int pt, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

}  // namespace

void create_main_window(int x, int y, bool offscreen);

int run() {
    create_main_window(-1, -1, false);
    ShowWindow(g_wnd, SW_SHOW);
    SetFocus(item(IDC_PLAY));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        HWND dlg = g_keys.wnd && IsChild(g_keys.wnd, msg.hwnd) ? g_keys.wnd : g_wnd;
        // Tab / Enter navigation, except while the key-binding list is waiting for a key.
        if (g_keys.capturing < 0 && IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

namespace {
void pump() {
    MSG msg;
    for (int i = 0; i < 50; i++) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(10);
    }
}
void capture(HWND h, const char* png) {
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, hh);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(h, mem, 2 /*PW_RENDERFULLCONTENT*/);
    std::vector<u8> px((size_t)w * hh * 4);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -hh, 1, 32, BI_RGB};
    GetDIBits(mem, bmp, 0, hh, px.data(), &bi, DIB_RGB_COLORS);
    for (size_t i = 0; i < px.size(); i += 4) {
        std::swap(px[i], px[i + 2]);
        px[i + 3] = 255;
    }
    gles::write_png(png, px.data(), w, hh);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}
}  // namespace

int screenshot(const char* main_png, const char* keys_png) {
    create_main_window(-4000, -4000, true);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    pump();
    capture(g_wnd, main_png);
    open_key_bindings();
    SetWindowPos(g_keys.wnd, nullptr, -3000, -3000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    pump();
    capture(g_keys.wnd, keys_png);
    return 0;
}

void create_main_window(int x, int y, bool offscreen) {
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);
    settings::load();
    g_dpi = (int)GetDpiForSystem();
    g_font = make_font(9, FW_NORMAL, L"Segoe UI");
    g_title_font = make_font(20, FW_SEMIBOLD, L"Segoe UI");
    g_sub_font = make_font(10, FW_NORMAL, L"Segoe UI");
    g_header_brush = CreateSolidBrush(kHeaderBg);
    g_icon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, S(64), S(64), LR_DEFAULTCOLOR);

    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"IB3Launcher";
    wc.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(kWidth), S(kHeaderH + 16 + 324 + 110)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    if (x == -1) x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_wnd = CreateWindowExW(offscreen ? WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE : 0, L"IB3Launcher", L"Infinity Blade III",
                            style, x, y, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    create_controls();
}

}  // namespace launcher
