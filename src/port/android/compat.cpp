// Win32 compatibility layer for the Android build (see windows.h next to this file).
#include <windows.h>
#include <io.h>
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/futex.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <climits>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

thread_local DWORD t_last_error = 0;

int futex_wait(std::atomic<uint32_t>* addr, uint32_t expected, const timespec* timeout) {
    return (int)syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAIT_PRIVATE, expected, timeout, nullptr, 0);
}
void futex_wake(std::atomic<uint32_t>* addr, int count) {
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(addr), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}

uint64_t now_ns(clockid_t clock) {
    timespec ts;
    clock_gettime(clock, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// --- handles ------------------------------------------------------------------------------------

struct Handle {
    virtual ~Handle() = default;
};

struct Event : Handle {
    std::mutex m;
    std::condition_variable cv;
    bool manual = false, signaled = false;
};

struct ThreadHandle : Handle {
    pthread_t thread{};
    Event done;
    unsigned (*fn)(void*) = nullptr;
    void* arg = nullptr;
    std::atomic<int> refs{2};  // the creator's handle and the thread itself
};

void release(ThreadHandle* t) {
    if (t->refs.fetch_sub(1) == 1) delete t;
}

void* thread_main(void* p) {
    auto* t = static_cast<ThreadHandle*>(p);
    t->fn(t->arg);
    {
        std::lock_guard lock(t->done.m);
        t->done.signaled = true;
    }
    t->done.cv.notify_all();
    release(t);
    return nullptr;
}

bool wait_event(Event* e, DWORD ms) {
    std::unique_lock lock(e->m);
    auto ready = [e] { return e->signaled; };
    if (ms == INFINITE) e->cv.wait(lock, ready);
    else if (!e->cv.wait_for(lock, std::chrono::milliseconds(ms), ready)) return false;
    if (!e->manual) e->signaled = false;
    return true;
}

// --- strings ------------------------------------------------------------------------------------

std::u32string utf8_to_utf32(const char* s, size_t n) {
    std::u32string out;
    for (size_t i = 0; i < n;) {
        uint8_t c = (uint8_t)s[i];
        char32_t cp;
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
        cp = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
        for (int k = 1; k < len && i + k < n; k++) cp = (cp << 6) | ((uint8_t)s[i + k] & 0x3f);
        out += cp;
        i += len;
    }
    return out;
}

std::string utf32_to_utf8(const wchar_t* s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = (uint32_t)s[i];
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) out += (char)(0xC0 | (cp >> 6)), out += (char)(0x80 | (cp & 0x3f));
        else if (cp < 0x10000)
            out += (char)(0xE0 | (cp >> 12)), out += (char)(0x80 | ((cp >> 6) & 0x3f)), out += (char)(0x80 | (cp & 0x3f));
        else
            out += (char)(0xF0 | (cp >> 18)), out += (char)(0x80 | ((cp >> 12) & 0x3f)),
                out += (char)(0x80 | ((cp >> 6) & 0x3f)), out += (char)(0x80 | (cp & 0x3f));
    }
    return out;
}

std::string narrow(const wchar_t* w) { return w ? utf32_to_utf8(w, wcslen(w)) : std::string(); }

// --- memory -------------------------------------------------------------------------------------

int to_prot(DWORD protect) {
    switch (protect & 0xff) {
    case PAGE_NOACCESS: return PROT_NONE;
    case PAGE_READONLY: return PROT_READ;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_READ | PROT_WRITE;
    }
}

std::mutex g_vm_mutex;
std::map<uintptr_t, size_t> g_reservations;  // VirtualAlloc(MEM_RESERVE) base -> size

// --- .ini files ---------------------------------------------------------------------------------

using Ini = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;

Ini read_ini(const std::string& path) {
    Ini ini;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[') {
            ini.push_back({line.substr(1, line.find(']') - 1), {}});
        } else if (auto eq = line.find('='); eq != std::string::npos && !ini.empty()) {
            ini.back().second.push_back({line.substr(0, eq), line.substr(eq + 1)});
        }
    }
    return ini;
}

const std::string* ini_find(const Ini& ini, const std::string& section, const std::string& key) {
    for (auto& [s, keys] : ini)
        if (strcasecmp(s.c_str(), section.c_str()) == 0)
            for (auto& [k, v] : keys)
                if (strcasecmp(k.c_str(), key.c_str()) == 0) return &v;
    return nullptr;
}

std::mutex g_ini_mutex;

}  // namespace

// --- threads, time ------------------------------------------------------------------------------

DWORD GetCurrentThreadId() { return (DWORD)gettid(); }
DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(-1); }
HANDLE GetCurrentThread() { return reinterpret_cast<HANDLE>(-2); }

void Sleep(DWORD ms) {
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

ULONGLONG GetTickCount64() { return now_ns(CLOCK_MONOTONIC) / 1000000ull; }

BOOL QueryPerformanceCounter(LARGE_INTEGER* v) {
    v->QuadPart = (LONGLONG)now_ns(CLOCK_MONOTONIC);
    return TRUE;
}

BOOL QueryPerformanceFrequency(LARGE_INTEGER* v) {
    v->QuadPart = 1000000000LL;
    return TRUE;
}

void GetSystemTimePreciseAsFileTime(FILETIME* ft) {
    uint64_t t = now_ns(CLOCK_REALTIME) / 100 + 116444736000000000ull;  // 100 ns since 1601
    ft->dwLowDateTime = (DWORD)t;
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

uintptr_t _beginthreadex(void*, unsigned stack, unsigned (*fn)(void*), void* arg, unsigned, unsigned* id) {
    auto* t = new ThreadHandle;
    t->fn = fn;
    t->arg = arg;
    t->done.manual = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    // Guest code runs on this stack (native CPU backend): be generous.
    pthread_attr_setstacksize(&attr, std::max<size_t>(stack, 8u << 20));
    int r = pthread_create(&t->thread, &attr, thread_main, t);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        delete t;
        return 0;
    }
    pthread_detach(t->thread);
    if (id) *id = 0;
    return reinterpret_cast<uintptr_t>(t);
}

// --- locks --------------------------------------------------------------------------------------

void InitializeSRWLock(SRWLOCK* l) { l->state.store(0); }

void AcquireSRWLockExclusive(SRWLOCK* l) {
    uint32_t c = 0;
    if (l->state.compare_exchange_strong(c, 1, std::memory_order_acquire)) return;
    if (c != 2) c = l->state.exchange(2, std::memory_order_acquire);
    while (c != 0) {
        futex_wait(&l->state, 2, nullptr);
        c = l->state.exchange(2, std::memory_order_acquire);
    }
}

BOOL TryAcquireSRWLockExclusive(SRWLOCK* l) {
    uint32_t c = 0;
    return l->state.compare_exchange_strong(c, 1, std::memory_order_acquire);
}

void ReleaseSRWLockExclusive(SRWLOCK* l) {
    if (l->state.exchange(0, std::memory_order_release) == 2) futex_wake(&l->state, 1);
}

void InitializeConditionVariable(CONDITION_VARIABLE* c) { c->seq.store(0); }

BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* c, SRWLOCK* l, DWORD ms, ULONG) {
    uint32_t seq = c->seq.load(std::memory_order_acquire);
    ReleaseSRWLockExclusive(l);
    timespec ts{(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    int r = futex_wait(&c->seq, seq, ms == INFINITE ? nullptr : &ts);
    bool timed_out = r == -1 && errno == ETIMEDOUT;
    // Re-take the lock as "contended" so a waiter woken with others still gets woken later.
    uint32_t s = l->state.exchange(2, std::memory_order_acquire);
    while (s != 0) {
        futex_wait(&l->state, 2, nullptr);
        s = l->state.exchange(2, std::memory_order_acquire);
    }
    if (timed_out) {
        t_last_error = 1460;  // ERROR_TIMEOUT
        return FALSE;
    }
    return TRUE;
}

void WakeConditionVariable(CONDITION_VARIABLE* c) {
    c->seq.fetch_add(1, std::memory_order_release);
    futex_wake(&c->seq, 1);
}

void WakeAllConditionVariable(CONDITION_VARIABLE* c) {
    c->seq.fetch_add(1, std::memory_order_release);
    futex_wake(&c->seq, INT_MAX);
}

// --- events and handles ---------------------------------------------------------------------------

HANDLE CreateEventW(void*, BOOL manual_reset, BOOL initial, LPCWSTR) {
    auto* e = new Event;
    e->manual = manual_reset;
    e->signaled = initial;
    return static_cast<Handle*>(e);
}

BOOL SetEvent(HANDLE h) {
    auto* e = dynamic_cast<Event*>(static_cast<Handle*>(h));
    if (!e) return FALSE;
    {
        std::lock_guard lock(e->m);
        e->signaled = true;
    }
    if (e->manual) e->cv.notify_all();
    else e->cv.notify_one();
    return TRUE;
}

BOOL ResetEvent(HANDLE h) {
    auto* e = dynamic_cast<Event*>(static_cast<Handle*>(h));
    if (!e) return FALSE;
    std::lock_guard lock(e->m);
    e->signaled = false;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h) return WAIT_FAILED;
    auto* base = static_cast<Handle*>(h);
    if (auto* e = dynamic_cast<Event*>(base)) return wait_event(e, ms) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (auto* t = dynamic_cast<ThreadHandle*>(base)) return wait_event(&t->done, ms) ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    return WAIT_FAILED;
}

BOOL CloseHandle(HANDLE h) {
    if (!h || h == GetCurrentProcess() || h == GetCurrentThread()) return TRUE;
    auto* base = static_cast<Handle*>(h);
    if (auto* t = dynamic_cast<ThreadHandle*>(base)) release(t);
    else delete base;
    return TRUE;
}

// --- memory ---------------------------------------------------------------------------------------

void* VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect) {
    size = (size + 4095) & ~size_t(4095);
    if (!(type & MEM_RESERVE)) {  // commit inside an earlier reservation
        if (mprotect(addr, size, to_prot(protect)) != 0) return nullptr;
        return addr;
    }
    int prot = (type & MEM_COMMIT) ? to_prot(protect) : PROT_NONE;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | (addr ? MAP_FIXED_NOREPLACE : 0);
    void* p = mmap(addr, size, prot, flags, -1, 0);
    if (p == MAP_FAILED || (addr && p != addr)) {
        if (p != MAP_FAILED) munmap(p, size);
        t_last_error = 487;  // ERROR_INVALID_ADDRESS
        return nullptr;
    }
    std::lock_guard lock(g_vm_mutex);
    g_reservations[reinterpret_cast<uintptr_t>(p)] = size;
    return p;
}

BOOL VirtualFree(void* addr, SIZE_T size, DWORD type) {
    if (type & MEM_RELEASE) {
        std::lock_guard lock(g_vm_mutex);
        auto it = g_reservations.find(reinterpret_cast<uintptr_t>(addr));
        if (it == g_reservations.end()) return FALSE;
        munmap(addr, it->second);
        g_reservations.erase(it);
        return TRUE;
    }
    size = (size + 4095) & ~size_t(4095);
    madvise(addr, size, MADV_DONTNEED);
    return mprotect(addr, size, PROT_NONE) == 0;
}

BOOL VirtualProtect(void* addr, SIZE_T size, DWORD protect, DWORD* old) {
    if (old) *old = PAGE_READWRITE;
    uintptr_t lo = reinterpret_cast<uintptr_t>(addr) & ~uintptr_t(4095);
    uintptr_t hi = (reinterpret_cast<uintptr_t>(addr) + size + 4095) & ~uintptr_t(4095);
    return mprotect(reinterpret_cast<void*>(lo), hi - lo, to_prot(protect)) == 0;
}

SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* info, SIZE_T) {
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    std::ifstream maps("/proc/self/maps");
    std::string line;
    memset(info, 0, sizeof *info);
    uintptr_t prev_end = 0;
    while (std::getline(maps, line)) {
        uintptr_t lo = 0, hi = 0;
        char perms[8] = {};
        if (sscanf(line.c_str(), "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (a < lo) {  // in a gap
            info->BaseAddress = reinterpret_cast<void*>(prev_end);
            info->RegionSize = lo - prev_end;
            info->State = MEM_FREE;
            info->Protect = PAGE_NOACCESS;
            return sizeof *info;
        }
        if (a < hi) {
            info->BaseAddress = info->AllocationBase = reinterpret_cast<void*>(lo);
            info->RegionSize = hi - lo;
            info->State = MEM_COMMIT;
            info->Type = MEM_PRIVATE;
            bool r = perms[0] == 'r', w = perms[1] == 'w', x = perms[2] == 'x';
            info->Protect = !r ? PAGE_NOACCESS : x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                                                   : (w ? PAGE_READWRITE : PAGE_READONLY);
            return sizeof *info;
        }
        prev_end = hi;
    }
    return 0;
}

// --- process, errors, strings -----------------------------------------------------------------------

DWORD GetLastError() { return t_last_error; }
void SetLastError(DWORD e) { t_last_error = e; }
void ExitProcess(UINT code) { _exit((int)code); }

DWORD GetModuleFileNameA(HMODULE, LPSTR out, DWORD size) {
    ssize_t n = readlink("/proc/self/exe", out, size ? size - 1 : 0);
    if (n < 0) n = 0;
    out[n] = 0;
    return (DWORD)n;
}

DWORD GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size) {
    char buf[1024];
    GetModuleFileNameA(m, buf, sizeof buf);
    return (DWORD)MultiByteToWideChar(CP_UTF8, 0, buf, -1, out, (int)size) - 1;
}

int MultiByteToWideChar(UINT, DWORD, const char* s, int n, wchar_t* out, int out_n) {
    size_t len = n < 0 ? strlen(s) + 1 : (size_t)n;
    std::u32string u = utf8_to_utf32(s, len);
    if (out_n == 0) return (int)u.size();
    int count = std::min<int>((int)u.size(), out_n);
    for (int i = 0; i < count; i++) out[i] = (wchar_t)u[i];
    return count;
}

int WideCharToMultiByte(UINT, DWORD, const wchar_t* s, int n, char* out, int out_n, const char*, BOOL*) {
    size_t len = n < 0 ? wcslen(s) + 1 : (size_t)n;
    std::string u = utf32_to_utf8(s, len);
    if (out_n == 0) return (int)u.size();
    int count = std::min<int>((int)u.size(), out_n);
    memcpy(out, u.data(), count);
    return count;
}

void OutputDebugStringA(const char* s) { __android_log_write(ANDROID_LOG_INFO, "ib3", s); }

HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }

HRESULT CoCreateGuid(GUID* g) {
    if (getrandom(g, sizeof *g, 0) != (ssize_t)sizeof *g) return -1;
    return S_OK;
}

DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR out, DWORD size) {
    const char* v = getenv(narrow(name).c_str());
    if (!v) return 0;
    return (DWORD)MultiByteToWideChar(CP_UTF8, 0, v, -1, out, (int)size) - 1;
}

BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value) {
    if (!value) return unsetenv(narrow(name).c_str()) == 0;
    return setenv(narrow(name).c_str(), narrow(value).c_str(), 1) == 0;
}

// --- .ini files -------------------------------------------------------------------------------------

UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, int def, LPCWSTR file) {
    std::lock_guard lock(g_ini_mutex);
    Ini ini = read_ini(narrow(file));
    const std::string* v = ini_find(ini, narrow(section), narrow(key));
    return v ? (UINT)atoi(v->c_str()) : (UINT)def;
}

DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file) {
    std::lock_guard lock(g_ini_mutex);
    Ini ini = read_ini(narrow(file));
    const std::string* v = ini_find(ini, narrow(section), narrow(key));
    std::string value = v ? *v : narrow(def);
    int n = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out, (int)size);
    if (n > 0) out[n - 1] = 0;
    return n > 0 ? (DWORD)(n - 1) : 0;
}

BOOL WritePrivateProfileStringW(LPCWSTR section_w, LPCWSTR key_w, LPCWSTR value_w, LPCWSTR file_w) {
    std::lock_guard lock(g_ini_mutex);
    std::string path = narrow(file_w), section = narrow(section_w), key = narrow(key_w), value = narrow(value_w);
    Ini ini = read_ini(path);
    auto sec = std::find_if(ini.begin(), ini.end(), [&](auto& s) { return strcasecmp(s.first.c_str(), section.c_str()) == 0; });
    if (sec == ini.end()) {
        ini.push_back({section, {}});
        sec = ini.end() - 1;
    }
    auto k = std::find_if(sec->second.begin(), sec->second.end(),
                          [&](auto& kv) { return strcasecmp(kv.first.c_str(), key.c_str()) == 0; });
    if (k != sec->second.end()) k->second = value;
    else sec->second.push_back({key, value});
    std::ofstream out(path, std::ios::trunc);
    for (auto& [s, keys] : ini) {
        out << "[" << s << "]\r\n";
        for (auto& [kk, vv] : keys) out << kk << "=" << vv << "\r\n";
    }
    return (BOOL)out.good();
}

// --- files (io.h) ---------------------------------------------------------------------------------------

int _wopen(const wchar_t* path, int flags, int mode) { return open(narrow(path).c_str(), flags, mode); }
int _wstat64(const wchar_t* path, struct stat* s) { return stat(narrow(path).c_str(), s); }
int _waccess(const wchar_t* path, int mode) { return access(narrow(path).c_str(), mode); }
int _wmkdir(const wchar_t* path) { return mkdir(narrow(path).c_str(), 0755); }
int _wrmdir(const wchar_t* path) { return rmdir(narrow(path).c_str()); }
int _wunlink(const wchar_t* path) { return unlink(narrow(path).c_str()); }
FILE* _wfopen(const wchar_t* path, const wchar_t* mode) { return fopen(narrow(path).c_str(), narrow(mode).c_str()); }

namespace {
struct Find : Handle {
    DIR* dir = nullptr;
    std::string path;
    ~Find() override {
        if (dir) closedir(dir);
    }
};

bool next_entry(Find* f, WIN32_FIND_DATAW* data) {
    while (dirent* e = readdir(f->dir)) {
        std::string name = e->d_name;
        bool is_dir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) {
            struct stat s;
            is_dir = stat((f->path + "/" + name).c_str(), &s) == 0 && S_ISDIR(s.st_mode);
        }
        data->dwFileAttributes = is_dir ? FILE_ATTRIBUTE_DIRECTORY : 0;
        int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, data->cFileName, MAX_PATH - 1);
        data->cFileName[n > 0 ? n - 1 : 0] = 0;
        return true;
    }
    return false;
}
}  // namespace

HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW* data) {
    std::string path = narrow(pattern);
    if (auto slash = path.find_last_of("\\/"); slash != std::string::npos) path.resize(slash);  // drop "\*"
    auto* f = new Find;
    f->path = path;
    f->dir = opendir(path.c_str());
    if (!f->dir || !next_entry(f, data)) {
        delete f;
        return INVALID_HANDLE_VALUE;
    }
    return static_cast<Handle*>(f);
}

BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW* data) {
    auto* f = dynamic_cast<Find*>(static_cast<Handle*>(h));
    return f && next_entry(f, data);
}

BOOL FindClose(HANDLE h) {
    delete dynamic_cast<Find*>(static_cast<Handle*>(h));
    return TRUE;
}

BOOL MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD) { return rename(narrow(from).c_str(), narrow(to).c_str()) == 0; }

HRESULT SetThreadDescription(HANDLE, LPCWSTR name) {
    std::string n = narrow(name).substr(0, 15);  // the kernel's limit
    pthread_setname_np(pthread_self(), n.c_str());
    return S_OK;
}

// --- message boxes ------------------------------------------------------------------------------------

int MessageBoxA(HWND, const char* text, const char* caption, UINT type) {
    __android_log_print(ANDROID_LOG_ERROR, "ib3", "[%s] %s", caption ? caption : "", text ? text : "");
    return (type & 0xf) == MB_OKCANCEL ? IDCANCEL : IDOK;
}

int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type) {
    return MessageBoxA(owner, narrow(text).c_str(), narrow(caption).c_str(), type);
}

// --- dynamic libraries ------------------------------------------------------------------------------

HMODULE LoadLibraryA(LPCSTR name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
HMODULE LoadLibraryW(LPCWSTR name) { return LoadLibraryA(narrow(name).c_str()); }
HMODULE GetModuleHandleW(LPCWSTR) { return nullptr; }
void* GetProcAddress(HMODULE m, const char* name) { return m ? dlsym(m, name) : nullptr; }
