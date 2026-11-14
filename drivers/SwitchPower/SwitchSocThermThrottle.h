// SPDX-License-Identifier: BSD-2-Clause-Patent
// SOC_THERM's hardware throttle (TRM 40.6-40.7) as L4T programs it on this console (soctherm.c tegra_soctherm_throttle,
// throttrip_program, soctherm_oc_counter_init): one indicator, heavy, raised by the PLLX group's LEVEL2 monitor at the
// PLL-therm zone's "hot" trip (tegra210b01-nx-common.dtsi cpu_heavy, 94.5 C), with priority 100, CPU depth 0 % and GPU
// level NONE (tegra210b01-soc-base.dtsi throttle_heavy, which replaces T210's 85 % and HIGH). No light indicator and no
// OC throttle: the console's DT leaves oc1 and oc2 commented out. So on T210B01 no SOC_THERM indicator drives the GPU's
// thermal event inputs (NV_THERM EXT_THERM_0-2, TegraGM20BTherm.h), and the CPU's pulse skipper stays at 256/256.
//
// The GPU vector is L4T's THROT_LEVEL_TO_DEPTH thermometer code: LOW 3'b001, MED 3'b011, HIGH 3'b111 (inferred: bit n
// asserts EXT_THERM_n, so HIGH is the GPU's /8). Order as L4T's: the vectors, priority lock and global enable; then
// CAR's SUPER_CCLKG_DIVIDER bit 30 (clk.c tegra_super_cdiv_use_therm_controls), done by the caller; then the statistics
// and the LEVEL2 monitor that raises the indicator.
#ifndef SWITCH_SOCTHERM_THROTTLE_H
#define SWITCH_SOCTHERM_THROTTLE_H

#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "../../kits/SwitchMMIO/SwitchMMIO.h"
#include "SwitchSocThermWindow.h"
#include "SwitchThermalLimits.h"

#define SWITCH_SOCTHERM_LEVEL2_TSENSE    0x4cu
#define SWITCH_SOCTHERM_LEVEL2_UP_STATS  0x50u
#define SWITCH_SOCTHERM_STATS_CTL        0x94u
#define SWITCH_SOCTHERM_OC1_CFG          0x310u
#define SWITCH_SOCTHERM_OC_STRIDE        0x14u
#define SWITCH_SOCTHERM_OC1_STATS        0x3a8u
#define SWITCH_SOCTHERM_OC_STATS_CTL     0x3c4u
#define SWITCH_SOCTHERM_GLOBAL_CFG       0x400u
#define SWITCH_SOCTHERM_CPU_PSKIP_STATUS 0x418u
#define SWITCH_SOCTHERM_GPU_PSKIP_STATUS 0x41cu
#define SWITCH_SOCTHERM_PRIORITY_LOCK    0x424u
#define SWITCH_SOCTHERM_THROT_STATUS     0x428u
#define SWITCH_SOCTHERM_LITE_CPU_PSKIP   0x430u
#define SWITCH_SOCTHERM_LITE_GPU_PSKIP   0x438u
#define SWITCH_SOCTHERM_HEAVY_CPU_PSKIP  0x460u
#define SWITCH_SOCTHERM_HEAVY_CPU_RAMP   0x464u
#define SWITCH_SOCTHERM_HEAVY_GPU_PSKIP  0x468u
#define SWITCH_SOCTHERM_HEAVY_PRIORITY   0x474u
#define SWITCH_SOCTHERM_HEAVY_DELAY      0x478u
#define SWITCH_SOCTHERM_OC_ALARMS        4u
#define SWITCH_CAR_SUPER_CCLKG_DIVIDER   0x36cu
#define SWITCH_CAR_CDIV_USE_THERM        (1u << 30)

#define SWITCH_SOCTHERM_PSKIP_ENB        (1u << 31)
#define SWITCH_SOCTHERM_PSKIP_DIVIDEND   (0xffu << 8)
#define SWITCH_SOCTHERM_PSKIP_DIVISOR    0xffu
#define SWITCH_SOCTHERM_PSKIP_GPU_VECTOR (0x7u << 16)
#define SWITCH_SOCTHERM_RAMP_DURATION    (0xffffu << 8)
#define SWITCH_SOCTHERM_RAMP_STEP        0xffu
#define SWITCH_SOCTHERM_LVL_CPU_THROT    (0x3u << 5)
#define SWITCH_SOCTHERM_LVL_GPU_THROT    (0x3u << 3)
#define SWITCH_SOCTHERM_LVL_HEAVY        2u
#define SWITCH_SOCTHERM_GLOBAL_ENB       1u
#define SWITCH_SOCTHERM_STATS_ALL        0xfu
#define SWITCH_SOCTHERM_OC_STATS_ENB_ALL 1u
#define SWITCH_SOCTHERM_OC_EN_THROTTLE   1u

#define SWITCH_SOCTHERM_HEAVY_PRIORITY_L4T 100u
#define SWITCH_SOCTHERM_HEAVY_CPU_DEPTH    0u
#define SWITCH_SOCTHERM_HEAVY_GPU_LEVEL    0u

/* L4T's THROT_DEPTH_DIVIDEND: the skipper's dividend over a divisor of 255 for `depth` percent off. */
static inline uint32_t switch_soctherm_depth_dividend(uint32_t depth)
{
    return 256u * (100u - depth) / 100u - 1u;
}

/* L4T's THROT_LEVEL_TO_DEPTH: NONE 0, LOW 1, MED 2, HIGH 3 to the GPU vector. */
static inline uint32_t switch_soctherm_gpu_vector(uint32_t level)
{
    return (1u << level) - 1u;
}

static inline void switch_soctherm_throttle_vectors(volatile uint8_t *regs)
{
    switch_mmio_rmw32(regs, SWITCH_SOCTHERM_HEAVY_CPU_PSKIP,
                      SWITCH_SOCTHERM_PSKIP_ENB | SWITCH_SOCTHERM_PSKIP_DIVIDEND | SWITCH_SOCTHERM_PSKIP_DIVISOR,
                      SWITCH_SOCTHERM_PSKIP_ENB | switch_soctherm_depth_dividend(SWITCH_SOCTHERM_HEAVY_CPU_DEPTH) << 8 |
                          0xffu);
    switch_mmio_rmw32(regs, SWITCH_SOCTHERM_HEAVY_CPU_RAMP, SWITCH_SOCTHERM_RAMP_DURATION | SWITCH_SOCTHERM_RAMP_STEP,
                      0xffu << 8 | 0xfu);
    switch_mmio_rmw32(regs, SWITCH_SOCTHERM_HEAVY_GPU_PSKIP,
                      SWITCH_SOCTHERM_PSKIP_ENB | SWITCH_SOCTHERM_PSKIP_GPU_VECTOR,
                      SWITCH_SOCTHERM_PSKIP_ENB | switch_soctherm_gpu_vector(SWITCH_SOCTHERM_HEAVY_GPU_LEVEL) << 16);
    switch_mmio_write32(regs, SWITCH_SOCTHERM_HEAVY_PRIORITY, SWITCH_SOCTHERM_HEAVY_PRIORITY_L4T);
    switch_mmio_write32(regs, SWITCH_SOCTHERM_HEAVY_DELAY, 0);
    if ((switch_mmio_read32(regs, SWITCH_SOCTHERM_PRIORITY_LOCK) & 0xffu) < SWITCH_SOCTHERM_HEAVY_PRIORITY_L4T)
        switch_mmio_write32(regs, SWITCH_SOCTHERM_PRIORITY_LOCK, SWITCH_SOCTHERM_HEAVY_PRIORITY_L4T);
    switch_mmio_write32(regs, SWITCH_SOCTHERM_GLOBAL_CFG, SWITCH_SOCTHERM_GLOBAL_ENB);
}

static inline void switch_soctherm_throttle_monitors(volatile uint8_t *regs)
{
    const uint32_t thresh = switch_soctherm_thresh(SWITCH_THERMAL_HEAVY_PLLX_MILLIC);
    switch_mmio_write32(regs, SWITCH_SOCTHERM_STATS_CTL, SWITCH_SOCTHERM_STATS_ALL);
    switch_mmio_write32(regs, SWITCH_SOCTHERM_OC_STATS_CTL, SWITCH_SOCTHERM_OC_STATS_ENB_ALL);
    switch_mmio_rmw32(regs, SWITCH_SOCTHERM_LEVEL2_TSENSE,
                      SWITCH_SOCTHERM_LVL0_THRESH << SWITCH_SOCTHERM_LVL0_UP_SHIFT |
                          SWITCH_SOCTHERM_LVL0_THRESH << SWITCH_SOCTHERM_LVL0_DN_SHIFT | SWITCH_SOCTHERM_LVL_CPU_THROT |
                          SWITCH_SOCTHERM_LVL_GPU_THROT | SWITCH_SOCTHERM_LVL0_EN,
                      thresh << SWITCH_SOCTHERM_LVL0_UP_SHIFT | thresh << SWITCH_SOCTHERM_LVL0_DN_SHIFT |
                          SWITCH_SOCTHERM_LVL_HEAVY << 5 | SWITCH_SOCTHERM_LVL_HEAVY << 3 | SWITCH_SOCTHERM_LVL0_EN);
}

struct switch_soctherm_throttle_state {
    uint32_t level2, heavy_cpu, heavy_gpu, heavy_priority, lock, global, lite_cpu, lite_gpu, cpu_status, gpu_status,
        throt_status, heavy_ups, cclkg_divider;
    uint32_t oc_cfg[SWITCH_SOCTHERM_OC_ALARMS], oc_stats[SWITCH_SOCTHERM_OC_ALARMS];
};

static inline struct switch_soctherm_throttle_state switch_soctherm_throttle_read(const volatile uint8_t *regs,
                                                                                  uint32_t cclkg_divider)
{
    struct switch_soctherm_throttle_state s = {};
    s.level2 = switch_mmio_read32(regs, SWITCH_SOCTHERM_LEVEL2_TSENSE);
    s.heavy_cpu = switch_mmio_read32(regs, SWITCH_SOCTHERM_HEAVY_CPU_PSKIP);
    s.heavy_gpu = switch_mmio_read32(regs, SWITCH_SOCTHERM_HEAVY_GPU_PSKIP);
    s.heavy_priority = switch_mmio_read32(regs, SWITCH_SOCTHERM_HEAVY_PRIORITY);
    s.lock = switch_mmio_read32(regs, SWITCH_SOCTHERM_PRIORITY_LOCK);
    s.global = switch_mmio_read32(regs, SWITCH_SOCTHERM_GLOBAL_CFG);
    s.lite_cpu = switch_mmio_read32(regs, SWITCH_SOCTHERM_LITE_CPU_PSKIP);
    s.lite_gpu = switch_mmio_read32(regs, SWITCH_SOCTHERM_LITE_GPU_PSKIP);
    s.cpu_status = switch_mmio_read32(regs, SWITCH_SOCTHERM_CPU_PSKIP_STATUS);
    s.gpu_status = switch_mmio_read32(regs, SWITCH_SOCTHERM_GPU_PSKIP_STATUS);
    s.throt_status = switch_mmio_read32(regs, SWITCH_SOCTHERM_THROT_STATUS);
    s.heavy_ups = switch_mmio_read32(regs, SWITCH_SOCTHERM_LEVEL2_UP_STATS);
    s.cclkg_divider = cclkg_divider;
    for (uint32_t i = 0; i < SWITCH_SOCTHERM_OC_ALARMS; ++i) {
        s.oc_cfg[i] = switch_mmio_read32(regs, SWITCH_SOCTHERM_OC1_CFG + i * SWITCH_SOCTHERM_OC_STRIDE);
        s.oc_stats[i] = switch_mmio_read32(regs, SWITCH_SOCTHERM_OC1_STATS + i * 4u);
    }
    return s;
}

static inline const char *switch_soctherm_gpu_level_name(uint32_t pskip)
{
    if (!(pskip & SWITCH_SOCTHERM_PSKIP_ENB))
        return "off";
    switch ((pskip & SWITCH_SOCTHERM_PSKIP_GPU_VECTOR) >> 16) {
    case 0:
        return "none";
    case 1:
        return "low";
    case 3:
        return "med";
    case 7:
        return "high";
    default:
        return "invalid";
    }
}

static inline const char *switch_soctherm_heavy_requests(uint32_t level2)
{
    const bool cpu = ((level2 & SWITCH_SOCTHERM_LVL_CPU_THROT) >> 5) == SWITCH_SOCTHERM_LVL_HEAVY;
    const bool gpu = ((level2 & SWITCH_SOCTHERM_LVL_GPU_THROT) >> 3) == SWITCH_SOCTHERM_LVL_HEAVY;
    if (cpu && gpu)
        return "cpu+gpu";
    if (cpu)
        return "cpu";
    return gpu ? "gpu" : "none";
}

/* The key=value line of the boot log and kern.switch_soctherm (HW Test system.soctherm_throttle). */
static inline int switch_soctherm_throttle_format(const struct switch_soctherm_throttle_state *s, char *buf, size_t len)
{
    const uint32_t up = (s->level2 >> SWITCH_SOCTHERM_LVL0_UP_SHIFT) & SWITCH_SOCTHERM_LVL0_THRESH;
    char heavy[16] = "off";
    char cpu[16] = "off";
    char skip[16] = "off";
    uint32_t oc = 0;
    if (s->level2 & SWITCH_SOCTHERM_LVL0_EN)
        snprintf(heavy, sizeof(heavy), "%u.%u", up / 2u, (up & 1u) * 5u);
    if (s->heavy_cpu & SWITCH_SOCTHERM_PSKIP_ENB)
        snprintf(cpu, sizeof(cpu), "%u/%u", ((s->heavy_cpu >> 8) & 0xffu) + 1u, (s->heavy_cpu & 0xffu) + 1u);
    if (s->cpu_status & 1u)
        snprintf(skip, sizeof(skip), "%u/%u", (s->cpu_status >> 12) & 0xffu, (s->cpu_status >> 4) & 0xffu);
    for (uint32_t i = 0; i < SWITCH_SOCTHERM_OC_ALARMS; ++i)
        oc |= (s->oc_cfg[i] & SWITCH_SOCTHERM_OC_EN_THROTTLE) << i;
    return snprintf(buf, len,
                    "heavy_pllx_c=%s heavy_req=%s heavy_cpu=%s heavy_gpu=%s heavy_prio=%u lock=%u global=%s light=%s "
                    "oc_throttle=0x%x cdiv_therm=%s gpu_depth=%u cpu_skip=%s throt_status=0x%x heavy_ups=%u "
                    "oc_events=%u,%u,%u,%u",
                    heavy, switch_soctherm_heavy_requests(s->level2), cpu, switch_soctherm_gpu_level_name(s->heavy_gpu),
                    s->heavy_priority & 0xffu, s->lock & 0xffu, s->global & SWITCH_SOCTHERM_GLOBAL_ENB ? "on" : "off",
                    (s->lite_cpu | s->lite_gpu) & SWITCH_SOCTHERM_PSKIP_ENB ? "on" : "off", oc,
                    s->cclkg_divider & SWITCH_CAR_CDIV_USE_THERM ? "on" : "off", (s->gpu_status >> 20) & 0x7u, skip,
                    s->throt_status, s->heavy_ups, s->oc_stats[0], s->oc_stats[1], s->oc_stats[2], s->oc_stats[3]);
}

#endif
