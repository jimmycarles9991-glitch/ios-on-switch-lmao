// SPDX-License-Identifier: BSD-2-Clause-Patent
// Mutually exclusive offline probes: unchanged target keystore loading, or
// ephemeral notification-only service. Neither supplies keys or unlock state.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
extern "C" OSReturn switch_kernel_classes(void (*initialize)());
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" {
volatile uint32_t native_keyevents_stage, native_keyevents_refused_opens;
volatile int native_keyevents_status = -1;
volatile uint64_t native_keyevents_service;

void native_keyevents_open_refused(void)
{
    native_keyevents_refused_opens = native_keyevents_refused_opens + 1;
}

// The owned AppleKeyStore answers the state queries (no passcode, nothing locked); counters feed a debug kernel's
// diagnostic page's extension rows (slots 0-1): opens/calls, then selector:count per selector seen ('e' = the last
// answer was an error).
#if DEVELOPMENT || DEBUG
extern char boot_progress_ext[8][48];
volatile uint32_t native_aks_opens, native_aks_calls;
static volatile uint32_t aks_count[96];
static volatile int32_t aks_rc[96];
#endif

bool native_aks_state_enabled(void)
{
    return true;
}

// The device-state "sls" field, which MKBGetDeviceLockState returns: 0 unlocked, 1 locked, 2 locking, 3 disabled (no
// passcode). Default 3: this keybag has no passcode, and nothing here could check one (no SEP, no AppleCredentialManager).
// With 0, MobileKeyBag reports "passcode set, unlocked": SpringBoard's hasPasscodeSet is YES and lockdownd asks for
// the passcode after Trust (LocalAuthentication, ACMContextCreate -5), takes the failure as a denial and refuses USB
// pairing.
uint32_t native_aks_lock_state(void)
{
    return 3;
}

#if DEVELOPMENT || DEBUG
static void aks_publish()
{
    // Slot 0: counters; slot 1: selector:count tokens ('e' = the last answer was an error). Slots 2-3 belong to the Data store.
    char row[48];
    unsigned used = 0;
    row[0] = 0;
    for (unsigned sel = 0; sel < 96; ++sel) {
        if (!aks_count[sel])
            continue;
        char token[16];
        const int n = snprintf(token, sizeof token, "%u:%u%s ", sel, aks_count[sel], aks_rc[sel] ? "e" : "");
        if (n <= 0 || used + (unsigned)n > 46)
            break;
        memcpy(row + used, token, (size_t)n + 1);
        used += (unsigned)n;
    }
    char head[48];
    snprintf(head, sizeof head, "AKS opens=%u calls=%u", native_aks_opens, native_aks_calls);
    for (unsigned i = 0; i < 47; ++i)
        boot_progress_ext[0][i] = head[i] ? head[i] : ' ';
    boot_progress_ext[0][47] = 0;
    unsigned i = 0;
    for (; i < 47 && row[i]; ++i)
        boot_progress_ext[1][i] = row[i];
    for (; i < 47; ++i)
        boot_progress_ext[1][i] = ' ';
    boot_progress_ext[1][47] = 0;
}

void native_aks_opened(void)
{
    native_aks_opens = native_aks_opens + 1;
    aks_publish();
}

void native_aks_call(uint32_t selector, int32_t result)
{
    native_aks_calls = native_aks_calls + 1;
    if (selector < 96) {
        aks_count[selector] = aks_count[selector] + 1;
        aks_rc[selector] = result;
    }
    aks_publish();
}
#else
void native_aks_opened(void)
{}

void native_aks_call(uint32_t, int32_t)
{}
#endif

static void native_keyevents_probe()
{
    if (native_keyevents_stage)
        return;
    native_keyevents_stage = 1;
    native_keyevents_status = kIOReturnNotPermitted;
    // No SEP: stand in for AppleKeyStore whether or not Data is ephemeral.
    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    const bool absent = (name != nullptr) && (OSMetaClass::getMetaClassWithName(name) == nullptr);
    OSSafeReleaseNULL(name);
    if (!absent)
        return;
    IOService *provider = IOService::getResourceService();
    if (!provider)
        return;
    native_keyevents_status = switch_kernel_classes(native_keyevents_static_init);
    if (native_keyevents_status)
        return;
    native_keyevents_stage = 2;
    IOService *service = native_keyevents_allocate();
    const bool initialized = (service != nullptr) && service->init();
    const bool attached = initialized && service->attach(provider);
    const bool started = attached && service->start(provider);
    if (!started) {
        if (attached)
            service->detach(provider);
        OSSafeReleaseNULL(service);
        native_keyevents_status = kIOReturnError;
        return;
    }
    native_keyevents_service = (uintptr_t)service;
    native_keyevents_status = kIOReturnSuccess;
    native_keyevents_stage = 3;
}

void native_keystore_probe(void)
{
    native_keyevents_probe();
}
}
