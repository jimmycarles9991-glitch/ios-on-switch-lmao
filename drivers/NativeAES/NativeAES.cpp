// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalIOAESAccelerator.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/crypto/aes.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include <libkern/crypto/crypto_internal.h>
#pragma clang diagnostic pop
#include <corecrypto/cc.h>
#include <kern/task.h>
#include <mach/vm_param.h>
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../TegraSE/TegraSE.h"

extern "C" {
bool native_aks_device_digest(const char *label, uint8_t out[32]);
void native_aks_device_key_start(void);
extern unsigned long gPhysBase;
extern unsigned long gPhysSize;
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers expose these to IOKit C++
boolean_t pmap_valid_page(ppnum_t pn);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers expose these to IOKit C++
unsigned int pmap_cache_attributes(ppnum_t pn);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers expose these to IOKit C++
vm_map_address_t phystokv_range(addr64_t pa, vm_size_t *max_len);
}

namespace {
constexpr uint64_t kRefusedKey = switch_refusal_key("NativeAES", "hardware key");
// Apple's engine holds the device-unique UID key (selector 2000) and keys derived from it, which the caller names but
// never sees (AppleImage4 entangles its nonces with 2212); below 2000 are the GID keys that decrypt Apple's firmware,
// which nothing here can stand in for.
constexpr unsigned kFirstDeviceKey = 2000;
constexpr uint32_t kDeviceKeyCacheCap = 16;

struct CachedDeviceKey {
    uint32_t selector;
    uint32_t bits;
    uint8_t bytes[32];
    aes_encrypt_ctx enc;
    aes_decrypt_ctx dec;
};

CachedDeviceKey gDeviceKeys[kDeviceKeyCacheCap];
uint32_t gDeviceKeyCount = 0;
IOLock *gDeviceKeyLock = nullptr;

IOLock *deviceKeyLock()
{
    IOLock *lock = __atomic_load_n(&gDeviceKeyLock, __ATOMIC_ACQUIRE);
    if (lock)
        return lock;
    IOLock *fresh = IOLockAlloc();
    if (!fresh)
        return nullptr;
    IOLock *expected = nullptr;
    if (__atomic_compare_exchange_n(&gDeviceKeyLock, &expected, fresh, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    IOLockFree(fresh);
    return expected;
}

CachedDeviceKey *findCachedDeviceKey(uint32_t selector, uint32_t bits)
{
    const uint32_t n = __atomic_load_n(&gDeviceKeyCount, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n; ++i) {
        if (gDeviceKeys[i].selector == selector && gDeviceKeys[i].bits == bits)
            return &gDeviceKeys[i];
    }
    return nullptr;
}

bool deriveDeviceDigest(uint32_t selector, uint8_t digest[32])
{
    native_aks_device_key_start();
    char label[48];
    snprintf(label, sizeof label, "Switch IOAESAccelerator key %u", selector);
    return native_aks_device_digest(label, digest);
}

struct PreparedKey {
    const uint8_t *raw = nullptr;
    unsigned keyBytes = 0;
    aes_encrypt_ctx *enc = nullptr;
    aes_decrypt_ctx *dec = nullptr;
    IOAESKeyData derived{};

    union {
        aes_encrypt_ctx encrypt;
        aes_decrypt_ctx decrypt;
    } local{};

    bool hasLocal = false;

    ~PreparedKey()
    {
        cc_clear(sizeof(derived), &derived);
        if (hasLocal)
            cc_clear(sizeof(local), &local);
    }

    bool ensureCpuSchedule(IOAESOperation operation)
    {
        if (operation == IOAESDecrypt) {
            if (dec)
                return true;
            if (aes_decrypt_key(raw, static_cast<int>(keyBytes), &local.decrypt) != 0)
                return false;
            dec = &local.decrypt;
            hasLocal = true;
            return true;
        }
        if (enc)
            return true;
        if (aes_encrypt_key(raw, static_cast<int>(keyBytes), &local.encrypt) != 0)
            return false;
        enc = &local.encrypt;
        hasLocal = true;
        return true;
    }
};

// The console's own key for a UID-class selector: an HMAC of the selector under the device key AppleKeyStore uses
// (drivers/NativeKeyStoreEvents/AKSDeviceKey.cpp), cached with its expanded AES schedules for the boot.
bool prepareDeviceKey(const IOAESKeyData &key, PreparedKey *prep)
{
    if (key.selector < kFirstDeviceKey)
        return false;
    if (CachedDeviceKey *hit = findCachedDeviceKey(key.selector, key.bits)) {
        prep->raw = hit->bytes;
        prep->keyBytes = key.bits / 8;
        prep->enc = &hit->enc;
        prep->dec = &hit->dec;
        return true;
    }
    IOLock *lock = deviceKeyLock();
    if (lock) {
        IOLockLock(lock);
        if (CachedDeviceKey *hit = findCachedDeviceKey(key.selector, key.bits)) {
            IOLockUnlock(lock);
            prep->raw = hit->bytes;
            prep->keyBytes = key.bits / 8;
            prep->enc = &hit->enc;
            prep->dec = &hit->dec;
            return true;
        }
        if (gDeviceKeyCount < kDeviceKeyCacheCap) {
            uint8_t digest[32];
            if (!deriveDeviceDigest(key.selector, digest)) {
                IOLockUnlock(lock);
                return false;
            }
            CachedDeviceKey &slot = gDeviceKeys[gDeviceKeyCount];
            slot.selector = key.selector;
            slot.bits = key.bits;
            memcpy(slot.bytes, digest, sizeof slot.bytes);
            cc_clear(sizeof digest, digest);
            const int keyLen = static_cast<int>(key.bits / 8);
            if (aes_encrypt_key(slot.bytes, keyLen, &slot.enc) != 0 ||
                aes_decrypt_key(slot.bytes, keyLen, &slot.dec) != 0) {
                cc_clear(sizeof slot, &slot);
                IOLockUnlock(lock);
                return false;
            }
            __atomic_store_n(&gDeviceKeyCount, gDeviceKeyCount + 1, __ATOMIC_RELEASE);
            IOLockUnlock(lock);
            prep->raw = slot.bytes;
            prep->keyBytes = key.bits / 8;
            prep->enc = &slot.enc;
            prep->dec = &slot.dec;
            return true;
        }
        IOLockUnlock(lock);
    }
    uint8_t digest[32];
    if (!deriveDeviceDigest(key.selector, digest))
        return false;
    prep->derived.bits = key.bits;
    prep->derived.selector = 0;
    memcpy(prep->derived.bytes, digest, sizeof prep->derived.bytes);
    cc_clear(sizeof digest, digest);
    prep->raw = prep->derived.bytes;
    prep->keyBytes = key.bits / 8;
    return true;
}

bool cacheableRam(uint64_t pa)
{
    constexpr unsigned kWimgDefault = 0x2;
    if (pa < gPhysBase || (pa - gPhysBase) >= gPhysSize)
        return false;
    const ppnum_t pn = static_cast<ppnum_t>(atop_64(pa));
    return pmap_valid_page(pn) != 0 && pmap_cache_attributes(pn) == kWimgDefault;
}

// No padding, and no change to the registered corecrypto modes.
int cbcChunk(const unsigned char *input, unsigned char *output, size_t bytes, unsigned char iv[16], PreparedKey &prep,
             IOAESOperation operation)
{
    // The Security Engine first (drivers/TegraSE: it updates iv the same way); the CPU if it declines.
    if (!tegra_se_aes(TEGRA_SE_CBC, operation != IOAESDecrypt, prep.raw, prep.keyBytes, iv, input, output, bytes))
        return 0;
    if (!prep.ensureCpuSchedule(operation))
        return -1;
    unsigned char nextIV[16];
    int result = 0;
    if (operation == IOAESDecrypt) {
        memcpy(nextIV, input + bytes - 16, 16);
        result = aes_decrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, prep.dec);
    } else {
        result = aes_encrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, prep.enc);
        if (!result)
            memcpy(nextIV, output + bytes - 16, 16);
    }
    if (!result)
        memcpy(iv, nextIV, 16);
    cc_clear(sizeof(nextIV), nextIV);
    return result;
}
}  // namespace

extern "C" {
volatile uint32_t native_aes_registered = 0;
volatile uint32_t native_aes_completed = 0;
volatile uint32_t native_aes_unsupported_key = 0;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class NativeAES : public IOAESAccelerator {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(NativeAES) public : bool start(IOService *) override;

    IOWorkLoop *getWorkLoop() const override
    {
        // NOLINTNEXTLINE(bugprone-parent-virtual-call): IOAESAccelerator's drives Apple's engine; IOService's is wanted
        return IOService::getWorkLoop();
    }

    IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *a, void *b, void *c, void *d) override
    {
        // NOLINTNEXTLINE(bugprone-parent-virtual-call): IOAESAccelerator's drives Apple's engine; IOService's is wanted
        return IOService::callPlatformFunction(name, wait, a, b, c, d);
    }

    const char *getAlgorithmName() override
    {
        return "AES-CBC";
    }

    bool isKeySupported(unsigned selector) override
    {
        return selector == 0 || selector >= kFirstDeviceKey;
    }

    unsigned getSupportedOptions() override
    {
        return 1;
    }

    IOReturn performAES(void *, void *, task_t, unsigned long long, IOAESIV *, IOAESOperation, IOAESKeyData *,
                        IOAESCompletion, void *) override;
    IOReturn performAES(IOMemoryDescriptor *, IOMemoryDescriptor *, unsigned long long, IOAESIV *, IOAESOperation,
                        IOAESKeyData *, unsigned long long, unsigned long long, IOAESCompletion, void *) override;
};

OSDefineMetaClassAndStructors(NativeAES, IOAESAccelerator)

    static IOReturn resolveKey(const IOAESKeyData &key, IOAESOperation operation, PreparedKey *prep)
{
    if (operation != IOAESEncrypt && operation != IOAESDecrypt) {
        __atomic_fetch_add(&native_aes_unsupported_key, 1, __ATOMIC_RELAXED);
        switch_refusal_k(switch_refusal_key_with(kRefusedKey, static_cast<uint64_t>(key.selector) << 32 |
                                                                  static_cast<uint32_t>(operation)),
                         "NativeAES", "hardware key",
                         "key selector %u, operation %d: only keys the caller passes or the console's own, encrypt or "
                         "decrypt",
                         static_cast<unsigned>(key.selector), static_cast<int>(operation));
        return kIOReturnUnsupported;
    }
    if (key.selector) {
        if (!prepareDeviceKey(key, prep)) {
            __atomic_fetch_add(&native_aes_unsupported_key, 1, __ATOMIC_RELAXED);
            switch_refusal_k(switch_refusal_key_with(kRefusedKey, static_cast<uint64_t>(key.selector) << 32 |
                                                                      static_cast<uint32_t>(operation)),
                             "NativeAES", "hardware key",
                             "key selector %u, operation %d: only keys the caller passes or the console's own, encrypt "
                             "or decrypt",
                             static_cast<unsigned>(key.selector), static_cast<int>(operation));
            return kIOReturnUnsupported;
        }
    } else {
        prep->raw = key.bytes;
        prep->keyBytes = key.bits / 8;
    }
    return kIOReturnSuccess;
}

bool NativeAES::start(IOService *provider)
{
    if (!g_crypto_funcs || !g_crypto_funcs->ccaes_cbc_encrypt || !g_crypto_funcs->ccaes_cbc_decrypt) {
        return false;
    }
    // NOLINTNEXTLINE(bugprone-parent-virtual-call): IOAESAccelerator's start drives Apple's engine; IOService's is wanted
    if (!IOService::start(provider)) {
        return false;
    }
    setProperty("NativeAESSoftwareKeysOnly", true);
    native_aes_registered = 1;
    registerService();
    return true;
}

IOReturn NativeAES::performAES(IOMemoryDescriptor *input, IOMemoryDescriptor *output, unsigned long long bytes,
                               IOAESIV *iv, IOAESOperation operation, IOAESKeyData *key, unsigned long long inputOffset,
                               unsigned long long outputOffset, IOAESCompletion callback, void *refcon)
{
    if (!input || !output || !key || !iv || !bytes || (bytes & 15) ||
        (key->bits != 128 && key->bits != 192 && key->bits != 256))
        return kIOReturnBadArgument;
    PreparedKey prep;
    IOReturn result = resolveKey(*key, operation, &prep);
    if (result)
        return result;
    if (inputOffset > input->getLength() || bytes > input->getLength() - inputOffset ||
        outputOffset > output->getLength() || bytes > output->getLength() - outputOffset)
        return kIOReturnBadArgument;
    result = input->prepare();
    if (result)
        return result;
    const bool sameDescriptor = input == output;
    if (!sameDescriptor) {
        result = output->prepare();
        if (result) {
            input->complete();
            return result;
        }
    }
    unsigned char chaining[16];
    memcpy(chaining, iv->bytes, 16);
    unsigned char block[16];
    bool usedBlock = false;
    // Walk physical segments through the direct physical aperture instead of createMappingInTask(kernel_task):
    // avoids per-call kernel_pmap broadcast TLB shootdowns and guarantees identical physical addresses produce the
    // identical virtual address (never two aliased VAs for in-place CBC decrypt).
    for (unsigned long long done = 0; done < bytes;) {
        IOByteCount inLen = 0;
        IOByteCount outLen = 0;
        const addr64_t inPa = input->getPhysicalSegment(inputOffset + done, &inLen, kIOMemoryMapperNone);
        addr64_t outPa = inPa;
        if (sameDescriptor && inputOffset == outputOffset) {
            outLen = inLen;
        } else {
            outPa = output->getPhysicalSegment(outputOffset + done, &outLen, kIOMemoryMapperNone);
        }
        if (!inPa || !inLen || !outPa || !outLen) {
            result = kIOReturnIOError;
            break;
        }
        const unsigned long long remain = bytes - done;
        if (cacheableRam(inPa) && cacheableRam(outPa)) {
            vm_size_t inVaLen = static_cast<vm_size_t>(inLen < remain ? inLen : remain);
            const vm_map_address_t inVa = phystokv_range(inPa, &inVaLen);
            vm_size_t outVaLen = inVaLen;
            vm_map_address_t outVa = inVa;
            if (outPa != inPa) {
                outVaLen = static_cast<vm_size_t>(outLen < remain ? outLen : remain);
                outVa = phystokv_range(outPa, &outVaLen);
            }
            const size_t chunk = (inVaLen < outVaLen ? inVaLen : outVaLen) & ~static_cast<size_t>(15);
            const bool safeDirect =
                inVa && outVa && (chunk >= 16) && (inPa == outPa || inPa + chunk <= outPa || outPa + chunk <= inPa);
            if (safeDirect) {
                if (cbcChunk(reinterpret_cast<const unsigned char *>(inVa), reinterpret_cast<unsigned char *>(outVa),
                             chunk, chaining, prep, operation)) {
                    result = kIOReturnIOError;
                    break;
                }
                done += chunk;
                continue;
            }
        }
        usedBlock = true;
        if (input->readBytes(inputOffset + done, block, 16) != 16 ||
            cbcChunk(block, block, 16, chaining, prep, operation) != 0 ||
            output->writeBytes(outputOffset + done, block, 16) != 16) {
            result = kIOReturnIOError;
            break;
        }
        done += 16;
    }
    if (usedBlock)
        cc_clear(sizeof(block), block);
    if (!result) {
        memcpy(iv->bytes, chaining, 16);
        __atomic_fetch_add(&native_aes_completed, 1, __ATOMIC_RELAXED);
    }
    cc_clear(sizeof(chaining), chaining);
    if (!sameDescriptor)
        output->complete();
    input->complete();
    if (callback) {
        callback(refcon, result);
        return kIOReturnSuccess;
    }
    return result;
}

IOReturn NativeAES::performAES(void *input, void *output, task_t task, unsigned long long bytes, IOAESIV *iv,
                               IOAESOperation operation, IOAESKeyData *key, IOAESCompletion callback, void *refcon)
{
    if (!task || !input || !output || !key || !iv || !bytes || (bytes & 15) ||
        (key->bits != 128 && key->bits != 192 && key->bits != 256))
        return kIOReturnBadArgument;
    if (task == kernel_task) {
        PreparedKey prep;
        IOReturn result = resolveKey(*key, operation, &prep);
        if (result)
            return result;
        unsigned char chaining[16];
        memcpy(chaining, iv->bytes, 16);
        if (cbcChunk(static_cast<const unsigned char *>(input), static_cast<unsigned char *>(output),
                     static_cast<size_t>(bytes), chaining, prep, operation)) {
            result = kIOReturnIOError;
        } else {
            memcpy(iv->bytes, chaining, 16);
            __atomic_fetch_add(&native_aes_completed, 1, __ATOMIC_RELAXED);
        }
        cc_clear(sizeof(chaining), chaining);
        if (callback) {
            callback(refcon, result);
            return kIOReturnSuccess;
        }
        return result;
    }
    if (input == output) {
        IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withAddressRange(
            reinterpret_cast<mach_vm_address_t>(input), bytes, kIODirectionOutIn, task);
        if (!descriptor)
            return kIOReturnNoMemory;
        const IOReturn result = performAES(descriptor, descriptor, bytes, iv, operation, key, 0, 0, callback, refcon);
        descriptor->release();
        return result;
    }
    IOMemoryDescriptor *source =
        IOMemoryDescriptor::withAddressRange(reinterpret_cast<mach_vm_address_t>(input), bytes, kIODirectionOut, task);
    IOMemoryDescriptor *destination =
        IOMemoryDescriptor::withAddressRange(reinterpret_cast<mach_vm_address_t>(output), bytes, kIODirectionIn, task);
    if (!source || !destination) {
        if (source)
            source->release();
        if (destination)
            destination->release();
        return kIOReturnNoMemory;
    }
    const IOReturn result = performAES(source, destination, bytes, iv, operation, key, 0, 0, callback, refcon);
    source->release();
    destination->release();
    return result;
}
