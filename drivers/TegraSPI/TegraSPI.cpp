// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 SPI master: PIO blocks, packed four bytes to a FIFO word, one IOLock per bus, every completion by interrupt
// (TegraSPI.h).
// The command word, FIFO handling and interrupt handling follow Linux spi-tegra114.c (tegra_spi_setup_transfer_one,
// tegra_spi_start_cpu_based_transfer, tegra_spi_isr, handle_cpu_based_xfer, tegra_spi_clear_fifo; T210 chip data:
// intr_mask_reg); clock, reset and pinmux bring-up as ../TegraI2C (hekate clock_enable).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <libkern/OSAtomic.h>
#include "TegraSPI.h"
#include "TegraSPIWords.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#if SWITCH_STATS_IO
#include <os/log.h>
#endif

namespace {
constexpr vm_offset_t kSpiPA = 0x7000D000, kSpiSize = 0x1000;        // SPI1 +0x400 .. SPI4 +0xa00
constexpr vm_offset_t kCarPA = 0x60006000, kCarSize = 0x1000;        // CLK_RST_CONTROLLER
constexpr vm_offset_t kPinmuxPA = 0x70003000, kPinmuxSize = 0x1000;  // PINMUX_AUX
constexpr unsigned kBuses = 4;
constexpr uint32_t kPllP = 408000000;
constexpr uint32_t kGraceMs = 50;  // after a timed-out wait, for an interrupt action already under way

enum : uint32_t {
    COMMAND1 = 0x000,
    TRANS_STATUS = 0x010,
    FIFO_STATUS = 0x014,
    DMA_CTL = 0x020,
    DMA_BLK = 0x024,
    TX_FIFO = 0x108,
    RX_FIFO = 0x188,
    INTR_MASK = 0x18c,
};

enum : uint32_t {
    CMD1_BIT_LENGTH_MASK = 0x1f,
    CMD1_BIT_LENGTH_8 = 7,
    CMD1_PACKED = 1u << 5,
    CMD1_TX_EN = 1u << 11,
    CMD1_RX_EN = 1u << 12,
    CMD1_BOTH_EN_BYTE = 1u << 13,
    CMD1_BOTH_EN_BIT = 1u << 14,
    CMD1_LSBYTE_FE = 1u << 15,
    CMD1_LSBIT_FE = 1u << 16,
    CMD1_BIDIROE = 1u << 17,
    CMD1_CS_SS_VAL = 1u << 20,  // with CS_SW_HW: the chip select's level (1 = high, inactive for these devices)
    CMD1_CS_SW_HW = 1u << 21,
    CMD1_CS_SEL_SHIFT = 26,
    CMD1_CS_SEL_MASK = 3u << 26,
    CMD1_MODE_SHIFT = 28,
    CMD1_MODE_MASK = 3u << 28,
    CMD1_M_S = 1u << 30,
    CMD1_PIO = 1u << 31,
    TRANS_RDY = 1u << 30,
    FIFO_RX_EMPTY = 1u << 0,
    FIFO_TX_EMPTY = 1u << 2,
    FIFO_ERRORS = 0xf0u,  // RX underflow/overflow, TX underflow/overflow
    FIFO_ERR = 1u << 8,
    FIFO_TX_FLUSH = 1u << 14,
    FIFO_RX_FLUSH = 1u << 15,
    INTR_ALL = 0x1fu << 25,  // FIFO errors and RDY; 1 = masked
};

constexpr uint32_t kRxCountShift = 23, kRxCountMask = 0x7f;

// A block's state (Bus::state): started and waiting for RDY (kActive), RDY seen by the handler (kDone, the bytes
// still in the RX FIFO for the waiter), or none (kIdle). Normal completion hands off kActive -> kDone -> kIdle
// with plain acquire/release stores; a rare timeout quiesces the handler via b.irqNub->disableInterrupt(0) before
// touching state.
enum : uint32_t {
    kIdle,
    kActive,
    kDone
};

struct BusInfo {
    uint16_t regs;                     // offset from kSpiPA; 0 = not described
    uint16_t reset, enable;            // CAR RST_DEV_x_SET, CLK_ENB_x_SET (CLR is +4)
    uint16_t resetState, enableState;  // CAR RST_DEVICES_x, CLK_OUT_ENB_x
    uint16_t source;                   // CAR CLK_SOURCE_SBCx
    uint8_t bit;
    uint8_t divider;                // 7.1 divider field from PLLP: rate = 408 MHz * 2 / (divider + 2)
    uint8_t mode;                   // SPI mode (CPOL << 1 | CPHA)
    uint16_t mosi, miso, sck, cs0;  // PINMUX_AUX offsets
    uint16_t spi;                   // GIC SPI
};

// SPI4: tegra210.dtsi spi@7000da00 (SPI 93, clock/reset 68 = U bit 4), CLK_SOURCE_SBC4 0x1b4 (clk-tegra-periph.c);
// the IMU's 8 MHz and mode 3 (tegra210-nx-sensor.dtsi); pads at PINMUX_AUX 0x3078/307c/3080/3084 (fric pinmux).
constexpr BusInfo kBus[kBuses] = {
    {},
    {},
    {},
    {0xa00, 0x310, 0x330, 0x00c, 0x018, 0x1b4, 4, 100, 3, 0x078, 0x07c, 0x080, 0x084, 93},
};
// PINMUX_AUX: function 1:0 (0 = SPIx), pull 3:2, tristate 4, park 5, input enable 6 (hekate bdk/soc/pinmux.h).
constexpr uint32_t kPinmuxFieldMask = 0x7f, kPinmuxOut = 0, kPinmuxIn = (2u << 2) | (1u << 4) | (1u << 6);

struct Bus {
    volatile uint8_t *regs;
    IOLock *lock;  // serializes transfers, recovery and suspend/resume
    IOService *irqNub;
    uint32_t hz, idle;  // bus clock; COMMAND1 between transfers (master, mode, 8-bit words, chip select high)
    bool ready;
    uint32_t state;  // kIdle, kActive, kDone (shared with spiInterrupt)
    uint32_t fifo;   // FIFO_STATUS when the handler saw RDY: the RX word count and the error bits
};

IOLock *volatile gSetupLock;
volatile uint8_t *gSpi, *gCar, *gPinmux;
Bus gBus[kBuses];

inline uint32_t intid(unsigned bus)
{
    return 32 + kBus[bus - 1].spi;
}

// hekate clock_enable(): reset, gate, select PLLP / divider, ungate, release reset.
void clockEnable(const BusInfo &info)
{
    const uint32_t bit = 1u << info.bit;
    switch_mmio_write32(gCar, info.reset, bit);
    switch_mmio_write32(gCar, info.enable + 4, bit);
    switch_mmio_write32(gCar, info.source, (0u << 29) | info.divider);
    switch_mmio_write32(gCar, info.enable, bit);
    IODelay(2);
    switch_mmio_write32(gCar, info.reset + 4, bit);
    (void)switch_mmio_read32(gCar, info.reset);
}

void pinmux(const BusInfo &info)
{
    const uint16_t outputs[] = {info.mosi, info.sck, info.cs0};
    for (const uint16_t pad : outputs)
        switch_mmio_write32(gPinmux, pad, (switch_mmio_read32(gPinmux, pad) & ~kPinmuxFieldMask) | kPinmuxOut);
    switch_mmio_write32(gPinmux, info.miso, (switch_mmio_read32(gPinmux, info.miso) & ~kPinmuxFieldMask) | kPinmuxIn);
}

// Both FIFOs empty (tegra_spi_clear_fifo): a flush completes within a few microseconds.
bool flushFifos(const Bus &b)
{
    const uint32_t status = switch_mmio_read32(b.regs, FIFO_STATUS);
    if ((status & (FIFO_RX_EMPTY | FIFO_TX_EMPTY)) == (FIFO_RX_EMPTY | FIFO_TX_EMPTY))
        return true;
    switch_mmio_write32(b.regs, FIFO_STATUS, status | FIFO_RX_FLUSH | FIFO_TX_FLUSH);
    return switch_mmio_wait32(b.regs, FIFO_STATUS, FIFO_RX_EMPTY | FIFO_TX_EMPTY, FIFO_RX_EMPTY | FIFO_TX_EMPTY, 2000);
}

void clearStatus(const Bus &b)
{
    switch_mmio_write32(b.regs, TRANS_STATUS, switch_mmio_read32(b.regs, TRANS_STATUS));  // write 1 to clear
    switch_mmio_write32(b.regs, FIFO_STATUS, FIFO_ERR | FIFO_ERRORS);
}

// Normal transfers follow filterInterrupt(), which already cleared TRANS_STATUS and FIFO_ERRORS; skip clearStatus()
// unless FIFO_STATUS shows residual data or error bits.
bool ensureCleanFifos(const Bus &b)
{
    const uint32_t status = switch_mmio_read32(b.regs, FIFO_STATUS);
    if ((status & (FIFO_RX_EMPTY | FIFO_TX_EMPTY | FIFO_ERR | FIFO_ERRORS)) == (FIFO_RX_EMPTY | FIFO_TX_EMPTY))
        return true;
    if ((status & (FIFO_RX_EMPTY | FIFO_TX_EMPTY)) != (FIFO_RX_EMPTY | FIFO_TX_EMPTY)) {
        switch_mmio_write32(b.regs, FIFO_STATUS, status | FIFO_RX_FLUSH | FIFO_TX_FLUSH);
        if (!switch_mmio_wait32(b.regs, FIFO_STATUS, FIFO_RX_EMPTY | FIFO_TX_EMPTY, FIFO_RX_EMPTY | FIFO_TX_EMPTY,
                                2000)) {
            return false;
        }
    }
    clearStatus(b);
    return true;
}

// Master, the bus's mode, 8-bit packets MSB first (packed per block), chip select 0 under software control and high; no
// DMA; the RDY and FIFO error interrupts unmasked (tegra_spi_probe def_command1_reg, tegra_spi_set_intr_mask).
int configure(Bus &b, const BusInfo &info)
{
    uint32_t cmd = switch_mmio_read32(b.regs, COMMAND1);
    cmd &= ~(CMD1_BIT_LENGTH_MASK | CMD1_PACKED | CMD1_TX_EN | CMD1_RX_EN | CMD1_BOTH_EN_BYTE | CMD1_BOTH_EN_BIT |
             CMD1_LSBYTE_FE | CMD1_LSBIT_FE | CMD1_BIDIROE | CMD1_CS_SEL_MASK | CMD1_MODE_MASK | CMD1_PIO);
    cmd |= CMD1_M_S | (static_cast<uint32_t>(info.mode) << CMD1_MODE_SHIFT) | CMD1_CS_SW_HW | CMD1_CS_SS_VAL |
           CMD1_BIT_LENGTH_8;
    b.idle = cmd;
    switch_mmio_write32(b.regs, COMMAND1, cmd);
    switch_mmio_write32(b.regs, DMA_CTL, 0);
    switch_mmio_write32(b.regs, INTR_MASK, switch_mmio_read32(b.regs, INTR_MASK) & ~INTR_ALL);
    if (!flushFifos(b))
        return TEGRA_SPI_ETIMEDOUT;
    clearStatus(b);
    return TEGRA_SPI_OK;
}

// Primary interrupt context, the bus's SPI (level): RDY ends the block. Status bits are cleared before publishing
// kDone so the next transfer cannot observe stale TRANS_STATUS/FIFO_ERRORS once the waiter wakes.
void spiInterrupt(OSObject *, void *refCon, IOService *, int)
{
    auto &b = *static_cast<Bus *>(refCon);
    const uint32_t trans = switch_mmio_read32(b.regs, TRANS_STATUS);
    const uint32_t fifo = switch_mmio_read32(b.regs, FIFO_STATUS);
    switch_mmio_write32(b.regs, TRANS_STATUS, trans);  // write 1 to clear: RDY off, the line low
    if (fifo & FIFO_ERRORS)
        switch_mmio_write32(b.regs, FIFO_STATUS, FIFO_ERR | (fifo & FIFO_ERRORS));
    if ((trans & TRANS_RDY) && __atomic_load_n(&b.state, __ATOMIC_RELAXED) == kActive) {
        __atomic_store_n(&b.fifo, fifo, __ATOMIC_RELAXED);
        __atomic_store_n(&b.state, kDone, __ATOMIC_RELEASE);
        thread_wakeup(reinterpret_cast<event_t>(&b.state));
    }
}

// Thread context, the block ended by RDY (kDone): its bytes from the RX FIFO (handle_cpu_based_xfer), a word per byte
// clocked or, packed, per four (TegraSPIWords.h; the last word's bytes past the block's end are not data), counted by
// the FIFO_STATUS the filter saw.
int take(Bus &b, uint8_t *rx, size_t len, bool packed)
{
    const uint32_t fifo = __atomic_load_n(&b.fifo, __ATOMIC_RELAXED);
    size_t done = 0;
    bool overflow = false;
    for (uint32_t words = (fifo >> kRxCountShift) & kRxCountMask; words; --words) {
        const uint32_t word = switch_mmio_read32(b.regs, RX_FIFO);
        if (done == len) {
            overflow = true;
            continue;
        }
        done = tegra_spi_rx_word(word, rx, done, len, packed);
    }
    __atomic_store_n(&b.state, kIdle, __ATOMIC_RELAXED);
    return (fifo & FIFO_ERRORS) || overflow || done != len ? TEGRA_SPI_EIO : TEGRA_SPI_OK;
}

// Caller holds b.lock and has started the block. Sleeps until the filter ends the wait or `us` pass. Timing out while
// TRANS_STATUS shows RDY (unmasked) means the controller's interrupt is not reaching us: after a grace period for an
// interrupt already under way, panic with the controller's and the GIC's state.
int sleepForAction(unsigned bus, Bus &b, uint64_t us, uint8_t *rx, size_t len, bool packed)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(us), kMicrosecondScale, &deadline);
    for (bool grace = false;; grace = true) {
        for (;;) {
            const wait_result_t wait =
                assert_wait_deadline(reinterpret_cast<event_t>(&b.state), THREAD_UNINT, deadline);
            if (__atomic_load_n(&b.state, __ATOMIC_ACQUIRE) != kActive) {
                if (wait == THREAD_WAITING)
                    clear_wait(current_thread(), THREAD_AWAKENED);
                return take(b, rx, len, packed);
            }
            if (wait != THREAD_WAITING || thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT)
                break;
        }
        if (__atomic_load_n(&b.state, __ATOMIC_ACQUIRE) != kActive)
            return take(b, rx, len, packed);
        const uint32_t trans = switch_mmio_read32(b.regs, TRANS_STATUS);
        const uint32_t mask = switch_mmio_read32(b.regs, INTR_MASK);
        if (!(trans & TRANS_RDY) || (mask & INTR_ALL))
            break;
        if (grace) {
            char gic[192];
            switch_interrupt_describe(intid(bus), gic, sizeof(gic));
            panic("TegraSPI: SPI%u interrupt lost (INTID %u): TRANS_STATUS %#x FIFO_STATUS %#x INTR_MASK %#x, %s", bus,
                  intid(bus), trans, switch_mmio_read32(b.regs, FIFO_STATUS), mask, gic);
        }
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    // Quiesce any in-flight spiInterrupt() before resetting b.state without a hard-IRQ CAS.
    b.irqNub->disableInterrupt(0);
    const bool done = __atomic_load_n(&b.state, __ATOMIC_ACQUIRE) != kActive;
    if (!done)
        __atomic_store_n(&b.state, kIdle, __ATOMIC_RELAXED);
    b.irqNub->enableInterrupt(0);
    return done ? take(b, rx, len, packed) : TEGRA_SPI_ETIMEDOUT;
}

// One PIO block (tegra_spi_start_cpu_based_transfer): empty FIFOs, chip select low, TX words queued, block length in
// packets, then PIO starts the clock (TegraSPIWords.h packs them). The budget is the block's bit time plus 2 ms.
int block(unsigned bus, Bus &b, unsigned cs, const uint8_t *tx, uint8_t *rx, size_t len)
{
    int result = ensureCleanFifos(b) ? TEGRA_SPI_OK : TEGRA_SPI_ETIMEDOUT;
    if (result == TEGRA_SPI_OK) {
        const bool packed = tegra_spi_packed(len);
        const uint32_t cmd = (b.idle & ~(CMD1_CS_SS_VAL | CMD1_CS_SEL_MASK)) |
                             (static_cast<uint32_t>(cs) << CMD1_CS_SEL_SHIFT) | CMD1_TX_EN | CMD1_RX_EN |
                             (packed ? CMD1_PACKED : 0);
        switch_mmio_write32(b.regs, COMMAND1, cmd);
        for (size_t i = 0; i < tegra_spi_words(len, packed); ++i)
            switch_mmio_write32(b.regs, TX_FIFO, tegra_spi_tx_word(tx, len, packed, i));
        switch_mmio_write32(b.regs, DMA_BLK, static_cast<uint32_t>(len) - 1);
        __atomic_store_n(&b.state, kActive, __ATOMIC_RELAXED);
        switch_mmio_write32_publish(b.regs, COMMAND1, cmd | CMD1_PIO);
        result = sleepForAction(bus, b, 2000 + static_cast<uint64_t>(len) * 8 * 1000000 / b.hz, rx, len, packed);
    }
    switch_mmio_write32(b.regs, COMMAND1, b.idle);  // chip select high
    return result;
}

// After a failed block the controller is reset and reprogrammed (as tegra_spi_transfer_one_message on a timeout).
// The interrupt is off meanwhile, so the filter never reads an unclocked controller.
void recover(Bus &b, const BusInfo &info)
{
    b.irqNub->disableInterrupt(0);
    clockEnable(info);
    (void)configure(b, info);
    b.irqNub->enableInterrupt(0);
}

#if SWITCH_STATS_IO
// Each bus's line, from its own transfers (no timer): every 10 s of traffic, its blocks (one RDY interrupt each) and
// the FIFO words they cost the CPU each way. A 2 s pause starts a new window.
struct BusStats {
    uint64_t start, last;
    uint32_t blocks, packed, bytes, words, failed;
};

BusStats gStats[kBuses];

// Under the bus's transfer lock; returns true with a snapshot to log after releasing the lock.
bool noteBlock(unsigned bus, size_t len, int result, BusStats *emit, uint64_t *msOut)
{
    BusStats &s = gStats[bus - 1];
    const bool packed = tegra_spi_packed(len);
    const uint64_t now = mach_absolute_time();
    uint64_t window = 0, pause = 0, ns = 0;
    nanoseconds_to_absolutetime(10 * NSEC_PER_SEC, &window);
    nanoseconds_to_absolutetime(2 * NSEC_PER_SEC, &pause);
    if (s.start == 0 || now - s.last >= pause)
        s = {now, now, 0, 0, 0, 0, 0};
    s.last = now;
    ++s.blocks;
    s.packed += packed ? 1 : 0;
    s.bytes += static_cast<uint32_t>(len);
    s.words += static_cast<uint32_t>(tegra_spi_words(len, packed));
    s.failed += result != TEGRA_SPI_OK ? 1 : 0;
    if (now - s.start < window)
        return false;
    absolutetime_to_nanoseconds(now - s.start, &ns);
    *emit = s;
    *msOut = ns / NSEC_PER_MSEC;
    s = {now, now, 0, 0, 0, 0, 0};
    return true;
}

void logBusStats(unsigned bus, const BusStats &s, uint64_t ms)
{
    os_log(OS_LOG_DEFAULT,
           "TegraSPI stats: SPI%u %llu ms: %u blocks (%llu/s), %u packed, %u bytes in %u FIFO words each way, %u "
           "failed",
           bus, ms, s.blocks, ms ? uint64_t{s.blocks} * 1000 / ms : 0, s.packed, s.bytes, s.words, s.failed);
}
#endif

IOLock *setupLock()
{
    if (!gSetupLock) {
        IOLock *lock = IOLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, &gSetupLock))
            IOLockFree(lock);
    }
    return gSetupLock;
}

int bringUp(unsigned bus, Bus &b)
{
    const BusInfo &info = kBus[bus - 1];
    b.regs = gSpi + info.regs;
    b.hz = kPllP * 2 / (info.divider + 2u);
    clockEnable(info);
    pinmux(info);
    const int result = configure(b, info);
    if (result != TEGRA_SPI_OK)
        return result;
    switch_mmio_write32(b.regs, INTR_MASK,
                        switch_mmio_read32(b.regs, INTR_MASK) | INTR_ALL);  // quiet until the event source is in place
    IOService *nub = switch_interrupt_nub(intid(bus), false);
    if (!nub || nub->registerInterrupt(0, nullptr, &spiInterrupt, &b) != kIOReturnSuccess) {
        panic("TegraSPI: cannot attach the SPI%u interrupt (INTID %u)", bus, intid(bus));
    }
    b.irqNub = nub;
    b.irqNub->enableInterrupt(0);
    switch_mmio_write32(b.regs, INTR_MASK, switch_mmio_read32(b.regs, INTR_MASK) & ~INTR_ALL);
    return TEGRA_SPI_OK;
}
}  // namespace

extern "C" int tegra_spi_init(unsigned bus)
{
    if (bus < 1 || bus > kBuses || !kBus[bus - 1].regs)
        return TEGRA_SPI_EINVAL;
    IOLock *setup = setupLock();
    if (!setup)
        return TEGRA_SPI_ENOMEM;
    IOLockLock(setup);
    int result = TEGRA_SPI_OK;
    if (!gSpi) {
        gCar = switch_mmio_map(kCarPA, kCarSize);
        gPinmux = switch_mmio_map(kPinmuxPA, kPinmuxSize);
        gSpi = (gCar && gPinmux) ? switch_mmio_map(kSpiPA, kSpiSize) : nullptr;
    }
    Bus &b = gBus[bus - 1];
    if (!gSpi) {
        result = TEGRA_SPI_ENOMEM;
    } else if (!b.irqNub) {
        if (!b.lock)
            b.lock = IOLockAlloc();
        if (!b.lock) {
            result = TEGRA_SPI_ENOMEM;
        } else {
            IOLockLock(b.lock);
            result = bringUp(bus, b);  // claims the bus's interrupt (sleeps until the GIC is up)
            b.ready = result == TEGRA_SPI_OK;
            IOLockUnlock(b.lock);
        }
    }
    IOLockUnlock(setup);
    if (result != TEGRA_SPI_OK)
        IOLog("TegraSPI: SPI%u init failed (%d)\n", bus, result);
    return result;
}

extern "C" int tegra_spi_transfer(unsigned bus, unsigned cs, const void *tx, void *rx, size_t len)
{
    if (bus < 1 || bus > kBuses || !kBus[bus - 1].regs || cs > 3 || !len || len > TEGRA_SPI_MAX_TRANSFER) {
        return TEGRA_SPI_EINVAL;
    }
    Bus &b = gBus[bus - 1];
    if (!b.lock)
        return TEGRA_SPI_ENOTREADY;
    IOLockLock(b.lock);
    const int result = b.ready ? block(bus, b, cs, (const uint8_t *)tx, (uint8_t *)rx, len) : TEGRA_SPI_ENOTREADY;
    if (result != TEGRA_SPI_OK && result != TEGRA_SPI_ENOTREADY)
        recover(b, kBus[bus - 1]);
#if SWITCH_STATS_IO
    BusStats emit = {};
    uint64_t emitMs = 0;
    const bool logStats = result != TEGRA_SPI_ENOTREADY && noteBlock(bus, len, result, &emit, &emitMs);
#endif
    IOLockUnlock(b.lock);
#if SWITCH_STATS_IO
    if (logStats)
        logBusStats(bus, emit, emitMs);
#endif
    return result;
}

extern "C" void tegra_spi_suspend(unsigned bus)
{
    if (bus < 1 || bus > kBuses || !kBus[bus - 1].regs)
        return;
    Bus &b = gBus[bus - 1];
    if (!b.lock || !b.irqNub)
        return;
    const BusInfo &info = kBus[bus - 1];
    IOLockLock(b.lock);
    if (b.ready) {
        b.irqNub->disableInterrupt(0);
        switch_mmio_write32(b.regs, INTR_MASK, switch_mmio_read32(b.regs, INTR_MASK) | INTR_ALL);
        switch_mmio_write32(gCar, info.enable + 4, 1u << info.bit);  // clock off
        b.ready = false;
    }
    IOLockUnlock(b.lock);
}

extern "C" void tegra_spi_resume(unsigned bus)
{
    if (bus < 1 || bus > kBuses || !kBus[bus - 1].regs)
        return;
    Bus &b = gBus[bus - 1];
    if (!b.lock || !b.irqNub)
        return;
    const BusInfo &info = kBus[bus - 1];
    IOLockLock(b.lock);
    if (!b.ready) {
        clockEnable(info);
        pinmux(info);
        const int result = configure(b, info);
        if (result != TEGRA_SPI_OK)
            panic("TegraSPI: SPI%u did not come back after sleep (%d)", bus, result);
        b.irqNub->enableInterrupt(0);
        b.ready = true;
    }
    IOLockUnlock(b.lock);
}
