// SPDX-License-Identifier: BSD-2-Clause-Patent
// The cryptography a WPA2-Personal supplicant needs (IEEE 802.11-2016 12.7): SHA-1, HMAC-SHA1, the passphrase to
// PMK mapping (PBKDF2-HMAC-SHA1, 4096 iterations), the 802.11 PRF, AES-128 decryption and the RFC 3394 key unwrap
// that protects the group key. Ported from MyEcoria/nxk user/wlan/src/crypto.rs (MIT OR Apache-2.0). Plain C++
// with no kernel dependency, so the same file runs the known-answer tests on the host (scratchpad hosttest).
#ifndef SWITCH_WLAN_CRYPTO_H
#define SWITCH_WLAN_CRYPTO_H

#include <stddef.h>
#include <stdint.h>

namespace swlan::crypto {

struct Sha1 {
    uint32_t h[5];
    uint8_t buffer[64];
    size_t used;
    uint64_t length;

    void init();
    void update(const uint8_t *data, size_t n);
    void finish(uint8_t out[20]);
};

// HMAC-SHA1 with the key's inner and outer states kept, so PBKDF2's 8192 MACs do not re-hash the key.
struct Hmac {
    Sha1 inner, outer;
    void init(const uint8_t *key, size_t keyLength);
    // MAC of the concatenation of `count` parts.
    void mac(const uint8_t *const *parts, const size_t *lengths, size_t count, uint8_t out[20]) const;
};

void hmacSha1(const uint8_t *key, size_t keyLength, const uint8_t *const *parts, const size_t *lengths, size_t count,
              uint8_t out[20]);
// PMK = PBKDF2(passphrase, ssid, 4096, 256 bits).
void passphrasePmk(const uint8_t *passphrase, size_t passphraseLength, const uint8_t *ssid, size_t ssidLength,
                   uint8_t pmk[32]);
// The PMK given as 64 hexadecimal digits; false for anything else.
bool pmkFromHex(const uint8_t *text, size_t length, uint8_t pmk[32]);
// PRF-n (802.11 12.7.1.2): HMAC-SHA1(key, label || 0 || data || i) blocks.
void prf(const uint8_t *key, size_t keyLength, const char *label, const uint8_t *data, size_t dataLength, uint8_t *out,
         size_t outLength);
// RFC 3394 unwrap with a 128-bit KEK: false if the integrity check fails or the sizes are wrong.
bool keyUnwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t wrappedLength, uint8_t *out);
// The same in constant time over equal lengths.
bool same(const uint8_t *a, const uint8_t *b, size_t n);

struct Aes128 {
    uint8_t roundKeys[11][16];
    void init(const uint8_t key[16]);
    void encrypt(uint8_t block[16]) const;  // used by the host tests' wrap
    void decrypt(uint8_t block[16]) const;
};

}  // namespace swlan::crypto

#endif
