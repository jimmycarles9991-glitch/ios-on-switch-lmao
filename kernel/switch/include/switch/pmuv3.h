// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ARMv8.0 PMUv3 (Cortex-A57): PMCCNTR_EL0 (64-bit, slot 0) plus six 32-bit
 * programmable counters PMEVCNTR0..5_EL0 (slot 1 fixed to INST_RETIRED, slots
 * 2..6 configurable).
 */
#define SWITCH_PMUV3_EVENT_CTRS         6U
#define SWITCH_PMUV3_FIXED_COUNT        2U
#define SWITCH_PMUV3_CONFIGURABLE_COUNT 5U
#define SWITCH_PMUV3_PMC_COUNT          (SWITCH_PMUV3_FIXED_COUNT + SWITCH_PMUV3_CONFIGURABLE_COUNT)

#define SWITCH_PMUV3_SLOT_CYCLES 0U
#define SWITCH_PMUV3_SLOT_INSTRS 1U

#define SWITCH_PMUV3_HW_CYCLE_BIT  31U
#define SWITCH_PMUV3_FIXED_HW_MASK ((UINT32_C(1) << SWITCH_PMUV3_HW_CYCLE_BIT) | UINT32_C(1))
#define SWITCH_PMUV3_EVENT_HW_MASK ((UINT32_C(1) << SWITCH_PMUV3_EVENT_CTRS) - UINT32_C(1))
#define SWITCH_PMUV3_ALL_HW_MASK   ((UINT32_C(1) << SWITCH_PMUV3_HW_CYCLE_BIT) | SWITCH_PMUV3_EVENT_HW_MASK)

#define SWITCH_PMUV3_EVENT_INST_RETIRED UINT16_C(0x0008)
#define SWITCH_PMUV3_EVENT_MASK         UINT32_C(0x03FF)

/*
 * Free-running 32-bit event counters reload to 2^31 rather than 0 so an
 * overflow wrap to a small value is always < prev even if interrupts were
 * masked across the wrap boundary.
 */
#define SWITCH_PMUV3_IDLE_RELOAD_32 UINT64_C(0x80000000)
#define SWITCH_PMUV3_MAX_PERIOD_32  UINT64_C(0x80000000)

/* PMCR_EL0 control bits (ARM DDI 0487, ARMv8.0). LC=1 is required for 64-bit PMCCNTR_EL0 overflow. */
#define SWITCH_PMCR_E        (UINT32_C(1) << 0)
#define SWITCH_PMCR_P        (UINT32_C(1) << 1)
#define SWITCH_PMCR_C        (UINT32_C(1) << 2)
#define SWITCH_PMCR_D        (UINT32_C(1) << 3)
#define SWITCH_PMCR_X        (UINT32_C(1) << 4)
#define SWITCH_PMCR_DP       (UINT32_C(1) << 5)
#define SWITCH_PMCR_LC       (UINT32_C(1) << 6)
#define SWITCH_PMCR_N_SHIFT  11U
#define SWITCH_PMCR_N_MASK   UINT32_C(0x1F)
#define SWITCH_PMCR_DISABLED SWITCH_PMCR_LC
#define SWITCH_PMCR_ENABLED  (SWITCH_PMCR_LC | SWITCH_PMCR_E)

/*
 * PMEVTYPER<n>_EL0 / PMCCFILTR_EL0 Non-Secure EL1/EL0 filter bits:
 * P=1 excludes EL1, U=1 excludes EL0, NSK/NSU=0 follow P/U, NSH=0 excludes EL2.
 */
#define SWITCH_PMEVTYPER_P      (UINT32_C(1) << 31)
#define SWITCH_PMEVTYPER_U      (UINT32_C(1) << 30)
#define SWITCH_PMEVTYPER_NSK    (UINT32_C(1) << 29)
#define SWITCH_PMEVTYPER_NSU    (UINT32_C(1) << 28)
#define SWITCH_PMEVTYPER_NSH    (UINT32_C(1) << 27)
#define SWITCH_PMEVTYPER_M      (UINT32_C(1) << 26)
#define SWITCH_PMCCFILTR_ALL_NS UINT32_C(0)

/* Tegra210 GICv2 per-CPU PMU overflow SPIs (INT_CPU0..3_PMU_INTR = SPI 144..147, level-high). */
#define SWITCH_PMUV3_MAX_CPUS   4U
#define SWITCH_PMUV3_SPI_BASE   144U
#define SWITCH_PMUV3_INTID_BASE (32U + SWITCH_PMUV3_SPI_BASE)

struct switch_pmuv3_snapshot {
    uint32_t pmcr;
    uint32_t pmcnten;
    uint32_t pminten;
    uint32_t pmccfiltr;
    uint32_t pmevtyper[SWITCH_PMUV3_EVENT_CTRS];
    uint64_t pmccntr;
    uint32_t pmevcntr[SWITCH_PMUV3_EVENT_CTRS];
};

static inline uint32_t switch_pmuv3_evtyper(uint16_t event, bool count_user, bool count_kernel)
{
    uint32_t value = (uint32_t)event & SWITCH_PMUV3_EVENT_MASK;
    if (!count_kernel) {
        value |= SWITCH_PMEVTYPER_P;
    }
    if (!count_user) {
        value |= SWITCH_PMEVTYPER_U;
    }
    return value;
}

static inline uint16_t switch_pmuv3_evtyper_event(uint32_t typer)
{
    return (uint16_t)(typer & SWITCH_PMUV3_EVENT_MASK);
}

static inline bool switch_pmuv3_evtyper_counts_user(uint32_t typer)
{
    const bool u = (typer & SWITCH_PMEVTYPER_U) != 0U;
    const bool nsu = (typer & SWITCH_PMEVTYPER_NSU) != 0U;
    return u == nsu;
}

static inline bool switch_pmuv3_evtyper_counts_kernel(uint32_t typer)
{
    const bool p = (typer & SWITCH_PMEVTYPER_P) != 0U;
    const bool nsk = (typer & SWITCH_PMEVTYPER_NSK) != 0U;
    return p == nsk;
}

static inline bool switch_pmuv3_is_64bit(unsigned int slot)
{
    return slot == SWITCH_PMUV3_SLOT_CYCLES;
}

static inline uint64_t switch_pmuv3_max_value(unsigned int slot)
{
    return switch_pmuv3_is_64bit(slot) ? UINT64_MAX : (uint64_t)UINT32_MAX;
}

static inline uint64_t switch_pmuv3_mask_value(unsigned int slot, uint64_t value)
{
    return switch_pmuv3_is_64bit(slot) ? value : (value & (uint64_t)UINT32_MAX);
}

static inline uint64_t switch_pmuv3_idle_reload(unsigned int slot)
{
    return switch_pmuv3_is_64bit(slot) ? UINT64_C(0) : SWITCH_PMUV3_IDLE_RELOAD_32;
}

static inline uint64_t switch_pmuv3_reload_value(unsigned int slot, uint64_t period)
{
    if (switch_pmuv3_is_64bit(slot)) {
        return period == 0U ? UINT64_C(0) : (UINT64_C(0) - period);
    }
    if (period == 0U || period >= SWITCH_PMUV3_MAX_PERIOD_32) {
        return SWITCH_PMUV3_IDLE_RELOAD_32;
    }
    return (UINT64_C(0x100000000) - period) & (uint64_t)UINT32_MAX;
}

static inline uint64_t switch_pmuv3_delta(unsigned int slot, uint64_t prev, uint64_t cur)
{
    if (switch_pmuv3_is_64bit(slot)) {
        return cur - prev;
    }
    return (uint64_t)(uint32_t)((uint32_t)cur - (uint32_t)prev);
}

static inline uint32_t switch_pmuv3_slot_hw_bit(unsigned int slot)
{
    if (slot == SWITCH_PMUV3_SLOT_CYCLES) {
        return SWITCH_PMUV3_HW_CYCLE_BIT;
    }
    if (slot < SWITCH_PMUV3_PMC_COUNT) {
        return slot - 1U;
    }
    return UINT32_MAX;
}

static inline uint32_t switch_pmuv3_slot_hw_mask(unsigned int slot)
{
    const uint32_t bit = switch_pmuv3_slot_hw_bit(slot);
    return bit < 32U ? (UINT32_C(1) << bit) : UINT32_C(0);
}

static inline uint32_t switch_pmuv3_slot_mask_to_hw(uint32_t slot_mask)
{
    uint32_t hw_mask = 0U;
    if ((slot_mask & (UINT32_C(1) << SWITCH_PMUV3_SLOT_CYCLES)) != 0U) {
        hw_mask |= UINT32_C(1) << SWITCH_PMUV3_HW_CYCLE_BIT;
    }
    hw_mask |= (slot_mask >> 1U) & SWITCH_PMUV3_EVENT_HW_MASK;
    return hw_mask;
}

static inline uint32_t switch_pmuv3_hw_mask_to_slots(uint32_t hw_mask)
{
    uint32_t slot_mask = 0U;
    if ((hw_mask & (UINT32_C(1) << SWITCH_PMUV3_HW_CYCLE_BIT)) != 0U) {
        slot_mask |= UINT32_C(1) << SWITCH_PMUV3_SLOT_CYCLES;
    }
    slot_mask |= (hw_mask & SWITCH_PMUV3_EVENT_HW_MASK) << 1U;
    return slot_mask;
}

static inline uint32_t switch_pmuv3_cpu_intid(uint32_t cpu)
{
    return cpu < SWITCH_PMUV3_MAX_CPUS ? (SWITCH_PMUV3_INTID_BASE + cpu) : UINT32_C(0);
}

static inline int32_t switch_pmuv3_intid_to_cpu(uint32_t intid)
{
    if (intid < SWITCH_PMUV3_INTID_BASE || intid >= SWITCH_PMUV3_INTID_BASE + SWITCH_PMUV3_MAX_CPUS) {
        return -1;
    }
    return (int32_t)(intid - SWITCH_PMUV3_INTID_BASE);
}

static inline void switch_pmuv3_snapshot_init(struct switch_pmuv3_snapshot *snap)
{
    snap->pmcr = SWITCH_PMCR_ENABLED;
    snap->pmcnten = SWITCH_PMUV3_FIXED_HW_MASK;
    snap->pminten = SWITCH_PMUV3_FIXED_HW_MASK;
    snap->pmccfiltr = SWITCH_PMCCFILTR_ALL_NS;
    snap->pmccntr = UINT64_C(0);
    for (unsigned int i = 0U; i < SWITCH_PMUV3_EVENT_CTRS; i++) {
        snap->pmevtyper[i] = 0U;
        snap->pmevcntr[i] = (uint32_t)SWITCH_PMUV3_IDLE_RELOAD_32;
    }
    snap->pmevtyper[0] = switch_pmuv3_evtyper(SWITCH_PMUV3_EVENT_INST_RETIRED, true, true);
}

#if defined(__arm64__) && defined(KERNEL)

static inline uint32_t switch_pmuv3_read_pmcr(void)
{
    return (uint32_t)__builtin_arm_rsr64("PMCR_EL0");
}

static inline void switch_pmuv3_write_pmcr(uint32_t value)
{
    __builtin_arm_wsr64("PMCR_EL0", (uint64_t)value);
}

static inline void switch_pmuv3_write_pmuserenr(uint32_t value)
{
    __builtin_arm_wsr64("PMUSERENR_EL0", (uint64_t)value);
}

static inline uint32_t switch_pmuv3_read_pmcnten(void)
{
    return (uint32_t)__builtin_arm_rsr64("PMCNTENSET_EL0");
}

static inline void switch_pmuv3_write_pmcntenset(uint32_t mask)
{
    __builtin_arm_wsr64("PMCNTENSET_EL0", (uint64_t)mask);
}

static inline void switch_pmuv3_write_pmcntenclr(uint32_t mask)
{
    __builtin_arm_wsr64("PMCNTENCLR_EL0", (uint64_t)mask);
}

static inline uint32_t switch_pmuv3_read_pminten(void)
{
    return (uint32_t)__builtin_arm_rsr64("PMINTENSET_EL1");
}

static inline void switch_pmuv3_write_pmintenset(uint32_t mask)
{
    __builtin_arm_wsr64("PMINTENSET_EL1", (uint64_t)mask);
}

static inline void switch_pmuv3_write_pmintenclr(uint32_t mask)
{
    __builtin_arm_wsr64("PMINTENCLR_EL1", (uint64_t)mask);
}

static inline uint32_t switch_pmuv3_read_pmovs(void)
{
    return (uint32_t)__builtin_arm_rsr64("PMOVSCLR_EL0");
}

static inline void switch_pmuv3_clear_pmovs(uint32_t mask)
{
    __builtin_arm_wsr64("PMOVSCLR_EL0", (uint64_t)mask);
}

static inline uint32_t switch_pmuv3_read_pmccfiltr(void)
{
    return (uint32_t)__builtin_arm_rsr64("PMCCFILTR_EL0");
}

static inline void switch_pmuv3_write_pmccfiltr(uint32_t value)
{
    __builtin_arm_wsr64("PMCCFILTR_EL0", (uint64_t)value);
}

static inline uint64_t switch_pmuv3_read_pmccntr(void)
{
    return __builtin_arm_rsr64("PMCCNTR_EL0");
}

static inline void switch_pmuv3_write_pmccntr(uint64_t value)
{
    __builtin_arm_wsr64("PMCCNTR_EL0", value);
}

static inline uint32_t switch_pmuv3_read_evcntr(unsigned int idx)
{
    switch (idx) {
    case 0:
        return (uint32_t)__builtin_arm_rsr64("PMEVCNTR0_EL0");
    case 1:
        return (uint32_t)__builtin_arm_rsr64("PMEVCNTR1_EL0");
    case 2:
        return (uint32_t)__builtin_arm_rsr64("PMEVCNTR2_EL0");
    case 3:
        return (uint32_t)__builtin_arm_rsr64("PMEVCNTR3_EL0");
    case 4:
        return (uint32_t)__builtin_arm_rsr64("PMEVCNTR4_EL0");
    case 5:
        return (uint32_t)__builtin_arm_rsr64("PMEVCNTR5_EL0");
    default:
        return 0U;
    }
}

static inline void switch_pmuv3_write_evcntr(unsigned int idx, uint32_t value)
{
    switch (idx) {
    case 0:
        __builtin_arm_wsr64("PMEVCNTR0_EL0", (uint64_t)value);
        break;
    case 1:
        __builtin_arm_wsr64("PMEVCNTR1_EL0", (uint64_t)value);
        break;
    case 2:
        __builtin_arm_wsr64("PMEVCNTR2_EL0", (uint64_t)value);
        break;
    case 3:
        __builtin_arm_wsr64("PMEVCNTR3_EL0", (uint64_t)value);
        break;
    case 4:
        __builtin_arm_wsr64("PMEVCNTR4_EL0", (uint64_t)value);
        break;
    case 5:
        __builtin_arm_wsr64("PMEVCNTR5_EL0", (uint64_t)value);
        break;
    default:
        break;
    }
}

static inline uint32_t switch_pmuv3_read_evtyper(unsigned int idx)
{
    switch (idx) {
    case 0:
        return (uint32_t)__builtin_arm_rsr64("PMEVTYPER0_EL0");
    case 1:
        return (uint32_t)__builtin_arm_rsr64("PMEVTYPER1_EL0");
    case 2:
        return (uint32_t)__builtin_arm_rsr64("PMEVTYPER2_EL0");
    case 3:
        return (uint32_t)__builtin_arm_rsr64("PMEVTYPER3_EL0");
    case 4:
        return (uint32_t)__builtin_arm_rsr64("PMEVTYPER4_EL0");
    case 5:
        return (uint32_t)__builtin_arm_rsr64("PMEVTYPER5_EL0");
    default:
        return 0U;
    }
}

static inline void switch_pmuv3_write_evtyper(unsigned int idx, uint32_t value)
{
    switch (idx) {
    case 0:
        __builtin_arm_wsr64("PMEVTYPER0_EL0", (uint64_t)value);
        break;
    case 1:
        __builtin_arm_wsr64("PMEVTYPER1_EL0", (uint64_t)value);
        break;
    case 2:
        __builtin_arm_wsr64("PMEVTYPER2_EL0", (uint64_t)value);
        break;
    case 3:
        __builtin_arm_wsr64("PMEVTYPER3_EL0", (uint64_t)value);
        break;
    case 4:
        __builtin_arm_wsr64("PMEVTYPER4_EL0", (uint64_t)value);
        break;
    case 5:
        __builtin_arm_wsr64("PMEVTYPER5_EL0", (uint64_t)value);
        break;
    default:
        break;
    }
}

static inline uint64_t switch_pmuv3_read_slot(unsigned int slot)
{
    if (slot == SWITCH_PMUV3_SLOT_CYCLES) {
        return switch_pmuv3_read_pmccntr();
    }
    if (slot < SWITCH_PMUV3_PMC_COUNT) {
        return (uint64_t)switch_pmuv3_read_evcntr(slot - 1U);
    }
    return UINT64_C(0);
}

static inline void switch_pmuv3_write_slot(unsigned int slot, uint64_t value)
{
    if (slot == SWITCH_PMUV3_SLOT_CYCLES) {
        switch_pmuv3_write_pmccntr(value);
    } else if (slot < SWITCH_PMUV3_PMC_COUNT) {
        switch_pmuv3_write_evcntr(slot - 1U, (uint32_t)value);
    }
}

static inline void switch_pmuv3_stop(void)
{
    switch_pmuv3_write_pmcr(SWITCH_PMCR_DISABLED);
    __builtin_arm_isb(0xf);
}

static inline void switch_pmuv3_start(void)
{
    switch_pmuv3_write_pmcr(SWITCH_PMCR_ENABLED);
    __builtin_arm_isb(0xf);
}

static inline void switch_pmuv3_quiesce_hw(void)
{
    switch_pmuv3_write_pmcr(SWITCH_PMCR_DISABLED);
    switch_pmuv3_write_pmuserenr(0U);
    switch_pmuv3_write_pmcntenclr(SWITCH_PMUV3_ALL_HW_MASK);
    switch_pmuv3_write_pmintenclr(SWITCH_PMUV3_ALL_HW_MASK);
    switch_pmuv3_clear_pmovs(SWITCH_PMUV3_ALL_HW_MASK);
    __builtin_arm_isb(0xf);
}

static inline void switch_pmuv3_configure_fixed_filters(void)
{
    switch_pmuv3_write_pmuserenr(0U);
    switch_pmuv3_write_pmccfiltr(SWITCH_PMCCFILTR_ALL_NS);
    switch_pmuv3_write_evtyper(0U, switch_pmuv3_evtyper(SWITCH_PMUV3_EVENT_INST_RETIRED, true, true));
}

static inline void switch_pmuv3_snapshot_save_hw(struct switch_pmuv3_snapshot *snap)
{
    snap->pmcr = switch_pmuv3_read_pmcr();
    snap->pmcnten = switch_pmuv3_read_pmcnten() & SWITCH_PMUV3_ALL_HW_MASK;
    snap->pminten = switch_pmuv3_read_pminten() & SWITCH_PMUV3_ALL_HW_MASK;
    switch_pmuv3_stop();
    snap->pmccfiltr = switch_pmuv3_read_pmccfiltr();
    snap->pmccntr = switch_pmuv3_read_pmccntr();
    for (unsigned int i = 0U; i < SWITCH_PMUV3_EVENT_CTRS; i++) {
        snap->pmevtyper[i] = switch_pmuv3_read_evtyper(i);
        snap->pmevcntr[i] = switch_pmuv3_read_evcntr(i);
    }
}

static inline void switch_pmuv3_snapshot_restore_hw(const struct switch_pmuv3_snapshot *snap)
{
    switch_pmuv3_quiesce_hw();
    switch_pmuv3_write_pmccfiltr(snap->pmccfiltr);
    switch_pmuv3_write_pmccntr(snap->pmccntr);
    for (unsigned int i = 0U; i < SWITCH_PMUV3_EVENT_CTRS; i++) {
        switch_pmuv3_write_evtyper(i, snap->pmevtyper[i]);
        switch_pmuv3_write_evcntr(i, snap->pmevcntr[i]);
    }
    switch_pmuv3_clear_pmovs(SWITCH_PMUV3_ALL_HW_MASK);
    __builtin_arm_isb(0xf);
    switch_pmuv3_write_pmintenset(snap->pminten & SWITCH_PMUV3_ALL_HW_MASK);
    switch_pmuv3_write_pmcntenset(snap->pmcnten & SWITCH_PMUV3_ALL_HW_MASK);
    switch_pmuv3_write_pmcr((snap->pmcr & SWITCH_PMCR_E) | SWITCH_PMCR_LC);
    __builtin_arm_isb(0xf);
}

#endif /* defined(__arm64__) && defined(KERNEL) */

#ifdef __cplusplus
}
#endif
