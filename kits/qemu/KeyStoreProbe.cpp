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
static bool keyeventsProbed;
#if DEVELOPMENT || DEBUG
volatile uint32_t native_keyevents_stage, native_keyevents_refused_opens;
volatile int native_keyevents_status = -1;
volatile uint64_t native_keyevents_service;
#endif

void native_keyevents_open_refused(void)
{
#if DEVELOPMENT || DEBUG
    native_keyevents_refused_opens = native_keyevents_refused_opens + 1;
#endif
}

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

static void native_keyevents_probe()
{
    if (keyeventsProbed)
        return;
    keyeventsProbed = true;
#if DEVELOPMENT || DEBUG
    native_keyevents_stage = 1;
    native_keyevents_status = kIOReturnNotPermitted;
#endif
    // No SEP: stand in for AppleKeyStore whether or not Data is ephemeral.
    const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
    const bool absent = (name != nullptr) && (OSMetaClass::getMetaClassWithName(name) == nullptr);
    OSSafeReleaseNULL(name);
    if (!absent)
        return;
    IOService *provider = IOService::getResourceService();
    if (!provider)
        return;
    const OSReturn status = switch_kernel_classes(native_keyevents_static_init);
#if DEVELOPMENT || DEBUG
    native_keyevents_status = status;
#endif
    if (status)
        return;
#if DEVELOPMENT || DEBUG
    native_keyevents_stage = 2;
#endif
    IOService *service = native_keyevents_allocate();
    const bool initialized = (service != nullptr) && service->init();
    const bool attached = initialized && service->attach(provider);
    const bool started = attached && service->start(provider);
    if (!started) {
        if (attached)
            service->detach(provider);
        OSSafeReleaseNULL(service);
#if DEVELOPMENT || DEBUG
        native_keyevents_status = kIOReturnError;
#endif
        return;
    }
#if DEVELOPMENT || DEBUG
    native_keyevents_service = (uintptr_t)service;
    native_keyevents_status = kIOReturnSuccess;
    native_keyevents_stage = 3;
#endif
}

void native_keystore_probe(void)
{
    native_keyevents_probe();
}
}
