// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 APB DMA controller (T210/T210B01, "nvidia,tegra210-apbdma" = tegra148 chip data) for statically linked
// Switch drivers: the UARTs' DMA, as Linux serial-tegra.c drives it through drivers/dma/tegra20-apb-dma.c, and I2C3's
// long reads (TegraI2C), as Linux i2c-tegra.c does.
//
// Hardware (Linux tegra20-apb-dma.c, tegra210.dtsi dma@60020000): 0x60020000, 32 channels of 0x40 bytes from +0x1000
// (CSR, STATUS, CSRE pause, AHB pointer/sequence, APB pointer/sequence, WCOUNT, WORD_TRANSFER), CAR clock and reset 34
// (H bit 2). Channel n raises GIC SPI 104 + n (n < 16) or 128 + n - 16, level high. The engine does not snoop the CPU
// caches and takes 32-bit memory addresses: buffers come from tegra_apbdma_alloc (physically contiguous, below 4 GiB,
// cacheable), cleaned before the engine reads them and invalidated before the CPU reads what it wrote. Its memory
// client (PPCS) is not translated by the SMMU (ppcs-asid bit 31 clear).
//
// Channels are fixed per user (one owner each), so nothing is allocated at run time. A channel's end-of-count
// interrupt is acknowledged in primary interrupt context and the owner's handler runs there too, on CPU0 like every
// SPI: it may take the owner's simple lock (IOSimpleLockLock: interrupts are already off) and signal its own work loop.
//
// tegra_apbdma_open() checks that the controller and the channel take their registers and fails with a reason (the
// caller logs one line and keeps its PIO path). A completion that never arrives while the channel shows it done is a
// lost interrupt: tegra_apbdma_describe() gives the registers for the owner's panic line.
#ifndef TEGRA_APBDMA_H
#define TEGRA_APBDMA_H
#include <stddef.h>
#include <stdint.h>

class IOWorkLoop;

// Request selectors (tegra210.dtsi dmas = <&apbdma N>) and the fixed channels.
enum : unsigned {
    TEGRA_APBDMA_REQ_UARTB = 9,
    TEGRA_APBDMA_REQ_UARTC = 10,
    TEGRA_APBDMA_REQ_UARTD = 19,
    TEGRA_APBDMA_REQ_I2C3 = 23,  // i2c@7000c500 dmas = <&apbdma 23>, <&apbdma 23> (rx, tx)
};

enum : unsigned {
    TEGRA_APBDMA_CH_UARTB_RX = 0,  // Joy-Con (R) rail
    TEGRA_APBDMA_CH_UARTC_RX = 1,  // Joy-Con (L) rail
    TEGRA_APBDMA_CH_UARTD_TX = 2,  // Bluetooth
    TEGRA_APBDMA_CH_UARTD_RX = 3,  // Bluetooth (UART-D's request line serves both directions, as Linux's two channels)
    TEGRA_APBDMA_CH_I2C3_RX = 4,   // touch panel's long event reads (TegraI2C)
    TEGRA_APBDMA_CHANNELS = 5,
};

// The longest transfer one programming takes (WCOUNT is 16 bits of bytes, word-granular).
#define TEGRA_APBDMA_MAX_BYTES 65536u

struct tegra_apbdma_buffer {
    uint8_t *va;  // kernel mapping, cacheable
    uint32_t pa;  // physical, below 4 GiB, contiguous for `size`
    uint32_t size;
    void *memory;  // the IOBufferMemoryDescriptor
};

struct tegra_apbdma_channel;
typedef void (*tegra_apbdma_handler)(void *context);

// A buffer for one channel's transfers: `size` bytes (page-rounded), zeroed and written back.
bool tegra_apbdma_alloc(uint32_t size, struct tegra_apbdma_buffer *out);

// Brings the controller up once (clock, reset, enable, every channel's interrupt unmasked at the controller), then
// claims `channel` with its interrupt on `loop`. `handler(context)` runs in primary interrupt context after each
// end-of-count the channel raises, already acknowledged. Thread context; may sleep until the GIC is up. Returns null
// with a reason in `why` when the hardware does not behave (never panics for that).
struct tegra_apbdma_channel *tegra_apbdma_open(unsigned channel, IOWorkLoop *loop, tegra_apbdma_handler handler,
                                               void *context, char *why, size_t whySize);

// Device to memory, continuously: one 8-bit APB read of `apb` (a UART's RBR) per byte, four to a burst into
// [pa, pa + size) (size a multiple of 4), flow-controlled by request line `req`, round and round; end-of-count (and the
// handler) at every wrap. The caller's lock.
void tegra_apbdma_start_ring(struct tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t size);
// Memory to device, once: `size` bytes (a multiple of 16, at most TEGRA_APBDMA_MAX_BYTES) from `pa` (4-byte aligned)
// to `apb` (a UART's THR), 16-byte bursts on request line `req`; end-of-count (and the handler) when the last has gone.
void tegra_apbdma_start_once(struct tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa, uint32_t size);
// Device to memory, once, a byte at a time: `words` 32-bit APB reads of `apb` (a UART's RBR: each pops one byte, in
// bits 7:0, as the CPU's 32-bit reads do), one read per request on line `req` (a UART in DMA mode with its RX trigger
// at one byte), each read stored as one word from `pa` (4-byte aligned; at most TEGRA_APBDMA_MAX_BYTES / 4 words).
// End-of-count (and the handler) after the last. No byte is left below a burst for PIO to take, so the channel is the
// FIFO's only reader while it runs (SwitchBluetooth "RX by APB DMA").
void tegra_apbdma_start_rx_words(struct tegra_apbdma_channel *ch, uint32_t apb, unsigned req, uint32_t pa,
                                 uint32_t words);
// Bytes moved in the current pass of the transfer last started (0 .. size, word-granular), and whether an end-of-count
// is pending that the interrupt has not taken yet (the count then belongs to the next pass, or the transfer is done).
uint32_t tegra_apbdma_moved(const struct tegra_apbdma_channel *ch, bool *endPending);
// The channel off, its interrupt status cleared. Bytes in flight in a burst may still land (the caller stops the
// device's requests first).
void tegra_apbdma_stop(const struct tegra_apbdma_channel *ch);
bool tegra_apbdma_running(const struct tegra_apbdma_channel *ch);
// Interrupts taken (end-of-counts) since open.
uint32_t tegra_apbdma_interrupts(const struct tegra_apbdma_channel *ch);
// One line of the channel's registers and its interrupt's GIC state, for a log or panic line.
void tegra_apbdma_describe(const struct tegra_apbdma_channel *ch, char *buffer, size_t size);

#endif
