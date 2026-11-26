// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 (T210/T210B01) GPIO controller with per-pin interrupts, for statically linked Switch drivers.
//
// TegraGPIO is an IOInterruptController. tegra_gpio_interrupt_nub() hands out one IOService nub per pin whose
// interrupt index 0 is that pin, so a driver uses the ordinary IOKit event sources on its own work loop:
//
//   IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_PX4, TEGRA_GPIO_LEVEL_LOW);
//   auto *source = IOInterruptEventSource::interruptEventSource(this, &Driver::alert, nub, 0);
//   workLoop->addEventSource(source);
//   source->enable();
//
// A level pin is masked at the GPIO controller while its event source's action runs and unmasked when the action
// returns (IOKit's usual model), so the action must make the device release the line. Edge pins are acknowledged
// before the handler runs, so an edge during the action is not lost.
//
// Hardware (Linux drivers/gpio/gpio-tegra.c, tegra210_gpio_config; hekate bdk/soc/gpio.c): 0x6000D000, 8 banks of
// 4 ports of 8 pins; each bank has one level-high GIC SPI, 32 33 34 35 55 87 89 125 (Linux tegra210.dtsi:491-498,
// the OLED DTB gpio@6000d000, hekate gpio.c:43-50). Numbering is Linux's (dt-bindings/gpio/tegra-gpio.h):
// port A=0 .. Z=25, AA=26 .. FF=31, gpio = port * 8 + pin, bank = gpio / 32.
//
// Pins used by Switch drivers (Switch OLED, tegra210b01-fric DTB; switchroot tegra210b01-nx-common.dtsi):
//   PX1 185  FTM4 touch INT (I2C3 0x49)      level low   bank 5 SPI 87
//   PX2 186  IMU INT2 data-ready (SPI4 CS0)  level high  bank 5 SPI 87
//   PX4 188  TMP451 ALERT (I2C1 0x4c)        level low   bank 5 SPI 87
//   PX6 190  volume up, active low, pull-up  both edges  bank 5 SPI 87
//   PX7 191  volume down, active low         both edges  bank 5 SPI 87
//   PY0 192  MAX17050 ALRT (I2C1 0x36)       falling     bank 6 SPI 89
//   PZ0 200  BQ24193 INT (I2C1 0x6b)         falling     bank 6 SPI 89
//   PZ1 201  SD card detect (SDMMC1), active low, pull-up  rising  bank 6 SPI 89
//   PK4  84  BM92T36 ALERT (I2C1 0x18)       DT: rising  bank 2 SPI 34
//   PJ7  79  FTM4 touch reset/VIO enable     output, active high (DT stm,vio-gpio; hekate touch.c)
//   PX3 187  v_vdd5v3 enable (USB-C source) output, active high (DT tegra210-nx-power-fixed regulator@0xb)
//   PK5  85  BM92T36 VCONN enable            output, active high (DT rohm,vconn-en-gpio)
// The power button is not a GPIO on this board (gpio-keys "power" PX5 is disabled, "Not connected"): it is the
// MAX77620 ONOFF interrupt on the PMIC's nIRQ, GIC SPI 86 (switch_interrupt_nub(118, false)).
//
// For these pins the pinmux is set here too (inputs: input enabled, tristate, the DT pull; the outputs PJ7, PX3, PK5: driven,
// input disabled, pull-down; tegra210b01-fric-pinmux.dtsi); any other pin must already be muxed. Call everything
// from thread context.
//
// Touch (level low: the action must read the FTM4 event FIFO so the panel releases INT before returning):
//   IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_TOUCH_INT, TEGRA_GPIO_LEVEL_LOW);
// Buttons (both edges; read the level in the action, 0 = pressed; hardware debounce as the DT's 16 ms):
//   tegra_gpio_set_debounce(TEGRA_GPIO_VOLUME_UP, 16);
//   IOService *nub = tegra_gpio_interrupt_nub(TEGRA_GPIO_VOLUME_UP, TEGRA_GPIO_EDGE_BOTH);
//   ... in the action: bool pressed = tegra_gpio_read(TEGRA_GPIO_VOLUME_UP) == 0;
// Outputs (touch power-up, hekate touch_power_on: reset low, supplies on, then high):
//   tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 0); ... tegra_gpio_output(TEGRA_GPIO_TOUCH_RESET, 1);
// One event source per nub (a pin has one owner). There is no polled mode anywhere: a consumer that gets nullptr, or
// whose interrupt never arrives while the pin shows the event (tegra_gpio_read), panics and names it, with
// tegra_gpio_describe() in the message.
#ifndef TEGRA_GPIO_H
#define TEGRA_GPIO_H
#include <stddef.h>
#include <stdint.h>

// Ports, as Linux dt-bindings/gpio/tegra-gpio.h: TEGRA_GPIO(X, 1) == 185 == PX1.
enum {
    TEGRA_GPIO_PORT_A,
    TEGRA_GPIO_PORT_B,
    TEGRA_GPIO_PORT_C,
    TEGRA_GPIO_PORT_D,
    TEGRA_GPIO_PORT_E,
    TEGRA_GPIO_PORT_F,
    TEGRA_GPIO_PORT_G,
    TEGRA_GPIO_PORT_H,
    TEGRA_GPIO_PORT_I,
    TEGRA_GPIO_PORT_J,
    TEGRA_GPIO_PORT_K,
    TEGRA_GPIO_PORT_L,
    TEGRA_GPIO_PORT_M,
    TEGRA_GPIO_PORT_N,
    TEGRA_GPIO_PORT_O,
    TEGRA_GPIO_PORT_P,
    TEGRA_GPIO_PORT_Q,
    TEGRA_GPIO_PORT_R,
    TEGRA_GPIO_PORT_S,
    TEGRA_GPIO_PORT_T,
    TEGRA_GPIO_PORT_U,
    TEGRA_GPIO_PORT_V,
    TEGRA_GPIO_PORT_W,
    TEGRA_GPIO_PORT_X,
    TEGRA_GPIO_PORT_Y,
    TEGRA_GPIO_PORT_Z,
    TEGRA_GPIO_PORT_AA,
    TEGRA_GPIO_PORT_BB,
    TEGRA_GPIO_PORT_CC,
    TEGRA_GPIO_PORT_DD,
    TEGRA_GPIO_PORT_EE,
    TEGRA_GPIO_PORT_FF,
};

#define TEGRA_GPIO(port, pin) (TEGRA_GPIO_PORT_##port * 8 + (pin))

// Named pins of this board (table above).
enum {
    TEGRA_GPIO_PJ7 = 9 * 8 + 7,
    TEGRA_GPIO_PK4 = 10 * 8 + 4,
    TEGRA_GPIO_PX1 = 23 * 8 + 1,
    TEGRA_GPIO_PX2 = 23 * 8 + 2,
    TEGRA_GPIO_PX4 = 23 * 8 + 4,
    TEGRA_GPIO_PX6 = 23 * 8 + 6,
    TEGRA_GPIO_PX7 = 23 * 8 + 7,
    TEGRA_GPIO_PY0 = 24 * 8 + 0,
    TEGRA_GPIO_PZ0 = 25 * 8 + 0,
    TEGRA_GPIO_PZ1 = 25 * 8 + 1,
    TEGRA_GPIO_TOUCH_INT = TEGRA_GPIO_PX1,    // TEGRA_GPIO_LEVEL_LOW
    TEGRA_GPIO_IMU_INT = TEGRA_GPIO_PX2,      // TEGRA_GPIO_LEVEL_HIGH
    TEGRA_GPIO_TOUCH_RESET = TEGRA_GPIO_PJ7,  // output, 1 = running
    TEGRA_GPIO_TEMP_ALERT = TEGRA_GPIO_PX4,   // TEGRA_GPIO_LEVEL_LOW
    TEGRA_GPIO_VOLUME_UP = TEGRA_GPIO_PX6,    // pressed = 0; TEGRA_GPIO_EDGE_BOTH, debounce 16 ms (DT)
    TEGRA_GPIO_VOLUME_DOWN = TEGRA_GPIO_PX7,  // pressed = 0; TEGRA_GPIO_EDGE_BOTH, debounce 16 ms (DT)
    TEGRA_GPIO_GAUGE_ALERT = TEGRA_GPIO_PY0,
    TEGRA_GPIO_CHARGER_INT = TEGRA_GPIO_PZ0,
    TEGRA_GPIO_SD_CD = TEGRA_GPIO_PZ1,  // 0 = inserted, 1 = removed; TEGRA_GPIO_EDGE_RISING
    TEGRA_GPIO_PD_ALERT = TEGRA_GPIO_PK4,
    TEGRA_GPIO_COUNT = 256,
};

// Trigger types; the values are Linux's IRQ_TYPE_* (dt-bindings/interrupt-controller/irq.h), as in DT cells.
enum {
    TEGRA_GPIO_EDGE_RISING = 1,
    TEGRA_GPIO_EDGE_FALLING = 2,
    TEGRA_GPIO_EDGE_BOTH = 3,
    TEGRA_GPIO_LEVEL_HIGH = 4,
    TEGRA_GPIO_LEVEL_LOW = 8,
};

#ifdef __cplusplus
class IOService;
extern "C" {
#endif

// Starts the controller: maps it, masks every pin interrupt, claims the eight bank SPIs from the GIC
// (switch_interrupt_nub: sleeps until the GIC is up, panics after 10 s) and registers the "TegraGPIO" interrupt
// controller. tegra_gpio_interrupt_nub() calls it on first use, so calling it is optional. Returns 0 on success (or
// if already started), -1 only if memory or the register mappings could not be had. Panics if a bank's interrupt
// cannot be attached.
int tegra_gpio_start(void);

// Input level of a pin (0 or 1), or -1 before tegra_gpio_start() or for a bad pin: GPIO_IN, meant for a pin in GPIO
// mode. A pin left to its special function (CNF 0) may not be shown (both I2C2 lines have read 0 while the I2C
// controller saw SDA high; unverified); tegra_gpio_sample() reads those.
int tegra_gpio_read(unsigned gpio);

// Diagnostics only: the pad's level read with the pin briefly a GPIO input (OE 0, CNF 1, 2 us, IN), its CNF and OE
// written back after. Only for a pin whose special function is idle and not driving it (an I2C bus between transfers,
// with that bus's owner calling): the pad is never driven by this. `config` (may be null) gets the pin as found:
// bit 0 CNF, 1 OE, 2 OUT, 3 GPIO_IN. Returns 0 or 1, or -1 before tegra_gpio_start() or for a bad pin.
int tegra_gpio_sample(unsigned gpio, unsigned *config);

// Diagnostics only, for any pin (an output included): its GPIO registers as they are, nothing written. Bit 0 CNF,
// 1 OE, 2 OUT, 3 GPIO_IN (meaningful only with the pad's input enabled); -1 before tegra_gpio_start() or for a bad pin.
int tegra_gpio_config(unsigned gpio);

// One line for a panic message: the pin's CNF/OE/IN/INT_ENB/INT_STA/INT_LVL bits and trigger, then its bank's GIC
// state (switch_interrupt_describe).
void tegra_gpio_describe(unsigned gpio, char *buffer, size_t size);

// Lost-interrupt check for consumers without a natural wait (call it from a slow timer or before trusting a level
// read): if the pin's interrupt is pending and enabled at the GPIO controller and still is 50 ms later, the bank
// interrupt never reached the CPU: panics "<who>: GPIO n interrupt lost: <tegra_gpio_describe>". May sleep.
void tegra_gpio_check_delivery(unsigned gpio, const char *who);

// Hardware debounce, 0 (off) to 255 ms. The counter is per port: the longest request on a port wins.
int tegra_gpio_set_debounce(unsigned gpio, unsigned ms);

// Drives a pin as a GPIO output at `value` (0 or 1): pinmux for the board's output pins (table above), then OUT, OE
// and CNF, as hekate gpio_direction_output. Starts the controller if needed. 0 on success, -1 for a bad pin or if
// the controller could not be started.
int tegra_gpio_output(unsigned gpio, int value);

// System sleep (drivers/SwitchSuspend): save every port before SC7 and write it back after, with the controller's
// clock re-enabled. Boot CPU, interrupts off, other cores off; no-ops before tegra_gpio_start().
void tegra_gpio_suspend(void);
void tegra_gpio_resume(void);

#ifdef __cplusplus
// Interrupt nub for one pin (interrupt index 0), configured as a GPIO input with the given trigger. Starts the
// controller if needed (may sleep until the GIC is up). The same nub is returned for repeated calls; the trigger can
// change only while no event source is registered on it. nullptr only for a bad pin or trigger, a trigger conflict
// or no memory; the caller panics then. The registry keeps the nub (it is attached under the controller).
IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned trigger);
#endif

#ifdef __cplusplus
}
#endif
#endif
