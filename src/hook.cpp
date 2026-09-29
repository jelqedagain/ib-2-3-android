#include "hook.h"
#include "hle.h"

namespace hook {

namespace {

// True for instructions whose meaning depends on their address (cannot be moved).
bool pc_relative(u32 insn) {
    if ((insn & 0x1f000000) == 0x10000000) return true;  // ADR / ADRP
    if ((insn & 0x7c000000) == 0x14000000) return true;  // B / BL
    if ((insn & 0xff000010) == 0x54000000) return true;  // B.cond
    if ((insn & 0x7e000000) == 0x34000000) return true;  // CBZ / CBNZ
    if ((insn & 0x7e000000) == 0x36000000) return true;  // TBZ / TBNZ
    if ((insn & 0x3b000000) == 0x18000000) return true;  // LDR (literal) / PRFM literal
    return false;
}

constexpr u32 kLdrX16Plus8 = 0x58000050;  // ldr x16, #8
constexpr u32 kBrX16 = 0xd61f0200;        // br x16

}  // namespace

GuestAddr install(GuestAddr func, const std::string& name, cpu::Handler handler) {
    u32* code = gptr<u32>(func);
    for (int i = 0; i < 4; i++) {
        if (pc_relative(code[i])) {
            LOG_ERROR("hook %s: prologue instruction %d (0x%08x) is PC-relative; not hooked", name.c_str(), i, code[i]);
            return 0;
        }
    }
    // Trampoline: the four displaced instructions, then a jump back to func + 16.
    auto* tramp = static_cast<u32*>(hle::alloc_static(32, 16));
    for (int i = 0; i < 4; i++) tramp[i] = code[i];
    tramp[4] = kLdrX16Plus8;
    tramp[5] = kBrX16;
    *reinterpret_cast<u64*>(&tramp[6]) = func + 16;

    GuestAddr stub = cpu::make_stub("hook:" + name, std::move(handler));
    code[0] = kLdrX16Plus8;
    code[1] = kBrX16;
    *reinterpret_cast<u64*>(&code[2]) = stub;
    LOG_DEBUG("hooked %s at 0x%llx", name.c_str(), (unsigned long long)func);
    return gaddr(tramp);
}

}  // namespace hook
