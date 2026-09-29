// Keyboard actions, shared by the game (keyboard.cpp) and the launcher's key-binding editor.
#pragma once
#include <string>

namespace game {

struct Action {
    const char* id;           // settings.ini [Controls] key
    const char* label;        // shown in the launcher
    const char* default_key;  // Unreal key name (IB2 PC layout)
    // IB3 input name sent to Unreal (bound in IB3's PlayerInput config), or an @action
    // implemented by the port. "a|b": the first one the current HUD has a touch zone for.
    const char* input;
};

// The three fight buttons change with the weapon (IB3's HeroBattleHud_* input groups):
//   sword and shield: dodge left | block      | dodge right
//   dual blades:      dodge left | dodge down | dodge right
//   heavy (2-handed): block left | block      | block right
// so the keys follow the button in the same place.
inline constexpr Action kActions[] = {
    {"DodgeLeft", "Left button (dodge / block left)", "A", "Sword_BttnLeftDodge|Sword_BttnBlockLeft"},
    {"DodgeRight", "Right button (dodge / block right)", "D", "Sword_BttnRightDodge|Sword_BttnBlockRight"},
    {"Block", "Center button (block / dodge down, hold)", "S", "Sword_BttnBlock|Sword_BttnCenterDodge"},
    {"Stab", "Stab", "F", "Five"},  // SwordStab is only bound to a raw key in IB3
    // Boss battles (BossBattleHud / BossRecoveryHud) have their own super move and special
    // attack buttons in place of super move and magic.
    {"SuperMove", "Super move", "Q", "SwordActivateSuperMove|Sword_BossActivateSuperMove"},
    {"Magic", "Magic (boss special attack)", "E", "Sword_MagicMode|Sword_BossActivateSpecialAttack"},
    {"FinalStrike", "Final strike", "R", "Sword_FinalStrikeMode"},  // FinalStrikeHud
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
