// pthreads, OSAtomic, OSSpinLock, mach thread ports.
#include "libc/pthread.h"
#include "hle.h"
#include "modules.h"
#include <atomic>
#include <mutex>
#include <process.h>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace libc {

namespace {

constexpr int D_EINVAL = 22, D_ESRCH = 3, D_ETIMEDOUT = 60, D_EBUSY = 16;
constexpr u64 kThreadSig = 0x54485244;  // our pthread_t block signature

struct PThread {
    GuestAddr handle = 0;  // guest pthread_t (points at an 8 KiB guest block, like Darwin)
    HANDLE host = nullptr;
    u64 retval = 0;
    bool detached = false;
    u32 port = 0;
    std::vector<u64> tsd;
    std::string name;
};

std::mutex g_threads_mutex;
std::unordered_map<GuestAddr, PThread*> g_threads;
std::atomic<u32> g_next_port{0x1103};
thread_local PThread* t_self = nullptr;

PThread* new_pthread() {
    auto* th = new PThread;
    th->handle = gaddr(std::calloc(1, 8192));
    *gptr<u64>(th->handle) = kThreadSig;
    th->port = g_next_port += 4;
    std::lock_guard lock(g_threads_mutex);
    g_threads[th->handle] = th;
    return th;
}

PThread* self() {
    if (!t_self) t_self = new_pthread();
    return t_self;
}

PThread* find(GuestAddr h) {
    std::lock_guard lock(g_threads_mutex);
    auto it = g_threads.find(h);
    return it == g_threads.end() ? nullptr : it->second;
}

// --- keys ---
constexpr u32 kMaxKeys = 512;
std::atomic<u32> g_next_key{1};
GuestAddr g_key_dtors[kMaxKeys];

void run_tsd_destructors(PThread* th) {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (u32 k = 1; k < th->tsd.size(); k++) {
            u64 v = th->tsd[k];
            if (v && g_key_dtors[k]) {
                th->tsd[k] = 0;
                cpu::current().call(g_key_dtors[k], {v});
                any = true;
            }
        }
        if (!any) break;
    }
}

// --- mutex / cond laid out inside the guest objects ---
struct MutexBody {  // at offset 8 of pthread_mutex_t (56 bytes available)
    SRWLOCK lock;
    std::atomic<u32> owner;
    u32 count;
};
static_assert(sizeof(MutexBody) <= 56);
MutexBody* body(GuestAddr m) { return gptr<MutexBody>(m + 8); }

void mutex_lock(GuestAddr m) {
    MutexBody* b = body(m);
    u32 tid = GetCurrentThreadId();
    if (b->owner.load(std::memory_order_relaxed) == tid) {
        b->count++;
        return;
    }
    AcquireSRWLockExclusive(&b->lock);
    b->owner.store(tid, std::memory_order_relaxed);
    b->count = 1;
}
bool mutex_trylock(GuestAddr m) {
    MutexBody* b = body(m);
    u32 tid = GetCurrentThreadId();
    if (b->owner.load(std::memory_order_relaxed) == tid) {
        b->count++;
        return true;
    }
    if (!TryAcquireSRWLockExclusive(&b->lock)) return false;
    b->owner.store(tid, std::memory_order_relaxed);
    b->count = 1;
    return true;
}
void mutex_unlock(GuestAddr m) {
    MutexBody* b = body(m);
    if (b->owner.load(std::memory_order_relaxed) != GetCurrentThreadId()) {
        LOG_WARN("pthread_mutex_unlock of mutex 0x%llx not owned by caller", (unsigned long long)m);
        return;
    }
    if (--b->count == 0) {
        b->owner.store(0, std::memory_order_relaxed);
        ReleaseSRWLockExclusive(&b->lock);
    }
}
CONDITION_VARIABLE* condvar(GuestAddr c) { return gptr<CONDITION_VARIABLE>(c + 8); }

int cond_wait(GuestAddr c, GuestAddr m, DWORD ms) {
    MutexBody* b = body(m);
    u32 saved = b->count;
    b->owner.store(0, std::memory_order_relaxed);
    b->count = 0;
    BOOL ok = SleepConditionVariableSRW(condvar(c), &b->lock, ms, 0);
    b->owner.store(GetCurrentThreadId(), std::memory_order_relaxed);
    b->count = saved;
    return ok ? 0 : D_ETIMEDOUT;
}

unsigned __stdcall thread_entry(void* p) {
    auto* start = static_cast<std::pair<PThread*, std::function<void()>>*>(p);
    t_self = start->first;
    logging::set_thread_name(start->first->name.c_str());
    LOG_DEBUG("thread started: %s", start->first->name.c_str());
    if (!start->first->name.empty()) {
        std::wstring w(start->first->name.begin(), start->first->name.end());
        SetThreadDescription(GetCurrentThread(), w.c_str());
    }
    start->second();
    run_tsd_destructors(t_self);
    delete start;
    return 0;
}

}  // namespace

GuestAddr spawn_guest_thread(const char* name, std::function<void()> body_fn, size_t guest_stack) {
    PThread* th = new_pthread();
    th->name = name ? name : "";
    auto* start = new std::pair<PThread*, std::function<void()>>(th, [body_fn, guest_stack] {
        cpu::Thread cpu(guest_stack);
        body_fn();
    });
    // Host stack must hold nested HLE <-> guest frames and dynarmic's own usage.
    th->host = (HANDLE)_beginthreadex(nullptr, 16 << 20, thread_entry, start, 0, nullptr);
    if (!th->host) fatal("could not start host thread");
    return th->handle;
}

GuestAddr pthread_self_handle() { return self()->handle; }

void guest_mutex_lock(GuestAddr m) { mutex_lock(m); }
void guest_mutex_unlock(GuestAddr m) { mutex_unlock(m); }

void install_pthread() {
    using hle::fn;

    // threads
    fn("_pthread_create", [](GuestAddr* out, GuestAddr attr, GuestAddr start, u64 arg) {
        u64 stack = attr ? std::max<u64>(*gptr<u64>(attr + 8), 1 << 20) : (1 << 20);
        bool detached = attr && *gptr<u32>(attr + 16) == 2;
        GuestAddr h = spawn_guest_thread("guest", [start, arg] {
            u64 r = cpu::current().call(start, {arg});
            t_self->retval = r;
        }, stack);
        PThread* th = find(h);
        th->detached = detached;
        *out = h;
        LOG_DEBUG("pthread_create start=%s -> 0x%llx\n%s", cpu::symbolize(start).c_str(), (unsigned long long)h,
                 cpu::current().backtrace().c_str());
        return 0;
    });
    fn("_pthread_join", [](GuestAddr h, u64* retval) {
        PThread* th = find(h);
        if (!th || !th->host) return D_ESRCH;
        WaitForSingleObject(th->host, INFINITE);
        if (retval) *retval = th->retval;
        return 0;
    });
    fn("_pthread_detach", [](GuestAddr h) {
        if (PThread* th = find(h)) th->detached = true;
        return 0;
    });
    fn("_pthread_self", []() { return self()->handle; });
    fn("_pthread_mach_thread_np", [](GuestAddr h) -> u32 {
        PThread* th = find(h);
        return th ? th->port : 0;
    });
    fn("_mach_thread_self", []() -> u32 { return self()->port; });

    // attributes: +8 stack size, +16 detach state
    fn("_pthread_attr_init", [](GuestAddr a) {
        std::memset(gptr<void>(a), 0, 64);
        *gptr<u64>(a) = 0x54485244;
        *gptr<u64>(a + 8) = 512 * 1024;
        *gptr<u32>(a + 16) = 1;
        return 0;
    });
    fn("_pthread_attr_destroy", [](GuestAddr) { return 0; });
    fn("_pthread_attr_setdetachstate", [](GuestAddr a, int s) {
        *gptr<u32>(a + 16) = (u32)s;
        return 0;
    });

    // mutexes (all recursive: a superset of normal semantics)
    fn("_pthread_mutexattr_init", [](GuestAddr a) {
        std::memset(gptr<void>(a), 0, 16);
        return 0;
    });
    fn("_pthread_mutexattr_settype", [](GuestAddr a, int type) {
        *gptr<u32>(a + 8) = (u32)type;
        return 0;
    });
    fn("_pthread_mutex_init", [](GuestAddr m, GuestAddr) {
        std::memset(gptr<void>(m), 0, 64);
        *gptr<u64>(m) = 0x4d334249;
        return 0;
    });
    fn("_pthread_mutex_destroy", [](GuestAddr) { return 0; });
    fn("_pthread_mutex_lock", [](GuestAddr m) {
        mutex_lock(m);
        return 0;
    });
    fn("_pthread_mutex_trylock", [](GuestAddr m) { return mutex_trylock(m) ? 0 : D_EBUSY; });
    fn("_pthread_mutex_unlock", [](GuestAddr m) {
        mutex_unlock(m);
        return 0;
    });

    // condition variables: CONDITION_VARIABLE at +8 (zero == initialized)
    fn("_pthread_cond_init", [](GuestAddr c, GuestAddr) {
        std::memset(gptr<void>(c), 0, 48);
        return 0;
    });
    fn("_pthread_cond_destroy", [](GuestAddr) { return 0; });
    fn("_pthread_cond_wait", [](GuestAddr c, GuestAddr m) { return cond_wait(c, m, INFINITE); });
    fn("_pthread_cond_timedwait", [](GuestAddr c, GuestAddr m, const s64* abstime) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        s64 now_ns = (s64)(((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ull) * 100;
        s64 target_ns = abstime[0] * 1000000000ll + abstime[1];
        // Round up: UE3's FEventIPhone::Wait asks for sub-millisecond waits, and truncating them to
        // 0 turned its wait loop into a spin (hundreds of thousands of calls per second).
        s64 ms = (target_ns - now_ns + 999999) / 1000000;
        return cond_wait(c, m, ms <= 0 ? 0 : (DWORD)ms);
    });
    fn("_pthread_cond_signal", [](GuestAddr c) {
        WakeConditionVariable(condvar(c));
        return 0;
    });
    fn("_pthread_cond_broadcast", [](GuestAddr c) {
        WakeAllConditionVariable(condvar(c));
        return 0;
    });

    // thread-specific data
    fn("_pthread_key_create", [](u64* key, GuestAddr dtor) {
        u32 k = g_next_key++;
        if (k >= kMaxKeys) return 35;  // EAGAIN
        g_key_dtors[k] = dtor;
        *key = k;
        return 0;
    });
    fn("_pthread_key_delete", [](u64 key) {
        if (key < kMaxKeys) g_key_dtors[key] = 0;
        return 0;
    });
    fn("_pthread_getspecific", [](u64 key) -> u64 {
        PThread* th = self();
        return key < th->tsd.size() ? th->tsd[key] : 0;
    });
    fn("_pthread_setspecific", [](u64 key, u64 value) {
        if (key >= kMaxKeys) return D_EINVAL;
        PThread* th = self();
        if (th->tsd.size() <= key) th->tsd.resize(kMaxKeys);
        th->tsd[key] = value;
        return 0;
    });

    // libkern atomics
    fn("_OSAtomicAdd32", [](s32 amt, s32* p) -> s32 { return __atomic_add_fetch(p, amt, __ATOMIC_SEQ_CST); });
    fn("_OSAtomicAdd32Barrier", [](s32 amt, s32* p) -> s32 { return __atomic_add_fetch(p, amt, __ATOMIC_SEQ_CST); });
    fn("_OSAtomicCompareAndSwap32", [](s32 o, s32 n, s32* p) {
        return __atomic_compare_exchange_n(p, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    });
    fn("_OSAtomicCompareAndSwap32Barrier", [](s32 o, s32 n, s32* p) {
        return __atomic_compare_exchange_n(p, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    });
    fn("_OSAtomicCompareAndSwapPtrBarrier", [](u64 o, u64 n, u64* p) {
        return __atomic_compare_exchange_n(p, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    });
    fn("_OSMemoryBarrier", []() { __atomic_thread_fence(__ATOMIC_SEQ_CST); });
    fn("_OSSpinLockLock", [](s32* l) {
        for (int spins = 0;; spins++) {
            s32 expected = 0;
            if (__atomic_compare_exchange_n(l, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            if (spins > 100) SwitchToThread();
        }
    });
    fn("_OSSpinLockUnlock", [](s32* l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); });

    // mach thread control (used by crash reporting / profiling; not supported)
    fn("_thread_policy_set", [](u32, u32, void*, u32) { return 0; });
    fn("_thread_info", [](u32, u32, void*, u32*) { return 5; });
    fn("_thread_suspend", [](u32) { return 5; });
    fn("_thread_resume", [](u32) { return 5; });
    fn("_thread_get_state", [](u32, u32, void*, u32*) { return 5; });
    fn("_task_threads", [](u32, void*, u32* count) {
        if (count) *count = 0;
        return 5;
    });
}

}  // namespace libc
