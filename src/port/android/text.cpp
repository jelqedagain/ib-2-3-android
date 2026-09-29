// UILabel text measurement and rendering for Android, with stb_truetype and the system's Roboto
// (the Windows build uses GDI in uikit/labels.cpp).
#include "uikit/labels.h"
#include "uikit/uikit.h"
#include <cmath>
#include <cstdio>
#include <mutex>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb/stb_truetype.h>

namespace uikit {

namespace {

struct Font {
    std::vector<u8> data;
    stbtt_fontinfo info{};
    bool ok = false;
};

bool load_font(Font& f, std::initializer_list<const char*> paths) {
    for (const char* p : paths) {
        FILE* fp = std::fopen(p, "rb");
        if (!fp) continue;
        std::fseek(fp, 0, SEEK_END);
        long n = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        f.data.resize(n > 0 ? (size_t)n : 0);
        bool read = n > 0 && std::fread(f.data.data(), 1, f.data.size(), fp) == f.data.size();
        std::fclose(fp);
        if (read && stbtt_InitFont(&f.info, f.data.data(), stbtt_GetFontOffsetForIndex(f.data.data(), 0))) {
            f.ok = true;
            return true;
        }
    }
    return false;
}

std::mutex g_font_mutex;

// Returns the regular or bold font; `fake_bold` is set when bold must be synthesized.
const Font* font(bool bold, bool& fake_bold) {
    static Font regular, heavy;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        load_font(regular, {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf", "/system/fonts/DroidSans.ttf"});
        load_font(heavy, {"/system/fonts/DroidSans-Bold.ttf", "/system/fonts/Roboto-Bold.ttf"});
        if (!regular.ok) LOG_WARN("labels: no system font found; text will not be drawn");
    }
    fake_bold = bold && !heavy.ok;
    return bold && heavy.ok ? &heavy : regular.ok ? &regular : nullptr;
}

std::u32string to_codepoints(const std::u16string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        out += c;
    }
    return out;
}

struct Layout {
    std::vector<std::u32string> lines;
    std::vector<float> widths;
    float line_height = 0, ascent = 0, scale = 0, width = 0;
};

float advance(const Font* f, float scale, char32_t c, char32_t next) {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&f->info, (int)c, &adv, &lsb);
    float a = adv * scale;
    if (next) a += stbtt_GetCodepointKernAdvance(&f->info, (int)c, (int)next) * scale;
    return a;
}

float line_width(const Font* f, float scale, const std::u32string& s) {
    float w = 0;
    for (size_t i = 0; i < s.size(); i++) w += advance(f, scale, s[i], i + 1 < s.size() ? s[i + 1] : 0);
    return w;
}

// Word-wraps `text` to `max_width` pixels (0 = no limit), like DrawText's DT_WORDBREAK.
Layout layout(const Font* f, const std::u32string& text, float px, float max_width, bool single_line) {
    Layout l;
    l.scale = stbtt_ScaleForMappingEmToPixels(&f->info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    l.ascent = asc * l.scale;
    l.line_height = (asc - desc + gap) * l.scale;
    auto push = [&](const std::u32string& s) {
        l.lines.push_back(s);
        l.widths.push_back(line_width(f, l.scale, s));
        l.width = std::max(l.width, l.widths.back());
    };
    std::u32string para;
    auto flush_paragraph = [&]() {
        if (single_line || max_width <= 0) {
            push(para);
            return;
        }
        std::u32string line;
        size_t i = 0;
        while (i <= para.size()) {
            size_t j = para.find(U' ', i);
            if (j == std::u32string::npos) j = para.size();
            std::u32string word = para.substr(i, j - i);
            std::u32string candidate = line.empty() ? word : line + U' ' + word;
            if (!line.empty() && line_width(f, l.scale, candidate) > max_width) {
                push(line);
                line = word;
            } else {
                line = candidate;
            }
            i = j + 1;
        }
        push(line);
    };
    for (char32_t c : text) {
        if (c == U'\n' && !single_line) {
            flush_paragraph();
            para.clear();
        } else {
            para += c == U'\n' ? U' ' : c;
        }
    }
    flush_paragraph();
    return l;
}

}  // namespace

CGSize measure_text(const std::u16string& text, double font_size, bool bold, double max_width) {
    std::lock_guard lock(g_font_mutex);
    bool fake_bold = false;
    const Font* f = font(bold, fake_bold);
    if (!f) return CGSize{text.size() * font_size * 0.5, font_size * 1.2};
    Layout l = layout(f, to_codepoints(text), (float)font_size, (float)max_width, false);
    return CGSize{std::ceil(l.width + (fake_bold ? 1 : 0)), std::ceil(l.line_height * l.lines.size())};
}

void draw_labels(u8* rgba, int w, int h, const std::vector<LabelSnapshot>& labels) {
    if (labels.empty()) return;
    std::lock_guard lock(g_font_mutex);
    double sx = w / g_device.width_pt, sy = h / g_device.height_pt;
    for (auto& lab : labels) {
        bool fake_bold = false;
        const Font* f = font(lab.style.bold, fake_bold);
        if (!f) return;
        float left = (float)(lab.frame.origin.x * sx), top = (float)(lab.frame.origin.y * sy);
        float box_w = (float)(lab.frame.size.width * sx), box_h = (float)(lab.frame.size.height * sy);
        Layout l = layout(f, to_codepoints(lab.text), (float)(lab.style.font_size * sy), box_w, lab.style.lines == 1);
        float text_h = l.line_height * l.lines.size();
        float y0 = top + std::max(0.0f, (box_h - text_h) / 2);  // UILabel centers vertically
        if (y0 + text_h > h - 2) y0 = std::max(0.0f, h - 2 - text_h);  // keep on screen

        auto pass = [&](float dx, float dy, const ColorData& c) {
            for (size_t li = 0; li < l.lines.size(); li++) {
                const std::u32string& s = l.lines[li];
                float x = left + dx;
                if (lab.style.alignment == 1) x += (box_w - l.widths[li]) / 2;
                else if (lab.style.alignment == 2) x += box_w - l.widths[li];
                float baseline = y0 + dy + l.ascent + l.line_height * li;
                for (size_t i = 0; i < s.size(); i++) {
                    int gx0, gy0, gx1, gy1;
                    float fx = x - std::floor(x);
                    stbtt_GetCodepointBitmapBoxSubpixel(&f->info, (int)s[i], l.scale, l.scale, fx, 0, &gx0, &gy0, &gx1, &gy1);
                    int gw = gx1 - gx0, gh = gy1 - gy0;
                    if (gw > 0 && gh > 0) {
                        std::vector<u8> cov((size_t)gw * gh);
                        stbtt_MakeCodepointBitmapSubpixel(&f->info, cov.data(), gw, gh, gw, l.scale, l.scale, fx, 0, (int)s[i]);
                        int ox = (int)std::floor(x) + gx0, oy = (int)std::lround(baseline) + gy0;
                        for (int bold_dx = 0; bold_dx <= (fake_bold ? 1 : 0); bold_dx++)
                            for (int yy = 0; yy < gh; yy++) {
                                int py = oy + yy;
                                if (py < 0 || py >= h) continue;
                                for (int xx = 0; xx < gw; xx++) {
                                    int px = ox + xx + bold_dx;
                                    u8 a8 = cov[(size_t)yy * gw + xx];
                                    if (!a8 || px < 0 || px >= w) continue;
                                    float a = a8 / 255.0f * (float)c.a;
                                    u8* d = rgba + ((size_t)py * w + px) * 4;
                                    d[0] = (u8)(d[0] * (1 - a) + c.r * 255 * a);
                                    d[1] = (u8)(d[1] * (1 - a) + c.g * 255 * a);
                                    d[2] = (u8)(d[2] * (1 - a) + c.b * 255 * a);
                                }
                            }
                    }
                    x += advance(f, l.scale, s[i], i + 1 < s.size() ? s[i + 1] : 0);
                }
            }
        };
        if (lab.style.has_shadow) pass((float)(lab.style.shadow_dx * sx), (float)(lab.style.shadow_dy * sy), lab.style.shadow);
        pass(0, 0, lab.style.color);
    }
}

}  // namespace uikit
