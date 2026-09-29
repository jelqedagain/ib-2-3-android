#ifndef UNICODE
#define UNICODE
#endif
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include "win/dialogs.h"
#include <commctrl.h>

namespace win {

int choose(HWND owner, const std::wstring& title, const std::wstring& message, const std::vector<std::wstring>& buttons,
           int cancel_index) {
    std::vector<TASKDIALOG_BUTTON> tb;
    for (size_t i = 0; i < buttons.size(); i++) tb.push_back({100 + (int)i, buttons[i].c_str()});
    if (tb.empty()) tb.push_back({100, L"OK"});
    TASKDIALOGCONFIG cfg{sizeof cfg};
    cfg.hwndParent = owner;
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    cfg.pszWindowTitle = L"Infinity Blade III";
    cfg.pszMainInstruction = title.c_str();
    cfg.pszContent = message.empty() ? nullptr : message.c_str();
    cfg.cButtons = (UINT)tb.size();
    cfg.pButtons = tb.data();
    cfg.nDefaultButton = 100;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)) || pressed == IDCANCEL)
        return cancel_index >= 0 ? cancel_index : (int)tb.size() - 1;
    return pressed - 100;
}

namespace {

struct Prompt {
    HWND edit = nullptr;
    bool done = false, ok = false;
    std::wstring text;
};

LRESULT CALLBACK prompt_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* p = reinterpret_cast<Prompt*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            p->ok = LOWORD(wp) == IDOK;
            int n = GetWindowTextLengthW(p->edit);
            p->text.assign(n, L'\0');
            GetWindowTextW(p->edit, p->text.data(), n + 1);
            p->done = true;
            return 0;
        }
        break;
    case WM_CLOSE:
        p->done = true;
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool prompt_text(HWND owner, const std::wstring& title, const std::wstring& message, std::wstring& text, bool password,
                 const std::wstring& ok_label, const std::wstring& cancel_label) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = prompt_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"IB3Prompt";
        RegisterClassW(&wc);
        registered = true;
    }
    int dpi = owner ? (int)GetDpiForWindow(owner) : (int)GetDpiForSystem();
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    NONCLIENTMETRICSW ncm{sizeof ncm};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    RECT r{0, 0, S(420), S(message.empty() ? 120 : 160)};
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
    RECT o{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (owner) GetWindowRect(owner, &o);
    int w = r.right - r.left, h = r.bottom - r.top;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"IB3Prompt", title.c_str(), style,
                               (o.left + o.right - w) / 2, (o.top + o.bottom - h) / 2, w, h, owner, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    Prompt p;
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&p);
    auto mk = [&](const wchar_t* cls, const wchar_t* t, DWORD st, int x, int y, int cw, int chh, int id) {
        HWND c = CreateWindowExW(cls == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, t, WS_CHILD | WS_VISIBLE | st,
                                 S(x), S(y), S(cw), S(chh), dlg, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
        return c;
    };
    int y = 14;
    if (!message.empty()) {
        mk(L"STATIC", message.c_str(), 0, 16, y, 388, 40, -1);
        y += 44;
    }
    p.edit = mk(L"EDIT", text.c_str(), ES_AUTOHSCROLL | WS_TABSTOP | (password ? ES_PASSWORD : 0), 16, y, 388, 26, 10);
    y += 40;
    mk(L"BUTTON", cancel_label.c_str(), BS_PUSHBUTTON | WS_TABSTOP, 212, y, 92, 30, IDCANCEL);
    mk(L"BUTTON", ok_label.c_str(), BS_DEFPUSHBUTTON | WS_TABSTOP, 312, y, 92, 30, IDOK);
    SendMessageW(p.edit, EM_SETSEL, 0, -1);

    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    SetFocus(p.edit);
    MSG msg;
    while (!p.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(dlg);
    DeleteObject(font);
    if (owner) SetForegroundWindow(owner);
    if (p.ok) text = p.text;
    return p.ok;
}

}  // namespace win
