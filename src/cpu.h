// Guest CPU. On Windows (x86-64): one dynarmic A64 JIT per host thread that runs guest code;
// guest code reaches the runtime through HLE stubs, each a single `SVC #0` in a stub region,
// and handlers run on the host stack outside the JIT and may call back into guest code.
// On Android (ARM64, IB3_NATIVE_CPU): the game's code runs directly on the CPU; stubs are small
// trampolines into cpu_native.S, which saves the registers and calls the same handlers.
#pragma once
#if defined(__aarch64__) && defined(__ANDROID__)
#define IB3_NATIVE_CPU 1
#endif
#include "common.h"
#include <array>
#include <functional>
#include <initializer_list>
#include <atomic>
#include <memory>
#include <unordered_map>

namespace Dynarmic::A64 { class Jit; }
namespace macho { struct Image; }

namespace cpu {

class Thread;
using Handler = std::function<void(Thread&)>;

// --- HLE stub table --------------------------------------------------------
// Allocates a stub whose execution invokes `h`. `name` is used for tracing.
GuestAddr make_stub(const std::string& name, Handler h);
const std::string& stub_name(GuestAddr stub);
bool is_stub(GuestAddr addr);

// Must be called once before creating threads.
void init(const macho::Image* image);
const macho::Image* image();
std::string symbolize(GuestAddr addr);

struct Vec128 { u64 lo, hi; };

#if IB3_NATIVE_CPU
struct Context;              // saved guest registers (cpu_native.S)
void dispatch(Context* c);   // runs the handler of the stub that was called
void* alloc_code(size_t bytes);  // executable memory (hle::native)
#endif

class Thread {
public:
    // Creates the guest context for the calling host thread. `stack_size` bytes of guest stack.
    explicit Thread(size_t stack_size = 1 << 20);
    ~Thread();

    // Registers.
    u64 x(int i) const;
    void set_x(int i, u64 v);
    Vec128 v(int i) const;
    void set_v(int i, Vec128 v);
    double d(int i) const;
    float s(int i) const;
    void set_d(int i, double v);
    void set_s(int i, float v);
    u64 sp() const;
    void set_sp(u64 v);
    u64 pc() const;
    // From inside a handler: continue guest execution at `addr` instead of returning to LR.
    void jump(u64 addr);
    // Arguments passed on the stack (Darwin: variadic args are 8-byte slots starting at SP).
    u64 stack_slot(int i) const { return *gptr<u64>(sp() + 8ull * i); }

    // Calls guest function `fn` with integer args (x0..x7, rest on stack). Returns x0.
    u64 call(GuestAddr fn, std::initializer_list<u64> args = {});
    // Full-control variant: caller fills registers via `setup`; returns after the guest returns.
    void call_raw(GuestAddr fn, const std::function<void(Thread&)>& setup);
    // Return registers of the last call()/call_raw().
    u64 last_x0() const { return last_x0_; }
    u64 last_x1() const { return last_x1_; }
    Vec128 last_v0() const { return last_v0_; }

    // Runs guest code at the current PC until the matching return trap (used internally
    // and by the entry point).
    void run_until_return();

#if !IB3_NATIVE_CPU
    Dynarmic::A64::Jit& jit() { return *jit_; }
#endif
    u32 id() const { return id_; }
    GuestAddr stack_base() const { return stack_base_; }
    size_t stack_size() const { return stack_size_; }
    std::string backtrace() const;

    // Per-thread TPIDRRO_EL0/TPIDR_EL0 storage.
    u64 tpidrro = 0;
    u64 tpidr = 0;

    // Internal (used by the dynarmic callbacks).
    bool svc_pending = false;
    bool jumped = false;

private:
#if IB3_NATIVE_CPU
    friend void dispatch(Context*);
    Context* ctx_ = nullptr;  // registers of the innermost guest -> HLE call
#else
    std::unique_ptr<Dynarmic::A64::Jit> jit_;
    struct CallbacksImpl;
    std::unique_ptr<CallbacksImpl> callbacks_;
#endif
    u32 id_;
    GuestAddr stack_base_ = 0;
    size_t stack_size_ = 0;
    u64 last_x0_ = 0, last_x1_ = 0;
    Vec128 last_v0_{};
    int depth_ = 0;

public:
    // Name of the HLE function this thread is currently inside (nullptr while running guest code).
    std::atomic<const char*> in_hle{nullptr};
    std::atomic<u64> sampled_pc{0};  // guest PC at the last profiler halt
    std::atomic<bool> in_jit{false};  // inside Jit::Run (not blocked in host code)
    unsigned host_tid = 0;
};

// The guest context of the calling host thread, created on first use.
Thread& current();
Thread* current_or_null();

// Logs every guest thread's current HLE call and backtrace (diagnostics).
void dump_all_threads();

// Profiling (-profile): per-stub call counts plus periodic thread sampling.
void enable_profiling();
void profile_sample(std::unordered_map<std::string, u64>& samples);
void profile_report(double seconds, const std::unordered_map<std::string, u64>& samples, u64 total_samples);

}  // namespace cpu
