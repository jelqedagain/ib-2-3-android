// CoreGraphics geometry (real) and drawing (no-op); UIKit controls; GameKit, StoreKit, social,
// iCloud and motion stubs that report "unavailable" so the game continues offline.
#include "uikit/uikit.h"
#include "win/dialogs.h"
#include "uikit/labels.h"
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "libc/format.h"
#include "libc/vfs.h"
#include "objc/internal.h"
#include <cmath>
#include <filesystem>

namespace uikit {

namespace {
using namespace ns;
using objc::Class;
using objc::id;
using objc::SEL;

struct Affine {
    double a, b, c, d, tx, ty;
};
void ret_affine(cpu::Thread& t, const Affine& m) { std::memcpy(gptr<void>(t.x(8)), &m, sizeof m); }
Affine mul(const Affine& l, const Affine& r) {
    return {l.a * r.a + l.b * r.c, l.a * r.b + l.b * r.d, l.c * r.a + l.d * r.c, l.c * r.b + l.d * r.d,
            l.tx * r.a + l.ty * r.c + r.tx, l.tx * r.b + l.ty * r.d + r.ty};
}

id not_available_error() {
    return objc::send(objc::class_named("NSError"), "errorWithDomain:code:userInfo:",
                      {str("GKErrorDomain"), 2, dict({{str("NSLocalizedDescription"), str("Not available on PC")}})});
}

// Calls a completion block later on the main thread with (arg0, error).
void complete_later(GuestAddr block, u64 arg0) {
    if (!block) return;
    GuestAddr b = objc::block_copy(block);
    post_to_main([b, arg0] {
        objc::call_block(b, {arg0, not_available_error()});
        objc::block_release(b);
    });
}

struct TextData : objc::HostData {
    id text = 0;
};
std::wstring to_wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string to_utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct AlertData : objc::HostData {
    id title = 0, message = 0, delegate = 0;
    std::vector<id> buttons;
    s64 cancel_index = -1;
    s64 style = 0;  // UIAlertViewStyle: 0 default, 1 secure text, 2 plain text, 3 login and password
    id fields[2] = {0, 0};
};

// Shows the alert as a native dialog (main thread); returns the index of the button chosen.
s64 show_alert_dialog(id self, AlertData& a) {
    HWND owner = (HWND)main_window();
    std::vector<std::wstring> buttons;
    for (id b : a.buttons) buttons.push_back(to_wide(utf8(b)));
    std::wstring title = to_wide(utf8(a.title)), message = to_wide(utf8(a.message));
    if (a.style == 0) return win::choose(owner, title, message, buttons, (int)a.cancel_index);
    // Text entry: OK is the first button that is not the cancel button.
    s64 ok = 0;
    while (ok == a.cancel_index && ok + 1 < (s64)buttons.size()) ok++;
    std::wstring ok_label = ok < (s64)buttons.size() ? buttons[ok] : L"OK";
    std::wstring cancel_label = a.cancel_index >= 0 && a.cancel_index < (s64)buttons.size() ? buttons[a.cancel_index] : L"Cancel";
    id field = objc::send(self, "textFieldAtIndex:", {0});
    std::wstring text = to_wide(utf8(objc::send(field, "text")));
    if (!win::prompt_text(owner, title, message, text, a.style == 1, ok_label, cancel_label))
        return a.cancel_index >= 0 ? a.cancel_index : ok;
    objc::send(field, "setText:", {str(to_utf8(text))});
    return ok;
}

}  // namespace

void install_misc() {
    using hle::fn;
    using objc::class_method;
    using objc::method;

    // ---------------- CoreGraphics geometry ----------------
    static CGRect s_rect_zero{};
    hle::data("_CGRectZero", gaddr(&s_rect_zero));
    static Affine s_identity{1, 0, 0, 1, 0, 0};
    hle::data("_CGAffineTransformIdentity", gaddr(&s_identity));
    fn("_CGRectGetMinX", [](CGRect r) { return r.origin.x; });
    fn("_CGRectGetMinY", [](CGRect r) { return r.origin.y; });
    fn("_CGRectGetMaxX", [](CGRect r) { return r.origin.x + r.size.width; });
    fn("_CGRectGetMaxY", [](CGRect r) { return r.origin.y + r.size.height; });
    fn("_CGRectGetMidX", [](CGRect r) { return r.origin.x + r.size.width / 2; });
    fn("_CGRectGetMidY", [](CGRect r) { return r.origin.y + r.size.height / 2; });
    fn("_CGRectGetWidth", [](CGRect r) { return std::fabs(r.size.width); });
    fn("_CGRectGetHeight", [](CGRect r) { return std::fabs(r.size.height); });
    fn("_CGRectContainsPoint", [](CGRect r, CGPoint p) {
        return p.x >= r.origin.x && p.y >= r.origin.y && p.x < r.origin.x + r.size.width && p.y < r.origin.y + r.size.height;
    });
    fn("_CGRectEqualToRect", [](CGRect a, CGRect b) { return std::memcmp(&a, &b, sizeof a) == 0; });
    fn("_CGRectInset", [](CGRect r, double dx, double dy) {
        return CGRect{{r.origin.x + dx, r.origin.y + dy}, {r.size.width - 2 * dx, r.size.height - 2 * dy}};
    });
    fn("_CGRectOffset", [](CGRect r, double dx, double dy) {
        return CGRect{{r.origin.x + dx, r.origin.y + dy}, r.size};
    });
    fn("_CGRectDivide", [](CGRect r, CGRect* slice, CGRect* rem, double amount, u32 edge) {
        CGRect s = r, m = r;
        switch (edge) {
        case 0: s.size.width = amount, m.origin.x += amount, m.size.width -= amount; break;   // MinX
        case 1: s.size.height = amount, m.origin.y += amount, m.size.height -= amount; break;  // MinY
        case 2: s.origin.x += r.size.width - amount, s.size.width = amount, m.size.width -= amount; break;
        default: s.origin.y += r.size.height - amount, s.size.height = amount, m.size.height -= amount; break;
        }
        if (slice) *slice = s;
        if (rem) *rem = m;
    });
    hle::raw("_CGAffineTransformMakeRotation", [](cpu::Thread& t) {
        double a = t.d(0);
        ret_affine(t, {std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0});
    });
    hle::raw("_CGAffineTransformMakeScale", [](cpu::Thread& t) { ret_affine(t, {t.d(0), 0, 0, t.d(1), 0, 0}); });
    hle::raw("_CGAffineTransformRotate", [](cpu::Thread& t) {
        double a = t.d(0);
        ret_affine(t, mul({std::cos(a), std::sin(a), -std::sin(a), std::cos(a), 0, 0}, *gptr<Affine>(t.x(0))));
    });
    hle::raw("_CGAffineTransformScale", [](cpu::Thread& t) { ret_affine(t, mul({t.d(0), 0, 0, t.d(1), 0, 0}, *gptr<Affine>(t.x(0)))); });

    // ---------------- CoreGraphics drawing: accepted and ignored ----------------
    static u64 s_dummy_handle[4];
    for (const char* s : {"_CGColorSpaceCreateDeviceRGB", "_CGColorCreate", "_CGPathCreateMutable", "_CGGradientCreateWithColors",
                          "_UIGraphicsGetCurrentContext"})
        fn(s, []() { return gaddr(s_dummy_handle); });
    for (const char* s : {"_CGColorRelease", "_CGColorSpaceRelease", "_CGGradientRelease", "_CGContextAddArcToPoint",
                          "_CGContextAddLineToPoint", "_CGContextAddLines", "_CGContextAddPath", "_CGContextAddRect",
                          "_CGContextBeginPath", "_CGContextClip", "_CGContextClosePath", "_CGContextDrawLinearGradient",
                          "_CGContextDrawPath", "_CGContextFillPath", "_CGContextFillRect", "_CGContextMoveToPoint",
                          "_CGContextRestoreGState", "_CGContextSaveGState", "_CGContextScaleCTM", "_CGContextSetFillColor",
                          "_CGContextSetFillColorWithColor", "_CGContextSetLineJoin", "_CGContextSetLineWidth",
                          "_CGContextSetShadowWithColor", "_CGContextSetStrokeColor", "_CGContextSetStrokeColorSpace",
                          "_CGContextSetStrokeColorWithColor", "_CGContextStrokeLineSegments", "_CGContextStrokePath",
                          "_CGContextTranslateCTM", "_CGPathAddArcToPoint", "_CGPathAddLineToPoint", "_CGPathAddPath",
                          "_CGPathCloseSubpath", "_CGPathMoveToPoint", "_UIGraphicsBeginImageContextWithOptions",
                          "_UIGraphicsEndImageContext"})
        fn(s, []() {});
    fn("_UIGraphicsGetImageFromCurrentImageContext", []() { return objc::autorelease(objc::alloc(objc::class_named("UIImage"))); });
    fn("_UIImageJPEGRepresentation", [](id, double) -> id { return 0; });

    // ---------------- UIKit value classes ----------------
    Class COL = objc::host_class("UIColor");
    auto color = [](Class c, double r, double g, double b, double a) {
        id o = objc::alloc(c);
        auto& d = objc::ensure<ColorData>(o);
        d.r = r, d.g = g, d.b = b, d.a = a;
        return objc::autorelease(o);
    };
    static decltype(color) s_color = color;
    class_method(COL, "colorWithRed:green:blue:alpha:", [](Class c, SEL, double r, double g, double b, double a) { return s_color(c, r, g, b, a); });
    class_method(COL, "colorWithWhite:alpha:", [](Class c, SEL, double w, double a) { return s_color(c, w, w, w, a); });
    method(COL, "initWithRed:green:blue:alpha:", [](id self, SEL, double r, double g, double b, double a) {
        auto& d = objc::ensure<ColorData>(self);
        d.r = r, d.g = g, d.b = b, d.a = a;
        return self;
    });
    for (auto [name, v] : std::initializer_list<std::pair<const char*, double>>{{"blackColor", 0.0}, {"whiteColor", 1.0}, {"grayColor", 0.5}, {"darkGrayColor", 0.33}, {"lightGrayColor", 0.67}})
        objc::add_method(COL, name, [v](cpu::Thread& t) { t.set_x(0, s_color(t.x(0), v, v, v, 1)); }, true);
    for (const char* name : {"clearColor", "redColor", "greenColor", "blueColor", "yellowColor", "orangeColor", "groupTableViewBackgroundColor"})
        class_method(COL, name, [](Class c, SEL) { return s_color(c, 0, 0, 0, 0); });
    class_method(COL, "colorWithPatternImage:", [](Class c, SEL, id) { return s_color(c, 0, 0, 0, 0); });
    method(COL, "CGColor", [](id self, SEL) { return self; });
    method(COL, "colorWithAlphaComponent:", [](id self, SEL, double a) {
        auto& d = objc::ensure<ColorData>(self);
        return s_color(objc::isa(self), d.r, d.g, d.b, a);
    });
    method(COL, "set", [](id, SEL) {});
    method(COL, "setFill", [](id, SEL) {});

    Class FONT = objc::host_class("UIFont");
    auto make_font = [](Class c, double size, bool bold) {
        id f = objc::alloc(c);
        auto& d = objc::ensure<FontData>(f);
        d.size = size;
        d.bold = bold;
        return objc::autorelease(f);
    };
    static decltype(make_font) s_make_font = make_font;
    class_method(FONT, "systemFontOfSize:", [](Class c, SEL, double size) { return s_make_font(c, size, false); });
    class_method(FONT, "italicSystemFontOfSize:", [](Class c, SEL, double size) { return s_make_font(c, size, false); });
    class_method(FONT, "boldSystemFontOfSize:", [](Class c, SEL, double size) { return s_make_font(c, size, true); });
    class_method(FONT, "fontWithName:size:", [](Class c, SEL, id name, double size) {
        return s_make_font(c, size, utf8(name).find("Bold") != std::string::npos);
    });
    method(FONT, "pointSize", [](id self, SEL) { return objc::ensure<FontData>(self).size; });
    method(FONT, "lineHeight", [](id self, SEL) { return objc::ensure<FontData>(self).size * 1.2; });
    method(FONT, "fontWithSize:", [](id self, SEL, double size) {
        return s_make_font(objc::isa(self), size, objc::ensure<FontData>(self).bold);
    });

    method(objc::class_named("UIView"), "transform", [](cpu::Thread& t) {
        static const double identity[6] = {1, 0, 0, 1, 0, 0};
        std::memcpy(gptr<void>(t.x(8)), identity, sizeof identity);
    });
    Class IMG = objc::host_class("UIImage");
    // No name, or a file that is not there, gives nil, as on iOS. IB2 asks for Logo.m4v
    // after Isa's post-credits scene; the Community Patch has neither Logo.m4v nor Logo.png, so the
    // game calls imageNamed:nil, and a made-up image made it wait forever for a splash screen that
    // never ends (black screen, music playing).
    for (const char* s : {"imageNamed:", "imageWithData:"})
        class_method(IMG, s, [](Class c, SEL, id arg) -> id { return arg ? objc::autorelease(objc::alloc(c)) : 0; });
    // -CGImage below is always NULL, so this one cannot tell a real image from none.
    class_method(IMG, "imageWithCGImage:", [](Class c, SEL, id) { return objc::autorelease(objc::alloc(c)); });
    static auto file_exists = [](id path) {
        if (!path) return false;
        std::string host = vfs::to_host(utf8(path).c_str());
        std::error_code ec;
        return !host.empty() && std::filesystem::exists(std::filesystem::path(libc::utf8_to_wide(host)), ec);
    };
    class_method(IMG, "imageWithContentsOfFile:", [](Class c, SEL, id path) -> id {
        return file_exists(path) ? objc::autorelease(objc::alloc(c)) : 0;
    });
    method(IMG, "initWithContentsOfFile:", [](id self, SEL, id path) -> id {
        if (file_exists(path)) return self;
        objc::release(self);
        return 0;
    });
    method(IMG, "initWithData:", [](id self, SEL, id) { return self; });
    method(IMG, "size", [](id, SEL) { return CGSize{1, 1}; });
    method(IMG, "scale", [](id, SEL) { return 1.0; });
    method(IMG, "CGImage", [](id, SEL) -> u64 { return 0; });
    method(IMG, "stretchableImageWithLeftCapWidth:topCapHeight:", [](id self, SEL, s64, s64) { return self; });
    method(IMG, "resizableImageWithCapInsets:", [](id self, SEL) { return self; });

    // ---------------- controls (views that do nothing special) ----------------
    Class CTRL = objc::host_class("UIControl", "UIView");
    Class BTN = objc::host_class("UIButton", "UIControl");
    for (auto [n, s] : std::initializer_list<std::pair<const char*, const char*>>{{"UILabel", "UIView"}, {"UIImageView", "UIView"}, {"UIScrollView", "UIView"}, {"UITextView", "UIScrollView"},
                        {"UITextField", "UIControl"}, {"UIActivityIndicatorView", "UIView"}, {"UIWebView", "UIView"},
                        {"UITableView", "UIScrollView"}, {"UITableViewCell", "UIView"}, {"UINavigationBar", "UIView"},
                        {"UIActionSheet", "UIView"}, {"UIAlertView", "UIView"}, {"ADBannerView", "UIView"},
                        {"UINavigationController", "UIViewController"}, {"UITabBarController", "UIViewController"},
                        {"GKAchievementViewController", "UIViewController"}, {"GKLeaderboardViewController", "UIViewController"},
                        {"GKMatchmakerViewController", "UIViewController"}, {"MFMailComposeViewController", "UIViewController"},
                        {"MFMessageComposeViewController", "UIViewController"}, {"SLComposeViewController", "UIViewController"},
                        {"UINavigationItem", "NSObject"}, {"UIBarButtonItem", "NSObject"}, {"UITapGestureRecognizer", "NSObject"},
                        {"NSMutableParagraphStyle", "NSObject"}, {"UILocalNotification", "NSObject"}})
        objc::host_class(n, s);
    class_method(BTN, "buttonWithType:", [](Class c, SEL, s64) { return objc::autorelease(objc::send(objc::alloc(c), "init")); });
    method(CTRL, "addTarget:action:forControlEvents:", [](id, SEL, id, SEL, u64) {});
    method(CTRL, "removeTarget:action:forControlEvents:", [](id, SEL, id, SEL, u64) {});
    method(CTRL, "setEnabled:", [](id, SEL, bool) {});
    method(CTRL, "setSelected:", [](id, SEL, bool) {});
    method(BTN, "setTitle:forState:", [](id, SEL, id, u64) {});
    method(BTN, "setImage:forState:", [](id, SEL, id, u64) {});
    method(BTN, "setBackgroundImage:forState:", [](id, SEL, id, u64) {});
    method(BTN, "setTitleColor:forState:", [](id, SEL, id, u64) {});
    method(objc::class_named("UIImageView"), "initWithImage:", [](id self, SEL, id) { return objc::send(self, "init"); });
    method(objc::class_named("UIImageView"), "setImage:", [](id, SEL, id) {});
    Class LBL = objc::class_named("UILabel");
    method(LBL, "setText:", [](id self, SEL, id t) { objc::ensure<TextData>(self).text = objc::retain(t); });
    method(LBL, "text", [](id self, SEL) { return objc::ensure<TextData>(self).text; });
    for (const char* s : {"setFont:", "setTextColor:", "setShadowColor:"}) method(LBL, s, [](id, SEL, id) {});
    for (const char* s : {"setTextAlignment:", "setNumberOfLines:", "setLineBreakMode:"}) method(LBL, s, [](id, SEL, s64) {});
    method(LBL, "setAdjustsFontSizeToFitWidth:", [](id, SEL, bool) {});
    Class TF = objc::class_named("UITextField");
    method(TF, "setText:", [](id self, SEL, id t) { objc::ensure<TextData>(self).text = objc::retain(t); });
    method(TF, "text", [](id self, SEL) { return objc::ensure<TextData>(self).text; });
    for (const char* s : {"setDelegate:", "setFont:", "setTextColor:", "setPlaceholder:"}) method(TF, s, [](id, SEL, id) {});
    for (const char* s : {"setKeyboardType:", "setReturnKeyType:", "setAutocorrectionType:", "setAutocapitalizationType:",
                          "setBorderStyle:", "setTextAlignment:", "setClearButtonMode:", "setKeyboardAppearance:"})
        method(TF, s, [](id, SEL, s64) {});
    for (const char* s : {"setClearsOnBeginEditing:", "setSecureTextEntry:", "setEnablesReturnKeyAutomatically:"})
        method(TF, s, [](id, SEL, bool) {});
    Class AIV = objc::class_named("UIActivityIndicatorView");
    method(AIV, "initWithActivityIndicatorStyle:", [](id self, SEL, s64) { return objc::send(self, "init"); });
    for (const char* s : {"startAnimating", "stopAnimating"}) method(AIV, s, [](id, SEL) {});
    method(AIV, "setHidesWhenStopped:", [](id, SEL, bool) {});
    method(AIV, "isAnimating", [](id, SEL) { return false; });

    // UIAlertView: shown as a native dialog (test runs dismiss it with the cancel button).
    Class AV = objc::class_named("UIAlertView");
    objc::add_method(AV, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", [](cpu::Thread& t) {
        id self = objc::send(t.x(0), "init");
        auto& d = objc::ensure<AlertData>(self);
        d.title = objc::retain(t.x(2));
        d.message = objc::retain(t.x(3));
        d.delegate = t.x(4);
        if (t.x(5)) {
            d.cancel_index = 0;
            d.buttons.push_back(objc::retain(t.x(5)));
        }
        if (id first = t.x(6)) {
            d.buttons.push_back(objc::retain(first));
            for (int i = 0;; i++) {
                id b = t.stack_slot(i);
                if (!b) break;
                d.buttons.push_back(objc::retain(b));
            }
        }
        t.set_x(0, self);
    });
    method(AV, "addButtonWithTitle:", [](id self, SEL, id title) -> s64 {
        auto& d = objc::ensure<AlertData>(self);
        d.buttons.push_back(objc::retain(title));
        return (s64)d.buttons.size() - 1;
    });
    method(AV, "setCancelButtonIndex:", [](id self, SEL, s64 i) { objc::ensure<AlertData>(self).cancel_index = i; });
    method(AV, "cancelButtonIndex", [](id self, SEL) -> s64 { return objc::ensure<AlertData>(self).cancel_index; });
    method(AV, "numberOfButtons", [](id self, SEL) -> s64 { return (s64)objc::ensure<AlertData>(self).buttons.size(); });
    method(AV, "buttonTitleAtIndex:", [](id self, SEL, s64 i) -> id {
        auto& d = objc::ensure<AlertData>(self);
        return i >= 0 && (size_t)i < d.buttons.size() ? d.buttons[i] : 0;
    });
    method(AV, "setDelegate:", [](id self, SEL, id dlg) { objc::ensure<AlertData>(self).delegate = dlg; });
    method(AV, "setAlertViewStyle:", [](id self, SEL, s64 style) { objc::ensure<AlertData>(self).style = style; });
    method(AV, "alertViewStyle", [](id self, SEL) -> s64 { return objc::ensure<AlertData>(self).style; });
    method(AV, "textFieldAtIndex:", [](id self, SEL, s64 i) -> id {
        auto& d = objc::ensure<AlertData>(self);
        if (i < 0 || i > 1) return 0;
        if (!d.fields[i]) d.fields[i] = objc::send(objc::alloc(objc::class_named("UITextField")), "init");
        return d.fields[i];
    });
    method(AV, "show", [](id self, SEL) {
        auto& d = objc::ensure<AlertData>(self);
        LOG_INFO("UIAlertView \"%s\": %s", utf8(d.title).c_str(), utf8(d.message).c_str());
        objc::retain(self);
        post_to_main([self] {
            auto& a = objc::ensure<AlertData>(self);
            s64 idx = a.cancel_index >= 0 ? a.cancel_index : 0;
            if (!g_test_mode) idx = show_alert_dialog(self, a);
            LOG_INFO("UIAlertView answered with button %lld", (long long)idx);
            if (a.delegate && objc::responds_to(a.delegate, objc::sel("alertView:clickedButtonAtIndex:")))
                objc::send(a.delegate, "alertView:clickedButtonAtIndex:", {self, (u64)idx});
            if (a.delegate && objc::responds_to(a.delegate, objc::sel("alertView:didDismissWithButtonIndex:")))
                objc::send(a.delegate, "alertView:didDismissWithButtonIndex:", {self, (u64)idx});
            objc::release(self);
        });
    });
    method(AV, "dismissWithClickedButtonIndex:animated:", [](id, SEL, s64, bool) {});

    // Local notifications are accepted and ignored.
    Class LN = objc::class_named("UILocalNotification");
    for (const char* s : {"setFireDate:", "setTimeZone:", "setAlertBody:", "setUserInfo:", "setSoundName:", "setAlertAction:",
                          "setAlertLaunchImage:", "setCategory:"})
        method(LN, s, [](id, SEL, id) {});
    for (const char* s : {"setApplicationIconBadgeNumber:", "setRepeatInterval:", "setHasAction:"}) method(LN, s, [](id, SEL, s64) {});
    Class PB = objc::host_class("UIPasteboard");
    class_method(PB, "generalPasteboard", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(PB, "setString:", [](id self, SEL, id s) { objc::ensure<TextData>(self).text = objc::retain(s); });
    method(PB, "string", [](id self, SEL) { return objc::ensure<TextData>(self).text; });

    // ---------------- iCloud: unavailable ----------------
    Class DOC = objc::host_class("UIDocument");
    method(DOC, "initWithFileURL:", [](id self, SEL, id) { return self; });
    method(DOC, "openWithCompletionHandler:", [](id, SEL, GuestAddr b) { complete_later(b, 0); });
    method(DOC, "closeWithCompletionHandler:", [](id, SEL, GuestAddr b) { complete_later(b, 0); });
    method(DOC, "saveToURL:forSaveOperation:completionHandler:", [](id, SEL, id, s64, GuestAddr b) { complete_later(b, 0); });
    method(DOC, "documentState", [](id, SEL) -> u64 { return 1; });
    Class KVS = objc::host_class("NSUbiquitousKeyValueStore");
    class_method(KVS, "defaultStore", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(KVS, "synchronize", [](id, SEL) { return false; });
    method(KVS, "objectForKey:", [](id, SEL, id) -> id { return 0; });
    method(KVS, "dictionaryRepresentation", [](id, SEL) { return dict({}); });
    for (const char* s : {"setObject:forKey:", "setData:forKey:", "setString:forKey:"}) method(KVS, s, [](id, SEL, id, id) {});
    method(KVS, "removeObjectForKey:", [](id, SEL, id) {});
    Class MQ = objc::host_class("NSMetadataQuery");
    for (const char* s : {"setSearchScopes:", "setPredicate:", "setSortDescriptors:"}) method(MQ, s, [](id, SEL, id) {});
    for (const char* s : {"stopQuery", "disableUpdates", "enableUpdates"}) method(MQ, s, [](id, SEL) {});
    method(MQ, "startQuery", [](id, SEL) { return false; });
    method(MQ, "resultCount", [](id, SEL) -> u64 { return 0; });
    method(MQ, "results", [](id, SEL) { return array({}); });
    // IB2 resolves iCloud save conflicts through NSFileVersion: there are none.
    Class FV = objc::host_class("NSFileVersion");
    class_method(FV, "currentVersionOfItemAtURL:", [](Class, SEL, id) -> id { return 0; });
    class_method(FV, "unresolvedConflictVersionsOfItemAtURL:", [](Class, SEL, id) { return array({}); });
    class_method(FV, "otherVersionsOfItemAtURL:", [](Class, SEL, id) { return array({}); });
    class_method(FV, "removeOtherVersionsOfItemAtURL:error:", [](Class, SEL, id, u64* err) {
        if (err) *err = 0;
        return true;
    });
    method(FV, "setResolved:", [](id, SEL, bool) {});

    // ---------------- GameKit: never authenticated ----------------
    Class GLP = objc::host_class("GKLocalPlayer", "GKPlayer");
    class_method(GLP, "localPlayer", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(GLP, "isAuthenticated", [](id, SEL) { return false; });
    method(GLP, "playerID", [](id, SEL) -> id { return 0; });
    method(GLP, "alias", [](id, SEL) { return str("Player"); });
    method(GLP, "displayName", [](id, SEL) { return str("Player"); });
    method(GLP, "isUnderage", [](id, SEL) { return false; });
    method(GLP, "setAuthenticateHandler:", [](id, SEL, GuestAddr b) {
        LOG_INFO("Game Center: reporting not authenticated");
        complete_later(b, 0);
    });
    method(GLP, "authenticateWithCompletionHandler:", [](id, SEL, GuestAddr b) {
        if (!b) return;
        GuestAddr bb = objc::block_copy(b);
        post_to_main([bb] {
            objc::call_block(bb, {not_available_error()});
            objc::block_release(bb);
        });
    });
    method(GLP, "loadFriendsWithCompletionHandler:", [](id, SEL, GuestAddr b) { complete_later(b, 0); });
    for (const char* c : {"GKAchievement", "GKAchievementDescription", "GKLeaderboard", "GKScore", "GKMatchRequest", "GKMatchmaker", "GKVoiceChat"})
        objc::host_class(c);
    class_method(objc::class_named("GKAchievement"), "loadAchievementsWithCompletionHandler:", [](Class, SEL, GuestAddr b) { complete_later(b, 0); });
    class_method(objc::class_named("GKAchievement"), "resetAchievementsWithCompletionHandler:", [](Class, SEL, GuestAddr b) {
        if (b) objc::call_block(b, {not_available_error()});
    });
    class_method(objc::class_named("GKAchievementDescription"), "loadAchievementDescriptionsWithCompletionHandler:",
                 [](Class, SEL, GuestAddr b) { complete_later(b, 0); });
    method(objc::class_named("GKAchievement"), "initWithIdentifier:", [](id self, SEL, id) { return self; });
    method(objc::class_named("GKAchievement"), "reportAchievementWithCompletionHandler:", [](id, SEL, GuestAddr b) {
        if (b) objc::call_block(b, {not_available_error()});
    });
    method(objc::class_named("GKScore"), "initWithCategory:", [](id self, SEL, id) { return self; });
    method(objc::class_named("GKScore"), "reportScoreWithCompletionHandler:", [](id, SEL, GuestAddr b) {
        if (b) objc::call_block(b, {not_available_error()});
    });
    class_method(objc::class_named("GKMatchmaker"), "sharedMatchmaker", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    // IB2 also loads leaderboard scores and player names, and keeps achievement progress.
    method(objc::class_named("GKLeaderboard"), "loadScoresWithCompletionHandler:", [](id, SEL, GuestAddr b) { complete_later(b, 0); });
    class_method(objc::class_named("GKPlayer"), "loadPlayersForIdentifiers:withCompletionHandler:",
                 [](Class, SEL, id, GuestAddr b) { complete_later(b, 0); });
    method(objc::class_named("GKAchievement"), "setPercentComplete:", [](id, SEL, double) {});
    method(objc::class_named("GKAchievement"), "percentComplete", [](id, SEL) { return 0.0; });

    // ---------------- Twitter requests, iAd attribution: not available ----------------
    // Each request with a completion handler gets the answer of an offline iPhone, so the game never
    // waits for a reply (accounts: see "social" below).
    for (const char* c : {"SLRequest", "TWRequest"})
        method(objc::host_class(c), "performRequestWithHandler:", [](id, SEL, GuestAddr b) {  // (data, response, error)
            if (!b) return;
            GuestAddr bb = objc::block_copy(b);
            post_to_main([bb] {
                objc::call_block(bb, {0, 0, not_available_error()});
                objc::block_release(bb);
            });
        });
    Class ADC = objc::host_class("ADClient");
    class_method(ADC, "sharedClient", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(ADC, "determineAppInstallationAttributionWithCompletionHandler:", [](id, SEL, GuestAddr b) { complete_later(b, 0); });
    method(ADC, "lookupAdConversionDetails:", [](id, SEL, GuestAddr b) {  // (purchase date, impression date)
        if (!b) return;
        GuestAddr bb = objc::block_copy(b);
        post_to_main([bb] {
            objc::call_block(bb, {0, 0});
            objc::block_release(bb);
        });
    });

    // ---------------- StoreKit: purchases unavailable ----------------
    Class PQ = objc::host_class("SKPaymentQueue");
    class_method(PQ, "canMakePayments", [](Class, SEL) { return false; });
    class_method(PQ, "defaultQueue", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    for (const char* s : {"addTransactionObserver:", "removeTransactionObserver:", "addPayment:", "finishTransaction:"})
        method(PQ, s, [](id, SEL, id) {});
    method(PQ, "restoreCompletedTransactions", [](id, SEL) {});
    method(PQ, "transactions", [](id, SEL) { return array({}); });
    Class PR = objc::host_class("SKProductsRequest");
    struct ReqData : objc::HostData {
        id delegate = 0;
    };
    method(PR, "initWithProductIdentifiers:", [](id self, SEL, id) { return self; });
    method(PR, "setDelegate:", [](id self, SEL, id d) { objc::ensure<ReqData>(self).delegate = d; });
    method(PR, "delegate", [](id self, SEL) { return objc::ensure<ReqData>(self).delegate; });
    method(PR, "start", [](id self, SEL) {
        objc::retain(self);
        post_to_main([self] {
            id d = objc::ensure<ReqData>(self).delegate;
            if (d && objc::responds_to(d, objc::sel("request:didFailWithError:")))
                objc::send(d, "request:didFailWithError:", {self, not_available_error()});
            objc::release(self);
        });
    });
    method(PR, "cancel", [](id, SEL) {});
    objc::host_class("SKPayment");
    class_method(objc::class_named("SKPayment"), "paymentWithProduct:", [](Class c, SEL, id) { return objc::autorelease(objc::alloc(c)); });

    // ---------------- dynamically looked-up constants and helpers ----------------
    for (const char* sym : {"_SLServiceTypeFacebook", "_ACFacebookAudienceEveryone", "_ACFacebookAudienceFriends",
                            "_ACFacebookAudienceOnlyMe", "_ACAccountTypeIdentifierFacebook"}) {
        std::string name = sym + 1;
        hle::data_lazy(sym, [name] {
            auto* cell = static_cast<u64*>(hle::alloc_static(8));
            *cell = str_retained(name);
            return gaddr(cell);
        });
    }
    // CATransform3D is 16 doubles, returned indirectly through x8.
    hle::raw("_CATransform3DMakeScale", [](cpu::Thread& t) {
        double m[16] = {t.d(0), 0, 0, 0, 0, t.d(1), 0, 0, 0, 0, t.d(2), 0, 0, 0, 0, 1};
        std::memcpy(gptr<void>(t.x(8)), m, sizeof m);
    });
    hle::raw("_CATransform3DMakeTranslation", [](cpu::Thread& t) {
        double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t.d(0), t.d(1), t.d(2), 1};
        std::memcpy(gptr<void>(t.x(8)), m, sizeof m);
    });
    hle::raw("_CATransform3DConcat", [](cpu::Thread& t) {
        const double* a = gptr<double>(t.x(0));
        const double* b = gptr<double>(t.x(1));
        double m[16];
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) {
                double s = 0;
                for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
                m[r * 4 + c] = s;
            }
        std::memcpy(gptr<void>(t.x(8)), m, sizeof m);
    });
    fn("_AudioServicesCreateSystemSoundID", [](id, u32* out) {
        if (out) *out = 1;
        return 0;
    });
    fn("_AudioServicesDisposeSystemSoundID", [](u32) { return 0; });
    fn("_AudioServicesPlaySystemSound", [](u32) {});
    Class CAT = objc::host_class("CATransaction");
    for (const char* s : {"begin", "commit", "flush"}) class_method(CAT, s, [](Class, SEL) {});
    class_method(CAT, "setDisableActions:", [](Class, SEL, bool) {});
    class_method(CAT, "setAnimationDuration:", [](Class, SEL, double) {});
    class_method(CAT, "setCompletionBlock:", [](Class, SEL, GuestAddr b) {
        if (b) objc::call_block(b);
    });
    Class ASI = objc::host_class("ASIdentifierManager");
    class_method(ASI, "sharedManager", [](Class c, SEL) {
        static id s = objc::alloc(c);
        return s;
    });
    method(ASI, "isAdvertisingTrackingEnabled", [](id, SEL) { return false; });
    method(ASI, "advertisingIdentifier", [](id, SEL) { return objc::autorelease(objc::alloc(objc::class_named("NSUUID"))); });

    // ---------------- social / mail / accounts / motion ----------------
    class_method(objc::class_named("SLComposeViewController"), "isAvailableForServiceType:", [](Class, SEL, id) { return false; });
    class_method(objc::class_named("MFMailComposeViewController"), "canSendMail", [](Class, SEL) { return false; });
    class_method(objc::class_named("MFMessageComposeViewController"), "canSendText", [](Class, SEL) { return false; });
    objc::host_class("SLRequest");
    Class ACS = objc::host_class("ACAccountStore");
    method(ACS, "accountTypeWithAccountTypeIdentifier:", [](id self, SEL, id) { return self; });
    method(ACS, "accountsWithAccountType:", [](id, SEL, id) { return array({}); });
    method(ACS, "requestAccessToAccountsWithType:options:completion:", [](id, SEL, id, id, GuestAddr b) {
        if (!b) return;
        GuestAddr bb = objc::block_copy(b);
        post_to_main([bb] {
            objc::call_block(bb, {0, 0});
            objc::block_release(bb);
        });
    });
    // IB2 uses the older request and renews credentials: no access, renewal failed.
    method(ACS, "requestAccessToAccountsWithType:withCompletionHandler:", [](id, SEL, id, GuestAddr b) {
        if (!b) return;
        GuestAddr bb = objc::block_copy(b);
        post_to_main([bb] {
            objc::call_block(bb, {0, 0});
            objc::block_release(bb);
        });
    });
    method(ACS, "renewCredentialsForAccount:completion:", [](id, SEL, id, GuestAddr b) {
        complete_later(b, 2);  // ACAccountCredentialRenewResultFailed
    });
    Class CMM = objc::host_class("CMMotionManager");
    for (const char* s : {"isDeviceMotionAvailable", "isAccelerometerAvailable", "isGyroAvailable", "isMagnetometerAvailable",
                          "isDeviceMotionActive", "isAccelerometerActive", "isGyroActive"})
        method(CMM, s, [](id, SEL) { return false; });
    for (const char* s : {"startDeviceMotionUpdates", "stopDeviceMotionUpdates", "startAccelerometerUpdates",
                          "stopAccelerometerUpdates", "startGyroUpdates", "stopGyroUpdates"})
        method(CMM, s, [](id, SEL) {});
    method(CMM, "setDeviceMotionUpdateInterval:", [](id, SEL, double) {});
    method(CMM, "setAccelerometerUpdateInterval:", [](id, SEL, double) {});
    method(CMM, "deviceMotion", [](id, SEL) -> id { return 0; });
    method(CMM, "accelerometerData", [](id, SEL) -> id { return 0; });
}

}  // namespace uikit
