// Minimal Unreal Engine 3 reflection for IB3's arm64 build: names, property lookup by name,
// script function calls. Everything here runs guest code, so call it on the game thread only.
#pragma once
#include "cpu.h"
#include <string>
#include <vector>

namespace macho { struct Image; }

namespace ue {

// Native layout of this build (UObject / UField / UStruct), from Ghidra.
constexpr u64 kObjName = 0x48;       // FName
constexpr u64 kObjClass = 0x50;      // UClass*
constexpr u64 kFieldNext = 0x60;     // UField::Next
constexpr u64 kStructChildren = 0x70;

struct FString {
    u64 data;  // wchar_t* (UTF-32)
    s32 num, max;
};
template <class T>
struct TArray {
    u64 data;
    s32 num, max;
    T at(int i) const { return gptr<T>(data)[i]; }
};

bool init(const macho::Image& img);

u64 fname(cpu::Thread& t, const std::string& name);  // FName (FNAME_Add), cached
const u32* wide(const std::string& s);  // guest wchar_t (UTF-32) copy of an ASCII string, cached
std::string name_string(cpu::Thread& t, u64 name);
std::string object_name(cpu::Thread& t, GuestAddr obj);
std::string class_name(cpu::Thread& t, GuestAddr obj);
bool is_a(cpu::Thread& t, GuestAddr obj, const std::string& class_name);
std::string read_fstring(GuestAddr fstring);

// Byte offset of script property `name` in objects of `obj`'s class (searching super classes),
// or -1. Cached per class.
int property_offset(cpu::Thread& t, GuestAddr obj, const char* name);
template <class T>
bool read_property(cpu::Thread& t, GuestAddr obj, const char* name, T& out) {
    int off = obj ? property_offset(t, obj, name) : -1;
    if (off < 0) return false;
    out = *gptr<T>(obj + off);
    return true;
}

// Byte offset in `obj` of member `member` of its struct property `struct_prop`, or -1.
int struct_member_offset(cpu::Thread& t, GuestAddr obj, const char* struct_prop, const char* member);

// Byte offset of parameter `param` in the parameter block of `obj`'s script function `func`, or -1.
int param_offset(cpu::Thread& t, GuestAddr obj, const std::string& func, const char* param);

// Calls script function `func` on `obj` with a parameter block (UObject::ProcessEvent).
bool call_event(cpu::Thread& t, GuestAddr obj, const std::string& func, void* params);

// Logs every script property of `obj`'s class (name, type, offset), for reverse engineering.
void dump_properties(cpu::Thread& t, GuestAddr obj, const char* filter = nullptr);

// Logs the values of `obj`'s game (Sword*) properties, then those of the game objects it points to.
void dump_values(cpu::Thread& t, GuestAddr obj, int depth = 0);

// Logs the game (Sword*) script functions of `obj`'s class whose names contain one of `words`
// ("gold|xp|level", lower case).
void dump_functions(cpu::Thread& t, GuestAddr obj, const char* words);

// GEngine, or 0 before the engine exists.
GuestAddr engine();

// GEngine->GamePlayers[0]->Actor (the player controller), or 0.
GuestAddr player_controller(cpu::Thread& t);

// GEngine->GamePlayers[0]->Actor->PlayerInput, or 0.
GuestAddr player_input(cpu::Thread& t);

}  // namespace ue
