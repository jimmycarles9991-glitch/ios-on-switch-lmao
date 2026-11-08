// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 I2C master: packet mode with PIO FIFOs, one IOLock per bus, every completion by interrupt.
// Register layout and bring-up follow hekate bdk/soc/{i2c,clock,pinmux}.c (proven on this board); packet headers,
// FIFO triggers, the interrupt handler and error handling follow Linux i2c-tegra.c (tegra_i2c_xfer_msg,
// tegra_i2c_isr; T210 has_config_load_reg, has_per_pkt_xfer_complete_irq, supports_bus_clear).
//
// Chained buses (I2C3, the touch panel's): a write-then-read is one submission, as L4T's
// tegra_i2c_multi_pkt_xfer sends an i2c_transfer of several messages on T210 (switchroot's 4.9 kernel, the one that
// drives this board's FTM4): the write packet (REPEAT_START, no per-packet interrupt), its payload and the read packet's
// header go into the TX FIFO together (at most 3 + 2 + 3 words, so writes of up to 8 bytes), and the one completion is
// ALL_PACKETS_XFER_COMPLETE, after the read. One interrupt and one wake-up per register read instead of two of each and
// the wait between the packets. A read that fits the 32-byte RX FIFO (hekate i2c_xfer_packet; L4T
// I2C_PIO_MODE_MAX_LEN) leaves the RX request masked and is drained at the completion, so it takes no RX interrupts.
// A longer read goes by APB DMA (request 23, tegra210.dtsi i2c@7000c500 dmas; one word per request at the 1-word RX
// trigger); the completion then also waits for the channel's end of count. Each stays off until the bus's owner has
// checked it once against the one-packet path (tegra_i2c_transfer_chained / _dma, TegraI2C.h), and a check that fails
// costs one line and that path for the boot, never a panic: a chain whose completion never comes times out (nothing is
// left asserted), and a DMA end of count that has never reached the CPU is reported, not panicked on; once one has, a
// lost one panics. Other buses keep one packet per message.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/OSAtomic.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/types.h>
#include <string.h>
#include "TegraI2C.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraAPBDMA/TegraAPBDMA.h"
#ifndef TEGRA_I2C_HOST_MODEL
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIO.h"
// kernel/switch/diag/threads.c
extern "C" void switch_print_thread(const void *thread, const char *who);
extern "C" void switch_print_processors(const char *who);
extern "C" void switch_sched_describe(char *buffer, size_t size, const void *thread);
#endif

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern "C" boolean_t preemption_enabled(void);

namespace {
constexpr vm_offset_t kI2CPA = 0x7000C000, kI2CSize = 0x2000;          // I2C1..I2C6
constexpr vm_offset_t kCarPA = 0x60006000, kCarSize = 0x1000;          // CLK_RST_CONTROLLER
constexpr vm_offset_t kPinmuxPA = 0x70003000, kPinmuxSize = 0x1000;    // PINMUX_AUX
constexpr vm_offset_t kApbMiscPA = 0x70000000, kApbMiscSize = 0x1000;  // APB_MISC: the pads' drive (GP_*_CFGPADCTRL)
constexpr unsigned kBuses = 5;
constexpr uint32_t kClkM = 19200000;  // CLK_M = 38.4 MHz OSC / 2
constexpr uint32_t kBusDivide = 48;   // (TLOW 4 + THIGH 2 + 2) * (CLK_DIVISOR std/fast 5 + 1)
constexpr uint32_t kGraceMs = 50;     // after a timed-out wait, for an interrupt action already under way
constexpr unsigned kArbRetries = 3;   // multi-master bus: repeats of a transfer that lost arbitration
constexpr unsigned kFifoWords = 8;    // TX and RX FIFO depth, 32-bit words (hekate i2c_xfer_packet: 32 bytes each)
constexpr size_t kChainMaxWrite =
    static_cast<size_t>(kFifoWords - 6) * 4;  // a chain's write payload: both headers + it fit the TX FIFO
constexpr uint32_t kDmaBufferSize = 4096;     // TEGRA_I2C_MAX_TRANSFER

// Controller registers.
enum : uint32_t {
    CNFG = 0x00,
    SL_CNFG = 0x20,
    SL_ADDR1 = 0x2c,
    SL_ADDR2 = 0x30,
    TX_FIFO = 0x50,
    RX_FIFO = 0x54,
    PACKET_TRANSFER_STATUS = 0x58,
    FIFO_CONTROL = 0x5c,
    FIFO_STATUS = 0x60,
    INT_MASK = 0x64,
    INT_STATUS = 0x68,
    CLK_DIVISOR = 0x6c,
    BUS_CLEAR_CNFG = 0x84,
    BUS_CLEAR_STATUS = 0x88,
    CONFIG_LOAD = 0x8c,
    INTERFACE_TIMING_0 = 0x94,
};

enum : uint32_t {
    CNFG_DEBOUNCE_4T = 2u << 12,
    CNFG_PACKET_MODE = 1u << 10,
    CNFG_NEW_MASTER_FSM = 1u << 11,
    CNFG_MULTI_MASTER = 1u << 17,
    SL_CNFG_NACK = 1u << 1,
    SL_CNFG_NEWSL = 1u << 2,
    FIFO_FLUSH = 3u << 0,      // RX_FLUSH | TX_FLUSH
    FIFO_TX_TRIG_8 = 7u << 5,  // TX request when 8 slots are free, RX request at 1 word
    INT_RX_REQ = 1u << 0,
    INT_TX_REQ = 1u << 1,
    INT_ARB_LOST = 1u << 2,
    INT_NO_ACK = 1u << 3,
    INT_ALL_PACKETS_DONE = 1u << 6,  // the TX FIFO's last packet ended (L4T I2C_INT_ALL_PACKETS_XFER_COMPLETE)
    INT_PACKET_DONE = 1u << 7,
    PKT_ARB_LOST = 1u << 1,  // PACKET_TRANSFER_STATUS bits 1-3 (Atmosphere vapours/tegra/tegra_i2c.hpp)
    PKT_NOACK_DATA = 1u << 2,
    PKT_NOACK_ADDR = 1u << 3,
    INT_BUS_CLEAR_DONE = 1u << 11,
    MSTR_CONFIG_LOAD = 1u << 0,
    BC_ENABLE = 1u << 0,
    BC_CONFIG = (9u << 16) | (1u << 2) | (1u << 1),  // 9 pulses, STOP, terminate
    BC_STATUS = 1u << 0,
    // Packet header words (generic header 0, I2C header 1, protocol header 2).
    PKT0_I2C = (1u << 16) | (1u << 4),  // packet id 1, protocol I2C
    HDR_REPEAT_START = 1u << 16,
    HDR_IE = 1u << 17,
    HDR_READ = 1u << 19,
};

constexpr uint32_t kPinmuxI2C = 1u << 6;  // function 0 (I2Cx), input enabled, no pull
constexpr int kPending = 1;               // packet result while the transfer runs

struct BusInfo {
    uint16_t regs;                     // offset from kI2CPA
    uint16_t reset;                    // CAR RST_DEV_x_SET (CLR is +4)
    uint16_t enable;                   // CAR CLK_ENB_x_SET (CLR is +4)
    uint16_t source;                   // CAR CLK_SOURCE_I2Cx
    uint16_t resetState, enableState;  // CAR RST_DEVICES_x / CLK_OUT_ENB_x
    uint8_t bit;
    uint8_t divider;    // CLK_M divider: 4 -> 100 kHz, 1 -> 400 kHz
    uint16_t scl, sda;  // PINMUX_AUX offsets
    uint16_t spi;       // GIC SPI: Linux tegra210.dtsi:676/691/706/721/739, OLED DTB i2c@7000c000.. interrupts
    uint16_t sclDrive, sdaDrive;  // APB_MISC GP_*_I2C_*_CFGPADCTRL
    uint8_t pullDown;             // DRVDN, the pads' pull-down (sink) strength; DRVUP is 0
    bool chained;                 // write-then-read as one submission, short reads without RX interrupts (file comment)
    uint8_t dmaRequest;           // APB DMA request line for long reads (0: none), tegra210.dtsi dmas
    uint8_t dmaChannel;           // its TegraAPBDMA channel
};

// hekate clock.c _clock_i2c[] and pinmux.h PINMUX_AUX_X_I2C_SCL/SDA. The drive: Horizon's
// (Atmosphere pinmux_initial_drive_pad_config.inc: Gen1/Gen2/Cam/Pwr I2C 0x1f, Gen3 0x07) and L4T's for the OLED
// (tegra210b01-fric-pinmux.dtsi drive_default: the same pull-down strengths, pull-up 0); offsets from
// pinmux_drive_pad_characters.inc and Linux pinctrl-tegra210.c.
// Chained and DMA: I2C3 only (the touch panel); the other buses keep one packet per message.
constexpr BusInfo kBus[kBuses] = {
    {0x0000, 0x300, 0x320, 0x124, 0x004, 0x010, 12, 4, 0xbc, 0xc0, 38, 0x9a8, 0x9ac, 0x1f, false, 0, 0},  // I2C1 (L)
    {0x0400, 0x308, 0x328, 0x198, 0x008, 0x014, 22, 1, 0xc4, 0xc8, 84, 0x9b0, 0x9b4, 0x1f, false, 0, 0},  // I2C2 (H)
    {0x0500, 0x310, 0x330, 0x1b8, 0x00c, 0x018, 3, 1, 0xcc, 0xd0, 92, 0x9b8, 0x9bc, 0x07, true, TEGRA_APBDMA_REQ_I2C3,
     TEGRA_APBDMA_CH_I2C3_RX},  // I2C3 (U)
    {0x0700, 0x430, 0x440, 0x3c4, 0x358, 0x360, 7, 4, 0xd4, 0xd8, 120, 0x934, 0x938, 0x1f, false, 0,
     0},  // I2C4 (V), CAM_I2C pads
    {0x1000, 0x308, 0x328, 0x128, 0x008, 0x014, 15, 1, 0xdc, 0xe0, 53, 0xa6c, 0xa70, 0x1f, false, 0,
     0},  // I2C5 (H), PWR_I2C pads
};
constexpr uint32_t kDriveMask = (0x1fu << 20) | (0x1fu << 12);  // DRVUP 24:20, DRVDN 16:12

struct Bus {
    volatile uint8_t *regs;
    IOLock *lock;     // one transfer at a time
    IOLock *irqLock;  // hands a wait between the caller and the interrupt action
    IOInterruptEventSource *irq;
    uint32_t hz;
    bool ready;
    bool owned;        // we reset/clocked it, so error recovery may reset it again
    bool multiMaster;  // another master shares the pads: lost arbitration is retried, never reset or bus-cleared
    bool final;        // tegra_i2c_final_transfer() has the bus for the rest of the boot
    // The wait in flight (under irqLock): a packet, a chain (write packet + read packet, ended by doneBit
    // INT_ALL_PACKETS_DONE), or the bus clear. With dmaActive the APB DMA moves the read's words, not serviceFifo, and
    // the wait also needs the channel's end of count (dmaComplete) before it ends.
    const uint8_t *tx;
    uint8_t *rx;
    size_t len, done;
    bool active, busClear, overflow;
    bool packetDone, dmaActive, dmaComplete;
    uint32_t doneBit;
    int result;
    // APB DMA for long reads on a chained bus (file comment): the channel and its buffer once opened, the software source
    // its end of count signals on the work loop, and whether reads use it (the caller's check passed: tegra_i2c_set_dma).
    tegra_apbdma_channel *dma;
    tegra_apbdma_buffer dmaBuffer;
    IOInterruptEventSource *dmaDoneSource;
    bool dmaOn, dmaFailed;
    bool chainOn;     // chained write-then-read and drain-at-end short reads, after the owner's check passed
    bool dmaUnheard;  // a wait ended because the channel's first end of count never reached the CPU
    // tegra_i2c_get_counters: interrupt actions, transfers, reads by DMA.
    volatile uint32_t interrupts, transfers, dmaReads;
    // Diagnostics (tegra_i2c_describe): the last failed packet's INT_STATUS and PACKET_TRANSFER_STATUS, as the action
    // or the timeout found them before recovery reset the controller; the pads as the boot chain left them.
    uint32_t faultInt, faultPacket;
    uint8_t addr, faultAddr;  // the packet in flight's address; the failed one's
    bool faulted, padsSaved;
    uint32_t bootPad[2], bootDrive[2];
    uint32_t clearStatus;  // BUS_CLEAR_STATUS after the last bus clear
    int clearResult;       // and how its wait ended
};

IOLock *volatile gSetupLock;
volatile uint8_t *gI2C, *gCar, *gPinmux, *gApbMisc;
IOWorkLoop *gLoop;
Bus gBus[kBuses];

#ifndef TEGRA_I2C_HOST_MODEL  // SwitchTouch/test/switch_touch_test.cpp supplies these over its controller model
inline uint32_t r32(const volatile uint8_t *base, uint32_t o)
{
    return switch_mmio_read32(base, o);
}

inline void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    switch_mmio_write32(base, o, v);
}

// The DMA buffer's lines cleaned and invalidated: before the engine writes it (no dirty line lands on top later) and
// after (the CPU reads what the engine wrote, not a line it prefetched meanwhile). The engine does not snoop.
inline void dmaSync(const uint8_t *va, size_t size)
{
    switch_dcache_clean_invalidate(va, size);
}
#endif
inline uint64_t nowUs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

inline uint32_t intid(unsigned bus)
{
    return 32 + kBus[bus - 1].spi;
}

// I2C5 runs in MULTI_MASTER_MODE unless switch-no-dfll: the CPU may go on the DFLL (SwitchThermal's governed
// default), whose integrated I2C master writes VDD_CPU over the same PWR_I2C pads. L4T sets
// the bit on every T210 controller (i2c-tegra.c tegra_i2c_init, tegra210_i2c_hw.has_multi_master_mode) and answers
// arbitration loss with a re-init and -EAGAIN (no bus clear, which would cut into the other master's transfer); here
// the transfer is simply repeated.
bool multiMasterBus(unsigned bus)
{
    unsigned noDfll = 0;
    return bus == 5 && ((PE_parse_boot_argn("switch-no-dfll", &noDfll, sizeof(noDfll)) == 0) || (noDfll == 0u));
}

// Register handshakes without an interrupt of their own (FIFO flush, config load): a few microseconds.
bool waitClear(const Bus &b, uint32_t reg, uint32_t bits, uint32_t us)
{
    for (const uint64_t end = nowUs() + us; r32(b.regs, reg) & bits;) {
        if (nowUs() > end)
            return (r32(b.regs, reg) & bits) == 0u;
    }
    return true;
}

bool loadConfig(const Bus &b)
{
    w32(b.regs, CONFIG_LOAD, MSTR_CONFIG_LOAD);
    return waitClear(b, CONFIG_LOAD, MSTR_CONFIG_LOAD, 2000);
}

// hekate clock_enable(): reset, gate, select CLK_M / divider, ungate, release reset.
void clockEnable(const BusInfo &info)
{
    const uint32_t bit = 1u << info.bit;
    w32(gCar, info.reset, bit);
    w32(gCar, info.enable + 4, bit);
    w32(gCar, info.source, (6u << 29) | (info.divider - 1u));
    w32(gCar, info.enable, bit);
    IODelay(2);
    w32(gCar, info.reset + 4, bit);
    (void)r32(gCar, info.reset);
}

// The bus's two pads: function I2Cx with the input on (hekate pinmux_config_i2c), and on I2C2 the pull-down strength
// Horizon and L4T give the OLED's GEN2_I2C pads (kBus): pinmux 0x40 is Atmosphere pinmux_initial_pad_config_aula.inc
// Gen2I2cScl/Sda (I2C2, no pull, driven, input, normal voltage) and L4T tegra210b01-fric-pinmux.dtsi; the drive is
// Atmosphere pinmux_initial_drive_pad_config.inc (DRVDN 0x1f, DRVUP 0, the non-Hoag table) and L4T's drive_default.
// The ALS NACKs at reset strength as at this one, so the drive is not its problem. The other buses keep their
// current strength (touch, the PMIC and the camera bus work with it; a wrong drive on PWR_I2C would cost the boot). The
// values found first are kept for tegra_i2c_describe().
void padConfig(Bus &b, const BusInfo &info)
{
    if (!b.padsSaved) {
        b.bootPad[0] = r32(gPinmux, info.scl);
        b.bootPad[1] = r32(gPinmux, info.sda);
        b.bootDrive[0] = r32(gApbMisc, info.sclDrive);
        b.bootDrive[1] = r32(gApbMisc, info.sdaDrive);
        b.padsSaved = true;
    }
    w32(gPinmux, info.scl, kPinmuxI2C);
    w32(gPinmux, info.sda, kPinmuxI2C);
    if (info.sclDrive != kBus[1].sclDrive)
        return;
    w32(gApbMisc, info.sclDrive,
        (r32(gApbMisc, info.sclDrive) & ~kDriveMask) | static_cast<uint32_t>(info.pullDown) << 12);
    w32(gApbMisc, info.sdaDrive,
        (r32(gApbMisc, info.sdaDrive) & ~kDriveMask) | static_cast<uint32_t>(info.pullDown) << 12);
}

bool clockRunning(const BusInfo &info)
{
    const uint32_t bit = 1u << info.bit;
    return ((r32(gCar, info.enableState) & bit) != 0u) && ((r32(gCar, info.resetState) & bit) == 0u);
}

// Moves payload between the FIFOs and the packet: drains the RX FIFO for a read (discarding words past the end,
// which is an overflow), tops up the TX FIFO for a write. Nothing while the APB DMA takes the read's words.
void serviceFifo(Bus &b)
{
    if (b.dmaActive)
        return;
    const uint32_t fifo = r32(b.regs, FIFO_STATUS);
    if (b.rx) {
        for (uint32_t words = fifo & 0xf; words; --words) {
            const uint32_t word = r32(b.regs, RX_FIFO);
            if (b.done == b.len) {
                b.overflow = true;
                continue;
            }
            const size_t n = b.len - b.done < 4 ? b.len - b.done : 4;
            memcpy(b.rx + b.done, &word, n);
            b.done += n;
        }
    } else {
        for (uint32_t words = (fifo >> 4) & 0xf; words && b.done < b.len; --words) {
            uint32_t word = 0;
            const size_t n = b.len - b.done < 4 ? b.len - b.done : 4;
            memcpy(&word, b.tx + b.done, n);
            w32(b.regs, TX_FIFO, word);
            b.done += n;
        }
    }
}

// The packet's (or chain's) outcome once INT_STATUS shows an error or its completion (and, for a DMA read, the
// channel's end of count has come too), kPending otherwise. A chain's NACK on its write half ends it here as well:
// the controller stops at the NACK and the read never starts (ALL_PACKETS_DONE does not come).
int outcome(Bus &b, uint32_t status)
{
    if (status & (INT_NO_ACK | INT_ARB_LOST))
        return (status & INT_NO_ACK) ? TEGRA_I2C_ENACK : TEGRA_I2C_EARB;
    if (status & b.doneBit)
        b.packetDone = true;
    if (!b.packetDone || (b.dmaActive && !b.dmaComplete))
        return kPending;
    return b.done == b.len && !b.overflow ? TEGRA_I2C_OK : TEGRA_I2C_EIO;
}

// The wait ends (under irqLock): the controller's interrupts masked, a DMA read that did not finish stopped, the
// failure's registers kept for tegra_i2c_describe, the sleeper woken.
void finish(Bus &b, uint32_t status)
{
    if (b.result != TEGRA_I2C_OK && !b.busClear) {
        b.faultInt = status;
        b.faultPacket = r32(b.regs, PACKET_TRANSFER_STATUS);
        b.faultAddr = b.addr;
        b.faulted = true;
    }
    if (b.dmaActive && !b.dmaComplete)
        tegra_apbdma_stop(b.dma);
    w32(b.regs, INT_MASK, 0);
    b.active = false;
    IOLockWakeup(b.irqLock, &b.active, true);
}

// Work loop action for a bus's SPI (level; the GIC masks it while this runs). Mirrors tegra_i2c_isr: errors end
// the packet, FIFO requests move data, PACKET_DONE ends it; the TX request is masked once the payload is queued.
// NOLINTNEXTLINE(misc-const-correctness): IOInterruptEventSource::Action's signature
void interruptOccurred(OSObject *, IOInterruptEventSource *sender, int)
{
    for (Bus &b : gBus) {
        if (b.irq != sender)
            continue;
        IOLockLock(b.irqLock);
        b.interrupts = b.interrupts + 1;
        const uint32_t status = r32(b.regs, INT_STATUS);
        if (!b.active) {
            w32(b.regs, INT_MASK, 0);  // stale: nobody waits any more
        } else {
            if (b.busClear) {
                b.result = (status & INT_BUS_CLEAR_DONE) ? TEGRA_I2C_OK : kPending;
            } else {
                serviceFifo(b);
                b.result = outcome(b, status);
            }
            if (b.result != kPending) {
                finish(b, status);
            } else if (b.packetDone) {
                // A DMA read whose packets are done: only the channel's end of count is still to come.
                w32(b.regs, INT_MASK, r32(b.regs, INT_MASK) & ~b.doneBit);
            } else if (!b.busClear && !b.rx && b.done == b.len) {
                w32(b.regs, INT_MASK, r32(b.regs, INT_MASK) & ~INT_TX_REQ);
            }
        }
        w32(b.regs, INT_STATUS, status);  // the FIFO request bits are levels and ignore this
        IOLockUnlock(b.irqLock);
    }
}

// Primary interrupt context, CPU0 (TegraAPBDMA's filter, the end of count already acknowledged): the bus's own work-loop
// source takes it from here, so the end of count and the controller's interrupt are handled on one thread.
void dmaEnded(void *context)
{
    static_cast<Bus *>(context)->dmaDoneSource->interruptOccurred(nullptr, nullptr, 0);
}

// Work loop: a DMA read's words have all landed; the wait ends if the packets have too.
// NOLINTNEXTLINE(misc-const-correctness): IOInterruptEventSource::Action's signature
void dmaFinished(OSObject *, IOInterruptEventSource *sender, int)
{
    for (Bus &b : gBus) {
        if (b.dmaDoneSource != sender)
            continue;
        IOLockLock(b.irqLock);
        b.interrupts = b.interrupts + 1;
        if (b.active && b.dmaActive && !b.dmaComplete) {
            b.dmaComplete = true;
            b.done = b.len;
            b.result = outcome(b, 0);
            if (b.result != kPending)
                finish(b, 0);
        }
        IOLockUnlock(b.irqLock);
    }
}

// Caller holds irqLock and has unmasked the interrupts it waits for. Sleeps until the action ends the wait or `us`
// pass. Timing out while the controller still asserts an unmasked status bit means its interrupt is not reaching
// us: after a grace period for an action already under way, panic with the controller's and the GIC's state.
// Otherwise the device simply did not finish (clock stretching, a stuck bus): ETIMEDOUT.
int sleepForAction(unsigned bus, Bus &b, uint64_t us)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(us), kMicrosecondScale, &deadline);
#ifndef TEGRA_I2C_HOST_MODEL
    char atDeadline[320] = "";
#endif
    for (bool grace = false;; grace = true) {
        while (b.active && IOLockSleepDeadline(b.irqLock, &b.active, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
        }
        if (!b.active)
            return b.result;
        const uint32_t status = r32(b.regs, INT_STATUS);
        const uint32_t mask = r32(b.regs, INT_MASK);
        bool dmaEnd = false;
        if (b.dmaActive && !b.dmaComplete)
            (void)tegra_apbdma_moved(b.dma, &dmaEnd);
        if (!(status & mask) && !dmaEnd)
            break;
        if (grace) {
            if (dmaEnd && !tegra_apbdma_interrupts(b.dma)) {
                b.dmaUnheard = true;  // the caller's check reports it (TegraI2C.h), DMA stays off
                break;
            }
            if (dmaEnd) {
                char dma[256];
                tegra_apbdma_describe(b.dma, dma, sizeof(dma));
                panic("TegraI2C: I2C%u DMA end of count lost: %s", bus, dma);
            }
#ifndef TEGRA_I2C_HOST_MODEL
            // The line fired and its source masked it, but the shared work loop never ran the action: say where its
            // thread is (state, wait, stack) before the panic.
            IOLog("TegraI2C: at the deadline, work loop %s\n", atDeadline);
            switch_print_thread(gLoop->getThread(), "TegraI2C: work loop");
            switch_print_processors("TegraI2C");
            for (unsigned other = 1; other <= kBuses; ++other)
                IOLog("TegraI2C: I2C%u irqLock %p\n", other, gBus[other - 1].irqLock);
#endif
            char gic[192];
            switch_interrupt_describe(intid(bus), gic, sizeof(gic));
            panic("TegraI2C: I2C%u interrupt lost (INTID %u): INT_STATUS %#x INT_MASK %#x FIFO_STATUS %#x, %s", bus,
                  intid(bus), status, mask, r32(b.regs, FIFO_STATUS), gic);
        }
#ifndef TEGRA_I2C_HOST_MODEL
        switch_sched_describe(atDeadline, sizeof(atDeadline), gLoop->getThread());
#endif
        clock_interval_to_deadline(kGraceMs, kMillisecondScale, &deadline);
    }
    if (!b.busClear) {
        b.faultInt = r32(b.regs, INT_STATUS);
        b.faultPacket = r32(b.regs, PACKET_TRANSFER_STATUS);
        b.faultAddr = b.addr;
        b.faulted = true;
    }
    if (b.dmaActive && !b.dmaComplete)
        tegra_apbdma_stop(b.dma);
    w32(b.regs, INT_MASK, 0);
    b.active = false;
    return TEGRA_I2C_ETIMEDOUT;
}

// hekate i2c_init() plus Linux tegra_i2c_init(): master FSM in packet mode, interrupts masked, std/fast divisor 5,
// never answer as a slave, then a bus clear (completion by INT_BUS_CLEAR_DONE) so a device left holding SDA by an
// interrupted transfer releases it.
int configure(unsigned bus, Bus &b)
{
    w32(b.regs, CNFG,
        CNFG_NEW_MASTER_FSM | CNFG_PACKET_MODE | CNFG_DEBOUNCE_4T | (b.multiMaster ? CNFG_MULTI_MASTER : 0));
    w32(b.regs, INT_MASK, 0);
    w32(b.regs, CLK_DIVISOR, (5u << 16) | 1u);
    w32(b.regs, SL_CNFG, r32(b.regs, SL_CNFG) | SL_CNFG_NACK | SL_CNFG_NEWSL);
    w32(b.regs, SL_ADDR1, 0xfc);
    w32(b.regs, SL_ADDR2, 0);
    w32(b.regs, FIFO_CONTROL, FIFO_FLUSH);
    if (!waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000) || !loadConfig(b)) {
        IOLog("TegraI2C: I2C%u config load timed out\n", bus);
        return TEGRA_I2C_ETIMEDOUT;
    }
    w32(b.regs, BUS_CLEAR_CNFG, BC_CONFIG);
    if (!loadConfig(b))
        return TEGRA_I2C_ETIMEDOUT;
    IOLockLock(b.irqLock);
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    b.busClear = true;
    b.packetDone = b.dmaActive = false;
    b.result = kPending;
    b.active = true;
    w32(b.regs, INT_MASK, INT_BUS_CLEAR_DONE);
    w32(b.regs, BUS_CLEAR_CNFG, BC_CONFIG | BC_ENABLE);
    const int cleared = sleepForAction(bus, b, 10000);
    b.busClear = false;
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    b.clearResult = cleared;
    b.clearStatus = r32(b.regs, BUS_CLEAR_STATUS);
    IOLockUnlock(b.irqLock);
    if (cleared != TEGRA_I2C_OK || !(b.clearStatus & BC_STATUS)) {
        IOLog("TegraI2C: I2C%u bus clear did not complete\n", bus);
    }
    return TEGRA_I2C_OK;
}

// The bus's SPI on the shared work loop; switch_interrupt_nub() sleeps until the GIC is up. Only called once the
// controller is clocked, so the action never reads an unclocked block.
void attachInterrupt(unsigned bus, Bus &b)
{
    if (b.irq)
        return;
    w32(b.regs, INT_MASK, 0);
    IOService *nub = switch_interrupt_nub(intid(bus), false);
    b.irq = IOInterruptEventSource::interruptEventSource(gLoop, &interruptOccurred, nub, 0);
    if (!b.irq || gLoop->addEventSource(b.irq) != kIOReturnSuccess) {
        panic("TegraI2C: cannot attach the I2C%u interrupt (INTID %u)", bus, intid(bus));
    }
    b.irq->enable();
}

int bringUp(unsigned bus, Bus &b)
{
    const BusInfo &info = kBus[bus - 1];
    b.regs = gI2C + info.regs;
    b.multiMaster = multiMasterBus(bus);
    if (bus == 5 && clockRunning(info)) {
        // PWR_I2C left clocked by U-Boot (PLLP, 400 kHz): adopt its clock and config, never reset or re-clock it.
        b.owned = false;
        b.hz = 400000;
        const uint32_t cnfg = r32(b.regs, CNFG);
        const uint32_t want = cnfg | CNFG_PACKET_MODE | CNFG_NEW_MASTER_FSM | (b.multiMaster ? CNFG_MULTI_MASTER : 0);
        if (cnfg != want) {
            w32(b.regs, CNFG, want);
            if (!loadConfig(b))
                return TEGRA_I2C_ETIMEDOUT;
        }
        attachInterrupt(bus, b);
        return TEGRA_I2C_OK;
    }
    b.owned = true;
    b.hz = kClkM / info.divider / kBusDivide;
    clockEnable(info);
    padConfig(b, info);
    if (bus == 3 || bus == 4) {
        // hekate pinmux_config_i2c(): detach the CAM_I2C (I2C4) pads from the I2C3 function.
        w32(gPinmux, kBus[3].scl, r32(gPinmux, kBus[3].scl) | 1u);
        w32(gPinmux, kBus[3].sda, r32(gPinmux, kBus[3].sda) | 1u);
    }
    attachInterrupt(bus, b);
    return configure(bus, b);
}

// Flush the FIFOs, then queue one packet: header and, for a write, as much payload as fits.
int startPacket(Bus &b, uint8_t addr, const uint8_t *out, uint8_t *in, size_t len, bool repeatStart)
{
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    w32(b.regs, FIFO_CONTROL, FIFO_TX_TRIG_8 | FIFO_FLUSH);
    if (!waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000))
        return TEGRA_I2C_ETIMEDOUT;
    b.tx = out;
    b.rx = in;
    b.len = len;
    b.done = 0;
    b.overflow = false;
    w32(b.regs, TX_FIFO, PKT0_I2C);
    w32(b.regs, TX_FIFO, static_cast<uint32_t>(len) - 1);
    w32(b.regs, TX_FIFO,
        HDR_IE | (repeatStart ? HDR_REPEAT_START : 0) | (in ? HDR_READ : 0) | (static_cast<uint32_t>(addr) << 1));
    if (out)
        serviceFifo(b);
    return TEGRA_I2C_OK;
}

// One packet: queue it, unmask the controller's interrupts (tegra_i2c_xfer_msg: errors and packet completion
// always, the FIFO request for the direction in use) and sleep until the action reports the outcome. The budget is
// 9 bits per byte plus START/address, twice over, and 5 ms for clock stretching. On a chained bus a read that fits the
// RX FIFO leaves its request masked: the completion's action drains it (file comment).
int packet(unsigned bus, Bus &b, uint8_t addr, const uint8_t *out, uint8_t *in, size_t len, bool repeatStart)
{
    IOLockLock(b.irqLock);
    int result = startPacket(b, addr, out, in, len, repeatStart);
    if (result == TEGRA_I2C_OK) {
        const bool drainAtEnd = (in != nullptr) && b.chainOn && (len + 3) / 4 <= kFifoWords;
        b.addr = addr;
        b.packetDone = b.dmaActive = false;
        b.doneBit = INT_PACKET_DONE;
        b.result = kPending;
        b.active = true;
        uint32_t fifoRequest = 0;
        if (in) {
            fifoRequest = drainAtEnd ? 0 : INT_RX_REQ;
        } else if (b.done < b.len) {
            fifoRequest = INT_TX_REQ;
        }
        w32(b.regs, INT_MASK, INT_NO_ACK | INT_ARB_LOST | INT_PACKET_DONE | fifoRequest);
        result = sleepForAction(bus, b, 5000 + static_cast<uint64_t>(len + 2) * 18 * 1000000 / b.hz);
        w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    }
    b.tx = nullptr;
    b.rx = nullptr;
    IOLockUnlock(b.irqLock);
    return result;
}

// A write of at most kChainMaxWrite bytes and a read after a repeated START, as one submission on a chained bus (file
// comment): flush, both packets queued (L4T tegra_i2c_prepare_buffer: no per-packet interrupt in a multi-packet
// transfer), errors and ALL_PACKETS_DONE unmasked, the RX request only for a PIO read longer than the RX FIFO. With
// `dma` the read's words go by the bus's APB DMA channel (started before the headers, as L4T
// tegra_i2c_start_dma_xfer) into its buffer, and the wait also needs the channel's end of count. One budget for
// both packets.
int chain(unsigned bus, Bus &b, uint8_t addr, const uint8_t *out, size_t outLen, uint8_t *in, size_t inLen, bool dma)
{
    const BusInfo &info = kBus[bus - 1];
    const uint32_t words = static_cast<uint32_t>((inLen + 3) / 4);
    IOLockLock(b.irqLock);
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    w32(b.regs, FIFO_CONTROL, FIFO_TX_TRIG_8 | FIFO_FLUSH);
    int result = waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000) ? TEGRA_I2C_OK : TEGRA_I2C_ETIMEDOUT;
    if (result == TEGRA_I2C_OK) {
        b.tx = nullptr;
        b.rx = dma ? nullptr : in;
        b.len = inLen;
        b.done = 0;
        b.overflow = false;
        b.packetDone = false;
        b.dmaActive = dma;
        b.dmaComplete = false;
        b.doneBit = INT_ALL_PACKETS_DONE;
        b.addr = addr;
        b.result = kPending;
        b.active = true;
        if (dma) {
            dmaSync(b.dmaBuffer.va, static_cast<size_t>(words) * 4);
            // Each 32-bit read of RX_FIFO pops one word of the read (four bytes; a UART's pops one byte).
            tegra_apbdma_start_rx_words(b.dma, static_cast<uint32_t>(kI2CPA + info.regs + RX_FIFO), info.dmaRequest,
                                        b.dmaBuffer.pa, words);
        }
        w32(b.regs, TX_FIFO, PKT0_I2C);
        w32(b.regs, TX_FIFO, static_cast<uint32_t>(outLen) - 1);
        w32(b.regs, TX_FIFO, HDR_REPEAT_START | (static_cast<uint32_t>(addr) << 1));
        for (size_t at = 0; at < outLen; at += 4) {
            uint32_t word = 0;
            memcpy(&word, out + at, outLen - at < 4 ? outLen - at : 4);
            w32(b.regs, TX_FIFO, word);
        }
        w32(b.regs, TX_FIFO, PKT0_I2C);
        w32(b.regs, TX_FIFO, static_cast<uint32_t>(inLen) - 1);
        w32(b.regs, TX_FIFO, HDR_READ | (static_cast<uint32_t>(addr) << 1));
        w32(b.regs, INT_MASK,
            INT_NO_ACK | INT_ARB_LOST | INT_ALL_PACKETS_DONE | (!dma && words > kFifoWords ? INT_RX_REQ : 0));
        result = sleepForAction(bus, b, 5000 + static_cast<uint64_t>(outLen + inLen + 4) * 18 * 1000000 / b.hz);
        w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
        if (result == TEGRA_I2C_OK && dma) {
            dmaSync(b.dmaBuffer.va, static_cast<size_t>(words) * 4);
            memcpy(in, b.dmaBuffer.va, inLen);
            b.dmaReads = b.dmaReads + 1;
        }
    }
    b.tx = nullptr;
    b.rx = nullptr;
    b.dmaActive = false;
    IOLockUnlock(b.irqLock);
    return result;
}

// The bus's DMA channel, its buffer and the work-loop source its end of count signals, once (thread context, b.lock
// held). False with the reason in `why`; the bus then stays on PIO for the boot.
bool openDma(const BusInfo &info, Bus &b, char *why, size_t whySize)
{
    if (b.dma)
        return true;
    if (b.dmaFailed) {
        snprintf(why, whySize, "its DMA could not be had earlier");
        return false;
    }
    b.dmaFailed = true;
    if (!b.dmaDoneSource) {
        b.dmaDoneSource = IOInterruptEventSource::interruptEventSource(gLoop, &dmaFinished, nullptr, 0);
        if (!b.dmaDoneSource || gLoop->addEventSource(b.dmaDoneSource) != kIOReturnSuccess) {
            snprintf(why, whySize, "no work-loop source for the DMA end of count");
            return false;
        }
        b.dmaDoneSource->enable();
    }
    if (!b.dmaBuffer.va && !tegra_apbdma_alloc(kDmaBufferSize, &b.dmaBuffer)) {
        snprintf(why, whySize, "no DMA buffer (%u bytes below 4 GiB)", kDmaBufferSize);
        return false;
    }
    char reason[96] = "";
    b.dma = tegra_apbdma_open(info.dmaChannel, gLoop, &dmaEnded, &b, reason, sizeof(reason));
    if (!b.dma) {
        snprintf(why, whySize, "APB DMA channel %u: %s", info.dmaChannel, reason);
        return false;
    }
    b.dmaFailed = false;
    return true;
}

// tegra_i2c_final_transfer()'s packet: the one polled transfer in this driver (TegraI2C.h). The controller's interrupts
// stay masked; INT_STATUS is read until an error or PACKET_DONE ends the packet, moving FIFO words on every pass, within
// the same budget packet() gives the interrupt. The caller holds irqLock when it could take it.
int pollPacket(Bus &b, uint8_t addr, const uint8_t *out, uint8_t *in, size_t len, bool repeatStart)
{
    int result = startPacket(b, addr, out, in, len, repeatStart);
    const uint64_t end = nowUs() + 5000 + static_cast<uint64_t>(len + 2) * 18 * 1000000 / b.hz;
    while (result == TEGRA_I2C_OK) {
        const uint32_t status = r32(b.regs, INT_STATUS);
        serviceFifo(b);
        const int done = outcome(b, status);
        if (done != kPending) {
            result = done;
            break;
        }
        if (nowUs() > end)
            result = TEGRA_I2C_ETIMEDOUT;
    }
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    b.tx = nullptr;
    b.rx = nullptr;
    return result;
}

// Reset after a failed transfer. irqLock is held while the clock is gated so no action reads an unclocked block.
// A multi-master bus only gets its FIFOs flushed: a reset's bus clear would cut into the other master's transfer.
void recover(unsigned bus, Bus &b)
{
    IOLockLock(b.irqLock);
    if (b.owned && !b.multiMaster) {
        clockEnable(kBus[bus - 1]);
        IOLockUnlock(b.irqLock);
        (void)configure(bus, b);
        return;
    }
    w32(b.regs, INT_MASK, 0);
    w32(b.regs, FIFO_CONTROL, r32(b.regs, FIFO_CONTROL) | FIFO_FLUSH);
    (void)waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000);
    w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
    IOLockUnlock(b.irqLock);
}

IOLock *setupLock()
{
    if (!gSetupLock) {
        IOLock *lock = IOLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, &gSetupLock))
            IOLockFree(lock);
    }
    return gSetupLock;
}
}

extern "C" int tegra_i2c_init(unsigned bus)
{
    if (bus < 1 || bus > kBuses)
        return TEGRA_I2C_EINVAL;
    IOLock *setup = setupLock();
    if (!setup)
        return TEGRA_I2C_ENOMEM;
    IOLockLock(setup);
    int result = TEGRA_I2C_OK;
    if (!gI2C) {
        gCar = (volatile uint8_t *)ml_io_map(kCarPA, kCarSize);
        gPinmux = (volatile uint8_t *)ml_io_map(kPinmuxPA, kPinmuxSize);
        gApbMisc = (volatile uint8_t *)ml_io_map(kApbMiscPA, kApbMiscSize);
        gI2C = (gCar && gPinmux && gApbMisc) ? (volatile uint8_t *)ml_io_map(kI2CPA, kI2CSize) : nullptr;
    }
    if (!gLoop)
        gLoop = IOWorkLoop::workLoop();
    Bus &b = gBus[bus - 1];
    if (!gI2C || !gLoop) {
        result = TEGRA_I2C_ENOMEM;
    } else if (!b.ready) {
        if (!b.lock)
            b.lock = IOLockAlloc();
        if (!b.irqLock)
            b.irqLock = IOLockAlloc();
        if (!b.lock || !b.irqLock) {
            result = TEGRA_I2C_ENOMEM;
        } else {
            IOLockLock(b.lock);
            result = bringUp(bus, b);  // claims the bus's interrupt (sleeps until the GIC is up)
            b.ready = result == TEGRA_I2C_OK;
            IOLockUnlock(b.lock);
        }
    }
    IOLockUnlock(setup);
    if (result != TEGRA_I2C_OK)
        IOLog("TegraI2C: I2C%u init failed (%d)\n", bus, result);
    return result;
}

extern "C" int tegra_i2c_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen)
{
    if (bus < 1 || bus > kBuses || addr > 0x7f || (!txlen && !rxlen) || txlen > TEGRA_I2C_MAX_TRANSFER ||
        rxlen > TEGRA_I2C_MAX_TRANSFER || (txlen && !tx) || (rxlen && !rx)) {
        return TEGRA_I2C_EINVAL;
    }
    Bus &b = gBus[bus - 1];
    if (!b.lock)
        return TEGRA_I2C_ENOTREADY;
    IOLockLock(b.lock);
    int result = b.ready ? TEGRA_I2C_OK : TEGRA_I2C_ENOTREADY;
    const bool chained = b.chainOn && (txlen != 0u) && (rxlen != 0u) && txlen <= kChainMaxWrite;
    const bool dma = chained && b.dmaOn && rxlen > static_cast<size_t>(kFifoWords) * 4 && rxlen <= b.dmaBuffer.size;
    if (result == TEGRA_I2C_OK)
        b.transfers = b.transfers + 1;
    for (unsigned attempt = 0; result == TEGRA_I2C_OK; ++attempt) {
        if (chained) {
            result = chain(bus, b, addr, (const uint8_t *)tx, txlen, (uint8_t *)rx, rxlen, dma);
        } else {
            if (txlen)
                result = packet(bus, b, addr, (const uint8_t *)tx, nullptr, txlen, rxlen != 0);
            if (result == TEGRA_I2C_OK && rxlen)
                result = packet(bus, b, addr, nullptr, (uint8_t *)rx, rxlen, false);
        }
        if (result == TEGRA_I2C_OK)
            break;
        recover(bus, b);
        if (result != TEGRA_I2C_EARB || !b.multiMaster || attempt == kArbRetries)
            break;
        IODelay(100);  // the other master's transfer (a few bytes at 400 kHz) finishes
        result = TEGRA_I2C_OK;
    }
    IOLockUnlock(b.lock);
    return result;
}

// System sleep (drivers/SwitchSuspend's sleep and wake actions; both on the same thread, IOCPUSleepKernel's). Suspend
// takes every ready bus's lock and keeps it, so a transfer started afterwards waits for resume instead of reaching a
// controller that SC7 is about to reset; that thread must not transfer again until it has called resume. Resume clocks
// and configures each bus again (after SC7 every controller and its CAR clock are back at reset, PWR_I2C included:
// U-Boot's setup is gone unless the BPMP firmware's SC7 exit re-clocked it, in which case it is adopted as at boot)
// and then lets the waiting transfers through.
extern "C" void tegra_i2c_suspend(void)
{
    for (unsigned bus = 1; bus <= kBuses; ++bus) {
        const Bus &b = gBus[bus - 1];
        if (!b.ready)
            continue;
        IOLockLock(b.lock);
        if (b.irq)
            b.irq->disable();
        IOLockLock(b.irqLock);
        w32(b.regs, INT_MASK, 0);
        IOLockUnlock(b.irqLock);
    }
}

extern "C" void tegra_i2c_resume(void)
{
    for (unsigned bus = 1; bus <= kBuses; ++bus) {
        Bus &b = gBus[bus - 1];
        if (!b.ready)
            continue;
        const BusInfo &info = kBus[bus - 1];
        const bool adopted = bus == 5 && clockRunning(info);
        if (!adopted) {
            clockEnable(info);
            padConfig(b, info);
            if (bus == 5)
                b.hz = kClkM / info.divider / kBusDivide;
        }
        if (b.irq)
            b.irq->enable();
        const int result = adopted ? TEGRA_I2C_OK : configure(bus, b);
        if (adopted) {
            w32(b.regs, CNFG,
                r32(b.regs, CNFG) | CNFG_PACKET_MODE | CNFG_NEW_MASTER_FSM | (b.multiMaster ? CNFG_MULTI_MASTER : 0));
            (void)loadConfig(b);
        }
        if (result != TEGRA_I2C_OK)
            panic("TegraI2C: I2C%u did not come back after sleep (%d)", bus, result);
        IOLockUnlock(b.lock);
    }
}

// Shut Down / Restart only (TegraI2C.h). Polling is acceptable here and nowhere else: these are the boot's last
// transfers, nothing is left to interrupt, and the caller may have interrupts off. A failed packet only has the FIFOs
// flushed (no reset, no bus clear): the caller has nothing left to retry with but its fallback.
extern "C" int tegra_i2c_final_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx,
                                        size_t rxlen)
{
    if (bus < 1 || bus > kBuses || addr > 0x7f || (!txlen && !rxlen) || txlen > TEGRA_I2C_MAX_TRANSFER ||
        rxlen > TEGRA_I2C_MAX_TRANSFER || (txlen && !tx) || (rxlen && !rx)) {
        return TEGRA_I2C_EINVAL;
    }
    Bus &b = gBus[bus - 1];
    if (!b.lock || !b.irqLock || !b.ready)
        return TEGRA_I2C_ENOTREADY;
    const bool canSleep = preemption_enabled() != 0;
    if (!b.final) {
        // Keep the bus from here on: a transfer in flight finishes first (by its interrupt) when this thread may
        // sleep; with interrupts off a busy bus is taken as it is, since nothing else runs here to finish it.
        if (canSleep) {
            IOLockLock(b.lock);
        } else {
            (void)IOLockTryLock(b.lock);
        }
        b.final = true;
    }
    const bool irqHeld = (canSleep ? static_cast<int>(IOLockLock(b.irqLock), true) : IOLockTryLock(b.irqLock)) != 0;
    b.active = false;  // a late action for this bus only masks and clears
    w32(b.regs, INT_MASK, 0);
    int result = TEGRA_I2C_OK;
    for (unsigned attempt = 0;; ++attempt) {
        if (txlen)
            result = pollPacket(b, addr, (const uint8_t *)tx, nullptr, txlen, rxlen != 0);
        if (result == TEGRA_I2C_OK && rxlen)
            result = pollPacket(b, addr, nullptr, (uint8_t *)rx, rxlen, false);
        if (result == TEGRA_I2C_OK)
            break;
        w32(b.regs, FIFO_CONTROL, r32(b.regs, FIFO_CONTROL) | FIFO_FLUSH);
        (void)waitClear(b, FIFO_CONTROL, FIFO_FLUSH, 2000);
        w32(b.regs, INT_STATUS, r32(b.regs, INT_STATUS));
        if (result != TEGRA_I2C_EARB || !b.multiMaster || attempt == kArbRetries)
            break;
        IODelay(100);  // the other master's transfer (a few bytes at 400 kHz) finishes
        result = TEGRA_I2C_OK;
    }
    if (irqHeld)
        IOLockUnlock(b.irqLock);
    return result;
}

#ifndef TEGRA_I2C_HOST_MODEL
// tegra_i2c_raw_write (TegraI2C.h): no Bus state, no lock; the controller re-clocked and set up only if its clock or
// reset says it lost that (SC7), then one packet polled to its end.
extern "C" int tegra_i2c_raw_write(unsigned bus, uint8_t addr, const uint8_t *data, size_t len)
{
    if (bus < 1 || bus > kBuses || addr > 0x7f || !data || !len || len > 8)
        return TEGRA_I2C_EINVAL;
    if (!gI2C)
        return TEGRA_I2C_ENOTREADY;
    const BusInfo &info = kBus[bus - 1];
    volatile uint8_t *regs = gI2C + info.regs;
    const auto settled = [regs](uint32_t reg, uint32_t bits) {
        for (const uint64_t end = nowUs() + 2000; r32(regs, reg) & bits;) {
            if (nowUs() > end)
                return !(r32(regs, reg) & bits);
        }
        return true;
    };
    if (!clockRunning(info)) {
        clockEnable(info);
        w32(gPinmux, info.scl, kPinmuxI2C);
        w32(gPinmux, info.sda, kPinmuxI2C);
        w32(regs, CNFG,
            CNFG_NEW_MASTER_FSM | CNFG_PACKET_MODE | CNFG_DEBOUNCE_4T | (multiMasterBus(bus) ? CNFG_MULTI_MASTER : 0));
        w32(regs, CLK_DIVISOR, (5u << 16) | 1u);
        w32(regs, CONFIG_LOAD, MSTR_CONFIG_LOAD);
        if (!settled(CONFIG_LOAD, MSTR_CONFIG_LOAD))
            return TEGRA_I2C_ETIMEDOUT;
    }
    w32(regs, INT_MASK, 0);
    w32(regs, INT_STATUS, r32(regs, INT_STATUS));
    w32(regs, FIFO_CONTROL, FIFO_FLUSH);
    if (!settled(FIFO_CONTROL, FIFO_FLUSH))
        return TEGRA_I2C_ETIMEDOUT;
    w32(regs, TX_FIFO, PKT0_I2C);
    w32(regs, TX_FIFO, (uint32_t)len - 1);
    w32(regs, TX_FIFO, HDR_IE | (uint32_t)addr << 1);
    for (size_t i = 0; i < len; i += 4) {
        uint32_t word = 0;
        for (size_t j = 0; j < 4 && i + j < len; ++j)
            word |= (uint32_t)data[i + j] << (8 * j);
        w32(regs, TX_FIFO, word);
    }
    int result = TEGRA_I2C_ETIMEDOUT;
    for (const uint64_t end = nowUs() + 5000 + (uint64_t)(len + 2) * 18 * 1000000 / 100000; nowUs() <= end;) {
        const uint32_t status = r32(regs, INT_STATUS);
        if (status & (INT_NO_ACK | INT_ARB_LOST)) {
            result = (status & INT_NO_ACK) ? TEGRA_I2C_ENACK : TEGRA_I2C_EARB;
            break;
        }
        if (status & INT_PACKET_DONE) {
            result = TEGRA_I2C_OK;
            break;
        }
    }
    w32(regs, INT_STATUS, r32(regs, INT_STATUS));
    return result;
}
#endif

static const char *packetFailure(uint32_t status)
{
    if (status & PKT_NOACK_ADDR)
        return "address NACK";
    if (status & PKT_NOACK_DATA)
        return "data NACK";
    return (status & PKT_ARB_LOST) ? "arbitration lost" : "no NACK flag";
}

// Thread context; waits for a transfer in flight. Registers are read only while the controller is clocked.
extern "C" void tegra_i2c_describe(unsigned bus, char *buffer, size_t size)
{
    if (!size)
        return;
    if (bus < 1 || bus > kBuses || !gBus[bus - 1].lock) {
        snprintf(buffer, size, "I2C%u: not initialised", bus);
        return;
    }
    const Bus &b = gBus[bus - 1];
    const BusInfo &info = kBus[bus - 1];
    IOLockLock(b.lock);
    char fault[112] = "no failed packet";
    if (b.faulted) {
        const uint32_t p = b.faultPacket;
        // Bits 15:4, the bytes transferred (TRANSFER_BYTENUM in the T210 TRM as remembered; the raw value is printed).
        snprintf(fault, sizeof(fault), "last failure 0x%02x: INT_STATUS %#x PACKET_TRANSFER_STATUS %#x (%s, 15:4 %u)",
                 b.faultAddr, b.faultInt, p, packetFailure(p), (p >> 4) & 0xfff);
    }
    const bool clocked = (b.regs != nullptr) && clockRunning(info);
    snprintf(buffer, size,
             "I2C%u: pinmux %#x %#x (boot %#x %#x), drive %#x %#x (boot %#x %#x); %s CNFG %#x CLK_DIVISOR %#x "
             "TIMING0 %#x, last bus clear %d BUS_CLEAR_STATUS %#x; %s",
             bus, r32(gPinmux, info.scl), r32(gPinmux, info.sda), b.bootPad[0], b.bootPad[1],
             r32(gApbMisc, info.sclDrive), r32(gApbMisc, info.sdaDrive), b.bootDrive[0], b.bootDrive[1],
             clocked ? "clocked," : "NOT clocked,", clocked ? r32(b.regs, CNFG) : 0,
             clocked ? r32(b.regs, CLK_DIVISOR) : 0, clocked ? r32(b.regs, INTERFACE_TIMING_0) : 0, b.clearResult,
             b.clearStatus, fault);
    IOLockUnlock(b.lock);
}

namespace {
// One chained transfer for a check (TegraI2C.h), PIO or DMA, whatever the bus's modes: the owner compares the data
// with what the one-packet path read. A failure leaves the reason in `why` and the bus recovered.
int checkedChain(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen, bool dma, char *why,
                 size_t whySize)
{
    if (!whySize)
        return TEGRA_I2C_EINVAL;
    why[0] = 0;
    if (bus < 1 || bus > kBuses || addr > 0x7f || !tx || !rx || !txlen || txlen > kChainMaxWrite || !rxlen ||
        rxlen > (dma ? kDmaBufferSize : static_cast<size_t>(TEGRA_I2C_MAX_TRANSFER))) {
        snprintf(why, whySize, "bad arguments");
        return TEGRA_I2C_EINVAL;
    }
    const BusInfo &info = kBus[bus - 1];
    Bus &b = gBus[bus - 1];
    if (!info.chained || (dma && !info.dmaRequest)) {
        snprintf(why, whySize, "I2C%u has no %s path", bus, dma ? "DMA read" : "chained");
        return TEGRA_I2C_EINVAL;
    }
    if (!b.lock) {
        snprintf(why, whySize, "I2C%u not initialised", bus);
        return TEGRA_I2C_ENOTREADY;
    }
    IOLockLock(b.lock);
    int result = b.ready ? TEGRA_I2C_OK : TEGRA_I2C_ENOTREADY;
    if (result == TEGRA_I2C_ENOTREADY)
        snprintf(why, whySize, "I2C%u not initialised", bus);
    if (result == TEGRA_I2C_OK && dma && !b.chainOn) {
        snprintf(why, whySize, "it needs the chained path, which is off");
        result = TEGRA_I2C_ENODMA;
    }
    if (result == TEGRA_I2C_OK && dma && !openDma(info, b, why, whySize))
        result = TEGRA_I2C_ENODMA;
    if (result == TEGRA_I2C_OK) {
        b.transfers = b.transfers + 1;
        b.dmaUnheard = false;
        result = chain(bus, b, addr, static_cast<const uint8_t *>(tx), txlen, static_cast<uint8_t *>(rx), rxlen, dma);
        if (result != TEGRA_I2C_OK) {
            char state[192];
            if (dma) {
                tegra_apbdma_describe(b.dma, state, sizeof(state));
            } else {
                snprintf(state, sizeof(state), "INT_STATUS %#x PACKET_TRANSFER_STATUS %#x", b.faultInt, b.faultPacket);
            }
            const char *what = dma ? "DMA read failed" : "chained read failed";
            if (b.dmaUnheard)
                what = "its first end of count never reached the CPU";
            snprintf(why, whySize, "%s (%d): %s", what, result, state);
            recover(bus, b);
        }
    }
    IOLockUnlock(b.lock);
    return result;
}
}  // namespace

extern "C" int tegra_i2c_transfer_chained(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx,
                                          size_t rxlen, char *why, size_t whySize)
{
    return checkedChain(bus, addr, tx, txlen, rx, rxlen, false, why, whySize);
}

extern "C" int tegra_i2c_transfer_dma(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen,
                                      char *why, size_t whySize)
{
    return checkedChain(bus, addr, tx, txlen, rx, rxlen, true, why, whySize);
}

extern "C" void tegra_i2c_set_chained(unsigned bus, bool on)
{
    if (bus < 1 || bus > kBuses || !gBus[bus - 1].lock)
        return;
    Bus &b = gBus[bus - 1];
    IOLockLock(b.lock);
    b.chainOn = on && kBus[bus - 1].chained;
    if (!b.chainOn)
        b.dmaOn = false;
    IOLockUnlock(b.lock);
}

extern "C" void tegra_i2c_set_dma(unsigned bus, bool on)
{
    if (bus < 1 || bus > kBuses || !gBus[bus - 1].lock)
        return;
    Bus &b = gBus[bus - 1];
    IOLockLock(b.lock);
    b.dmaOn = on && (b.dma != nullptr) && b.chainOn;
    IOLockUnlock(b.lock);
}

extern "C" void tegra_i2c_get_counters(unsigned bus, struct tegra_i2c_counters *out)
{
    *out = {};
    if (bus < 1 || bus > kBuses)
        return;
    const Bus &b = gBus[bus - 1];
    out->transfers = b.transfers;
    out->interrupts = b.interrupts;
    out->dmaReads = b.dmaReads;
    out->dmaOff = !b.dmaOn;
    out->chainOff = !b.chainOn;
}

extern "C" int tegra_i2c_read(unsigned bus, uint8_t addr, uint8_t reg, void *buf, size_t len)
{
    if (!len)
        return TEGRA_I2C_EINVAL;
    return tegra_i2c_transfer(bus, addr, &reg, 1, buf, len);
}

extern "C" int tegra_i2c_write(unsigned bus, uint8_t addr, uint8_t reg, const void *buf, size_t len)
{
    if (len > TEGRA_I2C_MAX_REG_WRITE || (len && !buf))
        return TEGRA_I2C_EINVAL;
    uint8_t data[TEGRA_I2C_MAX_REG_WRITE + 1];
    data[0] = reg;
    if (len)
        memcpy(data + 1, buf, len);
    return tegra_i2c_transfer(bus, addr, data, len + 1, nullptr, 0);
}
