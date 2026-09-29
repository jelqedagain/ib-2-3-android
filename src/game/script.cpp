// Test automation (-script file). Each line: "<seconds> <action> [args]", times measured from start:
//   key <UnrealKeyName> down|up|tap     e.g.  70 key S down
//   tap <x> <y>                          touch in screen points (736x414)
//   hold <x> <y> <seconds>
//   swipe <x1> <y1> <x2> <y2> [seconds]
//   shot [name]                           save a screenshot of the next presented frame
//   pad <control> <value>                 fake controller: A B X Y LB RB BACK START L3 R3 UP DOWN LEFT
//                                         RIGHT (0/1), LX LY RX RY (-1..1), LT RT (0..1); "pad off"
//   waitframes <n>                        wait until the game has presented n more frames (loading
//                                         takes a varying time); later times shift by the wait
#include "foundation/foundation.h"
#include "game/game.h"
#include "gles/gl.h"
#include "uikit/uikit.h"
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include <windows.h>

namespace game {

namespace {

constexpr int kScriptFinger = 90;

void on_main(std::function<void()> fn) { ns::post_to_main(std::move(fn)); }

void do_swipe(double x1, double y1, double x2, double y2, double secs) {
    const int steps = 6;
    on_main([=] { uikit::touch_down(kScriptFinger, {x1, y1}); });
    for (int i = 1; i <= steps; i++) {
        Sleep((DWORD)(secs * 1000 / steps));
        double f = (double)i / steps;
        on_main([=] { uikit::touch_move(kScriptFinger, {x1 + (x2 - x1) * f, y1 + (y2 - y1) * f}); });
    }
    on_main([=] { uikit::touch_up(kScriptFinger, {x2, y2}); });
}

}  // namespace

void run_script(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        LOG_ERROR("script: cannot open %s", path.c_str());
        return;
    }
    struct Step {
        double at;
        std::string line;
    };
    std::vector<Step> steps;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        double at;
        if (line.empty() || line[0] == '#' || !(in >> at)) continue;
        steps.push_back({at, line});
    }
    std::thread([steps] {
        logging::set_thread_name("script");
        u64 start = GetTickCount64();
        for (auto& s : steps) {
            u64 due = start + (u64)(s.at * 1000);
            u64 now = GetTickCount64();
            if (due > now) Sleep((DWORD)(due - now));
            std::istringstream in(s.line);
            double at;
            std::string action;
            in >> at >> action;
            LOG_INFO("script: %s", s.line.c_str());
            if (action == "key") {
                std::string key, mode;
                in >> key >> mode;
                if (mode == "tap") {
                    press_key(key, true);
                    Sleep(80);
                    press_key(key, false);
                } else {
                    press_key(key, mode != "up");
                }
            } else if (action == "tap" || action == "hold") {
                double x, y, dur = 0.06;
                in >> x >> y;
                if (action == "hold") in >> dur;
                on_main([=] { uikit::touch_down(kScriptFinger, {x, y}); });
                Sleep((DWORD)(dur * 1000));
                on_main([=] { uikit::touch_up(kScriptFinger, {x, y}); });
            } else if (action == "swipe") {
                double x1, y1, x2, y2, secs = 0.1;
                in >> x1 >> y1 >> x2 >> y2;
                in >> secs;
                do_swipe(x1, y1, x2, y2, secs);
            } else if (action == "pad") {
                std::string control;
                float value = 0;
                in >> control >> value;
                set_fake_pad(control, value);
            } else if (action == "waitframes") {
                u64 n = 0;
                in >> n;
                u64 target = uikit::frames_presented() + n;
                u64 began = GetTickCount64();
                while (uikit::frames_presented() < target) Sleep(50);
                start += GetTickCount64() - began;
                LOG_INFO("script: %llu frames presented", (unsigned long long)n);
            } else if (action == "shot") {
                std::string name;
                in >> name;
                gles::request_screenshot(name.empty() ? "shot_t" + std::to_string((int)s.at) + ".png" : name);
            }
        }
    }).detach();
}

}  // namespace game
