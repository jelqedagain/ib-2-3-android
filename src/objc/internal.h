// Runtime internals shared between objc/*.cpp files.
#pragma once
#include "objc/runtime.h"
#include <mutex>
#include <unordered_map>
#include <vector>

namespace macho { struct Image; }

namespace objc {
extern std::string (*g_nsstring_utf8)(id);
extern bool (*g_is_nsstring)(id);

void realize_image_classes(const macho::Image& img);
Class block_class_malloc();
Class block_class_stack();
std::mutex& side_mutex();
std::unordered_map<id, std::vector<GuestAddr>>& weak_table();
std::unordered_map<id, std::unordered_map<u64, std::pair<id, u64>>>& assoc_table();
std::unordered_map<id, std::recursive_mutex*>& sync_table();
void msg_send_handler(cpu::Thread& t);
void msg_send_super2_handler(cpu::Thread& t);
GuestAddr block_copy_byref(GuestAddr a);
void block_release_byref(GuestAddr a);
void destroy(id obj);
std::vector<GuestAddr> class_protocols(Class c);
GuestAddr class_ro(Class c);
void add_guest_method(Class c, SEL s, GuestAddr imp);
}  // namespace objc
