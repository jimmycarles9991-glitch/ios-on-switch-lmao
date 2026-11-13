// SPDX-License-Identifier: BSD-2-Clause-Patent
// What a stalled NVENC job's one line says about the falcon and the app it runs (TegraNVENC.cpp stalled()). Only read
// while the engine is powered: a job is in flight.
#include <IOKit/IOLib.h>
#include "TegraNVENC.h"
#include "../../kits/SwitchFalcon/SwitchFalcon.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
enum : uint32_t {
    ICD_RREG = 0x8,  // icd_cmd opc rreg; register index << 8: SP 20, PC 21 (nvgpu falcon.h FALCON_REG_*)
    THI_INT_STATUS = 0x78,
    FALCON_IO = 0x1000,           // the falcon's I/O address 0 (I[0x8] is IRQSTAT at 0x1008)
    ENCODER_INT_STATUS = 0x1870,  // I/O; bit 0 error, bit 1 macroblocks done, write 1 to clear (the H.264 app's ISR)
};

// The H.264 app's (app 1) DMEM words for the end of a picture, read with tools/falcon: the error its ISR (0xf1b)
// records, the macroblocks it counts (+= I[0x1848] per encoder interrupt) and the count fn 0x48b9 spins at 0x4916 until
// (the slice's num_mb, at most the picture's). The H.265 app's (app 3): the setup's error (magic, 0x14a95), its ISR's
// (GP timer, encoder; 0x26f5), the CTBs it counts (0x2776) and the target its spin at 0xfd35 waits for.
enum : uint32_t {
    APP_ERROR = 0x2c8,
    APP_MBS_DONE = 0x2e4,
    APP_MBS_TARGET = 0x354,
    APP3_SETUP_ERROR = 0x2ec,
    APP3_ERROR = 0x2f0,
    APP3_CTBS_DONE = 0x360,
    APP3_CTBS_TARGET = 0x38c,
};

const char *falconRunState(uint32_t cpuctl)
{
    if (cpuctl & 0x10)
        return "halted";
    return (cpuctl & 0x20) ? "stopped" : "running";
}

uint32_t dmem(volatile uint8_t *nvenc, uint32_t address)
{
    switch_mmio_write32(nvenc, SWITCH_FALCON_DMEMC, address);
    return switch_mmio_read32(nvenc, SWITCH_FALCON_DMEMD);
}
}  // namespace

// CPUCTL (bit 4 halted, bit 5 stopped, i.e. waiting for work), IDLESTATE (bit 0 the falcon, bits 1.. engine units
// busy), interrupts, mailboxes, the exception register, the falcon's DMA queue and its external-error capture, the OS's
// version word, the THI interrupt status, the last branch targets in the trace buffer (newest first), and PC and SP
// read three times through the in-circuit debugger (the same PC three times: a tight wait loop or a stall; different:
// still running code), as nvgpu gk20a_falcon_dump_stats.
extern "C" const char *tegra_nvenc_falcon_dump(volatile uint8_t *nvenc)
{
    static char text[512];
    const uint32_t cpuctl = switch_mmio_read32(nvenc, SWITCH_FALCON_CPUCTL);
    int n = snprintf(
        text, sizeof(text),
        "falcon CPUCTL %#x (%s) IDLESTATE %#x IRQSTAT %#x MAILBOX %#x %#x EXCI %#x OS %#x DEBUGINFO %#x "
        "DMATRFCMD %#x EXTERR %#x@%#x THI_INT %#x",
        cpuctl, falconRunState(cpuctl), switch_mmio_read32(nvenc, SWITCH_FALCON_IDLESTATE),
        switch_mmio_read32(nvenc, SWITCH_FALCON_IRQSTAT), switch_mmio_read32(nvenc, SWITCH_FALCON_MAILBOX0),
        switch_mmio_read32(nvenc, SWITCH_FALCON_MAILBOX1), switch_mmio_read32(nvenc, SWITCH_FALCON_EXCI),
        switch_mmio_read32(nvenc, SWITCH_FALCON_OS), switch_mmio_read32(nvenc, SWITCH_FALCON_DEBUGINFO),
        switch_mmio_read32(nvenc, SWITCH_FALCON_DMATRFCMD), switch_mmio_read32(nvenc, SWITCH_FALCON_EXTERRSTAT),
        switch_mmio_read32(nvenc, SWITCH_FALCON_EXTERRADDR), switch_mmio_read32(nvenc, THI_INT_STATUS));
    const uint32_t traced = (switch_mmio_read32(nvenc, SWITCH_FALCON_TRACEIDX) >> 16) & 0xff;
    n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " trace(%u)", traced);
    for (uint32_t i = 0; i < traced && i < 6; i++) {
        switch_mmio_write32(nvenc, SWITCH_FALCON_TRACEIDX, i);
        n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " %#x",
                      switch_mmio_read32(nvenc, SWITCH_FALCON_TRACEPC) & 0xffffff);
    }
    n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " PC/SP");
    for (int i = 0; i < 3; i++) {
        switch_mmio_write32(nvenc, SWITCH_FALCON_ICD_CMD, ICD_RREG | 21u << 8);
        const uint32_t pc = switch_mmio_read32(nvenc, SWITCH_FALCON_ICD_RDATA);
        switch_mmio_write32(nvenc, SWITCH_FALCON_ICD_CMD, ICD_RREG | 20u << 8);
        n += snprintf(text + n, sizeof(text) - static_cast<size_t>(n), " %#x/%#x", pc,
                      switch_mmio_read32(nvenc, SWITCH_FALCON_ICD_RDATA));
    }
    return text;
}

// Which way the app is stuck (the oldest unfinished job's: the one the falcon runs). An error (0xe2640010 GP timer,
// 0xe2640060 encoder error, 0xe2640200 I[0x1ac4] bits 0-2) with fewer macroblocks or CTBs than the target: the ISR saw
// the fault but the spin has no way out. No error and fewer: the engine stopped reporting them, or its interrupt is
// pending untaken (encoder status bit 1 set). Count at the target: past the spin, and the PC shows which of the polls
// after it holds the app.
extern "C" const char *tegra_nvenc_app_dump(volatile uint8_t *nvenc, uint32_t codec)
{
    static char text[176];
    const uint32_t status = switch_mmio_read32(nvenc, FALCON_IO + ENCODER_INT_STATUS);
    if (codec == TEGRA_NVENC_CODEC_H265)
        snprintf(text, sizeof(text), "H.265 app error %#x setup error %#x CTBs %u of %u encoder status %#x (I[%#x])",
                 dmem(nvenc, APP3_ERROR), dmem(nvenc, APP3_SETUP_ERROR), dmem(nvenc, APP3_CTBS_DONE),
                 dmem(nvenc, APP3_CTBS_TARGET), status, ENCODER_INT_STATUS);
    else
        snprintf(text, sizeof(text), "H.264 app error %#x macroblocks %u of %u encoder status %#x (I[%#x])",
                 dmem(nvenc, APP_ERROR), dmem(nvenc, APP_MBS_DONE), dmem(nvenc, APP_MBS_TARGET), status,
                 ENCODER_INT_STATUS);
    return text;
}
