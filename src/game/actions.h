// Keyboard actions, shared by the game (keyboard.cpp) and the launcher's key-binding editor.
#pragma once
#include <string>

namespace game {

struct Action {
    const char* id;           // settings.ini [Controls] key
    const char* label;        // shown in the launcher
    const char* default_key;  // Unreal key name (IB2 PC layout)
    // IB3 input name sent to Unreal (bound in IB3's PlayerInput config), or an @action
    // implemented by the port.
    const char* input;
};

inline constexpr Action kActions[] = {
    {"DodgeLeft", "Dodge left", "A", "Sword_BttnLeftDodge"},
    {"DodgeRight", "Dodge right", "D", "Sword_BttnRightDodge"},
    {"Block", "Block (hold)", "S", "Sword_BttnBlock"},
    {"Stab", "Stab", "F", "Five"},  // SwordStab is only bound to a raw key in IB3
    {"SuperMove", "Super move", "Q", "SwordActivateSuperMove"},
    {"Magic", "Magic", "E", "Sword_MagicMode"},
    {"Magic1", "Magic slot 1", "One", "MagicSlot0"},
    {"Magic2", "Magic slot 2", "Two", "MagicSlot1"},
    {"Magic3", "Magic slot 3", "Three", "MagicSlot2"},
    {"ClashMash", "Clash (mash)", "LeftAlt", "Sword_SwordClashMashTap"},
    {"BossInfo", "Boss info", "Tab", "Sword_BossInfo"},
    {"FastForward", "Fast-forward cutscene (hold)", "LeftShift", "Sword_FastForward"},
    {"Accept", "Accept prompt", "Enter", "@AcceptPrompt"},
    {"Back", "Back / quit", "Escape", "@Escape"},
    {"Menu", "Menu", "P", "Sword_PauseGame"},
    {"Pause", "Pause toggle", "SpaceBar", "SpaceBar"},  // PauseGameTestToggle
};

// Unreal key name for a Windows virtual-key code ("" if unsupported).
std::string key_name_for_vk(int vk);
// Readable form of a key name ("LeftShift" -> "Left Shift").
std::string key_display_name(const std::string& key);

}  // namespace game
