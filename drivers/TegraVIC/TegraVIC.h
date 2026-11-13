// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inside TegraVIC: the engine and its SMMU domain (TegraVIC.cpp), IOSurface access (TegraVICSurface.cpp) and the
// IOSurfaceAccelerator service (TegraVICUserClient.cpp).
//
// On by default; no boot-arg. The stages (TegraVIC.cpp) are V0 host1x and the kernel buffers, V1 SMMU domain, power,
// clock and falcon, V2 syncpoint proof of life by interrupt (and the FCE microcode handed to the firmware), V3 the
// AppleM2ScalerCSCDriver service published.
#ifndef TEGRA_VIC_H
#define TEGRA_VIC_H
#include <stddef.h>
#include <stdint.h>
#include "TegraVICConfig.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

class IOMemoryDescriptor;

// The periodic job and present lines and the probes (SwitchVICKernelTest.h): debug kernels only, so a release kernel
// has none of their code or strings.
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
#define TEGRA_VIC_STATS 1
#else
#define TEGRA_VIC_STATS 0
#endif

// A physically contiguous kernel buffer below 4 GiB, mapped 1:1 in VIC's address space (IOVA = PA).
struct TegraVICBuffer {
    uint64_t pa, size;
    uint8_t *va;
};

// The two windows of VIC's address space a job's surfaces are mapped into, page by page.
enum {
    TEGRA_VIC_WINDOW_SRC = 0,
    TEGRA_VIC_WINDOW_DST = 1,
    TEGRA_VIC_WINDOWS = 2
};

constexpr uint64_t kTegraVICWindowBase = 0x10000000ULL, kTegraVICWindowBytes = 128ULL << 20;

extern "C" {
// Engine (TegraVIC.cpp).
bool tegra_vic_ready(void);
const TegraVICTraits *tegra_vic_traits(void);
int tegra_vic_buffer(uint64_t size, TegraVICBuffer *out);  // from the kernel arena; never freed

// Another engine's syncpoint threshold VIC's channel waits for on the hardware before the job starts.
struct TegraVICAcquire {
    uint32_t syncpt, threshold;
};

// VIC's syncpoint, for an engine that waits for a job's fence.
uint32_t tegra_vic_syncpt(void);
uint64_t tegra_vic_config_address(void);
uint64_t tegra_vic_filter_address(void);

void tegra_vic_describe(char *buffer, size_t size);  // the falcon's state, for a failure line

// Service (TegraVICUserClient.cpp): V3.
int tegra_vic_publish(void);

// Idle power gating (TegraVIC.cpp): a submission holds the powered engine (on again first if it was off), and the
// queue draining starts the idle timer.
int switch_vic_power_use(void);
void switch_vic_power_done(void);
void switch_vic_power_idle(void);
}

struct SwitchVICPowerUse {
    SwitchVICPowerUse() : error(switch_vic_power_use())
    {}

    ~SwitchVICPowerUse()
    {
        if (!error)
            switch_vic_power_done();
    }

    SwitchVICPowerUse(const SwitchVICPowerUse &) = delete;
    SwitchVICPowerUse &operator=(const SwitchVICPowerUse &) = delete;
    const int error;
};

#if TEGRA_VIC_STATS
struct IOExternalMethodArguments;
int switch_vic_kernel_test_call(const IOExternalMethodArguments *arguments);  // an IOReturn
#endif

#endif
