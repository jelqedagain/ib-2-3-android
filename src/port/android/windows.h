// A small Win32 compatibility layer for the Android build: exactly the Windows functions the
// platform-independent parts of the runtime use, implemented on Linux/bionic (compat.cpp).
// Wide strings are wchar_t, which is 32-bit here; conversions use UTF-8 <-> UTF-32.
#pragma once
#include <pthread.h>
#include <sched.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cwchar>

#define WINAPI
#define CALLBACK
#define __stdcall
#define APIENTRY

using BOOL = int;
using BYTE = uint8_t;
using WORD = uint16_t;
using DWORD = uint32_t;
using UINT = unsigned int;
using ULONG = unsigned long;
using LONG = int32_t;
using LONGLONG = int64_t;
using ULONGLONG = uint64_t;
using ULONG_PTR = uintptr_t;
using SIZE_T = size_t;
using LPVOID = void*;
using LPCVOID = const void*;
using LPSTR = char*;
using LPCSTR = const char*;
using LPWSTR = wchar_t*;
using LPCWSTR = const wchar_t*;
using HANDLE = void*;
using HMODULE = void*;
using HWND = void*;
using HRESULT = int32_t;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define MAX_PATH 260
#define CP_UTF8 65001u
#define S_OK 0
#define S_FALSE 1
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define ERROR_SUCCESS 0u
#define COINIT_MULTITHREADED 0

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
};
union ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
};
struct FILETIME {
    DWORD dwLowDateTime, dwHighDateTime;
};
struct GUID {
    uint32_t Data1;
    uint16_t Data2, Data3;
    uint8_t Data4[8];
};

// --- threads, time ---------------------------------------------------------------------------
DWORD GetCurrentThreadId();
DWORD GetCurrentProcessId();
HANDLE GetCurrentProcess();
HANDLE GetCurrentThread();
void Sleep(DWORD ms);
inline BOOL SwitchToThread() { return sched_yield() == 0; }
ULONGLONG GetTickCount64();
inline DWORD GetTickCount() { return (DWORD)GetTickCount64(); }
BOOL QueryPerformanceCounter(LARGE_INTEGER* v);
BOOL QueryPerformanceFrequency(LARGE_INTEGER* v);
void GetSystemTimePreciseAsFileTime(FILETIME* ft);
inline void GetSystemTimeAsFileTime(FILETIME* ft) { GetSystemTimePreciseAsFileTime(ft); }
inline UINT timeBeginPeriod(UINT) { return 0; }
uintptr_t _beginthreadex(void* security, unsigned stack, unsigned (*fn)(void*), void* arg, unsigned flags, unsigned* id);
HRESULT SetThreadDescription(HANDLE thread, LPCWSTR name);  // the current thread only

// --- locks (zero-initialised = ready, 8 bytes each, like Windows) ---------------------------------
struct SRWLOCK {
    std::atomic<uint32_t> state;  // 0 free, 1 locked, 2 locked with waiters
    uint32_t pad;
};
struct CONDITION_VARIABLE {
    std::atomic<uint32_t> seq;
    uint32_t pad;
};
#define SRWLOCK_INIT {}
#define CONDITION_VARIABLE_INIT {}
void InitializeSRWLock(SRWLOCK* l);
void AcquireSRWLockExclusive(SRWLOCK* l);
BOOL TryAcquireSRWLockExclusive(SRWLOCK* l);
void ReleaseSRWLockExclusive(SRWLOCK* l);
void InitializeConditionVariable(CONDITION_VARIABLE* c);
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE* c, SRWLOCK* l, DWORD ms, ULONG flags);
void WakeConditionVariable(CONDITION_VARIABLE* c);
void WakeAllConditionVariable(CONDITION_VARIABLE* c);

// --- events and handles --------------------------------------------------------------------------
HANDLE CreateEventW(void* security, BOOL manual_reset, BOOL initial, LPCWSTR name);
BOOL SetEvent(HANDLE e);
BOOL ResetEvent(HANDLE e);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
BOOL CloseHandle(HANDLE h);

// --- memory ---------------------------------------------------------------------------------------
#define MEM_COMMIT 0x1000
#define MEM_RESERVE 0x2000
#define MEM_RELEASE 0x8000
#define MEM_DECOMMIT 0x4000
#define MEM_FREE 0x10000
#define MEM_PRIVATE 0x20000
#define MEM_IMAGE 0x1000000
#define PAGE_NOACCESS 0x01
#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define PAGE_EXECUTE_READ 0x20
#define PAGE_EXECUTE_READWRITE 0x40
#define PAGE_GUARD 0x100
struct MEMORY_BASIC_INFORMATION {
    void* BaseAddress;
    void* AllocationBase;
    DWORD AllocationProtect;
    SIZE_T RegionSize;
    DWORD State, Protect, Type;
};
// The device's memory page size: 4 KB on most phones, 16 KB on some newer ones.
size_t host_page_size();
void* VirtualAlloc(void* addr, SIZE_T size, DWORD type, DWORD protect);
BOOL VirtualFree(void* addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(void* addr, SIZE_T size, DWORD protect, DWORD* old);
SIZE_T VirtualQuery(const void* addr, MEMORY_BASIC_INFORMATION* info, SIZE_T len);

// --- process, errors, strings ----------------------------------------------------------------------
DWORD GetLastError();
void SetLastError(DWORD e);
[[noreturn]] void ExitProcess(UINT code);
DWORD GetModuleFileNameW(HMODULE m, LPWSTR out, DWORD size);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD size);
int MultiByteToWideChar(UINT cp, DWORD flags, const char* s, int n, wchar_t* out, int out_n);
int WideCharToMultiByte(UINT cp, DWORD flags, const wchar_t* s, int n, char* out, int out_n, const char* def, BOOL* used);
void OutputDebugStringA(const char* s);
HRESULT CoInitializeEx(void*, DWORD);
HRESULT CoCreateGuid(GUID* g);
DWORD GetEnvironmentVariableW(LPCWSTR name, LPWSTR out, DWORD size);
BOOL SetEnvironmentVariableW(LPCWSTR name, LPCWSTR value);

// --- .ini files (settings.ini) ---------------------------------------------------------------------
UINT GetPrivateProfileIntW(LPCWSTR section, LPCWSTR key, int def, LPCWSTR file);
DWORD GetPrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR def, LPWSTR out, DWORD size, LPCWSTR file);
BOOL WritePrivateProfileStringW(LPCWSTR section, LPCWSTR key, LPCWSTR value, LPCWSTR file);

// --- dynamic libraries -----------------------------------------------------------------------------
HMODULE LoadLibraryW(LPCWSTR name);
HMODULE LoadLibraryA(LPCSTR name);
HMODULE GetModuleHandleW(LPCWSTR name);
void* GetProcAddress(HMODULE m, const char* name);

// --- message boxes (logged; the Android UI shows errors itself) --------------------------------------
#define MB_OK 0x0u
#define MB_OKCANCEL 0x1u
#define MB_ICONERROR 0x10u
#define MB_ICONQUESTION 0x20u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MB_DEFBUTTON1 0x0u
#define MB_TOPMOST 0x40000u
#define IDOK 1
#define IDCANCEL 2
int MessageBoxA(HWND owner, const char* text, const char* caption, UINT type);
int MessageBoxW(HWND owner, LPCWSTR text, LPCWSTR caption, UINT type);

// --- CRT extras ----------------------------------------------------------------------------------------
#include <strings.h>
inline int _stricmp(const char* a, const char* b) { return strcasecmp(a, b); }
inline int _strnicmp(const char* a, const char* b, size_t n) { return strncasecmp(a, b, n); }
inline int _wtoi(const wchar_t* s) { return (int)wcstol(s, nullptr, 10); }

// --- virtual-key codes (keyboard.cpp's bindings are stored as Windows key codes) ------------------------
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_NUMPAD0 0x60
#define VK_NUMPAD9 0x69
#define VK_F1 0x70
#define VK_F4 0x73
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
