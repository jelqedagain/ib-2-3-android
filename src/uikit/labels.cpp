#include "uikit/labels.h"
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "uikit/uikit.h"
#include <cmath>
#include <mutex>
#include <windows.h>

namespace uikit {

// Implemented in uikit.cpp: every visible UILabel under the key window with its window frame.
void collect_labels(std::vector<std::pair<objc::id, CGRect>>& out);

namespace {

std::mutex g_snap_mutex;
std::vector<LabelSnapshot> g_snapshot;
u64 g_snapshot_version = 0;

HFONT make_font(int px, bool bold) {
    return CreateFontW(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Arial");  // metrics close to iOS Helvetica
}

void refresh_snapshot() {
    std::vector<std::pair<objc::id, CGRect>> found;
    collect_labels(found);
    std::vector<LabelSnapshot> snap;
    for (auto& [label, frame] : found) {
        auto* d = objc::get<LabelData>(label);
        if (!d || !d->text) continue;
        LabelSnapshot s;
        s.text = ns::utf16(d->text);
        if (s.text.empty()) continue;
        s.frame = frame;
        s.style = *d;
        snap.push_back(std::move(s));
    }
    std::lock_guard lock(g_snap_mutex);
    bool changed = snap.size() != g_snapshot.size();
    for (size_t i = 0; !changed && i < snap.size(); i++)
        changed = snap[i].text != g_snapshot[i].text || std::memcmp(&snap[i].frame, &g_snapshot[i].frame, sizeof(CGRect)) != 0;
    if (changed) {
        LOG_DEBUG("labels: %zu visible (found %zu label views)", snap.size(), found.size());
        for (auto& l : snap)
            LOG_DEBUG("  label '%s' at (%.0f,%.0f %.0fx%.0f) size %.1f", ns::utf8(ns::str16_retained(l.text)).c_str(), l.frame.origin.x,
                      l.frame.origin.y, l.frame.size.width, l.frame.size.height, l.style.font_size);
        g_snapshot = std::move(snap);
        g_snapshot_version++;
    }
}

}  // namespace

CGSize measure_text(const std::u16string& text, double font_size, bool bold, double max_width) {
    static std::mutex m;
    std::lock_guard lock(m);
    static HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = make_font((int)std::lround(font_size), bold);
    HGDIOBJ old = SelectObject(dc, font);
    RECT r{0, 0, max_width > 0 ? (LONG)max_width : 100000, 0};
    DrawTextW(dc, reinterpret_cast<const wchar_t*>(text.c_str()), (int)text.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(font);
    return CGSize{(double)(r.right - r.left), (double)(r.bottom - r.top)};
}

std::vector<LabelSnapshot> visible_labels(u64& version) {
    std::lock_guard lock(g_snap_mutex);
    version = g_snapshot_version;
    return g_snapshot;
}

void draw_labels(u8* rgba, int w, int h, const std::vector<LabelSnapshot>& labels) {
    if (labels.empty()) return;
    double sx = w / g_device.width_pt, sy = h / g_device.height_pt;
    // Render all labels as white-on-black coverage into a DIB, then tint and blend into the image.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    const u32* cov = static_cast<const u32*>(bits);
    for (auto& l : labels) {
        std::memset(bits, 0, (size_t)w * h * 4);
        HFONT font = make_font((int)std::lround(l.style.font_size * sy), l.style.bold);
        HGDIOBJ old_font = SelectObject(dc, font);
        UINT fmt = DT_NOPREFIX | (l.style.lines == 1 ? DT_SINGLELINE | DT_VCENTER : DT_WORDBREAK);
        fmt |= l.style.alignment == 1 ? DT_CENTER : l.style.alignment == 2 ? DT_RIGHT : DT_LEFT;
        RECT r{(LONG)(l.frame.origin.x * sx), (LONG)(l.frame.origin.y * sy), (LONG)((l.frame.origin.x + l.frame.size.width) * sx),
               (LONG)((l.frame.origin.y + l.frame.size.height) * sy)};
        if (l.style.lines != 1) {  // center wrapped text vertically like UILabel
            RECT calc = r;
            DrawTextW(dc, reinterpret_cast<const wchar_t*>(l.text.c_str()), (int)l.text.size(), &calc, fmt | DT_CALCRECT);
            LONG text_h = calc.bottom - calc.top;
            LONG pad = ((r.bottom - r.top) - text_h) / 2;
            if (pad > 0) r.top += pad;
            if (r.top + text_h > h - 2) r.top = std::max<LONG>(0, h - 2 - text_h);  // keep on screen
            r.bottom = std::max<LONG>(r.bottom, r.top + text_h);
        }
        auto blend_pass = [&](int dx, int dy, const ColorData& c) {
            std::memset(bits, 0, (size_t)w * h * 4);
            RECT rr{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
            DrawTextW(dc, reinterpret_cast<const wchar_t*>(l.text.c_str()), (int)l.text.size(), &rr, fmt);
            GdiFlush();
            for (int y = std::max<LONG>(0, rr.top - 4); y < std::min<LONG>(h, rr.bottom + 4); y++)
                for (int x = std::max<LONG>(0, rr.left - 4); x < std::min<LONG>(w, rr.right + 4); x++) {
                    u32 px = cov[(size_t)y * w + x];
                    u32 a8 = std::max({px & 0xff, (px >> 8) & 0xff, (px >> 16) & 0xff});
                    if (!a8) continue;
                    float a = a8 / 255.0f * (float)c.a;
                    u8* d = rgba + ((size_t)y * w + x) * 4;
                    d[0] = (u8)(d[0] * (1 - a) + c.r * 255 * a);
                    d[1] = (u8)(d[1] * (1 - a) + c.g * 255 * a);
                    d[2] = (u8)(d[2] * (1 - a) + c.b * 255 * a);
                }
        };
        if (l.style.has_shadow)
            blend_pass((int)std::lround(l.style.shadow_dx * sx), (int)std::lround(l.style.shadow_dy * sy), l.style.shadow);
        blend_pass(0, 0, l.style.color);
        SelectObject(dc, old_font);
        DeleteObject(font);
    }
    SelectObject(dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void install_labels() {
    using objc::class_method;
    using objc::id;
    using objc::method;
    using objc::SEL;
    objc::Class L = objc::host_class("UILabel", "UIView");
    method(L, "setText:", [](id self, SEL, id t) {
        auto& d = objc::ensure<LabelData>(self);
        id old = d.text;
        d.text = objc::retain(objc::send(t, "copy"));
        objc::release(old);
    });
    method(L, "text", [](id self, SEL) { return objc::ensure<LabelData>(self).text; });
    method(L, "setFont:", [](id self, SEL, id font) {
        if (auto* f = objc::get<FontData>(font)) {
            objc::ensure<LabelData>(self).font_size = f->size;
            objc::ensure<LabelData>(self).bold = f->bold;
        }
    });
    method(L, "setTextColor:", [](id self, SEL, id c) {
        if (auto* cd = objc::get<ColorData>(c)) objc::ensure<LabelData>(self).color = *cd;
    });
    method(L, "setShadowColor:", [](id self, SEL, id c) {
        auto& d = objc::ensure<LabelData>(self);
        d.has_shadow = c != 0;
        if (auto* cd = objc::get<ColorData>(c)) d.shadow = *cd;
    });
    method(L, "setShadowOffset:", [](id self, SEL, CGSize off) {
        auto& d = objc::ensure<LabelData>(self);
        d.shadow_dx = off.width;
        d.shadow_dy = off.height;
    });
    method(L, "setTextAlignment:", [](id self, SEL, s64 a) { objc::ensure<LabelData>(self).alignment = a; });
    method(L, "setNumberOfLines:", [](id self, SEL, s64 n) { objc::ensure<LabelData>(self).lines = n; });
    method(L, "setLineBreakMode:", [](id, SEL, s64) {});
    method(L, "setAdjustsFontSizeToFitWidth:", [](id, SEL, bool) {});
    method(L, "setMinimumFontSize:", [](id, SEL, double) {});

    // UIKit string drawing additions to NSString.
    objc::Class S = objc::class_named("NSString");
    method(S, "sizeWithFont:constrainedToSize:", [](id self, SEL, id font, CGSize max) {
        auto* f = objc::get<FontData>(font);
        return measure_text(ns::utf16(self), f ? f->size : 17, f && f->bold, max.width);
    });
    method(S, "sizeWithFont:", [](id self, SEL, id font) {
        auto* f = objc::get<FontData>(font);
        return measure_text(ns::utf16(self), f ? f->size : 17, f && f->bold, 0);
    });

    // Keep a label snapshot fresh for the video presenter.
    auto* tm = new ns::Timer;
    tm->fire_at = ns::now_ref() + 0.5;
    tm->interval = 1.0 / 30;
    tm->repeats = true;
    tm->fn = refresh_snapshot;
    ns::RunLoop::main().add_timer(tm);
}

}  // namespace uikit
