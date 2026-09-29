// Guest threads (pthreads) on top of host threads.
#pragma once
#include "common.h"
#include <functional>

namespace libc {

// Starts a host thread that runs guest code; `body` runs with a guest CPU context.
// Returns the guest pthread_t.
GuestAddr spawn_guest_thread(const char* name, std::function<void()> body, size_t guest_stack = 1 << 20);

// pthread_t of the calling thread (creates one for foreign host threads).
GuestAddr pthread_self_handle();

// Recursive mutex that lives inside guest memory (zero-initialized == unlocked).
struct GuestMutex;
void guest_mutex_lock(GuestAddr m);
void guest_mutex_unlock(GuestAddr m);

}  // namespace libc
