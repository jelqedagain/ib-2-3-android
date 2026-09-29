// Run loops, NSThread, NSTimer, performSelector variants, NSDate, and GCD.
#include "foundation/foundation.h"
#include "libc/format.h"
#include "foundation/runloop.h"
#include "libc/pthread.h"
#include "objc/internal.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <windows.h>

namespace ns {

// ---------------------------------------------------------------------------
// clock (seconds since 2001-01-01, like NSDate)
double now_ref() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    u64 t = ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
    return (double)(t - 126227808000000000ull) / 1e7;
}

// ---------------------------------------------------------------------------
// RunLoop
namespace {
thread_local RunLoop* t_runloop = nullptr;
RunLoop* g_main_runloop = nullptr;
DWORD g_main_tid = 0;
}  // namespace

std::function<void(DWORD timeout_ms, HANDLE wake)> g_main_pump;

RunLoop::RunLoop() { wake = CreateEventW(nullptr, FALSE, FALSE, nullptr); }

RunLoop& RunLoop::current() {
    if (!t_runloop) {
        t_runloop = new RunLoop;
        t_runloop->tid = GetCurrentThreadId();
        t_runloop->cf = gaddr(hle::alloc_static(64));
        *gptr<u64>(t_runloop->cf + 8) = gaddr(t_runloop);
    }
    return *t_runloop;
}
RunLoop& RunLoop::main() { return *g_main_runloop; }
RunLoop* RunLoop::from_cf(GuestAddr cf) { return cf ? gptr<RunLoop>(*gptr<u64>(cf + 8)) : nullptr; }

void RunLoop::post(std::function<void()> fn) {
    {
        std::lock_guard lock(m);
        queue.push_back(std::move(fn));
    }
    SetEvent(wake);
}

void RunLoop::add_timer(Timer* t) {
    {
        std::lock_guard lock(m);
        timers.push_back(t);
    }
    SetEvent(wake);
}

void RunLoop::remove_timers_if(const std::function<bool(Timer*)>& pred) {
    std::lock_guard lock(m);
    for (Timer* t : timers)
        if (pred(t)) t->valid = false;
}

bool RunLoop::run_once(double timeout_sec) {
    bool handled = false;
    u64 pool = objc::pool_push();
    // Work items
    for (;;) {
        std::function<void()> fn;
        {
            std::lock_guard lock(m);
            if (queue.empty()) break;
            fn = std::move(queue.front());
            queue.pop_front();
        }
        fn();
        handled = true;
    }
    // Timers
    double now = now_ref();
    std::vector<Timer*> due;
    {
        std::lock_guard lock(m);
        for (size_t i = 0; i < timers.size();) {
            Timer* t = timers[i];
            if (!t->valid) {
                timers.erase(timers.begin() + i);
                objc::release(t->obj);
                delete t;
                continue;
            }
            if (t->fire_at <= now) due.push_back(t);
            i++;
        }
    }
    for (Timer* t : due) {
        if (!t->valid) continue;
        t->fire();
        handled = true;
        if (t->repeats && t->valid) t->fire_at = std::max(t->fire_at + t->interval, now_ref() - t->interval);
        else t->valid = false;
    }
    objc::pool_pop(pool);
    if (handled) return true;

    // Wait for the next timer or a wake-up.
    double next = now_ref() + timeout_sec;
    {
        std::lock_guard lock(m);
        for (Timer* t : timers)
            if (t->valid) next = std::min(next, t->fire_at);
        if (!queue.empty()) return false;
    }
    double wait = next - now_ref();
    DWORD ms = wait <= 0 ? 0 : (DWORD)std::min(wait * 1000.0, 1e9);
    if (this == g_main_runloop && g_main_pump) g_main_pump(ms, wake);
    else WaitForSingleObject(wake, ms);
    return false;
}

void Timer::fire() {
    if (block) objc::call_block(block, {obj});
    else if (target) objc::send_sel(target, selector, {obj});
    else if (fn) fn();
}

// ---------------------------------------------------------------------------
// main-thread helpers
void set_main_thread() {
    g_main_tid = GetCurrentThreadId();
    g_main_runloop = &RunLoop::current();
}
bool is_main_thread() { return GetCurrentThreadId() == g_main_tid; }
void post_to_main(std::function<void()> fn) { g_main_runloop->post(std::move(fn)); }
void run_main_queue_once() { g_main_runloop->run_once(0); }

namespace {

// Runs fn on the main thread and waits for it.
void run_on_main_sync(const std::function<void()>& fn) {
    if (is_main_thread()) {
        fn();
        return;
    }
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    post_to_main([&] {
        fn();
        SetEvent(done);
    });
    WaitForSingleObject(done, INFINITE);
    CloseHandle(done);
}

// ---------------------------------------------------------------------------
// NSThread
struct ThreadData : objc::HostData {
    id target = 0;
    SEL selector = 0;
    id argument = 0;
    std::string name;
    u64 stack_size = 1 << 20;
    double priority = 0.5;
    bool cancelled = false, executing = false, finished = false, is_main = false;
    id dictionary = 0;
    RunLoop* runloop = nullptr;
};
Class g_nsthread, g_date, g_timer, g_runloop_cls;
thread_local id t_nsthread = 0;
id g_main_nsthread = 0;

id current_nsthread() {
    if (!t_nsthread) {
        t_nsthread = objc::alloc(g_nsthread);
        auto& d = objc::ensure<ThreadData>(t_nsthread);
        d.is_main = is_main_thread();
        d.runloop = &RunLoop::current();
    }
    return t_nsthread;
}

void start_nsthread(id thread) {
    objc::retain(thread);
    auto& d = objc::ensure<ThreadData>(thread);
    d.executing = true;
    std::string name = d.name.empty() ? "NSThread" : d.name;
    libc::spawn_guest_thread(name.c_str(), [thread] {
        t_nsthread = thread;
        objc::ensure<ThreadData>(thread).runloop = &RunLoop::current();
        u64 pool = objc::pool_push();
        objc::send(thread, "main");
        objc::pool_pop(pool);
        auto& dd = objc::ensure<ThreadData>(thread);
        dd.executing = false;
        dd.finished = true;
        objc::release(thread);
    }, std::max<u64>(d.stack_size, 1 << 20));
}

// ---------------------------------------------------------------------------
// NSDate
struct DateData : objc::HostData {
    double t = 0;  // seconds since reference date
};
id new_date(Class c, double t) {
    id d = objc::alloc(c);
    objc::ensure<DateData>(d).t = t;
    return d;
}
double date_value(id d) {
    if (!d) return 0;
    auto* dd = objc::get<DateData>(d);
    return dd ? dd->t : 0;
}

// ---------------------------------------------------------------------------
// NSTimer
struct TimerData : objc::HostData {
    Timer* timer = nullptr;
    id user_info = 0;
    ~TimerData() override { objc::release(user_info); }
};
id make_timer(Class c, double interval, id target, SEL s, id user_info, bool repeats) {
    id t = objc::alloc(c);
    auto& td = objc::ensure<TimerData>(t);
    td.user_info = objc::retain(user_info);
    auto* tm = new Timer;
    tm->obj = t;
    tm->target = objc::retain(target);
    tm->selector = s;
    tm->interval = std::max(interval, 0.0001);
    tm->repeats = repeats;
    tm->fire_at = now_ref() + interval;
    td.timer = tm;
    return t;
}
void schedule(RunLoop& rl, id timer) {
    auto& td = objc::ensure<TimerData>(timer);
    objc::retain(timer);  // the run loop holds the timer
    rl.add_timer(td.timer);
}

// ---------------------------------------------------------------------------
// GCD
struct Queue {
    std::string label;
    bool serial = true;
    bool main = false;
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> items;
    int workers = 0;
    int idle = 0;
    DWORD owner_tid = 0;  // serial queue worker thread
};
Queue* g_main_queue = nullptr;
Queue* g_global_queues[4] = {};
thread_local Queue* t_current_queue = nullptr;

void queue_worker(Queue* q) {
    t_current_queue = q;
    q->owner_tid = q->serial ? GetCurrentThreadId() : 0;
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock lock(q->m);
            q->idle++;
            q->cv.wait(lock, [&] { return !q->items.empty(); });
            q->idle--;
            fn = std::move(q->items.front());
            q->items.pop_front();
        }
        u64 pool = objc::pool_push();
        fn();
        objc::pool_pop(pool);
    }
}

void enqueue(Queue* q, std::function<void()> fn) {
    if (!q) q = g_global_queues[0];
    if (q->main) {
        post_to_main(std::move(fn));
        return;
    }
    bool spawn = false;
    {
        std::lock_guard lock(q->m);
        q->items.push_back(std::move(fn));
        int max_workers = q->serial ? 1 : 4;
        if (q->workers < max_workers && q->idle == 0) {
            q->workers++;
            spawn = true;
        }
    }
    if (spawn) libc::spawn_guest_thread(q->label.c_str(), [q] { queue_worker(q); }, 1 << 20);
    q->cv.notify_one();
}

Queue* new_queue(const char* label, bool serial) {
    auto* q = new Queue;
    q->label = label ? label : "queue";
    q->serial = serial;
    return q;
}

// dispatch_time values are nanoseconds on our own monotonic clock.
u64 now_ns() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (u64)((unsigned __int128)c.QuadPart * 1000000000 / f.QuadPart);
}

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    s64 count;
};

// Returns true if the caller must run the initializer; otherwise waits until it has run.
bool once_begin(s64* pred) {
    for (int spins = 0;; spins++) {
        s64 v = __atomic_load_n(pred, __ATOMIC_ACQUIRE);
        if (v == ~0ll) return false;
        if (v == 0) {
            s64 expected = 0;
            if (__atomic_compare_exchange_n(pred, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return true;
            continue;
        }
        if (spins > 50) Sleep(1);
        else SwitchToThread();
    }
}
void once_end(s64* pred) { __atomic_store_n(pred, ~0ll, __ATOMIC_RELEASE); }

struct RunLoopRef : objc::HostData {
    RunLoop* rl = nullptr;
};

}  // namespace

// ---------------------------------------------------------------------------
void install_thread() {
    using objc::class_method;
    using objc::method;
    g_nsthread = objc::host_class("NSThread");
    g_date = objc::host_class("NSDate");
    g_timer = objc::host_class("NSTimer");
    g_runloop_cls = objc::host_class("NSRunLoop");

    // NSThread
    Class T = g_nsthread;
    class_method(T, "currentThread", [](Class, SEL) { return current_nsthread(); });
    class_method(T, "mainThread", [](Class, SEL) { return g_main_nsthread; });
    class_method(T, "isMainThread", [](Class, SEL) { return is_main_thread(); });
    class_method(T, "isMultiThreaded", [](Class, SEL) { return true; });
    class_method(T, "sleepForTimeInterval:", [](Class, SEL, double s) { Sleep((DWORD)(s * 1000)); });
    class_method(T, "sleepUntilDate:", [](Class, SEL, id d) {
        double w = date_value(d) - now_ref();
        if (w > 0) Sleep((DWORD)(w * 1000));
    });
    class_method(T, "setThreadPriority:", [](Class, SEL, double) { return true; });
    class_method(T, "threadPriority", [](Class, SEL) { return 0.5; });
    class_method(T, "detachNewThreadSelector:toTarget:withObject:", [](Class c, SEL, SEL s, id target, id arg) {
        id th = objc::alloc(c);
        auto& d = objc::ensure<ThreadData>(th);
        d.target = objc::retain(target);
        d.selector = s;
        d.argument = objc::retain(arg);
        d.name = objc::class_name(objc::isa(target)) + "::" + objc::sel_name(s);
        LOG_INFO("NSThread detach -[%s %s]", objc::class_name(objc::isa(target)).c_str(), objc::sel_name(s));
        start_nsthread(th);
        objc::release(th);
    });
    method(T, "init", [](id self, SEL) {
        objc::ensure<ThreadData>(self);
        return self;
    });
    method(T, "initWithTarget:selector:object:", [](id self, SEL, id target, SEL s, id arg) {
        auto& d = objc::ensure<ThreadData>(self);
        d.target = objc::retain(target);
        d.selector = s;
        d.argument = objc::retain(arg);
        return self;
    });
    method(T, "start", [](id self, SEL) { start_nsthread(self); });
    method(T, "main", [](id self, SEL) {
        auto& d = objc::ensure<ThreadData>(self);
        if (d.target) objc::send_sel(d.target, d.selector, {d.argument});
    });
    method(T, "cancel", [](id self, SEL) { objc::ensure<ThreadData>(self).cancelled = true; });
    method(T, "isCancelled", [](id self, SEL) { return objc::ensure<ThreadData>(self).cancelled; });
    method(T, "isExecuting", [](id self, SEL) { return objc::ensure<ThreadData>(self).executing; });
    method(T, "isFinished", [](id self, SEL) { return objc::ensure<ThreadData>(self).finished; });
    method(T, "isMainThread", [](id self, SEL) { return objc::ensure<ThreadData>(self).is_main; });
    method(T, "setName:", [](id self, SEL, id n) { objc::ensure<ThreadData>(self).name = utf8(n); });
    method(T, "name", [](id self, SEL) { return str(objc::ensure<ThreadData>(self).name); });
    method(T, "setStackSize:", [](id self, SEL, u64 s) { objc::ensure<ThreadData>(self).stack_size = s; });
    method(T, "stackSize", [](id self, SEL) -> u64 { return objc::ensure<ThreadData>(self).stack_size; });
    method(T, "setThreadPriority:", [](id self, SEL, double p) { objc::ensure<ThreadData>(self).priority = p; });
    method(T, "threadPriority", [](id self, SEL) { return objc::ensure<ThreadData>(self).priority; });
    method(T, "threadDictionary", [](id self, SEL) {
        auto& d = objc::ensure<ThreadData>(self);
        if (!d.dictionary) d.dictionary = objc::retain(mutable_dict());
        return d.dictionary;
    });

    // performSelector family (NSObject)
    Class O = objc::class_named("NSObject");
    method(O, "performSelectorOnMainThread:withObject:waitUntilDone:", [](id self, SEL, SEL s, id arg, bool wait) {
        if (wait) {
            run_on_main_sync([&] { objc::send_sel(self, s, {arg}); });
            return;
        }
        objc::retain(self);
        objc::retain(arg);
        post_to_main([self, s, arg] {
            objc::send_sel(self, s, {arg});
            objc::release(arg);
            objc::release(self);
        });
    });
    method(O, "performSelectorOnMainThread:withObject:waitUntilDone:modes:", [](id self, SEL, SEL s, id arg, bool wait, id) {
        objc::send(self, "performSelectorOnMainThread:withObject:waitUntilDone:", {s, arg, wait});
    });
    method(O, "performSelector:onThread:withObject:waitUntilDone:", [](id self, SEL, SEL s, id thread, id arg, bool wait) {
        RunLoop* rl = thread ? objc::ensure<ThreadData>(thread).runloop : nullptr;
        if (!rl || rl == &RunLoop::current()) {
            objc::send_sel(self, s, {arg});
            return;
        }
        objc::retain(self);
        objc::retain(arg);
        HANDLE done = wait ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
        rl->post([self, s, arg, done] {
            objc::send_sel(self, s, {arg});
            objc::release(arg);
            objc::release(self);
            if (done) SetEvent(done);
        });
        if (done) {
            WaitForSingleObject(done, INFINITE);
            CloseHandle(done);
        }
    });
    method(O, "performSelectorInBackground:withObject:", [](id self, SEL, SEL s, id arg) {
        id th = objc::alloc(g_nsthread);
        auto& d = objc::ensure<ThreadData>(th);
        d.target = objc::retain(self);
        d.selector = s;
        d.argument = objc::retain(arg);
        start_nsthread(th);
        objc::release(th);
    });
    method(O, "performSelector:withObject:afterDelay:", [](id self, SEL, SEL s, id arg, double delay) {
        auto* tm = new Timer;
        tm->target = objc::retain(self);
        tm->selector = s;
        tm->obj = objc::retain(arg);
        tm->fire_at = now_ref() + delay;
        tm->interval = 0;
        tm->repeats = false;
        tm->is_perform = true;
        RunLoop::current().add_timer(tm);
    });
    method(O, "performSelector:withObject:afterDelay:inModes:", [](id self, SEL, SEL s, id arg, double delay, id) {
        objc::send(self, "performSelector:withObject:afterDelay:", {s, arg});
        (void)delay;
    });
    class_method(O, "cancelPreviousPerformRequestsWithTarget:", [](Class, SEL, id target) {
        RunLoop::current().remove_timers_if([target](Timer* t) { return t->is_perform && t->target == target; });
    });
    class_method(O, "cancelPreviousPerformRequestsWithTarget:selector:object:", [](Class, SEL, id target, SEL s, id) {
        RunLoop::current().remove_timers_if([target, s](Timer* t) { return t->is_perform && t->target == target && t->selector == s; });
    });

    // NSRunLoop / CFRunLoop
    Class R = g_runloop_cls;
    auto runloop_obj = [](RunLoop& rl) -> id {
        if (!rl.nsobj) {
            rl.nsobj = objc::alloc(g_runloop_cls);
            objc::ensure<RunLoopRef>(rl.nsobj).rl = &rl;
        }
        return rl.nsobj;
    };
    static decltype(runloop_obj) s_rlobj = runloop_obj;
    auto rl_of = [](id self) -> RunLoop& {
        auto* r = objc::get<RunLoopRef>(self);
        return r ? *r->rl : RunLoop::current();
    };
    static decltype(rl_of) s_rl_of = rl_of;
    objc::host_class("NSRunLoop");
    class_method(R, "currentRunLoop", [](Class, SEL) { return s_rlobj(RunLoop::current()); });
    class_method(R, "mainRunLoop", [](Class, SEL) { return s_rlobj(RunLoop::main()); });
    method(R, "runMode:beforeDate:", [](id self, SEL, id, id date) {
        double limit = date ? date_value(date) : now_ref();
        s_rl_of(self).run_once(std::max(0.0, std::min(limit - now_ref(), 1.0)));
        return true;
    });
    method(R, "runUntilDate:", [](id self, SEL, id date) {
        double limit = date_value(date);
        do {
            s_rl_of(self).run_once(std::max(0.0, std::min(limit - now_ref(), 0.1)));
        } while (now_ref() < limit);
    });
    method(R, "run", [](id self, SEL) {
        for (;;) s_rl_of(self).run_once(1.0);
    });
    method(R, "addTimer:forMode:", [](id self, SEL, id timer, id) { schedule(s_rl_of(self), timer); });
    method(R, "currentMode", [](id, SEL) { return str("kCFRunLoopDefaultMode"); });
    method(R, "getCFRunLoop", [](id self, SEL) { return s_rl_of(self).cf; });
    hle::fn("_CFRunLoopGetCurrent", []() { return RunLoop::current().cf; });
    hle::fn("_CFRunLoopRunInMode", [](id, double seconds, bool) -> s32 {
        bool handled = RunLoop::current().run_once(std::max(0.0, seconds));
        return handled ? 4 : 3;  // kCFRunLoopRunHandledSource / TimedOut
    });
    for (const char* mode : {"_NSDefaultRunLoopMode", "_NSRunLoopCommonModes", "_kCFRunLoopDefaultMode"}) {
        std::string m = mode;
        hle::data_lazy(mode, [m] {
            auto* cell = static_cast<u64*>(hle::alloc_static(8));
            *cell = str_retained(m.substr(1));
            return gaddr(cell);
        });
    }

    // NSTimer
    Class TM = g_timer;
    class_method(TM, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
                 [](Class c, SEL, double interval, id target, SEL s, id info, bool repeats) {
                     id t = make_timer(c, interval, target, s, info, repeats);
                     schedule(RunLoop::current(), t);
                     return objc::autorelease(t);
                 });
    class_method(TM, "timerWithTimeInterval:target:selector:userInfo:repeats:",
                 [](Class c, SEL, double interval, id target, SEL s, id info, bool repeats) {
                     return objc::autorelease(make_timer(c, interval, target, s, info, repeats));
                 });
    method(TM, "invalidate", [](id self, SEL) {
        auto& td = objc::ensure<TimerData>(self);
        if (td.timer) td.timer->valid = false;
    });
    method(TM, "isValid", [](id self, SEL) {
        auto& td = objc::ensure<TimerData>(self);
        return td.timer && td.timer->valid;
    });
    method(TM, "fire", [](id self, SEL) {
        auto& td = objc::ensure<TimerData>(self);
        if (td.timer) td.timer->fire();
    });
    method(TM, "userInfo", [](id self, SEL) { return objc::ensure<TimerData>(self).user_info; });
    method(TM, "timeInterval", [](id self, SEL) {
        auto& td = objc::ensure<TimerData>(self);
        return td.timer ? td.timer->interval : 0.0;
    });

    // NSDate
    Class D = g_date;
    class_method(D, "date", [](Class c, SEL) { return objc::autorelease(new_date(c, now_ref())); });
    class_method(D, "dateWithTimeIntervalSinceNow:", [](Class c, SEL, double s) { return objc::autorelease(new_date(c, now_ref() + s)); });
    class_method(D, "dateWithTimeIntervalSince1970:", [](Class c, SEL, double s) { return objc::autorelease(new_date(c, s - 978307200.0)); });
    class_method(D, "dateWithTimeIntervalSinceReferenceDate:", [](Class c, SEL, double s) { return objc::autorelease(new_date(c, s)); });
    class_method(D, "distantFuture", [](Class c, SEL) { return objc::autorelease(new_date(c, 63113904000.0)); });
    class_method(D, "distantPast", [](Class c, SEL) { return objc::autorelease(new_date(c, -63114076800.0)); });
    class_method(D, "timeIntervalSinceReferenceDate", [](Class, SEL) { return now_ref(); });
    method(D, "init", [](id self, SEL) {
        objc::ensure<DateData>(self).t = now_ref();
        return self;
    });
    method(D, "initWithTimeIntervalSinceNow:", [](id self, SEL, double s) {
        objc::ensure<DateData>(self).t = now_ref() + s;
        return self;
    });
    method(D, "initWithTimeIntervalSince1970:", [](id self, SEL, double s) {
        objc::ensure<DateData>(self).t = s - 978307200.0;
        return self;
    });
    method(D, "initWithTimeIntervalSinceReferenceDate:", [](id self, SEL, double s) {
        objc::ensure<DateData>(self).t = s;
        return self;
    });
    method(D, "timeIntervalSinceNow", [](id self, SEL) { return date_value(self) - now_ref(); });
    method(D, "timeIntervalSince1970", [](id self, SEL) { return date_value(self) + 978307200.0; });
    method(D, "timeIntervalSinceReferenceDate", [](id self, SEL) { return date_value(self); });
    method(D, "timeIntervalSinceDate:", [](id self, SEL, id o) { return date_value(self) - date_value(o); });
    method(D, "dateByAddingTimeInterval:", [](id self, SEL, double s) { return objc::autorelease(new_date(g_date, date_value(self) + s)); });
    method(D, "compare:", [](id self, SEL, id o) -> s64 {
        double a = date_value(self), b = date_value(o);
        return a < b ? -1 : a > b ? 1 : 0;
    });
    method(D, "isEqualToDate:", [](id self, SEL, id o) { return date_value(self) == date_value(o); });
    method(D, "earlierDate:", [](id self, SEL, id o) { return date_value(self) <= date_value(o) ? self : o; });
    method(D, "laterDate:", [](id self, SEL, id o) { return date_value(self) >= date_value(o) ? self : o; });
    method(D, "copyWithZone:", [](id self, SEL, u64) { return objc::retain(self); });
    method(D, "description", [](id self, SEL) {
        char buf[64];
        snprintf(buf, sizeof buf, "<NSDate %.3f>", date_value(self));
        return str(buf);
    });

    // ---- GCD ----
    g_main_queue = new_queue("com.apple.main-thread", true);
    g_main_queue->main = true;
    hle::data("__dispatch_main_q", gaddr(g_main_queue));
    for (int i = 0; i < 4; i++) g_global_queues[i] = new_queue("com.apple.root.global", false);
    using hle::fn;
    fn("_dispatch_get_global_queue", [](s64 prio, u64) {
        int i = prio == 2 ? 0 : prio == 0 ? 1 : prio == -2 ? 2 : 3;
        return gaddr(g_global_queues[i]);
    });
    fn("_dispatch_get_current_queue", []() { return gaddr(is_main_thread() ? g_main_queue : t_current_queue ? t_current_queue : g_global_queues[1]); });
    fn("_dispatch_queue_create", [](const char* label, u64 attr) { return gaddr(new_queue(label, attr == 0)); });
    fn("_dispatch_release", [](u64) {});
    fn("_dispatch_set_target_queue", [](u64, u64) {});
    fn("_dispatch_async", [](Queue* q, GuestAddr block) {
        GuestAddr b = objc::block_copy(block);
        enqueue(q, [b] {
            objc::call_block(b);
            objc::block_release(b);
        });
    });
    fn("_dispatch_sync", [](Queue* q, GuestAddr block) {
        if (!q) q = g_global_queues[0];
        bool inline_ok = (q->main && is_main_thread()) || !q->serial || t_current_queue == q ||
                         (q->owner_tid && q->owner_tid == GetCurrentThreadId());
        if (inline_ok) {
            objc::call_block(block);
            return;
        }
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        enqueue(q, [block, done] {
            objc::call_block(block);
            SetEvent(done);
        });
        WaitForSingleObject(done, INFINITE);
        CloseHandle(done);
    });
    fn("_dispatch_time", [](u64 when, s64 delta) -> u64 {
        if (when == ~0ull) return ~0ull;
        u64 base = when == 0 ? now_ns() : when;
        return (u64)((s64)base + delta);
    });
    fn("_dispatch_after", [](u64 when, Queue* q, GuestAddr block) {
        GuestAddr b = objc::block_copy(block);
        u64 now = now_ns();
        double delay = when > now ? (double)(when - now) / 1e9 : 0.0;
        auto* tm = new Timer;
        tm->fire_at = now_ref() + delay;
        tm->fn = [q, b] {
            enqueue(q, [b] {
                objc::call_block(b);
                objc::block_release(b);
            });
        };
        RunLoop::main().add_timer(tm);
    });
    fn("_dispatch_once", [](s64* pred, GuestAddr block) {
        if (once_begin(pred)) {
            objc::call_block(block);
            once_end(pred);
        }
    });
    fn("_dispatch_once_f", [](s64* pred, u64 ctx, GuestAddr func) {
        if (once_begin(pred)) {
            cpu::current().call(func, {ctx});
            once_end(pred);
        }
    });
    fn("_dispatch_semaphore_create", [](s64 n) -> Semaphore* {
        auto* s = new Semaphore;
        s->count = n;
        return s;
    });
    fn("_dispatch_semaphore_signal", [](Semaphore* s) -> s64 {
        {
            std::lock_guard lock(s->m);
            s->count++;
        }
        s->cv.notify_one();
        return 0;
    });
    fn("_dispatch_semaphore_wait", [](Semaphore* s, u64 timeout) -> s64 {
        std::unique_lock lock(s->m);
        auto ready = [s] { return s->count > 0; };
        if (timeout == ~0ull) s->cv.wait(lock, ready);
        else {
            u64 now = now_ns();
            u64 wait_ns = timeout > now ? timeout - now : 0;
            if (!s->cv.wait_for(lock, std::chrono::nanoseconds(wait_ns), ready)) return 1;
        }
        s->count--;
        return 0;
    });
}

double date_seconds(id d) { return date_value(d); }

void install_thread_late() { g_main_nsthread = objc::retain(current_nsthread()); }

}  // namespace ns
