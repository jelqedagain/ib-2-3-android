// CommonCrypto (CC_MD5/CC_SHA1/CCHmac/CCCrypt) on Windows CNG.
#include "hle.h"
#include "modules.h"
#include <windows.h>
#include <bcrypt.h>

namespace libc {

namespace {

bool hash(const wchar_t* alg, bool hmac, const void* key, u64 keylen, const void* data, u64 len, u8* out, u32 outlen) {
    BCRYPT_ALG_HANDLE a = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, alg, nullptr, hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0) != 0) return false;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptCreateHash(a, &h, nullptr, 0, (PUCHAR)key, (ULONG)keylen, 0) == 0 &&
              BCryptHashData(h, (PUCHAR)data, (ULONG)len, 0) == 0 && BCryptFinishHash(h, out, outlen, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(a, 0);
    return ok;
}

const wchar_t* hmac_alg(u32 alg, u32* outlen) {
    switch (alg) {
    case 0: *outlen = 20; return BCRYPT_SHA1_ALGORITHM;
    case 1: *outlen = 16; return BCRYPT_MD5_ALGORITHM;
    case 2: *outlen = 32; return BCRYPT_SHA256_ALGORITHM;
    case 3: *outlen = 48; return BCRYPT_SHA384_ALGORITHM;
    case 4: *outlen = 64; return BCRYPT_SHA512_ALGORITHM;
    default: *outlen = 0; return nullptr;
    }
}

constexpr s32 kCCSuccess = 0, kCCParamError = -4300, kCCBufferTooSmall = -4301, kCCUnimplemented = -4305;

s32 cc_crypt(u32 op, u32 alg, u32 options, const u8* key, u64 keylen, const u8* iv, const u8* in, u64 inlen, u8* out,
             u64 outavail, u64* moved) {
    if (alg != 0) {  // only kCCAlgorithmAES128
        LOG_WARN("CCCrypt: algorithm %u not implemented", alg);
        return kCCUnimplemented;
    }
    bool pkcs7 = options & 1, ecb = options & 2;
    BCRYPT_ALG_HANDLE a = nullptr;
    if (BCryptOpenAlgorithmProvider(&a, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return kCCParamError;
    const wchar_t* mode = ecb ? BCRYPT_CHAIN_MODE_ECB : BCRYPT_CHAIN_MODE_CBC;
    BCryptSetProperty(a, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)(wcslen(mode) + 1) * 2, 0);
    BCRYPT_KEY_HANDLE k = nullptr;
    s32 rc = kCCParamError;
    if (BCryptGenerateSymmetricKey(a, &k, nullptr, 0, (PUCHAR)key, (ULONG)keylen, 0) == 0) {
        u8 ivbuf[16] = {};
        if (iv && !ecb) std::memcpy(ivbuf, iv, 16);
        ULONG n = 0;
        ULONG flags = pkcs7 ? BCRYPT_BLOCK_PADDING : 0;
        NTSTATUS st = op == 0 ? BCryptEncrypt(k, (PUCHAR)in, (ULONG)inlen, nullptr, ecb ? nullptr : ivbuf, ecb ? 0 : 16, out,
                                              (ULONG)outavail, &n, flags)
                              : BCryptDecrypt(k, (PUCHAR)in, (ULONG)inlen, nullptr, ecb ? nullptr : ivbuf, ecb ? 0 : 16, out,
                                              (ULONG)outavail, &n, flags);
        if (moved) *moved = n;
        rc = st == 0 ? kCCSuccess : (st == (NTSTATUS)0xC0000023 ? kCCBufferTooSmall : kCCParamError);
        BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(a, 0);
    return rc;
}

}  // namespace

void install_crypto() {
    using hle::fn;
    fn("_CC_MD5", [](const void* d, u32 len, u8* md) {
        hash(BCRYPT_MD5_ALGORITHM, false, nullptr, 0, d, len, md, 16);
        return md;
    });
    fn("_CC_SHA1", [](const void* d, u32 len, u8* md) {
        hash(BCRYPT_SHA1_ALGORITHM, false, nullptr, 0, d, len, md, 20);
        return md;
    });
    fn("_CCHmac", [](u32 alg, const void* key, u64 keylen, const void* data, u64 len, u8* out) {
        u32 outlen;
        const wchar_t* a = hmac_alg(alg, &outlen);
        if (a) hash(a, true, key, keylen, data, len, out, outlen);
    });
    fn("_CCCrypt", cc_crypt);
    static u64 s_sec_random_default = 0;
    hle::data("_kSecRandomDefault", gaddr(&s_sec_random_default));
    fn("_SecRandomCopyBytes", [](u64, u64 n, u8* out) {
        BCryptGenRandom(nullptr, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        return 0;
    });
    fn("_SecKeyRawVerify", [](u64, u32, const void*, u64, const void*, u64) { return 0; });
}

}  // namespace libc
