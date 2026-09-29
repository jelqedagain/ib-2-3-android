// Controller support on Android: not implemented yet (the Windows build uses XInput, game/controller.cpp).
#include "game/game.h"

namespace game {

void start_controller() {}
void draw_controller_overlay(int, int) {}
void set_fake_pad(const std::string&, float) {}

}  // namespace game
