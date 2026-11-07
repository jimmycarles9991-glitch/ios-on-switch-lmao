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
//   and drops HID mode after ~2 s without host packets (hekate 1257-1261), so the host asks every 15 ms (J:1809;
//   hekate 1059-1074): the next queued output report, else 1F. That clock is the protocol, not a poll of hardware
//   state: every reply is taken by interrupt. One clock (`poll`) serves both rails, so two seated Joy-Cons cost one
//   wakeup per 15 ms, and it runs only while a Joy-Con is running: an empty rail, or a rail paused with the display,
//   has no timer at all. Rumble (output report 0x10) goes out on the reply, as L4T's rumble worker does (J:1132-1138,
//   1871-1905): right after the Joy-Con answers a request, the newest rumble report is sent if there is one it has not
//   had, or again every 50 ms while it asks for a non-zero amplitude (L4T's keep-alive, J:1845-1851: a Joy-Con stops
//   a vibration that is not refreshed). The request stays every 15 ms, so input never depends on whether the Joy-Con
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
// interrupt. A missing reply is either the Joy-Con gone (the line is silent: detach after 500 ms, J:1793) or a lost
// interrupt (bytes sit in the FIFO with RX interrupts enabled, or the TX FIFO is empty with THRI enabled): that
// panics in one line with the UART registers and the GIC state. A rail waiting for an attach arms nothing but the
// detect pin's interrupt; tegra_gpio_check_delivery() (as SwitchButtons) looks at that pin when the display goes off,
// not on a timer. Every packet also takes one THR-empty interrupt, so the interrupt count proves the UART's line
// reaches the CPU even when nothing comes back.
// Giving up on an attach logs one line of rail state (describe); a running Joy-Con that falls silent logs one line
// with the number of input reports it sent.
//
// RX by APB DMA while running (TegraAPBDMA.h): by FIFO trigger, a running Joy-Con's reply (~61 bytes of 0x30 report
// every 15 ms per rail) takes about eight interrupts, around 1000 a second on CPU0 for two rails. While a rail
// runs, the UART is in DMA mode as Linux serial-tegra puts it (FCR DMA select, 4-byte RX trigger, IER RLSI | RTOIE |
// EORD and no RDI): channel 0 (UART-B) / 1 (UART-C) moves every 4 bytes into a 4 KiB ring, continuously, and the reply
// takes one interrupt, EORD or RX timeout once the line has been idle for 4 characters. The filter then takes what the
// DMA put in the ring (cache lines invalidated first) and, by PIO, the 1-3 bytes below the burst still in the FIFO
// (serial-tegra tegra_uart_rx_buffer_push + tegra_uart_handle_rx_pio), into the same ring and framing as PIO, and
// wakes the action once. Reading the FIFO while the channel runs is safe here because a Joy-Con only speaks when spoken
// to: nothing arrives between its reply and the next request; should the ring move during that read anyway, the order
// of those bytes is unknown and they count as a line error (the packet is dropped and the stream resynchronises, as
// for any line error). TX stays PIO (a request fits the FIFO: one THR-empty interrupt). Handshake and pairing keep PIO,
// so their give-up lines see every byte. The DMA's end-of-count at every wrap is counted: more wraps than the bytes
// taken account for means the ring was overrun, data lost, and the action panics with one line; bytes in the ring that
// no interrupt took, after 50 ms, are a lost interrupt (checkDelivery). Non-fatal checks: a channel that does not open,
// a first reply that does not start with the reply magic, or 4 bytes or more left in the FIFO with the ring not moving
// turn DMA off for that rail with one log line; it keeps working by interrupt per FIFO trigger.
//
// Pairing (unless switch-no-bt): the address to pair to is the one SwitchBluetooth programs
// into the controller for bluetoothd: the unit's own (fuses, as switchroot's U-Boot derives it) or, when deploy staged
// hekate's "Dump Joy-Con BT" (switchroot/joycon_mac.ini), Horizon's, as switchroot's U-Boot takes it from joycon_mac.bin
// (SwitchBluetooth.cpp). SwitchBluetooth announces it once Data is mounted, before launchd (and again, unchanged, at
// bluetoothd's first connect), after the rails handshake the Joy-Cons seated at boot, so a Joy-Con that attaches before
// it runs unpaired and is paired the moment it arrives (switch_joycon_bluetooth_address: its HID device goes away for
// the pairing, about 0.1 s, and comes back, before anything has opened it); later attaches pair before the HID device
// is published. Pairing reads the Joy-Con's pairing log (SPI 0x2000, 0x26-byte
// slots: magic, size 0x22, checksum, host BD_ADDR display order, LTK; hekate 1084-1245, deku spi_flash_notes.md 44-76):
// earlier pairings have magic 0x00, the one in use 0x95, erased flash ends it. If the slot in use names this address,
// its key is reused and nothing is written (a Joy-Con paired in Horizon to the address SwitchBluetooth adopted); else
// the Joy-Con is paired over the rail with subcommand 0x01 (deku bluetooth_hid_subcommands_notes.md 13-52): 01 <host
// BD_ADDR LSB first> -> Joy-Con BD_ADDR; 02 -> LTK, each byte XOR 0xAA; 03 -> saved, which makes it the slot in use
// (Horizon pairs it back the next time it is attached there). The result is published as a SwitchJoyConPairing
// registry entry (Address, LinkKey in HCI order, Name, ClassOfDevice, ProductID) for joyconpaird (helper/), which
// hands it to bluetoothd: pulled off the rail, the Joy-Con pages this address and reconnects over Bluetooth.
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
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraAPBDMA/TegraAPBDMA.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "SwitchJoyCon.h"
#include "SwitchJoyConHID.h"
#include "SwitchJoyConForward.h"
#include "SwitchJoyConSession.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

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
    FCR_RX_TRIG_4 = 0x40,  // the DMA's one-word burst (serial-tegra, chips without 8-byte-only bursts)
    FCR_RX_TRIG_8 = 0x80
};

constexpr uint32_t kDmaRingBytes = 4096;

enum : uint32_t {
    LCR_8N1 = 0x03,
    LCR_2STOP = 0x04,
    LCR_DLAB = 0x80,
    MCR_RTS_EN = 0x40
};

enum : uint32_t {
    LSR_DR = 0x01,
    LSR_ERRORS = 0x1e,
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
    PMC_IO_DPD_REQ = 0x5b8,
    PMC_IO_DPD_STATUS = 0x5bc
};

constexpr uint32_t kPadUartPower = 1u << 2, kPadUartDpd = 1u << 14, kDpdOff = 1u << 30;
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
    kSubReportMode = 0x03,
    kSubHciState = 0x06,
    kSubSpiRead = 0x10
};

enum : uint8_t {
    kModeSimpleHid = 0x3f
};

// Largest input report: 0x31 (NFC/IR MCU), 362 bytes with the ID, 374 as a rail packet; room is left beyond it.
constexpr size_t kHeader = 12, kMaxOutput = 64, kMaxTx = kHeader + kMaxOutput, kMaxRx = kHeader + 0x200;
constexpr size_t kMinPacket = 7;      // magic, size, cmd, subcmd
constexpr size_t kStandardFull = 49;  // report 0x30 with its ID; also the head of every 0x31
constexpr uint8_t kReplyMagic[3] = {0x19, 0x81, 0x03};
constexpr uint8_t kRumbleNeutral[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

// Timing (J: 1709 retries, 1748 settle, 1793 silence, 1809 interval; hekate 1257 handshake reply).
constexpr uint32_t kSettleMS = 64, kHandshakeMS = 100, kCommandMS = 250, kRetryMS = 100, kAttempts = 30;
constexpr uint32_t kSubcommandMS = 1000, kIntervalMS = 15, kSilenceMS = 500;
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
constexpr uint8_t kSlotRead = 0x1a, kSlotMagic = 0x95, kSlotSize = 0x22;
constexpr uint32_t kJoyConClass = 0x000508;  // Peripheral / Gamepad, as switchroot's BlueZ record

uint8_t crc8(const uint8_t *data, size_t length)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x8d) : (uint8_t)(crc << 1);
    }
    return crc;
}

// One host packet; returns its length. `out` has room for kHeader + length.
size_t buildPacket(uint8_t *out, uint8_t cmd, uint8_t subcmd, const uint8_t *payload, uint16_t length)
{
    const uint16_t rest = (uint16_t)(7 + length);
    const uint8_t header[kHeader] = {0x19,
                                     0x01,
                                     0x03,
                                     (uint8_t)rest,
                                     (uint8_t)(rest >> 8),
                                     cmd,
                                     subcmd,
                                     (uint8_t)length,
                                     (uint8_t)(length >> 8),
                                     0,
                                     length ? crc8(payload, length) : (uint8_t)0,
                                     0};
    memcpy(out, header, kHeader);
    out[11] = crc8(out + 5, 6);
    if (length)
        memcpy(out + kHeader, payload, length);
    return kHeader + length;
}

uint64_t nowMS()
{
    uint64_t abs, ns;
    clock_get_uptime(&abs);
    absolutetime_to_nanoseconds(abs, &ns);
    return ns / 1000000;
}

bool bluetoothDisabled()
{
    unsigned off = 0;
    return PE_parse_boot_argn("switch-no-bt", &off, sizeof(off)) && off;
}
}  // namespace

// One pairing result for joyconpaird (helper/joyconpaird.c); see SwitchJoyConRail::publishPairing.
class SwitchJoyConPairing : public IOService {
    OSDeclareDefaultStructors(SwitchJoyConPairing)
};

OSDefineMetaClassAndStructors(SwitchJoyConPairing, IOService)

    class SwitchJoyConRail : public IOService {
    OSDeclareDefaultStructors(SwitchJoyConRail)

        enum Phase : uint8_t {
        kSettling,
        kWaiting,
        kBackoff,
        kHandshake,
        kPairing,
        kRunning,
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
        // RX by DMA while running (header "RX by APB DMA"), under `lock`: the channel and its ring (null: PIO only),
        // DMA mode on, the ring offset taken up to, bytes taken since the start, the channel's interrupt count at the
        // start, the first reply checked; a reason to turn it off (the action logs it), an overrun (the action panics),
        // and counts: replies taken, leftover bytes by PIO, ring moves during the leftover read.
        tegra_apbdma_channel *dma;
        tegra_apbdma_buffer dmaRing;
        bool dmaOn, dmaOff, dmaChecked, dmaOverrun;
        const char *dmaFailed;
        uint32_t dmaTaken, dmaLapsAtStart, dmaTakes, dmaPio, dmaRaces;
        uint64_t dmaBytes;
        // Work loop.
        Phase phase;
        uint8_t step, attempts, fastFailures, sends, slot, counter;  // sends: of the current handshake step
        uint32_t errorsSeen, epoch;  // epoch: bumped when the UART is reprogrammed and unread bytes are dropped
        uint64_t lastReply;
        uint32_t reports;  // input reports since the handshake completed
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
        bool charging, chargeKnown;
        bool pairPending;  // running unpaired until SwitchBluetooth gives the address (see the header)
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
        // Work loop: the last packet sent was a rumble report (the next reply does not send another), and when the last
        // rumble report went out.
        bool rumbleSent;
        uint64_t rumbleAt;
    };

    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *poll = nullptr;          // the 15 ms clock of every running Joy-Con; armed only while one runs
    IOInterruptEventSource *sessions = nullptr;  // software interrupt: a device's backboardd session ended
    volatile uint8_t *car = nullptr, *gpio = nullptr, *apbMisc = nullptr, *pinmux = nullptr, *uarts = nullptr,
                     *pmc = nullptr;
    Side sides[2] = {};
    bool pairing = false, nfc = false, suspended = false, displayOff = false, polling = false;
    bool republishOff = false;  // the loop check tripped (see the header)
    bool hostKnown = false;     // `host` is the address SwitchBluetooth programmed
    uint8_t host[6] = {};

    static uint32_t rd(Side &s, uint32_t reg)
    {
        return *(volatile uint32_t *)(s.uart + reg);
    }

    static void wr(Side &s, uint32_t reg, uint32_t value)
    {
        *(volatile uint32_t *)(s.uart + reg) = value;
    }

    void carWrite(uint32_t reg, uint32_t value)
    {
        *(volatile uint32_t *)(car + reg) = value;
    }

    uint32_t carRead(uint32_t reg)
    {
        return *(volatile uint32_t *)(car + reg);
    }

    void pinWrite(uint32_t reg, uint32_t value)
    {
        *(volatile uint32_t *)(pinmux + reg) = value;
    }

    uint32_t pinRead(uint32_t reg)
    {
        return *(volatile uint32_t *)(pinmux + reg);
    }

    void pmcWrite(uint32_t reg, uint32_t value)
    {
        *(volatile uint32_t *)(pmc + reg) = value;
    }

    uint32_t pmcRead(uint32_t reg)
    {
        return *(volatile uint32_t *)(pmc + reg);
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
    bool filter(Side &s);
    void fillTx(Side &s);
    bool drainRx(Side &s);
    static void frameByte(Side &s, uint8_t byte);
    bool takeDma(Side &s);
    void startDmaLocked(Side &s);
    void stopDmaLocked(Side &s);
    void dmaFailedLocked(Side &s, const char *why);

    // Work loop.
    void setUp(Side &s, const SideConfig *cfg);
    void preparePads(const char *when);
    void initHardware(Side &s);
    void quiesce(Side &s);
    void wake(Side &s);
    void configureUart(Side &s, bool fast);
    void send(Side &s, const uint8_t *bytes, size_t length);
    bool txIdle(Side &s);
    void sendCommand(Side &s, uint8_t subcmd, const uint8_t *payload = nullptr, uint16_t length = 0);
    void sendReport(Side &s, const uint8_t *report, size_t length);
    void sendOutput(Side &s, uint8_t *report, size_t length);
    void sendSubcommand(Side &s, uint8_t id, const uint8_t *data, size_t length);
    void enterDetect(Side &s);
    void attachJoyCon(Side &s);
    void startAttempt(Side &s);
    void failAttempt(Side &s, const char *why);
    void sendStep(Side &s);
    void startPairing(Side &s);
    void sendPairStep(Side &s);
    void finishPairing(Side &s, bool paired, const char *how);
    void run(Side &s);
    IOService *createHid(Side &s);
    void dropHid(Side &s);
    void republish(Side &s);
    void armPoll();
    void tick(Side &s);
    void rumbleAfterReply(Side &s);
    void detach(Side &s);
    void pause(Side &s);
    void received(Side &s, const uint8_t *packet, size_t length);
    void handshakeReply(Side &s, uint8_t cmd, uint8_t echo, const uint8_t *payload, size_t length);
    void pairingReply(Side &s, const uint8_t *report, size_t length);
    void runningReport(Side &s, const uint8_t *report, size_t length);
    void setCharging(Side &s, bool on);
    void publishPairing(Side &s, const char *source);
    void checkDelivery(Side &s, const char *why);
    void describe(Side &s, char *buffer, size_t size);

public:
    bool start(IOService *provider) override;
    void suspend();
    void resume();
    void displayPower(bool on);
    void bluetoothAddress(const uint8_t address[6]);
};

OSDefineMetaClassAndStructors(SwitchJoyConRail, IOService)

    void SwitchJoyConRail::gpioMask(unsigned pin, uint32_t reg, bool on)
{
    const uint32_t bit = 1u << (pin & 7);
    const uint32_t at = (pin >> 5) * 0x100 + ((pin >> 3) & 3) * 4 + reg;
    *(volatile uint32_t *)(gpio + at) = (bit << 8) | (on ? bit : 0);
    (void)*(volatile uint32_t *)(gpio + at);
}

// One pin's bit of CNF, OE, OUT or IN (IN follows the pad whenever its input receiver is on, GPIO or UART mode).
unsigned SwitchJoyConRail::gpioBit(unsigned pin, uint32_t reg)
{
    const uint32_t at = (pin >> 5) * 0x100 + ((pin >> 3) & 3) * 4 + reg;
    return (*(volatile uint32_t *)(gpio + at) >> (pin & 7)) & 1;
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
    if (s && self->filter(*s))
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
// serial-tegra's tegra_uart_isr does (RDI written on, then every RX source off: that clears EORD); once IIR is clear the
// ring and the FIFO's leftovers are taken (takeDma) and the RX sources come back.
bool SwitchJoyConRail::filter(Side &s)
{
    bool got = false, idle = false, dmaRx = false;
    IOSimpleLockLock(s.lock);
    s.interrupts = s.interrupts + 1;
    const uint32_t errorsBefore = s.lineErrors, overflowsBefore = s.overflows;
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
                if (((iir >> 1) & 7) == 3 && (rd(s, LSR) & LSR_ERRORS)) {  // line status: the read clears it
                    s.lineErrors = s.lineErrors + 1;
                    s.fastErrors = s.fastErrors + 1;
                }
                break;
            }
            idle |= ((iir >> 1) & 7) == 6;  // RX timeout
            got |= drainRx(s);
            break;
        }
    }
    if (dmaRx && !s.stuck) {
        idle = true;  // EORD and RX timeout come with the line idle: the end of a reply
        got |= takeDma(s);
        if (s.dmaOn)
            wr(s, IER, s.ier);  // a failed check left PIO's sources (dmaFailedLocked)
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
        s.frameTotal = 5 + (s.frameSizeLow | (uint32_t)byte << 8);
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

bool SwitchJoyConRail::drainRx(Side &s)
{
    bool got = false;
    for (;;) {
        const uint32_t lsr = rd(s, LSR);
        if (lsr & LSR_ERRORS) {
            s.lineErrors = s.lineErrors + 1;
            if (s.fast)
                s.fastErrors = s.fastErrors + 1;
        }
        if (!(lsr & LSR_DR))
            break;
        const uint8_t byte = (uint8_t)rd(s, RBR_THR);
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

// `lock` held, DMA mode, RX interrupts masked (filter): what the channel put in the ring since the last take (its cache
// lines invalidated first: the engine does not snoop), then the bytes below its 4-byte burst still in the FIFO, by PIO
// (drainRx), all into `ring` with the same framing. More end-of-counts (wraps) than the bytes taken account for is an
// overrun (the action panics). The non-fatal DMA checks (header) turn DMA off for the rail (dmaFailedLocked), which
// loses nothing: the PIO drain takes whatever the FIFO holds.
bool SwitchJoyConRail::takeDma(Side &s)
{
    const uint32_t size = s.dmaRing.size;
    const uint32_t at = tegra_apbdma_moved(s.dma, nullptr) % size;
    const uint32_t count = (at + size - s.dmaTaken) % size;
    const uint32_t laps = tegra_apbdma_interrupts(s.dma) - s.dmaLapsAtStart;
    if (laps > (s.dmaBytes + count) / size) {
        s.dmaOverrun = true;
        return true;
    }
    bool got = false;
    if (count) {
        const uint32_t from = s.dmaTaken;
        for (uint32_t done = 0; done < count;) {
            const uint32_t start = (from + done) % size,
                           run = count - done < size - start ? count - done : size - start;
            for (uintptr_t line = (uintptr_t)(s.dmaRing.va + start) & ~(uintptr_t)63,
                           end = (uintptr_t)(s.dmaRing.va + start + run);
                 line < end; line += 64)
                __asm__ volatile("dc civac, %0" ::"r"(line) : "memory");
            done += run;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        if (!s.dmaChecked) {
            // The first reply since the start begins at offset 0: the reply magic, after any zeros (uartAction's
            // "Obird workaround"). Anything else: the channel's position or data are not what the driver thinks.
            s.dmaChecked = true;
            uint32_t i = 0;
            while (i < count && !s.dmaRing.va[i])
                ++i;
            if (from != 0 || count - i < 3 || memcmp(s.dmaRing.va + i, kReplyMagic, 3)) {
                dmaFailedLocked(s, "the first reply by DMA did not start with the reply magic");
                return drainRx(s);
            }
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t byte = s.dmaRing.va[(from + i) % size];
            const uint32_t next = (s.head + 1) % sizeof(s.ring);
            if (next == s.tail) {
                s.overflows = s.overflows + 1;
                continue;
            }
            s.ring[s.head] = byte;
            s.head = next;
            frameByte(s, byte);
        }
        s.dmaTaken = at;
        s.dmaBytes += count;
        s.dmaTakes = s.dmaTakes + 1;
        got = true;
    }
    const uint32_t before = s.head;
    got |= drainRx(s);
    const uint32_t pio = (s.head + sizeof(s.ring) - before) % sizeof(s.ring);
    s.dmaPio += pio;
    if (pio >= 4 && !count) {
        dmaFailedLocked(s, "4 or more bytes waited in the FIFO and the DMA took none");
    } else if (tegra_apbdma_moved(s.dma, nullptr) % size != at) {
        s.dmaRaces = s.dmaRaces + 1;  // the ring moved while the FIFO was read: their order is unknown
        s.lineErrors = s.lineErrors + 1;
        s.fastErrors = s.fastErrors + 1;
    }
    return got;
}

// `lock` held, work loop (run): the UART into DMA mode with the ring from offset 0. What the FIFO holds is taken by
// PIO first.
void SwitchJoyConRail::startDmaLocked(Side &s)
{
    if (!s.dma || s.dmaOff || s.dmaOn)
        return;
    (void)drainRx(s);
    s.dmaTaken = 0;
    s.dmaBytes = 0;
    s.dmaChecked = false;
    s.dmaLapsAtStart = tegra_apbdma_interrupts(s.dma);
    for (uintptr_t line = (uintptr_t)s.dmaRing.va, end = line + s.dmaRing.size; line < end; line += 64)
        __asm__ volatile("dc civac, %0" ::"r"(line) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
    const unsigned request = s.cfg->uart == 'B' ? TEGRA_APBDMA_REQ_UARTB : TEGRA_APBDMA_REQ_UARTC;
    tegra_apbdma_start_ring(s.dma, (uint32_t)(kUartPA + s.cfg->uartOffset + RBR_THR), request, s.dmaRing.pa,
                            s.dmaRing.size);
    wr(s, IIR_FCR, FCR_FIFO | FCR_DMA | FCR_RX_TRIG_4 | FCR_TX_TRIG_16);
    s.ier = (s.ier & IER_THRI) | IER_RX_DMA;
    wr(s, IER, s.ier);
    s.dmaOn = true;
}

// `lock` held: back to PIO (RX interrupt per FIFO trigger), the channel stopped. Unread ring bytes are the caller's to
// drop (leaving the running phase) or already taken.
void SwitchJoyConRail::stopDmaLocked(Side &s)
{
    if (!s.dmaOn)
        return;
    wr(s, IIR_FCR, FCR_FIFO | FCR_RX_TRIG_8 | FCR_TX_TRIG_16);
    tegra_apbdma_stop(s.dma);
    s.dmaOn = false;
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
        panic("SwitchJoyCon: UART-%c RX DMA overran its ring (data lost): taken %llu bytes at %u, %s", s->cfg->uart,
              s->dmaBytes, s->dmaTaken, dma);
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
            s->frameLength = (uint32_t)s->packetLength;
            s->frameSizeLow = s->packetLength > 3 ? s->packet[3] : 0;
            s->frameTotal = s->packetLength > 4 ? 5 + (s->packet[3] | (uint32_t)s->packet[4] << 8) : 0;
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
            const size_t total = 5 + (s->packet[3] | (size_t)s->packet[4] << 8);
            if (total < kMinPacket || total > sizeof(s->packet)) {
                if (total > sizeof(s->packet) && !s->oversizeLogged) {
                    s->oversizeLogged = true;
                    IOLog("SwitchJoyCon: Joy-Con (%c) sent a %zu-byte rail packet (cmd %#x), dropped\n", s->cfg->letter,
                          total, s->packet[5]);
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
    s.lastReply = nowMS();
    const uint8_t cmd = packet[5];
    switch (s.phase) {
    case kHandshake:
        handshakeReply(s, cmd, packet[6], packet + kHeader, length > kHeader ? length - kHeader : 0);
        break;
    case kPairing:
        if (cmd == kCmdExtRet && length > kHeader)
            pairingReply(s, packet + kHeader, length - kHeader);
        break;
    case kRunning:
        if (cmd == kCmdExtRet && length > kHeader)
            runningReport(s, packet + kHeader, length - kHeader);
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
        s.txLength = (uint32_t)length;
        s.txPosition = 0;
        fillTx(s);
        // THR-empty armed even when the packet fit: its interrupt (fillTx disarms it) is the proof, counted in
        // `interrupts`, that this UART's line reaches the CPU while the Joy-Con says nothing.
        s.ier |= IER_THRI;
        wr(s, IER, s.ier);
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
    send(s, packet, buildPacket(packet, kCmdExtSend, subcmd, payload, length));
}

void SwitchJoyConRail::sendReport(Side &s, const uint8_t *report, size_t length)
{
    uint8_t packet[kHeader + kMaxOutput];
    send(s, packet, buildPacket(packet, kCmdExtRet, 0, report, (uint16_t)length));
}

// An output report from the HID device (0x01 subcommand, 0x10 rumble, 0x11/0x12 MCU) with the rail's packet counter in
// byte 1 (see the header).
void SwitchJoyConRail::sendOutput(Side &s, uint8_t *report, size_t length)
{
    report[1] = (uint8_t)(s.counter++ & 0xf);
    sendReport(s, report, length);
}

// A subcommand of the driver's own (pairing), framed as the Bluetooth output report 0x01 (J:785-815).
void SwitchJoyConRail::sendSubcommand(Side &s, uint8_t id, const uint8_t *data, size_t length)
{
    uint8_t report[11 + 8] = {kReportSubcommand, (uint8_t)(s.counter++ & 0xf)};
    memcpy(report + 2, kRumbleNeutral, sizeof(kRumbleNeutral));
    report[10] = id;
    memcpy(report + 11, data, length);
    sendReport(s, report, 11 + length);
}

// setReport from the HID device (a user client thread): queue for the next 15 ms slot.
IOReturn SwitchJoyConRail::output(void *context, const uint8_t *report, size_t length)
{
    Side &s = *static_cast<Side *>(context);
    if (length < 2 || length > kMaxOutput)
        return kIOReturnBadArgument;
    IOReturn result = kIOReturnSuccess;
    IOLockLock(s.outLock);
    switch_joycon_forward_output(&s.forward, report, length);
    if (report[0] == kReportRumble) {
        memcpy(s.rumble, report, length);
        s.rumbleLength = (uint8_t)length;
        s.rumbleFresh = true;
    } else if ((uint8_t)(s.queueTail - s.queueHead) == 8) {
        result = kIOReturnNoSpace;
    } else {
        const unsigned at = s.queueTail++ & 7;
        memcpy(s.queue[at], report, length);
        s.queueLength[at] = (uint8_t)length;
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
    if (pairing && hostKnown)
        startPairing(s);
    else
        run(s);
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

// From the attach, or for a running Joy-Con when the address arrives (bluetoothAddress): the HID device goes while the
// pairing subcommands and their replies use the rail, and comes back with run().
void SwitchJoyConRail::startPairing(Side &s)
{
    s.pairPending = false;
    dropHid(s);
    s.phase = kPairing;
    s.step = kReadSlot;
    s.slot = 0;
    sendPairStep(s);
}

void SwitchJoyConRail::sendPairStep(Side &s)
{
    switch (s.step) {
    case kReadSlot: {
        const uint32_t address = kSlotBase + s.slot * kSlotStride;
        const uint8_t read[5] = {(uint8_t)address, (uint8_t)(address >> 8), (uint8_t)(address >> 16),
                                 (uint8_t)(address >> 24), kSlotRead};
        sendSubcommand(s, kSubSpiRead, read, sizeof(read));
        break;
    }
    case kSendHost: {
        uint8_t request[7] = {0x01};
        for (unsigned i = 0; i < 6; ++i)
            request[1 + i] = host[5 - i];  // LSB first (deku)
        sendSubcommand(s, kSubPairing, request, sizeof(request));
        break;
    }
    case kGetKey: {
        const uint8_t request = 0x02;
        sendSubcommand(s, kSubPairing, &request, 1);
        break;
    }
    case kSave: {
        const uint8_t request = 0x03;
        sendSubcommand(s, kSubPairing, &request, 1);
        break;
    }
    }
    s.timer->setTimeoutMS(kSubcommandMS);
}

// Subcommand replies: report 0x21, ACK in byte 13 (bit 7), subcommand id in 14, data from 15 (deku
// bluetooth_hid_notes.md, J:392-412).
void SwitchJoyConRail::pairingReply(Side &s, const uint8_t *report, size_t length)
{
    if (length < 16 || report[0] != kReportReply)
        return;
    const uint8_t id = report[14];
    const uint8_t *data = report + 15;
    const bool ack = report[13] & 0x80;
    if (s.step == kReadSlot && id == kSubSpiRead) {
        // data: address (4), size, then the slot: magic, size, checksum (2), host BD_ADDR, LTK.
        const uint8_t *slot = data + 5;
        if (!ack || length < 15 + 5 + kSlotRead)
            return finishPairing(s, false, "pairing slot read refused");
        const uint32_t address = data[0] | data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        if (address != kSlotBase + s.slot * kSlotStride)
            return;  // a late reply to an earlier read
        // Checksum as hekate _jc_validate_pairing_info: byte sum of the slot from offset 4 (the unread tail is zero except
        // the capability byte, 0x68 for a Switch host, 0x08 for a PC), XOR 0x55. Erased flash fails it: no slot in use.
        uint8_t sum = 0;
        for (unsigned i = 4; i < kSlotRead; ++i)
            sum += slot[i];
        const bool valid = ((uint8_t)(sum + 0x68) ^ 0x55) == slot[2] || ((uint8_t)(sum + 0x08) ^ 0x55) == slot[2];
        if (valid && slot[0] != kSlotMagic && ++s.slot < kSlots)
            return sendPairStep(s);  // an earlier pairing
        if (valid && slot[0] == kSlotMagic && slot[1] == kSlotSize && !memcmp(slot + 4, host, 6)) {
            // Stored most significant byte first; HCI and bluetoothd take the key least significant byte first (hekate
            // 761-762 reverses it for joycon_mac.ini, which BlueZ uses as is).
            for (unsigned i = 0; i < 16; ++i)
                s.key[i] = slot[10 + 15 - i];
            return finishPairing(s, true, "flash");
        }
        // The slot in use names another host (Horizon's address on a console that kept its own), or there is none.
        s.step = kSendHost;
        return sendPairStep(s);
    }
    // Steps 1 and 2 echo their request type in the first data byte (deku); for step 3 the ACK is enough.
    if (id != kSubPairing || !ack || (s.step != kSave && data[0] != (s.step == kSendHost ? 0x01 : 0x02)))
        return;
    if (s.step == kSendHost) {
        s.step = kGetKey;
        return sendPairStep(s);
    }
    if (s.step == kGetKey) {
        // XOR 0xAA, same byte order as the slot it is saved to (deku): reversed for HCI as above.
        if (length < 15 + 1 + 16)
            return finishPairing(s, false, "short pairing key reply");
        for (unsigned i = 0; i < 16; ++i)
            s.key[i] = data[1 + 15 - i] ^ 0xaa;
        s.step = kSave;
        return sendPairStep(s);
    }
    finishPairing(s, true, "rail");
}

void SwitchJoyConRail::finishPairing(Side &s, bool paired, const char *how)
{
    s.timer->cancelTimeout();
    if (paired) {
        publishPairing(s, how);
        // Once per attach: the system log, not the console.
        os_log(OS_LOG_DEFAULT,
               "SwitchJoyCon: Joy-Con (%c) %02x:%02x:%02x:%02x:%02x:%02x paired to %02x:%02x:%02x:%02x:%02x:%02x (%s)",
               s.cfg->letter, s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5], host[0], host[1], host[2],
               host[3], host[4], host[5],
               strcmp(how, "rail") ? "its slot in use, nothing written" : "written over the rail");
    } else {
        IOLog("SwitchJoyCon: Joy-Con (%c) not paired for Bluetooth: %s\n", s.cfg->letter, how);
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
    OSData *mac = OSData::withBytes(s.mac, 6), *key = OSData::withBytes(s.key, 16),
           *hostAddress = OSData::withBytes(host, 6);
    const bool ok = record && mac && key && hostAddress && record->init();
    if (ok) {
        record->setProperty("Address", mac);
        record->setProperty("AddressString", address);
        record->setProperty("LinkKey", key);
        record->setProperty("HostAddress", hostAddress);
        record->setProperty("Name", s.cfg->left ? "Joy-Con (L)" : "Joy-Con (R)");
        record->setProperty("ClassOfDevice", (unsigned long long)kJoyConClass, 32);
        record->setProperty("VendorID", 0x057eull, 32);
        record->setProperty("ProductID",
                            (unsigned long long)(s.cfg->left ? kSwitchJoyConLeftPID : kSwitchJoyConRightPID), 32);
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
    s.lastReply = nowMS();
    s.reports = 0;
    s.errorsAtRun = s.lineErrors;
    s.overflowsAtRun = s.overflows;
    s.hid = createHid(s);
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
void SwitchJoyConRail::dropHid(Side &s)
{
    if (s.hid)
        switch_joycon_hid_destroy(s.hid);
    s.hid = nullptr;
    IOLockLock(s.outLock);
    s.republish = false;
    IOLockUnlock(s.outLock);
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
    const int32_t served = s.servedPid, opener = s.openerPid;
    s.republish = false;
    IOLockUnlock(s.outLock);
    if (!wanted || republishOff || s.phase != kRunning || !s.hid)
        return;
    if (!switch_joycon_republish_allowed(&s.guard, served, nowMS())) {
        republishOff = true;
        IOLog("SwitchJoyCon: Joy-Con (%c): backboardd[%d] ended a second HID session within %u ms; not publishing "
              "Joy-Con HID devices again this boot\n",
              s.cfg->letter, served, (unsigned)kSwitchJoyConRepublishGapMS);
        return;
    }
    dropHid(s);
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

// The shared 15 ms clock: armed while any Joy-Con runs, left to lapse when none does (see the header). Sides that stop
// running elsewhere (detach, pause, pairing) need nothing: the next fire finds them gone and does not re-arm.
void SwitchJoyConRail::armPoll()
{
    polling = false;
    for (Side &s : sides)
        polling |= s.phase == kRunning;
    if (polling)
        poll->setTimeoutMS(kIntervalMS);
}

void SwitchJoyConRail::pollAction(OSObject *owner, IOTimerEventSource *)
{
    auto *self = static_cast<SwitchJoyConRail *>(owner);
    for (Side &s : self->sides) {
        if (s.phase == kRunning)
            self->tick(s);
    }
    self->armPoll();
}

// One request per 15 ms: the oldest queued output report (subcommands keep their order), else the input request. A
// Joy-Con that has said nothing for 500 ms is gone (J:1793).
void SwitchJoyConRail::tick(Side &s)
{
    if (nowMS() - s.lastReply > kSilenceMS) {
        checkDelivery(s, "no reply for 500 ms");
        // Pulled off the rail, or the HID phase does not work (0 reports): either way it is handshaken again if seated.
        IOLog("SwitchJoyCon: Joy-Con (%c) on the rail stopped answering after %u input reports (line errors %u, ring "
              "overflows %u; RX DMA %s: %u replies, %u bytes by PIO, %u ring moves during it)\n",
              s.cfg->letter, s.reports, s.lineErrors - s.errorsAtRun, s.overflows - s.overflowsAtRun,
              s.dmaOn              ? "on"
              : s.dma && !s.dmaOff ? "idle"
                                   : "off",
              s.dmaTakes, s.dmaPio, s.dmaRaces);
        detach(s);
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
    s.rumbleSent = false;
    if (length) {
        sendOutput(s, report, length);
    } else {
        report[0] = kRequestInput;
        sendReport(s, report, 1);
    }
}

// After a reply to a request: the newest rumble report if the Joy-Con has not had it, or the last one again every 50 ms
// while it asks for a non-zero amplitude on this Joy-Con (L4T joycon_send_rumble_data: the left half of the 8 rumble
// bytes drives a Joy-Con (L), the right half a Joy-Con (R); zero amplitude is 0x40000000 under mask 0xFF80FE00, LE).
// A reply to the rumble report itself sends nothing.
void SwitchJoyConRail::rumbleAfterReply(Side &s)
{
    if (s.rumbleSent || !txIdle(s))
        return;
    uint8_t report[kMaxOutput];
    size_t length = 0;
    const uint64_t now = nowMS();
    IOLockLock(s.outLock);
    if (s.rumbleLength >= 10) {
        const uint8_t *half = s.rumble + (s.cfg->left ? 2 : 6);
        const uint32_t word = half[0] | half[1] << 8 | (uint32_t)half[2] << 16 | (uint32_t)half[3] << 24;
        const bool moving = (word & 0xff80fe00u) != 0x40000000u;
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
    sendOutput(s, report, length);
}

void SwitchJoyConRail::runningReport(Side &s, const uint8_t *report, size_t length)
{
    // Battery in the high nibble of byte 2 (0 empty .. 8 full, bit 4 charging; J:1143-1170): stop charging when full,
    // start again below "high", as L4T does to spare the Joy-Con's simple charger.
    if ((report[0] == kReportReply || report[0] == kReportFull || report[0] == kReportNfcIr) && length > 2) {
        const uint8_t level = report[2] >> 5;
        if (level == 4)
            setCharging(s, false);
        else if (level < 3)
            setCharging(s, true);
    }
    ++s.reports;
    rumbleAfterReply(s);
    if (!s.hid)
        return;
    IOLockLock(s.outLock);
    const bool forward = switch_joycon_forward_report(&s.forward, report, length, nowMS());
    IOLockUnlock(s.outLock);
    if (forward)
        switch_joycon_hid_input(s.hid, report, length);
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
        if (s->step == kDisconnect && s->sends < kDisconnectSends)
            self->sendStep(*s);
        else
            self->failAttempt(*s, "handshake step timed out");
        break;
    case kPairing:
        self->checkDelivery(*s, "pairing step timed out");
        self->finishPairing(*s, false, "no reply");
        break;
    case kRunning:
        break;  // the shared poll drives it
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
        const uint32_t seen = s.interrupts, lsr = rd(s, LSR);
        // DMA mode: bytes the channel put in the ring that no EORD/RX-timeout interrupt took.
        const bool ring = s.dmaOn && tegra_apbdma_moved(s.dma, nullptr) % s.dmaRing.size != s.dmaTaken;
        const bool rx = ((lsr & LSR_DR) && (s.ier & (IER_RDI | IER_RTOIE))) || ring;
        const bool tx = (lsr & LSR_THRE) && (s.ier & IER_THRI);
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
            s.dmaOn ? tegra_apbdma_moved(s.dma, nullptr) : 0, s.dmaTaken, gic);
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
    auto hex = [](char (&text)[sizeof(Side::stepHeard) * 3 + 8], const uint8_t *bytes, uint32_t count, unsigned room) {
        strlcpy(text, "none", sizeof(text));
        const unsigned shown = count < room ? count : room;
        size_t at = 0;
        for (unsigned i = 0; i < shown; ++i) {
            at += (size_t)snprintf(text + at, sizeof(text) - at, "%s%02x", i ? " " : "", bytes[i]);
        }
        if (count > shown)
            strlcat(text, " ...", sizeof(text));
    };
    char heard[sizeof(s.stepHeard) * 3 + 8], stepHeard[sizeof(s.stepHeard) * 3 + 8];
    hex(heard, s.heard, s.heardCount, sizeof(s.heard));
    hex(stepHeard, s.stepHeard, s.stepHeardCount, sizeof(s.stepHeard));
    unsigned in[4], cnf[4];
    uint32_t mux[4];
    const uint32_t muxReg[4] = {cfg->muxTx, cfg->muxRx, cfg->muxRts, cfg->muxCts};
    for (unsigned i = 0; i < 4; ++i) {
        in[i] = gpioBit(cfg->txPin + i, GPIO_IN);
        cnf[i] = gpioBit(cfg->txPin + i, GPIO_CNF);
        mux[i] = pinRead(muxReg[i]);
    }
    auto driven = [this](unsigned pin) {
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
        carRead(cfg->clockSource), !!(carRead(cfg->enbState) & cfg->clockBit),
        !!(carRead(cfg->rstState) & cfg->clockBit), in[0], in[1], in[2], in[3], cnf[0], cnf[1], cnf[2], cnf[3], mux[0],
        mux[1], mux[2], mux[3], gpioBit(cfg->detectPin, GPIO_IN), driven(cfg->chargePin), driven(kPin5V),
        !!(pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd), !!(pmcRead(PMC_NO_IOPOWER) & kPadUartPower),
        !!(pmcRead(PMC_PWR_DET_VAL) & kPadUartPower),
        *(volatile uint32_t *)(apbMisc + PP_PINMUX_GLOBAL) & kClampTristatedInputs);
}

// The pads both rails share (see the header): UART pads powered and out of deep power-down (a bounded handshake, as
// Linux tegra_io_pad_power_enable and TegraDisplayPower), tristated inputs unclamped. Each is touched only when wrong;
// a change means the boot chain left the rails deaf and is worth one line. The pad voltage (PWR_DET_VAL) is only
// reported: hekate's working rail never sets it. At start and after SC7.
void SwitchJoyConRail::preparePads(const char *when)
{
    char fixed[96] = "";
    if (pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd) {
        pmcWrite(PMC_IO_DPD_REQ, kDpdOff | kPadUartDpd);
        for (unsigned us = 0; us < 250 && (pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd); ++us)
            IODelay(1);
        strlcat(fixed,
                pmcRead(PMC_IO_DPD_STATUS) & kPadUartDpd ? " deep power-down stuck on," : " deep power-down off,",
                sizeof(fixed));
    }
    if (pmcRead(PMC_NO_IOPOWER) & kPadUartPower) {
        pmcWrite(PMC_NO_IOPOWER, pmcRead(PMC_NO_IOPOWER) & ~kPadUartPower);
        (void)pmcRead(PMC_NO_IOPOWER);
        strlcat(fixed, " IO power on,", sizeof(fixed));
    }
    volatile uint32_t *global = (volatile uint32_t *)(apbMisc + PP_PINMUX_GLOBAL);
    if (*global & kClampTristatedInputs) {
        *global = *global & ~kClampTristatedInputs;
        (void)*global;
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
// does not answer); the packet fits the TX FIFO, so waiting for the transmitter to empty is a bounded handshake.
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
                self->quiesce(s);
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
                if (on)
                    self->enterDetect(s);
                else
                    self->pause(s);
            }
            if (!on) {
                self->poll->cancelTimeout();
                self->polling = false;
            }
            return kIOReturnSuccess;
        },
        this, on ? this : nullptr);
}

// The address to pair to (see the header). Joy-Cons that ran unpaired while it was unknown are paired now; a new
// address (it changes only when SwitchBluetooth adopts the staged one) pairs every running Joy-Con again.
void SwitchJoyConRail::bluetoothAddress(const uint8_t address[6])
{
    loop->runAction(
        [](OSObject *owner, void *arg, void *, void *, void *) -> IOReturn {
            auto *self = static_cast<SwitchJoyConRail *>(owner);
            const uint8_t *address = static_cast<const uint8_t *>(arg);
            const bool changed = !self->hostKnown || memcmp(self->host, address, 6);
            memcpy(self->host, address, 6);
            self->hostKnown = true;
            if (!self->pairing)
                return kIOReturnSuccess;
            for (Side &s : self->sides) {
                if (s.phase == kRunning && (s.pairPending || changed))
                    self->startPairing(s);
            }
            return kIOReturnSuccess;
        },
        this, const_cast<uint8_t *>(address));
}

bool SwitchJoyConRail::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    car = reinterpret_cast<volatile uint8_t *>(ml_io_map(kCarPA, 0x1000));
    gpio = reinterpret_cast<volatile uint8_t *>(ml_io_map(kGpioPA, 0x1000));
    apbMisc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kApbMiscPA, 0x1000));
    pinmux = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPinmuxPA, 0x1000));
    uarts = reinterpret_cast<volatile uint8_t *>(ml_io_map(kUartPA, 0x1000));
    pmc = reinterpret_cast<volatile uint8_t *>(ml_io_map(kPmcPA, 0x1000));
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
    for (Side &s : sides)
        loop->runAction(
            [](OSObject *owner, void *side, void *, void *, void *) -> IOReturn {
                static_cast<SwitchJoyConRail *>(owner)->enterDetect(*static_cast<Side *>(side));
                return kIOReturnSuccess;
            },
            this, &s);
    IOLog("SwitchJoyCon: rail driver started%s\n", pairing ? " (Bluetooth pairing on attach)" : "");
    return true;
}

static SwitchJoyConRail *gRail;  // set once at start, for switch_joycon_suspend/resume

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
    gRail = driver;
    driver->release();  // the attachment keeps it
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
