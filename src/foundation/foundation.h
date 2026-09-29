// Host implementations of Foundation classes, plus helpers for other HLE modules.
#pragma once
#include "objc/runtime.h"
#include <string>
#include <vector>

namespace ns {
using objc::Class;
using objc::id;
using objc::SEL;

struct NSRange {
    u64 location, length;
};
constexpr u64 NSNotFound = 0x7fffffffffffffffull;

// --- NSString ---
bool is_string(id obj);
std::string utf8(id s);        // "" for nil
std::u16string utf16(id s);
id str(std::string_view utf8);           // autoreleased
id str_retained(std::string_view utf8);  // +1
id str16_retained(std::u16string s);     // +1
id string_with_format(Class cls, const char* fmt_utf8, GuestAddr va, bool retained);

// --- NSNumber / NSValue ---
id number_int(s64 v);  // autoreleased
id number_uint(u64 v);
id number_double(double v);
id number_bool(bool v);
bool is_number(id obj);
double number_double_value(id n);
s64 number_int_value(id n);

// --- collections ---
id array(const std::vector<id>& items);  // autoreleased NSArray
id mutable_array();
std::vector<id> array_items(id a);        // works for guest subclasses too
id dict(const std::vector<std::pair<id, id>>& items);  // autoreleased NSDictionary
id mutable_dict();
id dict_get(id d, id key);
void dict_set(id d, id key, id value);
std::vector<std::pair<id, id>> dict_items(id d);
bool is_array(id obj);
bool is_dict(id obj);
id null_object();

// --- NSData ---
id data_with(const void* bytes, u64 len);  // autoreleased
bool is_data(id obj);
std::vector<u8> data_bytes(id d);

// Hash / equality through the guest-visible protocol (-hash / -isEqual:).
u64 hash(id obj);
bool equal(id a, id b);

// Seconds since 2001-01-01 held by an NSDate.
double date_seconds(id date);

// Property lists (NSUserDefaults, Info.plist).
id plist_from_xml(const std::string& xml);  // retained object graph, 0 on failure
id plist_from_file(const std::string& host_path);
std::string plist_to_xml(id obj);

// Main-thread work queue shared by NSRunLoop, performSelectorOnMainThread and GCD's main queue.
void post_to_main(std::function<void()> fn);
bool is_main_thread();
void run_main_queue_once();  // called from the main run loop
void set_main_thread();

void install_string();
void install_collections();
void install_system();
void install_cf();
void install_thread();
void install_misc();

}  // namespace ns
