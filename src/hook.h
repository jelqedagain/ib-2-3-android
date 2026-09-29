// Detours for guest (ARM64) functions.
#pragma once
#include "cpu.h"

namespace hook {

// Redirects the guest function at `func` to `handler`. The handler runs with the original
// arguments in registers; it can continue into the original function with t.jump(original)
// (the returned trampoline), or return normally to skip it (x0 = return value).
// The first four instructions must not be PC-relative; install before guest code runs.
// Returns the trampoline address, or 0 if the prologue cannot be relocated.
GuestAddr install(GuestAddr func, const std::string& name, cpu::Handler handler);

}  // namespace hook
