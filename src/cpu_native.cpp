// Native ARM64 backend (Android): the game's code runs directly on the CPU, on the calling
// host thread's own stack. See cpu_native.S for the register save/restore glue.
#include "cpu.h"
#if IB3_NATIVE_CPU
#include "macho.h"
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" void ib3_hle_entry();
extern "C" void ib3_guest_call(cpu::Context* ctx, u64 fn);

namespace cpu {

struct Context {
    u64 x[31];
    u64 sp;
    Vec128 q[16];
    u64 target;
    u64 index;
    u64 stack_args;
    u64 nstack;
};
static_assert(offsetof(Context, sp) == 248 && offsetof(Context, q) == 256 && offsetof(Context, target) == 512 &&
              offsetof(Context, index) == 520 && offsetof(Context, stack_args) == 528 &&
              offsetof(Context, nstack) == 536 && sizeof(Context) == 544);

namespace {

constexpr size_t kMaxStubs = 1 << 18;
constexpr size_t kStubSize = 32;

u8* g_stubs = nullptr;
std::vector<Handler> g_handlers;
std::vector<std::string> g_stub_names;
std::mutex g_stub_mutex;
std::atomic<size_t> g_stub_count{0};
bool g_profile = false;
std::unique_ptr<std::atomic<u64>[]> g_call_counts;

const macho::Image* g_image = nullptr;
std::atomic<u32> g_next_thread_id{0};
thread_local Thread* t_current = nullptr;
std::mutex g_threads_mutex;
std::vector<Thread*> g_threads;

// Reads guest memory without faulting (for backtraces through possibly bad frame pointers).
bool safe_read(u64 addr, void* out, size_t n) {
    iovec local{out, n}, remote{reinterpret_cast<void*>(addr), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

std::string frames_from(u64 pc, u64 lr, u64 fp) {
    std::string out;
    char line[256];
    snprintf(line, sizeof line, "  pc  0x%llx %s\n  lr  0x%llx %s\n", (unsigned long long)pc, symbolize(pc).c_str(),
             (unsigned long long)lr, symbolize(lr).c_str());
    out += line;
    for (int i = 0; i < 32 && fp; i++) {
        u64 rec[2];
        if (!safe_read(fp, rec, sizeof rec) || rec[1] == 0) break;
        snprintf(line, sizeof line, "  #%-2d 0x%llx %s\n", i, (unsigned long long)rec[1], symbolize(rec[1]).c_str());
        out += line;
        if (rec[0] <= fp) break;
        fp = rec[0];
    }
    return out;
}

void crash_handler(int sig, siginfo_t* info, void* uc_) {
    auto* uc = static_cast<ucontext_t*>(uc_);
    const auto& m = uc->uc_mcontext;
    Thread* t = t_current;
    const char* hle = t ? t->in_hle.load() : nullptr;
    LOG_ERROR("CRASH: signal %d at 0x%llx (address 0x%llx)%s%s\n%s", sig, (unsigned long long)m.pc,
              (unsigned long long)info->si_addr, hle ? " inside HLE " : "", hle ? hle : "",
              frames_from(m.pc, m.regs[30], m.regs[29]).c_str());
    signal(sig, SIG_DFL);
    raise(sig);
}

}  // namespace

std::string symbolize(GuestAddr addr) {
    if (is_stub(addr)) return "stub:" + stub_name(addr);
    if (g_image) {
        auto s = g_image->symbolize(addr);
        if (!s.empty()) return s;
    }
    return {};
}

GuestAddr make_stub(const std::string& name, Handler h) {
    std::lock_guard lock(g_stub_mutex);
    size_t i = g_stub_count.load();
    if (i >= kMaxStubs) fatal("out of HLE stubs");
    g_handlers[i] = std::move(h);
    g_stub_names[i] = name;
    u32* code = reinterpret_cast<u32*>(g_stubs + i * kStubSize);
    code[0] = 0xD2800010 | ((u32)(i & 0xffff) << 5);          // movz x16, #index_lo
    code[1] = 0xF2A00010 | ((u32)((i >> 16) & 0xffff) << 5);  // movk x16, #index_hi, lsl #16
    code[2] = 0x58000051;                                     // ldr  x17, #8
    code[3] = 0xD61F0220;                                     // br   x17
    *reinterpret_cast<u64*>(&code[4]) = reinterpret_cast<u64>(&ib3_hle_entry);
    __builtin___clear_cache(reinterpret_cast<char*>(code), reinterpret_cast<char*>(code) + kStubSize);
    g_stub_count.store(i + 1);
    return gaddr(code);
}

void* alloc_code(size_t bytes) {
    static std::mutex m;
    static u8* cur = nullptr;
    static size_t left = 0;
    std::lock_guard lock(m);
    bytes = (bytes + 15) & ~size_t(15);
    if (bytes > left) {
        size_t chunk = std::max<size_t>(bytes, 64 << 10);
        void* p = mmap(nullptr, chunk, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) fatal("cannot allocate executable memory");
        cur = static_cast<u8*>(p);
        left = chunk;
    }
    void* r = cur;
    cur += bytes;
    left -= bytes;
    return r;
}

bool is_stub(GuestAddr a) {
    return a >= gaddr(g_stubs) && a < gaddr(g_stubs) + kStubSize * g_stub_count.load();
}

const std::string& stub_name(GuestAddr a) { return g_stub_names[(a - gaddr(g_stubs)) / kStubSize]; }

void init(const macho::Image* image) {
    g_image = image;
    // Stubs are written while other threads may be running earlier ones: keep the region RWX.
    void* p = mmap(nullptr, kMaxStubs * kStubSize, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) fatal("cannot allocate the HLE stub region");
    g_stubs = static_cast<u8*>(p);
    g_handlers.resize(kMaxStubs);
    g_call_counts.reset(new std::atomic<u64>[kMaxStubs]());
    g_stub_names.resize(kMaxStubs);
    // The image was loaded as data; make its code executable (and writable, for engine hooks).
    if (image)
        for (auto& s : image->sections)
            if (s.segname == "__TEXT") {
                u64 lo = s.addr & ~0xfffull, hi = (s.addr + s.size + 0xfff) & ~0xfffull;
                if (mprotect(gptr<void>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
                    fatal("cannot make the game's code executable");
                __builtin___clear_cache(gptr<char>(lo), gptr<char>(hi));
            }
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
}

const macho::Image* image() { return g_image; }

void dispatch(Context* c) {
    Thread& t = current();
    Context* outer = t.ctx_;
    t.ctx_ = c;
    size_t idx = c->index;
    const char* prev_hle = t.in_hle.exchange(g_stub_names[idx].c_str());
    if (g_profile) g_call_counts[idx].fetch_add(1, std::memory_order_relaxed);
    LOG_TRACE("-> %s (lr=0x%llx)", g_stub_names[idx].c_str(), (unsigned long long)c->x[30]);
    g_handlers[idx](t);
    t.in_hle = prev_hle;
    t.ctx_ = outer;
}

Thread::Thread(size_t stack_size) {
    id_ = g_next_thread_id++;
    // Guest code runs on this host thread's own stack.
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* base = nullptr;
        size_t size = 0;
        pthread_attr_getstack(&attr, &base, &size);
        pthread_attr_destroy(&attr);
        stack_base_ = gaddr(base);
        stack_size_ = size;
    }
    (void)stack_size;
    tpidrro = gaddr(std::calloc(1, 4096));
    // Signal handlers run on their own stack, so they never touch the red zone below a guest leaf function.
    stack_t ss{};
    ss.ss_sp = std::malloc(64 << 10);
    ss.ss_size = 64 << 10;
    sigaltstack(&ss, nullptr);
    if (t_current) fatal("host thread already has a guest context");
    t_current = this;
    host_tid = (unsigned)gettid();
    std::lock_guard lock(g_threads_mutex);
    g_threads.push_back(this);
}

Thread::~Thread() {
    if (t_current == this) t_current = nullptr;
    std::lock_guard lock(g_threads_mutex);
    std::erase(g_threads, this);
}

u64 Thread::x(int i) const { return ctx_->x[i]; }
void Thread::set_x(int i, u64 v) { ctx_->x[i] = v; }
Vec128 Thread::v(int i) const { return ctx_->q[i]; }
void Thread::set_v(int i, Vec128 v) { ctx_->q[i] = v; }
double Thread::d(int i) const {
    double d;
    std::memcpy(&d, &ctx_->q[i].lo, 8);
    return d;
}
float Thread::s(int i) const {
    float f;
    std::memcpy(&f, &ctx_->q[i].lo, 4);
    return f;
}
void Thread::set_d(int i, double v) {
    u64 bits;
    std::memcpy(&bits, &v, 8);
    ctx_->q[i] = {bits, 0};
}
void Thread::set_s(int i, float v) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    ctx_->q[i] = {bits, 0};
}
u64 Thread::sp() const { return ctx_->sp; }
void Thread::set_sp(u64 v) { ctx_->sp = v; }
u64 Thread::pc() const { return ctx_ ? ctx_->x[30] : 0; }
void Thread::jump(u64 addr) { ctx_->target = addr; }

void Thread::run_until_return() { fatal("run_until_return is not used by the native backend"); }

void Thread::call_raw(GuestAddr fn, const std::function<void(Thread&)>& setup) {
    Context call{};
    // Stack arguments are written below a scratch "stack top" by setup (see call()) and copied
    // onto the real stack by ib3_guest_call.
    alignas(16) u64 scratch[64];
    u64 top = gaddr(&scratch[64]);
    call.sp = top;
    Context* outer = ctx_;
    ctx_ = &call;
    setup(*this);
    ctx_ = outer;
    call.nstack = (top - call.sp) / 8;
    call.stack_args = call.sp;
    const char* outer_hle = in_hle.exchange(nullptr);  // guest code again, even if called from HLE
    ib3_guest_call(&call, fn);
    in_hle = outer_hle;
    last_x0_ = call.x[0];
    last_x1_ = call.x[1];
    last_v0_ = call.q[0];
}

u64 Thread::call(GuestAddr fn, std::initializer_list<u64> args) {
    call_raw(fn, [&](Thread& t) {
        int i = 0;
        size_t nstack = args.size() > 8 ? args.size() - 8 : 0;
        if (nstack) t.set_sp((t.sp() - 8 * nstack) & ~15ull);
        for (u64 a : args) {
            if (i < 8) t.set_x(i, a);
            else *gptr<u64>(t.sp() + 8 * (i - 8)) = a;
            i++;
        }
    });
    return last_x0_;
}

std::string Thread::backtrace() const {
    if (!ctx_) return "  (no guest frame)\n";
    return frames_from(ctx_->x[30], ctx_->x[30], ctx_->x[29]);
}

Thread& current() {
    if (!t_current) new Thread();
    return *t_current;
}
Thread* current_or_null() { return t_current; }

void enable_profiling() { g_profile = true; }

void profile_report(double seconds, const std::unordered_map<std::string, u64>& samples, u64 total_samples) {
    std::vector<std::pair<u64, size_t>> top;
    size_t n = g_stub_count.load();
    for (size_t i = 0; i < n; i++)
        if (u64 c = g_call_counts[i].exchange(0)) top.push_back({c, i});
    std::sort(top.rbegin(), top.rend());
    std::string out = "==== HLE calls\n";
    char line[160];
    for (size_t k = 0; k < top.size() && k < 25; k++) {
        snprintf(line, sizeof line, "  %10.0f/s  %s\n", top[k].first / seconds, g_stub_names[top[k].second].c_str());
        out += line;
    }
    LOG_INFO("%s", out.c_str());
}

void profile_sample(std::unordered_map<std::string, u64>& samples) {
    std::lock_guard lock(g_threads_mutex);
    for (Thread* t : g_threads) {
        const char* hle = t->in_hle.load();
        samples[std::string("t") + std::to_string(t->id()) + " " + (hle ? hle : "(game code)")]++;
    }
}

void dump_all_threads() {
    std::lock_guard lock(g_threads_mutex);
    LOG_INFO("==== %zu guest threads ====", g_threads.size());
    for (Thread* t : g_threads) {
        const char* hle = t->in_hle.load();
        LOG_INFO("thread %u (guest id %u): %s%s", t->host_tid, t->id(), hle ? "inside " : "running game code",
                 hle ? hle : "");
    }
}

}  // namespace cpu

extern "C" void ib3_hle_dispatch(cpu::Context* c) { cpu::dispatch(c); }
#endif
