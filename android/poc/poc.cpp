// Android proof of concept: runs Infinity Blade III's own ARM64 code natively on the phone.
// Loads the thin arm64 Mach-O at its iOS addresses, calls the game's appMemCrc(), and checks the
// result against a reference implementation that reads the same CRC table.
//   adb push ib3poc SwordGame_arm64 /data/local/tmp/ && adb shell /data/local/tmp/ib3poc /data/local/tmp/SwordGame_arm64
#include <sys/mman.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kAppMemCrc = 0x1001632f4;  // appMemCrc(const void*, int, unsigned)
constexpr uint64_t kCrcTable = 0x100eb67cc;   // GCRCTable (uint32_t[256])

template <class T>
T rd(const std::vector<uint8_t>& d, size_t off) {
    T v;
    memcpy(&v, &d[off], sizeof v);
    return v;
}

bool map_image(const std::vector<uint8_t>& d) {
    if (rd<uint32_t>(d, 0) != 0xFEEDFACF) {
        printf("not a thin 64-bit Mach-O\n");
        return false;
    }
    uint32_t ncmds = rd<uint32_t>(d, 16);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd = rd<uint32_t>(d, off), size = rd<uint32_t>(d, off + 4);
        if (cmd == 0x19) {  // LC_SEGMENT_64
            char name[17] = {};
            memcpy(name, &d[off + 8], 16);
            uint64_t vmaddr = rd<uint64_t>(d, off + 24), vmsize = rd<uint64_t>(d, off + 32);
            uint64_t fileoff = rd<uint64_t>(d, off + 40), filesize = rd<uint64_t>(d, off + 48);
            if (strcmp(name, "__PAGEZERO") != 0 && vmsize) {
                void* p = mmap(reinterpret_cast<void*>(vmaddr), vmsize, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
                if (p != reinterpret_cast<void*>(vmaddr)) {
                    printf("could not map %s at 0x%llx\n", name, (unsigned long long)vmaddr);
                    return false;
                }
                memcpy(p, &d[fileoff], filesize);
                printf("mapped %-12s 0x%llx  %6.2f MB\n", name, (unsigned long long)vmaddr, vmsize / 1048576.0);
                if (!strcmp(name, "__TEXT")) {
                    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + vmsize);
                    if (mprotect(p, vmsize, PROT_READ | PROT_EXEC) != 0) {
                        printf("could not make __TEXT executable\n");
                        return false;
                    }
                }
            }
        }
        off += size;
    }
    return true;
}

uint32_t reference_crc(const uint8_t* p, int n, uint32_t crc) {
    const uint32_t* table = reinterpret_cast<const uint32_t*>(kCrcTable);
    crc = ~crc;
    for (int i = 0; i < n; i++) crc = table[p[i] ^ (crc >> 24)] ^ (crc << 8);
    return ~crc;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: %s SwordGame_arm64\n", argv[0]);
        return 1;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    std::vector<uint8_t> image(s.begin(), s.end());
    printf("read %.1f MB\n", image.size() / 1048576.0);
    if (!map_image(image)) return 1;

    // GCRCTable is filled at engine start-up (UE3's CRC-32, polynomial 0x04C11DB7); do the same.
    uint32_t* table = reinterpret_cast<uint32_t*>(kCrcTable);
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i << 24;
        for (int j = 0; j < 8; j++) crc = crc & 0x80000000 ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
        table[i] = crc;
    }

    using CrcFn = uint32_t (*)(const void*, int, uint32_t);
    auto game_crc = reinterpret_cast<CrcFn>(kAppMemCrc);
    const char* text = "Infinity Blade III, running natively on Android";
    uint32_t native = game_crc(text, (int)strlen(text), 0);
    uint32_t expect = reference_crc(reinterpret_cast<const uint8_t*>(text), (int)strlen(text), 0);
    printf("game's appMemCrc: 0x%08x   reference: 0x%08x   %s\n", native, expect, native == expect ? "MATCH" : "MISMATCH");

    std::vector<uint8_t> big(64 << 20);
    for (size_t i = 0; i < big.size(); i++) big[i] = (uint8_t)(i * 2654435761u >> 13);
    auto t0 = std::chrono::steady_clock::now();
    uint32_t c = game_crc(big.data(), (int)big.size(), 0);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("64 MB through the game's code natively: %.0f MB/s (crc 0x%08x, %s)\n", 64 / secs, c,
           c == reference_crc(big.data(), (int)big.size(), 0) ? "correct" : "WRONG");
    return native == expect ? 0 : 2;
}
