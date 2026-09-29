// UILabel text rendering (used for movie subtitles) and UIKit string measurement.
#pragma once
#include "hle.h"
#include "objc/runtime.h"
#include <string>
#include <vector>

namespace uikit {

struct ColorData : objc::HostData {
    double r = 0, g = 0, b = 0, a = 1;
};
struct FontData : objc::HostData {
    double size = 17;
    bool bold = false;
};
struct LabelData : objc::HostData {
    objc::id text = 0;
    double font_size = 17;
    bool bold = false;
    ColorData color{};  // defaults to black like UIKit
    bool has_shadow = false;
    ColorData shadow{};
    double shadow_dx = 0, shadow_dy = -1;
    s64 alignment = 0;  // 0 left, 1 center, 2 right
    s64 lines = 1;      // 0 = unlimited
};

// A label as it appears on screen (window coordinates in points).
struct LabelSnapshot {
    std::u16string text;
    CGRect frame;
    LabelData style;
};

// Measures text like -[NSString sizeWithFont:constrainedToSize:] (points).
CGSize measure_text(const std::u16string& text, double font_size, bool bold, double max_width);

// Latest snapshot of visible labels (updated on the main thread ~30 times a second).
std::vector<LabelSnapshot> visible_labels(u64& version);

// Draws labels onto an RGBA image of the whole screen (w x h pixels, top row first).
void draw_labels(u8* rgba, int w, int h, const std::vector<LabelSnapshot>& labels);

void install_labels();

}  // namespace uikit
