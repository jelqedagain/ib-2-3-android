// iOS alerts on Android: not shown yet; each alert is answered as if dismissed (logged).
#include "win/dialogs.h"
#include "libc/format.h"

namespace win {

int choose(HWND, const std::wstring& title, const std::wstring& message, const std::vector<std::wstring>& buttons,
           int cancel_index) {
    int pick = cancel_index >= 0 ? cancel_index : (int)buttons.size() - 1;
    LOG_INFO("alert \"%s\": %s -> %s", libc::wide_to_utf8(title).c_str(), libc::wide_to_utf8(message).c_str(),
             pick >= 0 && pick < (int)buttons.size() ? libc::wide_to_utf8(buttons[pick]).c_str() : "(none)");
    return pick;
}

bool prompt_text(HWND, const std::wstring& title, const std::wstring&, std::wstring&, bool, const std::wstring&,
                 const std::wstring&) {
    LOG_INFO("text prompt \"%s\": cancelled", libc::wide_to_utf8(title).c_str());
    return false;
}

}  // namespace win
