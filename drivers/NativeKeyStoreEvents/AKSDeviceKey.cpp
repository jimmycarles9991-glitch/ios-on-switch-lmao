// SPDX-License-Identifier: BSD-2-Clause-Patent
// The AppleKeyStore stand-in's device key and the key operations built on it (NativeKeyStoreEvents.cpp dispatches the selectors).
//
// Device key: SHA-256 over a label and the seven fuse words that make up the Tegra chip's unique ID: vendor, fab, lot code 0 and 1,
// wafer, X and Y coordinate (FUSE base 0x7000F800 + 0x200..0x218: hekate bdk/soc/fuse.h:157-163, the same block Linux
// fuse-tegra30.c reads as FUSE_BEGIN 0x100 + 0x100..0x118 for its randomness, and switchroot U-Boot's lot0 at +0x208 in
// board/nintendo/switch/nintendo-switch.c:111-120). The fuse registers answer only while the FUSE clock runs and the controller is
// out of reset (CLK_OUT_ENB_H 0x14 / RST_DEVICES_H 0x8 bit CLK_H_FUSE = 7: hekate bdk/soc/clock.h:26,29,510) and while CAR
// MISC_CLK_ENB (0x48, clock.h:36) bit 28 makes them visible (bdk/soc/clock.c:199-203); the clock state is checked and left alone,
// the visibility bit restored after the read.
// The same chip always yields the same key, so a key wrapped in one boot unwraps in every later one, and only on this console.
//
// What it is not: the chip ID is not secret (any kernel code can read it, and it is exposed elsewhere), so this keeps the keychain
// working and ties it to the console. It is no SEP: anyone who has this console's fuse words can unwrap its keychain offline.
//
// Any failure to read a plausible chip ID panics: a wrong device key would make securityd drop every keychain item it cannot
// unwrap ("can't be decrypted on this device, ever"), which is worse than not booting.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/crypto/aes.h>
#include <libkern/crypto/sha2.h>
#include "AKSKeyWrap.h"

extern "C" void proc_selfname(char *buf, int size);
#if DEVELOPMENT || DEBUG
extern "C" void native_aks_se_probe(void);
#endif
// libkern/crypto/crypto_internal.h's crypto_functions_t, set when the corecrypto kext registers (APFS depends on it, so before
// userland). Only its presence is read here; that header's corecrypto includes do not compile as C++ (-Wextern-c-compat).
extern "C" const void *g_crypto_funcs;

namespace {
constexpr uint64_t kCarPA = 0x60006000, kFusePagePA = 0x7000F000;
constexpr uint32_t kCarRstDevicesH = 0x008, kCarClkOutEnbH = 0x014, kCarMiscClkEnb = 0x048;
constexpr uint32_t kFuseClockBit = 1u << 7;  // CLK_H_FUSE (hekate bdk/soc/clock.h:510)
constexpr uint32_t kFuseVisible = 1u << 28;  // MISC_CLK_ENB CFG_ALL_VISIBLE
constexpr uint32_t kFuseBase = 0x800;        // 0x7000F800 within the mapped page
constexpr uint32_t kFuseUid[] = {0x200, 0x204, 0x208, 0x20C, 0x210, 0x214, 0x218};

uint8_t gDeviceKey[32];
volatile bool gDeviceKeyReady;
volatile uint64_t gUnsupportedSeen[4];

void sha256Parts(const void *const *parts, const size_t *lengths, size_t count, uint8_t digest[32])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    for (size_t i = 0; i < count; ++i)
        SHA256_Update(&c, parts[i], lengths[i]);
    SHA256_Final(digest, &c);
}

struct Kek {
    aes_encrypt_ctx encrypt;
    aes_decrypt_ctx decrypt;
};

void encryptBlock(void *context, const uint8_t in[16], uint8_t out[16])
{
    uint8_t block[16];
    memcpy(block, in, sizeof block);
    aes_encrypt(block, out, &static_cast<Kek *>(context)->encrypt);
    aks::wipe(block, sizeof block);
}

void decryptBlock(void *context, const uint8_t in[16], uint8_t out[16])
{
    uint8_t block[16];
    memcpy(block, in, sizeof block);
    aes_decrypt(block, out, &static_cast<Kek *>(context)->decrypt);
    aks::wipe(block, sizeof block);
}

volatile uint32_t *mapRegisters(uint64_t physical, IOMemoryMap **map)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(physical, 0x1000);
    *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(memory);
    return *map ? reinterpret_cast<volatile uint32_t *>((*map)->getVirtualAddress()) : nullptr;
}

// The class KEK for (keybag, class) keyed into both AES directions; false while corecrypto is not registered.
bool keyKek(uint32_t keybag, uint32_t keyClass, Kek *kek)
{
    if (!g_crypto_funcs)
        return false;
    uint8_t key[32];
    aks::classKey(sha256Parts, gDeviceKey, keybag, keyClass, key);
    aes_encrypt_key256(key, &kek->encrypt);
    aes_decrypt_key256(key, &kek->decrypt);
    aks::wipe(key, sizeof key);
    return true;
}
}  // namespace

// A per-console value for identifiers that must stay the same across boots without revealing the device key (an HMAC of
// `label` under it); false until AppleKeyStore has started.
extern "C" bool native_aks_device_digest(const char *label, uint8_t out[32])
{
    if (!gDeviceKeyReady)
        return false;
    const void *parts[1] = {label};
    const size_t lengths[1] = {strlen(label)};
    aks::hmacSha256(sha256Parts, gDeviceKey, 32, parts, lengths, 1, out);
    return true;
}

// From AppleKeyStore::start, once, before the service is registered and any user client exists.
extern "C" void native_aks_device_key_start(void)
{
    if (gDeviceKeyReady)
        return;
    IOMemoryMap *carMap = nullptr, *fuseMap = nullptr;
    volatile uint32_t *car = mapRegisters(kCarPA, &carMap);
    volatile uint32_t *fuse = mapRegisters(kFusePagePA, &fuseMap);
    if (!car || !fuse)
        panic("AppleKeyStore: cannot map CAR/FUSE for the device key");
    const uint32_t clocks = car[kCarClkOutEnbH / 4], resets = car[kCarRstDevicesH / 4];
    if (!(clocks & kFuseClockBit) || (resets & kFuseClockBit)) {
        panic("AppleKeyStore: FUSE clock off or in reset (CLK_OUT_ENB_H %#x RST_DEVICES_H %#x), no device key", clocks,
              resets);
    }
    const uint32_t misc = car[kCarMiscClkEnb / 4];
    if (!(misc & kFuseVisible)) {
        car[kCarMiscClkEnb / 4] = misc | kFuseVisible;
        (void)car[kCarMiscClkEnb / 4];
    }
    uint32_t uid[sizeof kFuseUid / sizeof kFuseUid[0]];
    for (size_t i = 0; i < sizeof uid / sizeof uid[0]; ++i)
        uid[i] = fuse[(kFuseBase + kFuseUid[i]) / 4];
    if (!(misc & kFuseVisible))
        car[kCarMiscClkEnb / 4] = misc;
    OSSafeReleaseNULL(fuseMap);
    OSSafeReleaseNULL(carMap);
    // lot code 0 (uid[2]) is what U-Boot already builds MACs from; zero or all ones means the fuses did not answer.
    if (!uid[2] || uid[2] == ~0u || (!uid[5] && !uid[6])) {
        panic("AppleKeyStore: implausible fuse chip ID (lot0 %#x x %#x y %#x), no device key", uid[2], uid[5], uid[6]);
    }
    static const char label[] = "Switch AppleKeyStore device key v2";
    const void *parts[2] = {label, uid};
    const size_t lengths[2] = {sizeof label - 1, sizeof uid};
    sha256Parts(parts, lengths, 2, gDeviceKey);
    aks::wipe(uid, sizeof uid);
#if DEVELOPMENT || DEBUG
    // A non-secret identifier of the key (not the key, not the chip ID) so two boots can be compared on the console.
    static const char idLabel[] = "Switch AppleKeyStore key id";
    uint8_t id[32];
    const void *idParts[1] = {idLabel};
    const size_t idLengths[1] = {sizeof idLabel - 1};
    aks::hmacSha256(sha256Parts, gDeviceKey, 32, idParts, idLengths, 1, id);
    IOLog("AppleKeyStore: device key from the fuse chip ID, key id %02x%02x%02x%02x\n", id[0], id[1], id[2], id[3]);
#endif
    gDeviceKeyReady = true;
#if DEVELOPMENT || DEBUG
    // Read-only: whether the Security Engine's per-console slot is usable from here (TegraSecurityEngine.cpp).
    native_aks_se_probe();
#endif
}

// aks_wrap_key (selector 10) after the dispatcher checked counts: `key` is the plaintext, `out` the caller's buffer.
extern "C" IOReturn native_aks_wrap(uint32_t keybag, uint32_t keyClass, const uint8_t *key, size_t length, uint8_t *out,
                                    size_t capacity, size_t *written)
{
    if (!aks::wrapLengthValid(length))
        return (IOReturn)aks::kReturnBadArgument;
    if (capacity < length + aks::kBlock)
        return (IOReturn)aks::kReturnNoSpace;
    if (!gDeviceKeyReady)
        return (IOReturn)aks::kReturnNotReady;
    Kek kek;
    if (!keyKek(keybag, keyClass, &kek))
        return (IOReturn)aks::kReturnNotReady;
    const aks::Primitives p{&kek, encryptBlock, decryptBlock};
    const bool ok = aks::wrap(p, key, length, out);
    aks::wipe(&kek, sizeof kek);
    if (!ok)
        return (IOReturn)aks::kReturnBadArgument;
    *written = length + aks::kBlock;
    return kIOReturnSuccess;
}

// aks_unwrap_key (selector 11): a blob that fails RFC 3394's integrity check answers kReturnError, never the bytes.
extern "C" IOReturn native_aks_unwrap(uint32_t keybag, uint32_t keyClass, const uint8_t *wrapped, size_t length,
                                      uint8_t *out, size_t capacity, size_t *written)
{
    if (!aks::unwrappedLengthValid(length))
        return (IOReturn)aks::kReturnBadArgument;
    if (capacity < length - aks::kBlock)
        return (IOReturn)aks::kReturnNoSpace;
    if (!gDeviceKeyReady)
        return (IOReturn)aks::kReturnNotReady;
    Kek kek;
    if (!keyKek(keybag, keyClass, &kek))
        return (IOReturn)aks::kReturnNotReady;
    const aks::Primitives p{&kek, encryptBlock, decryptBlock};
    const bool ok = aks::unwrap(p, wrapped, length, out);
    aks::wipe(&kek, sizeof kek);
    if (!ok) {
        // Keys wrapped under the v1 labels' device key (an install from before them) never unwrap: say so once.
        static volatile UInt32 sReported;
        if (OSCompareAndSwap(0, 1, &sReported))
            IOLog("AppleKeyStore: a wrapped key does not unwrap under this device key (data from an older install?); "
                  "refused, as every such key is: reinstall to start fresh\n");
        return (IOReturn)aks::kReturnError;
    }
    *written = length - aks::kBlock;
    return kIOReturnSuccess;
}

// aks_get_bag_uuid (selector 23): a stable UUID per keybag under this device key (RFC 9562 version 8, custom).
extern "C" IOReturn native_aks_bag_uuid(uint32_t keybag, uint8_t uuid[16])
{
    if (!gDeviceKeyReady)
        return (IOReturn)aks::kReturnNotReady;
    static const char label[] = "Switch AppleKeyStore bag uuid v2";
    const uint8_t id[4] = {static_cast<uint8_t>(keybag >> 24), static_cast<uint8_t>(keybag >> 16),
                           static_cast<uint8_t>(keybag >> 8), static_cast<uint8_t>(keybag)};
    const void *parts[2] = {label, id};
    const size_t lengths[2] = {sizeof label - 1, sizeof id};
    uint8_t mac[32];
    aks::hmacSha256(sha256Parts, gDeviceKey, 32, parts, lengths, 2, mac);
    memcpy(uuid, mac, 16);
    uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0f) | 0x80);
    uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3f) | 0x80);
    return kIOReturnSuccess;
}

// One console line the first time each unimplemented selector is called, naming the caller, so the next selector the system
// needs shows on the console, not only as an e00002c7 in some daemon's log.
extern "C" void native_aks_unsupported(uint32_t selector)
{
    const uint32_t word = selector < 256 ? selector / 64 : 3, bit = selector < 256 ? selector % 64 : 63;
    const uint64_t mask = 1ull << bit;
    if (__atomic_fetch_or(&gUnsupportedSeen[word], mask, __ATOMIC_RELAXED) & mask)
        return;
    char name[17] = {};
    proc_selfname(name, sizeof name);
    IOLog("AppleKeyStore: selector %u unsupported (first from %s)\n", selector, name);
}
