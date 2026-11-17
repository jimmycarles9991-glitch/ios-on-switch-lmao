// SPDX-License-Identifier: GPL-2.0-only
#include <libkern/libkern.h>
#include <libkern/amfi/amfi.h>
#include <switch/capabilities.h>
#include <switch/entitlements_factory.h>

#if XNU_10063_141_1
#include <switch/entitlements_factory_21H461.h>
#include <kern/debug.h>

static __attribute__((noreturn)) void switch_entitlements_abort(const CERuntime_t runtime, const char *format, ...)
{
    (void)runtime;
    (void)format;
    panic("SwitchAppCode: CoreEntitlements runtime invariant failed");
}
#elif XNU_12377_121_6
#include <switch/entitlements_factory_24B119.h>
#endif

static bool switch_entitlements_entry(uintptr_t address, const uint32_t *expected, size_t count)
{
    if (!address || (address & 3)) return false;
    const uint32_t *actual = (const uint32_t *)address;
    for (size_t i = 0; i < count; ++i) {
        // The prelinker replaces PACIBSP with NOP on the A57.
        if (actual[i] != expected[i] && !(expected[i] == 0xd503237f && actual[i] == 0xd503201f)) return false;
    }
    return true;
}

#define SWITCH_ENTITLEMENTS_ENTRY(address, words) \
    switch_entitlements_entry(address, words, sizeof(words) / sizeof((words)[0]))

static uintptr_t switch_entitlements_factory(const void *der, size_t size)
{
    if (!der || !size || size > SWITCH_CAPABILITIES_MAX_SIZE || !amfi ||
        !amfi->OSEntitlements_invalidate) return 0;
    const uintptr_t anchor = (uintptr_t)amfi->OSEntitlements_invalidate;
    const uintptr_t factory = anchor + SWITCH_AMFI_FACTORY_DELTA;
    if (!SWITCH_ENTITLEMENTS_ENTRY(anchor, switch_amfi_anchor_words) ||
        !SWITCH_ENTITLEMENTS_ENTRY(factory, switch_amfi_factory_words)) return 0;
    return factory;
}

void *switch_app_code_native_dictionary(const void *der, size_t size)
{
    if (!der || !size || size > SWITCH_CAPABILITIES_MAX_SIZE || !amfi) return NULL;
#if XNU_10063_141_1
    const coreentitlements_t *ce = &amfi->CoreEntitlements;
    if (!ce->Validate || !ce->AcquireUnmanagedContext || !amfi->query_context_to_object) return NULL;
    const struct CERuntime runtime = {.version = CE_RUNTIME_VERSION, .abort = switch_entitlements_abort};
    CEValidationResult validated = {0};
    struct CEQueryContext context = {0};
    const uint8_t *bytes = der;
    if (ce->Validate(&runtime, &validated, bytes, bytes + size) != ce->kNoError ||
        ce->AcquireUnmanagedContext(&runtime, validated, &context) != ce->kNoError) return NULL;
    return amfi->query_context_to_object(&context);
#elif XNU_12377_121_6
    const uintptr_t factory = switch_entitlements_factory(der, size);
    if (!factory || !amfi->OSEntitlements_asdict) return NULL;
    void *object = ((void *(*)(const void *, size_t))factory)(der, size);
    if (!object) return NULL;
    void *dictionary = amfi->OSEntitlements_asdict(object);
    amfi->OSEntitlements_invalidate(object);
    osobject_release(object);
    return dictionary;
#endif
}

void *switch_app_code_native_entitlements(const void *der, size_t size, const char *identity,
                                         struct cs_blob *blob)
{
    if (!identity || !identity[0]) return NULL;
    const uintptr_t factory = switch_entitlements_factory(der, size);
    if (!factory) return NULL;
#if XNU_10063_141_1
    if (!amfi->CoreEntitlements.Validate) return NULL;
    const struct CERuntime runtime = {.version = CE_RUNTIME_VERSION, .abort = switch_entitlements_abort};
    CEValidationResult validated = {0};
    const uint8_t *bytes = der;
    if (amfi->CoreEntitlements.Validate(&runtime, &validated, bytes, bytes + size) != amfi->CoreEntitlements.kNoError)
        return NULL;
    return ((void *(*)(const CEValidationResult *, struct cs_blob *, bool))factory)(&validated, blob, false);
#elif XNU_12377_121_6
    (void)blob;
    const uintptr_t anchor = (uintptr_t)amfi->OSEntitlements_invalidate;
    const uintptr_t setter = anchor + SWITCH_AMFI_IDENTITY_DELTA;
    if (!SWITCH_ENTITLEMENTS_ENTRY(setter, switch_amfi_identity_words)) return NULL;
    void *object = ((void *(*)(const void *, size_t))factory)(der, size);
    if (object) ((void (*)(void *, const char *))setter)(object, identity);
    return object;
#endif
}
