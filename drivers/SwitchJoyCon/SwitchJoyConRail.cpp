// SPDX-License-Identifier: BSD-2-Clause-Patent
// Joy-Cons on the Switch rails: UART-B (right) and UART-C (left), presented to iPadOS as Joy-Con HID devices.
//
// Hardware (switchroot tegra210b01-nx-common.dtsi 566-595, the OLED fric DT includes it unchanged; hekate
// bdk/input/joycon.c; L4T drivers/input/joystick/joycon-serdev.c, cited J:line):
//   * UART-B 0x70006040 GIC SPI 37 (INTID 69), UART-C 0x70006200 SPI 46 (INTID 78), both level high
//     (tegra210-soc-base.dtsi 860-892); clock from PLLP through the UART source divider (CAR 0x17C, 0x1A0; hekate
//     clock.c 84-89, 223-239: 408 MHz / 25.5 = 16 MHz for 1 Mbaud, / 8.5 = 48 MHz for 3 Mbaud, DLL 1).
//     nvidia,invert-txd and nvidia,invert-rts (IRDA_CSR bits 1 and 3); hardware RTS (MCR RTS_EN), as hekate's
//     UART_AO_TX_HW_RX; two stop bits at 3 Mbaud (J:1664-1683, hekate uart.c).
//   * Detection: detect-gpios PH6 (R) / PE6 (L), active low, falling-edge interrupt (J:3507-3513). The pin shares the
//     rail net with the UART TX pad (detect-en-gpios PG0 / PD1), so while waiting the TX pad is a GPIO input (not
//     driven) and the pin reads the Joy-Con's pull-down; once it falls the pad goes back to the UART (J:1685-1771).
//   * Charging: regulators "left/right-jc-reg" = GPIO PCC3 / PK3, active high, 4.8 V from the 5 V rail (GPIO PA5,
//     which SwitchThermal turns on and keeps on) (tegra210-nx-power-fixed.dtsi 180-203). As L4T: on while
//     detecting and below "high"; off at "full" (J:1176-1186, 1724).
//   * Pads: all four UART pads in UART (SFIO) mode, not just TX; the UART IO rail (VDDIO_UART, 1.8 V) powered and out
//     of deep power-down (Linux tegra210_io_pads "uart": DPD bit 14, IO-power bit 2; hekate hw_init clears every
//     NO_IOPOWER bit but MEM/SDMMC1); tristated inputs not clamped (APB_MISC_PP_PINMUX_GLOBAL bit 0, which hekate
//     _config_gpios and U-Boot pinmux_init clear): otherwise RX reads a constant and the Joy-Con is never heard.
//     The UART drives its inverted idle (low) before the TX pad leaves GPIO mode, so the line never rises (a rise
//     is what a detach looks like to the Joy-Con). The TX and RTS pads have their input receivers on (harmless on a
//     driven pad; hekate does it for TX while detecting) so the give-up line can report the real level of all four.
//
// Protocol (hekate joycon.c 36-218, 299-312; J:367-383, 2583-2813):
//   packet = 19 01 03 (host) / 19 81 03 (Joy-Con), u16 LE length of the rest + 5 = total, cmd, subcmd,
//            u16 LE payload length, status, payload CRC, header CRC, payload. CRC-8 poly 0x8D init 0 MSB-first over
//            the six bytes cmd..payload CRC (the handshake carries 0). Joy-Con packets as short as cmd and subcmd (7
//            bytes) are taken, as hekate (joycon.c 917, any 8 bytes with the magic) and L4T (J:3032, size + 5) do.
//   handshake at 1 Mbaud: A1 A2 A3 A4, then A5 02 01 7E (reply cmd A5); 91/01 get info (reply 94/01: type, BD_ADDR
//   LSB first); 91/20 set 3 Mbaud (reply 94/20, then switch); at 3 Mbaud 91/11 HID disconnect, 91/10 HID connect,
//   91/12 report interval 15 ms (replies 94/same; deku README 101-123 has every byte). The Joy-Con does not hear the
//   first packets after its own switch: hekate sends 91/11 every 5 ms, "reply expected after 30ms" (joycon.c
//   1325-1341), L4T retries it after a timeout (J:607-612). Sent once, it is lost, and the Joy-Con then sits at
//   3 Mbaud, deaf to the 1 Mbaud wake, until its 2 s HID timeout: the driver resends 91/11 every 5 ms, and after a
//   failure past the switch waits those 2 s before the next wake. 91/10 and 91/12 are sent once and given 1 s, as
//   L4T gives every step (J:2690, 2722, HZ); hekate takes the 94/12 whenever it comes (joycon.c 1357), and a
//   Joy-Con (R) may take longer than 250 ms to answer 91/12.
//   then cmd 92 carries Bluetooth HID reports both ways: the host sends output reports (01 subcommand, 10 rumble) or
//   the "request input" payload 1F (J:1525-1543), the Joy-Con answers with input reports (21, 30). The Joy-Con only
//   speaks when spoken to (91/12 sets the interval it expects, it does not make it push: hekate and L4T keep asking)
//   and drops HID mode after ~2 s without host packets (hekate 1257-1261), so the host asks at the Joy-Con's cadence,
//   every 15 ms (J:1809; hekate 1059-1074) or 30 ms while it is quiet (SwitchJoyConCadence.h): the next queued output
//   report, else 1F. That clock is the protocol, not a poll of hardware state: every reply is taken by interrupt. One
//   clock (`poll`) serves both rails, so two seated Joy-Cons cost one wakeup per tick, and it runs only while a
//   Joy-Con is running: an empty rail, or a rail paused with the display, has no timer at all. A Joy-Con's stick
//   calibration and dead zone are read from its SPI flash before its session runs (calibrate, SwitchJoyConStick.h), and
//   a 0x30 report reaches the HID device only when it says something new (SwitchJoyConForward.h). Rumble (output
//   report 0x10) goes out on the reply, as L4T's rumble worker does (J:1132-1138,
//   1871-1905): right after the Joy-Con answers a request, the newest rumble report is sent if there is one it has not
//   had, or again every 50 ms while it asks for a non-zero amplitude (L4T's keep-alive, J:1845-1851: a Joy-Con stops
//   a vibration that is not refreshed). The request keeps its cadence, so input never depends on whether the Joy-Con
//   answers a rumble report. Every output report the rail sends carries the rail's own packet counter (byte 1, mod 16:
//   deku bluetooth_hid_notes.md "global packet number"), so a repeated rumble report is a new packet. The rail stays
//   in input report mode 0x30, as the console and L4T keep it: JoyConHIDServicePlugin asks for 0x3F ("simple HID",
//   which pushes only on button changes, deku bluetooth_hid_subcommands_notes.md 66-81) as soon as gamecontrollerd
//   connects, and a rail Joy-Con in 0x3F stops answering 1F, so it would look detached 500 ms later. That subcommand
//   0x03 goes out with 0x30 instead; the plugin serves its HID mode from 0x30 reports (21H461
//   handleStandardFullPayload:withData:timestamp: checks _inputReportMode == 0x3F) and gets the Joy-Con's own ACK.
//
// Interrupts, no polling: the UART interrupt is an IOFilterInterruptEventSource whose filter, in primary
// interrupt context, drains the RX FIFO into a ring and refills the TX FIFO (3 Mbaud fills the 32-byte FIFO in ~110 us,
// so the drain cannot wait for a thread, and the line is never left masked for one); its action parses on the work
// loop. Hardware RTS flow control backs this up for long replies. Attach is the detect pin's GPIO
// interrupt. A missing reply is either the Joy-Con gone (the line is silent) or a lost interrupt (bytes sit in the FIFO
// with RX interrupts enabled, or the TX FIFO is empty with THRI enabled): that panics in one line with the UART
// registers and the GIC state. A rail waiting for an attach arms nothing but the detect pin's interrupt;
// tegra_gpio_check_delivery() (as SwitchButtons) looks at that pin when the display goes off, not on a timer. Until the
// session runs, every packet also takes one THR-empty interrupt, so the interrupt count proves the UART's line reaches
// the CPU even when nothing comes back; a running Joy-Con's replies prove it, and its requests, which fit the TX FIFO,
// take none. Its replies take two each by RX DMA (below).
// Silence is counted in requests the Joy-Con left unanswered, not in time since its last reply, so a clock that fired
// late cannot pass for a Joy-Con that stopped. A running Joy-Con is gone after 1.5 s of them, not L4T's 500 ms
// (J:1793): it runs on its own battery, so a rail contact that bounces while the console is handled leaves it at
// 3 Mbaud in HID mode with its input mode, IMU, lights and vibration as the plugin set them, and it answers the next
// request once the contact is back. Detached at 500 ms it would ignore the 1 Mbaud wake until its own 2 s HID timeout
// and come back as a new HID device, which games see as a disconnect. 1.5 s stays inside that timeout; the cost is
// a Joy-Con pulled off the rail noticed 1 s later. (A silence seen while the console was handled was a Joy-Con that
// answered no request for 500 ms and then ignored the 1 Mbaud wake for 2.6 s: in HID mode, not reset.)
// Giving up on an attach logs one line of rail state (describe); a running Joy-Con that falls silent logs one line
// with its BD_ADDR, whether its session opened with a pairing, its input reports, the requests it left unanswered (the
// time since its last reply dates the detach), and the line errors and breaks since its last reply; one
// that answers again after 500 ms or more logs one line too.
//
// RX by APB DMA while running (TegraAPBDMA.h): by FIFO trigger, a running Joy-Con's reply (~61 bytes of 0x30 report
// every 15 ms per rail) takes about eight interrupts, around 1000 a second on CPU0 for two rails. While a rail runs,
// the UART is in DMA mode (FCR DMA select, RX trigger 1, IER RLSI | RTOIE | EORD and no RDI), and channel 0 (UART-B) /
// 1 (UART-C) is the RX FIFO's only reader, as SwitchBluetooth's: one 32-bit read of RBR per byte, one word a byte, into
// a 16 KiB ring, continuously (tegra_apbdma_start_rx_words_ring). The reply takes two interrupts, EORD (or RX timeout)
// each time the line has been idle for 4 characters: a Joy-Con pauses 19-263 us after a reply's first 4 bytes (deku
// packet_parse/joycon2consolegrey.csv, every one of 330 replies) and the reply ends; the filter takes the words that
// moved (cache lines invalidated first) into the same ring and framing as PIO and wakes the action once, at the end.
// Nothing reads the FIFO by PIO while the channel runs: on the console the channel can fall a whole reply behind (the
// FIFO full, RTS holding the Joy-Con, EORD with nothing in the ring), and a PIO read of the 1-3 bytes below a 4-byte
// burst, as serial-tegra does it, then raced the channel for the FIFO: it took the 0 that is a reply's length high
// byte, so the cmd byte 0x92 became it and the parser saw a 37437-byte packet. A channel that is behind is now only
// late: a take that leaves bytes in the FIFO or ends inside a packet leaves the tail to the next EORD (the rest of a
// reply after its header's pause) or at the latest the next request's tick, and masks RX timeout until then, so a slow
// channel cannot storm the line; `dmaLate` counts those takes. TX stays PIO (a request fits the FIFO). Handshake and
// pairing keep PIO, so their give-up lines see every byte. The DMA's end-of-count at every wrap is counted: more wraps
// than the words taken account for means the ring was overrun, data lost, and the action panics with one line; words in
// the ring that no interrupt took and no tail was expected, after 50 ms, are a lost interrupt (checkDelivery).
// Non-fatal checks, each a channel that does not work: one that does not open, a first reply that does not start with
// the reply magic, or bytes in the FIFO across a whole request interval with the channel not moving. Each turns DMA off
// for that rail with one log line (DMA select off first and a read in flight let land, so PIO sees every byte in
// order); it keeps working by interrupt per FIFO trigger.
//
// Pairing (unless switch-no-bt): the address to pair to is the one SwitchBluetooth programs
// into the controller for bluetoothd: the unit's own (fuses, as switchroot's U-Boot derives it) or, when deploy staged
// hekate's "Dump Joy-Con BT" (switchroot/joycon_mac.ini), Horizon's, as switchroot's U-Boot takes it from joycon_mac.bin
// (SwitchBluetooth.cpp). SwitchBluetooth announces it once Data is mounted, before launchd (and again, unchanged, at
// bluetoothd's first connect), after the rails handshake the Joy-Cons seated at boot.
// The pairing opens the rail session, as Horizon's does at every attach (dekuNukem packet_parse/c2j.txt: right after
// the handshake, subcommands 0x02, 0x08 0x00 and the pairing request). Paired that way, a Joy-Con pages this address
// within 1.5 s of leaving the rail however long it ran there (boot 16: 14 and 17 minutes); paired 2-3 s into a session
// a console boot had handshaken, it never paged when pulled off minutes later (boots 16 and 18), and paired at its next
// attach it did. So a Joy-Con that ran unpaired until the address arrived has its session ended (91/11, after which the
// Joy-Con is back at 1 Mbaud) and is handshaken and paired afresh, its HID device gone for the half second that takes.
// Every session sends 0x02 and 0x08 0x00 (shipment low-power state off: deku bluetooth_hid_subcommands_notes.md) and
// pairs with subcommand 0x01 (SwitchJoyConRailPacket.h): 1 with the host record -> Joy-Con BD_ADDR; 2 -> LTK, each byte
// XOR 0xAA; 3 -> saved, which makes it the slot in use (Horizon pairs it back the next time it is attached there). It
// does so whatever the Joy-Con already holds: seen on the console, a Joy-Con whose slot in use already named this
// address, left as it was, never paged after a detach. Debug builds first read the Joy-Con's pairing log (SPI 0x2000,
// 0x26-byte slots: magic, size 0x22, checksum, host BD_ADDR display order, LTK, host capability; hekate 1084-1245, deku
// spi_flash_notes.md 44-76: earlier pairings magic 0x00, the one in use 0x95, erased flash its end;
// SwitchJoyConPairingSlot.h) to log what the slot in use held before the pairing. The result is published as a
// SwitchJoyConPairing registry entry (Address, LinkKey in HCI order, Name, ClassOfDevice, ProductID) for joyconpaird
// (helper/), which hands it to bluetoothd: pulled off the rail, the Joy-Con pages this address and reconnects over
// Bluetooth. The console never reaches it the other way: a Joy-Con that is not in sync mode does not page-scan, so a
// connect from Settings, Bluetooth (bluetoothd paging it) times out (Connection Complete status 0x04).
//
// NFC (off: the user dropped it on 11-04, as joyconnfc's 30 s MCU scan ran on every Joy-Con (R) attach; nothing turns it
// on): joyconnfc (helper/) drives the Joy-Con (R)'s NFC/IR MCU through the HID device with the
// Bluetooth protocol (output report 0x11 MCU requests, subcommands 0x03/0x21/0x22; jc_toolkit nfc_tag_info, yuzu
// joycon_protocol/nfc.cpp), and the rail carries those reports like any other: 0x11 is a 49-byte output report and the
// MCU's 0x31 input report is 362 bytes, a 374-byte rail packet. The receive buffer has room beyond that, so a Joy-Con
// that appends anything is not silently dropped; an oversize packet is logged once per attach. The one rail-specific
// step: JoyConHIDServicePlugin parses only 0x21, 0x30 and 0x3F, so while the MCU streams 0x31 each one is also
// delivered as the 0x30 report its first 49 bytes are, and GameController keeps its input.
//
// SC7 (switch_joycon_suspend/resume): as L4T joycon_serdev_suspend, a running Joy-Con is told to sleep (subcommand
// 0x06 HCI state 0, which it does not answer), charging stops, and everything is quiesced; on resume the UARTs are set
// up again and both rails re-enter detection, which finds a seated Joy-Con by level and handshakes it afresh.
//
// Display off (switch_joycon_display_power, from SwitchBacklight): the rails pause. Nothing is sent, so a Joy-Con
// leaves HID mode after its 2 s timeout and stays seated and charging (the charge enable is left as it is); timers,
// the UART's and the detect pin's interrupts are off and the HID devices go (GameController sees them disconnect).
// Display on: both rails re-enter detection, as after SC7, and a seated Joy-Con is handshaken afresh (about 0.2 s).
//
// Sessions (SwitchJoyConSession.h): gamecontrollerd takes one JoyConHIDServicePlugin session per HID device, and a
// rail device outlives backboardd. When the backboardd session serving a running Joy-Con's device ends (its client
// closed the device: backboardd exited; or a different backboardd opened it), the device reports it from inside
// IOService::open/close, which may not wait for this work loop: the callback marks the side and fires `sessions`, a
// software interrupt source (no provider: interruptOccurred only counts and wakes the loop), and the loop publishes a
// new device in place of the old (destroy, create: a new registry ID, which gamecontrollerd claims afresh). The rail
// itself does not stop: the Joy-Con keeps its HID mode and the 15 ms clock its phase; only the output queue and rumble
// the old session left are dropped. One os_log line per republish. A backboardd that ends a second session on the
// same Joy-Con within 10 s (switch_joycon_republish_allowed) logs one line and turns republishing off for the boot.
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/clock.h>
#include <libkern/c++/OSData.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraAPBDMA/TegraAPBDMA.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "SwitchJoyCon.h"
#include "SwitchJoyConHID.h"
#include "SwitchJoyConCadence.h"
#include "SwitchJoyConForward.h"
#include "SwitchJoyConPairingSlot.h"
#include "SwitchJoyConProbe.h"
#include "SwitchJoyConRailPacket.h"
#include "SwitchJoyConRxRing.h"
#include "SwitchJoyConSession.h"
#include "SwitchJoyConStick.h"
#include "SwitchJoyConUse.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchSysctl/SwitchSysctl.h"
#include "../../kits/SwitchTime/SwitchTime.h"

namespace {
constexpr vm_offset_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kApbMiscPA = 0x70000000, kPinmuxPA = 0x70003000,
                      kUartPA = 0x70006000, kPmcPA = 0x7000E000;

// Tegra UART (16550 layout, 4-byte stride; Linux serial-tegra.c, hekate soc/uart.h).
enum : uint32_t {
    RBR_THR = 0x00,
    IER = 0x04,
    IIR_FCR = 0x08,
    LCR = 0x0c,
    MCR = 0x10,
    LSR = 0x14,
    MSR = 0x18,
    SPR = 0x1c,
    IRDA_CSR = 0x20
};

enum : uint32_t {
    IER_RDI = 0x01,
    IER_THRI = 0x02,
    IER_RLSI = 0x04,
    IER_RTOIE = 0x10,
    IER_EORD = 0x20,  // end of receive data: the line idle 4 characters (DMA mode; serial-tegra TEGRA_UART_IER_EORD)
    IER_RX = IER_RDI | IER_RLSI | IER_RTOIE,
    IER_RX_DMA = IER_RLSI | IER_RTOIE | IER_EORD,
};

enum : uint32_t {
    FCR_FIFO = 0x01,
    FCR_RX_CLR = 0x02,
    FCR_TX_CLR = 0x04,
    FCR_DMA = 0x08,  // UART_FCR_DMA_SELECT: RX/TX DMA requests
    FCR_TX_TRIG_16 = 0x00,
    FCR_RX_TRIG_1 = 0x00,  // DMA mode: a request per byte, one APB read each
    FCR_RX_TRIG_8 = 0x80
};

// One word a byte: 4096 bytes, about a second of one rail's replies between the channel's end-of-count interrupts.
constexpr uint32_t kDmaRingBytes = 16384;
// Leaving DMA mode: a read the channel had started lands (one byte, < 1 us; SwitchBluetooth kRxDmaSettleUs).
constexpr uint32_t kDmaSettleUs = 4;

enum : uint32_t {
    LCR_8N1 = 0x03,
    LCR_2STOP = 0x04,
    LCR_DLAB = 0x80,
    MCR_RTS_EN = 0x40
};

enum : uint32_t {
    LSR_DR = 0x01,
    LSR_ERRORS = 0x1e,
    LSR_BI = 0x10,
    LSR_THRE = 0x20,
    LSR_TEMT = 0x40,
    LSR_TX_FULL = 0x100
};

constexpr uint32_t kTxBurst = 16;  // bytes an empty TX FIFO takes without a status read (fillTx)

enum : uint32_t {
    CSR_INVERT_TXD = 0x02,
    CSR_INVERT_RTS = 0x08
};

// CAR UART source: PLLP (bits 31:29 = 0), fractional divider enable (bit 24), 7.1 divisor.
constexpr uint32_t kUartDivEnable = 1u << 24, kDiv1M = 49 /* 25.5 */, kDiv3M = 15 /* 8.5 */;

// GPIO registers (bank * 0x100 + port * 4; masked at +0x80, bits 15:8 select), pinmux fields (hekate soc/pinmux.h).
enum : uint32_t {
    GPIO_CNF = 0x00,
    GPIO_OE = 0x10,
    GPIO_OUT = 0x20,
    GPIO_IN = 0x30,
    GPIO_MSK_CNF = 0x80,
    GPIO_MSK_OE = 0x90
};

enum : uint32_t {
    PIN_PULL_DOWN = 1u << 2,
    PIN_TRISTATE = 1u << 4,
    PIN_INPUT = 1u << 6,
    PIN_DRIVE_4X = 3u << 13,
    PIN_KEEP_FUNCTION = 3u
};

// CAR state registers (RST_DEVICES_L/H, CLK_OUT_ENB_L/H), for the give-up line.
enum : uint32_t {
    RST_DEVICES_L = 0x004,
    RST_DEVICES_H = 0x008,
    CLK_OUT_ENB_L = 0x010,
    CLK_OUT_ENB_H = 0x014
};

// PMC (bytes from 0x7000E000; registers at +0x400: hekate soc/pmc_t210.h) and APB_MISC (hekate soc/t210.h).
enum : uint32_t {
    PMC_NO_IOPOWER = 0x444,
    PMC_PWR_DET_VAL = 0x4e4,
    PMC_IO_DPD_STATUS = 0x5bc
};

constexpr unsigned kPadUartDpdBit = 14;  // Linux tegra210 io_pad uart .dpd
constexpr uint32_t kPadUartPower = 1u << 2, kPadUartDpd = 1u << kPadUartDpdBit;
constexpr uint32_t PP_PINMUX_GLOBAL = 0x40, kClampTristatedInputs = 1u << 0;
constexpr unsigned kPin5V = TEGRA_GPIO(A, 5);  // the 5 V regulator feeding both rails (SwitchThermal drives it)

struct SideConfig {
    char uart, letter;  // 'B'/'C', 'R'/'L'
    bool left;
    uint32_t uartOffset, intid;
    uint32_t rstSet, rstClr, enbSet, enbClr, rstState, enbState, clockBit, clockSource;
    unsigned detectPin, txPin, chargePin;  // RX, RTS and CTS are txPin + 1, + 2, + 3 (PG0-PG3, PD1-PD4)
    uint32_t muxTx, muxRx, muxRts, muxCts, rtsPull, muxDetect, muxCharge, chargeMux;
};

// Pinmux values: tegra210b01-fric-pinmux.dtsi (uart2_* 686-715, uart3_* 718-747, pe6 870, ph6 1176) and hekate's
// charge-enable writes (joycon.c 460-470: SPDIF_IN = PCC3 pull-down function 1, GPIO_PK3 drive 4x pull-down function 2).
const SideConfig kSides[2] = {
    {'B',
     'R',
     false,
     0x040,
     69,
     0x300,
     0x304,
     0x320,
     0x324,
     RST_DEVICES_L,
     CLK_OUT_ENB_L,
     1u << 7,
     0x17c,
     TEGRA_GPIO(H, 6),
     TEGRA_GPIO(G, 0),
     TEGRA_GPIO(K, 3),
     0xf4,
     0xf8,
     0xfc,
     0x100,
     PIN_PULL_DOWN,
     0x250,
     0x260,
     PIN_DRIVE_4X | PIN_PULL_DOWN | 2},
    {'C',
     'L',
     true,
     0x200,
     78,
     0x308,
     0x30c,
     0x328,
     0x32c,
     RST_DEVICES_H,
     CLK_OUT_ENB_H,
     1u << 23,
     0x1a0,
     TEGRA_GPIO(E, 6),
     TEGRA_GPIO(D, 1),
     TEGRA_GPIO(CC, 3),
     0x104,
     0x108,
     0x10c,
     0x110,
     0,
     0x248,
     0x1a4,
     PIN_PULL_DOWN | 1},
};

enum : uint8_t {
    kCmdExtSend = 0x91,
    kCmdExtRet = 0x92,
    kCmdInitRet = 0x94,
    kCmdHandshake = 0xa5
};

enum : uint8_t {
    kInitMac = 0x01,
    kInitHidConnect = 0x10,
    kInitHidDisconnect = 0x11,
    kInitHidRate = 0x12,
    kInitBaud = 0x20
};

enum : uint8_t {
    kReportSubcommand = 0x01,
    kReportRumble = 0x10,
    kReportReply = 0x21,
    kReportFull = 0x30,
    kReportNfcIr = 0x31,
    kRequestInput = 0x1f
};

enum : uint8_t {
    kSubPairing = 0x01,
    kSubDeviceInfo = 0x02,
    kSubReportMode = 0x03,
    kSubHciState = 0x06,
    kSubShipment = 0x08,
    kSubSpiRead = 0x10,
    kSubImu = 0x40
};

enum : uint8_t {
    kModeSimpleHid = 0x3f
};

// Largest input report: 0x31 (NFC/IR MCU), 362 bytes with the ID, 374 as a rail packet; room is left beyond it.
constexpr size_t kHeader = SWITCH_JOYCON_RAIL_HEADER, kMaxOutput = 64, kMaxTx = kHeader + kMaxOutput,
                 kMaxRx = kHeader + 0x200;
static_assert(SWITCH_JOYCON_SUBCOMMAND_REPORT <= kMaxOutput &&
              SWITCH_JOYCON_PAIRING_REQUEST <= SWITCH_JOYCON_SUBCOMMAND_DATA);
constexpr size_t kMinPacket = 7;      // magic, size, cmd, subcmd
constexpr size_t kStandardFull = 49;  // report 0x30 with its ID; also the head of every 0x31
constexpr uint8_t kReplyMagic[3] = {0x19, 0x81, 0x03};

// Timing (J: 1709 retries, 1748 settle, 1793 silence, 1809 interval; hekate 1257 handshake reply).
constexpr uint32_t kSettleMS = 64, kHandshakeMS = 100, kCommandMS = 250, kRetryMS = 100, kAttempts = 30;
constexpr uint32_t kSubcommandMS = 1000;
// Unanswered requests (see the header), in the intervals they were sent at: a running Joy-Con is gone after kSilentMS;
// one that answers again after kQuietMS or more is logged.
constexpr uint32_t kSilentMS = 1500, kQuietMS = 500;
// Session start (calibrate): the stick's SPI reads (SwitchJoyConStick.h), then this step, the input report mode, then
// the IMU.
constexpr uint8_t kCalibrateMode = SWITCH_JOYCON_STICK_READS;
// A non-zero rumble is sent again this often (L4T JC_RUMBLE_PERIOD_MS, J:470).
constexpr uint32_t kRumbleKeepAliveMS = 50;
// After the 3 Mbaud switch: 91/11 every 5 ms for up to 200 ms (hekate 1325-1341: ten sends 5 ms apart; L4T's retry
// comes after 1 s, well inside the Joy-Con's 2 s), 1 s for the other steps (L4T); the Joy-Con's own HID timeout,
// after which it is back at 1 Mbaud (hekate 1254-1259); handshakes that may fail past the switch before giving up.
constexpr uint32_t kResendMS = 5, kDisconnectSends = 40, kFastCommandMS = 1000, kJoyConTimeoutMS = 2000,
                   kFastAttempts = 3;
// Pairing slots (deku spi_flash_notes.md 44-76; hekate 1158-1166 reads 0x1A of each 0x26-byte slot): the log fills the
// 4 KiB section (deku), so the slot in use can be any of them.
constexpr uint32_t kSlotBase = 0x2000, kSlotStride = 0x26, kSlots = 0x1000 / kSlotStride;
// The pairing log is read before every pairing, for its log line (see the header), in debug builds only.
#if DEVELOPMENT || DEBUG
constexpr bool kReportSlot = true;
#else
constexpr bool kReportSlot = false;
#endif
constexpr uint32_t kJoyConClass = 0x000508;  // Peripheral / Gamepad, as switchroot's BlueZ record

bool bluetoothDisabled()
{
    unsigned off = 0;
    return (PE_parse_boot_argn("switch-no-bt", &off, sizeof(off)) != 0) && (off != 0u);
}

unsigned flag(uint32_t value, uint32_t mask)
{
    return (value & mask) ? 1 : 0;
}
}  // namespace

// One pairing result for joyconpaird (helper/joyconpaird.c); see SwitchJoyConRail::publishPairing.
// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchJoyConPairing : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConPairing)
};

OSDefineMetaClassAndStructors(SwitchJoyConPairing, IOService)

    // NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
    class SwitchJoyConRail : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchJoyConRail)

        enum Phase : uint8_t {
        kSettling,
        kWaiting,
        kBackoff,
        kHandshake,
        kPairing,
        kCalibrating,  // the Joy-Con's stick calibration read, its input mode and IMU set (calibrate)
        kRunning,
        kEnding,  // a running session ended with 91/11 for a pairing to open the next one (bluetoothAddress)
        kSuspended
    };

    enum Step : uint8_t {
        kWake,
        kInfo,
        kBaud,
        kDisconnect,
        kConnect,
        kRate
    };

    enum PairStep : uint8_t {
        kReadSlot,
        kReadSlotTail,
        kDeviceInfo,
        kShipment,
        kSendHost,
        kGetKey,
        kSave
    };

    struct Side {
        SwitchJoyConRail *owner;
        const SideConfig *cfg;
        volatile uint8_t *uart;
        IOFilterInterruptEventSource *irq;
        IOInterruptEventSource *detect;
        IOTimerEventSource *timer;
        IOSimpleLock *lock;  // filter <-> work loop: ring head, TX buffer, IER shadow
        IOLock *outLock;     // setReport threads <-> work loop: output queue
        // Primary interrupt context writes these (under `lock`).
        uint8_t ring[1024];
        volatile uint32_t head, tail;
        volatile uint32_t interrupts, lineErrors, fastErrors, overflows;  // fastErrors: line errors at 3 Mbaud
        volatile uint32_t breaks;                                         // line errors that were a break (BI)
        volatile bool stuck;
        bool fast;  // UART at 3 Mbaud (configureUart)
        // The rail framing of the bytes the filter puts in the ring, as uartAction parses them (frameByte), so that a
        // running Joy-Con's reply wakes the work loop once, at its end (filter); uartAction puts it back in step with its
        // own whenever it has emptied the ring. Under `lock`.
        uint32_t frameLength, frameTotal;
        uint8_t frameSizeLow;
        bool packetDone;  // a packet ended in the ring since the last wakeup
        uint8_t tx[kMaxTx];
        uint32_t txLength, txPosition;
        uint32_t ier;
        // RX by DMA while running (header "RX by APB DMA"), under `lock`: the channel and its ring of words (null: PIO
        // only), DMA mode on, the word taken up to, words taken since the start, the channel's interrupt count at the
        // start, the first reply checked; a tail left to the next tick (RX timeout masked until then); the FIFO held
        // bytes at the last tick, with the channel at word dmaTickAt (the not-moving check); a reason to turn it off
        // (the action logs it), an overrun (the action panics); and counts: takes, takes that found the channel behind,
        // tails the tick took.
        tegra_apbdma_channel *dma;
        tegra_apbdma_buffer dmaRing;
        bool dmaOn, dmaOff, dmaChecked, dmaOverrun, dmaTail, dmaWaited;
        const char *dmaFailed;
        uint32_t dmaTaken, dmaLapsAtStart, dmaTakes, dmaLate, dmaTails, dmaTickAt;
        uint64_t dmaWords;
        // Work loop.
        Phase phase;
        uint8_t step, attempts, fastFailures, sends, slot, counter;  // sends: of the current handshake step
        uint32_t errorsSeen, epoch;  // epoch: bumped when the UART is reprogrammed and unread bytes are dropped
        uint64_t lastReply;
        uint32_t reports;     // input reports since the handshake completed
        uint32_t unanswered;  // requests sent since the last reply (header: silence)
        uint32_t silentMs;    // the intervals they were sent at, summed
        uint32_t errorsAtReply, breaksAtReply;
        // Since boot, for kern.switch_joycon (debug kernels): input reports, packets dropped for a length no Joy-Con
        // sends, silences that detached it, silences of kQuietMS or more it answered after, devices republished; requests
        // sent, packets that answered none, input reports handed to the HID device and held back (SwitchJoyConForward.h).
        uint32_t replies, dropped, silences, recoveries, republished;
        uint32_t requests, unsolicited, forwarded, suppressed;
        uint8_t packet[kMaxRx];
        size_t packetLength;
        bool oversizeLogged;
        // Since the attach, for the give-up line: the furthest handshake step sent, the first bytes taken from the ring,
        // their count, and the interrupt and line-error counters as they were at the attach; packets sent, bytes taken
        // and line errors at 3 Mbaud; bytes that came after an attempt had failed (a late reply); and the first bytes
        // taken since the current step was first sent (never a reply it accepted: that starts the next step).
        uint8_t furthest;
        uint8_t heard[12], stepHeard[16];
        uint32_t heardCount, interruptsAtAttach, errorsAtAttach, stepHeardCount;
        uint32_t fastSent, fastHeardCount, fastErrorsAtAttach, lateCount;
        uint32_t errorsAtRun, overflowsAtRun;  // for the stopped-answering line
        uint8_t mac[6];
        uint8_t key[16];
        // The pairing log as read at this attach (SwitchJoyConPairingSlot.h): the slot in use (index `slot`), whole
        // (its head kept while its tail is read); one was found and checks out, or one was found that does not.
        uint8_t slotBytes[SWITCH_JOYCON_SLOT_BYTES];
        bool slotInUse, slotCorrupt;
        bool charging, chargeKnown;
        bool pairPending;  // the session's pairing is still to come: SwitchBluetooth's address was not known (header)
        bool paired;       // this session opened with a pairing
        IOService *hid;
        SwitchJoyConPairing *record;
        // The device's backboardd session ended (sessionEnded, any thread; under outLock): publish it again.
        bool republish;
        int32_t servedPid, openerPid;
        switch_joycon_republish_guard guard;  // work loop
        // Output queue (outLock): subcommand reports in order, rumble coalesced to the latest, `rumbleFresh` until sent.
        uint8_t queue[8][kMaxOutput];
        uint8_t queueLength[8];
        uint8_t queueHead, queueTail;
        uint8_t rumble[kMaxOutput];
        uint8_t rumbleLength;
        bool rumbleFresh;
        switch_joycon_forward forward;
        switch_joycon_use use;
        // The stick as this session's Joy-Con describes it (calibrate), kept for the boot by BD_ADDR; the cadence and
        // the last report it was judged by (SwitchJoyConCadence.h).
        switch_joycon_stick stick;
        uint8_t stickMac[6];
        bool stickKnown;
        switch_joycon_cadence cadence;
        uint8_t state[12];
        bool stateKnown;
        // Work loop: the Joy-Con's IMU is to be turned off at the next tick (its HID client wanted motion and went), and
        // the subcommand of the rail's own whose reply is not the HID device's.
        bool imuOff;
        uint8_t ownReply;
#if DEVELOPMENT || DEBUG
        switch_joycon_probe probe;  // HW Test's (SwitchJoyConProbe.h): it holds the requests while it runs
#endif
        // Work loop: the last packet sent was a rumble report (the next reply does not send another), and when the last
        // rumble report went out.
        bool rumbleSent;
        uint64_t rumbleAt;
    };

    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *poll = nullptr;          // the clock of every running Joy-Con; armed only while one runs
    uint32_t pollInterval = 0;                   // what it was last armed at (SwitchJoyConCadence.h)
    IOInterruptEventSource *sessions = nullptr;  // software interrupt: a device's backboardd session ended
    volatile uint8_t *car = nullptr, *gpio = nullptr, *apbMisc = nullptr, *pinmux = nullptr, *uarts = nullptr,
                     *pmc = nullptr;
    Side sides[2] = {};
    bool pairing = false, nfc = false, suspended = false, displayOff = false, polling = false;
    bool republishOff = false;  // the loop check tripped (see the header)
    bool hostKnown = false;     // `host` is the address SwitchBluetooth programmed
    uint8_t host[6] = {};
    // The rail pair's game controller devices exist (SwitchJoyConUse.h); the last gamepad use; publishes and idle-outs.
    bool gamepads = false;
    uint64_t lastUse = 0;
    uint32_t gamepadsPublished = 0, gamepadsIdled = 0;

    static uint32_t rd(Side &s, uint32_t reg)
    {
        return switch_mmio_read32(s.uart, reg);
    }

    static void wr(Side &s, uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(s.uart, reg, value);
    }

    void carWrite(uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(car, reg, value);
    }

    uint32_t carRead(uint32_t reg)
    {
        return switch_mmio_read32(car, reg);
    }

    void pinWrite(uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(pinmux, reg, value);
    }

    uint32_t pinRead(uint32_t reg)
    {
        return switch_mmio_read32(pinmux, reg);
    }

    void pmcWrite(uint32_t reg, uint32_t value)
    {
        switch_mmio_write32(pmc, reg, value);
    }

    uint32_t pmcRead(uint32_t reg)
    {
        return switch_mmio_read32(pmc, reg);
    }

    void gpioMask(unsigned pin, uint32_t reg, bool on);
    unsigned gpioBit(unsigned pin, uint32_t reg);

    static bool uartFilter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void uartAction(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void detectAction(OSObject *owner, IOInterruptEventSource *sender, int count);
    static void timerAction(OSObject *owner, IOTimerEventSource *sender);
    static void pollAction(OSObject *owner, IOTimerEventSource *sender);
    static void sessionAction(OSObject *owner, IOInterruptEventSource *sender, int count);
    static IOReturn output(void *context, const uint8_t *report, size_t length);
    static void sessionEnded(void *context, int32_t servedPid, int32_t openerPid);
    Side *sideFor(const OSObject *source);
    static bool filter(Side &s);
    static void fillTx(Side &s);
    static bool drainRx(Side &s);
    static void countLineStatus(Side &s, uint32_t lsr);
    static void frameByte(Side &s, uint8_t byte);
    static bool takeDma(Side &s);
    static void takeTail(Side &s);
    static void startDmaLocked(Side &s);
    static void stopDmaLocked(Side &s);
    static void dmaFailedLocked(Side &s, const char *why);

    // Work loop.
    void setUp(Side &s, const SideConfig *cfg);
    void preparePads(const char *when);
    void initHardware(Side &s);
    static void quiesce(Side &s);
    void wake(Side &s);
    void configureUart(Side &s, bool fast);
    static void send(Side &s, const uint8_t *bytes, size_t length);
    static bool txIdle(Side &s);
    static void sendCommand(Side &s, uint8_t subcmd, const uint8_t *payload = nullptr, uint16_t length = 0);
    static void sendReport(Side &s, const uint8_t *report, size_t length);
    static void sendOutput(Side &s, uint8_t *report, size_t length);
    static void sendSubcommand(Side &s, uint8_t id, const uint8_t *data, size_t length);
    void enterDetect(Side &s);
    void attachJoyCon(Side &s);
    void startAttempt(Side &s);
    void failAttempt(Side &s, const char *why);
    static void sendStep(Side &s);
    void startPairing(Side &s);
    static void endSession(Side &s);
    void sendPairStep(Side &s);
    void finishPairing(Side &s, bool paired, const char *how);
    void calibrate(Side &s);
    static void sendCalibrationStep(Side &s);
    void calibrationReply(Side &s, const uint8_t *report, size_t length);
    void run(Side &s);
    static IOService *createHid(Side &s);
    static void dropGamepad(Side &s);
    static void dropHid(Side &s);
    bool gamepadsLive(uint64_t now);
    void noteUse();
    void idleGamepads();
    void republish(Side &s);
    void armPoll();
    void tick(Side &s);
    static bool ownReplyTaken(Side &s, const uint8_t *report, size_t length);
    static bool probing(const Side &s);
#if DEVELOPMENT || DEBUG
    static bool probeTick(Side &s);
    static bool probePacket(Side &s, const uint8_t *packet, size_t length, uint64_t now);
#endif
    static void rumbleAfterReply(Side &s);
    void detach(Side &s);
    static void pause(Side &s);
    void received(Side &s, const uint8_t *packet, size_t length);
    void handshakeReply(Side &s, uint8_t cmd, uint8_t echo, const uint8_t *payload, size_t length);
    void pairingReply(Side &s, const uint8_t *report, size_t length);
    void pairingSlotReply(Side &s, const uint8_t *data, bool ack, size_t length);
    void runningReport(Side &s, const uint8_t *report, size_t length);
    static void setCharging(Side &s, bool on);
    void publishPairing(Side &s, const char *source);
    static void checkDelivery(Side &s, const char *why);
    void describe(Side &s, char *buffer, size_t size);

public:
    bool start(IOService *provider) override;
    void suspend();
    void resume();
    void displayPower(bool on);
    void bluetoothAddress(const uint8_t address[6]);
    // kern.switch_joycon (debug kernels): each rail's state and counters since boot, for hw_test_app's input.rail.
    void statsLine(char *buffer, size_t size) const;
#if DEVELOPMENT || DEBUG
    // debug.switch_joycon_probe (SwitchJoyConProbe.h): start it on every running Joy-Con (0, EBUSY or ENODEV), and its
    // state: "idle", "running" with each rail's stage, or "done" with each rail's results.
    int probeStart();
    void probeState(char *buffer, size_t size);
#endif
};

OSDefineMetaClassAndStructors(SwitchJoyConRail, IOService)

    void SwitchJoyConRail::gpioMask(unsigned pin, uint32_t reg, bool on)
{
    const uint32_t bit = 1u << (pin & 7);
    const uint32_t at = (pin >> 5) * 0x100 + ((pin >> 3) & 3) * 4 + reg;
    switch_mmio_write32(gpio, at, (bit << 8) | (on ? bit : 0));
    (void)switch_mmio_read32(gpio, at);
}

// One pin's bit of CNF, OE, OUT or IN (IN follows the pad whenever its input receiver is on, GPIO or UART mode).
unsigned SwitchJoyConRail::gpioBit(unsigned pin, uint32_t reg)
{
    const uint32_t at = (pin >> 5) * 0x100 + ((pin >> 3) & 3) * 4 + reg;
    return (switch_mmio_read32(gpio, at) >> (pin & 7)) & 1;
}

SwitchJoyConRail::Side *SwitchJoyConRail::sideFor(const OSObject *source)
{
    for (Side &s : sides) {
        if (source == s.irq || source == s.detect || source == s.timer)
            return &s;
    }
    return nullptr;
}

// The action is scheduled with signalInterrupt() and the filter returns false: returning true on a level line makes
// IOFilterInterruptEventSource::disableInterruptOccurred mask it until the work loop has run the action, and at
// 3 Mbaud the next 8 bytes arrive in ~27 us. The filter has already cleared the UART's condition, which is what
// signalInterrupt() requires (IOFilterInterruptEventSource.h).
bool SwitchJoyConRail::uartFilter(OSObject *owner, IOFilterInterruptEventSource *sender)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (s && SwitchJoyConRail::filter(*s))
        sender->signalInterrupt();
    return false;
}

// Every pending UART interrupt: RX data, RX timeout and line status drain the FIFO into the ring, THR empty refills the
// TX FIFO (Linux tegra_uart_isr). The action runs only when bytes arrived. An identification register that never
// clears would hold the level line forever: stop the UART's interrupts and let the action panic with the registers.
// A running Joy-Con's reply (0x30 input reports, ~60 bytes) wakes the action once, not at every 8-byte RX FIFO trigger
// (about 7 times, ~900 wakeups a second with both rails): while running, the action only acts on whole packets, so
// bytes in the middle of one stay in the ring until the interrupt that brings its last byte (frameByte), the line
// going idle (RX timeout), a line error or an overflow. That is the earliest the action could use them: no input
// report or rumble goes out later than before. Every other phase (handshake, pairing) records every byte for its
// give-up line and keeps a wakeup per interrupt.
//
// DMA mode (header "RX by APB DMA"): an RX condition (EORD, RX timeout, line status) masks the RX interrupts as
// serial-tegra's tegra_uart_isr does (RDI written on, then every RX source off: that clears EORD); once IIR is clear
// the words the channel moved are taken (takeDma) and the RX sources come back, RX timeout only once no tail is left
// to the next tick. The action wakes for a packet's end, not for a take that ends inside one.
bool SwitchJoyConRail::filter(Side &s)
{
    bool got = false;
    bool idle = false;
    bool dmaRx = false;
    IOSimpleLockLock(s.lock);
    s.interrupts = s.interrupts + 1;
    const uint32_t errorsBefore = s.lineErrors;
    const uint32_t overflowsBefore = s.overflows;
    unsigned rounds = 0;
    for (;; ++rounds) {
        const uint32_t iir = rd(s, IIR_FCR);
        if (iir & 1)
            break;
        if (rounds == 64) {
            s.ier = 0;
            wr(s, IER, 0);
            s.stuck = true;
            got = true;
            break;
        }
        switch ((iir >> 1) & 7) {
        case 0:
            (void)rd(s, MSR);
            break;
        case 1:
            fillTx(s);
            break;
        default:
            if (s.dmaOn) {
                if (!dmaRx) {
                    wr(s, IER, s.ier | IER_RDI);
                    wr(s, IER, s.ier & ~(IER_RX | IER_EORD));
                }
                dmaRx = true;
                if (((iir >> 1) & 7) == 3)  // line status: the read clears it
                    countLineStatus(s, rd(s, LSR));
                break;
            }
            idle |= ((iir >> 1) & 7) == 6;  // RX timeout
            got |= drainRx(s);
            break;
        }
    }
    if (dmaRx && !s.stuck) {
        got |= takeDma(s);
        // EORD and RX timeout come with the line idle: the end of a reply, unless the channel is behind.
        idle = !s.dmaTail;
        if (s.dmaOn)
            wr(s, IER, s.dmaTail ? s.ier & ~IER_RTOIE : s.ier);  // a failed check left PIO's sources (dmaFailedLocked)
    }
    if (got && !s.stuck && s.phase == kRunning && !s.packetDone && !idle && s.lineErrors == errorsBefore &&
        s.overflows == overflowsBefore)
        got = false;  // the middle of a reply: its end wakes the action
    if (got)
        s.packetDone = false;
    IOSimpleLockUnlock(s.lock);
    return got;
}

// `lock` held. One more byte in the ring, framed as uartAction does (magic 19 81 03, a 16-bit size, size + 5 bytes):
// packetDone when it ends a packet.
void SwitchJoyConRail::frameByte(Side &s, uint8_t byte)
{
    if (s.frameLength < 3 && byte != kReplyMagic[s.frameLength]) {
        s.frameLength = byte == kReplyMagic[0] ? 1 : 0;
        return;
    }
    if (s.frameLength == 3)
        s.frameSizeLow = byte;
    if (s.frameLength == 4)
        s.frameTotal = 5 + (s.frameSizeLow | static_cast<uint32_t>(byte) << 8);
    if (++s.frameLength < 5)
        return;
    if (s.frameTotal < kMinPacket || s.frameTotal > kMaxRx) {
        s.frameLength = 0;
        return;
    }
    if (s.frameLength < s.frameTotal)
        return;
    s.frameLength = 0;
    s.packetDone = true;
}

void SwitchJoyConRail::fillTx(Side &s)
{
    // An empty TX FIFO (THRE, the FIFO is always on) takes a 16-byte burst (serial-tegra's PIO burst,
    // TEGRA_UART_MIN_DMA; the FIFO holds 32) with one status read, not one per byte, which would be 14 LSR reads per
    // 13-byte request, 133 times a second with both rails running. The rest goes byte by byte while LSR shows room.
    if (s.txPosition < s.txLength && (rd(s, LSR) & LSR_THRE)) {
        const uint32_t end = s.txLength - s.txPosition > kTxBurst ? s.txPosition + kTxBurst : s.txLength;
        while (s.txPosition < end)
            wr(s, RBR_THR, s.tx[s.txPosition++]);
    }
    while (s.txPosition < s.txLength && !(rd(s, LSR) & LSR_TX_FULL))
        wr(s, RBR_THR, s.tx[s.txPosition++]);
    if (s.txPosition == s.txLength && (s.ier & IER_THRI)) {
        s.ier &= ~IER_THRI;
        wr(s, IER, s.ier);
    }
}

// `lock` held: a line status read's errors, counted once.
void SwitchJoyConRail::countLineStatus(Side &s, uint32_t lsr)
{
    if (!(lsr & LSR_ERRORS))
        return;
    s.lineErrors = s.lineErrors + 1;
    if (s.fast)
        s.fastErrors = s.fastErrors + 1;
    if (lsr & LSR_BI)
        s.breaks = s.breaks + 1;
}

bool SwitchJoyConRail::drainRx(Side &s)
{
    bool got = false;
    for (;;) {
        const uint32_t lsr = rd(s, LSR);
        countLineStatus(s, lsr);
        if (!(lsr & LSR_DR))
            break;
        const uint8_t byte = static_cast<uint8_t>(rd(s, RBR_THR));
        const uint32_t next = (s.head + 1) % sizeof(s.ring);
        if (next == s.tail) {
            s.overflows = s.overflows + 1;
            continue;
        }
        s.ring[s.head] = byte;
        s.head = next;
        frameByte(s, byte);
        got = true;
    }
    return got;
}

// `lock` held, DMA mode (filter, or tick for a tail): the words the channel moved since the last take (their cache
// lines invalidated first: the engine does not snoop), one byte each, into `ring` with the same framing. More
// end-of-counts (wraps) than the words taken account for is an overrun (the action panics). Bytes still in the FIFO, or
// a take that ends inside a packet, leave a tail: the channel is behind, and the next tick takes the rest.
bool SwitchJoyConRail::takeDma(Side &s)
{
    const uint32_t words = s.dmaRing.size / 4;
    const switch_joycon_rx_take take =
        switch_joycon_rx_ring_take(words, s.dmaTaken, tegra_apbdma_moved(s.dma, nullptr),
                                   tegra_apbdma_interrupts(s.dma) - s.dmaLapsAtStart, s.dmaWords);
    if (take.overrun) {
        s.dmaOverrun = true;
        return true;
    }
    const auto *ring = reinterpret_cast<const uint32_t *>(s.dmaRing.va);
    if (take.count) {
        for (uint32_t done = 0; done < take.count;) {
            const uint32_t start = (take.from + done) % words;
            const uint32_t run = take.count - done < words - start ? take.count - done : words - start;
            switch_dcache_clean_invalidate_lines(s.dmaRing.va + static_cast<size_t>(start) * 4,
                                                 static_cast<size_t>(run) * 4);
            done += run;
        }
        switch_dcache_sync();
    }
    if (take.count && !s.dmaChecked) {
        // The first reply since the start begins at word 0: the reply magic, after any zeros (uartAction's "Obird
        // workaround"). Anything else: the channel's position or data are not what the driver thinks.
        s.dmaChecked = true;
        uint32_t i = 0;
        while (i < take.count && !(ring[i] & 0xff))
            ++i;
        const bool magic = take.count - i >= 3 && (ring[i] & 0xff) == kReplyMagic[0] &&
                           (ring[i + 1] & 0xff) == kReplyMagic[1] && (ring[i + 2] & 0xff) == kReplyMagic[2];
        if (take.from != 0 || !magic) {
            dmaFailedLocked(s, "the first reply by DMA did not start with the reply magic");
            return drainRx(s);
        }
    }
    for (uint32_t i = 0; i < take.count; ++i) {
        const auto byte = static_cast<uint8_t>(ring[(take.from + i) % words]);
        const uint32_t next = (s.head + 1) % sizeof(s.ring);
        if (next == s.tail) {
            s.overflows = s.overflows + 1;
            continue;
        }
        s.ring[s.head] = byte;
        s.head = next;
        frameByte(s, byte);
    }
    s.dmaTaken = (take.from + take.count) % words;
    s.dmaWords += take.count;
    if (take.count)
        s.dmaTakes = s.dmaTakes + 1;
    const bool behind = (rd(s, LSR) & LSR_DR) != 0u;
    if (behind)
        s.dmaLate = s.dmaLate + 1;
    s.dmaTail = behind || s.frameLength != 0;
    return take.count != 0;
}

// Work loop, a running Joy-Con's tick, before its next request: the tail a late channel left (header "RX by APB DMA"),
// for the action to parse; and the channel's not-moving check, bytes in the FIFO at two ticks in a row with the channel
// at the same word.
void SwitchJoyConRail::takeTail(Side &s)
{
    bool got = false;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    if (s.dmaOn && s.dmaTail) {
        got = takeDma(s);
        if (got)
            s.dmaTails = s.dmaTails + 1;
        if (s.dmaOn && !s.dmaTail)
            wr(s, IER, s.ier);
    }
    if (s.dmaOn) {
        const uint32_t at = (tegra_apbdma_moved(s.dma, nullptr) / 4) % (s.dmaRing.size / 4);
        const bool waiting = (rd(s, LSR) & LSR_DR) != 0u;
        if (waiting && s.dmaWaited && at == s.dmaTickAt) {
            dmaFailedLocked(s, "bytes waited in the FIFO for a whole request interval with the channel not moving");
            (void)drainRx(s);
            got = true;  // the action logs why
        }
        s.dmaWaited = waiting;
        s.dmaTickAt = at;
    }
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    if (got)
        s.irq->signalInterrupt();
}

// `lock` held, work loop (run): the UART into DMA mode with the ring from word 0. What the FIFO holds is taken by PIO
// first.
void SwitchJoyConRail::startDmaLocked(Side &s)
{
    if (!s.dma || s.dmaOff || s.dmaOn)
        return;
    (void)drainRx(s);
    s.dmaTaken = 0;
    s.dmaWords = 0;
    s.dmaChecked = false;
    s.dmaTail = false;
    s.dmaWaited = false;
    s.dmaLapsAtStart = tegra_apbdma_interrupts(s.dma);
    switch_dcache_clean_invalidate(s.dmaRing.va, s.dmaRing.size);
    const unsigned request = s.cfg->uart == 'B' ? TEGRA_APBDMA_REQ_UARTB : TEGRA_APBDMA_REQ_UARTC;
    tegra_apbdma_start_rx_words_ring(s.dma, static_cast<uint32_t>(kUartPA + s.cfg->uartOffset + RBR_THR), request,
                                     s.dmaRing.pa, s.dmaRing.size);
    wr(s, IIR_FCR, FCR_FIFO | FCR_DMA | FCR_RX_TRIG_1 | FCR_TX_TRIG_16);
    s.ier = (s.ier & IER_THRI) | IER_RX_DMA;
    wr(s, IER, s.ier);
    s.dmaOn = true;
}

// `lock` held: back to PIO (RX interrupt per FIFO trigger), the channel stopped. DMA select goes off first and a read
// the channel had started lands, so what is left in the FIFO is PIO's, in order. Unread ring words are the caller's to
// drop (leaving the running phase) or already taken.
void SwitchJoyConRail::stopDmaLocked(Side &s)
{
    if (!s.dmaOn)
        return;
    wr(s, IIR_FCR, FCR_FIFO | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
    IODelay(kDmaSettleUs);
    tegra_apbdma_stop(s.dma);
    s.dmaOn = false;
    s.dmaTail = false;
    s.ier = (s.ier & IER_THRI) | IER_RX;
    wr(s, IER, s.ier);
}

// `lock` held: a non-fatal DMA check failed (header): PIO for this rail from now on; the action logs `why` once.
void SwitchJoyConRail::dmaFailedLocked(Side &s, const char *why)
{
    stopDmaLocked(s);
    s.dmaOff = true;
    s.dmaFailed = why;
}

void SwitchJoyConRail::uartAction(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (!s)
        return;
    if (s->stuck) {
        char gic[160];
        switch_interrupt_describe(s->cfg->intid, gic, sizeof(gic));
        panic("SwitchJoyCon: UART-%c interrupt stuck (INTID %u): IIR %#x LSR %#x, %s", s->cfg->uart, s->cfg->intid,
              rd(*s, IIR_FCR), rd(*s, LSR), gic);
    }
    if (s->dmaOverrun) {
        char dma[320];
        tegra_apbdma_describe(s->dma, dma, sizeof(dma));
        panic("SwitchJoyCon: UART-%c RX DMA overran its ring (data lost): taken %llu words, at word %u, %s",
              s->cfg->uart, s->dmaWords, s->dmaTaken, dma);
    }
    if (const char *why = __atomic_exchange_n(&s->dmaFailed, nullptr, __ATOMIC_ACQ_REL)) {
        char dma[320];
        tegra_apbdma_describe(s->dma, dma, sizeof(dma));
        IOLog("SwitchJoyCon: UART-%c RX DMA off for this boot (%s): RX by interrupt per FIFO trigger as before; %s\n",
              s->cfg->uart, why, dma);
    }
    // A line error inside a packet (framing, parity, overrun) spoils it: drop what is assembled and resynchronise.
    if (s->lineErrors != s->errorsSeen) {
        s->errorsSeen = s->lineErrors;
        s->packetLength = 0;
    }
    for (;;) {
        const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s->lock);
        const uint32_t head = s->head;
        if (s->tail == head) {
            // The ring is empty: the filter's framing (frameByte) becomes this parser's, which is the reference.
            s->frameLength = static_cast<uint32_t>(s->packetLength);
            s->frameSizeLow = s->packetLength > 3 ? s->packet[3] : 0;
            s->frameTotal = s->packetLength > 4 ? 5 + (s->packet[3] | static_cast<uint32_t>(s->packet[4]) << 8) : 0;
            s->packetDone = false;
        }
        IOSimpleLockUnlockEnableInterrupt(s->lock, state);
        if (s->tail == head)
            break;
        const uint32_t epoch = s->epoch;
        while (s->tail != head && s->epoch == epoch) {
            const uint8_t byte = s->ring[s->tail];
            s->tail = (s->tail + 1) % sizeof(s->ring);
            if (s->heardCount < sizeof(s->heard))
                s->heard[s->heardCount] = byte;
            ++s->heardCount;
            if (s->fast)
                ++s->fastHeardCount;
            if (s->phase == kBackoff)
                ++s->lateCount;
            if (s->phase == kHandshake) {
                if (s->stepHeardCount < sizeof(s->stepHeard))
                    s->stepHeard[s->stepHeardCount] = byte;
                ++s->stepHeardCount;
            }
            // Wait for the magic: the Joy-Con may send zeros before a packet (J:3027 "Obird workaround").
            if (s->packetLength < 3 && byte != kReplyMagic[s->packetLength]) {
                s->packetLength = byte == 0x19 ? 1 : 0;
                if (s->packetLength)
                    s->packet[0] = byte;
                continue;
            }
            s->packet[s->packetLength++] = byte;
            if (s->packetLength < 5)
                continue;
            const size_t total = 5 + (s->packet[3] | static_cast<size_t>(s->packet[4]) << 8);
            if (total < kMinPacket || total > sizeof(s->packet)) {
                ++s->dropped;
                if (total > sizeof(s->packet) && !s->oversizeLogged) {
                    s->oversizeLogged = true;
                    IOLog("SwitchJoyCon: Joy-Con (%c) rail packet header gives %zu bytes (cmd %#x), dropped\n",
                          s->cfg->letter, total, s->packet[5]);
                }
                s->packetLength = 0;
                continue;
            }
            if (s->packetLength < total)
                continue;
            s->packetLength = 0;
            self->received(*s, s->packet, total);
        }
    }
}

void SwitchJoyConRail::received(Side &s, const uint8_t *packet, size_t length)
{
    const uint64_t now = switch_now_ms();
    if (s.phase == kRunning && s.unanswered == 0 && !s.rumbleSent && !probing(s))
        ++s.unsolicited;
    if (s.phase == kRunning && s.silentMs > kQuietMS) {
        IOLog("SwitchJoyCon: Joy-Con (%c) on the rail answered again after %u requests unanswered over %llu ms (line "
              "errors %u, breaks %u in between)\n",
              s.cfg->letter, s.unanswered - 1, now - s.lastReply, s.lineErrors - s.errorsAtReply,
              s.breaks - s.breaksAtReply);
        ++s.recoveries;
    }
    s.lastReply = now;
    s.unanswered = 0;
    s.silentMs = 0;
    s.errorsAtReply = s.lineErrors;
    s.breaksAtReply = s.breaks;
    const uint8_t cmd = packet[5];
    switch (s.phase) {
    case kHandshake:
        handshakeReply(s, cmd, packet[6], packet + kHeader, length > kHeader ? length - kHeader : 0);
        break;
    case kPairing:
        if (cmd == kCmdExtRet && length > kHeader)
            pairingReply(s, packet + kHeader, length - kHeader);
        break;
    case kCalibrating:
        if (cmd == kCmdExtRet && length > kHeader)
            calibrationReply(s, packet + kHeader, length - kHeader);
        break;
    case kRunning:
#if DEVELOPMENT || DEBUG
        if (probePacket(s, packet, length, now))
            break;
#endif
        if (cmd == kCmdExtRet && length > kHeader)
            runningReport(s, packet + kHeader, length - kHeader);
        break;
    case kEnding:
        // The Joy-Con answers 91/11 and goes back to 1 Mbaud 50 ms later (brcm_flash.c wiredCmdProcess): detection's
        // settle outlasts that, and a seated Joy-Con is then handshaken as on an attach.
        if (cmd == kCmdInitRet && packet[6] == kInitHidDisconnect)
            detach(s);
        break;
    default:
        break;  // a late reply to an attempt that already failed
    }
}

void SwitchJoyConRail::send(Side &s, const uint8_t *bytes, size_t length)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    // One packet at a time; at 3 Mbaud a packet leaves in ~0.2 ms and packets are 15 ms apart, so a busy TX here means
    // the THR interrupt never came. checkDelivery below decides.
    const bool busy = s.txPosition < s.txLength;
    if (!busy) {
        memcpy(s.tx, bytes, length);
        s.txLength = static_cast<uint32_t>(length);
        s.txPosition = 0;
        fillTx(s);
        // THR-empty armed for the rest of a packet the FIFO did not take, and before the session runs even when it fit:
        // there its interrupt (fillTx disarms it) is the proof, counted in `interrupts`, that this UART's line reaches
        // the CPU while the Joy-Con says nothing. A running Joy-Con's every reply proves it, so its requests take none.
        if (s.txPosition < s.txLength || s.phase != kRunning) {
            s.ier |= IER_THRI;
            wr(s, IER, s.ier);
        }
    }
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    if (busy)
        checkDelivery(s, "TX still busy");
}

// Whether the last packet has left (its THR-empty interrupt refilled the FIFO to the end). For packets sent on a reply:
// the Joy-Con answered, so it heard the whole request.
bool SwitchJoyConRail::txIdle(Side &s)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    const bool idle = s.txPosition == s.txLength;
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    return idle;
}

void SwitchJoyConRail::sendCommand(Side &s, uint8_t subcmd, const uint8_t *payload, uint16_t length)
{
    uint8_t packet[kHeader + 8];
    send(s, packet, switch_joycon_rail_packet(packet, kCmdExtSend, subcmd, payload, length));
}

void SwitchJoyConRail::sendReport(Side &s, const uint8_t *report, size_t length)
{
    uint8_t packet[kHeader + kMaxOutput];
    send(s, packet, switch_joycon_rail_packet(packet, kCmdExtRet, 0, report, static_cast<uint16_t>(length)));
}

// An output report from the HID device (0x01 subcommand, 0x10 rumble, 0x11/0x12 MCU) with the rail's packet counter in
// byte 1 (see the header).
void SwitchJoyConRail::sendOutput(Side &s, uint8_t *report, size_t length)
{
    report[1] = static_cast<uint8_t>(s.counter++ & 0xf);
    sendReport(s, report, length);
}

// A subcommand of the driver's own (pairing, sleep), framed as the Bluetooth output report 0x01 (J:785-815).
void SwitchJoyConRail::sendSubcommand(Side &s, uint8_t id, const uint8_t *data, size_t length)
{
    uint8_t report[SWITCH_JOYCON_SUBCOMMAND_REPORT];
    switch_joycon_subcommand_report(report, s.counter++, id, data, length);
    sendReport(s, report, sizeof(report));
}

// setReport from the HID device (a user client thread): queue for the next 15 ms slot.
IOReturn SwitchJoyConRail::output(void *context, const uint8_t *report, size_t length)
{
    Side &s = *static_cast<Side *>(context);
    if (length < 2 || length > kMaxOutput)
        return kIOReturnBadArgument;
    IOReturn result = kIOReturnSuccess;
#if DEVELOPMENT || DEBUG
    if (report[0] == kReportSubcommand && length > 11 && report[10] == kSubImu) {
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: Joy-Con (%c) HID client turns its IMU %s", s.cfg->letter,
               report[11] ? "on" : "off");
    }
#endif
    IOLockLock(s.outLock);
    switch_joycon_forward_output(&s.forward, report, length);
    if (report[0] == kReportRumble) {
        memcpy(s.rumble, report, length);
        s.rumbleLength = static_cast<uint8_t>(length);
        s.rumbleFresh = true;
    } else if (static_cast<uint8_t>(s.queueTail - s.queueHead) == 8) {
        result = kIOReturnNoSpace;
    } else {
        const unsigned at = s.queueTail++ & 7;
        memcpy(s.queue[at], report, length);
        s.queueLength[at] = static_cast<uint8_t>(length);
        // Input report mode 0x3F would silence the rail (see the header): ask for 0x30.
        if (report[0] == kReportSubcommand && length > 11 && report[10] == kSubReportMode &&
            report[11] == kModeSimpleHid) {
            s.queue[at][11] = kReportFull;
        }
    }
    IOLockUnlock(s.outLock);
    return result;
}

// The device's backboardd session ended (header, "Sessions"), from inside IOService::open/close on any thread: mark the
// side and wake the work loop. The device reports only until switch_joycon_hid_destroy() has detached it, and dropHid
// clears the mark after that, so a mark never outlives the device that set it.
void SwitchJoyConRail::sessionEnded(void *context, int32_t servedPid, int32_t openerPid)
{
    Side &s = *static_cast<Side *>(context);
    IOLockLock(s.outLock);
    s.republish = true;
    s.servedPid = servedPid;
    s.openerPid = openerPid;
    IOLockUnlock(s.outLock);
    s.owner->sessions->interruptOccurred(nullptr, nullptr, 0);
}

// UART at 1 or 3 Mbaud (hekate uart_init order; Linux tegra_uart_hw_init for the FIFO and interrupt setup).
void SwitchJoyConRail::configureUart(Side &s, bool fast)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    wr(s, IER, 0);
    carWrite(s.cfg->clockSource, kUartDivEnable | (fast ? kDiv3M : kDiv1M));
    wr(s, LCR, LCR_DLAB | LCR_8N1);
    wr(s, RBR_THR, 1);  // DLL
    wr(s, IER, 0);      // DLM
    wr(s, LCR, (fast ? LCR_2STOP : 0) | LCR_8N1);
    (void)rd(s, SPR);
    wr(s, IIR_FCR, FCR_FIFO);
    (void)rd(s, SPR);
    IODelay(20);  // >= 3 UART clocks after enabling the FIFO (Linux, T210 erratum)
    wr(s, MCR, 0);
    IODelay(96);  // flow control off before the FIFOs are cleared (hekate uart_init)
    wr(s, IIR_FCR, FCR_FIFO | FCR_RX_CLR | FCR_TX_CLR | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
    wr(s, IRDA_CSR, CSR_INVERT_TXD | CSR_INVERT_RTS);
    wr(s, MCR, MCR_RTS_EN);
    IODelay(3);
    while (rd(s, LSR) & LSR_DR)
        (void)rd(s, RBR_THR);
    s.tail = s.head;  // bytes of the old rate are dropped; the receive loop sees the new epoch and rereads head
    s.epoch++;
    s.frameLength = 0;
    s.packetDone = false;
    s.txLength = s.txPosition = 0;
    s.fast = fast;
    s.ier = IER_RX;
    wr(s, IER, s.ier);
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    s.packetLength = 0;
}

// Wait for a Joy-Con: UART quiet, TX pad released so the detect pin reads the rail, charger on (J:1711-1771).
void SwitchJoyConRail::enterDetect(Side &s)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    stopDmaLocked(s);
    s.ier = 0;
    wr(s, IER, 0);
    s.txLength = s.txPosition = 0;
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    gpioMask(s.cfg->txPin, GPIO_MSK_OE, false);
    gpioMask(s.cfg->txPin, GPIO_MSK_CNF, true);
    setCharging(s, true);
    s.phase = kSettling;
    s.timer->setTimeoutMS(kSettleMS);
}

void SwitchJoyConRail::detectAction(OSObject *owner, IOInterruptEventSource *sender, int)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    // An edge while the pad is still settling, or one that is already gone (contact bounce): keep waiting.
    if (s && s->phase == kWaiting && tegra_gpio_read(s->cfg->detectPin) == 0)
        self->attachJoyCon(*s);
}

void SwitchJoyConRail::attachJoyCon(Side &s)
{
    s.detect->disable();
    s.timer->cancelTimeout();
    s.attempts = s.fastFailures = 0;
    s.oversizeLogged = false;
    s.furthest = 0;
    s.heardCount = s.fastHeardCount = s.fastSent = s.lateCount = 0;
    s.interruptsAtAttach = s.interrupts;
    s.errorsAtAttach = s.lineErrors;
    s.fastErrorsAtAttach = s.fastErrors;
    s.paired = false;
    startAttempt(s);
}

void SwitchJoyConRail::startAttempt(Side &s)
{
    configureUart(s, false);
    // TX pad back to the UART (after the first attempt it already is) only now that the UART drives the inverted
    // idle: the rail line stays low from detection into the handshake instead of rising.
    gpioMask(s.cfg->txPin, GPIO_MSK_CNF, false);
    s.phase = kHandshake;
    s.step = kWake;
    s.sends = 0;
    s.stepHeardCount = 0;
    sendStep(s);
}

void SwitchJoyConRail::sendStep(Side &s)
{
    static const uint8_t wake[] = {0xa1, 0xa2,          0xa3, 0xa4, 0x19, 0x01, 0x03, 0x07,
                                   0x00, kCmdHandshake, 0x02, 0x01, 0x7e, 0x00, 0x00, 0x00};
    static const uint8_t baud[] = {0xc0, 0xc6, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x00};  // 3,000,000
    static const uint8_t rate[] = {0x0f, 0x00, 0x00, 0x00};                          // 15 ms
    uint32_t timeout = kCommandMS;
    if (s.step > s.furthest)
        s.furthest = s.step;
    ++s.sends;
    if (s.fast)
        ++s.fastSent;
    switch (s.step) {
    case kWake:
        send(s, wake, sizeof(wake));
        timeout = kHandshakeMS;
        break;
    case kInfo:
        sendCommand(s, kInitMac);
        break;
    case kBaud:
        sendCommand(s, kInitBaud, baud, sizeof(baud));
        break;
    // The first packet at 3 Mbaud: the Joy-Con is still switching and misses the first ones, so it is sent again every
    // 5 ms until answered (timerAction; see the header).
    case kDisconnect:
        sendCommand(s, kInitHidDisconnect);
        timeout = kResendMS;
        break;
    case kConnect:
        sendCommand(s, kInitHidConnect);
        timeout = kFastCommandMS;
        break;
    case kRate:
        sendCommand(s, kInitHidRate, rate, sizeof(rate));
        timeout = kFastCommandMS;
        break;
    default:
        break;
    }
    s.timer->setTimeoutMS(timeout);
}

void SwitchJoyConRail::handshakeReply(Side &s, uint8_t cmd, uint8_t echo, const uint8_t *payload, size_t length)
{
    static const uint8_t expected[] = {0, kInitMac, kInitBaud, kInitHidDisconnect, kInitHidConnect, kInitHidRate};
    if (s.step == kWake ? cmd != kCmdHandshake : (cmd != kCmdInitRet || echo != expected[s.step]))
        return;
    if (s.step == kInfo) {
        if (length < 7)
            return;
        for (unsigned i = 0; i < 6; ++i)
            s.mac[i] = payload[6 - i];  // LSB first on the wire (J:2607)
    }
    if (s.step == kBaud)
        configureUart(s, true);  // the reply was the last byte at 1 Mbaud
    if (s.step != kRate) {
        ++s.step;
        s.sends = 0;
        s.stepHeardCount = 0;
        sendStep(s);
        return;
    }
    s.timer->cancelTimeout();
    s.pairPending = pairing;
    if (pairing && hostKnown) {
        startPairing(s);
    } else {
        calibrate(s);
    }
}

void SwitchJoyConRail::failAttempt(Side &s, const char *why)
{
    checkDelivery(s, why);
    // kAttempts wakes are about 3 s, the time a Joy-Con takes to reboot (J:1709): give up until the next attach. Past
    // the switch the Joy-Con stays at 3 Mbaud until its HID timeout and ignores a 1 Mbaud wake until then.
    if (++s.attempts >= kAttempts || (s.fast && ++s.fastFailures >= kFastAttempts)) {
        char state[640];
        describe(s, state, sizeof(state));
        IOLog("SwitchJoyCon: Joy-Con (%c) on the rail does not answer (%s): %s\n", s.cfg->letter, why, state);
        enterDetect(s);
        return;
    }
    s.phase = kBackoff;
    s.timer->setTimeoutMS(s.fast ? kJoyConTimeoutMS : kRetryMS);
}

// Right after the handshake (see the header).
void SwitchJoyConRail::startPairing(Side &s)
{
    s.pairPending = false;
    s.phase = kPairing;
    s.step = kReportSlot ? kReadSlot : kDeviceInfo;
    s.slot = 0;
    s.slotInUse = s.slotCorrupt = false;
    sendPairStep(s);
}

// A running Joy-Con's session ends for a pairing to open the next one (see the header), at a tick after its last request
// was answered, so nothing of the Joy-Con's is still on the line. The HID device goes, and RX is by PIO from here.
void SwitchJoyConRail::endSession(Side &s)
{
    dropHid(s);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    stopDmaLocked(s);
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    s.phase = kEnding;
    sendCommand(s, kInitHidDisconnect);
    s.timer->setTimeoutMS(kFastCommandMS);
}

void SwitchJoyConRail::sendPairStep(Side &s)
{
    switch (s.step) {
    case kReadSlot:
    case kReadSlotTail: {
        const bool tail = s.step == kReadSlotTail;
        const uint32_t address = kSlotBase + s.slot * kSlotStride + (tail ? SWITCH_JOYCON_SLOT_HEAD : 0);
        const uint8_t read[5] = {static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8),
                                 static_cast<uint8_t>(address >> 16), static_cast<uint8_t>(address >> 24),
                                 static_cast<uint8_t>(tail ? SWITCH_JOYCON_SLOT_TAIL : SWITCH_JOYCON_SLOT_HEAD)};
        sendSubcommand(s, kSubSpiRead, read, sizeof(read));
        break;
    }
    case kDeviceInfo:
        sendSubcommand(s, kSubDeviceInfo, nullptr, 0);
        break;
    case kShipment: {
        const uint8_t off = 0;
        sendSubcommand(s, kSubShipment, &off, 1);
        break;
    }
    case kSendHost:
    case kGetKey:
    case kSave: {
        static const uint8_t types[] = {SWITCH_JOYCON_PAIR_HOST, SWITCH_JOYCON_PAIR_KEY, SWITCH_JOYCON_PAIR_SAVE};
        uint8_t request[SWITCH_JOYCON_PAIRING_REQUEST];
        const size_t length = switch_joycon_pairing_request(request, types[s.step - kSendHost], host);
        sendSubcommand(s, kSubPairing, request, length);
        break;
    }
    default:
        break;
    }
    s.timer->setTimeoutMS(kSubcommandMS);
}

// An SPI read of the pairing log (SwitchJoyConPairingSlot.h): data is the address (4), the size, then the bytes. The
// log is walked by magic and the slot in use read whole and checked, for the log line; the pairing follows either way.
void SwitchJoyConRail::pairingSlotReply(Side &s, const uint8_t *data, bool ack, size_t length)
{
    const bool tail = s.step == kReadSlotTail;
    const uint32_t size = tail ? SWITCH_JOYCON_SLOT_TAIL : SWITCH_JOYCON_SLOT_HEAD;
    if (!ack || length < 15 + 5 + size) {
        finishPairing(s, false, "pairing slot read refused");
        return;
    }
    const uint32_t address = data[0] | static_cast<uint32_t>(data[1]) << 8 | static_cast<uint32_t>(data[2]) << 16 |
                             static_cast<uint32_t>(data[3]) << 24;
    if (address != kSlotBase + s.slot * kSlotStride + (tail ? SWITCH_JOYCON_SLOT_HEAD : 0))
        return;  // a late reply to an earlier read
    memcpy(s.slotBytes + (tail ? SWITCH_JOYCON_SLOT_HEAD : 0), data + 5, size);
    if (!tail) {
        switch (switch_joycon_slot_head_kind(s.slotBytes)) {
        case SWITCH_JOYCON_SLOT_EARLIER:
            if (++s.slot < kSlots) {
                sendPairStep(s);
                return;
            }
            break;
        case SWITCH_JOYCON_SLOT_IN_USE:
            s.step = kReadSlotTail;
            sendPairStep(s);
            return;
        case SWITCH_JOYCON_SLOT_END:
            break;
        }
        s.slotInUse = false;
    } else {
        s.slotInUse = switch_joycon_slot_valid(s.slotBytes);
        s.slotCorrupt = !s.slotInUse;
    }
    s.step = kDeviceInfo;
    sendPairStep(s);
}

// Subcommand replies: report 0x21, ACK in byte 13 (bit 7), subcommand id in 14, data from 15 (deku
// bluetooth_hid_notes.md, J:392-412).
void SwitchJoyConRail::pairingReply(Side &s, const uint8_t *report, size_t length)
{
    if (length < 16 || report[0] != kReportReply)
        return;
    const uint8_t id = report[14];
    const uint8_t *data = report + 15;
    const bool ack = (report[13] & 0x80) != 0;
    if ((s.step == kReadSlot || s.step == kReadSlotTail) && id == kSubSpiRead) {
        pairingSlotReply(s, data, ack, length);
        return;
    }
    if ((s.step == kDeviceInfo && id == kSubDeviceInfo) || (s.step == kShipment && id == kSubShipment)) {
        if (!ack) {
            finishPairing(s, false, s.step == kDeviceInfo ? "device info refused" : "shipment state refused");
            return;
        }
        ++s.step;
        sendPairStep(s);
        return;
    }
    // Steps 1 and 2 echo their request type in the first data byte (deku); for step 3 the ACK is enough.
    if (id != kSubPairing || !ack || (s.step != kSave && data[0] != (s.step == kSendHost ? 0x01 : 0x02)))
        return;
    if (s.step == kSendHost) {
        s.step = kGetKey;
        sendPairStep(s);
        return;
    }
    if (s.step == kGetKey) {
        // XOR 0xAA, same byte order as the slot it is saved to (deku): reversed for HCI as above.
        if (length < 15 + 1 + 16) {
            finishPairing(s, false, "short pairing key reply");
            return;
        }
        for (unsigned i = 0; i < 16; ++i)
            s.key[i] = data[1 + 15 - i] ^ 0xaa;
        s.step = kSave;
        sendPairStep(s);
        return;
    }
    finishPairing(s, true, "rail");
}

void SwitchJoyConRail::finishPairing(Side &s, bool paired, const char *how)
{
    s.timer->cancelTimeout();
    s.paired = paired;
#if DEVELOPMENT || DEBUG
    char found[96] = "before: no slot in use";
    if (s.slotCorrupt)
        snprintf(found, sizeof(found), "before: slot %u in use fails its checksum", s.slot);
    if (s.slotInUse) {
        const uint8_t *h = s.slotBytes + 4;
        snprintf(found, sizeof(found),
                 "before: slot %u in use names %02x:%02x:%02x:%02x:%02x:%02x, host capability 0x%02x", s.slot, h[0],
                 h[1], h[2], h[3], h[4], h[5], s.slotBytes[SWITCH_JOYCON_SLOT_CAPS]);
    }
#endif
    if (paired) {
        publishPairing(s, how);
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT,
               "SwitchJoyCon: Joy-Con (%c) %02x:%02x:%02x:%02x:%02x:%02x paired to %02x:%02x:%02x:%02x:%02x:%02x over "
               "the rail (%s)",
               s.cfg->letter, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5], host[0], host[1], host[2],
               host[3], host[4], host[5], kReportSlot ? found : "");
#endif
    } else {
        IOLog("SwitchJoyCon: Joy-Con (%c) not paired for Bluetooth: %s\n", s.cfg->letter, how);
    }
    calibrate(s);
}

// Before the session runs and its HID device exists, so no reply of the rail's own reaches it: the Joy-Con's stick
// calibration and parameters (SwitchJoyConStick.h; once a boot per Joy-Con), then subcommand 0x03 0x30, the input
// report mode the rail runs in, whatever an earlier session left (Horizon sends it at every attach: deku
// packet_parse/c2j.txt 13), and 0x40 0 (its IMU samples from the HID connect on: deku packet_parse/j2c.txt's first 0x30
// report carries them before Horizon's 0x40 1).
void SwitchJoyConRail::calibrate(Side &s)
{
    s.phase = kCalibrating;
    s.step = kCalibrateMode;
    if (!s.stickKnown || memcmp(s.stickMac, s.mac, sizeof(s.mac)) != 0) {
        switch_joycon_stick_defaults(&s.stick, s.cfg->left);
        s.step = 0;
    }
    sendCalibrationStep(s);
}

void SwitchJoyConRail::sendCalibrationStep(Side &s)
{
    if (s.step < SWITCH_JOYCON_STICK_READS) {
        uint8_t read[5];
        switch_joycon_stick_request(&s.stick, s.step, read);
        sendSubcommand(s, kSubSpiRead, read, sizeof(read));
    } else if (s.step == kCalibrateMode) {
        const uint8_t mode = kReportFull;
        sendSubcommand(s, kSubReportMode, &mode, 1);
    } else {
        const uint8_t off = 0;
        sendSubcommand(s, kSubImu, &off, 1);
    }
    s.timer->setTimeoutMS(kSubcommandMS);
}

// A refused read leaves the defaults; a reply to an earlier read is ignored.
void SwitchJoyConRail::calibrationReply(Side &s, const uint8_t *report, size_t length)
{
    if (length < 16 || report[0] != kReportReply)
        return;
    const bool ack = (report[13] & 0x80) != 0;
    if (s.step < SWITCH_JOYCON_STICK_READS) {
        if (report[14] != kSubSpiRead ||
            (ack && !switch_joycon_stick_reply(&s.stick, s.step, report + 15, length - 15)))
            return;
        ++s.step;
        sendCalibrationStep(s);
        return;
    }
    if (report[14] != (s.step == kCalibrateMode ? kSubReportMode : kSubImu))
        return;
    if (s.step == kCalibrateMode) {
        ++s.step;
        sendCalibrationStep(s);
        return;
    }
    s.timer->cancelTimeout();
    if (!s.stickKnown || memcmp(s.stickMac, s.mac, sizeof(s.mac)) != 0) {
        memcpy(s.stickMac, s.mac, sizeof(s.mac));
        s.stickKnown = true;
#if DEVELOPMENT || DEBUG
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: Joy-Con (%c) stick centre %u,%u (%s calibration), dead zone %u (%s)",
               s.cfg->letter, s.stick.centre[0], s.stick.centre[1], s.stick.user ? "user" : "factory",
               s.stick.dead_zone, s.stick.params ? "its own" : "default");
#endif
    }
    run(s);
}

// The record joyconpaird watches for (IOKit matching on IOProviderClass SwitchJoyConPairing). Replaced on every pairing;
// the helper hands it to bluetoothd with BTAccessoryManagerSetLinkKeyEx, which is idempotent.
void SwitchJoyConRail::publishPairing(Side &s, const char *source)
{
    if (s.record) {
        s.record->terminate();
        OSSafeReleaseNULL(s.record);
    }
    char address[18];
    snprintf(address, sizeof(address), "%02x:%02x:%02x:%02x:%02x:%02x", s.mac[0], s.mac[1], s.mac[2], s.mac[3],
             s.mac[4], s.mac[5]);
    auto *record = OSTypeAlloc(SwitchJoyConPairing);
    OSData *mac = OSData::withBytes(s.mac, 6);
    OSData *key = OSData::withBytes(s.key, 16);
    OSData *hostAddress = OSData::withBytes(host, 6);
    const bool ok =
        (record != nullptr) && (mac != nullptr) && (key != nullptr) && (hostAddress != nullptr) && record->init();
    if (ok) {
        record->setProperty("Address", mac);
        record->setProperty("AddressString", address);
        record->setProperty("LinkKey", key);
        record->setProperty("HostAddress", hostAddress);
        record->setProperty("Name", s.cfg->left ? "Joy-Con (L)" : "Joy-Con (R)");
        record->setProperty("ClassOfDevice", static_cast<unsigned long long>(kJoyConClass), 32);
        record->setProperty("VendorID", 0x057eULL, 32);
        record->setProperty("ProductID",
                            static_cast<unsigned long long>(s.cfg->left ? kSwitchJoyConLeftPID : kSwitchJoyConRightPID),
                            32);
        record->setProperty("Source", source);
    }
    OSSafeReleaseNULL(mac);
    OSSafeReleaseNULL(key);
    OSSafeReleaseNULL(hostAddress);
    if (!ok || !record->attach(this)) {
        OSSafeReleaseNULL(record);
        IOLog("SwitchJoyCon: Joy-Con (%c) pairing record not published\n", s.cfg->letter);
        return;
    }
    record->registerService();
    s.record = record;
}

void SwitchJoyConRail::run(Side &s)
{
    IOLockLock(s.outLock);
    s.queueHead = s.queueTail = 0;
    s.rumbleLength = 0;
    s.rumbleFresh = false;
    IOLockUnlock(s.outLock);
    s.rumbleSent = false;
    s.phase = kRunning;
    s.lastReply = switch_now_ms();
    s.unanswered = 0;
    s.silentMs = 0;
    s.imuOff = false;
    s.ownReply = 0;
    s.stateKnown = false;
    switch_joycon_cadence_reset(&s.cadence, s.lastReply);
    s.errorsAtReply = s.lineErrors;
    s.breaksAtReply = s.breaks;
    s.reports = 0;
    s.errorsAtRun = s.lineErrors;
    s.overflowsAtRun = s.overflows;
    switch_joycon_use_reset(&s.use);
    s.hid = gamepadsLive(switch_now_ms()) ? createHid(s) : nullptr;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    startDmaLocked(s);  // before the first request: its reply is the first the DMA takes
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    tick(s);
    if (!polling)
        armPoll();  // a running clock keeps its phase: the other rail's Joy-Con is not delayed
}

IOService *SwitchJoyConRail::createHid(Side &s)
{
    IOLockLock(s.outLock);
    switch_joycon_forward_reset(&s.forward);
    IOLockUnlock(s.outLock);
    return switch_joycon_hid_create(s.cfg->left, s.mac, &SwitchJoyConRail::output, &SwitchJoyConRail::sessionEnded, &s);
}

// Every device goes through here. Once destroy has detached it, the device reports nothing more, so a session mark it
// set before that is cleared with it and cannot republish the next device.
void SwitchJoyConRail::dropGamepad(Side &s)
{
    if (s.hid)
        switch_joycon_hid_destroy(s.hid);
    s.hid = nullptr;
    IOLockLock(s.outLock);
    s.republish = false;
    s.imuOff |= s.forward.imu;
    s.forward.imu = false;
    IOLockUnlock(s.outLock);
}

void SwitchJoyConRail::dropHid(Side &s)
{
    dropGamepad(s);
    switch_joycon_hid_buttons_release(s.cfg->left);
}

// iOS 27 only: the focus engine an idle gamepad wakes is UIKitCore 24B119's (SwitchJoyConUse.h); 17 publishes at attach.
bool SwitchJoyConRail::gamepadsLive(uint64_t now)
{
#if XNU_12377_121_6
    if (gamepads && switch_joycon_use_idle(lastUse, now))
        idleGamepads();
    return gamepads;
#else
    (void)now;
    return true;
#endif
}

void SwitchJoyConRail::noteUse()
{
    lastUse = switch_now_ms();
    if (gamepads)
        return;
    gamepads = true;
    ++gamepadsPublished;
    for (Side &s : sides) {
        if (s.phase == kRunning && !s.hid)
            s.hid = createHid(s);
    }
    os_log(OS_LOG_DEFAULT, "SwitchJoyCon: rail Joy-Cons in use: game controllers published");
}

void SwitchJoyConRail::idleGamepads()
{
    gamepads = false;
    ++gamepadsIdled;
    for (Side &s : sides) {
        dropGamepad(s);
        switch_joycon_use_reset(&s.use);
    }
    os_log(OS_LOG_DEFAULT, "SwitchJoyCon: rail Joy-Cons idle for %u s: game controllers disconnected",
           SWITCH_JOYCON_USE_IDLE_MS / 1000u);
}

void SwitchJoyConRail::sessionAction(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    for (Side &s : self->sides)
        self->republish(s);
}

// A new device for a running Joy-Con whose backboardd session ended (header, "Sessions").
void SwitchJoyConRail::republish(Side &s)
{
    IOLockLock(s.outLock);
    const bool wanted = s.republish;
    const int32_t served = s.servedPid;
    const int32_t opener = s.openerPid;
    s.republish = false;
    IOLockUnlock(s.outLock);
    if (!wanted || republishOff || s.phase != kRunning || !s.hid)
        return;
    if (!switch_joycon_republish_allowed(&s.guard, served, switch_now_ms())) {
        republishOff = true;
        IOLog("SwitchJoyCon: Joy-Con (%c): backboardd[%d] ended a second HID session within %u ms; not publishing "
              "Joy-Con HID devices again this boot\n",
              s.cfg->letter, served, static_cast<unsigned>(kSwitchJoyConRepublishGapMS));
        return;
    }
    dropHid(s);
    ++s.republished;
    // What the old session queued or left vibrating is not the new session's (run() starts the same way).
    IOLockLock(s.outLock);
    s.queueHead = s.queueTail = 0;
    s.rumbleLength = 0;
    s.rumbleFresh = false;
    IOLockUnlock(s.outLock);
    s.rumbleSent = false;
    s.hid = createHid(s);
    if (opener) {
        os_log(OS_LOG_DEFAULT,
               "SwitchJoyCon: Joy-Con (%c) HID device published again: backboardd[%d] opened the one "
               "backboardd[%d] served",
               s.cfg->letter, opener, served);
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: Joy-Con (%c) HID device published again: backboardd[%d] closed it",
               s.cfg->letter, served);
    }
}

// The shared clock, at the shortest cadence of the running Joy-Cons (SwitchJoyConCadence.h): armed while any Joy-Con
// runs, left to lapse when none does (see the header). Sides that stop running elsewhere (detach, pause, pairing) need
// nothing: the next fire finds them gone and does not re-arm.
void SwitchJoyConRail::armPoll()
{
    polling = false;
    uint32_t interval = SWITCH_JOYCON_CADENCE_QUIET_MS;
    for (const Side &s : sides) {
        if (s.phase != kRunning)
            continue;
        polling = true;
        if (probing(s) || switch_joycon_cadence_interval(&s.cadence) < interval)
            interval = SWITCH_JOYCON_CADENCE_ACTIVE_MS;
    }
    pollInterval = interval;
    if (polling)
        poll->setTimeoutMS(interval);
}

void SwitchJoyConRail::pollAction(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    for (Side &s : self->sides) {
        if (s.phase == kRunning)
            self->tick(s);
    }
    (void)self->gamepadsLive(switch_now_ms());
    self->armPoll();
}

// A request at the Joy-Con's cadence (SwitchJoyConCadence.h): the IMU off its HID client left on, else the oldest
// queued output report (subcommands keep their order), else the input request, after the tail of the last reply a late
// RX channel left. A Joy-Con that left kSilentMS of requests unanswered is gone (header).
void SwitchJoyConRail::tick(Side &s)
{
    takeTail(s);
    if (s.phase != kRunning)
        return;
#if DEVELOPMENT || DEBUG
    if (probeTick(s))
        return;
#endif
    if (s.silentMs >= kSilentMS) {
        checkDelivery(s, "no reply to the requests of a whole silence");
        // Pulled off the rail, or the HID phase does not work (0 reports): either way it is handshaken again if seated.
        const char *rxDma = s.dma && !s.dmaOff ? "idle" : "off";
        if (s.dmaOn)
            rxDma = "on";
        IOLog(
            "SwitchJoyCon: Joy-Con (%c) %02x:%02x:%02x:%02x:%02x:%02x (%s) on the rail stopped answering after %u "
            "input reports: %u requests unanswered over %llu ms (line errors %u, breaks %u since its last reply; line "
            "errors %u, ring overflows %u since the handshake; RX DMA %s: %u takes, %u found it behind, %u tails at "
            "the tick)\n",
            s.cfg->letter, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5],
            s.paired ? "paired as its session opened" : "not paired", s.reports, s.unanswered,
            switch_now_ms() - s.lastReply, s.lineErrors - s.errorsAtReply, s.breaks - s.breaksAtReply,
            s.lineErrors - s.errorsAtRun, s.overflows - s.overflowsAtRun, rxDma, s.dmaTakes, s.dmaLate, s.dmaTails);
        ++s.silences;
        detach(s);
        return;
    }
    if (s.pairPending && hostKnown && s.unanswered == 0) {
        endSession(s);
        return;
    }
    IOLockLock(s.outLock);
    const bool busy = s.queueHead != s.queueTail || s.rumbleFresh || s.forward.imu ||
                      switch_joycon_rumble_moving(s.cfg->left, s.rumble, s.rumbleLength);
    IOLockUnlock(s.outLock);
    if (!switch_joycon_cadence_due(&s.cadence, busy || s.imuOff, switch_now_ms()))
        return;
    ++s.unanswered;
    ++s.requests;
    s.silentMs += switch_joycon_cadence_interval(&s.cadence);
    s.rumbleSent = false;
    s.ownReply = 0;
    if (s.imuOff) {
        const uint8_t off = 0;
        s.imuOff = false;
        s.ownReply = kSubImu;
        sendSubcommand(s, kSubImu, &off, 1);
        return;
    }
    uint8_t report[kMaxOutput];
    size_t length = 0;
    IOLockLock(s.outLock);
    if (s.queueHead != s.queueTail) {
        const unsigned at = s.queueHead++ & 7;
        length = s.queueLength[at];
        memcpy(report, s.queue[at], length);
    }
    IOLockUnlock(s.outLock);
    if (length) {
        sendOutput(s, report, length);
    } else {
        report[0] = kRequestInput;
        sendReport(s, report, 1);
    }
}

// After a reply to a request: the newest rumble report if the Joy-Con has not had it, or the last one again every 50 ms
// while it asks for a non-zero amplitude on this Joy-Con (switch_joycon_rumble_moving). A reply to the rumble report
// itself sends nothing.
void SwitchJoyConRail::rumbleAfterReply(Side &s)
{
    if (s.rumbleSent || !txIdle(s))
        return;
    uint8_t report[kMaxOutput];
    size_t length = 0;
    const uint64_t now = switch_now_ms();
    IOLockLock(s.outLock);
    if (s.rumbleLength >= 10) {
        const bool moving = switch_joycon_rumble_moving(s.cfg->left, s.rumble, s.rumbleLength);
        if (s.rumbleFresh || (moving && now - s.rumbleAt >= kRumbleKeepAliveMS)) {
            length = s.rumbleLength;
            memcpy(report, s.rumble, length);
            s.rumbleFresh = false;
        }
    }
    IOLockUnlock(s.outLock);
    if (!length)
        return;
    s.rumbleSent = true;
    s.rumbleAt = now;
    ++s.requests;
    sendOutput(s, report, length);
}

void SwitchJoyConRail::runningReport(Side &s, const uint8_t *report, size_t length)
{
    // Battery in the high nibble of byte 2 (0 empty .. 8 full, bit 4 charging; J:1143-1170): stop charging when full,
    // start again below "high", as L4T does to spare the Joy-Con's simple charger.
    if ((report[0] == kReportReply || report[0] == kReportFull || report[0] == kReportNfcIr) && length > 2) {
        const uint8_t level = report[2] >> 5;
        if (level == 4) {
            setCharging(s, false);
        } else if (level < 3) {
            setCharging(s, true);
        }
    }
    ++s.reports;
    ++s.replies;
    rumbleAfterReply(s);
    switch_joycon_hid_buttons(s.cfg->left, report, length);
    if (length >= sizeof(s.state) && report[0] != kReportReply) {
        const bool engaged = !s.stateKnown || switch_joycon_cadence_engaged(&s.stick, s.state, report);
        if (switch_joycon_cadence_input(&s.cadence, engaged, switch_now_ms()) &&
            pollInterval > SWITCH_JOYCON_CADENCE_ACTIVE_MS) {
            pollInterval = SWITCH_JOYCON_CADENCE_ACTIVE_MS;
            poll->setTimeoutMS(pollInterval);
        }
        memcpy(s.state, report, sizeof(s.state));
        s.stateKnown = true;
    }
#if XNU_12377_121_6
    if (switch_joycon_use_report(&s.use, &s.stick, report, length))
        noteUse();
#endif
    if (!s.hid || ownReplyTaken(s, report, length))
        return;
    IOLockLock(s.outLock);
    const bool forward = switch_joycon_forward_report(&s.forward, &s.stick, report, length);
    IOLockUnlock(s.outLock);
    if (forward) {
        ++s.forwarded;
        switch_joycon_hid_input(s.hid, report, length);
    } else {
        ++s.suppressed;
    }
    // 0x31 is 0x30 (timer, battery, buttons, sticks, vibrator, three IMU samples; yuzu joycon_types.h
    // InputReportNfcIr) followed by the MCU's data. JoyConHIDServicePlugin drops 0x31 (21H461, FUN_0000f928 handles
    // 0x21, 0x30, 0x3F), so the head goes to the device again as 0x30 and the controller keeps working during a scan.
    if (nfc && report[0] == kReportNfcIr && length >= kStandardFull) {
        uint8_t standard[kStandardFull];
        memcpy(standard, report, sizeof(standard));
        standard[0] = kReportFull;
        switch_joycon_hid_input(s.hid, standard, sizeof(standard));
    }
}

// The reply to the rail's own subcommand (tick: the IMU off) is not the HID client's, which waits for its own.
bool SwitchJoyConRail::ownReplyTaken(Side &s, const uint8_t *report, size_t length)
{
    if (!s.ownReply || report[0] != kReportReply || length < 15 || report[14] != s.ownReply)
        return false;
    s.ownReply = 0;
    return true;
}

bool SwitchJoyConRail::probing(const Side &s)
{
#if DEVELOPMENT || DEBUG
    return s.probe.active;
#else
    (void)s;
    return false;
#endif
}

#if DEVELOPMENT || DEBUG
// A stage the tick moved past is one log line, the decider of its question (SwitchJoyConProbe.h).
bool SwitchJoyConRail::probeTick(Side &s)
{
    const switch_joycon_probe_stage *stage = nullptr;
    const unsigned at = s.probe.stage;
    const uint8_t action = switch_joycon_probe_tick(&s.probe, switch_now_ms(), &stage);
    if (s.probe.stage != at) {
        char line[320];
        (void)switch_joycon_probe_format(&s.probe, at, line, sizeof(line));
        os_log(OS_LOG_DEFAULT, "SwitchJoyCon: probe Joy-Con (%c) %s", s.cfg->letter, line);
    }
    switch (action) {
    case SWITCH_JOYCON_PROBE_OFF:
        return false;
    case SWITCH_JOYCON_PROBE_REQUEST: {
        const uint8_t request = kRequestInput;
        sendReport(s, &request, 1);
        break;
    }
    case SWITCH_JOYCON_PROBE_INIT: {
        const uint8_t rate[4] = {stage->argument, 0, 0, 0};
        sendCommand(s, stage->command, stage->command == kInitHidRate ? rate : nullptr,
                    stage->command == kInitHidRate ? sizeof(rate) : 0);
        break;
    }
    case SWITCH_JOYCON_PROBE_MODE:
        sendSubcommand(s, stage->command, &stage->argument, 1);
        break;
    default:
        break;
    }
    return true;
}

bool SwitchJoyConRail::probePacket(Side &s, const uint8_t *packet, size_t length, uint64_t now)
{
    if (!s.probe.active)
        return false;
    switch_joycon_probe_packet(&s.probe, packet, length, now);
    return true;
}
#endif

void SwitchJoyConRail::detach(Side &s)
{
    s.timer->cancelTimeout();
    dropHid(s);
    enterDetect(s);
}

// Display off (see the header). A rail waiting for an attach has its detect interrupt's delivery checked first: the one
// look at it that needs no timer. The poll lapses by itself once no side runs.
void SwitchJoyConRail::pause(Side &s)
{
    if (s.phase == kWaiting)
        tegra_gpio_check_delivery(s.cfg->detectPin, "SwitchJoyCon");
    s.timer->cancelTimeout();
    s.detect->disable();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    stopDmaLocked(s);
    s.ier = 0;
    wr(s, IER, 0);
    s.txLength = s.txPosition = 0;
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    dropHid(s);
    s.phase = kSuspended;
}

void SwitchJoyConRail::setCharging(Side &s, bool on)
{
    if (s.chargeKnown && s.charging == on)
        return;
    if (tegra_gpio_output(s.cfg->chargePin, on ? 1 : 0))
        panic("SwitchJoyCon: charge-enable GPIO %u refused", s.cfg->chargePin);
    s.charging = on;
    s.chargeKnown = true;
}

void SwitchJoyConRail::timerAction(OSObject *owner, IOTimerEventSource *sender)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    Side *s = self->sideFor(sender);
    if (!s)
        return;
    switch (s->phase) {
    case kSettling:
        // Arm first, then look: a Joy-Con already on the rail never makes an edge (J:1753-1757). Nothing else is armed
        // while the rail is empty.
        s->phase = kWaiting;
        s->detect->enable();
        if (tegra_gpio_read(s->cfg->detectPin) == 0)
            self->attachJoyCon(*s);
        break;
    case kWaiting:
        break;
    case kBackoff:
        self->startAttempt(*s);
        break;
    case kHandshake:
        if (s->step == kDisconnect && s->sends < kDisconnectSends) {
            SwitchJoyConRail::sendStep(*s);
        } else {
            self->failAttempt(*s, "handshake step timed out");
        }
        break;
    case kPairing:
        SwitchJoyConRail::checkDelivery(*s, "pairing step timed out");
        self->finishPairing(*s, false, "no reply");
        break;
    case kCalibrating:
        SwitchJoyConRail::checkDelivery(*s, "calibration step timed out");
        IOLog("SwitchJoyCon: Joy-Con (%c) did not answer its %s; default stick calibration for this session\n",
              s->cfg->letter,
              s->step < SWITCH_JOYCON_STICK_READS ? "stick calibration read"
              : s->step == kCalibrateMode         ? "input report mode 0x30"
                                                  : "IMU off");
        self->run(*s);
        s->imuOff = true;
        break;
    case kEnding:
        SwitchJoyConRail::checkDelivery(*s, "no answer to 91/11");
        IOLog("SwitchJoyCon: Joy-Con (%c) did not answer the end of its session (91/11); handshaking it again\n",
              s->cfg->letter);
        self->detach(*s);
        break;
    case kRunning:  // the shared poll drives it
    case kSuspended:
        break;
    }
}

// A wait ended without the reply. Bytes in the RX FIFO with RX interrupts enabled, or an empty TX FIFO with THRI
// enabled (send() arms it for every packet), mean the UART asked and the CPU never heard: give it 50 ms, then
// panic. Otherwise the Joy-Con is simply not answering.
void SwitchJoyConRail::checkDelivery(Side &s, const char *why)
{
    for (unsigned look = 0; look < 2; ++look) {
        const uint32_t seen = s.interrupts;
        const uint32_t lsr = rd(s, LSR);
        // DMA mode: words the channel put in the ring that no EORD/RX-timeout interrupt took, and no tail was left
        // to the tick for.
        const bool ring =
            s.dmaOn && !s.dmaTail && (tegra_apbdma_moved(s.dma, nullptr) / 4) % (s.dmaRing.size / 4) != s.dmaTaken;
        const bool rx = (((lsr & LSR_DR) != 0u) && ((s.ier & (IER_RDI | IER_RTOIE)) != 0u)) || ring;
        const bool tx = ((lsr & LSR_THRE) != 0u) && ((s.ier & IER_THRI) != 0u);
        if (!rx && !tx)
            return;
        if (look == 0) {
            IOSleep(50);
            if (s.interrupts != seen)
                return;
            continue;
        }
        char gic[160];
        switch_interrupt_describe(s.cfg->intid, gic, sizeof(gic));
        panic(
            "SwitchJoyCon: UART-%c interrupt lost (INTID %u, %s): LSR %#x IER %#x MCR %#x tx %u/%u dma %u ring %u/%u, "
            "%s",
            s.cfg->uart, s.cfg->intid, why, lsr, rd(s, IER), rd(s, MCR), s.txPosition, s.txLength, s.dmaOn,
            s.dmaOn ? tegra_apbdma_moved(s.dma, nullptr) / 4 : 0, s.dmaTaken, gic);
    }
}

// The give-up line's state. step: the furthest handshake step sent (above 0, the Joy-Con did answer); rx: bytes taken
// since the attach and the first of them; irq: UART interrupts since the attach (at least one per packet sent,
// THR-empty); err: framing/overrun/break count. 3M: the same at 3 Mbaud (step 3 on; "sent" counts packets, 91/11
// resends included). late: bytes taken while backing off after a failed attempt (a reply that came too late). then:
// what the failing step got after it was first sent (none of it a reply it accepts). UART: the
// registers now, back at 1 Mbaud; LSR 0x60 = transmitter empty (every byte left), MSR bit 4 = CTS.
// Pads TX/RX/RTS/CTS: "in" is the line level (TX idles 0, inverted; RX idles 1 when a Joy-Con drives it; RTS 1 lets
// the Joy-Con send, deku), "cnf" 0 = UART mode, then the pinmux words. det: the detect pin, on the TX net, so 0 while
// TX idles low; chg/5V: charge-enable and PA5 driven high. PMC: the UART pads' deep power-down, no-IO-power and 3.3 V
// bits (want 0 0 0 on this 1.8 V rail); clamp: tristated inputs clamped (want 0).
void SwitchJoyConRail::describe(Side &s, char *buffer, size_t size)
{
    const SideConfig *cfg = s.cfg;
    const auto hex = [](char (&text)[sizeof(Side::stepHeard) * 3 + 8], const uint8_t *bytes, uint32_t count,
                        unsigned room) {
        strlcpy(text, "none", sizeof(text));
        const unsigned shown = count < room ? count : room;
        size_t at = 0;
        for (unsigned i = 0; i < shown; ++i) {
            at += static_cast<size_t>(snprintf(text + at, sizeof(text) - at, "%s%02x", i ? " " : "", bytes[i]));
        }
        if (count > shown)
            strlcat(text, " ...", sizeof(text));
    };
    char heard[sizeof(s.stepHeard) * 3 + 8];
    char stepHeard[sizeof(s.stepHeard) * 3 + 8];
    hex(heard, s.heard, s.heardCount, sizeof(s.heard));
    hex(stepHeard, s.stepHeard, s.stepHeardCount, sizeof(s.stepHeard));
    unsigned in[4];
    unsigned cnf[4];
    uint32_t mux[4];
    const uint32_t muxReg[4] = {cfg->muxTx, cfg->muxRx, cfg->muxRts, cfg->muxCts};
    for (unsigned i = 0; i < 4; ++i) {
        in[i] = gpioBit(cfg->txPin + i, GPIO_IN);
        cnf[i] = gpioBit(cfg->txPin + i, GPIO_CNF);
        mux[i] = pinRead(muxReg[i]);
    }
    const auto driven = [this](unsigned pin) {
        return gpioBit(pin, GPIO_CNF) & gpioBit(pin, GPIO_OE) & gpioBit(pin, GPIO_OUT);
    };
    snprintf(
        buffer, size,
        "step %u rx %u [%s] irq %u err %u; 3M sent %u rx %u err %u; late %u; then rx %u [%s]; UART-%c LSR %#x IER %#x "
        "MCR %#x MSR %#x LCR %#x CSR %#x clk %#x on %u rst %u; pads in %u%u%u%u cnf %u%u%u%u mux %#x %#x %#x %#x "
        "det %u chg %u 5V %u; PMC dpd %u noio %u 3v3 %u clamp %u",
        s.furthest, s.heardCount, heard, s.interrupts - s.interruptsAtAttach, s.lineErrors - s.errorsAtAttach,
        s.fastSent, s.fastHeardCount, s.fastErrors - s.fastErrorsAtAttach, s.lateCount, s.stepHeardCount, stepHeard,
        cfg->uart, rd(s, LSR), rd(s, IER), rd(s, MCR), rd(s, MSR), rd(s, LCR), rd(s, IRDA_CSR),
        carRead(cfg->clockSource), flag(carRead(cfg->enbState), cfg->clockBit),
        flag(carRead(cfg->rstState), cfg->clockBit), in[0], in[1], in[2], in[3], cnf[0], cnf[1], cnf[2], cnf[3], mux[0],
        mux[1], mux[2], mux[3], gpioBit(cfg->detectPin, GPIO_IN), driven(cfg->chargePin), driven(kPin5V),
        flag(pmcRead(PMC_IO_DPD_STATUS), kPadUartDpd), flag(pmcRead(PMC_NO_IOPOWER), kPadUartPower),
        flag(pmcRead(PMC_PWR_DET_VAL), kPadUartPower),
        switch_mmio_read32(apbMisc, PP_PINMUX_GLOBAL) & kClampTristatedInputs);
}

// The pads both rails share (see the header): UART pads powered and out of deep power-down (a bounded handshake, as
// Linux tegra_io_pad_power_enable and TegraDisplayPower), tristated inputs unclamped. Each is touched only when wrong;
// a change means the boot chain left the rails deaf and is worth one line. The pad voltage (PWR_DET_VAL) is only
// reported: hekate's working rail never sets it. At start and after SC7.
void SwitchJoyConRail::preparePads(const char *when)
{
    char fixed[96] = "";
    if (pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd) {
        (void)switch_pmc_io_pad_on(kPadUartDpdBit);
        strlcat(fixed,
                pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd ? " deep power-down stuck on," : " deep power-down off,",
                sizeof(fixed));
    }
    if (pmcRead(PMC_NO_IOPOWER) & kPadUartPower) {
        pmcWrite(PMC_NO_IOPOWER, pmcRead(PMC_NO_IOPOWER) & ~kPadUartPower);
        (void)pmcRead(PMC_NO_IOPOWER);
        strlcat(fixed, " IO power on,", sizeof(fixed));
    }
    if (switch_mmio_read32(apbMisc, PP_PINMUX_GLOBAL) & kClampTristatedInputs) {
        switch_mmio_write32_flush(apbMisc, PP_PINMUX_GLOBAL,
                                  switch_mmio_read32(apbMisc, PP_PINMUX_GLOBAL) & ~kClampTristatedInputs);
        strlcat(fixed, " tristate input clamp off,", sizeof(fixed));
    }
    if (!fixed[0])
        return;
    fixed[strlen(fixed) - 1] = 0;
    IOLog("SwitchJoyCon: rail pads were not usable at %s, fixed:%s\n", when, fixed);
}

// Pinmux (detect and charge pins are GPIOs TegraGPIO does not mux; TX and RTS with input receivers on, see the
// header), the RX/RTS/CTS pads in UART mode (TX follows detection), and the UART clock: reset, gate off, PLLP
// source, gate on, out of reset (hekate clock_enable), then the line inversions at once, so TX idles low from here.
// At start and after SC7.
void SwitchJoyConRail::initHardware(Side &s)
{
    const SideConfig *cfg = s.cfg;
    pinWrite(cfg->muxTx, PIN_INPUT);
    pinWrite(cfg->muxRx, PIN_INPUT | PIN_TRISTATE);
    pinWrite(cfg->muxRts, cfg->rtsPull | PIN_INPUT);
    pinWrite(cfg->muxCts, PIN_INPUT | PIN_TRISTATE);
    pinWrite(cfg->muxDetect, (pinRead(cfg->muxDetect) & PIN_KEEP_FUNCTION) | PIN_INPUT | PIN_TRISTATE);
    pinWrite(cfg->muxCharge, cfg->chargeMux);
    (void)pinRead(cfg->muxCharge);
    for (unsigned pad = 1; pad < 4; ++pad)
        gpioMask(cfg->txPin + pad, GPIO_MSK_CNF, false);

    carWrite(cfg->rstSet, cfg->clockBit);
    carWrite(cfg->enbClr, cfg->clockBit);
    carWrite(cfg->clockSource, kUartDivEnable | kDiv1M);
    carWrite(cfg->enbSet, cfg->clockBit);
    IODelay(2);
    carWrite(cfg->rstClr, cfg->clockBit);
    wr(s, IER, 0);
    wr(s, IRDA_CSR, CSR_INVERT_TXD | CSR_INVERT_RTS);
}

void SwitchJoyConRail::setUp(Side &s, const SideConfig *cfg)
{
    s.owner = this;
    s.cfg = cfg;
    s.uart = uarts + cfg->uartOffset;
    s.lock = IOSimpleLockAlloc();
    s.outLock = IOLockAlloc();
    if (!s.lock || !s.outLock)
        panic("SwitchJoyCon: no memory for the UART-%c locks", cfg->uart);
    initHardware(s);

    IOService *uartNub = switch_interrupt_nub(cfg->intid, false);
    s.irq = IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchJoyConRail::uartAction,
                                                                     &SwitchJoyConRail::uartFilter, uartNub, 0);
    if (!s.irq || loop->addEventSource(s.irq) != kIOReturnSuccess) {
        panic("SwitchJoyCon: cannot attach the UART-%c interrupt (INTID %u)", cfg->uart, cfg->intid);
    }
    IOService *detectNub = tegra_gpio_interrupt_nub(cfg->detectPin, TEGRA_GPIO_EDGE_FALLING);
    if (!detectNub) {
        char state[256];
        tegra_gpio_describe(cfg->detectPin, state, sizeof(state));
        panic("SwitchJoyCon: no interrupt nub for the Joy-Con (%c) detect pin: %s", cfg->letter, state);
    }
    s.detect = IOInterruptEventSource::interruptEventSource(this, &SwitchJoyConRail::detectAction, detectNub, 0);
    s.timer = IOTimerEventSource::timerEventSource(this, &SwitchJoyConRail::timerAction);
    if (!s.detect || !s.timer || loop->addEventSource(s.detect) != kIOReturnSuccess ||
        loop->addEventSource(s.timer) != kIOReturnSuccess) {
        panic("SwitchJoyCon: cannot attach the Joy-Con (%c) detect interrupt (GPIO %u)", cfg->letter, cfg->detectPin);
    }
    s.irq->enable();

    // RX DMA (header "RX by APB DMA"): the rail works by interrupt per FIFO trigger without it.
    char why[128] = "no DMA-able ring";
    if (tegra_apbdma_alloc(kDmaRingBytes, &s.dmaRing)) {
        s.dma = tegra_apbdma_open(cfg->uart == 'B' ? TEGRA_APBDMA_CH_UARTB_RX : TEGRA_APBDMA_CH_UARTC_RX, loop, nullptr,
                                  nullptr, why, sizeof(why));
    }
    if (!s.dma)
        IOLog("SwitchJoyCon: UART-%c RX DMA off (%s): RX by interrupt per FIFO trigger\n", cfg->uart, why);
}

// Work loop. A running Joy-Con is sent to sleep first (L4T joycon_serdev_suspend: joycon_set_hci_state(0), which it
// does not answer); the THR-empty interrupt tops the FIFO up and the 61-byte packet leaves in about 0.2 ms, so waiting for
// the transmitter to empty is a bounded handshake.
void SwitchJoyConRail::quiesce(Side &s)
{
    if (s.phase == kRunning) {
        const uint8_t sleep = 0;
        sendSubcommand(s, kSubHciState, &sleep, 1);
        for (unsigned waited = 0; waited < 1000 && !(rd(s, LSR) & LSR_TEMT); waited += 10)
            IODelay(10);
    }
    s.timer->cancelTimeout();
    s.detect->disable();
    s.irq->disable();
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(s.lock);
    stopDmaLocked(s);
    s.ier = 0;
    wr(s, IER, 0);
    s.txLength = s.txPosition = 0;
    IOSimpleLockUnlockEnableInterrupt(s.lock, state);
    dropHid(s);
    setCharging(s, false);
    s.chargeKnown = false;  // the charge-enable GPIO is driven again on resume
    s.phase = kSuspended;
}

// With the display still off the rail stays paused; displayPower(true) starts detection.
void SwitchJoyConRail::wake(Side &s)
{
    initHardware(s);
    s.irq->enable();
    if (!displayOff)
        enterDetect(s);
}

void SwitchJoyConRail::suspend()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            if (self->suspended)
                return kIOReturnSuccess;
            for (Side &s : self->sides)
                SwitchJoyConRail::quiesce(s);
            self->poll->cancelTimeout();
            self->polling = false;
            self->suspended = true;
            return kIOReturnSuccess;
        },
        this);
}

void SwitchJoyConRail::resume()
{
    loop->runAction(
        [](OSObject *owner, void *, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            if (!self->suspended)
                return kIOReturnSuccess;
            self->suspended = false;
            self->preparePads("resume");
            for (Side &s : self->sides)
                self->wake(s);
            return kIOReturnSuccess;
        },
        this);
}

void SwitchJoyConRail::displayPower(bool on)
{
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            const bool on = arg != nullptr;
            if (self->displayOff == !on)
                return kIOReturnSuccess;
            self->displayOff = !on;
            if (self->suspended)
                return kIOReturnSuccess;  // SC7 already stopped everything; resume() looks at displayOff
            for (Side &s : self->sides) {
                if (on) {
                    self->enterDetect(s);
                } else {
                    SwitchJoyConRail::pause(s);
                }
            }
            if (!on) {
                self->poll->cancelTimeout();
                self->polling = false;
            }
            return kIOReturnSuccess;
        },
        this, on ? this : nullptr);
}

// The address to pair to (see the header). A Joy-Con running unpaired, or paired to an address that changed (only when
// SwitchBluetooth adopts the staged one), ends its session at its next tick (endSession).
void SwitchJoyConRail::bluetoothAddress(const uint8_t address[6])
{
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            const uint8_t *address = static_cast<const uint8_t *>(arg);
            const bool changed = !self->hostKnown || memcmp(self->host, address, 6);
            memcpy(self->host, address, 6);
            self->hostKnown = true;
            for (Side &s : self->sides)
                s.pairPending |= self->pairing && changed;
            return kIOReturnSuccess;
        },
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): runAction's arguments are void *; the action reads it
        this, const_cast<uint8_t *>(address));
}

bool SwitchJoyConRail::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    car = switch_mmio_map(kCarPA, 0x1000);
    gpio = switch_mmio_map(kGpioPA, 0x1000);
    apbMisc = switch_mmio_map(kApbMiscPA, 0x1000);
    pinmux = switch_mmio_map(kPinmuxPA, 0x1000);
    uarts = switch_mmio_map(kUartPA, 0x1000);
    pmc = switch_mmio_map(kPmcPA, 0x1000);
    loop = IOWorkLoop::workLoop();
    if (!car || !gpio || !apbMisc || !pinmux || !uarts || !pmc || !loop) {
        panic("SwitchJoyCon: cannot map CAR/GPIO/APB_MISC/pinmux/UART/PMC or make a work loop");
    }

    pairing = !bluetoothDisabled();
    poll = IOTimerEventSource::timerEventSource(this, &SwitchJoyConRail::pollAction);
    if (!poll || loop->addEventSource(poll) != kIOReturnSuccess)
        panic("SwitchJoyCon: cannot attach the rail clock");
    sessions = IOInterruptEventSource::interruptEventSource(this, &SwitchJoyConRail::sessionAction);
    if (!sessions || loop->addEventSource(sessions) != kIOReturnSuccess) {
        panic("SwitchJoyCon: cannot attach the HID session event source");
    }

    preparePads("start");
    for (unsigned i = 0; i < 2; ++i)
        setUp(sides[i], &kSides[i]);
    for (Side &s : sides) {
        loop->runAction(
            [](OSObject *owner, void *side, void *, void *, void *) -> IOReturn {
                static_cast<SwitchJoyConRail *>(owner)->enterDetect(*static_cast<Side *>(side));
                return kIOReturnSuccess;
            },
            this, &s);
    }
    IOLog("SwitchJoyCon: rail driver started%s\n", pairing ? " (Bluetooth pairing on attach)" : "");
    return true;
}

void SwitchJoyConRail::statsLine(char *buffer, size_t size) const
{
    size_t used = 0;
    buffer[0] = 0;
    for (const Side &s : sides) {
        const int n = snprintf(buffer + used, size - used,
                               "%s%c running=%d dma=%s replies=%u late=%u tails=%u errors=%u breaks=%u overflows=%u "
                               "dropped=%u silences=%u recoveries=%u republished=%u gamepad=%d published=%u idled=%u "
                               "requests=%u interrupts=%u unsolicited=%u forwarded=%u suppressed=%u quiet=%d "
                               "deadzone=%u calibrated=%d",
                               used ? "; " : "", s.cfg->letter, s.phase == kRunning ? 1 : 0, s.dmaOn ? "on" : "off",
                               s.replies, s.dmaLate, s.dmaTails, s.lineErrors, s.breaks, s.overflows, s.dropped,
                               s.silences, s.recoveries, s.republished, s.hid ? 1 : 0, gamepadsPublished, gamepadsIdled,
                               s.requests, s.interrupts, s.unsolicited, s.forwarded, s.suppressed,
                               s.cadence.quiet ? 1 : 0, s.stick.dead_zone, s.stickKnown ? 1 : 0);
        if (n < 0 || static_cast<size_t>(n) >= size - used)
            break;
        used += static_cast<size_t>(n);
    }
}

#if DEVELOPMENT || DEBUG
int SwitchJoyConRail::probeStart()
{
    int result = ENODEV;
    loop->runAction(
        [](OSObject *owner, void *out, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            int &result = *static_cast<int *>(out);
            for (const Side &s : self->sides) {
                if (s.probe.active)
                    result = EBUSY;
            }
            for (Side &s : self->sides) {
                if (result != EBUSY && s.phase == kRunning) {
                    switch_joycon_probe_start(&s.probe);
                    result = 0;
                }
            }
            return kIOReturnSuccess;
        },
        this, &result);
    return result;
}

void SwitchJoyConRail::probeState(char *buffer, size_t size)
{
    loop->runAction(
        [](OSObject *owner, void *out, void *room, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            const switch_joycon_probe *probes[2] = {&self->sides[0].probe, &self->sides[1].probe};
            const char letters[2] = {self->sides[0].cfg->letter, self->sides[1].cfg->letter};
            switch_joycon_probe_state(probes, letters, 2, static_cast<char *>(out), *static_cast<size_t *>(room));
            return kIOReturnSuccess;
        },
        this, buffer, &size);
}
#endif

static SwitchJoyConRail *gRail;  // set once at start, for switch_joycon_suspend/resume

#if DEVELOPMENT || DEBUG
static int statsSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    char line[1024] = "";
    if (const SwitchJoyConRail *rail = __atomic_load_n(&gRail, __ATOMIC_ACQUIRE))
        rail->statsLine(line, sizeof(line));
    return SYSCTL_OUT(req, line, strlen(line) + 1);
}

// Any write starts the probe; a read is its state (SwitchJoyConRail::probeState).
static int probeSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    constexpr size_t kSize = 8192;
    SwitchJoyConRail *rail = __atomic_load_n(&gRail, __ATOMIC_ACQUIRE);
    if (!rail)
        return ENODEV;
    if (req->newptr) {
        if (const int error = rail->probeStart())
            return error;
    }
    auto *line = static_cast<char *>(IOMallocData(kSize));
    if (!line)
        return ENOMEM;
    rail->probeState(line, kSize);
    const int error = SYSCTL_OUT(req, line, strlen(line) + 1);
    IOFreeData(line, kSize);
    return error;
}

static void registerStats()
{
    static struct sysctl_oid probe;
    if (!probe.oid_name) {
        switch_sysctl_oid(&probe, &sysctl__debug_children, "switch_joycon_probe",
                          static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED), probeSysctl,
                          "A", "rail Joy-Con protocol probe: write to start, read for its state and results");
        sysctl_register_oid(&probe);
    }
    static struct sysctl_oid oid;
    if (oid.oid_name)
        return;
    switch_sysctl_oid(
        &oid, &sysctl__kern_children, "switch_joycon", static_cast<int>(CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED),
        statsSysctl, "A",
        "each rail: running, RX DMA, and since boot its replies, late and tail takes, "
        "line errors, breaks, overflows, dropped packets, silences, recoveries, republished devices, "
        "requests, UART interrupts, unsolicited packets, reports forwarded and held back; its cadence and "
        "stick dead zone");
    sysctl_register_oid(&oid);
}
#endif

extern "C" int switch_joycon_start(void)
{
    static bool started;
    if (started)
        return 0;
    started = true;
    // First, so whichever game-controller personalities serve are settled before any controller can appear.
    switch_joycon_hid_select_personalities();
    IOService *resources = IOService::getResourceService();
    // Before any Joy-Con device exists, so the personalities are in the catalogue when bluetoothd or the rail
    // publishes one.
    switch_joycon_hid_prepare();
    auto *driver = OSTypeAlloc(SwitchJoyConRail);
    if (!resources || !driver || !driver->init() || !driver->attach(resources) || !driver->start(resources)) {
        panic("SwitchJoyCon: rail driver attach or start failed");
    }
    __atomic_store_n(&gRail, driver, __ATOMIC_RELEASE);
    driver->release();  // the attachment keeps it
#if DEVELOPMENT || DEBUG
    registerStats();
#endif
    return 0;
}

extern "C" void switch_joycon_suspend(void)
{
    if (gRail)
        gRail->suspend();
}

extern "C" void switch_joycon_resume(void)
{
    if (gRail)
        gRail->resume();
}

extern "C" void switch_joycon_display_power(bool on)
{
    if (gRail)
        gRail->displayPower(on);
}

extern "C" void switch_joycon_bluetooth_address(const uint8_t address[6])
{
    if (gRail)
        gRail->bluetoothAddress(address);
}
