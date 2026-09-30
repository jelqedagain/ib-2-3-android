// Guest (iOS) path <-> host path mapping.
#pragma once
#include "common.h"
#include <string>

namespace vfs {

// Guest-visible locations.
extern const char* const kBundlePath;  // .../SwordGame.app
extern const char* const kHomePath;    // app sandbox (Documents, Library, tmp)

// Host directories (set in main before running guest code).
void set_roots(const std::string& host_bundle, const std::string& host_home);
const std::string& host_home();
const std::string& host_bundle();

// Serves `host_path` in place of a file of the app bundle (`bundle_relative`, e.g. "Binaries/Commands.txt").
void override_bundle_file(const std::string& bundle_relative, const std::string& host_path);

// Files in the app sandbox whose names start with `prefix` are the same files without it. (The game names its
// saves after the player's online account; offline ClashMobs give it one, and it must keep the same saves.)
void strip_home_file_prefix(const std::string& prefix);

// Maps a guest path (absolute or relative to the guest cwd) to a host UTF-8 path.
// Returns "" for paths outside the sandbox that have no host equivalent.
std::string to_host(const char* guest_path);
std::wstring to_host_w(const char* guest_path);

void set_cwd(const std::string& guest_path);

// Darwin errno for the last host CRT/Win32 failure.
int darwin_errno_from_crt(int e);
void set_errno(int darwin_errno);
GuestAddr errno_location();

}  // namespace vfs
