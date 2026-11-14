// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 (T210/T210B01) I2C master for statically linked Switch drivers.
//
// Buses use Tegra numbering, 1..5 (I2C1 0x7000C000 .. I2C5/PWR_I2C 0x7000D000):
//   I2C1 100 kHz  MAX17050 fuel gauge 0x36, BQ24193 charger 0x6B, BM92T36 USB-PD 0x18
//   I2C2 400 kHz  BH1730FVC ALS 0x29, our only user (Atmosphere i2c_bus_device_map.inc also lists the INA226
//                 power monitors 0x40-0x4e of development units and Hoag's NFC IC 0x08). Horizon enables a bus
//                 regulator at 2.9 V before using I2C2 (i2c_bus_accessor.cpp); the only 2.9 V rail is MAX77620 LDO6
//                 (switchbrew PCV: "TouchPanel, ALS"), so the pull-ups are taken to be on LDO6 (inferred).
//   I2C3 400 kHz  FTM4 touch 0x49 (L4T tegra210-soc-base.dtsi i2c3 clock-frequency 400000; hekate the same)
//   I2C4 100 kHz  CAM_I2C pads
//   I2C5 400 kHz  MAX77620 PMIC 0x3C (shared, see below)
// I2C6 (DPAUX1 pads) is not supported.
//
// What the boot chain leaves behind (hekate bdk/soc/hw_init.c, l4t.c; switch U-Boot):
//   * hekate hw_init enables + pinmuxes I2C1 and I2C5 (CLK_M source) and initializes both;
//     hw_deinit and the L4T launcher do not undo that. Touch power-off disables I2C3.
//   * switchroot U-Boot only probes I2C5 (board/nintendo/switch: MAX77620), re-clocking it
//     from PLLP at 400 kHz and leaving it in packet mode.
// tegra_i2c_init() therefore owns I2C1..I2C4 outright: CAR reset/clock-enable with CLK_M
// (19.2 MHz) as source, pinmux (function I2C, input enabled), controller config and a bus
// clear, mirroring hekate clock_enable()/pinmux_config_i2c()/i2c_init(). I2C5 is adopted
// as-is when it is already clocked: it is never reset or re-clocked because its clock and
// config are firmware-owned and CL-DVFS shares the PWR_I2C pads (hekate hw_init.c enables
// CL-DVFS "to avoid issues with I2C5 sharing"); runtime use by the T210B01 BPMP firmware is
// unverified. A PMIC driver on I2C5 must keep transfers short. Unless switch-no-dfll the
// DFLL's own I2C master may write VDD_CPU on these pads, so I2C5 runs in MULTI_MASTER_MODE (as L4T
// runs every T210 controller). CL-DVFS grants the pads to one master at a time (Tegra X1 TRM 35.4), so a transfer
// that loses arbitration anyway is a fault: one line, a FIFO flush (never a reset or bus clear) and TEGRA_I2C_EARB
// to the caller, which has its own error path. (On Hoag only, MAX77620 LDO8 also powers the I2C1 pads.) The touch panel
// needs its own supplies/reset GPIOs before it answers on I2C3.
//
// Transfers are serialized per bus with an IOLock; the API is synchronous and every completion comes by interrupt.
// tegra_i2c_init() claims the bus's SPI, I2C1 38, I2C2 84, I2C3 92, I2C4 120, I2C5 53 (Linux tegra210.dtsi:676/691/
// 706/721/739; the OLED DTB i2c@7000c000.. nodes), sleeping in switch_interrupt_nub() until the GIC is up; a caller
// sleeps until the work loop action reports packet completion, NACK or arbitration loss (FIFO refills and drains
// happen there too; the bus clear completes by INT_BUS_CLEAR_DONE). There is no polled mode (tegra_i2c_final_transfer,
// the power-off's last transfers, and tegra_i2c_raw_write, debug power marks with interrupts off, aside): an interrupt that cannot be attached, or a wait that times out while the
// controller asserts an unmasked status bit, panics with the controller and GIC state. I2C5 is no exception:
// switchroot's L4T kernel drives i2c@7000d000 by interrupt like the other buses (OLED DTB node: interrupts SPI 53,
// status okay; Linux i2c-tegra.c probe requests every non-DVC bus's IRQ), and nothing else uses that register block
// at runtime (CL-DVFS is its own initiator on the PWR_I2C pads; the PMC bootrom-commands on controller 4 run only in
// the BootROM on reset). Call only from thread context, never with a spinlock held or from an interrupt/filter
// handler. After a failed transfer an owned bus is reset and reinitialized (as Linux i2c-tegra does).
//
// I2C3 is a chained bus: once its owner's check has turned that on (tegra_i2c_set_chained), a write of up
// to 8 bytes then a read is one submission ending in one interrupt and a read that fits the 32-byte RX FIFO takes no RX
// interrupts; once tegra_i2c_set_dma() has turned that on too, a longer read's payload goes by APB DMA (TegraI2C.cpp's
// file comment). Until then, and for good after a failed check, the bus works as the others do.
#ifndef TEGRA_I2C_H
#define TEGRA_I2C_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_I2C_OK = 0,
    TEGRA_I2C_EINVAL = -1,     // bad bus/address/length
    TEGRA_I2C_ENOMEM = -2,     // mapping or lock allocation failed
    TEGRA_I2C_ENOTREADY = -3,  // tegra_i2c_init() not called (or failed) for this bus
    TEGRA_I2C_ENACK = -4,      // device did not acknowledge
    TEGRA_I2C_EARB = -5,       // arbitration lost / bus stuck
    TEGRA_I2C_ETIMEDOUT = -6,  // controller did not finish in time
    TEGRA_I2C_EIO = -7,        // packet completed short
    TEGRA_I2C_ENODMA = -8,     // tegra_i2c_transfer_dma: the bus's DMA channel could not be had
};

// Cumulative since boot, for a driver's stats line (the caller differences two reads).
struct tegra_i2c_counters {
    uint32_t transfers;   // transfers that reached the bus
    uint32_t interrupts;  // the bus's interrupt actions (the controller's, and DMA ends of count)
    uint32_t dmaReads;    // reads whose payload came by APB DMA
    bool dmaOff;          // long reads go by PIO (DMA never turned on, or off after a failed check)
    bool chainOff;        // one packet per message (not a chained bus, or its check has not passed)
};

// Longest single read or write payload, and longest register write (tegra_i2c_write).
#define TEGRA_I2C_MAX_TRANSFER  4096
#define TEGRA_I2C_MAX_REG_WRITE 63

// Maps the controller, CAR and pinmux (once), claims the bus's interrupt and brings the bus up.
// Idempotent and thread-safe; later calls for an initialized bus return TEGRA_I2C_OK immediately.
// May sleep until the GIC is up (panics after 10 s, in switch_interrupt_nub).
int tegra_i2c_init(unsigned bus);

// Write tx (if txlen) then, after a repeated START, read rx (if rxlen). 7-bit address.
int tegra_i2c_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen);

// Register read: write reg, repeated START, read len bytes.
int tegra_i2c_read(unsigned bus, uint8_t addr, uint8_t reg, void *buf, size_t len);

// Register write: reg followed by len (<= TEGRA_I2C_MAX_REG_WRITE) bytes, then STOP.
int tegra_i2c_write(unsigned bus, uint8_t addr, uint8_t reg, const void *buf, size_t len);

// One line for a driver's give-up message: the bus's pinmux and drive (now, and as the boot chain left them), CNFG,
// CLK_DIVISOR and INTERFACE_TIMING_0, the last bus clear's outcome and BUS_CLEAR_STATUS, and the last failed packet's
// address, INT_STATUS and PACKET_TRANSFER_STATUS as the controller had them before recovery reset it (address or data
// NACK, bytes sent). Thread context; waits for a transfer in flight.
void tegra_i2c_describe(unsigned bus, char *buffer, size_t size);

// The last transfers of the boot: SwitchPMIC's Shut Down / Restart (PE_halt_restart), and nothing else. The one polled
// path in this driver: the packet is queued and INT_STATUS polled until it ends (same budget as an interrupt-driven
// packet, then TEGRA_I2C_ETIMEDOUT), because nothing is left to interrupt by then and the caller may run with
// interrupts off, where neither the GIC nor the work loop answers. The first call takes the bus for good (waiting for
// a transfer in flight when it may sleep; with interrupts off it takes the bus as it finds it) and masks the
// controller's interrupts: transfers by anyone else wait forever after it. Same arguments and limits as
// tegra_i2c_transfer().
int tegra_i2c_final_transfer(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen);

// A register write (1..8 bytes: the register, then its values) for code that runs where this driver cannot: interrupts
// off, boot CPU only, the buses suspended (SC7 entry) or not yet resumed (SC7 exit, the controller's clock and reset
// back at their reset values). No driver state and no lock: the caller is the only code running. A controller whose
// clock is off or held in reset is clocked from CLK_M and set up as tegra_i2c_init() would (pads included), and is
// left that way; then one packet, its INT_STATUS polled to the end within the same budget as a 100 kHz packet. Needs
// tegra_i2c_init() to have run once this boot (the register mappings). Debug power marks (SwitchPowerTrail.h) only.
int tegra_i2c_raw_write(unsigned bus, uint8_t addr, const uint8_t *data, size_t len);

// A chained bus's (I2C3) optional paths, for its owner's one-time checks, in this order: the same transfer as
// tegra_i2c_transfer (a write of 1..8 bytes, then the read) as one chained submission by PIO, or with its read by APB
// DMA whatever its length (up to 4096 bytes; the first call claims the bus's DMA channel; needs the chained path on).
// TEGRA_I2C_OK with the data, else an error with the reason in `why` and the bus recovered, never a panic for the
// path's own failure (a chain that never completes times out; a first DMA end of count that never reaches the CPU is
// reported). Neither turns anything on: the caller compares the data with a one-packet read it knows to be the same
// (a chip ID) and then calls tegra_i2c_set_chained() / tegra_i2c_set_dma(). Thread context.
int tegra_i2c_transfer_chained(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen,
                               char *why, size_t whySize);
int tegra_i2c_transfer_dma(unsigned bus, uint8_t addr, const void *tx, size_t txlen, void *rx, size_t rxlen, char *why,
                           size_t whySize);
// Whether the bus chains write-then-reads (and drains short reads at their completion) from now on; off until called.
// Off also turns DMA off. Thread context.
void tegra_i2c_set_chained(unsigned bus, bool on);
// Whether reads above the RX FIFO (32 bytes) of a chained write-then-read go by DMA from now on; off until called, and
// off for good on a bus whose DMA channel could not be had or that is not chaining. Thread context.
void tegra_i2c_set_dma(unsigned bus, bool on);
// The bus's counters (zeros for a bad bus). Any thread; a snapshot, not taken under the bus's lock.
void tegra_i2c_get_counters(unsigned bus, struct tegra_i2c_counters *out);

// System sleep (drivers/SwitchSuspend), thread context, both from the same thread: suspend holds every bus (later
// transfers wait) until resume has re-clocked and reconfigured the controllers after SC7. Panics if a bus does not
// come back.
void tegra_i2c_suspend(void);
void tegra_i2c_resume(void);

#ifdef __cplusplus
}
#endif
#endif
