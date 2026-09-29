// libz (via miniz) and libsqlite3 exposed to the guest.
#include "hle.h"
#include "libc/vfs.h"
#include "modules.h"
#include <miniz/miniz.h>
#include <sqlite/sqlite3.h>

namespace libc {

void install_thirdparty() {
    using hle::fn;

    // zlib: uLong/uLongf are 64-bit on Darwin, 32-bit in miniz on Windows.
    fn("_compressBound", [](u64 n) -> u64 { return mz_compressBound((mz_ulong)n); });
    fn("_compress", [](u8* dst, u64* dstlen, const u8* src, u64 srclen) {
        mz_ulong n = (mz_ulong)*dstlen;
        int r = mz_compress(dst, &n, src, (mz_ulong)srclen);
        *dstlen = n;
        return r;
    });
    fn("_uncompress", [](u8* dst, u64* dstlen, const u8* src, u64 srclen) {
        mz_ulong n = (mz_ulong)*dstlen;
        int r = mz_uncompress(dst, &n, src, (mz_ulong)srclen);
        *dstlen = n;
        return r;
    });

    // sqlite3 (handles and strings are host pointers == guest pointers)
    fn("_sqlite3_open", [](const char* name, sqlite3** db) {
        std::string h = vfs::to_host(name);
        LOG_DEBUG("sqlite3_open(%s) -> %s", name, h.c_str());
        return sqlite3_open(h.empty() ? ":memory:" : h.c_str(), db);
    });
    fn("_sqlite3_open_v2", [](const char* name, sqlite3** db, int flags, const char*) {
        std::string h = vfs::to_host(name);
        LOG_DEBUG("sqlite3_open_v2(%s) -> %s", name, h.c_str());
        return sqlite3_open_v2(h.empty() ? ":memory:" : h.c_str(), db, flags, nullptr);
    });
    fn("_sqlite3_errmsg", [](sqlite3* db) { return sqlite3_errmsg(db); });
    fn("_sqlite3_changes", [](sqlite3* db) { return sqlite3_changes(db); });
    fn("_sqlite3_close", [](sqlite3* db) { return sqlite3_close(db); });
    fn("_sqlite3_exec", [](sqlite3* db, const char* sql, GuestAddr cb, u64 arg, char** err) {
        struct Ctx {
            GuestAddr cb;
            u64 arg;
        } ctx{cb, arg};
        return sqlite3_exec(db, sql, cb ? +[](void* p, int n, char** vals, char** cols) -> int {
            auto* c = static_cast<Ctx*>(p);
            return (int)cpu::current().call(c->cb, {c->arg, (u64)n, gaddr(vals), gaddr(cols)});
        } : nullptr, &ctx, err);
    });
    fn("_sqlite3_prepare_v2", [](sqlite3* db, const char* sql, int n, sqlite3_stmt** st, const char** tail) {
        return sqlite3_prepare_v2(db, sql, n, st, tail);
    });
    fn("_sqlite3_step", [](sqlite3_stmt* s) { return sqlite3_step(s); });
    fn("_sqlite3_reset", [](sqlite3_stmt* s) { return sqlite3_reset(s); });
    fn("_sqlite3_finalize", [](sqlite3_stmt* s) { return sqlite3_finalize(s); });
    fn("_sqlite3_bind_int", [](sqlite3_stmt* s, int i, int v) { return sqlite3_bind_int(s, i, v); });
    fn("_sqlite3_bind_int64", [](sqlite3_stmt* s, int i, s64 v) { return sqlite3_bind_int64(s, i, v); });
    fn("_sqlite3_bind_double", [](sqlite3_stmt* s, int i, double v) { return sqlite3_bind_double(s, i, v); });
    fn("_sqlite3_bind_text", [](sqlite3_stmt* s, int i, const char* t, int n, u64) {
        return sqlite3_bind_text(s, i, t, n, SQLITE_TRANSIENT);
    });
    fn("_sqlite3_column_int", [](sqlite3_stmt* s, int i) { return sqlite3_column_int(s, i); });
    fn("_sqlite3_column_double", [](sqlite3_stmt* s, int i) { return sqlite3_column_double(s, i); });
    fn("_sqlite3_column_text", [](sqlite3_stmt* s, int i) { return sqlite3_column_text(s, i); });
    fn("_sqlite3_last_insert_rowid", [](sqlite3* db) -> s64 { return sqlite3_last_insert_rowid(db); });
}

}  // namespace libc
