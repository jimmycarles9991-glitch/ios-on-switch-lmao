// SPDX-License-Identifier: BSD-2-Clause-Patent
// See SwitchWLANCrypto.h. Ported from MyEcoria/nxk user/wlan/src/crypto.rs (MIT OR Apache-2.0); the AES tables are
// computed at compile time from the field arithmetic as nxk does, so no table is typed in by hand.
#include <string.h>
#include "SwitchWLANCrypto.h"

namespace swlan::crypto {
namespace {

inline uint32_t rotl(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32 - n));
}

void compress(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<uint32_t>(block[static_cast<ptrdiff_t>(4) * i]) << 24) |
               (static_cast<uint32_t>(block[4 * i + 1]) << 16) | (static_cast<uint32_t>(block[4 * i + 2]) << 8) |
               block[4 * i + 3];
    }
    for (int i = 16; i < 80; ++i)
        w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0];
    uint32_t b = h[1];
    uint32_t c = h[2];
    uint32_t d = h[3];
    uint32_t e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32_t t = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

constexpr uint8_t xtime(uint8_t x)
{
    return static_cast<uint8_t>((static_cast<unsigned>(x) << 1) ^ ((x & 0x80) ? 0x1Bu : 0u));
}

uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b) {
        if (b & 1)
            p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

constexpr uint8_t rotl8(uint8_t x, unsigned n)
{
    const unsigned v = x;
    return static_cast<uint8_t>((v << n) | (v >> (8 - n)));
}

// S-box and its inverse from the multiplicative inverse in GF(2^8) and the affine map (FIPS-197 5.1.1).
struct Sboxes {
    uint8_t forward[256], inverse[256];

    constexpr Sboxes() : forward(), inverse()
    {
        uint8_t exp[256] = {};
        uint8_t log[256] = {};
        uint8_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = x;
            log[x] = static_cast<uint8_t>(i);
            x = static_cast<uint8_t>(x ^ xtime(x));
        }
        for (int v = 0; v < 256; ++v) {
            const uint8_t inv = v ? exp[(255 - log[v]) % 255] : 0;
            const uint8_t s = static_cast<uint8_t>(static_cast<unsigned>(inv) ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^
                                                   rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
            forward[v] = s;
            inverse[s] = static_cast<uint8_t>(v);
        }
    }
};

constexpr Sboxes kSbox;

void addRoundKey(uint8_t b[16], const uint8_t k[16])
{
    for (int i = 0; i < 16; ++i)
        b[i] ^= k[i];
}

void shiftRows(uint8_t b[16], bool inverse)
{
    uint8_t s[16];
    memcpy(s, b, 16);
    for (unsigned r = 1; r < 4; ++r) {
        for (unsigned c = 0; c < 4; ++c)
            b[r + 4 * c] = s[r + 4 * (inverse ? (c + 4 - r) % 4 : (c + r) % 4)];
    }
}

void mixColumns(uint8_t b[16], const uint8_t m[4])
{
    for (unsigned c = 0; c < 4; ++c) {
        uint8_t a[4];
        memcpy(a, b + static_cast<size_t>(4) * c, 4);
        for (unsigned r = 0; r < 4; ++r) {
            uint8_t acc = 0;
            for (unsigned k = 0; k < 4; ++k)
                acc ^= gmul(m[(k + 4 - r) % 4], a[k]);
            b[4 * c + r] = acc;
        }
    }
}

}  // namespace

void Sha1::init()
{
    static const uint32_t start[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(h, start, sizeof(h));
    used = 0;
    length = 0;
}

void Sha1::update(const uint8_t *data, size_t n)
{
    length += n;
    while (n) {
        const size_t take = 64 - used < n ? 64 - used : n;
        memcpy(buffer + used, data, take);
        used += take;
        data += take;
        n -= take;
        if (used == 64) {
            compress(h, buffer);
            used = 0;
        }
    }
}

void Sha1::finish(uint8_t out[20])
{
    const uint64_t bits = length * 8;
    static const uint8_t pad = 0x80;
    static const uint8_t zero = 0;
    update(&pad, 1);
    while (used != 56)
        update(&zero, 1);
    uint8_t be[8];
    for (unsigned i = 0; i < 8; ++i)
        be[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
    update(be, 8);
    for (unsigned i = 0; i < 5; ++i) {
        for (unsigned j = 0; j < 4; ++j)
            out[4 * i + j] = static_cast<uint8_t>(h[i] >> (24 - 8 * j));
    }
}

void Hmac::init(const uint8_t *key, size_t keyLength)
{
    uint8_t k[64] = {};
    if (keyLength > 64) {
        Sha1 s;
        s.init();
        s.update(key, keyLength);
        s.finish(k);
    } else {
        memcpy(k, key, keyLength);
    }
    uint8_t pad[64];
    for (int i = 0; i < 64; ++i)
        pad[i] = k[i] ^ 0x36;
    inner.init();
    inner.update(pad, 64);
    for (int i = 0; i < 64; ++i)
        pad[i] = k[i] ^ 0x5C;
    outer.init();
    outer.update(pad, 64);
    memset(k, 0, sizeof(k));
    memset(pad, 0, sizeof(pad));
}

void Hmac::mac(const uint8_t *const *parts, const size_t *lengths, size_t count, uint8_t out[20]) const
{
    Sha1 i = inner;
    Sha1 o = outer;
    for (size_t k = 0; k < count; ++k)
        i.update(parts[k], lengths[k]);
    uint8_t inside[20];
    i.finish(inside);
    o.update(inside, sizeof(inside));
    o.finish(out);
}

void hmacSha1(const uint8_t *key, size_t keyLength, const uint8_t *const *parts, const size_t *lengths, size_t count,
              uint8_t out[20])
{
    Hmac h;
    h.init(key, keyLength);
    h.mac(parts, lengths, count, out);
}

void passphrasePmk(const uint8_t *passphrase, size_t passphraseLength, const uint8_t *ssid, size_t ssidLength,
                   uint8_t pmk[32])
{
    Hmac prfKey;
    prfKey.init(passphrase, passphraseLength);
    for (uint32_t block = 1; block <= 2; ++block) {
        const uint8_t index[4] = {0, 0, 0, static_cast<uint8_t>(block)};
        const uint8_t *parts[2] = {ssid, index};
        const size_t lengths[2] = {ssidLength, 4};
        uint8_t u[20];
        uint8_t t[20];
        prfKey.mac(parts, lengths, 2, u);
        memcpy(t, u, sizeof(t));
        for (int i = 1; i < 4096; ++i) {
            const uint8_t *next[1] = {u};
            const size_t nextLength[1] = {sizeof(u)};
            prfKey.mac(next, nextLength, 1, u);
            for (int j = 0; j < 20; ++j)
                t[j] ^= u[j];
        }
        memcpy(pmk + static_cast<size_t>(20) * (block - 1), t, block == 1 ? 20 : 12);
    }
}

bool pmkFromHex(const uint8_t *text, size_t length, uint8_t pmk[32])
{
    if (length != 64)
        return false;
    const auto digit = [](uint8_t c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 32; ++i) {
        const int hi = digit(text[static_cast<ptrdiff_t>(2) * i]);
        const int lo = digit(text[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        pmk[i] = static_cast<uint8_t>(hi * 16 + lo);
    }
    return true;
}

void prf(const uint8_t *key, size_t keyLength, const char *label, const uint8_t *data, size_t dataLength, uint8_t *out,
         size_t outLength)
{
    Hmac h;
    h.init(key, keyLength);
    for (size_t i = 0; 20 * i < outLength; ++i) {
        const uint8_t zero = 0;
        const uint8_t counter = static_cast<uint8_t>(i);
        const uint8_t *parts[4] = {reinterpret_cast<const uint8_t *>(label), &zero, data, &counter};
        const size_t lengths[4] = {strlen(label), 1, dataLength, 1};
        uint8_t block[20];
        h.mac(parts, lengths, 4, block);
        const size_t take = outLength - 20 * i < 20 ? outLength - 20 * i : 20;
        memcpy(out + 20 * i, block, take);
    }
}

bool same(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; ++i)
        d |= static_cast<uint8_t>(a[i] ^ b[i]);
    return d == 0;
}

void Aes128::init(const uint8_t key[16])
{
    uint8_t w[44][4];
    memcpy(w, key, 16);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i) {
        uint8_t t[4];
        memcpy(t, w[i - 1], 4);
        if (i % 4 == 0) {
            const uint8_t r[4] = {static_cast<uint8_t>(kSbox.forward[t[1]] ^ rcon), kSbox.forward[t[2]],
                                  kSbox.forward[t[3]], kSbox.forward[t[0]]};
            memcpy(t, r, 4);
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j)
            w[i][j] = w[i - 4][j] ^ t[j];
    }
    for (int r = 0; r < 11; ++r)
        memcpy(roundKeys[r], w[static_cast<ptrdiff_t>(4) * r], 16);
}

void Aes128::encrypt(uint8_t b[16]) const
{
    static const uint8_t mix[4] = {2, 3, 1, 1};
    addRoundKey(b, roundKeys[0]);
    for (int round = 1; round <= 10; ++round) {
        for (int i = 0; i < 16; ++i)
            b[i] = kSbox.forward[b[i]];
        shiftRows(b, false);
        if (round != 10)
            mixColumns(b, mix);
        addRoundKey(b, roundKeys[round]);
    }
}

void Aes128::decrypt(uint8_t b[16]) const
{
    static const uint8_t mix[4] = {14, 11, 13, 9};
    addRoundKey(b, roundKeys[10]);
    for (int round = 9; round >= 0; --round) {
        shiftRows(b, true);
        for (int i = 0; i < 16; ++i)
            b[i] = kSbox.inverse[b[i]];
        addRoundKey(b, roundKeys[round]);
        if (round)
            mixColumns(b, mix);
    }
}

bool keyUnwrap(const uint8_t kek[16], const uint8_t *wrapped, size_t wrappedLength, uint8_t *out)
{
    if (wrappedLength % 8 || wrappedLength < 24)
        return false;
    const size_t n = wrappedLength / 8 - 1;
    Aes128 aes;
    aes.init(kek);
    uint8_t a[8];
    memcpy(a, wrapped, 8);
    memcpy(out, wrapped + 8, 8 * n);
    for (int j = 5; j >= 0; --j) {
        for (size_t i = n; i-- > 0;) {
            const uint64_t t = static_cast<uint64_t>(n) * static_cast<uint64_t>(j) + i + 1;
            uint8_t b[16];
            for (unsigned k = 0; k < 8; ++k)
                b[k] = a[k] ^ static_cast<uint8_t>(t >> (56 - 8 * k));
            memcpy(b + 8, out + 8 * i, 8);
            aes.decrypt(b);
            memcpy(a, b, 8);
            memcpy(out + 8 * i, b + 8, 8);
        }
    }
    static const uint8_t iv[8] = {0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6, 0xA6};
    return same(a, iv, 8);
}

}  // namespace swlan::crypto
