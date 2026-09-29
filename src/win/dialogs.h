// Native dialogs standing in for iOS alerts.
#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <windows.h>

namespace win {

// Shows a message with the given buttons (in order); returns the index of the button chosen.
// Escape / closing the dialog picks `cancel_index` (or the last button if it is -1).
int choose(HWND owner, const std::wstring& title, const std::wstring& message, const std::vector<std::wstring>& buttons,
           int cancel_index);

// Asks for a line of text. Returns false if cancelled; `text` holds the initial and final value.
bool prompt_text(HWND owner, const std::wstring& title, const std::wstring& message, std::wstring& text, bool password,
                 const std::wstring& ok_label, const std::wstring& cancel_label);

}  // namespace win
