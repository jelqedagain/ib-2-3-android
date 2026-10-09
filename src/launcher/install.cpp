#include "launcher/install.h"
#include "miniz/miniz.h"
#include "settings.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <windows.h>

namespace fs = std::filesystem;

namespace launcher {

namespace {

constexpr const char* kAppPrefix = "Payload/SwordGame.app/";

struct Writer {
    FILE* f;
};
size_t write_cb(void* opaque, mz_uint64, const void* buf, size_t n) {
    return fwrite(buf, 1, n, static_cast<Writer*>(opaque)->f);
}

bool starts_with(const char* s, const char* prefix) { return strncmp(s, prefix, strlen(prefix)) == 0; }

}  // namespace

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring game_root() { return settings::exe_dir() + L"game\\"; }
std::wstring app_dir() { return game_root() + L"Payload\\SwordGame.app\\"; }

bool game_installed() {
    std::error_code ec;
    return fs::is_regular_file(app_dir() + L"SwordGame", ec) && fs::is_regular_file(app_dir() + L"CookedIPhone\\Engine.xxx", ec);
}

std::string installed_version() {
    std::ifstream f(fs::path(app_dir() + L"Info.plist"));
    std::stringstream ss;
    ss << f.rdbuf();
    std::string xml = ss.str();
    size_t k = xml.find("<key>CFBundleShortVersionString</key>");
    if (k == std::string::npos) return {};
    size_t a = xml.find("<string>", k), b = xml.find("</string>", k);
    if (a == std::string::npos || b == std::string::npos || b < a) return {};
    return xml.substr(a + 8, b - a - 8);
}

bool install_from_ipa(const std::wstring& ipa, const std::function<void(double)>& progress, std::wstring& error) {
    FILE* in = _wfopen(ipa.c_str(), L"rb");
    if (!in) {
        error = L"Could not open the .ipa file.";
        return false;
    }
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_cfile(&zip, in, 0, 0)) {
        fclose(in);
        error = L"This file is not a valid .ipa (zip) archive.";
        return false;
    }
    // Check it really is IB3 and add up the work.
    mz_uint count = mz_zip_reader_get_num_files(&zip);
    u64 total = 0;
    bool has_binary = false, has_engine = false;
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || !starts_with(st.m_filename, kAppPrefix)) continue;
        total += st.m_uncomp_size;
        has_binary |= strcmp(st.m_filename, "Payload/SwordGame.app/SwordGame") == 0;
        has_engine |= strcmp(st.m_filename, "Payload/SwordGame.app/CookedIPhone/Engine.xxx") == 0;
    }
    if (!has_binary || !has_engine) {
        mz_zip_reader_end(&zip);
        fclose(in);
        error = L"This .ipa does not contain IB3 (Payload/SwordGame.app).";
        return false;
    }
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(settings::exe_dir().c_str(), &free_bytes, nullptr, nullptr) &&
        free_bytes.QuadPart < total + (256ull << 20)) {
        mz_zip_reader_end(&zip);
        fclose(in);
        error = L"Not enough free disk space: installing needs about " + std::to_wstring(total >> 30 ? (total >> 30) + 1 : 1) +
                L" GB.";
        return false;
    }

    // Extract into a staging folder and move it into place only when everything succeeded.
    std::wstring staging = settings::exe_dir() + L"game.partial\\";
    std::error_code ec;
    fs::remove_all(staging, ec);
    u64 done = 0;
    bool ok = true;
    for (mz_uint i = 0; i < count && ok; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st) || !starts_with(st.m_filename, kAppPrefix)) continue;
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        if (((st.m_external_attr >> 16) & 0xF000) == 0xA000) continue;  // symlink
        std::wstring rel = widen(st.m_filename);
        for (auto& c : rel)
            if (c == L'/') c = L'\\';
        fs::path dest = fs::path(staging) / rel;
        fs::create_directories(dest.parent_path(), ec);
        Writer w{_wfopen(dest.c_str(), L"wb")};
        if (!w.f) {
            error = L"Could not write " + dest.wstring();
            ok = false;
            break;
        }
        ok = mz_zip_reader_extract_to_callback(&zip, i, write_cb, &w, 0);
        ok = (fclose(w.f) == 0) && ok;
        if (!ok) error = L"Extracting " + rel + L" failed (the .ipa may be damaged, or the disk is full).";
        done += st.m_uncomp_size;
        if (progress && total) progress((double)done / total);
    }
    mz_zip_reader_end(&zip);
    fclose(in);
    if (!ok) {
        fs::remove_all(staging, ec);
        return false;
    }
    fs::remove_all(game_root(), ec);
    std::wstring from = staging.substr(0, staging.size() - 1), to = game_root().substr(0, game_root().size() - 1);
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = L"Could not move the extracted files into place (error " + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    return game_installed();
}

}  // namespace launcher
