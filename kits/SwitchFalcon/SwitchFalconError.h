// SPDX-License-Identifier: BSD-2-Clause-Patent
// A host1x engine's falcon error, as its THI raises it on the engine's own interrupt line (Tegra X1 TRM 15.4.1.4 and
// 15.4.1.7: "If there are any error/exception interrupts during the course of execution of the app (or OS) then the
// interrupt handlers raise interrupt to host with an appropriate error code and wait for the interrupt to be cleared
// before proceeding"; MAILBOX0 the kind, MAILBOX1 the code, Table 79; THI INT_STATUS/INT_MASK 15.5.8-9). Offsets are
// from the engine's base, its falcon at +0x1000 (SwitchFalcon.h).
#ifndef SWITCH_FALCON_ERROR_H
#define SWITCH_FALCON_ERROR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "../SwitchMMIO/SwitchMMIO.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWITCH_FALCON_THI_INT_STATUS = 0x78,
    SWITCH_FALCON_THI_INT_MASK = 0x7c,
    SWITCH_FALCON_THI_FALCON_INT = 1u << 0,
    SWITCH_FALCON_ERR_IRQSCLR = 0x1004,
    SWITCH_FALCON_ERR_IRQSTAT = 0x1008,
    SWITCH_FALCON_ERR_IRQMASK = 0x1018,
    SWITCH_FALCON_ERR_IRQDEST = 0x101c,
    SWITCH_FALCON_ERR_MAILBOX0 = 0x1040,
    SWITCH_FALCON_ERR_MAILBOX1 = 0x1044,
    SWITCH_FALCON_ERR_EXCI = 0x10d0,
    SWITCH_FALCON_ERR_CPUCTL = 0x1100,
};

typedef struct switch_falcon_error {
    uint32_t irqstat, irqmask, irqdest, mailbox0, mailbox1, exci, cpuctl;
} switch_falcon_error;

static inline bool switch_falcon_error_pending(const volatile uint8_t *engine)
{
    return (switch_mmio_read32(engine, SWITCH_FALCON_THI_INT_STATUS) & SWITCH_FALCON_THI_FALCON_INT) != 0;
}

// The interrupt's filter, interrupts off: reads only. The falcon stays parked on its error until the cancellation
// clears it and resets the engine, so it cannot go on to finish the job as if it had succeeded. false: no falcon
// interrupt is pending.
static inline bool switch_falcon_error_read(const volatile uint8_t *engine, switch_falcon_error *out)
{
    if (!switch_falcon_error_pending(engine))
        return false;
    out->irqstat = switch_mmio_read32(engine, SWITCH_FALCON_ERR_IRQSTAT);
    out->irqmask = switch_mmio_read32(engine, SWITCH_FALCON_ERR_IRQMASK);
    out->irqdest = switch_mmio_read32(engine, SWITCH_FALCON_ERR_IRQDEST);
    out->mailbox0 = switch_mmio_read32(engine, SWITCH_FALCON_ERR_MAILBOX0);
    out->mailbox1 = switch_mmio_read32(engine, SWITCH_FALCON_ERR_MAILBOX1);
    out->exci = switch_mmio_read32(engine, SWITCH_FALCON_ERR_EXCI);
    out->cpuctl = switch_mmio_read32(engine, SWITCH_FALCON_ERR_CPUCTL);
    return true;
}

// L4T nvhost_flcn_common_isr's clear, in thread context before the engine's reset: THI's FALCON_INT written back and
// read, then the falcon's host-routed pending sources cleared.
static inline void switch_falcon_error_clear(volatile uint8_t *engine)
{
    switch_mmio_write32(engine, SWITCH_FALCON_THI_INT_STATUS, SWITCH_FALCON_THI_FALCON_INT);
    (void)switch_mmio_read32(engine, SWITCH_FALCON_THI_INT_STATUS);
    const uint32_t host = switch_mmio_read32(engine, SWITCH_FALCON_ERR_IRQSTAT) &
                          switch_mmio_read32(engine, SWITCH_FALCON_ERR_IRQDEST) & 0xffffu;
    switch_mmio_write32(engine, SWITCH_FALCON_ERR_IRQSCLR, host);
}

// MAILBOX0's MSDEC OS codes (Table 79); an engine's own codes (VIC's 0xb0b600xx) are printed as numbers.
static inline const char *switch_falcon_error_name(uint32_t code)
{
    static const char *const kNames[] = {
        "MSDECOS_ERROR_NONE",
        "MSDECOS_ERROR_EXECUTE_INSUFFICIENT_DATA",
        "MSDECOS_ERROR_SEMAPHORE_INSUFFICIENT_DATA",
        "MSDECOS_ERROR_INVALID_METHOD",
        "MSDECOS_ERROR_INVALID_DMA_PAGE",
        "MSDECOS_ERROR_UNHANDLED_INTERRUPT",
        "MSDECOS_ERROR_EXCEPTION",
        "MSDECOS_ERROR_INVALID_CTXSW_REQUEST",
        "MSDECOS_ERROR_APPLICATION",
        "MSDECOS_ERROR_SWBREAKPT",
    };
    return code < sizeof(kNames) / sizeof(kNames[0]) ? kNames[code] : "an engine code";
}

static inline const char *switch_falcon_error_describe(const switch_falcon_error *e, char *text, size_t size)
{
    snprintf(text, size,
             "falcon error interrupt: MAILBOX0 %#x (%s) MAILBOX1 %#x; IRQSTAT %#x (host %#x) EXCI %#x CPUCTL %#x%s",
             e->mailbox0, switch_falcon_error_name(e->mailbox0), e->mailbox1, e->irqstat,
             e->irqstat & e->irqdest & 0xffffu, e->exci, e->cpuctl, (e->cpuctl & (1u << 4)) ? " (halted)" : "");
    return text;
}

#ifdef __cplusplus
}
#endif

#endif
