// The platform side of controller support (game/controller.cpp): reading the pad, and the text
// of the controls legend. XInput on Windows (game/pad_xinput.cpp), Android's gamepad input events
// on Android (port/android/pad.cpp).
#pragma once
#include "common.h"
#include <vector>

namespace game::pad {

// Button bits (XInput's values).
enum : u16 {
    kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
    kStart = 0x0010, kBack = 0x0020, kLeftThumb = 0x0040, kRightThumb = 0x0080,
    kLeftShoulder = 0x0100, kRightShoulder = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};

struct State {
    bool connected = false;
    u16 buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, up is positive
    float lt = 0, rt = 0;                  // 0..1
};

bool init();                  // false if controllers cannot be read on this system
void read(State& s);          // the first connected pad (s.connected stays false if none)
bool game_in_foreground();    // controllers only drive the game while it is in front

// The controls legend: a dark panel with these lines, as RGBA (top row first).
void render_legend(const std::vector<std::string>& lines, std::vector<u8>& rgba, int& w, int& h);
extern const char* const kBackName;   // what the pad's Back / Select button is called
extern const char* const kStartName;

}  // namespace game::pad
