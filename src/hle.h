// HLE registry: maps imported Mach-O symbols to host implementations, and marshals
// arguments/return values using the Darwin arm64 calling convention.
#pragma once
#include "common.h"
#include "cpu.h"
#include <functional>
#include <tuple>
#include <type_traits>

namespace macho { struct Image; }

// Core Graphics geometry (CGFloat is double on arm64). These are HFAs in the ABI.
struct CGPoint { double x, y; };
struct CGSize { double width, height; };
struct CGRect { CGPoint origin; CGSize size; };

namespace hle {

// Number of double members if T is a homogeneous floating-point aggregate we know, else 0.
template <typename T> inline constexpr int hfa_doubles = 0;
template <> inline constexpr int hfa_doubles<CGPoint> = 2;
template <> inline constexpr int hfa_doubles<CGSize> = 2;
template <> inline constexpr int hfa_doubles<CGRect> = 4;

// Sequential reader of a call's arguments.
class Args {
public:
    explicit Args(cpu::Thread& t) : t_(t), nsaa_(t.sp()) {}
    cpu::Thread& thread() { return t_; }

    template <typename T>
    T get() {
        using U = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<U, cpu::Thread>) {
            return t_;
        } else if constexpr (std::is_floating_point_v<U>) {
            if (nsrn_ < 8) return sizeof(U) == 4 ? (U)t_.s(nsrn_++) : (U)t_.d(nsrn_++);
            return stack<U>();
        } else if constexpr (hfa_doubles<U> > 0) {
            constexpr int n = hfa_doubles<U>;
            U out;
            double* f = reinterpret_cast<double*>(&out);
            if (nsrn_ + n <= 8) {
                for (int i = 0; i < n; i++) f[i] = t_.d(nsrn_++);
            } else {
                nsrn_ = 8;
                nsaa_ = (nsaa_ + 7) & ~7ull;
                std::memcpy(&out, gptr<void>(nsaa_), sizeof(U));
                nsaa_ += sizeof(U);
            }
            return out;
        } else if constexpr (std::is_same_v<U, bool>) {
            return (next_gpr<u64>() & 0xff) != 0;
        } else {
            static_assert(std::is_integral_v<U> || std::is_pointer_v<U> || std::is_enum_v<U>, "unsupported arg type");
            return next_gpr<U>();
        }
    }

    // Variadic arguments (Darwin: all on the stack, one 8-byte slot each, after named stack args).
    GuestAddr varargs() const { return (nsaa_ + 7) & ~7ull; }

private:
    template <typename U>
    U next_gpr() {
        if (ngrn_ < 8) {
            u64 raw = t_.x(ngrn_++);
            if constexpr (std::is_pointer_v<U>) return reinterpret_cast<U>(static_cast<uintptr_t>(raw));
            else return static_cast<U>(raw);
        }
        return stack<U>();
    }
    template <typename U>
    U stack() {
        // Darwin packs stacked arguments at their natural alignment.
        size_t sz = sizeof(U) < 8 ? sizeof(U) : 8;
        nsaa_ = (nsaa_ + sz - 1) & ~(u64)(sz - 1);
        U v;
        std::memcpy(&v, gptr<void>(nsaa_), sizeof(U));
        nsaa_ += sizeof(U);
        return v;
    }

    cpu::Thread& t_;
    int ngrn_ = 0, nsrn_ = 0;
    u64 nsaa_;
};

template <typename R>
void set_return(cpu::Thread& t, R v) {
    using U = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<U, float>) {
        t.set_s(0, v);
    } else if constexpr (std::is_same_v<U, double>) {
        t.set_d(0, v);
    } else if constexpr (hfa_doubles<U> > 0) {
        const double* f = reinterpret_cast<const double*>(&v);
        for (int i = 0; i < hfa_doubles<U>; i++) t.set_d(i, f[i]);
    } else if constexpr (std::is_pointer_v<U>) {
        t.set_x(0, gaddr(v));
    } else if constexpr (std::is_same_v<U, bool>) {
        t.set_x(0, v ? 1 : 0);
    } else if constexpr (std::is_signed_v<U>) {
        t.set_x(0, (u64)(s64)v);
    } else {
        static_assert(std::is_integral_v<U> || std::is_enum_v<U>, "unsupported return type");
        t.set_x(0, (u64)v);
    }
}

template <typename R, typename... A>
cpu::Handler wrap(R (*f)(A...)) {
    return [f](cpu::Thread& t) {
        Args a(t);
        // Braced initialization guarantees left-to-right evaluation of get<>().
        std::tuple<A...> args{a.get<A>()...};
        if constexpr (std::is_void_v<R>) std::apply(f, args);
        else set_return<R>(t, std::apply(f, args));
    };
}

// Register a raw handler (full control over registers).
void raw(const std::string& sym, cpu::Handler h);
// Register a captureless lambda / function; arguments are marshalled automatically.
template <typename F>
void fn(const std::string& sym, F f) {
    raw(sym, wrap(+f));
}
// Implements a function with native ARM64 code, so calls never leave the JIT (for hot leaf
// functions). Returns the code's address.
GuestAddr native(const std::string& sym, std::initializer_list<u32> code);
// Register a data symbol at a fixed address, or one created on first use.
void data(const std::string& sym, GuestAddr addr);
void data_lazy(const std::string& sym, std::function<GuestAddr()> make);

// Fallback resolvers for families of symbols (e.g. _OBJC_CLASS_$_*); return 0 to pass.
void add_resolver(std::function<GuestAddr(const std::string&)> r);

// Address for a symbol (stub or data); unknown functions get a logging stub.
GuestAddr resolve(const std::string& sym);
// Like resolve() but returns 0 for unknown symbols (dlsym semantics).
GuestAddr lookup(const std::string& sym);

// Writes resolved addresses into every bind location of the image.
void bind_image(const macho::Image& img);

// Guest-visible, never-freed allocations for runtime-owned data (strings, tables...).
void* alloc_static(size_t size, size_t align = 16);
const char* static_cstr(std::string_view s);

}  // namespace hle
