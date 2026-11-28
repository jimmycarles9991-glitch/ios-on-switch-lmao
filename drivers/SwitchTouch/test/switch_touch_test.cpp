// Host test of drivers/SwitchTouch/SwitchTouch.cpp over drivers/TegraI2C/TegraI2C.cpp: the two drivers and this file
// are separate translation units over switch_touch_stubs.h (BUILD.bazel), and each scenario runs in a process of its
// own (the drivers keep boot-wide state). Everything below the drivers is a model:
//   * Tegra210 I2C controllers (all five; I2C3 and I2C5 have devices): packet-mode TX and RX FIFOs of 8 words, packet
//     headers parsed from the TX FIFO (generic header, payload size, I2C header with READ / REPEAT_START / IE), bytes on
//     the wire at 400 kHz (9 bit times each, 10 for (repeated) START + address), a write stalls on an empty TX FIFO and
//     a read on a full RX FIFO, PACKET_DONE for a packet with IE, ALL_PACKETS_DONE when a packet ends with the TX FIFO
//     empty, NO_ACK for an absent address and ARB_LOST for a packet another master took the bus from (the controller
//     stops until its FIFOs are flushed), RX/TX requests as levels
//     from the FIFO_CONTROL triggers, write-1-to-clear INT_STATUS, self-clearing flush and config load, the bus clear.
//     A line (INT_STATUS & INT_MASK) reaches its work-loop action gWorkLoopNs later, as IOInterruptEventSource does.
//   * APB DMA channel 4 (TegraAPBDMA.h's API): while enabled on request 23 it moves one word per request from I2C3's RX
//     FIFO (at its 1-word trigger) into the buffer, then raises its end of count (the handler, in "primary interrupt
//     context"); it can be broken (never moves) or lose its interrupt.
//   * The FTM4 on I2C3 0x49 (Linux ftm4_ts.c / hekate touch.c commands): hardware register reads (B6: chip ID block,
//     event count), the event FIFO (86 pops 8 bytes per event, 85 one event), system reset (ready event), sense mode,
//     sense on, clear, interrupt enable; INT (PX1) low while enabled with events queued. Its scans are scheduled by
//     each scenario.
//   * MAX77620 on I2C5 0x3c: a register file (LDO6).
// Scenarios check the wire traffic per touch interrupt (exactly the count read and the event read), one controller
// interrupt per chained transaction, the DMA path and its self-check, HID report times (the filter's, strictly
// increasing), the stats line, and the one-line panics.
#include <algorithm>
#include <array>
#include <cinttypes>
#include <deque>
#include <memory>
#include <set>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "bazel/testing/fresh_process.h"
#include "bazel/testing/source_text.h"
#include "switch_touch_stubs.h"
#include "tegra_i2c_host_model.h"
#include "../SwitchHID.h"
#include "../SwitchTouch.h"
#include "../../TegraAPBDMA/TegraAPBDMA.h"
#include "../../TegraGPIO/TegraGPIO.h"
#include "../../TegraI2C/TegraI2C.h"

// Every check is a GoogleTest expectation; its message says what was expected.
#define check(ok, what) EXPECT_TRUE(ok) << (what)
static std::string format(const char *f, ...) __attribute__((format(printf, 1, 2)));

static std::string format(const char *f, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(text, sizeof(text), f, ap);
    va_end(ap);
    return text;
}

// --- I2C devices -----------------------------------------------------------------------------------------------------
struct Device {
    virtual ~Device() = default;
    virtual void begin(bool read) = 0;  // (repeated) START with this device's address, acknowledged
    virtual void write(uint8_t value) = 0;
    virtual uint8_t read() = 0;
    virtual void stop() = 0;
};

using Event = std::array<uint8_t, 8>;

// A contact event at FTM4 raw (x, y) (the landscape frame, 15..1264 x 15..704) with strength z (gain byte 0: z as is).
static Event contactEvent(unsigned type, unsigned id, unsigned x, unsigned y, unsigned z = 16)
{
    return {(uint8_t)(id << 4 | type),
            (uint8_t)(x >> 4),
            (uint8_t)(y >> 4),
            (uint8_t)((x & 0xf) << 4 | (y & 0xf)),
            (uint8_t)z,
            (uint8_t)(z >> 8),
            0,
            0};
}

struct Ftm4 : Device {
    std::deque<Event> fifo;
    bool intEnabled = false, sensing = false, reading = false;
    bool burstZeroPad = true;
    uint8_t mode = 0;
    std::vector<uint8_t> command;
    std::vector<uint8_t> out;  // a register read's bytes
    size_t readAt = 0;
    Event current = {};
    std::vector<std::string> ops;  // "R b6 00 23 x2", "W c3 01"
    std::vector<std::string> errors;

    bool intLow() const
    {
        return intEnabled && !fifo.empty();
    }

    std::string hex() const
    {
        std::string text;
        for (uint8_t b : command)
            text += format("%s%02x", text.empty() ? "" : " ", b);
        return text;
    }

    void begin(bool read) override
    {
        if (!read) {
            command.clear();
            reading = false;
            return;
        }
        reading = true;
        readAt = 0;
        out.clear();
        if (command.size() == 3 && command[0] == 0xb6) {
            const unsigned reg = command[1] << 8 | command[2];
            if (reg == 0x0004)
                out = {0x5a, 0x36, 0x70, 0x01, 0x00, 0x64, 0x23};  // byte 0: the dummy byte
            else if (reg == 0x0023)
                out = {0x5a, (uint8_t)(std::min<size_t>(fifo.size(), 127) << 1)};
            else
                errors.push_back(format("read of unmodelled register %04x", reg));
        } else if (command.size() != 1 || (command[0] != 0x86 && command[0] != 0x85)) {
            errors.push_back("read after unknown command " + hex());
        }
    }

    void write(uint8_t value) override
    {
        command.push_back(value);
    }

    uint8_t read() override
    {
        const size_t at = readAt++;
        if (!out.empty() || command[0] == 0xb6)
            return at < out.size() ? out[at] : 0;
        if (command[0] == 0x85 && at >= 8)
            return 0;
        if (at % 8 == 0) {  // the next event leaves the FIFO as its first byte is read
            current = {};
            if (!fifo.empty()) {
                current = fifo.front();
                fifo.pop_front();
            } else if (!burstZeroPad && command[0] == 0x86) {
                current.fill(0xaa);
            }
        }
        return current[at % 8];
    }

    void stop() override
    {
        if (reading) {
            ops.push_back(format("R %s x%zu", hex().c_str(), readAt));
            reading = false;
            return;
        }
        ops.push_back("W " + hex());
        const std::vector<uint8_t> c = command;
        if (c == std::vector<uint8_t>{0xb6, 0x00, 0x28, 0x80}) {
            fifo.clear();
            intEnabled = sensing = false;
            fifo.push_back({0x10, 0, 0, 0, 0, 0, 0, 0});  // controller ready
        } else if (c.size() == 2 && c[0] == 0xc3) {
            mode = c[1];
        } else if (c == std::vector<uint8_t>{0x93}) {
            sensing = true;
        } else if (c == std::vector<uint8_t>{0xa1}) {
            fifo.clear();
        } else if (c == std::vector<uint8_t>{0xb6, 0x00, 0x2c, 0x48}) {
            intEnabled = true;
        } else {
            errors.push_back("unknown command " + hex());
        }
    }

    void scan(const std::vector<Event> &events)
    {
        if (!sensing)
            return;
        for (const Event &e : events)
            fifo.push_back(e);
    }
};

struct Pmic : Device {
    uint8_t regs[256] = {};
    std::vector<uint8_t> command;
    bool reading = false;
    size_t at = 0;

    void begin(bool read) override
    {
        reading = read;
        if (!read)
            command.clear();
        at = 0;
    }

    void write(uint8_t value) override
    {
        command.push_back(value);
    }

    uint8_t read() override
    {
        return regs[(uint8_t)(command[0] + at++)];
    }

    void stop() override
    {
        if (!reading)
            for (size_t i = 1; i < command.size(); ++i)
                regs[(uint8_t)(command[0] + i - 1)] = command[i];
    }
};

// --- I2C controllers -------------------------------------------------------------------------------------------------
enum : uint32_t {
    CNFG = 0x00,
    TX_FIFO = 0x50,
    RX_FIFO = 0x54,
    PACKET_TRANSFER_STATUS = 0x58,
    FIFO_CONTROL = 0x5c,
    FIFO_STATUS = 0x60,
    INT_MASK = 0x64,
    INT_STATUS = 0x68,
    BUS_CLEAR_CNFG = 0x84,
    BUS_CLEAR_STATUS = 0x88,
    CONFIG_LOAD = 0x8c,
};

enum : uint32_t {
    INT_RX_REQ = 1u << 0,
    INT_TX_REQ = 1u << 1,
    INT_ARB_LOST = 1u << 2,
    INT_NO_ACK = 1u << 3,
    INT_ALL_PACKETS = 1u << 6,
    INT_PACKET_DONE = 1u << 7,
    INT_BUS_CLEAR_DONE = 1u << 11,
    HDR_REPEAT_START = 1u << 16,
    HDR_IE = 1u << 17,
    HDR_READ = 1u << 19,
};

constexpr uint64_t kBitNs = 2500;  // 400 kHz

void dmaRxChanged(unsigned bus);
static bool gNoAllPackets = false;  // a controller that never raises ALL_PACKETS_DONE (the chain check must catch it)
static bool gEarlyDone = false;     // completions at the last byte, before a DMA end of count (the other order)

struct Controller {
    unsigned bus = 0;
    std::map<uint8_t, Device *> devices;
    uint32_t regs[0x100 / 4] = {};
    uint32_t latched = 0, intMask = 0, fifoControl = 0;
    std::deque<uint32_t> tx, rx;
    bool busy = false, held = false, stalled = false, failed = false, reading = false;
    uint32_t header[3] = {};
    size_t len = 0, done = 0;
    uint32_t txWord = 0, rxWord = 0;
    unsigned txLeft = 0, rxHave = 0;
    Device *device = nullptr;
    uint64_t generation = 0;  // a flush cancels the packet's scheduled bytes
    IOInterruptEventSource *source = nullptr;
    IOFilterInterruptEventSource *filter = nullptr;
    bool deliveryPending = false, stall = false;
    unsigned packets = 0, actionsSeen = 0, filterCalls = 0, fifoFlushes = 0;
    uint32_t everUnmasked = 0;
    unsigned arbLosses = 0;  // packets that lose the bus to another master at their START (I2C5's CL-DVFS master)
    std::vector<std::string> trace;  // per submission: "W3+rs" etc.

    unsigned rxTrigger() const
    {
        return ((fifoControl >> 2) & 7) + 1;
    }

    unsigned txTrigger() const
    {
        return ((fifoControl >> 5) & 7) + 1;
    }

    uint32_t status() const
    {
        uint32_t s = latched;
        if (rx.size() >= rxTrigger())
            s |= INT_RX_REQ;
        if (8 - tx.size() >= txTrigger())
            s |= INT_TX_REQ;
        return s;
    }

    void later(uint64_t bits, std::function<void()> what)
    {
        const uint64_t g = generation;
        gSchedule(gNow + bits * kBitNs, [this, g, what] {
            if (g == generation)
                what();
        });
    }

    void kick()
    {
        if (busy || failed || tx.size() < 3)
            return;
        for (uint32_t &h : header) {
            h = tx.front();
            tx.pop_front();
        }
        if ((header[0] & 0xf0) != 0x10) {
            failed = true;
            check(false, format("I2C%u: packet header 0 %#x is not an I2C packet", bus, header[0]));
            return;
        }
        len = (header[1] & 0xfff) + 1;
        done = 0;
        txLeft = rxHave = 0;
        rxWord = 0;
        reading = header[2] & HDR_READ;
        busy = true;
        const uint8_t addr = (header[2] >> 1) & 0x7f;
        trace.push_back(format("%c%zu%s%s", reading ? 'R' : 'W', len, header[2] & HDR_REPEAT_START ? "+rs" : "",
                               header[2] & HDR_IE ? "+ie" : ""));
        later(10, [this, addr] {
            addressed(addr);
        });
    }

    void addressed(uint8_t addr)
    {
        if (stall)
            return;
        if (arbLosses) {
            --arbLosses;
            latched |= INT_ARB_LOST;
            regs[PACKET_TRANSFER_STATUS / 4] = 1u << 1;  // ARB_LOST
            failed = true;
            busy = held = false;
            return;
        }
        auto it = devices.find(addr);
        if (it == devices.end()) {
            latched |= INT_NO_ACK;
            regs[PACKET_TRANSFER_STATUS / 4] = 1u << 3;  // NOACK_FOR_ADDR
            failed = true;
            busy = held = false;
            return;
        }
        device = it->second;
        device->begin(reading);
        nextByte();
    }

    void nextByte()
    {
        stalled = false;
        if (done == len) {
            ended();
            return;
        }
        if (!reading) {
            if (!txLeft) {
                if (tx.empty()) {
                    stalled = true;
                    return;
                }
                txWord = tx.front();
                tx.pop_front();
                txLeft = 4;
            }
            const uint8_t value = (uint8_t)txWord;
            txWord >>= 8;
            --txLeft;
            later(9, [this, value] {
                device->write(value);
                if (++done == len)
                    txLeft = 0;
                nextByte();
            });
        } else {
            if (!rxHave && rx.size() >= 8) {
                stalled = true;
                return;
            }
            later(9, [this] {
                rxWord |= (uint32_t)device->read() << (8 * rxHave);
                ++rxHave;
                ++done;
                if (rxHave == 4 || done == len) {
                    rx.push_back(rxWord);
                    rxWord = 0;
                    rxHave = 0;
                    dmaRxChanged(bus);
                }
                nextByte();
            });
        }
    }

    // A packet that ends with a STOP reports its completion after the STOP's bit time (by then a DMA channel has taken
    // the last word, so its end of count comes first); one that ends with REPEAT_START reports at once. gEarlyDone
    // reports every completion at once (the I2C action then runs before the DMA's).
    void ended()
    {
        ++packets;
        busy = false;
        const bool stop = !(header[2] & HDR_REPEAT_START);
        uint32_t bits = header[2] & HDR_IE ? INT_PACKET_DONE : 0;
        if (tx.empty() && !gNoAllPackets)
            bits |= INT_ALL_PACKETS;
        if (stop) {
            held = false;
            device->stop();
        } else {
            held = true;
        }
        if (stop && !gEarlyDone) {
            later(1, [this, bits] {
                latched |= bits;
            });
        } else {
            latched |= bits;
        }
        kick();
    }

    // A flush empties the FIFOs and ends a packet in progress; a bus held after a REPEAT_START packet stays held (the
    // one-packet path flushes between its write and its read, as it did on hardware before r17-touch).
    void flush()
    {
        ++fifoFlushes;
        tx.clear();
        rx.clear();
        ++generation;
        busy = stalled = failed = false;
    }

    uint32_t readReg(uint32_t o)
    {
        switch (o) {
        case INT_STATUS:
            return status();
        case INT_MASK:
            return intMask;
        case FIFO_CONTROL:
            return fifoControl;
        case FIFO_STATUS:
            return (uint32_t)rx.size() | (uint32_t)(8 - tx.size()) << 4;
        case CONFIG_LOAD:
            return 0;
        case RX_FIFO: {
            if (rx.empty()) {
                check(false, format("I2C%u: RX FIFO read while empty", bus));
                return 0;
            }
            const uint32_t word = rx.front();
            rx.pop_front();
            if (stalled && reading)
                nextByte();
            return word;
        }
        default:
            return regs[o / 4];
        }
    }

    void writeReg(uint32_t o, uint32_t v)
    {
        switch (o) {
        case INT_STATUS:
            latched &= ~v;
            break;
        case INT_MASK:
            intMask = v;
            everUnmasked |= v;
            break;
        case FIFO_CONTROL:
            fifoControl = v & ~3u;
            if (v & 3)
                flush();
            break;
        case TX_FIFO:
            if (tx.size() >= 8) {
                check(false, format("I2C%u: TX FIFO overflow", bus));
                return;
            }
            tx.push_back(v);
            if (stalled && !reading)
                nextByte();
            kick();
            break;
        case BUS_CLEAR_CNFG:
            regs[o / 4] = v & ~1u;
            if (v & 1)
                later(9, [this] {
                    latched |= INT_BUS_CLEAR_DONE;
                    regs[BUS_CLEAR_STATUS / 4] = 1;
                });
            break;
        case CONFIG_LOAD:
            break;
        default:
            regs[o / 4] = v;
        }
    }
};

static Controller gControllers[6];  // index = Tegra bus number
static const uint32_t kControllerBase[6] = {0, 0x0000, 0x0400, 0x0500, 0x0700, 0x1000};
static uint32_t gIntid[6] = {0, 32 + 38, 32 + 84, 32 + 92, 32 + 120, 32 + 53};

// --- Memory map -----------------------------------------------------------------------------------------------------
alignas(64) static uint8_t gI2CSpace[0x2000], gCarSpace[0x1000], gPinmuxSpace[0x1000], gApbMiscSpace[0x1000];
static uint64_t gMmioOps = 0;

extern "C" vm_offset_t ml_io_map(vm_offset_t pa, vm_size_t)
{
    switch (pa) {
    case 0x7000C000:
        return (vm_offset_t)gI2CSpace;
    case 0x60006000:
        return (vm_offset_t)gCarSpace;
    case 0x70003000:
        return (vm_offset_t)gPinmuxSpace;
    case 0x70000000:
        return (vm_offset_t)gApbMiscSpace;
    default:
        check(false, format("ml_io_map of unexpected %#lx", (unsigned long)pa));
        return 0;
    }
}

extern "C" boolean_t preemption_enabled(void)
{
    return true;
}

static Controller *controllerAt(uintptr_t at, uint32_t *offset)
{
    const uintptr_t base = (uintptr_t)gI2CSpace;
    if (at < base || at >= base + sizeof(gI2CSpace))
        return nullptr;
    const uint32_t off = (uint32_t)(at - base);
    for (unsigned bus = 1; bus <= 5; ++bus) {
        if (off >= kControllerBase[bus] && off < kControllerBase[bus] + 0x100) {
            *offset = off - kControllerBase[bus];
            return &gControllers[bus];
        }
    }
    return nullptr;
}

uint32_t r32(volatile uint8_t *base, uint32_t o)
{
    ++gMmioOps;
    uint32_t offset = 0;
    if (Controller *c = controllerAt((uintptr_t)base + o, &offset))
        return c->readReg(offset);
    return *(volatile uint32_t *)(base + o);
}

void w32(volatile uint8_t *base, uint32_t o, uint32_t v)
{
    ++gMmioOps;
    uint32_t offset = 0;
    if (Controller *c = controllerAt((uintptr_t)base + o, &offset))
        return c->writeReg(offset, v);
    *(volatile uint32_t *)(base + o) = v;
}

static unsigned gDmaInvalidates = 0;
static unsigned gDmaInvalidateWhileActive = 0;

// --- APB DMA (TegraAPBDMA.h) ------------------------------------------------------------------------------------------
struct tegra_apbdma_channel {
    unsigned number = 0;
    tegra_apbdma_handler handler = nullptr;
    void *context = nullptr;
    // The channel's run state, as its registers hold it: the driver stops a const channel through its MMIO.
    mutable bool enabled = false, endPending = false;
    uint32_t apb = 0, req = 0, pa = 0, size = 0, moved = 0, interrupts = 0, starts = 0;
};

static tegra_apbdma_channel gDma[TEGRA_APBDMA_CHANNELS];
static std::map<uint32_t, uint8_t *> gDmaMemory;  // pa -> buffer
static bool gDmaBroken = false, gDmaLoseInterrupt = false, gDmaOpenFails = false;

void dmaInvalidate(const uint8_t *, size_t size)
{
    check(size != 0 && (size % 64) == 0, "DMA invalidate rounded to whole cache lines");
    if (gDma[TEGRA_APBDMA_CH_I2C3_RX].enabled)
        ++gDmaInvalidateWhileActive;
    ++gDmaInvalidates;
}

bool tegra_apbdma_alloc(uint32_t size, tegra_apbdma_buffer *out)
{
    const uint32_t pa = 0x90000000u + (uint32_t)gDmaMemory.size() * 0x10000u;
    auto *va = static_cast<uint8_t *>(calloc(1, size));
    gDmaMemory[pa] = va;
    *out = {va, pa, size, nullptr};
    return true;
}

tegra_apbdma_channel *tegra_apbdma_open(unsigned channel, IOWorkLoop *, tegra_apbdma_handler handler, void *context,
                                        char *why, size_t whySize)
{
    if (gDmaOpenFails || channel >= TEGRA_APBDMA_CHANNELS) {
        snprintf(why, whySize, "controller did not take its set-up (model)");
        return nullptr;
    }
    gDma[channel].number = channel;
    gDma[channel].handler = handler;
    gDma[channel].context = context;
    return &gDma[channel];
}

void tegra_apbdma_start_rx_words(tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t words)
{
    const uint32_t size = words * 4;
    check(words && size <= TEGRA_APBDMA_MAX_BYTES, "DMA length in words, within one programming");
    check(!ch->enabled, "DMA started while running");
    check(gDmaMemory.count(pa) == 1, "DMA into its buffer");
    *ch = {ch->number, ch->handler, ch->context, true, false, apb, req, pa, size, 0, ch->interrupts, ch->starts + 1};
    dmaRxChanged(3);
}

uint32_t tegra_apbdma_moved(const tegra_apbdma_channel *ch, bool *endPending)
{
    if (endPending)
        *endPending = ch->endPending;
    return ch->moved;
}

void tegra_apbdma_stop(const tegra_apbdma_channel *ch)
{
    ch->enabled = false;
    ch->endPending = false;
}

uint32_t tegra_apbdma_interrupts(const tegra_apbdma_channel *ch)
{
    return ch->interrupts;
}

void tegra_apbdma_describe(const tegra_apbdma_channel *ch, char *buffer, size_t size)
{
    snprintf(buffer, size, "APB DMA ch%u (model) moved %u of %u", ch->number, ch->moved, ch->size);
}

// The channel takes I2C3's RX words at the 1-word trigger, one request (and 200 ns) per word.
void dmaRxChanged(unsigned bus)
{
    tegra_apbdma_channel &ch = gDma[TEGRA_APBDMA_CH_I2C3_RX];
    Controller &c = gControllers[3];
    if (bus != 3 || !ch.enabled || gDmaBroken)
        return;
    if (ch.apb != 0x7000C554 || ch.req != TEGRA_APBDMA_REQ_I2C3)
        return;  // wrong FIFO or request: nothing moves
    if (c.rx.size() < c.rxTrigger())
        return;
    gSchedule(gNow + 200, [] {
        tegra_apbdma_channel &chan = gDma[TEGRA_APBDMA_CH_I2C3_RX];
        Controller &i2c = gControllers[3];
        if (!chan.enabled || i2c.rx.empty())
            return;
        const uint32_t word = i2c.rx.front();
        i2c.rx.pop_front();
        memcpy(gDmaMemory[chan.pa] + chan.moved, &word, 4);
        chan.moved += 4;
        if (i2c.stalled && i2c.reading)
            i2c.nextByte();
        if (chan.moved == chan.size) {
            chan.enabled = false;
            if (gDmaLoseInterrupt) {
                chan.endPending = true;
            } else {
                ++chan.interrupts;
                chan.handler(chan.context);  // primary interrupt context, end of count acknowledged
            }
        } else {
            dmaRxChanged(3);
        }
    });
}

// --- GIC, GPIO, HID and power stubs -------------------------------------------------------------------------------
static std::map<uint32_t, IOService *> gInterruptNubs;

IOService *switch_interrupt_nub(uint32_t intid, bool)
{
    IOService *&nub = gInterruptNubs[intid];
    if (!nub)
        nub = new IOService;
    return nub;
}

void switch_interrupt_describe(uint32_t intid, char *buffer, size_t size)
{
    snprintf(buffer, size, "GIC model INTID %u", intid);
}

bool switch_interrupts_ready(void)
{
    return true;
}

uint32_t switch_interrupt_count(uint32_t)
{
    return 0;
}

int switch_check_after_ms(IOTimerEventSource *timer, uint32_t ms)
{
    return timer->setTimeoutMS(ms);
}

static IOService gTouchNub;
static int gResetLine = -1;

extern "C" IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned trigger)
{
    check(gpio == TEGRA_GPIO_TOUCH_INT && trigger == TEGRA_GPIO_LEVEL_LOW, "touch INT is PX1, level low");
    return &gTouchNub;
}

extern "C" void tegra_gpio_describe(unsigned, char *buffer, size_t size)
{
    snprintf(buffer, size, "PX1 (model)");
}

extern "C" void tegra_gpio_check_delivery(unsigned, const char *)
{}

extern "C" int tegra_gpio_output(unsigned gpio, int value)
{
    if (gpio == TEGRA_GPIO_TOUCH_RESET)
        gResetLine = value;
    return 0;
}

static unsigned gPerfHints = 0;

extern "C" void switch_perf_hint(unsigned int)
{
    ++gPerfHints;
}

// XNU's frame diagnostics (debug kernels): each report's stamp, whether a contact was down, and whether one went
// down (1) or lifted (2) in it.
struct FramesTouch {
    uint64_t stamp;
    bool down;
    unsigned changes;
};

static std::vector<FramesTouch> gFramesTouches;

extern "C" void switch_frames_touch(uint64_t stamp, bool down, unsigned changes)
{
    gFramesTouches.push_back({stamp, down, changes});
}

static bool gDisplayOn = true;

extern "C" bool switch_display_is_on(void)
{
    return gDisplayOn;
}

static std::vector<uint64_t> gJoyConTouchMs;

extern "C" void switch_joycon_note_touch_ms(uint64_t now_ms)
{
    gJoyConTouchMs.push_back(now_ms);
}

struct Report {
    SwitchTouchReport report;
    uint64_t stamp, postedAt;
    bool timed;
};

static std::vector<Report> gReports;
static bool gTimedPosts = true;
static uint32_t gEdge = 0;
static IOWorkLoop *gHidLoop = nullptr;

IOService *switch_hid_create(SwitchHIDKind kind, IOWorkLoop *sharedLoop)
{
    check(kind == kSwitchHIDTouch, "only the touch device");
    gHidLoop = sharedLoop;
    if (sharedLoop)
        sharedLoop->retain();
    return new IOService;
}

IOService *switch_hid_create(SwitchHIDKind kind)
{
    return switch_hid_create(kind, nullptr);
}

bool switch_hid_supports_timestamps(IOService *)
{
    return gTimedPosts;
}

IOReturn switch_hid_post(IOService *, const void *report, size_t length)
{
    check(length == sizeof(SwitchTouchReport), "report size");
    Report r = {};
    memcpy(&r.report, report, sizeof(r.report));
    r.stamp = r.postedAt = gNow;
    gReports.push_back(r);
    return kIOReturnSuccess;
}

IOReturn switch_hid_post_at(IOService *, const void *report, size_t length, uint64_t timestamp)
{
    check(gTimedPosts, "post_at only where timestamps are supported");
    check(length == sizeof(SwitchTouchReport), "report size");
    Report r = {};
    memcpy(&r.report, report, sizeof(r.report));
    r.stamp = timestamp;
    r.postedAt = gNow;
    r.timed = true;
    gReports.push_back(r);
    return kIOReturnSuccess;
}

void switch_hid_set_edge_contacts(IOService *, uint32_t contacts)
{
    gEdge = contacts;
}

// --- Interrupt delivery ---------------------------------------------------------------------------------------------
static Ftm4 gFtm4;
static Pmic gPmic;
static IOInterruptEventSource *gTouchSource = nullptr;  // a filter source (this driver) or a plain one (before r17)
static IOFilterInterruptEventSource *gTouchFilter = nullptr;
static bool gTouchPending = false, gTouchInAction = false;
static std::vector<uint64_t> gFilterTimes;  // when the touch filter ran (INT reached the "CPU")

struct IrqRecord {
    uint64_t filter, start, end;
    size_t opsBefore, opsAfter, reportsBefore, reportsAfter;
    unsigned i2cActionsBefore, i2cActionsAfter;
};

static std::vector<IrqRecord> gIrqs;

static unsigned i2cActions(unsigned bus)
{
    unsigned n = gControllers[bus].filterCalls;
    if (bus == 3)
        n += gDma[TEGRA_APBDMA_CH_I2C3_RX].interrupts;
    for (const IOInterruptEventSource *s : IOInterruptEventSource::all) {
        if (s->provider == gInterruptNubs[gIntid[bus]] || (bus == 3 && !s->provider))
            n += s->actions;
    }
    return n;
}

static void deliver()
{
    for (unsigned bus = 1; bus <= 5; ++bus) {
        Controller &c = gControllers[bus];
        if (!c.filter && !c.source) {
            for (IOFilterInterruptEventSource *s : IOFilterInterruptEventSource::filters) {
                if (gInterruptNubs.contains(gIntid[bus]) && s->provider == gInterruptNubs[gIntid[bus]])
                    c.filter = s;
            }
            for (IOInterruptEventSource *s : IOInterruptEventSource::all) {
                if (!c.filter && gInterruptNubs.contains(gIntid[bus]) && s->provider == gInterruptNubs[gIntid[bus]])
                    c.source = s;
            }
        }
        if (c.filter) {
            if (!c.filter->enabled || !(c.status() & c.intMask))
                continue;
            ++c.filterCalls;
            if (c.filter->filter(c.filter->owner, c.filter) && !c.deliveryPending) {
                c.deliveryPending = true;
                gSchedule(gNow + gWorkLoopNs, [&c] {
                    c.deliveryPending = false;
                    if (c.filter->enabled)
                        c.filter->run();
                });
            }
            continue;
        }
        if (!c.source || c.deliveryPending || !(c.status() & c.intMask))
            continue;
        c.deliveryPending = true;
        gSchedule(gNow + gWorkLoopNs, [&c] {
            c.deliveryPending = false;
            if (c.source->enabled && (c.status() & c.intMask))
                c.source->run();
        });
    }
    if (!gTouchSource) {
        for (IOFilterInterruptEventSource *s : IOFilterInterruptEventSource::filters)
            if (s->provider == &gTouchNub)
                gTouchSource = gTouchFilter = s;
        for (IOInterruptEventSource *s : IOInterruptEventSource::all)
            if (!gTouchSource && s->provider == &gTouchNub)
                gTouchSource = s;
    }
    if (gTouchSource && gTouchSource->enabled && !gTouchPending && !gTouchInAction && gFtm4.intLow()) {
        gFilterTimes.push_back(gNow);
        if (!gTouchFilter || gTouchFilter->filter(gTouchFilter->owner, gTouchFilter)) {
            gTouchPending = true;
            const uint64_t filterAt = gNow;
            gSchedule(gNow + gWorkLoopNs, [filterAt] {
                gTouchInAction = true;
                IrqRecord r = {filterAt, gNow, 0, gFtm4.ops.size(), 0, gReports.size(), 0, i2cActions(3), 0};
                gTouchSource->run();
                r.end = gNow;
                r.opsAfter = gFtm4.ops.size();
                r.reportsAfter = gReports.size();
                r.i2cActionsAfter = i2cActions(3);
                gIrqs.push_back(r);
                gTouchInAction = false;
                gTouchPending = false;  // the level pin unmasked (TegraGPIO enableVector)
            });
        }
    }
}

static void fireWatchdog()
{
    IOTimerEventSource *timer = IOTimerEventSource::last;
    check(timer && timer->armed, "touch watchdog armed");
    if (!timer)
        return;
    timer->armed = false;
    timer->action(timer->owner, timer);
}

static void bringUp()
{
    gControllers[3].devices[0x49] = &gFtm4;
    gControllers[5].devices[0x3c] = &gPmic;
    for (unsigned bus = 1; bus <= 5; ++bus)
        gControllers[bus].bus = bus;
    gAfterEvent = deliver;
    gFtm4.fifo.push_back({0x10, 0, 0, 0, 0, 0, 0, 0});  // power-on ready
    check(switch_touch_start() == 0, "switch_touch_start");
    fireWatchdog();  // the 1 ms timer: power-up and bring-up on the work loop
    gRunUntil(gNow + 1000000);
    check((gPmic.regs[0x2f] & 0xc0) == 0xc0 && (gPmic.regs[0x2f] & 0x3f) == (2900 - 800) / 50, "LDO6 on at 2.9 V");
    check(gResetLine == 1, "touch reset released");
    check(gFtm4.intEnabled && gFtm4.sensing && gFtm4.mode == 0x01, "FTM4 sensing in finger mode, INT on");
}

static bool logged(const std::string &needle)
{
    return std::any_of(gLog.begin(), gLog.end(), [&](const std::string &l) {
        return l.find(needle) != std::string::npos;
    });
}

static std::string logLine(const std::string &needle)
{
    for (const std::string &l : gLog)
        if (l.find(needle) != std::string::npos)
            return l;
    return "";
}

// A drag: `fingers` contacts enter, move `moves` scans at `periodNs` (by stepX, stepY raw a scan: 24 is ~3 mm, a
// deliberate drag, out of the settle window at its first motion), leave. Returns the first scan's time.
static uint64_t drag(unsigned fingers, unsigned moves, uint64_t periodNs, unsigned stepX = 24, unsigned stepY = 3)
{
    const uint64_t start = gNow + 1000000;
    for (unsigned s = 0; s <= moves + 1; ++s) {
        gSchedule(start + s * periodNs, [=] {
            std::vector<Event> events;
            for (unsigned f = 0; f < fingers; ++f) {
                const unsigned type = s == 0 ? 3 : s == moves + 1 ? 4 : 5;
                events.push_back(contactEvent(type, f, 200 + 100 * f + stepX * s, 300 + stepY * s));
            }
            gFtm4.scan(events);
        });
    }
    gRunUntil(start + (moves + 3) * periodNs);
    return start;
}

// Each touch interrupt: single-transfer burst read (1-2 fingers) or count+events (3+ fingers / fallback), and `actions`
// I2C3 interrupt actions (0: report the most seen instead of checking).
static unsigned checkIrqTraffic(size_t fromIrq, unsigned want, const char *what, bool singleTransfer = false)
{
    unsigned most = 0;
    for (size_t i = fromIrq; i < gIrqs.size(); ++i) {
        const IrqRecord &r = gIrqs[i];
        std::vector<std::string> ops(gFtm4.ops.begin() + (long)r.opsBefore, gFtm4.ops.begin() + (long)r.opsAfter);
        const bool ok = singleTransfer
                            ? (ops.size() == 1 && ops[0].rfind("R 86 x", 0) == 0)
                            : (ops.size() == 2 && ops[0] == "R b6 00 23 x2" && ops[1].rfind("R 86 x", 0) == 0);
        check(ok, format("%s: interrupt %zu: unexpected wire ops (%zu ops: %s ...)", what, i, ops.size(),
                         ops.empty() ? "" : ops[0].c_str()));
        const unsigned actions = r.i2cActionsAfter - r.i2cActionsBefore;
        most = std::max(most, actions);
        if (want)
            check(actions == want,
                  format("%s: interrupt %zu: %u I2C3 interrupt actions (want %u)", what, i, actions, want));
    }
    return most;
}

static void checkStamps(size_t fromReport, const char *what)
{
    uint64_t last = 0;
    for (size_t i = fromReport; i < gReports.size(); ++i) {
        check(gReports[i].stamp > last, format("%s: report %zu stamp strictly increasing", what, i));
        last = gReports[i].stamp;
    }
    // Every report carries its interrupt's filter time (or a tick after the previous report of that interrupt).
    for (const IrqRecord &r : gIrqs) {
        for (size_t i = r.reportsBefore; i < r.reportsAfter; ++i) {
            if (i < fromReport)
                continue;
            const uint64_t want = r.filter + (i - r.reportsBefore);
            check(gReports[i].timed && gReports[i].stamp == want,
                  format("%s: report %zu stamped %" PRIu64 ", its interrupt's filter ran at %" PRIu64, what, i,
                         gReports[i].stamp, r.filter));
        }
    }
}

static unsigned downContacts(const SwitchTouchReport &r)
{
    unsigned n = 0;
    for (const SwitchTouchSlot &s : r.slot)
        n += (s.flags & kSwitchTouchTip) ? 1 : 0;
    return n;
}

// --- Scenarios ------------------------------------------------------------------------------------------------------
static void scenarioDrag()
{
    bringUp();
    check(logged("IOLog: SwitchTouch: started (FTM4 firmware 0100, config 64/23)"), "started line");
    check(logged("os_log: SwitchTouch: I2C3 chained reads and DMA on: the chip ID block reads the same"),
          "chained and DMA on line");
    check(!logged("off for this boot"), "no off line");
    // The bring-up's register reads were chained: one submission each (write with repeated START, then the read).
    const std::vector<std::string> &trace = gControllers[3].trace;
    check(std::find(trace.begin(), trace.end(), "W3+rs") != trace.end(), "chain's write half: 3 bytes, REPEAT_START");
    for (size_t i = 0; i + 1 < trace.size(); ++i) {
        if (trace[i] == "W3+rs")
            check(trace[i + 1].rfind("R", 0) == 0 && trace[i + 1].find("+ie") == std::string::npos,
                  "chained read follows its write, no per-packet interrupt");
    }
    const size_t irqsBefore = gIrqs.size(), reportsBefore = gReports.size();
    const unsigned hintsBefore = gPerfHints;
    const unsigned flushesBefore = gControllers[3].fifoFlushes;
    drag(1, 60, 8333333);  // 120 Hz
    check(gIrqs.size() - irqsBefore == 62, format("62 touch interrupts (got %zu)", gIrqs.size() - irqsBefore));
    check(gReports.size() - reportsBefore == 62, format("62 reports (got %zu)", gReports.size() - reportsBefore));
    check(gPerfHints - hintsBefore == 62, "one perf hint per interrupt, from the filter");
    check(gControllers[3].fifoFlushes == flushesBefore,
          format("clean consecutive I2C3 transfers skip redundant FIFO_FLUSH (got %u)",
                 gControllers[3].fifoFlushes - flushesBefore));
    checkIrqTraffic(irqsBefore, 1, "1-finger drag", true);
    checkStamps(reportsBefore, "1-finger drag");
    check(downContacts(gReports[reportsBefore].report) == 1 && downContacts(gReports.back().report) == 0,
          "first report down, last up");
    // Inactive slots never set kSwitchTouchValid; active slot 0 does on down and on lift.
    for (unsigned s = 1; s < kSwitchTouchSlots; ++s) {
        check(gReports[reportsBefore].report.slot[s].flags == 0, "inactive slot has flags == 0 on down");
        check(gReports.back().report.slot[s].flags == 0, "inactive slot has flags == 0 on lift");
    }
    check(gReports[reportsBefore].report.slot[0].flags == (kSwitchTouchValid | kSwitchTouchTip | kSwitchTouchInRange),
          "active slot has Valid|Tip|InRange on down");
    check(gReports.back().report.slot[0].flags == kSwitchTouchValid, "lifted slot has Valid only on lift");
    check(gReports[reportsBefore].report.contactCount == 1 && gReports.back().report.contactCount == 1,
          "contactCount counts active slots");
    check(gReports.back().report.scanTime != gReports[reportsBefore].report.scanTime,
          "scanTime advances in 100 us units");
    check(gFramesTouches.size() >= 62 && gFramesTouches[gFramesTouches.size() - 62].down &&
              !gFramesTouches.back().down && gFramesTouches.back().stamp == gReports.back().stamp,
          "each report reaches the frame diagnostics with its stamp, down until the lift");
    check(gFramesTouches[gFramesTouches.size() - 62].changes == 1 && gFramesTouches.back().changes == 2 &&
              gFramesTouches[gFramesTouches.size() - 2].changes == 0,
          "the touch-down report says began, the lift ended, the motion between neither");
    uint64_t worst = 0, sum = 0;
    for (size_t i = irqsBefore; i < gIrqs.size(); ++i) {
        worst = std::max(worst, gIrqs[i].end - gIrqs[i].filter);
        sum += gIrqs[i].end - gIrqs[i].filter;
    }
    std::printf("model: 1-finger irq->post mean %" PRIu64 " us, max %" PRIu64 " us\n",
                sum / (gIrqs.size() - irqsBefore) / 1000, worst / 1000);
    check(worst < 600000, "irq->post under 0.6 ms in the model");
    fireWatchdog();
    const std::string line = logLine("SwitchTouch stats:");
    std::printf("%s\n", line.c_str());
    check(!line.empty(), "stats line");
    // Periodic stats go to os_log only, never the console (IOLog).
    check(line.rfind("os_log: SwitchTouch stats: ", 0) == 0, "stats line by os_log");
    check(!logged("IOLog: SwitchTouch stats"), "stats never on the console");
    check(line.find("62 interrupts (0 empty), 62 reports, 62 events (max 1)") != std::string::npos, "stats counts");
    check(line.find("report interval 8333/") != std::string::npos && line.find("120.0 Hz") != std::string::npos,
          "report interval 8333 us, 120 Hz");
    check(line.find("I2C3 62 interrupts (1.0 per interrupt), 0 DMA reads; stamps irq") != std::string::npos,
          "1 I2C interrupt per touch interrupt, no DMA for 16-byte burst reads");
    gLog.clear();
    fireWatchdog();
    check(!logged("SwitchTouch stats:"), "no stats line for a window without interrupts");
}

static void scenarioFiveFingers(bool dmaWorks)
{
    gDmaBroken = !dmaWorks;
    bringUp();
    if (dmaWorks) {
        check(logged("I2C3 chained reads and DMA on"), "DMA on");
    } else {
        const std::string off = logLine("SwitchTouch: I2C3 DMA off for this boot:");
        std::printf("%s\n", off.c_str());
        check(off.find("DMA read failed (-6)") != std::string::npos, "DMA off: the check timed out, one line");
        check(!logged("DMA on"), "no DMA on line");
        check(!logged("chained reads off"), "the chain stays on");
    }
    const size_t irqsBefore = gIrqs.size(), reportsBefore = gReports.size();
    const unsigned startsBefore = gDma[TEGRA_APBDMA_CH_I2C3_RX].starts;
    const unsigned invalidatesBefore = gDmaInvalidates;
    drag(5, 30, 8333333);
    check(gReports.size() - reportsBefore == 32, format("32 reports (got %zu)", gReports.size() - reportsBefore));
    check(downContacts(gReports[reportsBefore + 5].report) == 5, "five contacts down");
    checkStamps(reportsBefore, "5-finger drag");
    // Steady-state 5-finger scans (after scan 0 lands all 5 from 0 down): count read + 40-byte event read by DMA/PIO.
    const unsigned most =
        checkIrqTraffic(irqsBefore + 1, dmaWorks ? 3 : 0, dmaWorks ? "5-finger drag, DMA" : "5-finger drag, PIO");
    std::printf("model: 5-finger %s: up to %u I2C3 interrupt actions per touch interrupt\n", dmaWorks ? "DMA" : "PIO",
                most);
    if (!dmaWorks)
        check(most > 3, "PIO takes RX interrupts for a read beyond the RX FIFO");
    // Every steady-state 40-byte event read went by DMA (or none did).
    const unsigned starts = gDma[TEGRA_APBDMA_CH_I2C3_RX].starts - startsBefore;
    check(starts == (dmaWorks ? 31u : 0u), format("DMA reads %u", starts));
    check(gDmaInvalidates - invalidatesBefore == starts,
          format("exactly one post-DMA invalidate per DMA read (%u for %u reads)", gDmaInvalidates - invalidatesBefore,
                 starts));
    check(gDmaInvalidateWhileActive == 0, "DMA invalidate never runs while channel is active");
    check(gControllers[3].filter && gControllers[3].filter->actions == 0,
          "I2C3 filter-only ISR never bounces to work loop");
    // Positions survived the trip: contact 4 at the last move.
    const SwitchTouchReport &r = gReports[reportsBefore + 30].report;
    check((r.slot[4].flags & kSwitchTouchTip) != 0, "contact 4 down at the last move");
    fireWatchdog();
    const std::string line = logLine("SwitchTouch stats:");
    std::printf("%s\n", line.c_str());
    check(line.find(dmaWorks ? "31 DMA reads" : "0 DMA reads (DMA off)") != std::string::npos, "stats DMA count");
}

static void scenarioSameBatch()
{
    bringUp();
    const size_t reportsBefore = gReports.size(), irqsBefore = gIrqs.size();
    gSchedule(gNow + 1000000, [] {
        gFtm4.scan({contactEvent(3, 0, 400, 400), contactEvent(4, 0, 400, 400)});  // a tap within one scan
    });
    gRunUntil(gNow + 20000000);
    check(gIrqs.size() - irqsBefore == 1, "one interrupt");
    check(gReports.size() - reportsBefore == 2, "down and up each get a report");
    if (gReports.size() - reportsBefore == 2) {
        check(downContacts(gReports[reportsBefore].report) == 1 &&
                  downContacts(gReports[reportsBefore + 1].report) == 0,
              "down, then up");
        check(gReports[reportsBefore + 1].stamp == gReports[reportsBefore].stamp + 1, "the second a tick later");
    }
}

static void scenarioDeepFifo()
{
    bringUp();
    const size_t irqsBefore = gIrqs.size();
    gSchedule(gNow + 1000000, [] {
        std::vector<Event> events;
        for (unsigned i = 0; i < 40; ++i)
            events.push_back(contactEvent(i == 0 ? 3 : i == 39 ? 4 : 5, 0, 100 + i, 100));
        gFtm4.scan(events);
    });
    gRunUntil(gNow + 30000000);
    check(gIrqs.size() - irqsBefore == 2, format("40 events: two interrupts (got %zu)", gIrqs.size() - irqsBefore));
    check(gFtm4.fifo.empty(), "FIFO drained");
    fireWatchdog();
    check(logLine("SwitchTouch stats:").find("2 interrupts (0 empty), 2 reports, 40 events (max 32)") !=
              std::string::npos,
          "stats: 32 then 8");
}

static void scenarioUntimed()
{
    gTimedPosts = false;
    bringUp();
    check(logged("IOLog: SwitchTouch: HID reports cannot carry the interrupt's time here"), "untimed line");
    const size_t before = gReports.size();
    drag(1, 5, 8333333);
    check(gReports.size() - before == 7, "7 reports");
    for (size_t i = before; i < gReports.size(); ++i)
        check(!gReports[i].timed, "posted untimed");
    fireWatchdog();
    check(logLine("SwitchTouch stats:").find("stamps post") != std::string::npos, "stats: stamps post");
}

static void expectPanic(const std::function<void()> &body, const std::string &needle, const char *what)
{
    try {
        body();
        check(false, format("%s: no panic", what));
    } catch (const Panic &p) {
        std::printf("panic: %s\n", p.what());
        check(std::string(p.what()).find(needle) != std::string::npos,
              format("%s: panic names it (%s)", what, p.what()));
    }
}

// The panel stops answering with a contact down: one line, the contact lifted, the panel powered down, no panic; the
// next display power-on brings it up from scratch.
static void scenarioNack()
{
    bringUp();
    gSchedule(gNow + 1000000, [] {
        gFtm4.scan({contactEvent(3, 0, 400, 400)});
    });
    gSchedule(gNow + 9000000, [] {
        gFtm4.scan({contactEvent(5, 0, 420, 400)});
        gControllers[3].devices.erase(0x49);
    });
    gRunUntil(gNow + 50000000);
    check(logged("IOLog: SwitchTouch: off: reading the FTM4 event FIFO failed: I2C error -4"), "NACK: one line");
    check(!gReports.empty() && downContacts(gReports.back().report) == 0, "NACK: the contact lifted");
    check(gResetLine == 0 && (gPmic.regs[0x2f] & 0xc0) == 0, "NACK: panel powered down");
    check(!IOTimerEventSource::last->armed, "NACK: no watchdog while off");
    gControllers[3].devices[0x49] = &gFtm4;
    switch_touch_display_power(true);
    fireWatchdog();
    gRunUntil(gNow + 1000000);
    check(gResetLine == 1 && gFtm4.intEnabled && gFtm4.sensing, "NACK: up again at the next display power-on");
}

// The DFLL's I2C master takes PWR_I2C from a MAX77620 read: CL-DVFS grants the pads to one master at a time (Tegra X1
// TRM 35.4), so the loss is a fault: the read fails with TEGRA_I2C_EARB after its one attempt, one line, and the next
// transfer goes through.
static void scenarioArbitrationLost()
{
    bringUp();
    Controller &pwr = gControllers[5];
    const size_t attempts = pwr.trace.size();
    const uint8_t reg = 0x2f;
    uint8_t value = 0;
    pwr.arbLosses = 1;
    const int lost = tegra_i2c_transfer(5, 0x3c, &reg, 1, &value, 1);
    check(lost == TEGRA_I2C_EARB, format("lost arbitration: TEGRA_I2C_EARB (got %d)", lost));
    check(pwr.trace.size() == attempts + 1,
          format("lost arbitration: one packet on the wire, not repeated (%zu)", pwr.trace.size() - attempts));
    check(logged("IOLog: TegraI2C: I2C5 0x3c lost arbitration; the transfer fails"), "lost arbitration: one line");
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, &value, 1) == TEGRA_I2C_OK && value == gPmic.regs[0x2f],
          "lost arbitration: the next transfer goes through");
}

// No FTM4 answers at all: one line, the panel's interrupt never enabled, no watchdog, no panic. LDO6 stays on for the
// ALS that shares it.
static void scenarioAbsent()
{
    gControllers[5].devices[0x3c] = &gPmic;
    for (unsigned bus = 1; bus <= 5; ++bus)
        gControllers[bus].bus = bus;
    gAfterEvent = deliver;
    check(switch_touch_start() == 0, "absent: switch_touch_start");
    fireWatchdog();
    gRunUntil(gNow + 1000000);
    check(logged("IOLog: SwitchTouch: off: FTM4 on I2C3 0x49 did not come up: I2C error -4, chip id 0000"),
          "absent: one line");
    check(!logged("SwitchTouch: started"), "absent: not started");
    check(!IOTimerEventSource::last->armed, "absent: no watchdog");
    check((gPmic.regs[0x2f] & 0xc0) == 0xc0, "absent: LDO6 left on");
}

// The channel's first end of count never reaches the CPU (never booted): one line, DMA off, touch on PIO. No panic.
static void scenarioDmaFirstLost()
{
    gDmaLoseInterrupt = true;
    bringUp();
    const std::string off = logLine("SwitchTouch: I2C3 DMA off for this boot:");
    std::printf("%s\n", off.c_str());
    check(off.find("its first end of count never reached the CPU (-6)") != std::string::npos,
          "first EOC lost: one line");
    const size_t before = gReports.size();
    drag(5, 3, 8333333);
    check(gReports.size() - before == 5, "five-finger reads by PIO");
}

// After the channel's end of count has reached the CPU once, a lost one panics.
static void scenarioDmaLostLater()
{
    bringUp();
    check(logged("chained reads and DMA on"), "DMA on");
    gDmaLoseInterrupt = true;
    expectPanic(
        [] {
            drag(5, 3, 8333333);
        },
        "TegraI2C: I2C3 DMA end of count lost: APB DMA ch4", "lost DMA interrupt");
}

// A controller whose ALL_PACKETS_DONE never comes: the chain check times out (nothing left asserted), one line, and the
// bus keeps one packet per message (and no DMA); touch works as before r17-touch, with two transactions per interrupt.
static void scenarioChainBroken()
{
    gNoAllPackets = true;
    bringUp();
    const std::string off = logLine("SwitchTouch: I2C3 chained reads off for this boot:");
    std::printf("%s\n", off.c_str());
    check(off.find("chained read failed (-6)") != std::string::npos, "chain check failed: one line");
    check(!logged("DMA on") && !logged("DMA off"), "no DMA check without the chain");
    const size_t before = gReports.size(), irqsBefore = gIrqs.size();
    drag(2, 10, 8333333);
    check(gReports.size() - before == 12, "reports by the one-packet path");
    const unsigned most = checkIrqTraffic(irqsBefore + 1, 0, "unchained", true);
    std::printf("model: unchained 2-finger: up to %u I2C3 interrupt actions per touch interrupt\n", most);
    fireWatchdog();
    check(logLine("SwitchTouch stats:").find("unchained, 0 DMA reads (DMA off)") != std::string::npos,
          "stats: unchained");
}

static void scenarioDmaOpenFails()
{
    gDmaOpenFails = true;
    bringUp();
    const std::string off = logLine("SwitchTouch: I2C3 DMA off for this boot:");
    check(off.find("APB DMA channel 4: controller did not take its set-up") != std::string::npos,
          "DMA off: the channel could not be had, one line");
    const size_t before = gReports.size();
    drag(5, 3, 8333333);
    check(gReports.size() - before == 5, "five-finger reads by PIO");
}

// --- Contact filter (SwitchTouchFilter.h) ----------------------------------------------------------------------------
// Scans of one contact at 120 Hz: raw points, then the leave. Returns the reports posted meanwhile. `strengths`, when
// given, is each scan's strength (else z for all).
static std::vector<SwitchTouchReport> scans(unsigned id, const std::vector<std::pair<unsigned, unsigned>> &points,
                                            unsigned z = 16, const std::vector<unsigned> &strengths = {})
{
    const size_t before = gReports.size();
    const uint64_t start = gNow + 1000000;
    for (size_t s = 0; s <= points.size(); ++s) {
        gSchedule(start + s * 8333333, [=] {
            const bool leave = s == points.size();
            const auto p = points[leave ? s - 1 : s];
            const unsigned strength = s < strengths.size() ? strengths[s] : z;
            gFtm4.scan({contactEvent(leave ? 4 : s == 0 ? 3 : 5, id, p.first, p.second, strength)});
        });
    }
    gRunUntil(start + (points.size() + 2) * 8333333);
    std::vector<SwitchTouchReport> out;
    for (size_t i = before; i < gReports.size(); ++i)
        out.push_back(gReports[i].report);
    return out;
}

// Report-frame position of FTM4 raw (x, y) (SwitchTouch.cpp scale() and the portrait-native mapping).
static std::pair<int, int> reported(unsigned rx, unsigned ry)
{
    const int lx = (int)((rx - 15) * 1279 / (1264 - 15)), ly = (int)((ry - 15) * 719 / (704 - 15));
    return {ly, 1279 - lx};
}

// A stationary contact emits periodic heartbeats (every ~16.7 ms at 120 Hz, well inside SpringBoard's 42 ms window),
// never moves from where it landed, and lifts cleanly.
static bool stillAndHeartbeated(const std::vector<SwitchTouchReport> &r, unsigned slot, size_t scanCount)
{
    if (r.size() < 3 || r.size() >= scanCount + 1)
        return false;
    for (const SwitchTouchReport &rep : r) {
        if (rep.slot[slot].x != r[0].slot[slot].x || rep.slot[slot].y != r[0].slot[slot].y)
            return false;
    }
    return (r.back().slot[slot].flags & kSwitchTouchTip) == 0;
}

static void scenarioFilter()
{
    bringUp();
    // A tap whose pad creeps 2 raw px (inside the hysteresis): reported at its first point from touch-down to lift.
    {
        std::vector<std::pair<unsigned, unsigned>> p;
        for (unsigned i = 0; i < 10; ++i)
            p.push_back({600, 400 + std::min(i, 2u)});
        const auto r = scans(0, p);
        check(stillAndHeartbeated(r, 0, p.size()),
              format("tap inside the hysteresis: paced stationary heartbeats, none moved (%zu reports)", r.size()));
    }
    // One whose centroid creeps 8 raw px at a steady strength once settled, as the FTM4's do for the first ~50 ms: it
    // stays inside the still radius (0.8 mm), so the tap lifts within a pixel of where it landed.
    {
        std::vector<std::pair<unsigned, unsigned>> p;
        for (unsigned i = 0; i < 10; ++i)
            p.push_back({600, 400 + 2 * (i < 3 ? 0 : std::min(i - 2, 4u))});
        const auto r = scans(1, p);
        const auto landed = reported(600, 400);
        check(std::abs(r.back().slot[1].x - landed.first) <= 1 && r.back().slot[1].y == landed.second,
              format("creeping tap: lifted at (%d,%d), within a pixel of where it landed (%d,%d)", r.back().slot[1].x,
                     r.back().slot[1].y, landed.first, landed.second));
    }
    // A still tap as the panel reports it: the centroid slides 12 raw px while the pad presses down (strength rising)
    // and 18 more while it lifts (strength falling). Neither is motion.
    {
        const std::vector<std::pair<unsigned, unsigned>> p = {{700, 500}, {700, 506}, {700, 512}, {700, 512},
                                                              {700, 512}, {700, 521}, {700, 530}};
        const auto r = scans(5, p, 16, {12, 35, 70, 72, 71, 40, 15});
        check(stillAndHeartbeated(r, 5, p.size()),
              format("pressed and lifted tap: paced stationary heartbeats, none moved (%zu reports)", r.size()));
    }
    // A finger held still with 1-2 px of jitter: stationary heartbeats stay inside SpringBoard's 42 ms window while
    // cutting duplicate HID traffic.
    {
        std::vector<std::pair<unsigned, unsigned>> p;
        for (unsigned i = 0; i < 20; ++i)
            p.push_back({700 + (i % 3), 300 + ((i * 7) % 2)});
        const size_t before = gReports.size();
        const auto r = scans(2, p);
        check(stillAndHeartbeated(r, 2, p.size()),
              format("jittering hold: paced stationary heartbeats, none moved (%zu reports)", r.size()));
        uint64_t worstGapNs = 0;
        for (size_t i = before + 1; i < gReports.size(); ++i)
            worstGapNs = std::max(worstGapNs, gReports[i].stamp - gReports[i - 1].stamp);
        check(worstGapNs <= 20000000ULL,
              format("jittering hold: max gap %" PRIu64 " us stays well under SpringBoard's 42 ms window",
                     worstGapNs / 1000));
    }
    // A slow drag (3 raw px a scan) from touch-down: initial still-radius scans pace, then moving scans report in
    // steps of at most 5 px and end within 3 px of the finger.
    {
        std::vector<std::pair<unsigned, unsigned>> p;
        for (unsigned i = 0; i < 30; ++i)
            p.push_back({800, 300 + 3 * i});
        const auto r = scans(3, p);
        check(r.size() >= 25 && r.size() <= p.size() + 1,
              format("slow drag: paced while inside still radius, then 1:1 (%zu reports)", r.size()));
        int last = r.empty() ? 0 : r[0].slot[3].x, worst = 0;
        for (const auto &rep : r) {
            worst = std::max(worst, std::abs((int)rep.slot[3].x - last));
            last = rep.slot[3].x;
        }
        check(worst <= 5, format("slow drag: largest step %d px", worst));
        const auto want = reported(800, 300 + 3 * 29);
        check(std::abs(r.back().slot[3].x - want.first) <= 3,
              format("slow drag: ends within 3 px of the finger (%d, want %d)", r.back().slot[3].x, want.first));
    }
    // A fast start (24 raw px a scan): every 120 Hz moving scan reported.
    {
        const auto r = scans(4, {{900, 200}, {900, 224}, {900, 248}, {900, 272}});
        check(r.size() == 5, format("fast drag: every scan reported (%zu)", r.size()));
    }
    fireWatchdog();
    const std::string geometry = logLine("SwitchTouch geometry:");
    std::printf("%s\n", geometry.c_str());
    check(geometry.find("taps raw travel 11/31/31 px (4, reported moving 1, reported travel 0/1/1 px; hysteresis "
                        "2.4 px)") != std::string::npos,
          "geometry: of the four taps only the creeping one moved, by a pixel");
}

static void scenarioPalm()
{
    bringUp();
    const size_t before = gReports.size();
    // A finger down, then its strength rises to a palm's: lifted once; its later events ignored until it leaves.
    gSchedule(gNow + 1000000, [] {
        gFtm4.scan({contactEvent(3, 0, 500, 300), contactEvent(3, 1, 900, 300)});
    });
    gSchedule(gNow + 9000000, [] {
        gFtm4.scan({contactEvent(5, 0, 520, 300, 600), contactEvent(5, 1, 930, 300)});
    });
    gSchedule(gNow + 17000000, [] {
        gFtm4.scan({contactEvent(5, 0, 560, 300, 700), contactEvent(5, 1, 960, 300)});
    });
    gSchedule(gNow + 25000000, [] {
        gFtm4.scan({contactEvent(4, 0, 560, 300), contactEvent(4, 1, 960, 300)});
    });
    // A palm from its first event: never reported.
    gSchedule(gNow + 40000000, [] {
        gFtm4.scan({contactEvent(3, 2, 300, 300, 800)});
    });
    gSchedule(gNow + 48000000, [] {
        gFtm4.scan({contactEvent(4, 2, 300, 300)});
    });
    gRunUntil(gNow + 70000000);
    bool zeroLifted = false, oneStayed = true, twoSeen = false;
    for (size_t i = before; i < gReports.size(); ++i) {
        const SwitchTouchReport &r = gReports[i].report;
        if (!(r.slot[0].flags & kSwitchTouchTip) && (r.slot[1].flags & kSwitchTouchTip))
            zeroLifted = true;
        if (i + 1 < gReports.size() && i > before && !(r.slot[1].flags & kSwitchTouchTip) &&
            (gReports[i + 1].report.slot[1].flags & kSwitchTouchTip))
            oneStayed = false;
        if (r.slot[2].flags & kSwitchTouchTip)
            twoSeen = true;
    }
    check(zeroLifted, "palm: contact 0 lifted while contact 1 stays down");
    check(oneStayed, "palm: contact 1 never lifted and put back");
    check(!twoSeen, "palm: a palm from its first event is never a contact");
    fireWatchdog();
    check(logLine("SwitchTouch geometry:").find("(palms 2)") != std::string::npos, "geometry: two palms");
}

static void scenarioEdge()
{
    bringUp();
    // Raw y (landscape) maps to report x: raw 15 + 34 ~ report x 35 (inside the 40 px band), raw 15 + 58 ~ 60 (outside).
    gEdge = 0;
    scans(0, {{600, 15 + 34}, {600, 15 + 34}});
    const uint32_t inside = gEdge;
    gEdge = 0;
    scans(1, {{600, 15 + 58}, {600, 15 + 58}});
    check((inside & 1u) != 0, "a touch starting 35 px from the edge is from the edge");
    check((gEdge & 2u) == 0, "one starting 60 px in is not");
    // The portrait right edge, landscape bottom (the home and switcher gestures'): raw y 670 ~ 36 px from it, 644 ~ 63.
    gEdge = 0;
    scans(3, {{600, 670}, {600, 670}});
    const uint32_t bottom = gEdge;
    gEdge = 0;
    scans(4, {{600, 644}, {600, 644}});
    check((bottom & (1u << 3)) != 0, "36 px from the landscape bottom edge is from the edge");
    check((gEdge & (1u << 4)) == 0, "63 px from it is not");
    // An edge swipe: first seen 30 px from the edge, then 150 px inward within 50 ms.
    scans(5, {{600, 15 + 29}, {600, 15 + 79}, {600, 15 + 129}, {600, 15 + 179}});
    fireWatchdog();
    const std::string geometry = logLine("SwitchTouch geometry:");
    check(geometry.find("edge swipes start 30/") != std::string::npos, "geometry: the edge swipe's first point, 30 px");
}

static void scenarioFourFingers()
{
    bringUp();
    // Four fingers over three scans (16.7 ms from first to fourth), then a swipe.
    gSchedule(gNow + 1000000, [] {
        gFtm4.scan({contactEvent(3, 0, 300, 200), contactEvent(3, 1, 400, 200)});
    });
    gSchedule(gNow + 9333333, [] {
        gFtm4.scan({contactEvent(3, 2, 500, 200)});
    });
    gSchedule(gNow + 17666666, [] {
        gFtm4.scan({contactEvent(3, 3, 600, 200)});
    });
    for (unsigned s = 1; s <= 10; ++s) {
        gSchedule(gNow + 17666666 + s * 8333333, [s] {
            std::vector<Event> e;
            for (unsigned f = 0; f < 4; ++f)
                e.push_back(contactEvent(s == 10 ? 4 : 5, f, 300 + 100 * f + 30 * s, 200));
            gFtm4.scan(e);
        });
    }
    gRunUntil(gNow + 140000000);
    unsigned most = 0;
    for (const Report &r : gReports)
        most = std::max(most, downContacts(r.report));
    check(most == 4, "four contacts down together");
    fireWatchdog();
    const std::string geometry = logLine("SwitchTouch geometry:");
    std::printf("%s\n", geometry.c_str());
    check(geometry.rfind("os_log: SwitchTouch geometry: ", 0) == 0, "geometry line by os_log");
    check(geometry.find("4-finger arrival 16/16/16 ms (1)") != std::string::npos, "geometry: arrival spread 16 ms");
}

// The same drags over this tree's drivers or main's (the before/after of r17-touch): per touch interrupt, the
// transactions on the wire, I2C3 interrupt actions, and INT-to-last-report time. `actions` is the I2C3 interrupt
// actions per touch interrupt expected (as printed, two decimals); returns the mean INT-to-last-report time in us.
static uint64_t measureDrag(unsigned fingers, const char *name, const char *opsPer, const char *actionsPer)
{
    const size_t irqsBefore = gIrqs.size();
    drag(fingers, 60, 8333333);
    uint64_t ops = 0, actions = 0, sum = 0, worst = 0;
    const size_t n = gIrqs.size() - irqsBefore;
    for (size_t i = irqsBefore; i < gIrqs.size(); ++i) {
        const IrqRecord &r = gIrqs[i];
        ops += r.opsAfter - r.opsBefore;
        actions += r.i2cActionsAfter - r.i2cActionsBefore;
        const uint64_t posted = r.reportsAfter > r.reportsBefore ? gReports[r.reportsAfter - 1].postedAt : r.end;
        sum += posted - r.filter;
        worst = std::max(worst, posted - r.filter);
    }
    check(n == 62, format("%s: 62 interrupts (got %zu)", name, n));
    if (!n)
        return 0;
    std::printf("measure %s: %zu interrupts, %.2f transactions and %.2f I2C3 interrupt actions per interrupt, "
                "INT->last report mean %" PRIu64 " us max %" PRIu64 " us\n",
                name, n, (double)ops / (double)n, (double)actions / (double)n, sum / n / 1000, worst / 1000);
    EXPECT_EQ(format("%.2f", (double)ops / (double)n), opsPer) << name << ": transactions per interrupt";
    EXPECT_EQ(format("%.2f", (double)actions / (double)n), actionsPer)
        << name << ": I2C3 interrupt actions per interrupt";
    return sum / n / 1000;
}

static void scenarioMeasure()
{
    bringUp();
    const uint64_t oneFinger = measureDrag(1, "1-finger", "1.00", "1.00");
    measureDrag(2, "2-finger", "1.02", "1.02");
    measureDrag(5, "5-finger", "2.02", "3.00");
    EXPECT_LT(oneFinger, 550u) << "1-finger INT->last report mean, us";
}

// Real hardware FTM4 scans at 200 Hz (5 ms): moving drags coalesce to 100 Hz (10 ms), pausing emits a stationary sample
// within 10 ms and then paces to 50 Hz (20 ms, 4x reduction while keeping 2-3 samples inside SpringBoard's 42 ms
// window), a pre-lift move on a skipped scan is flushed at its own timestamp before the lift, and inactive slots stay
// flags == 0.
static void scenarioCadence200Hz()
{
    bringUp();
    constexpr uint64_t k5ms = 5000000ULL;
    const size_t before = gReports.size();
    const uint64_t start = gNow + 1000000;
    // 20 moving scans (100 ms) at 200 Hz, then 16 stationary scans (80 ms), then 1 final move + immediate lift.
    unsigned s = 0;
    unsigned x = 200;
    for (unsigned i = 0; i < 20; ++i, ++s) {
        const unsigned type = i == 0 ? 3 : 5;
        const unsigned px = x;
        gSchedule(start + s * k5ms, [type, px] {
            gFtm4.scan({contactEvent(type, 0, px, 300)});
        });
        x += 20;
    }
    const unsigned heldX = x - 20;
    for (unsigned i = 0; i < 16; ++i, ++s) {
        gSchedule(start + s * k5ms, [heldX] {
            gFtm4.scan({contactEvent(5, 0, heldX, 300)});
        });
    }
    const unsigned flickX = heldX + 40;
    gSchedule(start + s * k5ms, [flickX] {
        gFtm4.scan({contactEvent(5, 0, flickX, 300)});
    });
    ++s;
    gSchedule(start + s * k5ms, [flickX] {
        gFtm4.scan({contactEvent(4, 0, flickX, 300)});
    });
    gRunUntil(start + (s + 4) * k5ms);

    const size_t totalReports = gReports.size() - before;
    // 38 scans at 200 Hz should coalesce to ~18-20 reports (down + ~10 moving + ~5-6 stationary + pre-lift + lift).
    check(totalReports >= 15 && totalReports <= 22,
          format("200 Hz drag+hold+flick coalesced to %zu reports (expected 15..22 from 38 scans)", totalReports));
    // Verify timestamps strictly increase and max gap during hold stays <= 20 ms (well inside 42 ms window).
    uint64_t maxGapNs = 0;
    for (size_t i = before + 1; i < gReports.size(); ++i) {
        check(gReports[i].stamp > gReports[i - 1].stamp, "200 Hz stamps strictly increasing");
        maxGapNs = std::max(maxGapNs, gReports[i].stamp - gReports[i - 1].stamp);
    }
    check(maxGapNs >= 16000000ULL && maxGapNs <= 20000000ULL,
          format("200 Hz stationary hold gap %" PRIu64 " us paces to 16..20 ms (50-60 Hz inside 42 ms window)",
                 maxGapNs / 1000));
    // Final lift report must reflect the flicked position, preceded by a down report at that flicked position.
    ASSERT_GE(totalReports, 2u);
    const Report &preLift = gReports[gReports.size() - 2];
    const Report &lift = gReports.back();
    check((preLift.report.slot[0].flags & kSwitchTouchTip) != 0, "pre-lift move stays down");
    check((lift.report.slot[0].flags & kSwitchTouchTip) == 0, "lift clears Tip");
    check(preLift.report.slot[0].y == lift.report.slot[0].y, "pre-lift move carries the final flick position");
    const auto wantHeld = reported(heldX, 300);
    check(preLift.report.slot[0].y != wantHeld.second, "pre-lift move advanced past the held position");
    for (size_t i = before; i < gReports.size(); ++i) {
        for (unsigned slot = 1; slot < kSwitchTouchSlots; ++slot)
            check(gReports[i].report.slot[slot].flags == 0, "inactive slots never set kSwitchTouchValid");
    }
}

// A controller whose 0x86 read does not zero-pad past FIFO end: bringUp detects it once, logs one line, and falls back
// cleanly to the 2-transfer RegEventCount + CmdReadAllEvents path.
static void scenarioSingleReadFallback()
{
    gFtm4.burstZeroPad = false;
    bringUp();
    check(logged("SwitchTouch: single-transfer event reads off for this boot"), "single-read fallback logged once");
    const size_t irqsBefore = gIrqs.size(), reportsBefore = gReports.size();
    drag(1, 10, 8333333);
    check(gReports.size() - reportsBefore == 12, "fallback still reports every 120 Hz scan");
    checkIrqTraffic(irqsBefore, 2, "fallback 2-transfer drag", false);
}

// Clean consecutive I2C transfers skip FIFO_FLUSH, while an error (NACK / ARB_LOST) invalidates fifoClean so the next
// transfer flushes the FIFOs and succeeds.
static void scenarioFifoFlushBehavior()
{
    bringUp();
    Controller &i2c5 = gControllers[5];
    const uint8_t reg = 0x2f;
    uint8_t val = 0;
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, &val, 1) == TEGRA_I2C_OK, "priming I2C5 read");
    const unsigned afterPrime = i2c5.fifoFlushes;
    for (int i = 0; i < 5; ++i)
        check(tegra_i2c_transfer(5, 0x3c, &reg, 1, &val, 1) == TEGRA_I2C_OK, "clean I2C5 read");
    check(i2c5.fifoFlushes == afterPrime,
          format("5 clean I2C5 transfers skipped FIFO_FLUSH (%u extra)", i2c5.fifoFlushes - afterPrime));
    i2c5.arbLosses = 1;
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, &val, 1) == TEGRA_I2C_EARB, "ARB_LOST error");
    const unsigned afterErr = i2c5.fifoFlushes;
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, &val, 1) == TEGRA_I2C_OK, "transfer after error succeeds");
    check(i2c5.fifoFlushes > afterErr, "transfer after error flushed FIFOs before queueing");
}

static void scenarioFilterOnlyNoWorkLoop()
{
    bringUp();
    check(gControllers[3].filter != nullptr && gControllers[5].filter != nullptr,
          "I2C buses attach IOFilterInterruptEventSource");
    check(gControllers[3].source == nullptr && gControllers[5].source == nullptr,
          "no plain IOInterruptEventSource attached for I2C");
    drag(1, 5, 8333333);
    drag(5, 5, 8333333);
    check(gControllers[3].filter->actions == 0, "I2C3 runs zero work-loop actions");
    check(gControllers[5].filter->actions == 0, "I2C5 runs zero work-loop actions");
    for (const IOInterruptEventSource *s : IOInterruptEventSource::all)
        check(s->provider != nullptr, "no provider-less dmaDoneSource created");
    check(gDmaInvalidates == gDma[TEGRA_APBDMA_CH_I2C3_RX].starts,
          "exactly one invalidate per DMA read across bring-up and drags");
    check(gDmaInvalidateWhileActive == 0, "DMA invalidate only runs after completion");
}

static void scenarioDrainAtEndNonChained()
{
    bringUp();
    Controller &i2c5 = gControllers[5];
    for (unsigned i = 0; i < 64; ++i)
        gPmic.regs[0x40 + i] = static_cast<uint8_t>(0xa0 ^ i);
    const uint8_t reg = 0x40;
    uint8_t small[32] = {};
    i2c5.everUnmasked = 0;
    const unsigned irqsBefore = i2c5.filterCalls;
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, small, sizeof(small)) == TEGRA_I2C_OK,
          "32-byte read on non-chained I2C5 succeeds");
    check((i2c5.everUnmasked & INT_RX_REQ) == 0, "32-byte read on non-chained bus never unmasks INT_RX_REQ");
    check(i2c5.filterCalls - irqsBefore == 2,
          format("32-byte write+read takes 2 filter IRQs (got %u)", i2c5.filterCalls - irqsBefore));
    for (unsigned i = 0; i < sizeof(small); ++i)
        check(small[i] == static_cast<uint8_t>(0xa0 ^ i), "32-byte drainAtEnd data matches");

    uint8_t large[40] = {};
    i2c5.everUnmasked = 0;
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, large, sizeof(large)) == TEGRA_I2C_OK,
          "40-byte PIO read on non-chained I2C5 succeeds");
    check((i2c5.everUnmasked & INT_RX_REQ) != 0, ">32-byte PIO read unmasks INT_RX_REQ");
    for (unsigned i = 0; i < sizeof(large); ++i)
        check(large[i] == static_cast<uint8_t>(0xa0 ^ i), "40-byte PIO data matches");
}

static void scenarioTimeoutQuiesceAndRecover()
{
    bringUp();
    Controller &i2c5 = gControllers[5];
    const uint8_t reg = 0x2f;
    uint8_t val = 0;
    i2c5.stall = true;
    const int timedOut = tegra_i2c_transfer(5, 0x3c, &reg, 1, &val, 1);
    check(timedOut == TEGRA_I2C_ETIMEDOUT, format("stalled bus returns ETIMEDOUT (got %d)", timedOut));
    check(i2c5.filter && i2c5.filter->enabled, "interrupt re-enabled after timeout quiesce and recovery");
    i2c5.stall = false;
    // A spurious late interrupt while idle must be masked/cleared cleanly without affecting subsequent transfers.
    i2c5.latched |= INT_PACKET_DONE;
    i2c5.intMask = INT_PACKET_DONE;
    deliver();
    check(i2c5.intMask == 0, "stale filter masks INT_MASK");
    check((i2c5.latched & INT_PACKET_DONE) == 0, "stale filter clears INT_STATUS");
    check(tegra_i2c_transfer(5, 0x3c, &reg, 1, &val, 1) == TEGRA_I2C_OK && val == gPmic.regs[0x2f],
          "transfer after timeout and stale IRQ succeeds");
}

static void scenarioJoyConTouchPreemptHook()
{
    bringUp();
    check(gJoyConTouchMs.empty(), "bringUp never reports touch activity to SwitchJoyCon");

    const size_t reportsBefore = gReports.size();
    drag(1, 5, 10000000);  // enter, 5 moves, leave (10 ms apart -> every report dispatched)
    const size_t dispatched = gReports.size() - reportsBefore;
    check(dispatched == 7, format("7 live reports dispatched (got %zu)", dispatched));
    check(gJoyConTouchMs.size() == dispatched,
          format("switch_joycon_note_touch_ms called once per live dispatch (%zu vs %zu)", gJoyConTouchMs.size(),
                 dispatched));
    for (size_t i = 0; i < std::min(dispatched, gJoyConTouchMs.size()); ++i) {
        check(gJoyConTouchMs[i] == gReports[reportsBefore + i].stamp / 1000000ULL,
              format("touch ms %zu matches report stamp ms (got %" PRIu64 ", want %" PRIu64 ")", i, gJoyConTouchMs[i],
                     gReports[reportsBefore + i].stamp / 1000000ULL));
    }

    // Contact down, then screen turns off (powerDown -> releaseAll): synthetic lift must NOT notify SwitchJoyCon.
    gSchedule(gNow + 2000000, [] {
        gFtm4.scan({contactEvent(3, 0, 500, 350)});
    });
    gRunUntil(gNow + 10000000);
    const size_t reportsWhileDown = gReports.size();
    const size_t hooksWhileDown = gJoyConTouchMs.size();
    check(hooksWhileDown == dispatched + 1, "contact-down notified SwitchJoyCon");
    switch_touch_display_power(false);
    check(gReports.size() == reportsWhileDown + 1, "display off released held contact");
    check(gJoyConTouchMs.size() == hooksWhileDown, "releaseAll on display off does not notify SwitchJoyCon");

    // Display back on, panel brings up, contact down, then panel goes dark before IRQ (shown == false -> releaseAll).
    switch_touch_display_power(true);
    if (IOTimerEventSource::last && IOTimerEventSource::last->armed) {
        IOTimerEventSource::last->armed = false;
        IOTimerEventSource::last->action(IOTimerEventSource::last->owner, IOTimerEventSource::last);
    }
    gSchedule(gNow + 2000000, [] {
        gFtm4.scan({contactEvent(3, 0, 520, 360)});
    });
    gRunUntil(gNow + 10000000);
    const size_t hooksAfterReenter = gJoyConTouchMs.size();
    check(hooksAfterReenter == hooksWhileDown + 1, "contact-down after wake notified SwitchJoyCon");
    gDisplayOn = false;
    gSchedule(gNow + 2000000, [] {
        gFtm4.scan({contactEvent(5, 0, 540, 360)});
    });
    gRunUntil(gNow + 10000000);
    check(gJoyConTouchMs.size() == hooksAfterReenter, "releaseAll on dark-screen IRQ does not notify SwitchJoyCon");

    // ESD controller reset (EvReady -> releaseAll) while screen is lit must NOT notify SwitchJoyCon.
    gDisplayOn = true;
    gSchedule(gNow + 2000000, [] {
        gFtm4.scan({contactEvent(3, 0, 560, 360)});
    });
    gRunUntil(gNow + 10000000);
    const size_t hooksBeforeEsd = gJoyConTouchMs.size();
    check(hooksBeforeEsd == hooksAfterReenter + 1, "contact-down before ESD notified SwitchJoyCon");
    gSchedule(gNow + 2000000, [] {
        gFtm4.scan({{0x10, 0, 0, 0, 0, 0, 0, 0}});
    });
    gRunUntil(gNow + 20000000);
    check(gJoyConTouchMs.size() == hooksBeforeEsd, "releaseAll on EvReady reset does not notify SwitchJoyCon");
}

static void scenarioSharedWorkLoop()
{
    gControllers[3].devices[0x49] = &gFtm4;
    gControllers[5].devices[0x3c] = &gPmic;
    for (unsigned bus = 1; bus <= 5; ++bus)
        gControllers[bus].bus = bus;
    gAfterEvent = deliver;
    gFtm4.fifo.push_back({0x10, 0, 0, 0, 0, 0, 0, 0});
    check(switch_touch_start() == 0, "shared workloop: switch_touch_start");
    check(IOWorkLoop::created == 1 && IOWorkLoop::last != nullptr,
          format("shared workloop: SwitchTouch creates one IOWorkLoop (got %u)", IOWorkLoop::created));
    check(gHidLoop == IOWorkLoop::last, "shared workloop: touch HID device shares SwitchTouch's IOWorkLoop");
    fireWatchdog();
    gRunUntil(gNow + 1000000);
    const size_t before = gReports.size();
    drag(1, 3, 10000000);
    check(gReports.size() > before, "shared workloop: touch reports dispatch after bring-up");
    switch_touch_display_power(false);
    switch_touch_display_power(true);
    fireWatchdog();
    gRunUntil(gNow + 1000000);
}

// --- One process per scenario (the drivers keep boot-wide state) ------------------------------------------------------
static void run(void (*scenario)())
{
    switch_ios::test::ExpectPassesInFreshProcess([scenario] {
        try {
            scenario();
        } catch (const Panic &p) {
            check(false, format("unexpected panic: %s", p.what()));
        } catch (const std::exception &e) {
            check(false, format("exception: %s", e.what()));
        }
        for (const std::string &e : gFtm4.errors)
            check(false, "FTM4 model: " + e);
    });
}

TEST(SwitchTouch, Drag)
{
    run(scenarioDrag);
}

TEST(SwitchTouch, FiveFingersDma)
{
    run([] {
        scenarioFiveFingers(true);
    });
}

TEST(SwitchTouch, FiveFingersDmaEarlyCompletion)
{
    run([] {
        gEarlyDone = true;
        scenarioFiveFingers(true);
    });
}

TEST(SwitchTouch, FiveFingersPio)
{
    run([] {
        scenarioFiveFingers(false);
    });
}

TEST(SwitchTouch, SameBatch)
{
    run(scenarioSameBatch);
}

TEST(SwitchTouch, DeepFifo)
{
    run(scenarioDeepFifo);
}

TEST(SwitchTouch, Untimed)
{
    run(scenarioUntimed);
}

TEST(SwitchTouch, Nack)
{
    run(scenarioNack);
}

TEST(SwitchTouch, Absent)
{
    run(scenarioAbsent);
}

TEST(SwitchTouch, ArbitrationLost)
{
    run(scenarioArbitrationLost);
}

TEST(SwitchTouch, DmaFirstEndOfCountLost)
{
    run(scenarioDmaFirstLost);
}

TEST(SwitchTouch, DmaEndOfCountLostLater)
{
    run(scenarioDmaLostLater);
}

TEST(SwitchTouch, ChainBroken)
{
    run(scenarioChainBroken);
}

TEST(SwitchTouch, DmaOpenFails)
{
    run(scenarioDmaOpenFails);
}

TEST(SwitchTouch, Measure)
{
    run(scenarioMeasure);
}

TEST(SwitchTouch, Filter)
{
    run(scenarioFilter);
}

TEST(SwitchTouch, Palm)
{
    run(scenarioPalm);
}

TEST(SwitchTouch, Edge)
{
    run(scenarioEdge);
}

TEST(SwitchTouch, FourFingers)
{
    run(scenarioFourFingers);
}

TEST(SwitchTouch, Cadence200HzAndCompaction)
{
    run(scenarioCadence200Hz);
}

TEST(SwitchTouch, SingleReadFallback)
{
    run(scenarioSingleReadFallback);
}

TEST(SwitchTouch, FifoFlushBehavior)
{
    run(scenarioFifoFlushBehavior);
}

TEST(SwitchTouch, TegraI2CFilterOnlyNoWorkLoop)
{
    run(scenarioFilterOnlyNoWorkLoop);
}

TEST(SwitchTouch, TegraI2CDrainAtEndNonChained)
{
    run(scenarioDrainAtEndNonChained);
}

TEST(SwitchTouch, TegraI2CTimeoutQuiesceAndRecover)
{
    run(scenarioTimeoutQuiesceAndRecover);
}

TEST(SwitchTouch, JoyConTouchPreemptHook)
{
    run(scenarioJoyConTouchPreemptHook);
}

TEST(SwitchTouch, SharedWorkLoop)
{
    run(scenarioSharedWorkLoop);
}
