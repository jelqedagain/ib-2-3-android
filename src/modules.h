// Install functions for every HLE module, called in order from main().
#pragma once

namespace libc {
void install_string();
void install_stdio();
void install_fs();
void install_time();
void install_pthread();
void install_system();
void install_crypto();
void install_thirdparty();
}  // namespace libc
