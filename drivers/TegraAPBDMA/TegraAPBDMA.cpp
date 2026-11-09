// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 APB DMA controller (TegraAPBDMA.h). Register use follows Linux drivers/dma/tegra20-apb-dma.c with the
// tegra148 chip data T210 uses (separate WCOUNT and WORD_TRANSFER registers, per-channel pause): tegra_dma_probe's
// bring-up, tegra_dma_prep_slave_sg / tegra_dma_prep_dma_cyclic's channel programming for a UART (8-bit APB side,
// 32-bit AHB side, APB address fixed, flow control on the UART's request line), tegra_dma_start, tegra_dma_stop and
// tegra_dma_isr's acknowledge. Progress is the WORD_TRANSFER down-counter: bytes moved = length - (count & 0xfffc) - 4
// (get_current_xferred_count); in continuous mode it reloads at the wrap a little before STATUS shows the end of count
// (tegra_dma_sg_bytes_xferred), which tegra_apbdma_moved's callers resolve against their own count.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <string.h>
#include "TegraAPBDMA.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern task_t kernel_task;
}

namespace {
constexpr uint64_t kDmaPA = 0x60020000, kDmaSize = 0x2000;
constexpr uint32_t kChannelBase = 0x1000, kChannelStride = 0x40;

// Controller (tegra20-apb-dma.c TEGRA_APBDMA_*).
enum : uint32_t {
    GENERAL = 0x000,
    CONTROL = 0x010,
    IRQ_MASK = 0x01c,
    IRQ_MASK_SET = 0x020
};

constexpr uint32_t kGeneralEnable = 1u << 31;

// Channel registers and bits.
enum : uint32_t {
    CSR = 0x00,
    STATUS = 0x04,
    CSRE = 0x0c,
    AHB_PTR = 0x10,
    AHB_SEQ = 0x14,
    APB_PTR = 0x18,
    APB_SEQ = 0x1c,
    WCOUNT = 0x20,
    WORD_TRANSFER = 0x24,
};

enum : uint32_t {
    CSR_ENB = 1u << 31,
    CSR_IE_EOC = 1u << 30,
    CSR_DIR = 1u << 28,  // memory (AHB) to device (APB)
    CSR_ONCE = 1u << 27,
    CSR_FLOW = 1u << 21,
    CSR_REQ_SEL_SHIFT = 16,
    STATUS_BUSY = 1u << 31,
    STATUS_ISE_EOC = 1u << 30,
    AHBSEQ_INTR_ENB = 1u << 31,
    AHBSEQ_BUS_WIDTH_32 = 2u << 28,
    AHBSEQ_BURST_1 = 4u << 24,  // one word: one APB read
    AHBSEQ_BURST_4 = 5u << 24,  // four words: the 16-byte TX trigger
    APBSEQ_BUS_WIDTH_8 = 0u << 28,
    APBSEQ_BUS_WIDTH_32 = 2u << 28,  // one 32-bit read of the register per word (a UART's RBR: one byte)
    APBSEQ_WRAP_WORD_1 = 1u << 16,   // the APB address stays on the one register
    COUNT_MASK = 0xfffc,
};

constexpr unsigned kClock = 34;  // TEGRA210_CLK_APBDMA: CAR bank H bit 2, also its reset

inline uint32_t intidOf(unsigned channel)
{
    return 32 + (channel < 16 ? 104 + channel : 128 + channel - 16);
}

volatile uint8_t *gDma;
IOLock *gLock;  // bring-up and open
bool gTried, gUp;
char gWhy[96];
}  // namespace

struct tegra_apbdma_channel {
    unsigned number;
    volatile uint8_t *regs;
    IOFilterInterruptEventSource *source;
    tegra_apbdma_handler handler;
    void *context;
    uint32_t size;  // of the transfer last started
    volatile uint32_t interrupts;
};

namespace {
tegra_apbdma_channel gChannels[TEGRA_APBDMA_CHANNELS];
bool gOpen[TEGRA_APBDMA_CHANNELS];

inline uint32_t rd(const tegra_apbdma_channel *ch, uint32_t reg)
{
    return switch_mmio_read32(ch->regs, reg);
}

inline void wr(const tegra_apbdma_channel *ch, uint32_t reg, uint32_t value)
{
    switch_mmio_write32(ch->regs, reg, value);
}

inline uint32_t grd(uint32_t reg)
{
    return switch_mmio_read32(gDma, reg);
}

inline void gwr(uint32_t reg, uint32_t value)
{
    switch_mmio_write32(gDma, reg, value);
}

// tegra_dma_probe: clock on, reset pulse, enable, no control bits, every channel's interrupt unmasked. Also after SC7,
// which leaves the controller in reset with its clock off (ensureUp). A controller someone else already runs (clocked,
// out of reset, a channel enabled) is left alone.
bool bringUp()
{
    const bool clocked = switch_car_clock_enabled(kClock) && !switch_car_reset_asserted(kClock);
    if (clocked) {
        for (unsigned c = 0; c < 32; ++c) {
            if (grd(kChannelBase + c * kChannelStride + CSR) & CSR_ENB) {
                snprintf(gWhy, sizeof(gWhy), "channel %u already enabled by someone else", c);
                return false;
            }
        }
    }
    switch_car_clock_enable(kClock);
    switch_car_reset_assert(kClock);
    (void)switch_car_reset_asserted(kClock);
    IODelay(2);
    switch_car_reset_deassert(kClock);
    (void)switch_car_reset_asserted(kClock);
    gwr(GENERAL, kGeneralEnable);
    gwr(CONTROL, 0);
    gwr(IRQ_MASK_SET, 0xffffffffu);
    const uint32_t general = grd(GENERAL);
    const uint32_t mask = grd(IRQ_MASK);
    if (!(general & kGeneralEnable) || mask != 0xffffffffu) {
        snprintf(gWhy, sizeof(gWhy), "controller did not take its set-up (GENERAL %#x IRQ_MASK %#x)", general, mask);
        return false;
    }
    return true;
}

// Before a transfer starts: the controller back up if SC7 reset it since (its enable reads 0 then).
void ensureUp()
{
    if (switch_car_clock_enabled(kClock) && !switch_car_reset_asserted(kClock) && (grd(GENERAL) & kGeneralEnable))
        return;
    switch_car_clock_enable(kClock);
    switch_car_reset_assert(kClock);
    (void)switch_car_reset_asserted(kClock);
    IODelay(2);
    switch_car_reset_deassert(kClock);
    (void)switch_car_reset_asserted(kClock);
    gwr(GENERAL, kGeneralEnable);
    gwr(CONTROL, 0);
    gwr(IRQ_MASK_SET, 0xffffffffu);
}

void stopLocked(const tegra_apbdma_channel *ch)
{
    const uint32_t csr = rd(ch, CSR) & ~CSR_IE_EOC;
    wr(ch, CSR, csr);
    wr(ch, CSR, csr & ~CSR_ENB);
    const uint32_t status = rd(ch, STATUS);
    if (status & STATUS_ISE_EOC)
        wr(ch, STATUS, status);
}

// Device to memory, one 32-bit APB read per request, each stored as one word (tegra_apbdma_start_rx_words).
void startRxWords(tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t bytes, uint32_t once)
{
    ensureUp();
    stopLocked(ch);
    ch->size = bytes;
    wr(ch, APB_PTR, apb);
    wr(ch, APB_SEQ, APBSEQ_BUS_WIDTH_32 | APBSEQ_WRAP_WORD_1);
    wr(ch, AHB_PTR, pa);
    wr(ch, AHB_SEQ, AHBSEQ_INTR_ENB | AHBSEQ_BUS_WIDTH_32 | AHBSEQ_BURST_1);
    wr(ch, WCOUNT, (bytes - 4) & COUNT_MASK);
    const uint32_t csr = CSR_IE_EOC | once | CSR_FLOW | (req & 0x1f) << CSR_REQ_SEL_SHIFT;  // device to memory
    wr(ch, CSR, csr);
    wr(ch, CSR, csr | CSR_ENB);
}

// Primary interrupt context, CPU0: the end of count acknowledged (tegra_dma_isr writes STATUS back), then the owner.
// Nothing for the work loop: the owner signals its own when it needs to.
// NOLINTNEXTLINE(misc-const-correctness): IOFilterInterruptEventSource::Filter's signature
bool filter(OSObject *, IOFilterInterruptEventSource *sender)
{
    for (tegra_apbdma_channel &ch : gChannels) {
        if (ch.source != sender)
            continue;
        const uint32_t status = rd(&ch, STATUS);
        if (!(status & STATUS_ISE_EOC))
            return false;
        wr(&ch, STATUS, status);
        ch.interrupts = ch.interrupts + 1;
        if (ch.handler)
            ch.handler(ch.context);
        return false;
    }
    return false;
}

void action(OSObject *, IOInterruptEventSource *, int)
{}
}  // namespace

bool tegra_apbdma_alloc(uint32_t size, tegra_apbdma_buffer *out)
{
    switch_dma_buffer buffer;
    if (!switch_dma_alloc(&buffer, size))
        return false;
    out->va = buffer.va;
    out->pa = static_cast<uint32_t>(buffer.pa);
    out->size = size;
    out->memory = buffer.memory;
    return true;
}

tegra_apbdma_channel *tegra_apbdma_open(unsigned channel, IOWorkLoop *loop, tegra_apbdma_handler handler, void *context,
                                        char *why, size_t whySize)
{
    if (channel >= TEGRA_APBDMA_CHANNELS || !loop) {
        snprintf(why, whySize, "channel %u or work loop invalid", channel);
        return nullptr;
    }
    static IOLock *volatile created;
    if (!created) {
        IOLock *lock = IOLockAlloc();
        if (lock && !__sync_bool_compare_and_swap(&created, nullptr, lock))
            IOLockFree(lock);
    }
    gLock = created;
    if (!gLock) {
        snprintf(why, whySize, "no memory for the lock");
        return nullptr;
    }
    IOLockLock(gLock);
    if (!gTried) {
        gTried = true;
        gDma = switch_mmio_map(kDmaPA, kDmaSize);
        if (!gDma) {
            snprintf(gWhy, sizeof(gWhy), "cannot map the controller or CAR");
        } else {
            gUp = bringUp();
        }
    }
    tegra_apbdma_channel *ch = &gChannels[channel];
    const char *fail = nullptr;
    if (!gUp)
        fail = gWhy;
    if (!fail && gOpen[channel])
        fail = "channel already open";
    if (!fail) {
        ch->number = channel;
        ch->regs = gDma + kChannelBase + static_cast<size_t>(channel) * kChannelStride;
        // The channel takes what it is given: a pointer register written and read back (then cleared).
        stopLocked(ch);
        wr(ch, AHB_PTR, 0x8badf00c);
        const uint32_t back = rd(ch, AHB_PTR);
        wr(ch, AHB_PTR, 0);
        if (back != 0x8badf00c) {
            snprintf(gWhy, sizeof(gWhy), "channel %u AHB_PTR read back %#x", channel, back);
            fail = gWhy;
        }
    }
    if (!fail) {
        ch->handler = handler;
        ch->context = context;
        IOService *nub = switch_interrupt_nub(intidOf(channel), false);
        ch->source =
            nub ? IOFilterInterruptEventSource::filterInterruptEventSource(loop, &action, &filter, nub, 0) : nullptr;
        if (!ch->source || loop->addEventSource(ch->source) != kIOReturnSuccess) {
            char gic[160];
            switch_interrupt_describe(intidOf(channel), gic, sizeof(gic));
            panic("TegraAPBDMA: cannot attach channel %u's interrupt (INTID %u): %s", channel, intidOf(channel), gic);
        }
        ch->source->enable();
        gOpen[channel] = true;
    }
    IOLockUnlock(gLock);
    if (fail) {
        snprintf(why, whySize, "%s", fail);
        return nullptr;
    }
    return ch;
}

void tegra_apbdma_start_once(tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t size)
{
    ensureUp();
    stopLocked(ch);
    ch->size = size;
    wr(ch, APB_PTR, apb);
    wr(ch, APB_SEQ, APBSEQ_BUS_WIDTH_8 | APBSEQ_WRAP_WORD_1);
    wr(ch, AHB_PTR, pa);
    wr(ch, AHB_SEQ, AHBSEQ_INTR_ENB | AHBSEQ_BUS_WIDTH_32 | AHBSEQ_BURST_4);
    wr(ch, WCOUNT, (size - 4) & COUNT_MASK);
    const uint32_t csr = CSR_IE_EOC | CSR_DIR | CSR_ONCE | CSR_FLOW | (req & 0x1f) << CSR_REQ_SEL_SHIFT;
    wr(ch, CSR, csr);
    wr(ch, CSR, csr | CSR_ENB);
}

void tegra_apbdma_start_rx_words(tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t words)
{
    startRxWords(ch, apb, req, pa, words * 4, CSR_ONCE);
}

void tegra_apbdma_start_rx_words_ring(tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t size)
{
    startRxWords(ch, apb, req, pa, size, 0);  // continuous: no ONCE
}

uint32_t tegra_apbdma_moved(const tegra_apbdma_channel *ch, bool *endPending)
{
    const uint32_t count = rd(ch, WORD_TRANSFER) & COUNT_MASK;
    const uint32_t status = rd(ch, STATUS);
    if (endPending)
        *endPending = ((status & STATUS_ISE_EOC) != 0u);
    const uint32_t left = count + 4;
    return left >= ch->size ? 0 : ch->size - left;
}

void tegra_apbdma_stop(const tegra_apbdma_channel *ch)
{
    stopLocked(ch);
}

bool tegra_apbdma_running(const tegra_apbdma_channel *ch)
{
    return (rd(ch, CSR) & CSR_ENB) != 0u;
}

uint32_t tegra_apbdma_interrupts(const tegra_apbdma_channel *ch)
{
    return ch->interrupts;
}

void tegra_apbdma_describe(const tegra_apbdma_channel *ch, char *buffer, size_t size)
{
    char gic[160];
    switch_interrupt_describe(intidOf(ch->number), gic, sizeof(gic));
    snprintf(buffer, size, "APB DMA ch%u csr %#x status %#x count %#x ahb %#x/%#x apb %#x irqs %u, %s", ch->number,
             rd(ch, CSR), rd(ch, STATUS), rd(ch, WORD_TRANSFER), rd(ch, AHB_PTR), rd(ch, AHB_SEQ), rd(ch, APB_PTR),
             ch->interrupts, gic);
}
