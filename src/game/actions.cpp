#include "game/actions.h"
#include <cctype>
#include <windows.h>

namespace game {

std::string key_name_for_vk(int vk) {
    if (vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
    static const char* digits[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
    if (vk >= '0' && vk <= '9') return digits[vk - '0'];
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::string("NumPad") + digits[vk - VK_NUMPAD0];
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    switch (vk) {
    case VK_SPACE: return "SpaceBar";
    case VK_RETURN: return "Enter";
    case VK_ESCAPE: return "Escape";
    case VK_TAB: return "Tab";
    case VK_BACK: return "BackSpace";
    case VK_SHIFT:
    case VK_LSHIFT: return "LeftShift";
    case VK_RSHIFT: return "RightShift";
    case VK_CONTROL:
    case VK_LCONTROL: return "LeftControl";
    case VK_RCONTROL: return "RightControl";
    case VK_MENU:
    case VK_LMENU: return "LeftAlt";
    case VK_RMENU: return "RightAlt";
    case VK_LEFT: return "Left";
    case VK_RIGHT: return "Right";
    case VK_UP: return "Up";
    case VK_DOWN: return "Down";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_OEM_3: return "Tilde";
    case VK_OEM_COMMA: return "Comma";
    case VK_OEM_PERIOD: return "Period";
    case VK_OEM_2: return "Slash";
    case VK_OEM_1: return "Semicolon";
    case VK_OEM_7: return "Quote";
    case VK_OEM_4: return "LeftBracket";
    case VK_OEM_6: return "RightBracket";
    case VK_OEM_MINUS: return "Underscore";
    case VK_OEM_PLUS: return "Equals";
    default: return {};
    }
}

std::string key_display_name(const std::string& key) {
    if (key.empty()) return "(none)";
    if (key == "SpaceBar") return "Space";
    static const char* digits[] = {"Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine"};
    for (int d = 0; d < 10; d++) {
        if (key == digits[d]) return std::to_string(d);
        if (key == std::string("NumPad") + digits[d]) return "Numpad " + std::to_string(d);
    }
    std::string out;
    for (size_t i = 0; i < key.size(); i++) {
        // "LeftShift" -> "Left Shift", "NumPadOne" -> "Num Pad One"; keep "F11" together.
        if (i > 0 && isupper((unsigned char)key[i]) && !isupper((unsigned char)key[i - 1])) out += ' ';
        out += key[i];
    }
    return out;
}

}  // namespace game
