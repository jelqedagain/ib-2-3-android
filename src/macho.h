// Loads the arm64 slice of a Mach-O executable at its preferred address.
#pragma once
#include "common.h"
#include <map>
#include <vector>

namespace macho {

struct Section {
    std::string segname, sectname;
    GuestAddr addr = 0;
    u64 size = 0;
    u32 flags = 0;
    u32 reserved1 = 0;
};

struct Bind {
    GuestAddr addr;      // location to patch
    std::string symbol;  // mangled, with leading '_'
    int dylib_ordinal;
    s64 addend;
    bool lazy;
    bool weak_import;
};

struct Image {
    std::vector<u8> file;  // the thin arm64 slice
    GuestAddr base = 0;     // __TEXT vmaddr
    GuestAddr end = 0;
    GuestAddr entry = 0;    // LC_MAIN entry point (absolute)
    std::vector<Section> sections;
    std::vector<std::string> dylibs;
    std::vector<Bind> binds;
    std::map<std::string, GuestAddr> exports;         // defined external symbols
    std::map<std::string, GuestAddr> locals;          // non-external symbols (e.g. template instances)
    std::map<GuestAddr, std::string> symbols_by_addr;  // for symbolication
    std::vector<GuestAddr> function_starts;

    const Section* section(std::string_view seg, std::string_view sect) const;
    // Returns "symbol+0x12" for a code address, or "" if unknown.
    std::string symbolize(GuestAddr addr) const;
    // Address of an exported/defined symbol (e.g. "_bIPhonePortraitMode"), or 0.
    GuestAddr find(const std::string& name) const {
        auto it = exports.find(name);
        if (it != exports.end()) return it->second;
        auto lt = locals.find(name);
        return lt == locals.end() ? 0 : lt->second;
    }
    bool contains(GuestAddr a) const { return a >= base && a < end; }
};

// Parses and maps the image; does not process binds (see Image::binds).
Image load(const std::string& path);

}  // namespace macho
