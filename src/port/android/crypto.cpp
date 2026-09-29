// CommonCrypto (CC_MD5/CC_SHA1/CCHmac/CCCrypt) for Android: small portable implementations
// (the Windows build uses CNG, libc/crypto.cpp).
#include "hle.h"
#include "modules.h"
#include <sys/random.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace libc {

namespace {

u32 rol(u32 x, int n) { return (x << n) | (x >> (32 - n)); }
u32 ror(u32 x, int n) { return (x >> n) | (x << (32 - n)); }
u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
void put_be32(u8* p, u32 v) { p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v; }

// Merkle-Damgard padding shared by MD5/SHA-1/SHA-256: 64-byte blocks, 64-bit bit length.
template <typename Block>
void md_pad(const u8* data, u64 len, bool big_endian_length, Block block) {
    u64 full = len / 64 * 64;
    for (u64 i = 0; i < full; i += 64) block(data + i);
    u8 tail[128] = {};
    u64 rest = len - full;
    std::memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    u64 n = rest + 1 + 8 <= 64 ? 64 : 128;
    u64 bits = len * 8;
    for (int i = 0; i < 8; i++) tail[n - 8 + i] = (u8)(big_endian_length ? bits >> (56 - 8 * i) : bits >> (8 * i));
    block(tail);
    if (n == 128) block(tail + 64);
}

void md5(const u8* data, u64 len, u8* out) {
    static u32 k[64];
    static const int s[16] = {7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21};
    if (!k[0])
        for (int i = 0; i < 64; i++) k[i] = (u32)(std::fabs(std::sin(i + 1.0)) * 4294967296.0);
    u32 h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    md_pad(data, len, false, [&](const u8* p) {
        u32 m[16];
        for (int i = 0; i < 16; i++) m[i] = (u32)p[4 * i] | (u32)p[4 * i + 1] << 8 | (u32)p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
        u32 a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; i++) {
            u32 f;
            int g;
            switch (i / 16) {
            case 0: f = (b & c) | (~b & d), g = i; break;
            case 1: f = (d & b) | (~d & c), g = (5 * i + 1) % 16; break;
            case 2: f = b ^ c ^ d, g = (3 * i + 5) % 16; break;
            default: f = c ^ (b | ~d), g = (7 * i) % 16; break;
            }
            u32 t = d;
            d = c;
            c = b;
            b = b + rol(a + f + k[i] + m[g], s[(i / 16) * 4 + i % 4]);
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d;
    });
    for (int i = 0; i < 16; i++) out[i] = (u8)(h[i / 4] >> (8 * (i % 4)));
}

void sha1(const u8* data, u64 len, u8* out) {
    u32 h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    md_pad(data, len, true, [&](const u8* p) {
        u32 w[80];
        for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5a827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ed9eba1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8f1bbcdc;
            else f = b ^ c ^ d, k = 0xca62c1d6;
            u32 t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    });
    for (int i = 0; i < 5; i++) put_be32(out + 4 * i, h[i]);
}

void sha256(const u8* data, u64 len, u8* out) {
    static const u32 k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    md_pad(data, len, true, [&](const u8* p) {
        u32 w[64];
        for (int i = 0; i < 16; i++) w[i] = be32(p + 4 * i);
        for (int i = 16; i < 64; i++) {
            u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            u32 t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    });
    for (int i = 0; i < 8; i++) put_be32(out + 4 * i, h[i]);
}

using HashFn = void (*)(const u8*, u64, u8*);

void hmac(HashFn hash, u32 outlen, const u8* key, u64 keylen, const u8* data, u64 len, u8* out) {
    u8 k[64] = {};
    if (keylen > 64) hash(key, keylen, k);
    else std::memcpy(k, key, keylen);
    std::vector<u8> inner(64 + len);
    for (int i = 0; i < 64; i++) inner[i] = k[i] ^ 0x36;
    std::memcpy(inner.data() + 64, data, len);
    u8 outer[64 + 32];
    for (int i = 0; i < 64; i++) outer[i] = k[i] ^ 0x5c;
    hash(inner.data(), inner.size(), outer + 64);
    hash(outer, 64 + outlen, out);
}

// --- AES (FIPS-197), byte-oriented; state byte 4*c + r is row r of column c ---------------------------

u8 g_sbox[256], g_inv_sbox[256];

u8 rotl8(u8 x, int n) { return (u8)((x << n) | (x >> (8 - n))); }
u8 xtime(u8 x) { return (u8)((x << 1) ^ (x & 0x80 ? 0x1b : 0)); }
u8 gmul(u8 a, u8 b) {
    u8 r = 0;
    for (; b; b >>= 1, a = xtime(a))
        if (b & 1) r ^= a;
    return r;
}

void init_sbox() {
    if (g_sbox[0]) return;
    u8 p = 1, q = 1;
    do {
        p = (u8)(p ^ (p << 1) ^ (p & 0x80 ? 0x1b : 0));  // p * 3
        q ^= (u8)(q << 1);                               // q / 3
        q ^= (u8)(q << 2);
        q ^= (u8)(q << 4);
        if (q & 0x80) q ^= 0x09;
        u8 x = q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4);
        g_sbox[p] = x ^ 0x63;
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_inv_sbox[g_sbox[i]] = (u8)i;
}

struct Aes {
    int rounds = 0;
    u8 rk[240];

    bool set_key(const u8* key, u64 keylen) {
        if (keylen != 16 && keylen != 24 && keylen != 32) return false;
        init_sbox();
        int nk = (int)keylen / 4;
        rounds = nk + 6;
        std::memcpy(rk, key, keylen);
        u8 rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); i++) {
            u8 t[4];
            std::memcpy(t, rk + 4 * (i - 1), 4);
            if (i % nk == 0) {
                u8 t0 = t[0];
                t[0] = g_sbox[t[1]] ^ rcon, t[1] = g_sbox[t[2]], t[2] = g_sbox[t[3]], t[3] = g_sbox[t0];
                rcon = xtime(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (u8& b : t) b = g_sbox[b];
            }
            for (int j = 0; j < 4; j++) rk[4 * i + j] = rk[4 * (i - nk) + j] ^ t[j];
        }
        return true;
    }

    void add(u8* s, int round) const {
        for (int i = 0; i < 16; i++) s[i] ^= rk[16 * round + i];
    }

    void encrypt(u8* s) const {
        add(s, 0);
        for (int round = 1; round <= rounds; round++) {
            u8 t[16];
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) t[4 * c + r] = g_sbox[s[4 * ((c + r) % 4) + r]];
            if (round != rounds)
                for (int c = 0; c < 4; c++) {
                    u8* a = t + 4 * c;
                    u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3;
                    a[1] = a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3;
                    a[2] = a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3);
                    a[3] = (xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3);
                }
            std::memcpy(s, t, 16);
            add(s, round);
        }
    }

    void decrypt(u8* s) const {
        add(s, rounds);
        for (int round = rounds - 1; round >= 0; round--) {
            u8 t[16];
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) t[4 * c + r] = g_inv_sbox[s[4 * ((c - r + 4) % 4) + r]];
            std::memcpy(s, t, 16);
            add(s, round);
            if (round != 0)
                for (int c = 0; c < 4; c++) {
                    u8* a = s + 4 * c;
                    u8 a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                    a[0] = gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9);
                    a[1] = gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13);
                    a[2] = gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11);
                    a[3] = gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14);
                }
        }
    }
};

constexpr s32 kCCSuccess = 0, kCCParamError = -4300, kCCBufferTooSmall = -4301, kCCAlignmentError = -4303,
              kCCDecodeError = -4304, kCCUnimplemented = -4305;

s32 cc_crypt(u32 op, u32 alg, u32 options, const u8* key, u64 keylen, const u8* iv, const u8* in, u64 inlen, u8* out,
             u64 outavail, u64* moved) {
    if (alg != 0) {  // only kCCAlgorithmAES128
        LOG_WARN("CCCrypt: algorithm %u not implemented", alg);
        return kCCUnimplemented;
    }
    bool pkcs7 = options & 1, ecb = options & 2;
    Aes aes;
    if (!aes.set_key(key, keylen)) return kCCParamError;
    u8 chain[16] = {};
    if (iv && !ecb) std::memcpy(chain, iv, 16);
    if (moved) *moved = 0;
    if (op == 0) {  // encrypt
        if (!pkcs7 && inlen % 16) return kCCAlignmentError;
        u64 total = pkcs7 ? (inlen / 16 + 1) * 16 : inlen;
        if (outavail < total) {
            if (moved) *moved = total;
            return kCCBufferTooSmall;
        }
        for (u64 off = 0; off < total; off += 16) {
            u8 b[16];
            u64 n = std::min<u64>(16, inlen > off ? inlen - off : 0);
            std::memcpy(b, in + off, n);
            std::memset(b + n, (int)(16 - n), 16 - n);  // PKCS#7 (only reached for the last block)
            if (!ecb)
                for (int i = 0; i < 16; i++) b[i] ^= chain[i];
            aes.encrypt(b);
            std::memcpy(chain, b, 16);
            std::memcpy(out + off, b, 16);
        }
        if (moved) *moved = total;
        return kCCSuccess;
    }
    if (inlen % 16) return kCCAlignmentError;
    std::vector<u8> plain(inlen);
    for (u64 off = 0; off < inlen; off += 16) {
        u8 b[16];
        std::memcpy(b, in + off, 16);
        aes.decrypt(b);
        if (!ecb)
            for (int i = 0; i < 16; i++) b[i] ^= chain[i];
        std::memcpy(chain, in + off, 16);
        std::memcpy(plain.data() + off, b, 16);
    }
    u64 n = inlen;
    if (pkcs7) {
        u8 pad = inlen ? plain[inlen - 1] : 0;
        if (pad < 1 || pad > 16 || pad > inlen) return kCCDecodeError;
        for (u64 i = inlen - pad; i < inlen; i++)
            if (plain[i] != pad) return kCCDecodeError;
        n -= pad;
    }
    if (outavail < n) {
        if (moved) *moved = n;
        return kCCBufferTooSmall;
    }
    std::memcpy(out, plain.data(), n);
    if (moved) *moved = n;
    return kCCSuccess;
}

}  // namespace

void install_crypto() {
    using hle::fn;
    fn("_CC_MD5", [](const u8* d, u32 len, u8* md) {
        md5(d, len, md);
        return md;
    });
    fn("_CC_SHA1", [](const u8* d, u32 len, u8* md) {
        sha1(d, len, md);
        return md;
    });
    fn("_CCHmac", [](u32 alg, const u8* key, u64 keylen, const u8* data, u64 len, u8* out) {
        switch (alg) {
        case 0: hmac(sha1, 20, key, keylen, data, len, out); break;
        case 1: hmac(md5, 16, key, keylen, data, len, out); break;
        case 2: hmac(sha256, 32, key, keylen, data, len, out); break;
        default: LOG_WARN("CCHmac: algorithm %u not implemented", alg); break;
        }
    });
    fn("_CCCrypt", cc_crypt);
    static u64 s_sec_random_default = 0;
    hle::data("_kSecRandomDefault", gaddr(&s_sec_random_default));
    fn("_SecRandomCopyBytes", [](u64, u64 n, u8* out) {
        for (u64 done = 0; done < n;) {
            ssize_t r = getrandom(out + done, n - done, 0);
            if (r <= 0) return -1;
            done += (u64)r;
        }
        return 0;
    });
    fn("_SecKeyRawVerify", [](u64, u32, const void*, u64, const void*, u64) { return 0; });
}

}  // namespace libc
