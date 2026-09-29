#include "cpu.h"
#include "macho.h"
#include <atomic>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <windows.h>
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/A64/config.h>
#include <dynarmic/interface/exclusive_monitor.h>

namespace cpu {

namespace {

constexpr u32 SVC0 = 0xd4000001;
constexpr size_t kMaxStubs = 1 << 18;
constexpr size_t kMaxThreads = 256;

GuestAddr g_stub_base = 0;
std::vector<Handler> g_handlers;
std::vector<std::string> g_stub_names;
std::mutex g_stub_mutex;
std::atomic<size_t> g_stub_count{0};
GuestAddr g_return_trap = 0;
bool g_profile = false;
std::unique_ptr<std::atomic<u64>[]> g_call_counts;

const macho::Image* g_image = nullptr;
Dynarmic::ExclusiveMonitor* g_monitor = nullptr;
std::atomic<u32> g_next_thread_id{0};
thread_local Thread* t_current = nullptr;
std::mutex g_threads_mutex;
std::vector<Thread*> g_threads;

LARGE_INTEGER g_qpc_freq;

bool readable(u64 addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(gptr<void>(addr), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize || readable(addr + n - 1, 1);
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

struct Thread::CallbacksImpl final : Dynarmic::A64::UserCallbacks {
    Thread* t;
    explicit CallbacksImpl(Thread* t) : t(t) {}

    [[noreturn]] void bad_access(u64 vaddr, const char* what) {
        fatal("guest %s fault at 0x%llx, pc=0x%llx (%s)\n%s", what, (unsigned long long)vaddr,
              (unsigned long long)t->pc(), symbolize(t->pc()).c_str(), t->backtrace().c_str());
    }
    template <typename T>
    T read(u64 vaddr) {
        if (!readable(vaddr, sizeof(T))) bad_access(vaddr, "read");
        T v;
        std::memcpy(&v, gptr<void>(vaddr), sizeof(T));
        return v;
    }
    template <typename T>
    void write(u64 vaddr, T v) {
        if (!readable(vaddr, sizeof(T))) bad_access(vaddr, "write");
        std::memcpy(gptr<void>(vaddr), &v, sizeof(T));
    }
    template <typename T>
    bool write_excl(u64 vaddr, T v, T expected) {
        if (!readable(vaddr, sizeof(T))) bad_access(vaddr, "exclusive write");
        return __atomic_compare_exchange_n(gptr<T>(vaddr), &expected, v, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    std::optional<u32> MemoryReadCode(u64 vaddr) override {
        if (!readable(vaddr, 4)) return std::nullopt;
        return *gptr<u32>(vaddr);
    }
    u8 MemoryRead8(u64 a) override { return read<u8>(a); }
    u16 MemoryRead16(u64 a) override { return read<u16>(a); }
    u32 MemoryRead32(u64 a) override { return read<u32>(a); }
    u64 MemoryRead64(u64 a) override { return read<u64>(a); }
    Dynarmic::A64::Vector MemoryRead128(u64 a) override { return read<Dynarmic::A64::Vector>(a); }
    void MemoryWrite8(u64 a, u8 v) override { write(a, v); }
    void MemoryWrite16(u64 a, u16 v) override { write(a, v); }
    void MemoryWrite32(u64 a, u32 v) override { write(a, v); }
    void MemoryWrite64(u64 a, u64 v) override { write(a, v); }
    void MemoryWrite128(u64 a, Dynarmic::A64::Vector v) override { write(a, v); }
    bool MemoryWriteExclusive8(u64 a, u8 v, u8 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive16(u64 a, u16 v, u16 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive32(u64 a, u32 v, u32 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive64(u64 a, u64 v, u64 e) override { return write_excl(a, v, e); }
    bool MemoryWriteExclusive128(u64 a, Dynarmic::A64::Vector v, Dynarmic::A64::Vector e) override {
        if (!readable(a, 16)) bad_access(a, "exclusive write");
        return __atomic_compare_exchange(gptr<unsigned __int128>(a), reinterpret_cast<unsigned __int128*>(&e),
                                         reinterpret_cast<unsigned __int128*>(&v), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    void InterpreterFallback(u64 pc, size_t n) override {
        fatal("interpreter fallback at 0x%llx (%s) x%zu", (unsigned long long)pc, symbolize(pc).c_str(), n);
    }
    void CallSVC(u32) override {
        t->svc_pending = true;
        t->jit_->HaltExecution(Dynarmic::HaltReason::UserDefined1);
    }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception e) override {
        using E = Dynarmic::A64::Exception;
        switch (e) {
        case E::Yield:
        case E::WaitForEvent:
        case E::WaitForInterrupt:
        case E::SendEvent:
        case E::SendEventLocal:
            t->jit_->SetPC(pc + 4);
            return;
        default:
            break;
        }
        fatal("guest exception %d at 0x%llx (%s)\n%s", (int)e, (unsigned long long)pc, symbolize(pc).c_str(),
              t->backtrace().c_str());
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return ~0ull >> 2; }
    u64 GetCNTPCT() override {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        // 24 MHz, like the A7's timebase.
        return (u64)((unsigned __int128)now.QuadPart * 24000000 / g_qpc_freq.QuadPart);
    }
};

GuestAddr make_stub(const std::string& name, Handler h) {
    std::lock_guard lock(g_stub_mutex);
    size_t i = g_stub_count.load();
    if (i >= kMaxStubs) fatal("out of HLE stubs");
    g_handlers[i] = std::move(h);
    g_stub_names[i] = name;
    gptr<u32>(g_stub_base)[i] = SVC0;
    g_stub_count.store(i + 1);
    return g_stub_base + 4 * i;
}

bool is_stub(GuestAddr a) { return a >= g_stub_base && a < g_stub_base + 4 * g_stub_count.load(); }

const std::string& stub_name(GuestAddr a) { return g_stub_names[(a - g_stub_base) / 4]; }

void init(const macho::Image* image) {
    g_image = image;
    QueryPerformanceFrequency(&g_qpc_freq);
    g_stub_base = gaddr(VirtualAlloc(nullptr, kMaxStubs * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_handlers.resize(kMaxStubs);
    g_call_counts.reset(new std::atomic<u64>[kMaxStubs]());
    g_stub_names.resize(kMaxStubs);
    g_monitor = new Dynarmic::ExclusiveMonitor(kMaxThreads);
    g_return_trap = make_stub("<return to host>", [](Thread&) { fatal("return trap dispatched"); });
}

const macho::Image* image() { return g_image; }

Thread::Thread(size_t stack_size) {
    id_ = g_next_thread_id++;
    if (id_ >= kMaxThreads) fatal("too many guest threads");
    callbacks_ = std::make_unique<CallbacksImpl>(this);

    Dynarmic::A64::UserConfig cfg;
    cfg.callbacks = callbacks_.get();
    cfg.processor_id = id_;
    cfg.global_monitor = g_monitor;
    cfg.fastmem_pointer = uintptr_t{0};  // guest address == host address
    cfg.fastmem_address_space_bits = 48;
    cfg.silently_mirror_fastmem = false;
    cfg.fastmem_exclusive_access = true;
    cfg.enable_cycle_counting = false;
    cfg.cntfrq_el0 = 24000000;
    cfg.tpidrro_el0 = &tpidrro;
    cfg.tpidr_el0 = &tpidr;
    cfg.define_unpredictable_behaviour = true;
    cfg.code_cache_size = 256ull << 20;
    jit_ = std::make_unique<Dynarmic::A64::Jit>(cfg);

    stack_size_ = stack_size;
    stack_base_ = gaddr(VirtualAlloc(nullptr, stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!stack_base_) fatal("cannot allocate guest stack");
    jit_->SetSP(stack_base_ + stack_size - 64);
    jit_->SetFpcr(0);
    tpidrro = gaddr(std::calloc(1, 4096));  // stand-in TSD area

    if (t_current) fatal("host thread already has a guest context");
    t_current = this;
    host_tid = GetCurrentThreadId();
    std::lock_guard lock(g_threads_mutex);
    g_threads.push_back(this);
}

Thread::~Thread() {
    if (t_current == this) t_current = nullptr;
    {
        std::lock_guard lock(g_threads_mutex);
        std::erase(g_threads, this);
    }
    VirtualFree(gptr<void>(stack_base_), 0, MEM_RELEASE);
}

u64 Thread::x(int i) const { return jit_->GetRegister(i); }
void Thread::set_x(int i, u64 v) { jit_->SetRegister(i, v); }
Vec128 Thread::v(int i) const {
    auto r = jit_->GetVector(i);
    return {r[0], r[1]};
}
void Thread::set_v(int i, Vec128 v) { jit_->SetVector(i, {v.lo, v.hi}); }
double Thread::d(int i) const {
    u64 b = jit_->GetVector(i)[0];
    double r;
    std::memcpy(&r, &b, 8);
    return r;
}
float Thread::s(int i) const {
    u32 b = (u32)jit_->GetVector(i)[0];
    float r;
    std::memcpy(&r, &b, 4);
    return r;
}
void Thread::set_d(int i, double v) {
    u64 b;
    std::memcpy(&b, &v, 8);
    jit_->SetVector(i, {b, 0});
}
void Thread::set_s(int i, float v) {
    u32 b;
    std::memcpy(&b, &v, 4);
    jit_->SetVector(i, {b, 0});
}
u64 Thread::sp() const { return jit_->GetSP(); }
void Thread::set_sp(u64 v) { jit_->SetSP(v); }
u64 Thread::pc() const { return jit_->GetPC(); }
void Thread::jump(u64 addr) {
    jit_->SetPC(addr);
    jumped = true;
}

void Thread::run_until_return() {
    ++depth_;
    for (;;) {
        svc_pending = false;
        in_jit = true;
        Dynarmic::HaltReason why = jit_->Run();
        in_jit = false;
        jit_->ClearHalt(Dynarmic::HaltReason::UserDefined1);
        if (Dynarmic::Has(why, Dynarmic::HaltReason::UserDefined2)) {  // profiler sample
            jit_->ClearHalt(Dynarmic::HaltReason::UserDefined2);
            sampled_pc = pc();
            if (!svc_pending) continue;
        }
        if (!svc_pending) fatal("guest halted unexpectedly at 0x%llx", (unsigned long long)pc());
        GuestAddr stub = pc() - 4;
        if (stub == g_return_trap) break;
        if (!is_stub(stub)) fatal("SVC outside stub region at 0x%llx (%s)", (unsigned long long)stub, symbolize(stub).c_str());
        size_t idx = (stub - g_stub_base) / 4;
        jumped = false;
        LOG_TRACE("-> %s (lr=0x%llx)", g_stub_names[idx].c_str(), (unsigned long long)x(30));
        const char* prev_hle = in_hle.exchange(g_stub_names[idx].c_str());
        if (g_profile) g_call_counts[idx].fetch_add(1, std::memory_order_relaxed);
        g_handlers[idx](*this);
        in_hle = prev_hle;
        if (!jumped) jit_->SetPC(x(30));
    }
    --depth_;
}

void Thread::call_raw(GuestAddr fn, const std::function<void(Thread&)>& setup) {
    auto regs = jit_->GetRegisters();
    auto vecs = jit_->GetVectors();
    u64 saved_sp = sp(), saved_pc = pc();
    u32 pstate = jit_->GetPstate(), fpsr = jit_->GetFpsr();
    bool saved_jumped = jumped;

    // Stay below the interrupted frame (and Darwin's 128-byte red zone).
    set_sp((saved_sp - 128) & ~15ull);
    setup(*this);
    set_x(30, g_return_trap);
    jit_->SetPC(fn);
    const char* outer_hle = in_hle.exchange(nullptr);  // guest code again, even if called from HLE
    run_until_return();
    in_hle = outer_hle;
    last_x0_ = x(0);
    last_x1_ = x(1);
    last_v0_ = v(0);

    jit_->SetRegisters(regs);
    jit_->SetVectors(vecs);
    jit_->SetSP(saved_sp);
    jit_->SetPC(saved_pc);
    jit_->SetPstate(pstate);
    jit_->SetFpsr(fpsr);
    jumped = saved_jumped;
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
    std::string out;
    char line[256];
    u64 pc = jit_->GetPC(), lr = jit_->GetRegister(30), fp = jit_->GetRegister(29);
    if (in_hle.load() && is_stub(pc - 4)) pc -= 4;
    snprintf(line, sizeof(line), "  pc  0x%llx %s\n  lr  0x%llx %s\n", (unsigned long long)pc, symbolize(pc).c_str(),
             (unsigned long long)lr, symbolize(lr).c_str());
    out += line;
    for (int i = 0; i < 32 && fp && readable(fp, 16); i++) {
        u64 ret = gptr<u64>(fp)[1];
        snprintf(line, sizeof(line), "  #%-2d 0x%llx %s\n", i, (unsigned long long)ret, symbolize(ret).c_str());
        out += line;
        u64 next = gptr<u64>(fp)[0];
        if (next <= fp) break;
        fp = next;
    }
    return out;
}

Thread& current() {
    if (!t_current) new Thread(1 << 20);  // registers itself as t_current
    return *t_current;
}

Thread* current_or_null() { return t_current; }

void enable_profiling() { g_profile = true; }

// Every `seconds`: top HLE calls by count, and where each thread was when sampled.
void profile_report(double seconds, const std::unordered_map<std::string, u64>& samples, u64 total_samples) {
    std::vector<std::pair<u64, size_t>> top;
    size_t n = g_stub_count.load();
    for (size_t i = 0; i < n; i++) {
        u64 c = g_call_counts[i].exchange(0);
        if (c) top.push_back({c, i});
    }
    std::sort(top.rbegin(), top.rend());
    std::string out;
    char line[160];
    u64 total = 0;
    for (auto& [c, i] : top) total += c;
    snprintf(line, sizeof line, "==== HLE calls: %.0f/s total\n", total / seconds);
    out += line;
    for (size_t k = 0; k < top.size() && k < 25; k++) {
        snprintf(line, sizeof line, "  %10.0f/s  %s\n", top[k].first / seconds, g_stub_names[top[k].second].c_str());
        out += line;
    }
    std::vector<std::pair<u64, std::string>> s;
    for (auto& [k, v] : samples) s.push_back({v, k});
    std::sort(s.rbegin(), s.rend());
    out += "==== thread samples (where threads were)\n";
    for (size_t k = 0; k < s.size() && k < 45; k++) {
        snprintf(line, sizeof line, "  %5.1f%%  %s\n", 100.0 * s[k].first / std::max<u64>(total_samples, 1), s[k].second.c_str());
        out += line;
    }
    LOG_INFO("%s", out.c_str());
}

void profile_sample(std::unordered_map<std::string, u64>& samples) {
    std::lock_guard lock(g_threads_mutex);
    for (Thread* t : g_threads) {
        const char* hle = t->in_hle.load();
        char key[160];
        if (hle) {
            snprintf(key, sizeof key, "t%u %s", t->id(), hle);
        } else if (!t->in_jit.load()) {
            snprintf(key, sizeof key, "t%u (host code)", t->id());
        } else {
            // Guest code: stop the JIT at the next block boundary to learn where it is; the
            // PC shows up in the next sample.
            t->jit().HaltExecution(Dynarmic::HaltReason::UserDefined2);
            u64 pc = t->sampled_pc.load();
            snprintf(key, sizeof key, "t%u guest %s", t->id(), pc ? symbolize(pc).c_str() : "?");
        }
        samples[key]++;
    }
}

void dump_all_threads() {
    std::lock_guard lock(g_threads_mutex);
    LOG_INFO("==== %zu guest threads ====", g_threads.size());
    for (Thread* t : g_threads) {
        const char* hle = t->in_hle.load();
        if (!hle) {
            LOG_INFO("thread %u (guest id %u): running guest code", t->host_tid, t->id());
            continue;
        }
        LOG_INFO("thread %u (guest id %u): inside %s\n%s", t->host_tid, t->id(), hle, t->backtrace().c_str());
    }
}

}  // namespace cpu
