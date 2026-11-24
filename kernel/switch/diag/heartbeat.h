// SPDX-License-Identifier: GPL-2.0-only
/*
 * The freeze heartbeat (heartbeat.c): its timing, the timer block words it writes and which class a stall is, free of
 * kernel types so kernel/tests can drive them. Times are nanoseconds of uptime.
 */
#ifndef SWITCH_DIAG_HEARTBEAT_H
#define SWITCH_DIAG_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_HEARTBEAT_BEAT_S   4u
#define SWITCH_HEARTBEAT_LEEWAY_S 2u
/* The WDT acts on consecutive expiries of its period: the interrupt at the first, PMC2CAR's reset at the fourth. START
 * reloads the period but not TMR6, which keeps running, so the first expiry comes up to one tick early. */
#define SWITCH_HEARTBEAT_TICK_US  250000u
#define SWITCH_HEARTBEAT_PERIOD   48u
#define SWITCH_HEARTBEAT_STALL_MS (SWITCH_HEARTBEAT_PERIOD * SWITCH_HEARTBEAT_TICK_US / 1000u)
#define SWITCH_HEARTBEAT_RESET_MS (4u * SWITCH_HEARTBEAT_STALL_MS)
#define SWITCH_HEARTBEAT_LATE_MS  (SWITCH_HEARTBEAT_STALL_MS - SWITCH_HEARTBEAT_TICK_US / 1000u)
#define SWITCH_HEARTBEAT_LATE_NS  ((uint64_t)SWITCH_HEARTBEAT_LATE_MS * 1000000u)
#define SWITCH_HEARTBEAT_CORES    8u

/* WDT4 is the one watchdog tied to no CPU core: resetting CPUn resets WDTn (TRM 8.4, 8.6), and every core powers down
 * in C7 here (whether C7 counts as that reset is not established). TRM 41.8.1 gives WDT4 to the BPMP firmware and says
 * the CPU should not touch it. The bpmpfw_b01.bin this console runs references no watchdog register, never builds the
 * 0xc45a unlock and registers no handler for COP_WD (SPI 124), WDT4's first expiry: a scan of the file, not a proof,
 * so the first beat that finds WDT4 not running logs it. */
#define SWITCH_HEARTBEAT_TMR_PA        UINT64_C(0x60005000)
#define SWITCH_HEARTBEAT_TMR6_PTV      0x068u
#define SWITCH_HEARTBEAT_WDT4_CFG      0x180u
#define SWITCH_HEARTBEAT_WDT4_STS      0x184u
#define SWITCH_HEARTBEAT_WDT4_CMD      0x188u
#define SWITCH_HEARTBEAT_WDT4_UNLOCK   0x18cu
#define SWITCH_HEARTBEAT_SHARED_STATUS 0x1a0u
#define SWITCH_HEARTBEAT_SHARED_WDTS   0x1fu
#define SWITCH_HEARTBEAT_CMD_START     1u
#define SWITCH_HEARTBEAT_CMD_DISABLE   2u
#define SWITCH_HEARTBEAT_UNLOCK        0xc45au
#define SWITCH_HEARTBEAT_STS_ENABLED   (1u << 0)
#define SWITCH_HEARTBEAT_STS_INT       (1u << 1)
#define SWITCH_HEARTBEAT_PTV_ENABLE    (1u << 31)

static inline uint32_t switch_heartbeat_ptv(void)
{
    return SWITCH_HEARTBEAT_PTV_ENABLE | (1u << 30) | (SWITCH_HEARTBEAT_TICK_US - 1u);
}

/* TMR6, the interrupt and PMC2CAR's reset. No FIQ, no core reset: both would go to BPMP-Lite. */
static inline uint32_t switch_heartbeat_wdt_config(void)
{
    return 6u | (SWITCH_HEARTBEAT_PERIOD << 4) | (1u << 12) | (1u << 15);
}

/* The offset of TMR0-9's PTV (TRM table 20), the watchdog sources; UINT32_MAX for any other. */
static inline uint32_t switch_heartbeat_ptv_offset(uint32_t timer)
{
    static const uint32_t offsets[10] = {0x88u, 0x00u, 0x08u, 0x50u, 0x58u, 0x60u, 0x68u, 0x70u, 0x78u, 0x80u};
    return timer < 10u ? offsets[timer] : UINT32_MAX;
}

enum switch_heartbeat_hardware {
    SWITCH_HEARTBEAT_HW_IDLE = 0,
    SWITCH_HEARTBEAT_HW_DORMANT, /* WDT4 enabled on a stopped timer: hekate's watchdog_end leaves it so */
    SWITCH_HEARTBEAT_HW_BUSY,
};

/* Before arming: TMR6 untouched, WDT4 off or counting nothing, no watchdog interrupt pending. */
static inline enum switch_heartbeat_hardware switch_heartbeat_hardware(uint32_t tmr6_ptv, uint32_t wdt4_status,
                                                                       uint32_t wdt4_source_ptv, uint32_t shared)
{
    if (tmr6_ptv != 0 || (shared & SWITCH_HEARTBEAT_SHARED_WDTS) != 0 || (wdt4_status & SWITCH_HEARTBEAT_STS_INT)) {
        return SWITCH_HEARTBEAT_HW_BUSY;
    }
    if (!(wdt4_status & SWITCH_HEARTBEAT_STS_ENABLED)) {
        return SWITCH_HEARTBEAT_HW_IDLE;
    }
    return wdt4_source_ptv & SWITCH_HEARTBEAT_PTV_ENABLE ? SWITCH_HEARTBEAT_HW_BUSY : SWITCH_HEARTBEAT_HW_DORMANT;
}

enum switch_heartbeat_core_kind {
    SWITCH_HEARTBEAT_CORE_IDLE = 0,
    SWITCH_HEARTBEAT_CORE_ALIVE,
    SWITCH_HEARTBEAT_CORE_MASKED,
};

struct switch_heartbeat_core {
    bool running;  /* PROCESSOR_RUNNING or PROCESSOR_DISPATCHING */
    uint64_t irqs; /* cpu_stat.irq_ex_cnt */
    uint64_t quiet_since;
};

/* A running core takes its quantum timer's interrupt every quantum, so one that stays running with the same IRQ count
 * has taken no interrupt since quiet_since. Waking an idle core is an interrupt too. */
static inline void switch_heartbeat_sample(struct switch_heartbeat_core *core, bool running, uint64_t irqs,
                                           uint64_t now)
{
    if (!running || !core->running || irqs != core->irqs) {
        core->quiet_since = now;
    }
    core->running = running;
    core->irqs = irqs;
}

/* A quiet_since after now is another context's later sample. */
static inline enum switch_heartbeat_core_kind switch_heartbeat_core_kind(const struct switch_heartbeat_core *core,
                                                                         uint64_t now)
{
    if (!core->running) {
        return SWITCH_HEARTBEAT_CORE_IDLE;
    }
    return core->quiet_since <= now && now - core->quiet_since >= SWITCH_HEARTBEAT_LATE_NS
               ? SWITCH_HEARTBEAT_CORE_MASKED
               : SWITCH_HEARTBEAT_CORE_ALIVE;
}

enum switch_heartbeat_class {
    SWITCH_HEARTBEAT_HEALTHY = 0,
    SWITCH_HEARTBEAT_MASKED,      /* class a */
    SWITCH_HEARTBEAT_UNSCHEDULED, /* class b */
};

/* Any WDT4 interrupt comes LATE_NS or more after the beat that restarted it. */
static inline enum switch_heartbeat_class switch_heartbeat_class(const struct switch_heartbeat_core *cores,
                                                                 unsigned count, uint64_t now, uint64_t last_beat)
{
    for (unsigned cpu = 0; cpu < count; ++cpu) {
        if (switch_heartbeat_core_kind(&cores[cpu], now) == SWITCH_HEARTBEAT_CORE_MASKED) {
            return SWITCH_HEARTBEAT_MASKED;
        }
    }
    return last_beat <= now && now - last_beat >= SWITCH_HEARTBEAT_LATE_NS ? SWITCH_HEARTBEAT_UNSCHEDULED
                                                                           : SWITCH_HEARTBEAT_HEALTHY;
}

#endif
