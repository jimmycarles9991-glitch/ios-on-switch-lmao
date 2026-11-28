// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 (T210/T210B01) SPI master for statically linked Switch drivers. Only SPI4 (SBC4, 0x7000DA00) is described:
// it carries the console's 6-axis IMU (drivers/SwitchMotion), its only device.
//
// Hardware (Linux drivers/spi/spi-tegra114.c, "nvidia,tegra210-spi": intr_mask_reg, no DMA here; tegra210.dtsi
// spi@7000da00: GIC SPI 93, CAR clock and reset 68 = U bit 4, CLK_SOURCE_SBC4 0x1b4 with parents pll_p/clk_m;
// switchroot tegra210-nx-sensor.dtsi imu@0: CS 0, SPI mode 3, spi-max-frequency 8 MHz; tegra210b01-fric-pinmux.dtsi
// "IMU access": spi4_mosi_pc7/miso_pd0/sck_pc5/cs0_pc6, function spi4, MISO pulled up and input-enabled):
//   * tegra_spi_init() resets and clocks the controller from PLLP (408 MHz / 51 = 8 MHz, the 7.1 divider 100), muxes
//     the four pads, programs master mode 3 with the chip select under software control, and claims the controller's
//     interrupt (INTID 125), sleeping in switch_interrupt_nub() until the GIC is up.
//   * A transfer is one PIO block of 8-bit packets, full duplex, with the chip select held low from before the first
//     clock to after the last. Longer than three bytes, the packets go four to a FIFO word (packed mode, TRM 37.1.2.4,
//     byte 0 first, as spi-tegra114.c packs them), so the 64-word FIFO takes 256 bytes and the CPU moves a word per four
//     bytes each way. TRM 37.1.2.2 keeps DMA mode for blocks over the FIFO's 64 words; nothing here sends one. The
//     caller sleeps until the controller's RDY interrupt, whose filter (primary interrupt context) clears it and wakes
//     the caller, which drains the RX FIFO. There is no polled mode: a wait that times out while TRANS_STATUS shows RDY
//     panics with the controller and GIC state.
// Call from thread context only. Transfers on one bus are serialized by an IOLock.
#ifndef TEGRA_SPI_H
#define TEGRA_SPI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_SPI_OK = 0,
    TEGRA_SPI_EINVAL = -1,     // bad bus, chip select or length
    TEGRA_SPI_ENOMEM = -2,     // mapping, lock or work loop allocation failed
    TEGRA_SPI_ENOTREADY = -3,  // tegra_spi_init() not called (or failed), or the bus is suspended
    TEGRA_SPI_ETIMEDOUT = -4,  // the controller did not finish (and showed no pending interrupt)
    TEGRA_SPI_EIO = -5,        // FIFO overflow/underflow, or fewer words came back than were clocked
};

// Longest transfer: one PIO block of the controller's 64-word FIFO, four bytes per word.
#define TEGRA_SPI_MAX_TRANSFER 256

// Maps the controller, CAR and pinmux (once), brings the bus up and claims its interrupt. Idempotent and
// thread-safe. May sleep until the GIC is up (panics after 10 s, in switch_interrupt_nub).
int tegra_spi_init(unsigned bus);

// Clocks len bytes out of tx (zeros if tx is null) while clocking len bytes into rx (dropped if rx is null), with
// chip select `cs` asserted throughout.
int tegra_spi_transfer(unsigned bus, unsigned cs, const void *tx, void *rx, size_t len);

// System sleep, for the bus's one client (drivers/SwitchMotion's power hooks), thread context. Suspend waits for a
// transfer in flight, gates the controller's clock and holds the bus (later transfers wait); resume clocks it again,
// reprograms it (SC7 resets the controller and its CAR state; the pinmux is restored by drivers/SwitchSuspend) and lets
// them through. No-ops for a bus that was never brought up.
void tegra_spi_suspend(unsigned bus);
void tegra_spi_resume(unsigned bus);

#ifdef __cplusplus
}
#endif
#endif
