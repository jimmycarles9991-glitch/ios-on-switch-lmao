// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AppleKeyStore stand-in's key material and key wrapping, free of kernel headers so keywrap_test checks it against the RFC
// vectors on the host. The block cipher and the hash come in as callbacks: the kernel passes libkern's corecrypto AES-256 and
// SHA-256 (AKSDeviceKey.cpp), the host test CommonCrypto.
//
//   wrap   RFC 3394 AES key wrap (6n rounds, default IV A6A6A6A6A6A6A6A6): n >= 2 64-bit blocks in, n + 1 out. The same 8-byte
//          overhead as the real AppleKeyStore's class-key wraps, so callers' buffers (securityd allocates length + 40, MobileKeyBag
//          length + 8) always fit, and unwrap detects a wrong key, class or tampered blob through the IV.
//   kdf    HMAC-SHA256(device key, label || be32 keybag || be32 class): one 256-bit KEK per keybag and protection class.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace aks {

// The AppleKeyStore answers the callers tell apart (securityd 21H461 ks_crypt at 0x100014aa8: e00002bc and the e007xxxx range ->
// errSecDecode "can't be decrypted on this device, ever, so drop the item"; e00002c1/e00002e2 -> errSecInteractionNotAllowed;
// e00002c2 -> errSecParam; anything else -> errSecNotAvailable. MobileKeyBag convertAKSErrorToMKB maps c1 c2 c9 ce d8 e2 f0 and
// logs every other code as "Unexpected AppleKeyStore error").
constexpr uint32_t kReturnError = 0xe00002bc;        // a blob that does not unwrap under this device's class key
constexpr uint32_t kReturnBadArgument = 0xe00002c2;  // malformed request: counts, lengths, class
constexpr uint32_t kReturnUnsupported = 0xe00002c7;  // a selector this stand-in does not implement
constexpr uint32_t kReturnNotReady = 0xe00002d8;     // corecrypto not registered yet
constexpr uint32_t kReturnNoSpace = 0xe00002db;      // the caller's output buffer is too small
constexpr uint32_t kReturnNotFound = 0xe00002f0;  // no such keybag handle, or no such record (configuration, memento)

struct Primitives {
    void *cipherContext;  // AES-256 already keyed with the KEK for both directions
    void (*encryptBlock)(void *context, const uint8_t in[16], uint8_t out[16]);
    void (*decryptBlock)(void *context, const uint8_t in[16], uint8_t out[16]);
};

using Sha256Parts = void (*)(const void *const *parts, const size_t *lengths, size_t count, uint8_t digest[32]);

constexpr size_t kBlock = 8;
constexpr size_t kMinKey = 16,
                 kMaxKey = 512;  // what callers wrap: 32-byte item and metadata keys, 32/64-byte file keys
constexpr uint8_t kDefaultIV[8] = {0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6};

inline void wipe(void *p, size_t n)
{
    volatile uint8_t *b = static_cast<volatile uint8_t *>(p);
    while (n--)
        *b++ = 0;
}

inline bool wrapLengthValid(size_t keyLength)
{
    return keyLength >= kMinKey && keyLength <= kMaxKey && keyLength % kBlock == 0;
}

inline bool unwrappedLengthValid(size_t wrappedLength)
{
    return wrappedLength >= kMinKey + kBlock && wrappedLength <= kMaxKey + kBlock && wrappedLength % kBlock == 0;
}

// RFC 3394 2.2.1. `out` holds keyLength + 8 bytes; returns false only for a length the caller should have rejected.
inline bool wrap(const Primitives &p, const uint8_t *key, size_t keyLength, uint8_t *out)
{
    if (!wrapLengthValid(keyLength))
        return false;
    const size_t n = keyLength / kBlock;
    uint8_t a[8];
    uint8_t b[16];
    for (size_t i = 0; i < 8; ++i)
        a[i] = kDefaultIV[i];
    for (size_t i = 0; i < keyLength; ++i)
        out[kBlock + i] = key[i];
    for (uint64_t j = 0; j < 6; ++j) {
        for (size_t i = 1; i <= n; ++i) {
            uint8_t *r = out + kBlock * i;
            for (size_t k = 0; k < 8; ++k) {
                b[k] = a[k];
                b[8 + k] = r[k];
            }
            p.encryptBlock(p.cipherContext, b, b);
            const uint64_t t = n * j + i;
            for (size_t k = 0; k < 8; ++k)
                a[k] = b[k] ^ static_cast<uint8_t>(t >> (56 - 8 * k));
            for (size_t k = 0; k < 8; ++k)
                r[k] = b[8 + k];
        }
    }
    for (size_t k = 0; k < 8; ++k)
        out[k] = a[k];
    wipe(b, sizeof b);
    return true;
}

// RFC 3394 2.2.2 with the 2.2.3 integrity check. `out` holds wrappedLength - 8 bytes and is wiped when the check fails.
inline bool unwrap(const Primitives &p, const uint8_t *wrapped, size_t wrappedLength, uint8_t *out)
{
    if (!unwrappedLengthValid(wrappedLength))
        return false;
    const size_t n = wrappedLength / kBlock - 1;
    uint8_t a[8];
    uint8_t b[16];
    for (size_t k = 0; k < 8; ++k)
        a[k] = wrapped[k];
    for (size_t i = 0; i < n * kBlock; ++i)
        out[i] = wrapped[kBlock + i];
    for (uint64_t j = 6; j-- > 0;) {
        for (size_t i = n; i >= 1; --i) {
            uint8_t *r = out + kBlock * (i - 1);
            const uint64_t t = n * j + i;
            for (size_t k = 0; k < 8; ++k) {
                b[k] = a[k] ^ static_cast<uint8_t>(t >> (56 - 8 * k));
                b[8 + k] = r[k];
            }
            p.decryptBlock(p.cipherContext, b, b);
            for (size_t k = 0; k < 8; ++k) {
                a[k] = b[k];
                r[k] = b[8 + k];
            }
        }
    }
    uint8_t diff = 0;
    for (size_t k = 0; k < 8; ++k)
        diff |= static_cast<uint8_t>(a[k] ^ kDefaultIV[k]);
    wipe(b, sizeof b);
    if (diff) {
        wipe(out, n * kBlock);
        return false;
    }
    return true;
}

// RFC 2104 HMAC over SHA-256 with a key of at most one block (64 bytes).
inline void hmacSha256(Sha256Parts sha256, const uint8_t *key, size_t keyLength, const void *const *parts,
                       const size_t *lengths, size_t count, uint8_t mac[32])
{
    uint8_t pad[64];
    uint8_t inner[32];
    for (size_t i = 0; i < 64; ++i)
        pad[i] = static_cast<uint8_t>((i < keyLength ? key[i] : 0u) ^ 0x36u);
    const void *innerParts[1 + 8];
    size_t innerLengths[1 + 8];
    innerParts[0] = pad;
    innerLengths[0] = sizeof pad;
    for (size_t i = 0; i < count && i < 8; ++i) {
        innerParts[1 + i] = parts[i];
        innerLengths[1 + i] = lengths[i];
    }
    sha256(innerParts, innerLengths, 1 + (count < 8 ? count : 8), inner);
    for (size_t i = 0; i < 64; ++i)
        pad[i] = static_cast<uint8_t>((i < keyLength ? key[i] : 0u) ^ 0x5cu);
    const void *outerParts[2] = {pad, inner};
    const size_t outerLengths[2] = {sizeof pad, sizeof inner};
    sha256(outerParts, outerLengths, 2, mac);
    wipe(pad, sizeof pad);
    wipe(inner, sizeof inner);
}

// One KEK per (keybag, class) under the device key.
inline void classKey(Sha256Parts sha256, const uint8_t deviceKey[32], uint32_t keybag, uint32_t keyClass,
                     uint8_t kek[32])
{
    static const char label[] = "Switch AppleKeyStore class key v2";
    const uint8_t ids[8] = {static_cast<uint8_t>(keybag >> 24),   static_cast<uint8_t>(keybag >> 16),
                            static_cast<uint8_t>(keybag >> 8),    static_cast<uint8_t>(keybag),
                            static_cast<uint8_t>(keyClass >> 24), static_cast<uint8_t>(keyClass >> 16),
                            static_cast<uint8_t>(keyClass >> 8),  static_cast<uint8_t>(keyClass)};
    const void *parts[2] = {label, ids};
    const size_t lengths[2] = {sizeof label - 1, sizeof ids};
    hmacSha256(sha256, deviceKey, 32, parts, lengths, 2, kek);
}

}  // namespace aks
