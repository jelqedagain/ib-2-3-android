// Image decoding through the Windows Imaging Component (PNG/JPEG on the host side).
#pragma once
#include "common.h"
#include <string>
#include <vector>
#include <windows.h>

namespace win {

// Decodes an image file into top-down RGBA8.
bool load_image_rgba(const std::wstring& path, std::vector<u8>& rgba, int& w, int& h);

// Builds a window icon from an image file (nullptr on failure).
HICON load_icon(const std::wstring& path);

}  // namespace win
