// SPDX-License-Identifier: MIT
// Board-side original PMGR ABI qualification. Default off. Modes: 1 releases
// unpublished objects, 2 publishes the original nub, 3 tests power refusal.
// No clocks, sleep, reset or secondary-CPU success is advertised.
#include "OriginalApplePMGR.h"
#include "OriginalApplePMGRNub.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <libkern/c++/OSNumber.h>
#include <os/log.h>
#include "../../drivers/SwitchPower/SwitchPower.h"
#include "../../drivers/SwitchPower/SwitchSensors.h"

extern "C" {
OSReturn switch_kernel_classes(void (*initialize)());
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void sheng_pmgr_static_init(void) __asm__("__GLOBAL__sub_I_ShengPlatformPower.cpp");
// stage,status,parent-size,own-size,provider,nub,bound-provider,read-before,
// read-after,own-before,own-after,nub-before,nub-after,attached,registered,mode.
volatile uint64_t sheng_pmgr_observation[16];

__attribute__((noinline)) void sheng_pmgr_checkpoint(void)
{
    __asm__ volatile("" ::: "memory");
}
}

__attribute__((noreturn)) static void unavailable(unsigned slot)
{
    sheng_pmgr_observation[0] = 0x100 + slot;
    sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnUnsupported);
    sheng_pmgr_checkpoint();
    panic("ShengPlatformPower: unimplemented operation %u", slot);
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class ShengPlatformPower : public ApplePMGR {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(ShengPlatformPower) public : void enableCPUCore(unsigned, uint64_t) override
    {
        unavailable(168);
    }

    void enableCPUCore(unsigned) override
    {
        unavailable(169);
    }

    void disableCPUCore(unsigned) override
    {
        unavailable(170);
    }

    void enableCPUCluster(unsigned) override
    {
        unavailable(171);
    }

    void disableCPUCluster(unsigned) override
    {
        unavailable(172);
    }

    void initCPUIdle(ml_processor_info_t *) override
    {
        unavailable(173);
    }

    void enterCPUIdle(UInt64 *) override
    {
        unavailable(174);
    }

    void exitCPUIdle(UInt64 *) override
    {
        unavailable(175);
    }

    void updateCPUIdle(UInt64 *) override
    {
        unavailable(176);
    }

    SWITCH_ABI_GUARDS(unavailable)
    IOReturn setProperties(OSObject *properties) override;
};

OSDefineMetaClassWithInit(ShengPlatformPower, ApplePMGR, ) OSMetaClassConstructorInit(ShengPlatformPower, ApplePMGR, )
    OSDefineBasicStructors(ShengPlatformPower, ApplePMGR)
    // The original object's storage exceeds kalloc_type's 16 KiB size limit.
    // Use XNU's existing zeroed, size-aware OSObject heap allocation pair.
    void *ShengPlatformPower::operator new(size_t size)
{
    return OSObject::operator new(size);
}

void ShengPlatformPower::operator delete(void *memory, size_t size)
{
    OSObject::operator delete(memory, size);
}

static_assert(sizeof(ShengPlatformPower) == sizeof(ApplePMGR), "Exact original provider storage");

// iPadOS's CPU ceiling. thermalmonitord (CLTM, 21H461 -[MitigationController updateCPU], CONTRACT.md) writes its
// decisions as IORegistryEntrySetCFProperty on IOServiceMatching("ApplePMGR"), this service: four DVD factor
// contributors, AppleARMPerformanceControllerDVDFactor1-{slowloop,hip,upo,lpm}, each a 16.16 share of the CPU's top
// performance (CLTM sends value < 100 ? value * 65536 / 100 : 0x10000). On an iPad, ApplePMGR passes them to the
// performance controller; here the lowest of the four becomes the CPU governor's ceiling (switch_perf_cltm_ceiling,
// drivers/SwitchPower/SwitchPerfGovernor.cpp). Each value is also published, so ioreg shows what CLTM asks for; one
// os_log line per change of the ceiling. Other properties are refused as IOService's setProperties does.
IOReturn ShengPlatformPower::setProperties(OSObject *properties)
{
    static uint32_t fraction[SWITCH_CLTM_CONTRIBUTORS] = {0x10000u, 0x10000u, 0x10000u, 0x10000u};
    static uint32_t ceiling = 0x10000u;
    const OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
    if (!dict)
        return kIOReturnBadArgument;
    unsigned taken = 0;
    for (unsigned i = 0; i < SWITCH_CLTM_CONTRIBUTORS; ++i) {
        OSNumber *value = OSDynamicCast(OSNumber, dict->getObject(switch_cltm_keys[i]));
        if (!value)
            continue;
        const uint64_t v = value->unsigned64BitValue();
        __atomic_store_n(&fraction[i], v > 0x10000u ? 0x10000u : static_cast<uint32_t>(v), __ATOMIC_RELAXED);
        setProperty(switch_cltm_keys[i], value);
        ++taken;
    }
    if (!taken)
        return kIOReturnUnsupported;
    uint32_t now[SWITCH_CLTM_CONTRIBUTORS];
    for (unsigned i = 0; i < SWITCH_CLTM_CONTRIBUTORS; ++i)
        now[i] = __atomic_load_n(&fraction[i], __ATOMIC_RELAXED);
    const uint32_t next = switch_cltm_ceiling(now);
    if (__atomic_exchange_n(&ceiling, next, __ATOMIC_RELAXED) != next) {
        const unsigned khz = switch_perf_cltm_ceiling(next);
        os_log(OS_LOG_DEFAULT,
               "ShengPlatformPower: thermalmonitord CPU ceiling %u.%u%% (slowloop %u, hip %u, upo %u, lpm %u /65536): "
               "%s %u MHz",
               next * 1000 / 0x10000 / 10, next * 1000 / 0x10000 % 10, now[0], now[1], now[2], now[3],
               khz ? "governor top" : "CPU not governed, no effect;", khz / 1000);
    }
    return taken == dict->getCount() ? kIOReturnSuccess : kIOReturnUnsupported;
}

static const OSMetaClass *lookup(const char *text)
{
    const OSSymbol *name = OSSymbol::withCString(text);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    return meta;
}

extern "C" void sheng_pmgr_run(void)
{
    constexpr unsigned mode = 2;
    sheng_pmgr_observation[15] = mode;
    sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
    if (sheng_pmgr_observation[0]) {
        sheng_pmgr_checkpoint();
        return;
    }
    sheng_pmgr_observation[0] = 1;
    kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
    if (result) {
        sheng_pmgr_observation[1] = static_cast<uint32_t>(result);
        sheng_pmgr_checkpoint();
        return;
    }
    const OSMetaClass *parent = lookup("ApplePMGR");
    const OSMetaClass *nubMeta = lookup("ApplePMGRNub");
    if (!parent || parent != &ApplePMGR::gMetaClass || parent->getClassSize() != sizeof(ApplePMGR) || !nubMeta ||
        nubMeta->getClassSize() != sizeof(ApplePMGRNub)) {
        sheng_pmgr_checkpoint();
        return;
    }
    sheng_pmgr_observation[2] = parent->getClassSize();
    result = switch_kernel_classes(sheng_pmgr_static_init);
    if (result) {
        sheng_pmgr_observation[1] = static_cast<uint32_t>(result);
        sheng_pmgr_checkpoint();
        return;
    }
    sheng_pmgr_observation[9] = ShengPlatformPower::gMetaClass.getInstanceCount();
    sheng_pmgr_observation[11] = nubMeta->getInstanceCount();
    ShengPlatformPower *provider = new ShengPlatformPower;
    IORegistryEntry *description = new IORegistryEntry;
    ApplePMGRNub *nub = nullptr;
    do {
        if (!provider || !description || !provider->init() || !description->init())
            break;
        sheng_pmgr_observation[0] = 2;
        sheng_pmgr_observation[3] = provider->getMetaClass()->getClassSize();
        sheng_pmgr_observation[4] = reinterpret_cast<uintptr_t>(provider);
        // Empty resource description: no Apple register map or device capabilities.
        description->setName("sheng-platform-power");
        nub = sheng_original_pmgr_nub_create(description, provider);
        sheng_pmgr_observation[5] = reinterpret_cast<uintptr_t>(nub);
        if (!nub || nub->getMetaClass() != nubMeta || !provider->metaCast("ApplePMGR"))
            break;
        const uintptr_t bound = reinterpret_cast<uintptr_t>(nub->pmgr());
        sheng_pmgr_observation[6] = bound;
        if (bound != reinterpret_cast<uintptr_t>(provider))
            break;
        sheng_pmgr_observation[0] = 3;
        const uint32_t value = sheng_original_pmgr_nub_read(nub);
        sheng_pmgr_observation[7] = value;
        sheng_original_pmgr_nub_write(nub, uint64_t(value | 4));
        sheng_pmgr_observation[8] = sheng_original_pmgr_nub_read(nub);
        sheng_pmgr_observation[13] =
            static_cast<uint64_t>(nub->getProvider() != nullptr || provider->getProvider() != nullptr);
        sheng_pmgr_observation[14] = (nub->getState() | provider->getState()) & kIOServiceRegisteredState;
        if (value != 0xffffffff || sheng_pmgr_observation[8] != 0xffffffff || sheng_pmgr_observation[13] ||
            sheng_pmgr_observation[14])
            break;
        {
            IOService *platform = IOService::getPlatform();
            if (!platform || !provider->attach(platform))
                break;
            if (!provider->start(platform)) {
                provider->detach(platform);
                break;
            }
            if (!nub->attach(provider)) {
                provider->stop(platform);
                provider->detach(platform);
                break;
            }
            if (!nub->start(provider)) {
                nub->detach(provider);
                provider->stop(platform);
                provider->detach(platform);
                break;
            }
            // thermalmonitord's DVD factor writes (setProperties below): the original sandbox profiles name only the
            // original classes, as for SwitchBacklight's brightness writes (XNU native_sandbox_grant).
            provider->setProperty("NativeSandboxGrantProcess", "thermalmonitord");
            provider->registerService(kIOServiceSynchronous);
            nub->registerService(kIOServiceSynchronous);
            sheng_pmgr_observation[0] = 5;
            sheng_pmgr_observation[13] =
                static_cast<uint64_t>(nub->getProvider() == provider && provider->getProvider() == platform);
            sheng_pmgr_observation[14] = (nub->getState() & provider->getState()) & kIOServiceRegisteredState;
            auto *matching = IOService::serviceMatching("ApplePMGRNub");
            const IOService *matched = matching ? IOService::waitForMatchingService(matching, 1000000) : nullptr;
            OSSafeReleaseNULL(matching);
            // Check actual service discovery and genuine type identity, then retain
            // both live objects. The registry and original nub refer to this provider.
            if (matched != nub || matched->getMetaClass() != nubMeta || !sheng_pmgr_observation[13] ||
                !sheng_pmgr_observation[14]) {
                sheng_pmgr_checkpoint();
                panic("ShengPlatformPower: original nub publication failed");
            }
            sheng_pmgr_observation[10] = ShengPlatformPower::gMetaClass.getInstanceCount();
            sheng_pmgr_observation[12] = nubMeta->getInstanceCount();
            sheng_pmgr_observation[1] = kIOReturnSuccess;
            OSSafeReleaseNULL(matched);
            OSSafeReleaseNULL(description);
            sheng_pmgr_checkpoint();
            return;
        }
        sheng_pmgr_observation[1] = kIOReturnSuccess;
    } while (false);
    OSSafeReleaseNULL(nub);
    // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks): OSObject's release() frees what new made
    OSSafeReleaseNULL(description);
    OSSafeReleaseNULL(provider);
    sheng_pmgr_observation[10] = ShengPlatformPower::gMetaClass.getInstanceCount();
    sheng_pmgr_observation[12] = nubMeta->getInstanceCount();
    if (sheng_pmgr_observation[9] != sheng_pmgr_observation[10] ||
        sheng_pmgr_observation[11] != sheng_pmgr_observation[12])
        sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
    sheng_pmgr_observation[0] = 4;
    sheng_pmgr_checkpoint();
}
