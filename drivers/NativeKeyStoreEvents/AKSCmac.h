// SPDX-License-Identifier: BSD-2-Clause-Patent
// AES-CMAC (RFC 4493, NIST SP 800-38B) over a one-block encrypt callback, free of kernel headers so keywrap_test checks it
// against the RFC vectors on the host. The kernel passes one Security Engine ECB block per call (TegraSecurityEngine.cpp), so a
// keyslot that can be used but never read gives a MAC without the key leaving the engine; only the chaining runs here.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "AKSKeyWrap.h"

namespace aks {

// One AES block encryption under the caller's key; false when the cipher failed (the MAC is then refused).
using BlockEncrypt = bool (*)(void *context, const uint8_t in[16], uint8_t out[16]);

// RFC 4493 2.3: multiply by x in GF(2^128).
inline void cmacDouble(const uint8_t in[16], uint8_t out[16])
{
    const uint8_t carry = in[0] >> 7;
    for (size_t i = 0; i < 15; ++i) {
        const unsigned byte = in[i];
        const unsigned next = in[i + 1];
        out[i] = static_cast<uint8_t>((byte << 1) | (next >> 7));
    }
    const unsigned last = in[15];
    out[15] = static_cast<uint8_t>((last << 1) ^ (carry ? 0x87u : 0u));
}

// RFC 4493 2.4. `mac` is zeroed when the cipher fails. Subkeys and chaining values are wiped before returning.
inline bool cmac(BlockEncrypt encrypt, void *context, const uint8_t *message, size_t length, uint8_t mac[16])
{
    static const uint8_t zero[16] = {};
    uint8_t l[16];
    uint8_t k1[16];
    uint8_t k2[16];
    uint8_t x[16] = {};
    uint8_t block[16];
    bool ok = encrypt(context, zero, l);
    if (ok) {
        cmacDouble(l, k1);
        cmacDouble(k1, k2);
        const size_t blocks = length ? (length + 15) / 16 : 1;
        const bool complete = (length != 0u) && length % 16 == 0;
        for (size_t i = 0; ok && i + 1 < blocks; ++i) {
            for (size_t k = 0; k < 16; ++k)
                block[k] = static_cast<uint8_t>(x[k] ^ message[16 * i + k]);
            ok = encrypt(context, block, x);
        }
        if (ok) {
            const size_t tail = length - 16 * (blocks - 1);
            for (size_t k = 0; k < 16; ++k) {
                unsigned m = 0;
                if (k < tail) {
                    m = message[16 * (blocks - 1) + k];
                } else if (k == tail) {
                    m = 0x80;
                }
                const unsigned subkey = complete ? k1[k] : k2[k];
                block[k] = static_cast<uint8_t>(x[k] ^ m ^ subkey);
            }
            ok = encrypt(context, block, mac);
        }
    }
    if (!ok)
        wipe(mac, 16);
    wipe(l, sizeof l);
    wipe(k1, sizeof k1);
    wipe(k2, sizeof k2);
    wipe(x, sizeof x);
    wipe(block, sizeof block);
    return ok;
}

}  // namespace aks
