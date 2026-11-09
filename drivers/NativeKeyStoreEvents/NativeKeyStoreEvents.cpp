// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleKeyStore without a SEP: a notification service (the public IOKit matching class used by MKBEventsRegister) and a user
// client for the unchanged libaks/MobileKeyBag/securityd of a device with no passcode: one system
// keybag (handle 0), always unlocked, and class keys derived from the console's fuse chip ID (AKSDeviceKey.cpp), so the
// keychain's wrapped keys survive reboots and stay on this console. Not SEP-grade: the chip ID is not secret.
//
// Selectors and layouts read from the target's AppleKeyStore.framework (21H461; libaks, the same code securityd links
// statically), IOConnectCallMethod(conn, selector, scalars in, struct in, scalars out, struct out):
//   0, 4, 5, 9, 12, 13, 26, 27, 55, 101, 102   succeed, no outputs (open handshake, bag/assertion/notify bookkeeping; the set
//            iOS 16.7 needs was established on hardware by github.com/dipa-studio/ios-sweet)
//   7        aks_get_lock_state(bag)                in [bag]; out [state]: bit 2 = unlocked since boot
//   10       aks_wrap_key(key, len, class, bag)     in [bag, class] + key; out [class] + RFC 3394 blob (len + 8)
//   11       aks_unwrap_key(blob, len, class, bag)  in [bag, class] + blob; out key (len - 8); a blob that fails the integrity
//            check answers e00002bc, which securityd turns into "can't be decrypted on this device, ever"
//   14       aks_get_system                         out [system keybag handle 0] (keybagd then needs no systembag.kb); the
//            first answer is the bag's first unlock: messageClients(e3ffd00f, &handle, 4), once per boot
//   17, 35   aks_get_(extended_)device_state        struct out: DER SET{ SEQUENCE{ UTF8String key, INTEGER value } ... }
//            keys ss sls sb sfa sgs sas sgpe srcd sms bh; ss = 4, sls = lock state: 3 = MobileKeyBag's "no passcode" (default;
//            lockdownd and SpringBoard read "has passcode" as sls != 3), sheng-aks-sls=<n> overrides (KeyStoreProbe.cpp)
//   23       aks_get_bag_uuid(bag)                  in [bag]; struct out 16-byte UUID, stable per keybag on this console
//   31, 139  aks_get_configuration / aks_memento_get_state: e00002f0 not found (no configuration, no recovery passcode), which
//            MobileKeyBag maps instead of logging "Unexpected AppleKeyStore error"
// Malformed requests answer e00002c2, a keybag other than 0 e00002f0; every other selector e00002c7 and one console line.
// aks_register_for_notifications (IOConnectSetNotificationPort, keybagd at start) is accepted.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <pexpert/pexpert.h>
#include "AKSKeyWrap.h"
extern "C" void native_keyevents_open_refused(void);
extern "C" bool native_aks_state_enabled(void);
extern "C" uint32_t native_aks_lock_state(void);
extern "C" void native_aks_call(uint32_t selector, int32_t result);
extern "C" void native_aks_opened(void);
extern "C" void native_aks_device_key_start(void);
extern "C" IOReturn native_aks_wrap(uint32_t keybag, uint32_t keyClass, const uint8_t *key, size_t length, uint8_t *out,
                                    size_t capacity, size_t *written);
extern "C" IOReturn native_aks_unwrap(uint32_t keybag, uint32_t keyClass, const uint8_t *wrapped, size_t length,
                                      uint8_t *out, size_t capacity, size_t *written);
extern "C" IOReturn native_aks_bag_uuid(uint32_t keybag, uint8_t uuid[16]);
extern "C" void native_aks_unsupported(uint32_t selector);

namespace {
constexpr uint32_t kSelLockState = 7, kSelWrapKey = 10, kSelUnwrapKey = 11, kSelGetSystem = 14, kSelDeviceState = 17,
                   kSelGetBagUUID = 23, kSelGetConfiguration = 31, kSelExtendedDeviceState = 35,
                   kSelSeSupportInRecovery = 107, kSelMementoState = 139;
constexpr uint64_t kUnlockedSinceBoot = 4;
constexpr uint32_t kStateFlags = 4;
constexpr uint64_t kSystemHandle = 0;
constexpr uint32_t kMessageFirstUnlock = 0xe3ffd00f;
constexpr uint64_t kMaxKeyClass = 31;  // protection classes are small integers (securityd's "class 11" is dku)
constexpr IOReturn kAksError =
    static_cast<IOReturn>(aks::kReturnError);  // also what libaks reports itself when the connection is missing
constexpr IOReturn kAksBadArgument = static_cast<IOReturn>(aks::kReturnBadArgument);
constexpr IOReturn kAksUnsupported = static_cast<IOReturn>(aks::kReturnUnsupported);
constexpr IOReturn kAksNotFound = static_cast<IOReturn>(aks::kReturnNotFound);

struct DerWriter {
    uint8_t *p;
    size_t cap, n = 0;
    bool ok = true;

    void put(uint8_t b)
    {
        if (n < cap) {
            p[n++] = b;
        } else {
            ok = false;
        }
    }

    void len(size_t l)
    {
        if (l < 0x80) {
            put(static_cast<uint8_t>(l));
        } else {
            put(0x81);
            put(static_cast<uint8_t>(l));
        }
    }  // entries stay below 256 bytes
};

// Smallest two's-complement big-endian INTEGER content for a non-negative value.
size_t derIntegerContent(uint64_t v, uint8_t out[9])
{
    uint8_t raw[8];
    for (unsigned i = 0; i < 8; ++i)
        raw[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
    size_t start = 0;
    while (start < 7 && raw[start] == 0)
        ++start;
    size_t n = 0;
    if (raw[start] & 0x80)
        out[n++] = 0;
    while (start < 8)
        out[n++] = raw[start++];
    return n;
}

size_t entrySize(const char *key, uint64_t value)
{
    uint8_t c[9];
    size_t kl = 0;
    while (key[kl])
        ++kl;
    const size_t body = (2 + kl) + (2 + derIntegerContent(value, c));
    return 2 + body;
}
}  // namespace

// DER SET of SEQUENCE{UTF8String, INTEGER}. Returns the length, or 0 when the buffer is too small.
extern "C" size_t native_aks_encode_state(uint8_t *out, size_t capacity, uint32_t lockState)
{
    struct Entry {
        const char *key;
        uint64_t value;
    };

    const Entry entries[] = {{"ss", kStateFlags}, {"sls", lockState}, {"sb", 0},   {"sfa", 0}, {"sgs", 0},
                             {"sas", 0},          {"sgpe", 0},        {"srcd", 0}, {"sms", 0}, {"bh", 0}};
    size_t total = 0;
    for (const Entry &e : entries)
        total += entrySize(e.key, e.value);
    DerWriter w{out, capacity};
    w.put(0x31);
    w.len(total);
    for (const Entry &e : entries) {
        uint8_t c[9];
        size_t kl = 0;
        while (e.key[kl])
            ++kl;
        const size_t il = derIntegerContent(e.value, c);
        w.put(0x30);
        w.len((2 + kl) + (2 + il));
        w.put(0x0c);
        w.put((uint8_t)kl);
        for (size_t i = 0; i < kl; ++i)
            w.put((uint8_t)e.key[i]);
        w.put(0x02);
        w.put((uint8_t)il);
        for (size_t i = 0; i < il; ++i)
            w.put(c[i]);
    }
    return w.ok ? w.n : 0;
}

// The caller's struct output: inline (structureOutput, at most 4 KiB) or a descriptor for larger buffers.
static IOReturn writeStructure(IOExternalMethodArguments *args, const uint8_t *bytes, size_t n)
{
    if (args->structureOutputDescriptor) {
        if (n > args->structureOutputDescriptor->getLength())
            return kIOReturnNoSpace;
        args->structureOutputDescriptor->writeBytes(0, bytes, n);
        args->structureOutputDescriptorSize = static_cast<uint32_t>(n);
        return kIOReturnSuccess;
    }
    if (!args->structureOutput || n > args->structureOutputSize)
        return kIOReturnNoSpace;
    for (size_t k = 0; k < n; ++k)
        static_cast<uint8_t *>(args->structureOutput)[k] = bytes[k];
    args->structureOutputSize = static_cast<uint32_t>(n);
    return kIOReturnSuccess;
}

static size_t structureCapacity(const IOExternalMethodArguments *args)
{
    if (args->structureOutputDescriptor)
        return args->structureOutputDescriptor->getLength();
    return args->structureOutput ? args->structureOutputSize : 0;
}

// AppleSEPKeyStore sends this once per boot when the SEP first unlocks a keybag, with the bag's handle (24B119 and 21H461:
// "Sending First Unlock notification for handle %d"); with no passcode that is when the system bag is set up, here the
// first aks_get_system, from keybagd --init. launchd registers its interest before any boot task runs, and on iOS 27
// reboots on a critical process's crash loop unless this arrived more than five minutes before.
static void sendFirstUnlock(IOService *store)
{
    static volatile UInt32 sent;
    if (!store || !OSCompareAndSwap(0, 1, &sent))
        return;
    uint32_t handle = static_cast<uint32_t>(kSystemHandle);
    store->messageClients(kMessageFirstUnlock, &handle, sizeof handle);
}

// Selectors 10 and 11: [bag, class] in, the key or blob inline (wrapped keys stay far below the 4 KiB inline limit).
static IOReturn keyOperation(uint32_t selector, IOExternalMethodArguments *args)
{
    const bool wrapping = selector == kSelWrapKey;
    if (args->scalarInputCount != 2 || !args->scalarInput || args->scalarOutputCount != (wrapping ? 1u : 0u) ||
        (wrapping && !args->scalarOutput) || !args->structureInput || args->structureInputDescriptor) {
        return kAksBadArgument;
    }
    const int64_t bag = static_cast<int64_t>(args->scalarInput[0]);
    const int64_t keyClass = static_cast<int64_t>(args->scalarInput[1]);
    if (bag != static_cast<int64_t>(kSystemHandle))
        return bag < 0 ? kAksBadArgument : kAksNotFound;
    if (keyClass < 1 || keyClass > static_cast<int64_t>(kMaxKeyClass))
        return kAksBadArgument;
    uint8_t out[aks::kMaxKey + aks::kBlock];
    size_t written = 0;
    size_t capacity = structureCapacity(args);
    if (capacity > sizeof out)
        capacity = sizeof out;
    const uint8_t *in = static_cast<const uint8_t *>(args->structureInput);
    IOReturn rc = wrapping ? native_aks_wrap(static_cast<uint32_t>(bag), static_cast<uint32_t>(keyClass), in,
                                             args->structureInputSize, out, capacity, &written)
                           : native_aks_unwrap(static_cast<uint32_t>(bag), static_cast<uint32_t>(keyClass), in,
                                               args->structureInputSize, out, capacity, &written);
    if (rc == kIOReturnSuccess)
        rc = writeStructure(args, out, written);
    aks::wipe(out, sizeof out);
    if (rc != kIOReturnSuccess)
        return rc;
    if (wrapping) {
        args->scalarOutput[0] = static_cast<uint64_t>(keyClass);
        args->scalarOutputCount = 1;
    }
    return kIOReturnSuccess;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs external linkage (kalloc type view)
class AppleKeyStoreUserClient final : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(AppleKeyStoreUserClient) public : IOReturn clientClose() override
    {
        terminate();
        return kIOReturnSuccess;
    }

    // keybagd's port for recovery completion and system keybag updates (24B119: "aks notification for recovery completion
    // %d"); neither happens to a keybag with no passcode and no SEP, so the right is dropped once accepted. A refusal costs
    // keybagd's "Can't register for notifications with AppleKeyStore" at every start.
    IOReturn registerNotificationPort(mach_port_t port, UInt32, UInt32) override
    {
        if (port != MACH_PORT_NULL)
            releaseNotificationPort(port);
        return kIOReturnSuccess;
    }

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *,
                            void *) override
    {
        IOReturn rc = kAksUnsupported;
        if (!args) {
            rc = kAksError;
        } else {
            switch (selector) {
            // Handshake right after IOServiceOpen (a failure makes libaks drop the connection and reopen on every call), and the
            // bag/assertion/notification calls that need no answer in an environment without keybags.
            case 0:
            case 4:
            case 5:
            case 9:
            case 12:
            case 13:
            case 26:
            case 27:
            case 55:
            case 101:
            case 102:
                args->scalarOutputCount = 0;
                rc = kIOReturnSuccess;
                break;
            case kSelGetSystem:
                if (args->scalarOutputCount < 1 || !args->scalarOutput) {
                    rc = kIOReturnBadArgument;
                    break;
                }
                args->scalarOutput[0] = kSystemHandle;
                args->scalarOutputCount = 1;
                rc = kIOReturnSuccess;
                sendFirstUnlock(getProvider());
                break;
            case kSelLockState:
                rc = (args->scalarInputCount == 1 && args->scalarOutputCount == 1 && args->scalarOutput)
                         ? kIOReturnSuccess
                         : kIOReturnBadArgument;
                if (rc == kIOReturnSuccess)
                    args->scalarOutput[0] = kUnlockedSinceBoot;
                break;
            case kSelDeviceState:
            case kSelExtendedDeviceState: {
                if (args->scalarInputCount > 1 || args->scalarOutputCount != 0) {
                    rc = kIOReturnBadArgument;
                    break;
                }
                uint8_t der[160];
                const size_t n = native_aks_encode_state(der, sizeof der, native_aks_lock_state());
                rc = n ? writeStructure(args, der, n) : kIOReturnNoSpace;
                break;
            }
            case kSelWrapKey:
            case kSelUnwrapKey:
                rc = keyOperation(selector, args);
                break;
            case kSelGetBagUUID: {
                if (args->scalarInputCount != 1 || !args->scalarInput || args->scalarOutputCount != 0) {
                    rc = kAksBadArgument;
                    break;
                }
                if (static_cast<int64_t>(args->scalarInput[0]) != static_cast<int64_t>(kSystemHandle)) {
                    rc = kAksNotFound;
                    break;
                }
                uint8_t uuid[16];
                rc = native_aks_bag_uuid(static_cast<uint32_t>(kSystemHandle), uuid);
                if (rc == kIOReturnSuccess)
                    rc = writeStructure(args, uuid, sizeof uuid);
                break;
            }
            // aks_se_support_in_rm_is_set (iOS 27's keybagd): no Secure Enclave, so no SE support in recovery mode.
            case kSelSeSupportInRecovery:
                if (args->scalarInputCount != 0 || args->scalarOutputCount != 1 || !args->scalarOutput) {
                    rc = kAksBadArgument;
                    break;
                }
                args->scalarOutput[0] = 0;
                rc = kIOReturnSuccess;
                break;
            case kSelGetConfiguration:
            case kSelMementoState:
                rc = (args->scalarInputCount == 1 && args->scalarOutputCount == 0) ? kAksNotFound : kAksBadArgument;
                break;
            default:
                native_aks_unsupported(selector);
                break;
            }
        }
        native_aks_call(selector, static_cast<int32_t>(rc));
        return rc;
    }
};

OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient)

    // NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs external linkage (kalloc type view)
    class AppleKeyStore final : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleKeyStore) IOReturn
        open(task_t task, void *security, UInt32 type, OSDictionary *properties, IOUserClient **out)
    {
        if (out)
            *out = nullptr;
        if (!native_aks_state_enabled() || !out) {
            native_keyevents_open_refused();
            return kIOReturnNotReady;
        }
        AppleKeyStoreUserClient *client = new AppleKeyStoreUserClient;
        if (!client)
            return kIOReturnNoMemory;
        if (!client->initWithTask(task, security, type, properties) || !client->attach(this)) {
            client->release();
            return kIOReturnError;
        }
        if (!client->start(this)) {
            client->detach(this);
            client->release();
            return kIOReturnError;
        }
        native_aks_opened();
        *out = client;
        return kIOReturnSuccess;
    }

public:
    bool start(IOService *provider) override
    {
        if (!IOService::start(provider))
            return false;
        setName("Native keystore state service");
        // Panics without a device key: securityd would otherwise drop every item it wrapped in an earlier boot.
        if (native_aks_state_enabled())
            native_aks_device_key_start();
        // CloudKeychainProxy's open is the one MACF refuses (its sandbox report is the kernel error line on its own thread at each
        // "failed to open connection to AppleKeyStore"); our IOUserClient.cpp grants the one named process in its place.
        if (!setProperty("NativeKeyStoreEvents", true) ||
            !setProperty("KeyOperationsAvailable", native_aks_state_enabled()) ||
            !setProperty("NativeSandboxGrantProcess", "CloudKeychainProxy")) {
            IOService::stop(provider);
            return false;
        }
        registerService();
        return true;
    }

    IOReturn newUserClient(task_t t, void *s, UInt32 type, OSDictionary *p, IOUserClient **out) override
    {
        return open(t, s, type, p, out);
    }

    IOReturn newUserClient(task_t t, void *s, UInt32 type, IOUserClient **out) override
    {
        return open(t, s, type, nullptr, out);
    }
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService) extern "C" IOService *native_keyevents_allocate(void)
{
    return new AppleKeyStore;
}
