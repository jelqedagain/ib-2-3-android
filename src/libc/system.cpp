// Process, dyld, C++ ABI, signals, sysctl, mach/vm, mmap, sockets (offline), setjmp.
#include "hle.h"
#include "libc/format.h"
#include "libc/vfs.h"
#include "modules.h"
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <windows.h>
#ifdef _WIN32
#include <psapi.h>
#else
#include <sys/mman.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

namespace libc {

// Emulated device (see sysctl hw.machine). Chosen in main().
std::string g_device_model = "iPad4,1";
u64 g_device_memory = 1ull << 30;

namespace {

constexpr int D_ENOENT = 2, D_ENOMEM = 12, D_EACCES = 13, D_EINVAL = 22, D_EAFNOSUPPORT = 47;
constexpr int KERN_SUCCESS = 0, KERN_FAILURE = 5;

std::mutex g_guard_mutex;
std::condition_variable g_guard_cv;

int write_sysctl(void* oldp, u64* oldlenp, const void* val, u64 len) {
    if (!oldlenp) return 0;
    if (oldp) {
        if (*oldlenp < len) {
            vfs::set_errno(D_ENOMEM);
            return -1;
        }
        std::memcpy(oldp, val, len);
    }
    *oldlenp = len;
    return 0;
}

int sysctl_by_name(const std::string& name, void* oldp, u64* oldlenp) {
    auto s = [&](const std::string& v) { return write_sysctl(oldp, oldlenp, v.c_str(), v.size() + 1); };
    auto i32 = [&](s32 v) { return write_sysctl(oldp, oldlenp, &v, 4); };
    auto i64 = [&](s64 v) { return write_sysctl(oldp, oldlenp, &v, 8); };
    if (name == "hw.machine" || name == "hw.model") return s(g_device_model);
    if (name == "hw.ncpu" || name == "hw.activecpu" || name == "hw.logicalcpu" || name == "hw.physicalcpu") return i32(2);
    if (name == "hw.memsize") return i64((s64)g_device_memory);
    if (name == "hw.physmem" || name == "hw.usermem") return i32((s32)g_device_memory);
    if (name == "hw.pagesize") return i64(4096);
    if (name == "hw.cpufrequency" || name == "hw.cpufrequency_max") return i64(1300000000);
    if (name == "hw.busfrequency") return i64(100000000);
    if (name == "hw.l1dcachesize") return i32(65536);
    if (name == "hw.l2cachesize") return i32(1 << 20);
    if (name == "hw.cachelinesize") return i32(64);
    if (name == "kern.osversion") return s("12H321");
    if (name == "kern.osrelease") return s("14.0.0");
    if (name == "kern.hostname") return s("ib3-pc");
    LOG_WARN("sysctlbyname(%s) unknown", name.c_str());
    vfs::set_errno(D_ENOENT);
    return -1;
}

// Darwin arm64 jmp_buf: x19..x30, sp, d8..d15 (we store: x19-x30 [12], fp-unused, sp, d8-d15).
void do_setjmp(cpu::Thread& t, GuestAddr buf) {
    u64* b = gptr<u64>(buf);
    for (int i = 0; i < 12; i++) b[i] = t.x(19 + i);  // x19..x30 (x30 = return address)
    b[12] = t.sp();
    for (int i = 0; i < 8; i++) b[13 + i] = t.v(8 + i).lo;
}
void do_longjmp(cpu::Thread& t, GuestAddr buf, int val) {
    const u64* b = gptr<u64>(buf);
    for (int i = 0; i < 12; i++) t.set_x(19 + i, b[i]);
    t.set_sp(b[12]);
    for (int i = 0; i < 8; i++) t.set_v(8 + i, {b[13 + i], 0});
    t.set_x(0, val ? (u64)val : 1);
    t.jump(b[11]);
}

#ifdef _WIN32
struct Mapping {
    HANDLE mapping;
    void* view;
};

u64 host_available_memory() {
    MEMORYSTATUSEX ms{sizeof(ms)};
    GlobalMemoryStatusEx(&ms);
    return ms.ullAvailPhys;
}

u64 host_resident_memory() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}
#else
struct Mapping {
    void* base;
    u64 len;
};

u64 host_available_memory() {
    struct sysinfo si {};
    sysinfo(&si);
    return (u64)si.freeram * si.mem_unit;
}

u64 host_resident_memory() {
    u64 pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (fscanf(f, "%llu %llu", (unsigned long long*)&pages, (unsigned long long*)&resident) != 2) resident = 0;
        std::fclose(f);
    }
    return resident * host_page_size();
}
#endif
std::mutex g_mmap_mutex;
std::unordered_map<u64, Mapping> g_mappings;  // returned address -> view

}  // namespace

void install_system() {
    using hle::fn;

    // process
    fn("_getpid", []() { return 1234; });
    fn("_getprogname", []() { return hle::static_cstr("SwordGame"); });
    fn("__NSGetExecutablePath", [](char* buf, u32* size) {
        std::string p = std::string(vfs::kBundlePath) + "/SwordGame";
        if (*size < p.size() + 1) {
            *size = (u32)p.size() + 1;
            return -1;
        }
        std::memcpy(buf, p.c_str(), p.size() + 1);
        return 0;
    });
    fn("_gethostname", [](char* buf, u64 n) {
        std::strncpy(buf, "ib3-pc", n);
        return 0;
    });
    fn("_sysconf", [](int name) -> s64 {
        switch (name) {
        case 29: return 4096;               // _SC_PAGESIZE
        case 57: case 58: return 2;         // _SC_NPROCESSORS_CONF/ONLN
        case 200: return (s64)(g_device_memory / 4096);  // _SC_PHYS_PAGES
        default:
            LOG_WARN("sysconf(%d) unknown", name);
            return -1;
        }
    });
    fn("_sysctlbyname", [](const char* name, void* oldp, u64* oldlenp, void*, u64) {
        return sysctl_by_name(name, oldp, oldlenp);
    });
    fn("_sysctl", [](const s32* mib, u32 n, void* oldp, u64* oldlenp, void*, u64) {
        if (n >= 2 && mib[0] == 6) {  // CTL_HW
            switch (mib[1]) {
            case 1: return sysctl_by_name("hw.machine", oldp, oldlenp);
            case 2: return sysctl_by_name("hw.model", oldp, oldlenp);
            case 3: return sysctl_by_name("hw.ncpu", oldp, oldlenp);
            case 5: return sysctl_by_name("hw.physmem", oldp, oldlenp);
            case 7: return sysctl_by_name("hw.pagesize", oldp, oldlenp);
            case 24: return sysctl_by_name("hw.memsize", oldp, oldlenp);
            case 25: return sysctl_by_name("hw.activecpu", oldp, oldlenp);
            }
        }
        if (n >= 4 && mib[0] == 1 && mib[1] == 14 && mib[2] == 1) {  // CTL_KERN, KERN_PROC, KERN_PROC_PID
            constexpr u64 kKinfoProcSize = 648;                          // zeroed = not being debugged
            if (oldlenp) {
                if (oldp) std::memset(oldp, 0, std::min<u64>(*oldlenp, kKinfoProcSize));
                *oldlenp = kKinfoProcSize;
            }
            return 0;
        }
        LOG_DEBUG("sysctl({%d,%d,%d}) unsupported", n > 0 ? mib[0] : -1, n > 1 ? mib[1] : -1, n > 2 ? mib[2] : -1);
        vfs::set_errno(D_ENOENT);
        return -1;
    });
    fn("_NSVersionOfLinkTimeLibrary", [](const char*) -> s32 { return -1; });

    // termination and diagnostics
    fn("_abort", [](cpu::Thread& t) { fatal("guest abort()\n%s", t.backtrace().c_str()); });
    fn("_exit", [](int code) {
        LOG_INFO("guest exit(%d)", code);
        ExitProcess(code);
    });
    fn("___assert_rtn", [](cpu::Thread& t, const char* func, const char* file, int line, const char* expr) {
        fatal("guest assertion failed: %s (%s:%d %s)\n%s", expr, file, line, func, t.backtrace().c_str());
    });
    fn("___stack_chk_fail", [](cpu::Thread& t) { fatal("guest stack smashing detected\n%s", t.backtrace().c_str()); });
    static u64 s_stack_guard = 0x2d5a3b4c1e0f7a69ull;
    hle::data("___stack_chk_guard", gaddr(&s_stack_guard));
    fn("_signal", [](int, u64) -> u64 { return 0; });
    fn("_sigaction", [](int, void*, void* old) {
        if (old) std::memset(old, 0, 16);
        return 0;
    });
    fn("_sigaltstack", [](void*, void* old) {
        if (old) std::memset(old, 0, 24);
        return 0;
    });
    fn("_raise", [](cpu::Thread& t, int sig) {
        LOG_WARN("guest raise(%d) ignored\n%s", sig, t.backtrace().c_str());
        return 0;
    });
    fn("_backtrace", [](void**, int) { return 0; });

    // C++ ABI
    fn("___cxa_atexit", [](u64, u64, u64) { return 0; });
    // Itanium guard: byte 0 = initialized, byte 1 = initialization in progress.
    fn("___cxa_guard_acquire", [](u8* guard) -> int {
        if (__atomic_load_n(guard, __ATOMIC_ACQUIRE)) return 0;
        std::unique_lock lock(g_guard_mutex);
        for (;;) {
            if (guard[0]) return 0;
            if (!guard[1]) {
                guard[1] = 1;
                return 1;
            }
            g_guard_cv.wait(lock);
        }
    });
    fn("___cxa_guard_release", [](u8* guard) {
        {
            std::lock_guard lock(g_guard_mutex);
            guard[1] = 0;
            __atomic_store_n(guard, 1, __ATOMIC_RELEASE);
        }
        g_guard_cv.notify_all();
    });
    fn("___cxa_guard_abort", [](u8* guard) {
        {
            std::lock_guard lock(g_guard_mutex);
            guard[1] = 0;
        }
        g_guard_cv.notify_all();
    });
    fn("___cxa_pure_virtual", [](cpu::Thread& t) { fatal("pure virtual call\n%s", t.backtrace().c_str()); });
    fn("__Unwind_Resume", [](cpu::Thread& t) { fatal("_Unwind_Resume (C++ exception)\n%s", t.backtrace().c_str()); });

    // setjmp/longjmp operate directly on guest registers.
    hle::raw("_setjmp", [](cpu::Thread& t) {
        do_setjmp(t, t.x(0));
        t.set_x(0, 0);
    });
    hle::raw("_longjmp", [](cpu::Thread& t) { do_longjmp(t, t.x(0), (int)t.x(1)); });

    // dyld
    fn("_dlopen", [](const char* path, int) -> u64 {
        LOG_DEBUG("dlopen(%s)", path ? path : "(null)");
        return 1;
    });
    fn("_dlsym", [](u64, const char* name) -> u64 {
        std::string sym = std::string("_") + name;
        u64 a = hle::lookup(sym);
        if (!a) {
            LOG_WARN("dlsym(%s): not implemented, returning a logging stub", name);
            a = hle::resolve(sym);
        }
        LOG_DEBUG("dlsym(%s) = 0x%llx", name, (unsigned long long)a);
        return a;
    });
    fn("_dladdr", [](u64, void*) { return 0; });
    fn("__dyld_register_func_for_add_image", [](u64) {});
    fn("__dyld_register_func_for_remove_image", [](u64) {});

    // mach ports / tasks / vm (crash reporting and memory stats)
    static u32 s_task_self = 0x103;
    hle::data("_mach_task_self_", gaddr(&s_task_self));
    static u64 s_ndr[1] = {0x0000000001000000ull};
    hle::data("_NDR_record", gaddr(s_ndr));
    static u64 s_vm_page_size = 4096;
    hle::data("_vm_page_size", gaddr(&s_vm_page_size));
    fn("_mach_host_self", []() -> u32 { return 0x203; });
    fn("_host_page_size", [](u32, u64* size) {
        *size = 4096;
        return KERN_SUCCESS;
    });
    fn("_host_statistics", [](u32, int flavor, u32* info, u32* count) {
        if (flavor == 2 && *count >= 15) {  // HOST_VM_INFO: vm_statistics is 15 ints
            std::memset(info, 0, 15 * 4);
            *count = 15;
            u64 free_pages = std::min<u64>(host_available_memory(), g_device_memory / 2) / 4096;
            info[0] = (u32)free_pages;                  // free_count
            info[1] = (u32)(g_device_memory / 4096 / 4); // active
            info[2] = (u32)(g_device_memory / 4096 / 8); // inactive
            info[3] = (u32)(g_device_memory / 4096 / 8); // wire
            return KERN_SUCCESS;
        }
        return KERN_FAILURE;
    });
    fn("_task_info", [](u32, u32 flavor, u32* info, u32* count) {
        if (flavor == 20 || flavor == 5) {  // MACH_TASK_BASIC_INFO / TASK_BASIC_INFO_64
            u32 n = flavor == 20 ? 12 : 10;  // struct sizes in natural_t units
            if (*count < n) return KERN_FAILURE;
            std::memset(info, 0, n * 4);
            *count = n;
            // mach_task_basic_info: virtual_size(8) resident_size(8) resident_size_max(8) ...
            u64 resident = std::min<u64>(host_resident_memory(), g_device_memory / 2);
            if (flavor == 20) {
                *reinterpret_cast<u64*>(info) = 1ull << 32;
                *reinterpret_cast<u64*>(info + 2) = resident;
                *reinterpret_cast<u64*>(info + 4) = resident;
            } else {
                info[0] = 0;  // suspend_count
                *reinterpret_cast<u64*>(info + 1) = 1ull << 32;
                *reinterpret_cast<u64*>(info + 3) = resident;
            }
            return KERN_SUCCESS;
        }
        return KERN_FAILURE;
    });
    for (const char* name : {"_mach_port_allocate", "_mach_port_deallocate", "_mach_port_insert_right", "_mach_port_mod_refs",
                             "_mach_port_move_member", "_mach_port_request_notification"})
        fn(name, []() { return KERN_SUCCESS; });
    for (const char* name : {"_task_get_exception_ports", "_task_swap_exception_ports", "_thread_get_exception_ports",
                             "_thread_swap_exception_ports", "_exception_raise", "_exception_raise_state",
                             "_exception_raise_state_identity", "_vm_map", "_mach_make_memory_entry_64"})
        fn(name, []() { return KERN_FAILURE; });
    fn("_mach_msg", [](cpu::Thread& t) {
        // Only crash-reporter exception servers receive here; park them forever.
        LOG_DEBUG("mach_msg from %s: parking thread", cpu::symbolize(t.x(30)).c_str());
        Sleep(INFINITE);
        return KERN_FAILURE;
    });
    fn("_vm_allocate", [](u32, u64* addr, u64 size, int) {
        void* p = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) return 3;  // KERN_NO_SPACE
        *addr = gaddr(p);
        return KERN_SUCCESS;
    });
    fn("_vm_deallocate", [](u32, u64 addr, u64) {
        VirtualFree(gptr<void>(addr), 0, MEM_RELEASE);
        return KERN_SUCCESS;
    });
    fn("_vm_protect", [](u32, u64, u64, int, int) { return KERN_SUCCESS; });
    fn("_vm_read_overwrite", [](u32, u64 addr, u64 size, u64 data, u64* outsize) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(gptr<void>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 1;  // KERN_INVALID_ADDRESS
        std::memcpy(gptr<void>(data), gptr<void>(addr), size);
        if (outsize) *outsize = size;
        return KERN_SUCCESS;
    });

    // mmap: anonymous memory, or read-only/private file views.
    fn("_mmap", [](u64 addr, u64 len, int prot, int flags, int fd, s64 off) -> u64 {
        const u64 kMapFailed = ~0ull;
        if (flags & 0x1000) {  // MAP_ANON
            void* p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            return p ? gaddr(p) : kMapFailed;
        }
#ifndef _WIN32
        // A private (copy-on-write) file view. The offset must be a multiple of the device's
        // page size, which can be larger than the game's (16 KB pages on some phones).
        u64 aligned = off & ~(u64)(host_page_size() - 1), delta = off - aligned;
        void* view = ::mmap(nullptr, len + delta, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, aligned);
        if (view == MAP_FAILED) {
            LOG_WARN("mmap: failed (errno %d)", errno);
            vfs::set_errno(errno == EBADF ? 9 : D_ENOMEM);
            return kMapFailed;
        }
        u64 result = gaddr(view) + delta;
        std::lock_guard lock(g_mmap_mutex);
        g_mappings[result] = {view, len + delta};
        (void)addr;
        (void)prot;
        return result;
#else
        HANDLE fh = (HANDLE)_get_osfhandle(fd);
        if (fh == INVALID_HANDLE_VALUE) {
            vfs::set_errno(9);
            return kMapFailed;
        }
        HANDLE m = CreateFileMappingW(fh, nullptr, PAGE_WRITECOPY, 0, 0, nullptr);
        if (!m) {
            LOG_WARN("mmap: CreateFileMapping failed (%lu)", GetLastError());
            vfs::set_errno(D_EACCES);
            return kMapFailed;
        }
        u64 aligned = off & ~0xffffull;
        u64 delta = off - aligned;
        void* view = MapViewOfFile(m, FILE_MAP_COPY, (DWORD)(aligned >> 32), (DWORD)aligned, len + delta);
        if (!view) {
            LOG_WARN("mmap: MapViewOfFile failed (%lu)", GetLastError());
            CloseHandle(m);
            vfs::set_errno(D_ENOMEM);
            return kMapFailed;
        }
        u64 result = gaddr(view) + delta;
        std::lock_guard lock(g_mmap_mutex);
        g_mappings[result] = {m, view};
        LOG_DEBUG("mmap(fd=%d, len=0x%llx, off=0x%llx) = 0x%llx", fd, (unsigned long long)len, (unsigned long long)off,
                  (unsigned long long)result);
        (void)addr;
        (void)prot;
        return result;
#endif
    });
    fn("_munmap", [](u64 addr, u64) {
        std::lock_guard lock(g_mmap_mutex);
        auto it = g_mappings.find(addr);
        if (it != g_mappings.end()) {
#ifdef _WIN32
            UnmapViewOfFile(it->second.view);
            CloseHandle(it->second.mapping);
#else
            ::munmap(it->second.base, it->second.len);
#endif
            g_mappings.erase(it);
            return 0;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(gptr<void>(addr), &mbi, sizeof mbi) && mbi.AllocationBase == gptr<void>(addr))
            VirtualFree(gptr<void>(addr), 0, MEM_RELEASE);
        return 0;
    });

    // networking: the game runs offline
    for (const char* name : {"_socket", "_accept", "_bind", "_connect", "_listen", "_recv", "_recvfrom", "_send",
                             "_sendto", "_getsockname", "_getsockopt", "_setsockopt", "_ioctl", "_getifaddrs"})
        hle::raw(name, [](cpu::Thread& t) {
            vfs::set_errno(D_EAFNOSUPPORT);
            t.set_x(0, ~0ull);
        });
    fn("_freeifaddrs", [](void*) {});
    fn("_getaddrinfo", [](const char* host, const char*, void*, u64* res) {
        LOG_DEBUG("getaddrinfo(%s): offline", host ? host : "");
        if (res) *res = 0;
        return 8;  // EAI_NONAME
    });
    fn("_freeaddrinfo", [](void*) {});
    fn("_inet_addr", [](const char* s) -> u32 {
        unsigned a, b, c, d;
        if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0xffffffff;
        return a | (b << 8) | (c << 16) | (d << 24);
    });
    fn("_inet_ntoa", [](u32 a) {
        static thread_local char* buf = static_cast<char*>(std::calloc(1, 16));
        snprintf(buf, 16, "%u.%u.%u.%u", a & 0xff, (a >> 8) & 0xff, (a >> 16) & 0xff, a >> 24);
        return buf;
    });
    // Server certificate checks (the game runs offline, so these never see a real connection).
    fn("_SecTrustEvaluate", [](u64, u32* result) {
        if (result) *result = 0;  // kSecTrustResultInvalid
        return 0;
    });
    fn("_SecTrustCopyInfo", [](u64) -> u64 { return 0; });
    fn("_if_nametoindex", [](const char*) -> u32 { return 0; });
    fn("_link_ntoa", [](void*) { return hle::static_cstr(""); });
    fn("_select", [](int, void*, void*, void*, s64* timeout) {
        if (timeout) Sleep((DWORD)(timeout[0] * 1000 + (s32)timeout[1] / 1000));
        return 0;
    });
}

}  // namespace libc
