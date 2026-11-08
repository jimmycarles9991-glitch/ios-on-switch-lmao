// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchFalcon.h"
#include <IOKit/IOLib.h>
#include "../SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint32_t kDmactlScrubbing = (1u << 1) | (1u << 2);  // IMEM, DMEM
constexpr uint32_t kDmatrfFull = 1u << 0, kDmatrfIdle = 1u << 1, kDmatrfImem = 1u << 4, kDmatrfSize256 = 6u << 8;
constexpr uint32_t kChunk = 256;
constexpr uint32_t kIrqmsetBoot = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (0xffu << 8);
constexpr uint32_t kIrqdestBoot = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7) | (0xffu << 8);
constexpr uint32_t kItfenCtx = 1u << 0, kItfenMethods = 1u << 1;
constexpr uint32_t kCpuctlStart = 1u << 1;
constexpr uint32_t kScrubUs = 10000, kDmaUs = 100000;

bool queue(volatile uint8_t *engine, uint32_t from, uint32_t to, bool code)
{
    if (!switch_mmio_wait32(engine, SWITCH_FALCON_DMATRFCMD, kDmatrfFull, 0, kDmaUs))
        return false;
    switch_mmio_write32(engine, SWITCH_FALCON_DMATRFMOFFS, to);
    switch_mmio_write32(engine, SWITCH_FALCON_DMATRFFBOFFS, from);
    switch_mmio_write32(engine, SWITCH_FALCON_DMATRFCMD, kDmatrfSize256 | (code ? kDmatrfImem : 0));
    return true;
}
}  // namespace

switch_falcon_result switch_falcon_load(volatile uint8_t *engine, uint64_t firmware_pa,
                                        const switch_falcon_image *image)
{
    if (!switch_mmio_wait32(engine, SWITCH_FALCON_DMACTL, kDmactlScrubbing, 0, kScrubUs))
        return SWITCH_FALCON_SCRUBBING;
    switch_mmio_write32(engine, SWITCH_FALCON_DMACTL, 0);
    switch_mmio_write32(engine, SWITCH_FALCON_DMATRFBASE, static_cast<uint32_t>((firmware_pa + image->bin_data) >> 8));
    bool queued = true;
    for (uint32_t offset = 0; queued && offset < image->data_size; offset += kChunk)
        queued = queue(engine, image->data + offset, offset, false);
    for (uint32_t offset = 0; queued && offset < image->code_size; offset += kChunk)
        queued = queue(engine, image->code + offset, offset, true);
    if (!queued || !switch_mmio_wait32(engine, SWITCH_FALCON_DMATRFCMD, kDmatrfIdle, kDmatrfIdle, kDmaUs))
        return SWITCH_FALCON_DMA;
    return SWITCH_FALCON_OK;
}

void switch_falcon_start(volatile uint8_t *engine)
{
    switch_mmio_write32(engine, SWITCH_FALCON_IRQMSET, kIrqmsetBoot);
    switch_mmio_write32(engine, SWITCH_FALCON_IRQDEST, kIrqdestBoot);
    switch_mmio_write32(engine, SWITCH_FALCON_ITFEN, kItfenMethods | kItfenCtx);
    switch_mmio_write32(engine, SWITCH_FALCON_BOOTVEC, 0);
    switch_mmio_write32(engine, SWITCH_FALCON_CPUCTL, kCpuctlStart);
}

const char *switch_falcon_state(const volatile uint8_t *engine, char *text, size_t size)
{
    snprintf(text, size, "falcon CPUCTL %#x IDLESTATE %#x IRQSTAT %#x MAILBOX0 %#x DEBUGINFO %#x",
             switch_mmio_read32(engine, SWITCH_FALCON_CPUCTL), switch_mmio_read32(engine, SWITCH_FALCON_IDLESTATE),
             switch_mmio_read32(engine, SWITCH_FALCON_IRQSTAT), switch_mmio_read32(engine, SWITCH_FALCON_MAILBOX0),
             switch_mmio_read32(engine, SWITCH_FALCON_DEBUGINFO));
    return text;
}
