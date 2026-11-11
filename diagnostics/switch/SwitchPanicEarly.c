// SPDX-License-Identifier: BSD-2-Clause-Patent
// A panic before the kernel's console (startup_phase before STARTUP_SUB_KPRINTF) and before SwitchPanicSave has its
// pages. A RELEASE kernel has no console and no SwitchPanicSave hooks at all, so this is all that keeps its text: it gets
// SwitchPanicSave's record (which hekate saves to SD:/L4T_panic.txt after the warm reset, and a DEVELOPMENT kernel's
// next boot files on Data) and its screen.
//
// Nothing is mapped for it yet: arm_vm_init is still on start.s's bootstrap tables or has just left them, and only the
// boot CPU runs. That CPU goes back to the bootstrap tables, whose V=P half maps the kernel window in 2 MiB blocks, and
// adds what the record needs at the physical addresses: hekate's pstore RAM and framebuffer (write-combined, as
// SwitchPanicSave maps them) and, as one 1 GiB Device block, the PMC. It never returns to the kernel.
#include <stdarg.h>
#include <stdint.h>
#include <arm64/proc_reg.h>
#include <kern/startup.h>
#include <pexpert/arm64/boot.h>
#include <pexpert/pexpert.h>
#include "SwitchDiagDisplay.h"
#include "SwitchPanicSave.h"

extern uint8_t bootstrap_pagetables[];
extern int vsnprintf(char *, size_t, const char *, va_list);

#define PSTORE_PA UINT64_C(0xb0000000)
#define MMIO_PA   UINT64_C(0x40000000) /* the 1 GiB holding the PMC */
#define PMC_PA    UINT64_C(0x7000e000)
#define L1_SHIFT  30
#define L2_SHIFT  21

static uint64_t block(uint64_t pa, uint64_t attribute)
{
    const uint64_t switch_attributes = ARM_TTE_BLOCK_ATTRINDX(attribute);
    return pa | ARM_TTE_VALID | ARM_TTE_TYPE_BLOCK | ARM_TTE_BLOCK_AF | ARM_TTE_BLOCK_PNX | ARM_TTE_BLOCK_NX |
           ARM_TTE_BLOCK_SH(SH_OUTER_MEMORY) | switch_attributes;
}

static void flush_tlb(void)
{
    __asm__ volatile("dsb ish; tlbi vmalle1; dsb ish; isb" ::: "memory");
}

/* The L2 table of the bootstrap V=P L1 entry covering pa, or NULL: valid once TTBR0 holds the bootstrap root. */
static volatile uint64_t *v_p_l2(const volatile uint64_t *l1, uint64_t pa)
{
    const uint64_t entry = l1[pa >> L1_SHIFT];
    if ((entry & (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE)) != (ARM_TTE_VALID | ARM_TTE_TYPE_TABLE))
        return NULL;
    return (volatile uint64_t *)(uintptr_t)(entry & ARM_TTE_TABLE_MASK);
}

/* TTBR0 back on the bootstrap tables with the pstore RAM in them, and the framebuffer and MMIO when they fit: whether the
 * pstore RAM is mapped. */
static int map_bootstrap(int *framebuffer, int *mmio)
{
    const boot_args *args = (const boot_args *)PE_state.bootArgs;
    if (!args)
        return 0;
    const uint64_t root = (uint64_t)(uintptr_t)bootstrap_pagetables - args->virtBase + args->physBase;
    __asm__ volatile("msr ttbr0_el1, %0" ::"r"(root) : "memory");
    flush_tlb();
    volatile uint64_t *l1 = (volatile uint64_t *)(uintptr_t)root;
    volatile uint64_t *low = v_p_l2(l1, PSTORE_PA);
    if (!low)
        return 0;
    low[(PSTORE_PA >> L2_SHIFT) & 511] = block(PSTORE_PA, CACHE_ATTRINDX_WRITECOMB);
    volatile uint64_t *high = v_p_l2(l1, SWITCH_FB_PA);
    /* hekate's framebuffer in either of the loader's modes: the depth in the low byte, the rotation above it. */
    const Boot_Video *video = &args->Video;
    *framebuffer = high && video->v_baseAddr == SWITCH_FB_PA && video->v_rowBytes == 2880 && video->v_width == 720 &&
                   video->v_height == 1280 && (video->v_depth & 0xff) == 32;
    if (*framebuffer) {
        for (uint64_t pa = SWITCH_FB_PA; pa < SWITCH_FB_PA + (UINT64_C(2) << L2_SHIFT); pa += UINT64_C(1) << L2_SHIFT)
            high[(pa >> L2_SHIFT) & 511] = block(pa, CACHE_ATTRINDX_WRITECOMB);
    }
    const uint64_t devices = block(MMIO_PA, CACHE_ATTRINDX_DISABLE);
    *mmio = !l1[MMIO_PA >> L1_SHIFT] || l1[MMIO_PA >> L1_SHIFT] == devices;
    if (*mmio)
        l1[MMIO_PA >> L1_SHIFT] = devices;
    flush_tlb();
    return 1;
}

void switch_panic_early(const char *format, va_list *args, unsigned long caller)
{
    static uint32_t entered;
    if (startup_phase >= STARTUP_SUB_KPRINTF || switch_panic_save_ready() ||
        __atomic_exchange_n(&entered, 1u, __ATOMIC_ACQ_REL))
        return;
    static char line[1024];
    /* NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_copy initializes it */
    va_list copy;
    va_copy(copy, *args);
    if (vsnprintf(line, sizeof(line), format, copy) < 0)
        line[0] = 0;
    va_end(copy);
    uint64_t kernel_ttbr0 = 0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(kernel_ttbr0));
    int framebuffer = 0;
    int mmio = 0;
    if (!map_bootstrap(&framebuffer, &mmio) || !mmio) {
        __asm__ volatile("msr ttbr0_el1, %0" ::"r"(kernel_ttbr0) : "memory");
        flush_tlb();
        return;
    }
    switch_panic_save_early(line, caller, (volatile uint8_t *)(uintptr_t)PSTORE_PA,
                            framebuffer ? (volatile uint32_t *)(uintptr_t)SWITCH_FB_PA : NULL,
                            (volatile uint32_t *)(uintptr_t)PMC_PA);
}

#if DEVELOPMENT || DEBUG
/* switch-early-panic=<where>: 1 on start.s's bootstrap tables, 2 once arm_vm_init left them. Not while the last boot's
 * record is still in the pstore RAM: the boot after the warm reset then gets as far as filing it. */
void switch_panic_early_test(unsigned int where)
{
    unsigned int armed = 0;
    if (!PE_parse_boot_argn("switch-early-panic", &armed, sizeof(armed)) || armed != where)
        return;
    uint64_t kernel_ttbr0 = 0;
    __asm__ volatile("mrs %0, ttbr0_el1" : "=r"(kernel_ttbr0));
    int framebuffer = 0;
    int mmio = 0;
    const int pending =
        map_bootstrap(&framebuffer, &mmio) && switch_panic_save_early_pending((volatile uint8_t *)(uintptr_t)PSTORE_PA);
    __asm__ volatile("msr ttbr0_el1, %0" ::"r"(kernel_ttbr0) : "memory");
    flush_tlb();
    if (!pending)
        panic("switch-early-panic=%u: a deliberate panic in arm_vm_init", where);
}
#endif
