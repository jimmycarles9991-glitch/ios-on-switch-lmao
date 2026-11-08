// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "OriginalIOAESAccelerator.h"
#include <IOKit/IOLib.h>
#include <libkern/crypto/aes.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#include <libkern/crypto/crypto_internal.h>
#pragma clang diagnostic pop
#include <corecrypto/cc.h>
#include <kern/task.h>
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../TegraSE/TegraSE.h"

extern "C" bool native_aks_device_digest(const char *label, uint8_t out[32]);
extern "C" void native_aks_device_key_start(void);

namespace {
constexpr uint64_t kRefusedKey = switch_refusal_key("NativeAES", "hardware key");
// Apple's engine holds the device-unique UID key (selector 2000) and keys derived from it, which the caller names but
// never sees (AppleImage4 entangles its nonces with 2212); below 2000 are the GID keys that decrypt Apple's firmware,
// which nothing here can stand in for.
constexpr unsigned kFirstDeviceKey = 2000;

// The console's own key for a UID-class selector: an HMAC of the selector under the device key AppleKeyStore uses
// (drivers/NativeKeyStoreEvents/AKSDeviceKey.cpp), so it is the same on every boot of this console and only on it.
bool deviceKey(const IOAESKeyData &key, IOAESKeyData *out)
{
    if (key.selector < kFirstDeviceKey)
        return false;
    native_aks_device_key_start();
    char label[48];
    snprintf(label, sizeof label, "Switch IOAESAccelerator key %u", key.selector);
    uint8_t digest[32];
    if (!native_aks_device_digest(label, digest))
        return false;
    out->bits = key.bits;
    out->selector = 0;
    memcpy(out->bytes, digest, sizeof out->bytes);
    cc_clear(sizeof digest, digest);
    return true;
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

    // No padding, and no change to the registered corecrypto modes.
    static int cbc(const unsigned char *input, unsigned char *output, size_t bytes, unsigned char iv[16],
                   const IOAESKeyData &key, IOAESOperation operation)
{
    union {
        aes_encrypt_ctx encrypt;
        aes_decrypt_ctx decrypt;
    } context;

    unsigned char nextIV[16];
    int result = 0;
    // The Security Engine first (drivers/TegraSE: it updates iv the same way); the CPU if it declines.
    if (!tegra_se_aes(TEGRA_SE_CBC, operation != IOAESDecrypt, key.bytes, key.bits / 8, iv, input, output, bytes))
        return 0;
    if (operation == IOAESDecrypt) {
        memcpy(nextIV, input + bytes - 16, 16);
        result = aes_decrypt_key(key.bytes, key.bits / 8, &context.decrypt);
        if (!result)
            result = aes_decrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, &context.decrypt);
    } else {
        result = aes_encrypt_key(key.bytes, key.bits / 8, &context.encrypt);
        if (!result)
            result = aes_encrypt_cbc(input, iv, static_cast<unsigned>(bytes / 16), output, &context.encrypt);
        if (!result)
            memcpy(nextIV, output + bytes - 16, 16);
    }
    if (!result)
        memcpy(iv, nextIV, 16);
    cc_clear(sizeof(context), &context);
    cc_clear(sizeof(nextIV), nextIV);
    return result;
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
    IOAESKeyData derived{};

    const struct Wipe {
        IOAESKeyData *key;

        ~Wipe()
        {
            cc_clear(sizeof *key, key);
        }
    } wipe{&derived};

    if (key->selector && deviceKey(*key, &derived))
        key = &derived;
    if (key->selector || (operation != IOAESEncrypt && operation != IOAESDecrypt)) {
        __atomic_fetch_add(&native_aes_unsupported_key, 1, __ATOMIC_RELAXED);
        switch_refusal_k(switch_refusal_key_with(kRefusedKey, static_cast<uint64_t>(key->selector) << 32 |
                                                                  static_cast<uint32_t>(operation)),
                         "NativeAES", "hardware key",
                         "key selector %u, operation %d: only keys the caller passes or the console's own, encrypt or "
                         "decrypt",
                         static_cast<unsigned>(key->selector), static_cast<int>(operation));
        return kIOReturnUnsupported;
    }
    if (inputOffset > input->getLength() || bytes > input->getLength() - inputOffset ||
        outputOffset > output->getLength() || bytes > output->getLength() - outputOffset)
        return kIOReturnBadArgument;
    IOReturn result = input->prepare();
    if (result)
        return result;
    result = output->prepare();
    if (result) {
        input->complete();
        return result;
    }
    // Mapped, not copied: TegraSE takes the whole run by scatter-gather DMA from these pages, and what it declines runs
    // on the CPU in place.
    IOMemoryMap *source =
        input->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapReadOnly, inputOffset, bytes);
    IOMemoryMap *destination = output->createMappingInTask(kernel_task, 0, kIOMapAnywhere, outputOffset, bytes);
    unsigned char chaining[16];
    memcpy(chaining, iv->bytes, 16);
    if (!source || !destination) {
        result = kIOReturnNoMemory;
    } else if (cbc(reinterpret_cast<const unsigned char *>(source->getVirtualAddress()),
                   reinterpret_cast<unsigned char *>(destination->getVirtualAddress()), static_cast<size_t>(bytes),
                   chaining, *key, operation)) {
        result = kIOReturnIOError;
    } else {
        memcpy(iv->bytes, chaining, 16);
        __atomic_fetch_add(&native_aes_completed, 1, __ATOMIC_RELAXED);
    }
    cc_clear(sizeof(chaining), chaining);
    OSSafeReleaseNULL(source);
    OSSafeReleaseNULL(destination);
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
    if (!task || !input || !output || !bytes)
        return kIOReturnBadArgument;
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
