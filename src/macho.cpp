#include "macho.h"
#include <algorithm>
#include <fstream>
#include <windows.h>

namespace macho {

namespace {

constexpr u32 FAT_MAGIC_BE = 0xcafebabe;
constexpr u32 MH_MAGIC_64 = 0xfeedfacf;
constexpr u32 CPU_TYPE_ARM64 = 0x0100000c;

constexpr u32 LC_SEGMENT_64 = 0x19;
constexpr u32 LC_SYMTAB = 0x2;
constexpr u32 LC_LOAD_DYLIB = 0xc;
constexpr u32 LC_LOAD_WEAK_DYLIB = 0x80000018;
constexpr u32 LC_DYLD_INFO = 0x22;
constexpr u32 LC_DYLD_INFO_ONLY = 0x80000022;
constexpr u32 LC_MAIN = 0x80000028;
constexpr u32 LC_FUNCTION_STARTS = 0x26;

u32 be32(const u8* p) { return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3]; }
template <typename T>
T rd(const std::vector<u8>& d, size_t off) {
    T v;
    std::memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

u64 uleb(const u8*& p) {
    u64 result = 0;
    int shift = 0;
    u8 byte;
    do {
        byte = *p++;
        result |= u64(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    return result;
}
s64 sleb(const u8*& p) {
    s64 result = 0;
    int shift = 0;
    u8 byte;
    do {
        byte = *p++;
        result |= s64(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) result |= -(s64(1) << shift);
    return result;
}

struct SegInfo {
    std::string name;
    u64 vmaddr, vmsize, fileoff, filesize;
};

// Runs a bind opcode stream (dyld's BIND_OPCODE_*), appending to out.
void parse_binds(const u8* p, const u8* end, const std::vector<SegInfo>& segs, bool lazy, std::vector<Bind>& out) {
    int ordinal = 0;
    std::string sym;
    u8 sym_flags = 0;
    s64 addend = 0;
    u64 addr = 0;
    auto do_bind = [&] {
        out.push_back(Bind{addr, sym, ordinal, addend, lazy, (sym_flags & 1) != 0});
        addr += 8;
    };
    while (p < end) {
        u8 b = *p++;
        u8 op = b & 0xf0, imm = b & 0x0f;
        switch (op) {
        case 0x00:  // DONE
            if (!lazy) return;
            break;
        case 0x10: ordinal = imm; break;
        case 0x20: ordinal = (int)uleb(p); break;
        case 0x30: ordinal = imm ? (int)(s8)(0xf0 | imm) : 0; break;
        case 0x40:
            sym_flags = imm;
            sym = reinterpret_cast<const char*>(p);
            p += sym.size() + 1;
            break;
        case 0x50: break;  // type: always pointer on arm64
        case 0x60: addend = sleb(p); break;
        case 0x70: addr = segs.at(imm).vmaddr + uleb(p); break;
        case 0x80: addr += uleb(p); break;
        case 0x90: do_bind(); break;
        case 0xa0: do_bind(); addr += uleb(p); break;
        case 0xb0: do_bind(); addr += u64(imm) * 8; break;
        case 0xc0: {
            u64 count = uleb(p), skip = uleb(p);
            for (u64 i = 0; i < count; i++) { do_bind(); addr += skip; }
            break;
        }
        default: fatal("bad bind opcode %02x", b);
        }
    }
}

}  // namespace

const Section* Image::section(std::string_view seg, std::string_view sect) const {
    for (auto& s : sections)
        if (s.segname == seg && s.sectname == sect) return &s;
    return nullptr;
}

std::string Image::symbolize(GuestAddr addr) const {
    if (!contains(addr) || symbols_by_addr.empty()) return {};
    auto it = symbols_by_addr.upper_bound(addr);
    if (it == symbols_by_addr.begin()) return {};
    --it;
    // Prefer a closer function start if the symbol is far away (stripped local function).
    auto fs = std::upper_bound(function_starts.begin(), function_starts.end(), addr);
    GuestAddr fstart = fs == function_starts.begin() ? 0 : *(fs - 1);
    char buf[64];
    if (fstart > it->first) {
        snprintf(buf, sizeof(buf), "sub_%llx+0x%llx", (unsigned long long)fstart, (unsigned long long)(addr - fstart));
        return buf;
    }
    snprintf(buf, sizeof(buf), "+0x%llx", (unsigned long long)(addr - it->first));
    return it->second + buf;
}

Image load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) fatal("cannot open %s", path.c_str());
    std::vector<u8> whole((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    Image img;
    if (be32(whole.data()) == FAT_MAGIC_BE) {
        u32 n = be32(whole.data() + 4);
        for (u32 i = 0; i < n; i++) {
            const u8* fa = whole.data() + 8 + 20 * i;
            if (be32(fa) == CPU_TYPE_ARM64) {
                u32 off = be32(fa + 8), size = be32(fa + 12);
                img.file.assign(whole.begin() + off, whole.begin() + off + size);
            }
        }
        if (img.file.empty()) fatal("no arm64 slice in %s", path.c_str());
    } else {
        img.file = std::move(whole);
    }
    auto& d = img.file;
    if (rd<u32>(d, 0) != MH_MAGIC_64) fatal("not a 64-bit Mach-O");
    u32 ncmds = rd<u32>(d, 16);

    std::vector<SegInfo> segs;
    u32 symoff = 0, nsyms = 0, stroff = 0;
    u32 bind_off = 0, bind_size = 0, lazy_off = 0, lazy_size = 0, export_off = 0, export_size = 0;
    u32 fs_off = 0, fs_size = 0;
    u64 entryoff = 0;

    size_t p = 32;
    for (u32 c = 0; c < ncmds; c++) {
        u32 cmd = rd<u32>(d, p), cmdsize = rd<u32>(d, p + 4);
        switch (cmd) {
        case LC_SEGMENT_64: {
            SegInfo s;
            s.name = std::string(reinterpret_cast<const char*>(&d[p + 8]), strnlen(reinterpret_cast<const char*>(&d[p + 8]), 16));
            s.vmaddr = rd<u64>(d, p + 24);
            s.vmsize = rd<u64>(d, p + 32);
            s.fileoff = rd<u64>(d, p + 40);
            s.filesize = rd<u64>(d, p + 48);
            u32 nsects = rd<u32>(d, p + 64);
            for (u32 k = 0; k < nsects; k++) {
                size_t q = p + 72 + 80 * k;
                Section sec;
                sec.sectname = std::string(reinterpret_cast<const char*>(&d[q]), strnlen(reinterpret_cast<const char*>(&d[q]), 16));
                sec.segname = std::string(reinterpret_cast<const char*>(&d[q + 16]), strnlen(reinterpret_cast<const char*>(&d[q + 16]), 16));
                sec.addr = rd<u64>(d, q + 32);
                sec.size = rd<u64>(d, q + 40);
                sec.flags = rd<u32>(d, q + 64);
                sec.reserved1 = rd<u32>(d, q + 68);
                img.sections.push_back(sec);
            }
            segs.push_back(s);
            break;
        }
        case LC_SYMTAB:
            symoff = rd<u32>(d, p + 8);
            nsyms = rd<u32>(d, p + 12);
            stroff = rd<u32>(d, p + 16);
            break;
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
            img.dylibs.emplace_back(reinterpret_cast<const char*>(&d[p + rd<u32>(d, p + 8)]));
            break;
        case LC_DYLD_INFO:
        case LC_DYLD_INFO_ONLY:
            bind_off = rd<u32>(d, p + 16);
            bind_size = rd<u32>(d, p + 20);
            lazy_off = rd<u32>(d, p + 32);
            lazy_size = rd<u32>(d, p + 36);
            export_off = rd<u32>(d, p + 40);
            export_size = rd<u32>(d, p + 44);
            break;
        case LC_MAIN: entryoff = rd<u64>(d, p + 8); break;
        case LC_FUNCTION_STARTS:
            fs_off = rd<u32>(d, p + 8);
            fs_size = rd<u32>(d, p + 12);
            break;
        }
        p += cmdsize;
    }

    // Map every segment except __PAGEZERO in one reservation at the preferred address.
    GuestAddr lo = ~0ull, hi = 0;
    for (auto& s : segs) {
        if (s.name == "__PAGEZERO") continue;
        lo = std::min(lo, s.vmaddr);
        hi = std::max(hi, s.vmaddr + s.vmsize);
    }
    hi = (hi + 0xffff) & ~0xffffull;
    // main() normally reserved this range already; commit inside that reservation.
    void* mem = VirtualAlloc(gptr<void>(lo), hi - lo, MEM_COMMIT, PAGE_READWRITE);
    if (!mem) mem = VirtualAlloc(gptr<void>(lo), hi - lo, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem != gptr<void>(lo))
        fatal("could not map image at 0x%llx (err %lu)", (unsigned long long)lo, GetLastError());
    for (auto& s : segs) {
        if (s.name == "__PAGEZERO" || s.filesize == 0) continue;
        std::memcpy(gptr<void>(s.vmaddr), d.data() + s.fileoff, s.filesize);
    }
    img.base = lo;
    img.end = hi;
    for (auto& s : segs)
        if (s.name == "__TEXT") img.entry = s.vmaddr + entryoff;

    // Symbols: exports (for weak-bind coalescing) and names for symbolication.
    for (u32 i = 0; i < nsyms; i++) {
        size_t o = symoff + 16 * i;
        u32 strx = rd<u32>(d, o);
        u8 type = d[o + 4];
        u64 value = rd<u64>(d, o + 8);
        if (type & 0xe0) continue;  // stab
        if ((type & 0x0e) != 0x0e) continue;  // not N_SECT
        std::string name(reinterpret_cast<const char*>(&d[stroff + strx]));
        if (type & 0x01) img.exports[name] = value;
        else img.locals.emplace(name, value);
        img.symbols_by_addr.emplace(value, name);
    }
    if (fs_size) {
        const u8* q = d.data() + fs_off;
        const u8* e = q + fs_size;
        GuestAddr a = img.base;
        while (q < e) {
            u64 delta = uleb(q);
            if (!delta) break;
            a += delta;
            img.function_starts.push_back(a);
        }
    }

    // Export trie: names for (many more) functions and globals than the symbol table has.
    if (export_size) {
        const u8* trie = d.data() + export_off;
        const u8* trie_end = trie + export_size;
        std::vector<std::pair<const u8*, std::string>> stack{{trie, ""}};
        while (!stack.empty()) {
            auto [node, prefix] = stack.back();
            stack.pop_back();
            if (node < trie || node >= trie_end) continue;
            const u8* q = node;
            u64 term = uleb(q);
            if (term) {
                const u8* t = q;
                u64 flags = uleb(t);
                if (!(flags & 0x8)) {  // not a re-export
                    GuestAddr a = img.base + uleb(t);
                    img.exports.emplace(prefix, a);
                    img.symbols_by_addr.emplace(a, prefix);
                }
            }
            q += term;
            u8 children = *q++;
            for (u8 c = 0; c < children; c++) {
                std::string edge(reinterpret_cast<const char*>(q));
                q += edge.size() + 1;
                u64 off = uleb(q);
                stack.push_back({trie + off, prefix + edge});
            }
        }
    }

    if (bind_size) parse_binds(d.data() + bind_off, d.data() + bind_off + bind_size, segs, false, img.binds);
    if (lazy_size) parse_binds(d.data() + lazy_off, d.data() + lazy_off + lazy_size, segs, true, img.binds);

    LOG_INFO("mapped %s: 0x%llx-0x%llx entry=0x%llx, %zu binds, %zu symbols, %zu functions", path.c_str(),
             (unsigned long long)img.base, (unsigned long long)img.end, (unsigned long long)img.entry,
             img.binds.size(), img.symbols_by_addr.size(), img.function_starts.size());
    return img;
}

}  // namespace macho
