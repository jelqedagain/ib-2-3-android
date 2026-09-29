// Per-thread run loop used by NSRunLoop/CFRunLoop, timers and main-thread dispatch.
#pragma once
#include "objc/runtime.h"
#include <deque>
#include <functional>
#include <mutex>
#include <windows.h>

namespace ns {

double now_ref();  // seconds since 2001-01-01

struct Timer {
    objc::id obj = 0;     // NSTimer object (or perform argument)
    objc::id target = 0;
    objc::SEL selector = 0;
    GuestAddr block = 0;
    std::function<void()> fn;
    double fire_at = 0, interval = 0;
    bool repeats = false, valid = true, is_perform = false;
    void fire();
};

struct RunLoop {
    RunLoop();
    static RunLoop& current();
    static RunLoop& main();
    static RunLoop* from_cf(GuestAddr cf);

    void post(std::function<void()> fn);
    void add_timer(Timer* t);
    void remove_timers_if(const std::function<bool(Timer*)>& pred);
    // Processes pending work and due timers; if nothing was handled, waits up to timeout.
    // Returns true if something was handled.
    bool run_once(double timeout_sec);

    HANDLE wake;
    DWORD tid = 0;
    GuestAddr cf = 0;   // CFRunLoopRef handle given to the guest
    objc::id nsobj = 0;  // NSRunLoop object
    std::mutex m;
    std::deque<std::function<void()>> queue;
    std::vector<Timer*> timers;
};

// Installed by the windowing layer: waits up to timeout_ms for window messages or `wake`,
// dispatching any window messages that arrive.
extern std::function<void(DWORD timeout_ms, HANDLE wake)> g_main_pump;

void install_thread_late();

}  // namespace ns
