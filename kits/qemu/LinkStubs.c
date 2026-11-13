// SPDX-License-Identifier: BSD-2-Clause-Patent
// Definitions of hooks that the patched XNU and the linked drivers reference from components the qemu defconfigs
// leave out. The kernel link needs each symbol even where its caller never runs or treats it as absent. They are weak:
// a linked component that defines a hook (qemu_root_defconfig's display archive defines the first one) replaces it.

#include <stdint.h>

#define STUB __attribute__((weak))

// Every hook below keeps its caller's exact signature and external linkage: the kernel link resolves those callers here.
// NOLINTBEGIN(misc-use-internal-linkage, readability-non-const-parameter)

// offline-component-load-probes.patch, bsd_init: called whether or not NativeLegacyDisplay is linked.
void native_legacy_display_probe(void);

STUB void native_legacy_display_probe(void)
{}

// The display archive (qemu_root_defconfig) shares the frame buffer with sheng's boot progress screen, which this
// board does not have. Each definition behaves as the absent hook: the display may always draw, keeps no diagnostic
// mapping and reports no stages.
int boot_progress_prepare_scanout(void);

STUB int boot_progress_prepare_scanout(void)
{
    return 1;
}

void boot_progress_cancel_scanout(void);

STUB void boot_progress_cancel_scanout(void)
{}

void boot_progress_stage(const char *message);

STUB void boot_progress_stage(const char *message)
{
    (void)message;
}

// The extension rows of sheng's diagnostic page, where KeyStoreProbe.cpp writes its AppleKeyStore counters.
STUB char boot_progress_ext[8][48];

int boot_progress_pulse_anchor(uintptr_t address, uint64_t bytes, unsigned width, unsigned height, unsigned row);

STUB int boot_progress_pulse_anchor(uintptr_t address, uint64_t bytes, unsigned width, unsigned height, unsigned row)
{
    (void)address;
    (void)bytes;
    (void)width;
    (void)height;
    (void)row;
    return 0;
}

// Called weakly and answered as the absent component does: the scheduler's render-dependency trace (TegraGM20B: nothing
// traced). The component that defines it replaces this.
void switch_sched_render_dependency(int entering);

STUB void switch_sched_render_dependency(int entering)
{
    (void)entering;
}

// NOLINTEND(misc-use-internal-linkage, readability-non-const-parameter)
