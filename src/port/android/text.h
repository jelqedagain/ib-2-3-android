// Text rendering helpers for Android (port/android/text.cpp).
#pragma once
#include "common.h"
#include <string>
#include <vector>

namespace uikit {

// A dark panel with lines of UTF-8 text at `px` pixels (the controller legend), as RGBA.
void render_text_panel(const std::vector<std::string>& lines, float px, std::vector<u8>& rgba, int& w, int& h);

}  // namespace uikit
