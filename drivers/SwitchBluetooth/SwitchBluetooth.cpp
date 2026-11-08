// SPDX-License-Identifier: BSD-2-Clause-Patent
// Switch OLED Bluetooth: the BCM4356A3's Bluetooth core on UART-D, brought up here and handed to iOS's Bluetooth
// daemons as the H4 byte stream they open on an iPad. Boot-arg switch-no-bt turns it off; see SwitchBluetooth.h.
//
// Hardware (switchroot tegra210b01-nx-common.dtsi / tegra210b01-fric-{gpio,pinmux}.dtsi, the OLED DTB):
// serial@70006300 is "nvidia,tegra114-hsuart" with a "brcm,bcm43438-bt" child: max-speed 3000000, RTS/CTS on
// uart4_{tx,rx,rts,cts}_pi4..7 (function uartd), shutdown-gpios PH4 (BT_REG_ON, active high), device-wakeup-gpios
// PH3 (active low), PH5 the chip's host wake (input). Interrupt: GIC SPI 90, level high = INTID 122 (Linux
// tegra210.dtsi:655 `interrupts = <GIC_SPI 90 IRQ_TYPE_LEVEL_HIGH>`; OLED DTB serial@70006300 `<0x00 0x5a 0x04>`;
// MyEcoria/nxk user/bluetooth "UART-D interrupt: SPI 90").
//
// Bring-up (MyEcoria/nxk user/bluetooth/src/{main,bcm,uart}.rs, proven on a Switch; Linux hci_bcm.c / btbcm.c):
// BT_REG_ON low 50 ms, high, 200 ms; HCI Reset at 115200; Broadcom 0xFC18 to 3 Mbaud; 0xFC2E download minidriver,
// 50 ms; the .hcd's own records (0xFC4C Write_RAM ... 0xFC4E Launch_RAM); 500 ms; the patched firmware restarts
// at 115200: Reset, 0xFC18 to 3 Mbaud again, Reset; 0xFC27 sleep mode off (BT_WAKE stays asserted); 0xFC01 BD
// address. A Hardware Error event means only a power cycle helps (nxk: Erista's first bring-up after a cold boot
// gets one for seconds), so the first bring-up retries with 1-8 s pauses.
//
// Why the kernel does the firmware (path "b"), from 21H461's /usr/sbin/BlueTool and /usr/sbin/bluetoothd: on an
// iPad, bluetoothd asks BlueTool (XPC "RunBuiltinScript" {script: boot}) to power and patch the chip. BlueTool picks a
// compiled-in boot function and .hcd from a 247-entry table keyed on MobileGestalt ProductHash, the chip it derives
// from HCI Read Local Version's LMP subversion (BCM4355C0, 4350C5, 4357B1, ...; anything else is "BCM") and
// MobileGestalt WifiVendor. There is no BCM4356A3 entry, and its /etc/bluetool override scripts are read only on
// Vendor/Internal release types. So the controller must arrive ready.
//
// UART-D (Linux serial-tegra.c, T210 = tegra30_uart_chip_data: TX FIFO full in LSR bit 8, no TX FIFO reset in FIFO
// mode; hekate bdk/soc/uart.c and clock.c for the clock): a 16550 with 32-byte FIFOs and hardware RTS/CTS
// (MCR CTS_EN | RTS_EN). Clock source PLLP 408 MHz: 3 Mbaud through the UART fractional divider (8.5 -> 48 MHz,
// divisor latch 1), 115200 from undivided PLLP with divisor latch 221 (hekate clock_uart_use_src_div, uart_init;
// nxk notes Erista needed 115200 outside the 16x divider mode). Everything moves by interrupt: an
// IOFilterInterruptEventSource drains the RX FIFO into a ring and refills the TX FIFO from another (THRI at the
// 16-byte trigger) in primary context; its work loop action hands received bytes to the bring-up's HCI parser or to
// the connected client and wakes waiting writers. A full RX ring masks the RX interrupts; the FIFO then fills and
// RTS stops the chip, so nothing is lost. A wait that times out while the UART shows a pending, enabled condition
// for 50 ms more is a lost interrupt and panics with one line.
// The filter never leaves a condition it cannot clear asserted: a line held low (a core that is not driving its TX,
// so breaks), a stuck BI/FIFOE that comes straight back after the FIFO reset, an IIR condition that survives
// kFilterRounds, or a line that re-fires with IIR showing nothing (kIdleStorm; first cleared as serial-tegra clears
// latched interrupts, clearInterruptsLocked, and masked if it comes back) masks RX (maskRxFaultLocked) until the
// next flush (every bring-up step, every connect) or send (kickTx). Before r8 such a line re-fired the moment the
// filter returned, pinned CPU0 and froze the thread there for good, often holding the work-loop gate: stackshots
// stacks-2026-11-04-045010 (r6k) and -055657 (r7a, "BT Stack Stuck") show bluetoothd's connect 30 s on-core in
// ml_set_interrupts_enabled under the flush's runAction; r7b-r7d's first bring-up never ended (no line at all) and
// every connect timed out after kConnectWaitMs.
//
// TX by APB DMA (r15-offload; ../TegraAPBDMA): what bluetoothd sends (an A2DP stream to AirPods is ~45 KB/s; the
// firmware download at every bring-up ~40 KB) took a THR-empty interrupt per 16 bytes and two register accesses per
// byte. The TX ring now lives in DMA-able memory and leaves as serial-tegra's tegra_uart_start_next_tx sends it: from a
// 4-byte-aligned ring offset, whole 16-byte bursts contiguous to the ring's end go by channel 2 (UART-D's request line,
// flow-controlled by the UART, so CTS still holds the chip's pace), one end-of-count interrupt for the lot; the
// unaligned head and the tail below 16 bytes by PIO and THR-empty as before. The ring's tail moves when the channel's
// end-of-count is taken (txDone, primary interrupt context), which also starts what follows and wakes writers through
// the work loop. Lost interrupt: a transfer that the channel shows ended but no interrupt took, after the 50 ms grace,
// panics (checkDelivery). Never
// booted: a channel that does not open, or a transfer that has not moved with the TX FIFO empty and CTS asserted (the
// UART asked and the channel did not serve), turns TX DMA off for the boot with one line; the ring keeps every byte
// the channel did not take, and PIO sends it.
//
// RX by APB DMA (r17-iodma; SwitchBluetoothRxDma.h, ../TegraAPBDMA): at peak (two Joy-Cons' 59-byte HID reports at
// 120 Hz, AirPods' A2DP completions, LE scanning for AirDrop) every byte took two UART register reads and every 16 bytes
// an interrupt. Linux serial-tegra's RX DMA stops the channel at every packet end (RTS off, a 20 us burst wait, residue,
// PIO, restart); the Joy-Con rails' ring (../SwitchJoyCon) takes the 1-3 bytes below a 4-byte burst by PIO, which is
// safe only because a rail's Joy-Con says nothing until asked again: this controller sends whenever it has something,
// and a byte the channel took while PIO read the FIFO would reorder the H4 stream. So here the channel is the FIFO's
// only reader while the UART is in DMA mode (FCR DMA select, RX trigger 1): channel 3 (UART-D's request line, as TX)
// reads RBR 32 bits at a time, one byte a read, one word a byte (tegra_apbdma_start_rx_words), into transfers that
// always start at word 0 of rxBuf. The CPU takes what moved, cache lines invalidated, into the same ring and framing as
// PIO did, at two points only: the line going idle (EORD, 4 characters after the last byte, the PIO path's RX timeout:
// an isolated packet is one interrupt) and a transfer's end of count, which also starts the next one, sized
// (SwitchBluetoothRxDma.h) to end 5 bytes (the longest H4 header) past the end of the packet it starts in, so a packet
// sent back to back with the next, no idle between, is seen 5 characters after its end where PIO's 16-byte trigger took
// up to 16 (host model at the peak load, test/switch_bluetooth_rx_dma_test: 1531 interrupts/s and 0.96% of CPU0
// against 4343 and 4.5%, the worst HID report's last byte taken 30 us after it against 64). Nothing waits on a timer,
// no transfer is stopped while bytes may flow, no PIO read ever races the channel. A ring with no room for what moved
// leaves the rest in rxBuf and the next transfer unstarted: the FIFO fills and RTS holds the chip, as PIO's stall did;
// resumeRx takes it and starts the channel again. Leaving DMA mode (a flush, power off, a fault, a stuck BI/FIFOE that
// PIO's FIFO reset then takes, the checks below) turns DMA select off first and waits 4 us for a read the channel had
// started, so PIO again sees every byte in order. Line errors lose the framing until the next sync, as an errored byte
// did under PIO. Lost interrupt (checkDelivery): a transfer ended with no interrupt taken, or bytes moved with the line
// idle and no EORD taken, 50 ms on: one line, panic. Never booted (one line, RX by PIO for the boot): a channel that
// does not open; a first packet after power-on that is not an HCI event; bytes left in the FIFO across two line-idle
// interrupts with the channel not moving; and, until a line-idle interrupt has once delivered what the channel moved,
// moved bytes no interrupt took.
//
// Userland interface: kernel control "com.apple.uart.bluetooth" (one client), SOCK_STREAM, raw H4 both ways;
// TIOCGETA / TIOCSETA as SYSPROTO_CONTROL socket options carry a struct termios whose speed is applied to the UART
// (flow control stays on). That is what BlueTool's "device -D" opens (FUN_10001e988) and what bluetoothd's POSIX
// HCI driver opens when named (FUN_1000ddaa8); bluetoothd's unnamed default is the Skywalk variant
// "com.apple.uart.sk.bluetooth", so kern_exec.c names this one in bluetoothd's BT_HCI_TRANSPORT. Its
// chip-boot requests go to the BlueTool stand-in (BlueTool/BlueTool.c, shown at /usr/sbin/BlueTool from the Data
// overlay), which reports success: the work is done here. A client that sent anything leaves the chip in an unknown
// state (an original BlueTool would push its own baud rate and firmware), so the next connect brings the chip up
// again.
//
// BD address: the unit's own from the fuses (publishAddress), or Horizon's when deploy staged hekate's "Dump Joy-Con BT"
// (switchroot/joycon_mac.ini on the FAT, read there by tools/deploy/joycon_pairing.py and written to Data as
// kStagedAddressPath: 6 bytes, display order), as switchroot's U-Boot sets the controller to the host address in
// joycon_mac.bin so that Joy-Cons paired in Horizon reconnect without being paired again. bluetoothd must see one
// address for its whole life: it reads local-mac-address at its start (21H461 FindLocalMacAddress, "Got
// local-mac-address", before it connects) and the controller's (Read BD_ADDR) once its stack is up, and every classic
// link key it keeps carries the local address it was stored under. When its stack starts it drops each key stored
// under another one ("We have a link key for the wrong local device", "Not link key, result 150"), and a link key
// request for that device then finds none. Until r14 the staged address replaced the fuses' only at bluetoothd's
// first connect, after it had read local-mac-address: every boot's stack start dropped the Joy-Con and AirPods keys
// stored under the other address (two or three a boot in r7r's logs), and only the keys joyconpaird imported after
// that check were there for the boot. r7r 11:02:51 dropped the right Joy-Con's (imported
// just before) and kept the left's (just after); at 11:07:43-:46 the right one connected four times and dropped the
// link within 330 ms without authenticating, at :49 its link key request found no key, and after it was put back on
// the rail (re-import, 11:07:57) its key was found. So the staged address is published once Data is mounted, before
// launchd (switch_bluetooth_data_mounted, called by SwitchNVRAM's hook in load_init_program): bluetoothd reads it at
// its start, and its first connect writes it to the controller (0xFC01) before the stream is handed over. Every
// connect then hands the controller's address to the rail Joy-Con driver, which pairs rail Joy-Cons to it
// (switch_joycon_bluetooth_address, ../SwitchJoyCon/SwitchJoyCon.h).
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/fcntl.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/vnode.h>
#include <sys/conf.h>
#include <miscfs/devfs/devfs.h>
#include <string.h>
#include "SwitchBluetooth.h"
#include "SwitchBluetoothLeFilter.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "SwitchBluetoothLinks.h"
#include "SwitchBluetoothRxDma.h"
#include "SwitchCoex.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraAPBDMA/TegraAPBDMA.h"
#include "../SwitchSuspend/SwitchWake.h"

// The rail Joy-Con driver's strong definition wins when it is linked (../SwitchJoyCon/SwitchJoyCon.h).
extern "C" __attribute__((weak)) void switch_joycon_bluetooth_address(const uint8_t[6])
{}

// SwitchWLAN.cpp: the Wi-Fi infra channel in MHz while associated, else 0 (for AFH, "Coexistence").
extern "C" __attribute__((weak)) uint16_t swlan_awdl_infra_freq(void)
{
    return 0;
}

// SwitchWLAN.cpp: the controller's link count, at every change (the Wi-Fi coexistence mode follows it).
extern "C" __attribute__((weak)) void switch_wlan_bt_links(unsigned)
{}

namespace {
constexpr vm_offset_t kUartPA = 0x70006000, kUartD = 0x300;  // UART-A page; UART-D at +0x300 (hekate uart.c)
constexpr vm_offset_t kCarPA = 0x60006000, kGpioPA = 0x6000D000, kPinmuxPA = 0x70003000, kFusePA = 0x7000F000;
constexpr uint32_t kIntid = 32 + 90;
constexpr uint32_t kPllpHz = 408000000;
constexpr uint32_t kInitBaud = 115200, kFastBaud = 3000000;
constexpr const char *kControlName = "com.apple.uart.bluetooth";
constexpr const char *kStagedAddressPath = "/private/var/Switch/bluetooth/host-address";  // see the header

// Clock and reset controller (hekate bdk/soc/clock.h, clock.c _clock_uart[3]): UARTD is bit 1 of the U registers.
enum : uint32_t {
    RST_DEVICES_U = 0x00c,
    CLK_OUT_ENB_U = 0x018,
    CLK_SOURCE_UARTD = 0x1c0,
    RST_DEV_U_SET = 0x310,
    RST_DEV_U_CLR = 0x314,
    CLK_ENB_U_SET = 0x330,
};

constexpr uint32_t kUartDBit = 1u << 1;

// The fuse block is readable only while clocked (CLK_OUT_ENB_H bit 7) and visible (MISC_CLK_ENB bit 28); an
// unclocked read stalls the bus (hekate clock_enable_fuse; SwitchWLANProbe.cpp switch_wlan_mac).
enum : uint32_t {
    CLK_OUT_ENB_H = 0x014,
    MISC_CLK_ENB = 0x048
};

constexpr uint32_t kFuseClock = 1u << 7, kFuseVisible = 1u << 28;
constexpr uint32_t kFuseLotCode0 = 0x800 + 0x208;  // FUSE_OPT_LOT_CODE_0 (FUSE_BASE 0x7000F800 + 0x208)
constexpr uint32_t kUartDivEnable = 1u << 24;      // source bits 31:29 = 0 (PLLP); divider is 7.1 fixed point minus 1

// UART registers (4 bytes apart) and bits: Linux serial_reg.h + serial-tegra.c, hekate bdk/soc/uart.h.
enum : uint32_t {
    THR = 0x00,
    IER = 0x04,
    IIR = 0x08,
    FCR = 0x08,
    LCR = 0x0c,
    MCR = 0x10,
    LSR = 0x14,
    MSR = 0x18,
    SPR = 0x1c,
    IRDA_CSR = 0x20
};

enum : uint32_t {
    IER_RDI = 1u << 0,
    IER_THRI = 1u << 1,
    IER_RLSI = 1u << 2,
    IER_RTOIE = 1u << 4,  // RTOIE: Tegra RX timeout
    IER_EORD = 1u << 5,  // end of receive data: the line idle 4 characters (DMA mode; serial-tegra TEGRA_UART_IER_EORD)
    IER_RX = IER_RDI | IER_RLSI | IER_RTOIE,
    IER_RX_DMA = IER_RLSI | IER_RTOIE | IER_EORD,  // DMA mode: no RDI, the channel takes every byte
    IIR_NO_INT = 1u << 0,
    FCR_ENABLE = 1u << 0,
    FCR_CLEAR_RX = 1u << 1,
    FCR_CLEAR_TX = 1u << 2,
    FCR_DMA = 1u << 3,         // UART_FCR_DMA_SELECT: RX DMA requests (header "RX by APB DMA")
    FCR_RX_TRIG_1 = 0u << 6,   // DMA mode: a request per byte, one APB read each
    FCR_TX_TRIG_16 = 0u << 4,  // Tegra's TX trigger runs backwards: 0 = interrupt at 16 bytes left
    FCR_RX_TRIG_16 = 3u << 6,  // RX data interrupt above 16 bytes; the timeout one takes the rest (RX trigger below)
    LCR_8N1 = 3u,
    LCR_DLAB = 1u << 7,
    MCR_RTS = 1u << 1,
    MCR_CTS_EN = 1u << 5,
    MCR_RTS_EN = 1u << 6,
    LSR_DR = 1u << 0,
    LSR_ERRORS = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4),  // OE PE FE BI
    LSR_BI = 1u << 4,
    LSR_THRE = 1u << 5,
    LSR_TEMT = 1u << 6,
    LSR_FIFOE = 1u << 7,  // an errored character in the RX FIFO
    LSR_TXFIFO_FULL = 1u << 8,
    MSR_CTS = 1u << 4,
};

enum : uint32_t {
    ID_MODEM = 0,
    ID_TX = 1,
    ID_RX = 2,
    ID_LINE = 3,
    ID_EORD = 4,
    ID_RX_TIMEOUT = 6
};  // (IIR >> 1) & 7

constexpr uint32_t kFifoBytes = 32;
constexpr uint32_t kIrqRxBudget =
    64;  // RX bytes one interrupt moves before masking RX and handing the rest to the work loop
constexpr uint32_t kFilterRounds = 32;  // IIR conditions one interrupt services; more is one it cannot clear
constexpr uint32_t kRxDmaSettleUs = 4;  // leaving DMA mode: a read the channel had started lands (one byte, < 1 us)
constexpr uint32_t kFaultRun = 2;       // break / stuck-error events with no good byte between them that mask RX
// Interrupts in a row that find nothing to do, within kIdleStormNs: the line re-fires straight away with IIR showing
// nothing (one such entry after a real interrupt is the level line's deassert latency; ~1 ms apart is normal too).
constexpr uint32_t kIdleStorm = 64, kIdleStormNs = 1000000;

// GPIO (TegraGPIO.h numbering, gpio = port * 8 + pin) and its per-port registers; the masked copies at +0x80
// (bits 15:8 select, 7:0 value) change one pin without a read-modify-write, so TegraGPIO's pins are never touched.
constexpr unsigned PH3 = 7 * 8 + 3, PH4 = 7 * 8 + 4, PH5 = 7 * 8 + 5, PI4 = 8 * 8 + 4;

enum : uint32_t {
    GPIO_CNF = 0x00,
    GPIO_OE = 0x10,
    GPIO_OUT = 0x20,
    GPIO_IN = 0x30,
    GPIO_MSK = 0x80
};

inline uint32_t portReg(unsigned gpio, uint32_t reg)
{
    return (gpio >> 5) * 0x100 + ((gpio >> 3) & 3) * 4 + reg;
}

// Pinmux, low 7 bits of PINMUX_AUX (function 1:0, pull 3:2, tristate 4, park 5 cleared, input 6), as nxk's
// Pads::setup with the switchroot DT values (fric-pinmux.dtsi lines 750-780 and 1149-1173).
enum : uint32_t {
    PM_PULL_DOWN = 1u << 2,
    PM_TRISTATE = 1u << 4,
    PM_INPUT = 1u << 6,
    PM_LOW = 0x7f
};

struct Pad {
    uint16_t offset;
    uint8_t value;
};

constexpr Pad kPads[] = {
    {0x114, 0},                                      // uart4_tx_pi4: uartd, driven
    {0x118, PM_PULL_DOWN | PM_TRISTATE | PM_INPUT},  // uart4_rx_pi5
    {0x11c, 0},                                      // uart4_rts_pi6
    {0x120, PM_PULL_DOWN | PM_TRISTATE | PM_INPUT},  // uart4_cts_pi7
    {0x1c0, PM_PULL_DOWN | PM_INPUT},                // ap_wake_bt_ph3 (input on: GPIO_IN reads back the level driven)
    {0x1c4, PM_PULL_DOWN | PM_INPUT},                // bt_rst_ph4
    {0x1c8, PM_TRISTATE | PM_INPUT},                 // bt_wake_ap_ph5
};

// HCI (Bluetooth Core spec; Broadcom vendor commands as Linux btbcm.c / hci_bcm.c).
enum : uint16_t {
    OP_RESET = 0x0c03,
    OP_READ_LOCAL_NAME = 0x0c14,
    OP_READ_VERSION = 0x1001,
    OP_BCM_WRITE_BDADDR = 0xfc01,
    OP_BCM_UART_BAUD = 0xfc18,
    OP_BCM_SLEEP_MODE = 0xfc27,
    OP_BCM_MINIDRIVER = 0xfc2e,
    OP_BCM_WRITE_RAM = 0xfc4c,
    OP_BCM_LAUNCH_RAM = 0xfc4e,
};

enum : uint8_t {
    H4_COMMAND = 1,
    H4_ACL = 2,
    H4_SCO = 3,
    H4_EVENT = 4
};

enum : uint8_t {
    EV_COMMAND_COMPLETE = 0x0e,
    EV_COMMAND_STATUS = 0x0f,
    EV_HARDWARE_ERROR = 0x10
};

enum : uint8_t {
    EV_NUM_COMPLETED = 0x13,
    EV_LE_META = 0x3e,
    EV_VENDOR = 0xff,
    LE_ADV_REPORT = 0x02
};

enum : uint8_t {
    EV_FLUSH_OCCURRED = 0x11
};  // {handle}: an ACL packet flushed after its flush timeout

constexpr uint16_t OP_READ_BUFFER_SIZE = 0x1005;  // returns {ACL length, SCO length, ACL packets, SCO packets}

enum : uint8_t {
    LE_CONN_COMPLETE = 0x01,
    LE_ENH_CONN_COMPLETE = 0x0a
};

// LE commands bluetoothd's own (Apple-chip) choices meet on this controller (BLE section in the client-mode comments).
enum : uint16_t {
    OP_SET_EVENT_MASK = 0x0c01,
    OP_SET_EVENT_MASK_2 = 0x0c63,
    OP_WRITE_LE_HOST_SUPPORT = 0x0c6d,
    OP_LE_SET_ADV_PARAMS = 0x2006,
    OP_LE_SET_SCAN_ENABLE = 0x200c,
    OP_BCM_LE_META = 0xfce9,           // Broadcom LE meta VSC: sub-operation first (bluetoothd _BCM_LEMeta*)
    OP_BCM_MULTI_ADV_PARAMS = 0xfd4a,  // bluetoothd _BCM_SetMultipleAdvInstanceParams
    OP_ACCEPT_CONNECTION = 0x0409,     // Accept Connection Request {address, role} (SwitchBluetoothLinks.h)
    OP_DISCONNECT = 0x0406,            // Disconnect {handle, reason}: Command Status, then Disconnection Complete
};

// 0xFCE9 sub-operations not passed on (BLE section): enable features, PCF feature selection, PCF service UUID (and
// its V2/V3 forms).
constexpr uint8_t kRefusedLeMeta[] = {0x01, 0x05, 0x07, 0x50, 0x61};
constexpr uint16_t kAdvIntervalMinNonConnectable = 0x00a0;  // 100 ms: Core 4.x, LE Set Advertising Parameters
constexpr uint8_t kHciVersion50 = 9;                        // Core 5.0 dropped that minimum
// Client-mode completion fix (frameRx): the one command that has no completion event, the LE controller OGF, the
// vendor OGF, and the status bluetoothd already copes with for vendor commands this controller lacks.
constexpr uint16_t OP_HOST_NUMBER_OF_COMPLETED_PACKETS = 0x0c35;
constexpr uint16_t kOgfLe = 0x08, kOgfVendor = 0x3f;
constexpr uint8_t kUnknownCommand = 0x01;
constexpr uint32_t kMaxPending = 16;
constexpr uint16_t kBroadcom = 0x000f;

constexpr uint32_t kPowerOffMs = 50, kPowerOffAfterErrorMs = 500, kPowerOnMs = 200;  // nxk POWER_OFF/ON_MS
constexpr uint32_t kMinidriverMs = 50, kLaunchMs = 500;                              // btbcm, nxk LAUNCH_MS
constexpr uint32_t kCommandMs = 500, kRecordMs = 1000, kGraceMs = 50;
constexpr uint32_t kWriteMs = 2000, kConnectWaitMs = 20000;
constexpr uint32_t kRetrySeconds[] = {1, 2, 4, 8};
// A bring-up attempt takes about a second and, every wait in it being bounded, ~20 s at worst; a connect less. One
// still running after this has stopped for a reason the driver does not know, and says so once (watch()).
constexpr uint32_t kStuckMs = 60000;
constexpr uint32_t kRingBytes = 16384;
constexpr uint32_t kStatsMs = 10000;  // the stats line: at most this often, and only after traffic (logStats)
#if DEVELOPMENT || DEBUG
constexpr uint32_t kStatsRoutineWindows = 6;  // routine LE scanning only: one line per 6 windows (a minute)
#endif
constexpr uint32_t kTraceEntries = 80, kTraceParams = 24, kTraceReturn = 12;
#if DEVELOPMENT || DEBUG
constexpr uint32_t kLinkTraceLines = 600;  // link trace lines a boot (os_log; "Links" section)
#endif
// Traced once a boot with their completions besides the LE and vendor commands: Read Local Supported Features, Read
// Buffer Size (the ACL and SCO buffers every link shares), Read BD_ADDR (the address bluetoothd takes from the
// controller), controller-to-host flow control and Host Buffer Size, Write Simple Pairing Mode, Write Secure
// Connections Host Support, Write Class Of Device, Write Page Timeout, Write Connection Accept Timeout, Write Page
// Scan Type, Write Inquiry Scan Type.
#if DEVELOPMENT || DEBUG
constexpr uint16_t kTracedSetup[] = {0x1003, 0x1005, 0x1009, 0x0c31, 0x0c33, 0x0c56,
                                     0x0c7a, 0x0c24, 0x0c18, 0x0c16, 0x0c47, 0x0c43};
#endif

inline uint16_t le16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | static_cast<unsigned>(p[1]) << 8);
}

inline unsigned flag(uint32_t value, uint32_t mask)
{
    return (value & mask) ? 1 : 0;
}

// Header bytes after an outgoing packet's H4 type: a command's 3, ACL's 4, SCO's 3; 0 for anything else.
inline uint8_t txHeaderBytes(uint8_t type)
{
    switch (type) {
    case H4_COMMAND:
    case H4_SCO:
        return 3;
    case H4_ACL:
        return 4;
    default:
        return 0;
    }
}

// Header bytes after an incoming packet's H4 type: an event's 2, ACL's 4, SCO's 3; 0 for anything else.
inline uint8_t rxHeaderBytes(uint8_t type)
{
    switch (type) {
    case H4_EVENT:
        return 2;
    case H4_ACL:
        return 4;
    case H4_SCO:
        return 3;
    default:
        return 0;
    }
}

// A .hcd is HCI commands back to back (opcode, length, parameters): Write_RAM records, Launch_RAM last.
bool validPatch(const uint8_t *p, uint32_t size)
{
    uint32_t at = 0;
    uint32_t records = 0;
    uint16_t op = 0;
    while (at + 3 <= size) {
        op = le16(p + at);
        if (op != OP_BCM_WRITE_RAM && op != OP_BCM_LAUNCH_RAM)
            return false;
        at += 3u + p[at + 2];
        ++records;
    }
    return (records != 0u) && at == size && op == OP_BCM_LAUNCH_RAM;
}

// Single producer, single consumer; indices run free and are masked on use.
struct Ring {
    uint8_t *data;
    uint32_t head, tail;

    uint32_t used() const
    {
        return __atomic_load_n(&head, __ATOMIC_ACQUIRE) - __atomic_load_n(&tail, __ATOMIC_ACQUIRE);
    }

    uint32_t space() const
    {
        return kRingBytes - used();
    }
};

// H4 framing of what the controller sends while the kernel itself talks to it.
struct H4Parser {
    uint8_t state;  // 0 packet type, 1 header, 2 payload
    uint8_t type, need, have;
    uint8_t header[4];
    uint16_t length, got;
    uint8_t payload[255];
};

enum State {
    kStarting,
    kBusy,
    kReady,
    kFailed
};

enum Mode {
    kIdle,
    kKernel,
    kClient
};  // who receives the controller's bytes
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class SwitchBluetooth : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchBluetooth)

        volatile uint8_t *uart = nullptr,
                         *car = nullptr, *gpio = nullptr, *pinmux = nullptr;
    IOWorkLoop *loop = nullptr;
    IOFilterInterruptEventSource *irq = nullptr;
    IOSimpleLock *hwLock = nullptr;  // UART registers (DLAB, IER) against the filter
    IOLock *stateLock = nullptr, *hciLock = nullptr, *txLock = nullptr;
    Ring rx = {}, tx = {};
    uint32_t ier = 0, baud = 0;
    bool chipOn = false;              // BT_REG_ON high; RX interrupts are only on while it is (hwLock)
    bool rxStalled = false;           // RX interrupts masked: ring full or an interrupt's budget spent (hwLock)
    bool rxFault = false;             // RX masked until the next flush or send: see maskRxFaultLocked (hwLock)
    uint32_t rxFaultRun = 0;          // break / stuck-error events since the last good byte (hwLock)
    volatile bool txWanted = false;   // a writer waits for ring space or an empty ring
    volatile bool actionDue = false;  // the filter asked for the action; the line stays masked until it has run
    uint32_t irqs = 0, rxBytes = 0, txBytes = 0, lineErrors = 0, dropped = 0, resyncs = 0, hardwareErrors = 0;
    uint32_t idleIrqs = 0, rxResets = 0;  // interrupts that moved no byte; RX FIFO resets for a stuck BI/FIFOE
    // TX by DMA (header "TX by APB DMA"), hwLock: the channel and the ring's DMA-able memory (null channel: PIO only),
    // the bytes the transfer in flight takes from the tail (0: none), transfers done, a reason it went off (logged by
    // checkDelivery's caller thread once).
    tegra_apbdma_channel *txDma = nullptr;
    tegra_apbdma_buffer txBuf = {};
    uint32_t txDmaBytes = 0, txDmaRuns = 0;
    bool txDmaOff = false;
    static void txDone(void *context);
    void txDmaFailedLocked(uint32_t moved);
    // RX by DMA (header "RX by APB DMA"), hwLock: the channel and its buffer of words (null channel: PIO only); DMA
    // mode on; a transfer running from word 0 (rxDmaLength words, rxDmaTaken of them taken); off for the boot; a
    // line-idle interrupt has delivered what moved (until then a lost one is a failed check, not a panic); the first
    // packet after power-on was looked at; bytes seen in the FIFO at the last line-idle interrupt with the channel at
    // rxDmaIdleMoved. Counts: transfers, ends of count taken, line-idle takes, bytes. rxDmaFailed: why it went off (the
    // action logs it).
    tegra_apbdma_channel *rxDma = nullptr;
    tegra_apbdma_buffer rxBuf = {};
    bool rxDmaOn = false, rxDmaArmed = false, rxDmaOff = false, rxDmaProven = false, rxDmaChecked = false;
    bool rxDmaIdleFull = false;
    uint32_t rxDmaLength = 0, rxDmaTaken = 0, rxDmaIdleMoved = 0;
    uint32_t rxDmaTransfers = 0, rxDmaEnds = 0, rxDmaIdleTakes = 0, rxDmaBytes = 0;
    const char *rxDmaFailed = nullptr;
    char rxDmaWhy[96] = {};
    uint64_t isrTicks = 0;  // time in the UART filter and the channels' handlers (the stats line's interrupt time)
    static void rxDone(void *context);
    bool takeRxDmaLocked(uint32_t movedBytes, bool ended);
    bool endRxDmaLocked();
    bool rxDmaIdleLocked(bool idle, bool lineError);
    void armRxDmaLocked();
    void enterRxDmaLocked();
    void leaveRxDmaLocked(bool keep);
    void rxDmaFailedLocked(const char *why);
    uint32_t rxFaults = 0;  // times RX was masked for a fault
    uint32_t idleRun = 0;   // idle interrupts in a row (hwLock), the first at idleSince
    uint64_t idleSince = 0, idleStormAbs = 0;
    bool stormCleared = false;       // a storm got clearInterruptsLocked; the next one masks (hwLock)
    const char *lastFault = "none";  // what the last RX fault was, for describe() and a failed bring-up
    // One work loop wakeup per controller packet in client mode (filter): the H4 framing of the bytes the filter puts
    // in the ring, kept in step with frameRx's by syncFramerLocked (hwLock). Lost (a line error or dropped bytes):
    // every interrupt wakes the work loop until the next sync. RX DMA sizes its transfers from it too.
    switch_bt_h4_frame rxFrame = {};
    bool rxPacketDone = false;  // a packet ended in the ring since the last wakeup
    bool framePackets = false;  // client mode: wake the work loop at packet ends only
    void frameByteLocked(uint8_t byte);
    void syncFramerLocked();

    // Kernel HCI (bring-up), under hciLock.
    H4Parser h4 = {};

    struct {
        uint16_t opcode;
        bool waiting, done, hardwareError;
        uint8_t length;
        uint8_t data[255];  // return parameters, status first
    } cmd = {};

    // Userland side (stateLock), mode under the work loop gate.
    kern_ctl_ref control = nullptr;
    State state = kStarting;
    Mode mode = kIdle;
    uint32_t client = 0;
    bool clientUsed = false, chipDirty = false, clientBlocked = false;
    struct termios tio = {};

    // Client-mode HCI bookkeeping (frameRx): the commands the client has sent and not yet seen completed, from its H4
    // stream (noteClientTx; cmdLock), and the framing of the controller's stream, so that a Command Complete or Status
    // is looked at before it goes out (work loop).
    IOSimpleLock *cmdLock = nullptr;
    uint16_t sentCmds[kMaxPending] = {};
    uint32_t sentCount = 0;

    struct {
        uint8_t state, type, have, need;
        uint8_t header[4];
        uint32_t left;
    } txFrame = {};

    uint32_t rxPacketLeft = 0;  // bytes of the controller's packet being passed on still to go
    bool framingLost = false;   // either stream stopped parsing as H4: everything goes through untouched
    bool rxDrop = false;        // the packet being taken is a completion the client did not ask for: not passed on
    bool sentOverflow = false;  // a command left `sentCmds` unanswered (cmdLock): unknown completions pass as they come
    uint32_t completionFixes = 0, strayDrops = 0;
    uint16_t strayOps[8] = {};  // opcodes of the stray completions logged this boot
    uint32_t strayOpCount = 0;
    void resetClientFraming();
    size_t noteClientTx(const uint8_t *bytes, size_t length);
    void noteCommandLocked(uint16_t opcode);
    bool frameRx(uint32_t avail);
    bool takePendingLocked(uint16_t opcode);
    uint16_t takeVendorLocked();

    // BLE (client-mode comments): whole commands looked at before they go out, the answers given here, the trace and
    // the stats line.
    uint8_t hciVersion = 0;      // Read Local Version's HCI version at the last bring-up (0 unknown)
    uint8_t lastCredits = 1;     // the controller's Num_HCI_Command_Packets in its last completion (work loop)
    uint8_t answers[4][8] = {};  // Command Complete events made here, not yet passed on (cmdLock)
    uint32_t answerHead = 0, answerTail = 0;
    // bluetoothd's LE filtering in the controller's APCF filters (SwitchBluetoothLeFilter.h), client thread; leApcf
    // from the last bring-up's capability probe, until a refused APCF command turns it off for the boot.
    switch_le_filter leFilter = {};
    bool leApcf = false, leDirty = false, leScanning = false, leCapsLogged = false;
    bool answer(uint16_t opcode, uint8_t status, int sub);
    bool programLeFilter();

    struct Traced {
        uint16_t opcode;
        uint8_t sub;     // 0xFCE9's sub-operation, else 0
        uint8_t state;   // 1 sent, waiting for its completion; 2 logged
        uint8_t length;  // parameter bytes the command had
        uint8_t params[kTraceParams];
    } traced[kTraceEntries] = {};

    uint32_t tracedCount = 0;  // cmdLock

    struct Stats {
        uint32_t actions;                                // runs of the interrupt action (work loop wakeups)
        uint32_t events, acl, sco;                       // controller packets passed to the client
        uint32_t cc, cs, nocp, le, vendor, otherEvents;  // events by code
        uint32_t leAdv, leAdvReports, leConn, leOther;   // LE meta events by sub-event
        uint32_t commands, leCommands, vendorCommands;   // client commands
        uint32_t answered, advFixed, scanOn, scanOff;    // answered here, intervals raised, LE scan enables
        uint32_t central;                                // gamepads accepted as central (acceptAsCentral)
    } st = {}, stLogged = {};

    struct Failed {
        uint16_t opcode;
        uint8_t status;
        uint32_t count;
    } failed[8] = {}, failedLogged[8] = {};  // completions with a non-zero status, by opcode (work loop)

    uint32_t irqsLogged = 0, rxBytesLogged = 0, txBytesLogged = 0;
    uint32_t rxDmaBytesLogged = 0, rxDmaTransfersLogged = 0, rxDmaEndsLogged = 0, rxDmaIdleTakesLogged = 0;
    uint32_t txDmaRunsLogged = 0;
    uint64_t isrTicksLogged = 0;
    uint64_t statsAt = 0;      // mach_absolute_time of the last stats line
    uint64_t statsWindow = 0;  // kStatsMs in absolute time units
    errno_t clientCommand(uint8_t *packet, size_t size);
    bool answerHere(uint16_t opcode, const uint8_t *params, uint8_t length);
    bool fixAdvertising(uint16_t opcode, uint8_t *params, uint8_t length);
    void traceCommand(uint16_t opcode, const uint8_t *params, uint8_t length, bool answered);
    void traceCompletion(uint16_t opcode, bool status, uint8_t result, const uint8_t *ret, uint32_t retLength);
    void countCompletion(uint16_t opcode, uint8_t result);
    bool passAnswers();
    bool clientRoom(size_t need);
    void logStats(bool force);

    // Links ("Links" section, SwitchBluetoothLinks.h): ACL packets per connection handle (cmdLock) and the link trace.
    switch_bt_links links = {};
    uint8_t roleAddr[6] = {};  // the last Role Change's address and role, for a link whose Connection Complete follows
    uint8_t role = SWITCH_BT_ROLE_UNKNOWN;
    uint32_t linkLines = 0;            // link trace lines logged this boot (atomic)
    switch_bt_requests requests = {};  // incoming Connection Requests until their Accept (cmdLock)
    bool centralLogged = false;
    bool acceptAsCentral(uint8_t *params, uint8_t length);

    // Coexistence ("Coexistence" section, SwitchCoex.h): the scans and the ACL priority bluetoothd has set (cmdLock),
    // one line per change, and a snapshot for SwitchWLAN's stats line.
    switch_coex_bt coex = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, SWITCH_COEX_NO_HANDLE, 0};
    void coexCommand(uint16_t opcode, const uint8_t *params, uint8_t length);
    // AFH from the Wi-Fi channel: a command of ours put into the client's stream between its commands (cmdLock: the
    // opcode outstanding, its status and return parameters), the Wi-Fi channel the controller's classification is
    // for (0: every channel usable, as after a reset), when to read the links' maps back, and whether it refused.
    uint16_t injectOp = 0;
    uint8_t injectStatus = 0, injectLength = 0, injectRet[16] = {};
    static constexpr uint8_t kInjectMax = sizeof(switch_le_command::params);
    // Air-time evidence for the stats lines: Flush Occurred events (packets the controller dropped after the flush
    // timeout, A2DP's 60 or 200 ms) and the controller's ACL buffers (Read Buffer Size), against which a link's most
    // held packets in a window is read.
    uint32_t coexFlushes = 0, coexFlushesSeen = 0;  // atomic / coexSnapshot
    uint16_t aclBuffers = 0;
    uint16_t afhMhz = 0;                      // the Wi-Fi channel the checks below are for (0: none on 2.4 GHz)
    uint64_t afhCheckAt = 0;                  // when to read the links' AFH maps (0: not due)
    unsigned afhLinks = 0;                    // ACL links at the last check
    bool afhSet = false, afhRefused = false;  // our classification is in the controller; it refused one
    uint8_t inject(uint16_t opcode, const uint8_t *params, uint8_t length, uint8_t *ret, uint8_t *retLength);
    void coexAfh(uint16_t opcode);

    // System sleep (SwitchBluetooth.h): the client's sends wait at sendGate from suspend to resume; disconnects counts
    // Disconnection Complete events (thread_wakeup on it) for the controllers suspend drops.
    IOLock *sendGate = nullptr;
    bool asleep = false, slept = false;  // suspended; the controller was up and the UART stopped for it
    uint32_t disconnects = 0;
    uint8_t sleepCommand(uint16_t opcode, const uint8_t *params, uint8_t length);
    unsigned dropControllers();

public:
    bool sleep();
    void wake();

private:
public:
    bool coexSnapshot(switch_coex_bt *out, char *linksText, size_t size);

private:
    // Scratch for the work loop's link event and stats line (both under the work-loop gate: action or runAction), off
    // the stack of whichever thread runs deliver() (it can be bluetoothd's, through ctlSend or ctlRcvd).
    uint8_t linkParams[255] = {};
    char linkText[200] = {};
    switch_bt_links statsLinks = {};
    char statsHeld[520] = {};
    char statsLine[1024] = {};
    void linkEvent(uint8_t code, uint32_t length);
    void linkCommand(uint16_t opcode, const uint8_t *params, uint8_t length);
    void linkCompletion(uint16_t opcode, bool status, uint8_t result);
    void linkLog(const char *direction, const char *line, const switch_bt_link *last = nullptr);

    const OSData *patch = nullptr;
    uint8_t address[6] = {};
    bool haveAddress = false;
    const char *addressFrom = "ADT";
    uint8_t chipAddress[6] = {};  // what the controller was last given (Write BD_ADDR), valid with chipAddressSet
    bool chipAddressSet = false;
    bool stagedChecked = false;  // stateLock: the staged address was looked for (first connect)
    bool adoptStagedAddress();
    void publishAddressProperties();

    // A bring-up or connect that outlives kStuckMs (watch / unwatch) logs one line; a thread call, so a work loop or
    // gate that is itself stuck cannot hold it up.
    thread_call_t stuckCall = nullptr;
    static void stuck(thread_call_param_t owner, thread_call_param_t what);
    void watch(const char *what);
    void unwatch();

    // Registers and pins.
    void setPin(unsigned pin, uint32_t reg, bool on)
    {
        switch_mmio_write32(gpio, portReg(pin, reg) + GPIO_MSK, (1u << (pin & 7)) << 8 | (on ? 1u << (pin & 7) : 0));
    }

    bool pin(unsigned p, uint32_t reg) const
    {
        return (switch_mmio_read32(gpio, portReg(p, reg)) & (1u << (p & 7))) != 0u;
    }

    void setupPins();

    // BT_REG_ON. RX interrupts follow the core's power: an unpowered core leaves RX on its pull-down, a break that keeps
    // the line-status interrupt asserted on CPU0 (r6k: bluetoothd's re-bring-up spun 30 s in the flush after power-on).
    // The flush after power-on (gated 0) turns them back on.
    void power(bool on)
    {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        chipOn = on;
        if (!on) {
            leaveRxDmaLocked(false);  // what an unpowered core sends is noise; the flush after power-on starts it again
            ier &= ~IER_RX;
            rxStalled = false;
            switch_mmio_write32(uart, IER, ier);
        }
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        setPin(PH4, GPIO_OUT, on);
    }

    void resetRxFifoLocked();
    void maskRxFaultLocked(const char *what);
    bool rxFaultEventLocked(const char *what);
    void clearInterruptsLocked();
    void setupUart(bool rts = true);
    void setBaudLocked(uint32_t rate);
    void setHostBaud(uint32_t rate);
    void describe(char *buffer, size_t size);

    // Interrupt path.
    static bool filter(OSObject *owner, IOFilterInterruptEventSource *sender);
    static void action(OSObject *owner, IOInterruptEventSource *sender, int count);
    bool drainRx(uint32_t &budget, bool stallWhenSpent);
    bool resumeRx();
    bool fillTx();
    void kickTx();
    void deliver();
    void checkDelivery(const char *what);

    // Transmit and the kernel's own HCI.
    int transmit(const uint8_t *bytes, size_t length, uint32_t timeoutMs);
    int drainTx(uint32_t timeoutMs);
    void parse(const uint8_t *bytes, uint32_t length);
    void event(uint8_t code, const uint8_t *params, uint8_t length);
    int command(uint16_t opcode, const uint8_t *params, uint8_t length, uint32_t timeoutMs, uint8_t *out = nullptr,
                uint8_t *outLength = nullptr);
    int resetAt(uint32_t rate, unsigned tries);
    bool fast();
    bool bringUp(uint32_t offMs, char *why, size_t whySize);

    // Work loop gated actions.
    static IOReturn gated(OSObject *owner, void *what, void *, void *, void *);
    void flushRx();
    void setMode(Mode m);

    // Kernel control.
    static errno_t ctlConnect(kern_ctl_ref ref, struct sockaddr_ctl *sac, void **unitinfo);
    static errno_t ctlDisconnect(kern_ctl_ref ref, u_int32_t unit, void *unitinfo);
    static errno_t ctlSend(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, mbuf_t m, int flags);
    errno_t clientSend(u_int32_t unit, mbuf_t m);
    static errno_t ctlSetopt(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t len);
    static errno_t ctlGetopt(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, int opt, void *data, size_t *len);
    static void ctlRcvd(kern_ctl_ref ref, u_int32_t unit, void *unitinfo, int flags);

public:
    bool start(IOService *provider) override;
    void run();
};

OSDefineMetaClassAndStructors(SwitchBluetooth, IOService)

    namespace
{
    SwitchBluetooth *gDriver;
    bool gAddressFromFuses;

    // The staged address (see the header, "BD address"), looked for once Data is mounted, before launchd
    // (switch_bluetooth_data_mounted): kStagedNone until then and when there is none, kStagedFound with it in gStaged.
    enum {
        kStagedUnread,
        kStagedNone,
        kStagedFound
    };

    int gStagedState = kStagedUnread;  // atomic; gStaged is written before kStagedFound is stored (release)
    uint8_t gStaged[6];

    // kStagedAddressPath, read with the kernel's context: true with a unicast address in `out`. A missing file is the
    // normal case (no dump staged); anything else is one line and no staged address.
    bool readStagedAddress(uint8_t out[6])
    {
        vfs_context_t ctx = vfs_context_kernel();
        vnode_t vp = NULLVP;
        const int opened = vnode_open(kStagedAddressPath, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx);
        if (opened) {
            if (opened != ENOENT)
                IOLog("SwitchBluetooth: %s not opened (%d)\n", kStagedAddressPath, opened);
            return false;
        }
        uint8_t staged[7] = {};
        int resid = 0;
        const int error = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(staged), sizeof(staged), 0, UIO_SYSSPACE,
                                  IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        vnode_close(vp, FREAD, ctx);
        static const uint8_t zero[6] = {};
        static const uint8_t ones[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        // Exactly 6 bytes (one more asked for, so a longer file shows), a unicast address.
        if (error || resid != 1 || !memcmp(staged, zero, 6) || !memcmp(staged, ones, 6) || (staged[0] & 1)) {
            IOLog("SwitchBluetooth: %s is not a BD address (error %d, %d bytes); ignored\n", kStagedAddressPath, error,
                  static_cast<int>(sizeof(staged)) - resid);
            return false;
        }
        memcpy(out, staged, 6);
        return true;
    }

    // Where bluetoothd (/arm-io/bluetooth local-mac-address) and MobileGestalt (/chosen mac-address-bluetooth0) read
    // the address.
    void publishAddressBytes(const uint8_t bytes[6])
    {
        OSData *value = OSData::withBytes(bytes, 6);
        IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
        IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
        if (value && node)
            node->setProperty("local-mac-address", value);
        if (value && chosen)
            chosen->setProperty("mac-address-bluetooth0", value);
        OSSafeReleaseNULL(chosen);
        OSSafeReleaseNULL(node);
        OSSafeReleaseNULL(value);
    }

    // The rails pair seated Joy-Cons to the address bluetoothd reads at its start, before backboardd and
    // gamecontrollerd open their HID devices: pairing replaces a running Joy-Con's device, and done at bluetoothd's
    // first connect it took away both devices gamecontrollerd had just prepared. That connect then finds the same
    // address and pairs nothing.
    void announceAddressToRails()
    {
        const IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
        uint8_t unit[6];
        if (switch_dt_value(node, "local-mac-address", &unit))
            switch_joycon_bluetooth_address(unit);
        OSSafeReleaseNULL(node);
    }

    // The unit's own BD address, as U-Boot derives device_bt_mac (switchroot board/nintendo/switch/nintendo-switch.c
    // generate_and_set_serial): Nintendo's OUI 98:B6:E9, bits 23:16 and 15:8 of FUSE_OPT_LOT_CODE_0, its low byte + 1
    // (+2 is Wi-Fi's). It replaces the packager's shared fallback where bluetoothd (/arm-io/bluetooth
    // local-mac-address) and MobileGestalt (/chosen mac-address-bluetooth0) read it. Runs in switch_bluetooth_start,
    // ahead of launchd and of anything that can sleep, and so (the board thread's first step, seconds before the Data
    // mount) ahead of a staged address, which it never replaces. Left alone when the fuses cannot be read safely or
    // are blank.
    void publishAddress()
    {
        if (__atomic_load_n(&gStagedState, __ATOMIC_ACQUIRE) == kStagedFound)
            return;
        volatile uint8_t *car = switch_mmio_map(kCarPA, 0x1000);
        if (!car || !(switch_mmio_read32(car, CLK_OUT_ENB_H) & kFuseClock) ||
            !(switch_mmio_read32(car, MISC_CLK_ENB) & kFuseVisible))
            return;
        volatile uint8_t *fuse = switch_mmio_map(kFusePA, 0x1000);
        const uint32_t lot0 = fuse ? switch_mmio_read32(fuse, kFuseLotCode0) : 0;
        if (!lot0)
            return;
        const uint8_t unit[6] = {0x98,
                                 0xb6,
                                 0xe9,
                                 static_cast<uint8_t>(lot0 >> 16),
                                 static_cast<uint8_t>(lot0 >> 8),
                                 static_cast<uint8_t>(lot0 + 1)};
        publishAddressBytes(unit);
        gAddressFromFuses = true;
    }
}

// ---- Setup -------------------------------------------------------------------------------------------------------

bool SwitchBluetooth::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    uart = switch_mmio_map(kUartPA, 0x1000);
    car = switch_mmio_map(kCarPA, 0x1000);
    gpio = switch_mmio_map(kGpioPA, 0x1000);
    pinmux = switch_mmio_map(kPinmuxPA, 0x1000);
    hwLock = IOSimpleLockAlloc();
    cmdLock = IOSimpleLockAlloc();
    stateLock = IOLockAlloc();
    sendGate = IOLockAlloc();
    hciLock = IOLockAlloc();
    txLock = IOLockAlloc();
    rx.data = static_cast<uint8_t *>(IOMalloc(kRingBytes));
    tx.data = tegra_apbdma_alloc(kRingBytes, &txBuf) ? txBuf.va : static_cast<uint8_t *>(IOMalloc(kRingBytes));
    loop = IOWorkLoop::workLoop();
    stuckCall = thread_call_allocate(&SwitchBluetooth::stuck, this);
    nanoseconds_to_absolutetime(kIdleStormNs, &idleStormAbs);
    nanoseconds_to_absolutetime(static_cast<uint64_t>(kStatsMs) * 1000000ULL, &statsWindow);
    if (!uart || !car || !gpio || !pinmux || !hwLock || !stateLock || !hciLock || !txLock || !rx.data || !tx.data ||
        !loop || !stuckCall || !cmdLock || !sendGate) {
        IOLog("SwitchBluetooth: out of memory or mappings\n");
        return false;
    }
    uart += kUartD;

    // The packager's /arm-io/bluetooth node (it is also the IOService bluetoothd finds by name). The patch must be
    // whole Write_RAM records ending in Launch_RAM before the chip ever sees it.
    if (const IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane)) {
        const OSData *data = switch_dt_data(node, "switch,patchram");
        if (data && validPatch(static_cast<const uint8_t *>(data->getBytesNoCopy()), data->getLength())) {
            patch = data;
            patch->retain();
        } else if (data) {
            IOLog("SwitchBluetooth: switch,patchram is not a .hcd record list (%u bytes); ignored\n",
                  data->getLength());
        }
        if (switch_dt_value(node, "local-mac-address", &address)) {  // the unit's, see publishAddress()
            haveAddress = true;
            if (__atomic_load_n(&gStagedState, __ATOMIC_ACQUIRE) == kStagedFound && !memcmp(address, gStaged, 6)) {
                addressFrom = "staged";
            } else if (gAddressFromFuses) {
                addressFrom = "fuses";
            }
        }
        node->release();
    }

    setupPins();
    setupUart();

    // UART-D's level interrupt (switch_interrupt_nub sleeps until ArmCpu has the GIC up).
    IOService *nub = switch_interrupt_nub(kIntid, false);
    irq = nub ? IOFilterInterruptEventSource::filterInterruptEventSource(this, &SwitchBluetooth::action,
                                                                         &SwitchBluetooth::filter, nub, 0)
              : nullptr;
    if (!irq || loop->addEventSource(irq) != kIOReturnSuccess) {
        char gic[160];
        switch_interrupt_describe(kIntid, gic, sizeof(gic));
        panic("SwitchBluetooth: cannot attach the UART-D interrupt (INTID %u): %s", kIntid, gic);
    }
    // RX interrupts stay off: setupPins left the core unpowered (see power()); the first flush after power-on enables them.
    irq->enable();
    // TX by DMA (header): the ring is sent by PIO without it.
    char why[128] = "the TX ring is not DMA-able";
    if (txBuf.va)
        txDma = tegra_apbdma_open(TEGRA_APBDMA_CH_UARTD_TX, loop, &SwitchBluetooth::txDone, this, why, sizeof(why));
    if (!txDma)
        IOLog("SwitchBluetooth: TX DMA off (%s): TX by THR-empty interrupt\n", why);
    // RX by DMA (header "RX by APB DMA"): PIO without it. DMA mode starts at the first flush after power-on.
    char rxWhy[128] = "no DMA-able memory for its buffer";
    if (tegra_apbdma_alloc(SWITCH_BT_RX_DMA_WORDS * 4, &rxBuf))
        rxDma = tegra_apbdma_open(TEGRA_APBDMA_CH_UARTD_RX, loop, &SwitchBluetooth::rxDone, this, rxWhy, sizeof(rxWhy));
    if (!rxDma)
        IOLog("SwitchBluetooth: RX DMA off (%s): RX by interrupt per FIFO trigger\n", rxWhy);

    tio.c_iflag = IGNPAR;
    tio.c_cflag = CS8 | CREAD | CLOCAL | CRTSCTS;
    tio.c_ispeed = tio.c_ospeed = kFastBaud;
    registerService();
    return true;
}

// UART pads to the UART (GPIO CNF 0 = special function), BT_WAKE asserted (low) and BT_REG_ON low (core off),
// host wake an input. Masked GPIO writes only; nxk Pads::setup and power() order.
void SwitchBluetooth::setupPins()
{
    for (const Pad &pad : kPads)
        switch_mmio_rmw32(pinmux, pad.offset, PM_LOW, pad.value);
    for (unsigned p = PI4; p < PI4 + 4; ++p)
        setPin(p, GPIO_CNF, false);
    setPin(PH3, GPIO_OUT, false);
    setPin(PH3, GPIO_OE, true);
    setPin(PH3, GPIO_CNF, true);
    setPin(PH4, GPIO_OUT, false);
    setPin(PH4, GPIO_OE, true);
    setPin(PH4, GPIO_CNF, true);
    setPin(PH5, GPIO_OE, false);
    setPin(PH5, GPIO_CNF, true);
    (void)switch_mmio_read32(gpio, portReg(PH4, GPIO_OUT));
}

// Reset and clock UART-D, then hekate uart_init's order with Linux tegra_uart_hw_init's FIFO setup: 8N1, FIFOs on
// (RX trigger 16, TX trigger 16), 115200, hardware flow control. Interrupts stay off until the event source is up.
//
// RX trigger (r15-offload): above 16 bytes, Linux serial-tegra's choice for RX by PIO (UART_FCR_R_TRIG_11), not 8. A
// controller packet then takes about a third fewer interrupts (the host model in test/switch_bluetooth_rx_test.cpp: a
// Joy-Con's 59-byte HID report 6 -> 4, an LE advertising report 5 -> 3, a 300-byte ACL burst 18 -> 9.5). A packet's
// last bytes below the trigger still come by the RX timeout, 4 characters (13 us at 3 Mbaud) after the last one, as
// before for any packet that did not end on a trigger: no byte waits longer than that timeout plus the interrupt's own
// latency (19 us in the model, the same worst case as at 8). The FIFO's other 15 bytes are 50 us at 3 Mbaud; past
// that RTS holds the chip (MCR RTS_EN), as when the ring is full, so nothing is lost.
void SwitchBluetooth::setupUart(bool rts)
{
    switch_mmio_write32(car, RST_DEV_U_SET, kUartDBit);
    switch_mmio_write32(car, CLK_ENB_U_SET, kUartDBit);
    switch_mmio_write32(car, CLK_SOURCE_UARTD, 0);
    IODelay(10);
    switch_mmio_write32(car, RST_DEV_U_CLR, kUartDBit);
    (void)switch_mmio_read32(car, RST_DEVICES_U);

    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    switch_mmio_write32(uart, IER, 0);
    switch_mmio_write32(uart, LCR, LCR_8N1);
    switch_mmio_write32(uart, IRDA_CSR, 0);  // no line inversion left behind
    switch_mmio_write32(uart, FCR, FCR_ENABLE);
    (void)switch_mmio_read32(uart, SPR);
    IODelay(20);
    setBaudLocked(kInitBaud);
    switch_mmio_write32(uart, FCR, FCR_ENABLE | FCR_CLEAR_RX | FCR_CLEAR_TX | FCR_RX_TRIG_16 | FCR_TX_TRIG_16);
    switch_mmio_write32(uart, MCR, rts ? MCR_CTS_EN | MCR_RTS_EN | MCR_RTS : MCR_CTS_EN);
    (void)switch_mmio_read32(uart, SPR);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    IODelay(100);
}

// hekate clock_uart_use_src_div + uart_init. 3 M and 1 M come from PLLP through the fractional divider (8.5 ->
// 48 MHz, 25.5 -> 16 MHz, 16x oversampling, divisor latch 1); other rates from undivided PLLP and the divisor latch.
// IER and the data registers are banked behind DLAB, so the filter must not run meanwhile (hwLock held).
void SwitchBluetooth::setBaudLocked(uint32_t rate)
{
    uint32_t divisor = 1;
    if (rate == 3000000) {
        switch_mmio_write32(car, CLK_SOURCE_UARTD, kUartDivEnable | 15);
    } else if (rate == 1000000) {
        switch_mmio_write32(car, CLK_SOURCE_UARTD, kUartDivEnable | 49);
    } else {
        switch_mmio_write32(car, CLK_SOURCE_UARTD, 2);
        divisor = (8 * rate + kPllpHz) / (16 * rate);
    }
    switch_mmio_write32(uart, LCR, LCR_DLAB | LCR_8N1);
    switch_mmio_write32(uart, THR, divisor & 0xff);
    switch_mmio_write32(uart, IER, (divisor >> 8) & 0xff);
    switch_mmio_write32(uart, LCR, LCR_8N1);
    (void)switch_mmio_read32(uart, SPR);
    baud = rate;
}

// As serial-tegra's tegra_uart_set_termios: latched interrupts cleared and IER 0 while the clock and divisor change and
// for the two characters at the new rate that tegra_set_baudrate waits, then IER back. r7h's storms (kIdleStorm idle
// interrupts once per attempt, around the 3 Mbaud switch) most likely began at a rate change under live RX interrupts.
void SwitchBluetooth::setHostBaud(uint32_t rate)
{
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    clearInterruptsLocked();
    switch_mmio_write32(uart, IER, 0);
    setBaudLocked(rate);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    IODelay(2 * 10 * 1000000 / rate + 20);
    s = IOSimpleLockLockDisableInterrupt(hwLock);
    switch_mmio_write32(uart, IER, ier);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
}

// ---- Interrupt path ----------------------------------------------------------------------------------------------

// Primary interrupt context on CPU0 (every SPI targets it). Services the UART until IIR reports nothing pending, so
// the level line drops; returns true when the work loop has something to deliver or a writer to wake. RX is bounded
// to kIrqRxBudget bytes per interrupt: once that is spent (or the ring fills) drainRx masks the RX interrupts and
// rxStalled is set, so the level line drops and this cannot be re-entered until deliver() drains the ring and
// re-enables RX. A real producer/consumer hand-off, no re-fire storm, no polling: CPU0 is never pinned here, so the
// GPU fence interrupt it shares CPU0 with is not starved. Nor by a condition that no read clears: that masks RX (and,
// should something else be left, every source) before the filter returns, so the line drops either way.
//
// Received bytes wake the work loop once per controller packet in client mode, not at every RX FIFO trigger (16 bytes,
// ~53 us at 3 Mbaud: about 60 wakeups for one 1021-byte ACL packet of an A2DP stream): the filter frames the H4
// stream it puts in the ring (frameByteLocked) and wakes the work loop when a packet has ended there, when the line
// went idle (RX timeout: the controller paused, a packet's last bytes below the FIFO trigger come this way anyway), and
// for whatever the work loop must see at once (RX stalled, a fault, a line error, framing it could not follow). Bytes
// in the middle of a packet stay in the ring until its end, which is the earliest the client could use them: no packet
// reaches bluetoothd later than before. frameRx's framing is the reference, and deliver() puts the filter's back in step
// with it whenever the ring is empty (syncFramerLocked). The kernel's own HCI (bring-up) keeps a wakeup per interrupt.
//
// DMA mode (header "RX by APB DMA"): a receive condition (EORD, RX timeout, line status) masks the RX sources as
// serial-tegra's tegra_uart_isr does (RDI written on, then every RX source off: that clears EORD); once IIR is clear
// what the channel moved is taken (rxDmaIdleLocked) and the sources come back.
bool SwitchBluetooth::filter(OSObject *owner, IOFilterInterruptEventSource *)
{
    auto *self = static_cast<SwitchBluetooth *>(owner);
#if DEVELOPMENT || DEBUG
    const uint64_t entered = mach_absolute_time();
#endif
    bool work = false;
    bool rxWork = false;
    bool idle = false;
    bool done = false;
    bool dmaRx = false;
    bool dmaLineError = false;
    bool dmaStuck = false;
    IOSimpleLockLock(self->hwLock);
    ++self->irqs;
    const uint32_t rxBefore = self->rx.head;
    const uint32_t txBefore = self->tx.tail;
    const uint32_t resetsBefore = self->rxResets;
    const uint32_t faultsBefore = self->rxFaults;
    const uint32_t errorsBefore = self->lineErrors;
    uint32_t budget = kIrqRxBudget;
    for (unsigned round = 0; round < kFilterRounds && !done; ++round) {
        const uint32_t iir = switch_mmio_read32(self->uart, IIR);
        if (iir & IIR_NO_INT) {
            done = true;
            break;
        }
        const uint32_t id = (iir >> 1) & 7;
        if (self->rxDmaOn && id != ID_TX && id != ID_MODEM) {
            if (!dmaRx) {
                switch_mmio_write32(self->uart, IER, self->ier | IER_RDI);
                switch_mmio_write32(self->uart, IER, self->ier & ~(IER_RX | IER_EORD));
            }
            dmaRx = true;
            idle |= id == ID_EORD || id == ID_RX_TIMEOUT;
            if (id == ID_LINE) {
                const uint32_t lsr = switch_mmio_read32(self->uart, LSR);  // the read clears the error bits
                if (lsr & LSR_ERRORS) {
                    ++self->lineErrors;
                    dmaLineError = true;
                }
                // T210's BI/FIFOE that stays with the FIFO empty (drainRx): PIO's FIFO reset and fault counting take
                // it.
                dmaStuck |= ((lsr & LSR_DR) == 0u) && ((lsr & (LSR_BI | LSR_FIFOE)) != 0u);
            }
            continue;
        }
        switch (id) {
        case ID_TX:
            work |= self->fillTx();
            break;
        case ID_RX_TIMEOUT:
            idle = true;
            [[fallthrough]];
        case ID_RX:
        case ID_LINE:
        case ID_EORD:
            rxWork |= self->drainRx(budget, true);
            break;
        case ID_MODEM:
            (void)switch_mmio_read32(self->uart, MSR);
            break;
        default:
            break;  // 5 and 7: T210's break indications (serial-tegra), receive side like the line status
        }
        // RX masked (budget spent, ring full, or a fault): the work loop drains and, but for a fault, re-enables it.
        if (self->rxStalled || self->rxFault)
            done = true;
    }
    if (dmaRx)
        rxWork |= self->rxDmaIdleLocked(idle, dmaLineError);
    if (dmaStuck) {
        self->leaveRxDmaLocked(true);  // PIO until the next flush; its next interrupt resets the FIFO (drainRx)
        rxWork = true;
    }
    // The middle of a packet: its end wakes the work loop (above).
    if (rxWork && self->framePackets && !self->rxFrame.lost && !self->rxPacketDone && !idle && !self->rxStalled &&
        !self->rxFault && self->lineErrors == errorsBefore)
        rxWork = false;
    if (rxWork)
        self->rxPacketDone = false;
    work |= rxWork;
    if (!done) {
        // kFilterRounds conditions and IIR still reports one. All but THRE and modem status (both cleared above) are
        // receive conditions, so RX goes off; anything left after that takes every source with it.
        self->maskRxFaultLocked("an IIR condition no read clears");
        if (!(switch_mmio_read32(self->uart, IIR) & IIR_NO_INT)) {
            self->ier = 0;
            switch_mmio_write32(self->uart, IER, 0);
        }
    }
    // Counted for describe(): a steady rate of these is an interrupt source the loop above does not clear. kIdleStorm
    // of them in a row within kIdleStormNs is the line re-firing with nothing to clear: RX goes off, then every source.
    if (self->rx.head == rxBefore && self->tx.tail == txBefore && self->rxResets == resetsBefore &&
        self->rxFaults == faultsBefore) {
        ++self->idleIrqs;
        const uint64_t now = mach_absolute_time();
        if (!self->idleRun++)
            self->idleSince = now;
        if (self->idleRun >= kIdleStorm) {
            if (now - self->idleSince < self->idleStormAbs) {
                if (!self->stormCleared) {  // serial-tegra's way to drop latched interrupts first
                    self->clearInterruptsLocked();
                    self->stormCleared = true;
                } else if (!self->rxFault) {
                    self->maskRxFaultLocked("an interrupt storm with IIR clear");
                } else {
                    self->ier = 0;
                    switch_mmio_write32(self->uart, IER, 0);
                }
            }
            self->idleRun = 0;
        }
    } else {
        self->idleRun = 0;
        self->stormCleared = false;
    }
    work |= self->rxFaults != faultsBefore;  // the work loop runs once more; RX comes back at a flush or a send
    work |= self->rxDmaFailed != nullptr;    // its one line
#if DEVELOPMENT || DEBUG
    self->isrTicks += mach_absolute_time() - entered;
#endif
    IOSimpleLockUnlock(self->hwLock);
    if (work)
        self->actionDue = true;
    return work;
}

// hwLock held. Reading LSR clears its error bits, so they are counted here; a byte with an error is still kept
// (H4 resynchronises on the next packet type). The FIFO is drained until it empties, the ring fills, or this
// interrupt's byte budget is spent. In the latter two cases the RX interrupts are masked and rxStalled is set: the
// rest stays in the FIFO, where RTS holds the chip, and deliver() drains the ring and re-enables RX. Bounding the
// batch keeps one interrupt short so CPU0 is not pinned by a sender that outruns the slow UART MMIO. With
// stallWhenSpent false (resumeRx) a spent budget just stops the drain and RX stays on: only a full ring stalls.
// A break character, or a stuck BI/FIFOE that the FIFO reset does not cure, is a fault event (rxFaultEventLocked).
bool SwitchBluetooth::drainRx(uint32_t &budget, bool stallWhenSpent)
{
    uint32_t head = rx.head;
    const uint32_t tail = __atomic_load_n(&rx.tail, __ATOMIC_ACQUIRE);
    bool any = false;
    uint32_t lsr = 0;
    while ((lsr = switch_mmio_read32(uart, LSR)) & LSR_DR) {
        if (lsr & LSR_ERRORS)
            ++lineErrors;
        if (head - tail == kRingBytes || budget == 0) {
            if (head - tail == kRingBytes || stallWhenSpent) {
                ier &= ~IER_RX;
                switch_mmio_write32(uart, IER, ier);
                rxStalled = true;
            }
            break;
        }
        const uint8_t byte = static_cast<uint8_t>(switch_mmio_read32(uart, THR));
        rx.data[head++ & (kRingBytes - 1)] = byte;
        if (lsr & LSR_ERRORS)
            rxFrame.lost = true;  // the byte may not be the one sent: framing unknown till the sync
        frameByteLocked(byte);
        --budget;
        any = true;
        if (lsr & LSR_BI) {  // the line was low for a whole character: nothing drives it
            if (rxFaultEventLocked("the line held low (break)"))
                break;
        } else if (!(lsr & LSR_ERRORS)) {
            rxFaultRun = 0;  // a good byte: what came before was a glitch
        }
    }
    // T210 (serial-tegra tegra_uart_decode_rx_error): a break, or an errored character, can leave BI or FIFOE set with
    // the FIFO empty, and the line-status interrupt then stays asserted until the RX FIFO is reset. Reset once; the
    // same again with no good byte in between is the line itself (held low), and masks RX instead.
    if (!rxFault && !(lsr & LSR_DR) && (lsr & (LSR_BI | LSR_FIFOE)) && !rxFaultEventLocked("a stuck BI/FIFOE")) {
        resetRxFifoLocked();
        ++rxResets;
    }
    rxBytes += head - rx.head;
    __atomic_store_n(&rx.head, head, __ATOMIC_RELEASE);
    return any || rxStalled || rxFault;
}

// hwLock held. One more byte of the controller's H4 stream in the ring (filter): rxPacketDone when it ends a packet.
// Anything that is not an H4 packet type loses the framing until the next sync.
void SwitchBluetooth::frameByteLocked(uint8_t byte)
{
    if (switch_bt_h4_frame_byte(&rxFrame, byte))
        rxPacketDone = true;
}

// hwLock held, work loop, client mode, the ring empty: the filter's framing set to frameRx's, the reference. Between
// packets, or in the payload of one with rxPacketLeft bytes still to come; lost while frameRx's is.
void SwitchBluetooth::syncFramerLocked()
{
    rxFrame = {};
    rxPacketDone = false;
    rxFrame.lost = framingLost;
    if (rxPacketLeft) {
        rxFrame.state = 2;
        rxFrame.left = rxPacketLeft;
    }
}

// hwLock held. One break or stuck-error event; true when it masked RX: kFaultRun of them with no good byte between.
bool SwitchBluetooth::rxFaultEventLocked(const char *what)
{
    if (++rxFaultRun < kFaultRun)
        return false;
    maskRxFaultLocked(what);
    return true;
}

// hwLock held. The receiver reports a condition no read or FIFO reset clears: the line held low (a core that is not
// driving its TX, e.g. not yet up or gone), an IIR condition the filter cannot clear, or a line that re-fires with
// none. Left enabled, its level line re-fires the moment the filter returns and pins CPU0, and with it whatever thread
// ran there: a flush or the work loop thread, either holding the work-loop gate, so every later runAction (bring-up,
// connect) waits for good. RX stays off until the next flush (every bring-up step, every connect) or send (kickTx);
// ctlConnect brings the chip up again if it faulted since. checkDelivery sees RX disabled: no lost interrupt.
void SwitchBluetooth::maskRxFaultLocked(const char *what)
{
    leaveRxDmaLocked(true);  // DMA mode too ends here; the next flush starts it again
    ier &= ~IER_RX;
    switch_mmio_write32(uart, IER, ier);
    if (!rxFault)
        ++rxFaults;
    rxFault = true;
    rxFaultRun = 0;
    lastFault = what;
}

// hwLock held. serial-tegra's tegra_uart_set_termios before it changes the configuration: "Clear all interrupts" by
// writing IER with RDI set, then 0, reading each back; IER as it was afterwards.
void SwitchBluetooth::clearInterruptsLocked()
{
    switch_mmio_write32(uart, IER, ier | IER_RDI);
    (void)switch_mmio_read32(uart, IER);
    switch_mmio_write32(uart, IER, 0);
    (void)switch_mmio_read32(uart, IER);
    switch_mmio_write32(uart, IER, ier);
}

// hwLock held. Tops the TX FIFO up from the ring; THRI stays enabled while the ring has bytes. With TX DMA (header): a
// transfer in flight owns the tail (nothing to do until txDone); from an aligned tail, whole 16-byte bursts up to the
// ring's end start one; otherwise PIO takes the bytes up to the next 4-byte boundary, or the last ones below 16.
bool SwitchBluetooth::fillTx()
{
    if (txDmaBytes) {
        if (ier & IER_THRI) {  // the FIFO stays below the trigger while the channel fills it
            ier &= ~IER_THRI;
            switch_mmio_write32(uart, IER, ier);
        }
        return txWanted;
    }
    uint32_t tail = tx.tail;
    const uint32_t head = __atomic_load_n(&tx.head, __ATOMIC_ACQUIRE);
    uint32_t stop = head;
    if (txDma && !txDmaOff && tail != head) {
        const uint32_t at = tail & (kRingBytes - 1);
        const uint32_t run = head - tail < kRingBytes - at ? head - tail : kRingBytes - at;
        if (!(at & 3) && run >= 16) {
            txDmaBytes = run & ~15u;
            switch_dcache_clean(tx.data + at, txDmaBytes);
            if (ier & IER_THRI) {
                ier &= ~IER_THRI;
                switch_mmio_write32(uart, IER, ier);
            }
            tegra_apbdma_start_once(txDma, static_cast<uint32_t>(kUartPA + kUartD + THR), TEGRA_APBDMA_REQ_UARTD,
                                    txBuf.pa + at, txDmaBytes);
            return txWanted;
        }
        const uint32_t piece = (at & 3) ? 4 - (at & 3) : run;  // to the boundary, or the short rest of the run
        stop = tail + (piece < run ? piece : run);
    }
    while (tail != stop && !(switch_mmio_read32(uart, LSR) & LSR_TXFIFO_FULL))
        switch_mmio_write32(uart, THR, tx.data[tail++ & (kRingBytes - 1)]);
    txBytes += tail - tx.tail;
    __atomic_store_n(&tx.tail, tail, __ATOMIC_RELEASE);
    if (tail == head && (ier & IER_THRI)) {
        ier &= ~IER_THRI;
        switch_mmio_write32(uart, IER, ier);
    }
    return txWanted;
}

// Thread context: starts transmission of what was just queued and arms THRI for the rest. RX masked for a fault comes
// back here too: what is sent now gets an answer, and reading it is what clears a receiver left waiting on data. The
// fault, if it is still there, masks RX again within an interrupt or two (one more fault per send at most).
void SwitchBluetooth::kickTx()
{
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    (void)fillTx();
    uint32_t want = ier;
    if (tx.used() && !txDmaBytes)
        want |= IER_THRI;  // a DMA transfer in flight ends by its own interrupt (txDone)
    if (rxFault && chipOn && !rxStalled) {
        want |= IER_RX;
        rxFault = false;
        rxFaultRun = 0;
        stormCleared = false;
    }
    if (want != ier) {
        ier = want;
        switch_mmio_write32(uart, IER, ier);
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
}

// Primary interrupt context, CPU0 (TegraAPBDMA.h): channel 2's end of count. The transfer's bytes leave the ring, what
// follows starts (the next transfer or PIO with THRI), and a waiting writer is woken through the work loop.
void SwitchBluetooth::txDone(void *context)
{
    auto *self = static_cast<SwitchBluetooth *>(context);
#if DEVELOPMENT || DEBUG
    const uint64_t entered = mach_absolute_time();
#endif
    IOSimpleLockLock(self->hwLock);
    if (self->txDmaBytes) {
        __atomic_store_n(&self->tx.tail, self->tx.tail + self->txDmaBytes, __ATOMIC_RELEASE);
        self->txBytes += self->txDmaBytes;
        self->txDmaBytes = 0;
        ++self->txDmaRuns;
    }
    (void)self->fillTx();
    if (self->tx.used() && !self->txDmaBytes && !(self->ier & IER_THRI)) {
        self->ier |= IER_THRI;
        switch_mmio_write32(self->uart, IER, self->ier);
    }
    const bool wake = self->txWanted;
#if DEVELOPMENT || DEBUG
    self->isrTicks += mach_absolute_time() - entered;
#endif
    IOSimpleLockUnlock(self->hwLock);
    if (wake) {
        self->actionDue = true;
        self->irq->signalInterrupt();
    }
}

// hwLock held: a never-booted check failed (header "TX by APB DMA"): the channel stopped, the bytes it moved off the
// ring, PIO (THRI) for the rest of the boot. The caller logs it, unlocked.
void SwitchBluetooth::txDmaFailedLocked(uint32_t moved)
{
    tegra_apbdma_stop(txDma);
    if (moved > txDmaBytes)
        moved = txDmaBytes;
    __atomic_store_n(&tx.tail, tx.tail + moved, __ATOMIC_RELEASE);
    txBytes += moved;
    txDmaBytes = 0;
    txDmaOff = true;
    (void)fillTx();
    if (tx.used() && !(ier & IER_THRI)) {
        ier |= IER_THRI;
        switch_mmio_write32(uart, IER, ier);
    }
}

// ---- RX by APB DMA (header "RX by APB DMA") -------------------------------------------------------------------------

// hwLock held, DMA mode. What the transfer shows written since the last take (cache lines invalidated first: the
// channel does not snoop), a byte a word, into the ring and framing as drainRx puts them. True when bytes were taken. A
// ring with no room for all of it stalls RX: the rest stays in rxBuf for resumeRx and no transfer starts meanwhile, so
// the FIFO fills and RTS holds the chip. The first packet after power-on is looked at once a boot (never booted: it is
// the controller's answer to HCI Reset, an event); a failure marks DMA off, which the caller then leaves.
bool SwitchBluetooth::takeRxDmaLocked(uint32_t movedBytes, bool ended)
{
    const uint32_t visible = switch_bt_rx_dma_visible(rxDmaLength, movedBytes, ended, rxDmaTaken);
    const uint32_t count = visible - rxDmaTaken;
    if (!count)
        return false;
    uint32_t head = rx.head;
    const uint32_t room = kRingBytes - (head - __atomic_load_n(&rx.tail, __ATOMIC_ACQUIRE));
    const uint32_t n = count < room ? count : room;
    if (n) {
        const uint32_t *words = reinterpret_cast<const uint32_t *>(rxBuf.va) + rxDmaTaken;
        switch_dcache_clean_invalidate(words, static_cast<size_t>(n) * sizeof(*words));
        if (!rxDmaChecked && !rxFrame.lost) {
            rxDmaChecked = true;
            const uint8_t type = static_cast<uint8_t>(words[0]);
            const uint8_t code = n > 1 ? static_cast<uint8_t>(words[1]) : EV_COMMAND_COMPLETE;
            if (rxDmaTaken || type != H4_EVENT ||
                (code != EV_COMMAND_COMPLETE && code != EV_COMMAND_STATUS && code != EV_HARDWARE_ERROR &&
                 code != EV_VENDOR)) {
                snprintf(rxDmaWhy, sizeof(rxDmaWhy), "the first bytes by DMA after power-on were %02x %02x at word %u",
                         type, n > 1 ? static_cast<uint8_t>(words[1]) : 0, rxDmaTaken);
                rxDmaOff = true;
                rxDmaFailed = rxDmaWhy;
            }
        }
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t byte = static_cast<uint8_t>(words[i]);
            rx.data[head++ & (kRingBytes - 1)] = byte;
            frameByteLocked(byte);
        }
        rxDmaTaken += n;
        rxBytes += n;
        rxDmaBytes += n;
        __atomic_store_n(&rx.head, head, __ATOMIC_RELEASE);
    }
    if (n < count && !rxStalled) {
        rxStalled = true;
        ier &= ~(IER_RX | IER_EORD);
        switch_mmio_write32(uart, IER, ier);
    }
    return n != 0;
}

// hwLock held, DMA mode, RX sources masked (filter): a line-idle (EORD, RX timeout) or line-status interrupt. What
// moved is taken, the never-booted FIFO check runs (header), the sources come back unless RX stalled. True when the
// work loop has something.
bool SwitchBluetooth::rxDmaIdleLocked(bool idle, bool lineError)
{
    if (lineError)
        rxFrame.lost = true;  // an errored byte somewhere in what moved: framing unknown till the sync
    bool took = false;
    bool ended = false;
    const uint32_t moved = rxDmaArmed ? tegra_apbdma_moved(rxDma, &ended) : 0;
    if (rxDmaArmed && ended) {
        // Ended, its end of count not taken yet: done here, so what waits in the FIFO moves on now (UART-D's INTID is
        // below the channel's, and its RX timeout would come back first for as long as those bytes waited).
        took = endRxDmaLocked();
        if (idle)
            ++rxDmaIdleTakes;
    } else if (rxDmaArmed) {
        took = takeRxDmaLocked(moved, false);
        if (idle) {
            ++rxDmaIdleTakes;
            if (took)
                rxDmaProven = true;
            // Bytes in the FIFO 4 characters after the line went quiet, the transfer running and not moving: the
            // channel does not serve the UART. Twice in a row with no progress (one that landed just now is not that).
            const bool waiting = (switch_mmio_read32(uart, LSR) & LSR_DR) != 0u;
            if (waiting && rxDmaIdleFull && moved == rxDmaIdleMoved)
                rxDmaFailedLocked("bytes waited in the FIFO across two line-idle interrupts and the channel took none");
            rxDmaIdleFull = waiting;
            rxDmaIdleMoved = moved;
        }
    }
    if (rxDmaOff && rxDmaOn)
        leaveRxDmaLocked(true);
    if (rxDmaOn && !rxStalled)
        switch_mmio_write32(uart, IER, ier);
    return took || lineError || rxStalled || (rxDmaFailed != nullptr);
}

// hwLock held: the running transfer ended (its end of count, or seen ended first by the UART filter or resumeRx): taken
// whole, the next one started unless RX stalled. A start clears an end of count the channel's filter has not taken yet
// (TegraAPBDMA stopLocked), which then finds nothing.
bool SwitchBluetooth::endRxDmaLocked()
{
    rxDmaArmed = false;
    ++rxDmaEnds;
    const bool took = takeRxDmaLocked(rxDmaLength * 4, true);
    if (rxDmaOff)
        leaveRxDmaLocked(true);
    if (!rxStalled)
        armRxDmaLocked();
    return took;
}

// hwLock held: the next transfer, from word 0 (everything before it taken), as long as the framing says
// (SwitchBluetoothRxDma.h switch_bt_rx_dma_length).
void SwitchBluetooth::armRxDmaLocked()
{
    if (!rxDmaOn || rxDmaArmed)
        return;
    rxDmaLength = switch_bt_rx_dma_length(&rxFrame);
    rxDmaTaken = 0;
    rxDmaIdleFull = false;
    tegra_apbdma_start_rx_words(rxDma, static_cast<uint32_t>(kUartPA + kUartD + THR), TEGRA_APBDMA_REQ_UARTD, rxBuf.pa,
                                rxDmaLength);
    rxDmaArmed = true;
    ++rxDmaTransfers;
}

// Primary interrupt context, CPU0 (TegraAPBDMA.h): channel 3's end of count. The transfer is taken whole and the next
// one starts at once (bytes that land meanwhile wait in the FIFO, in order); the work loop wakes for a packet end.
void SwitchBluetooth::rxDone(void *context)
{
    auto *self = static_cast<SwitchBluetooth *>(context);
#if DEVELOPMENT || DEBUG
    const uint64_t entered = mach_absolute_time();
#endif
    IOSimpleLockLock(self->hwLock);
    bool work = false;
    if (self->rxDmaOn && self->rxDmaArmed) {
        const bool took = self->endRxDmaLocked();
        work = (took && (!self->framePackets || self->rxPacketDone || self->rxFrame.lost)) || self->rxStalled ||
               (self->rxDmaFailed != nullptr);
        if (work)
            self->rxPacketDone = false;
    }
#if DEVELOPMENT || DEBUG
    self->isrTicks += mach_absolute_time() - entered;
#endif
    IOSimpleLockUnlock(self->hwLock);
    if (work) {
        self->actionDue = true;
        self->irq->signalInterrupt();
    }
}

// hwLock held: DMA mode on, if the channel is there and nothing keeps RX off. What the FIFO holds goes by PIO first,
// with DMA select still off; from the FCR write on the channel is the FIFO's only reader.
void SwitchBluetooth::enterRxDmaLocked()
{
    if (!rxDma || rxDmaOff || rxDmaOn || !chipOn || rxFault)
        return;
    uint32_t budget = kFifoBytes;
    (void)drainRx(budget, false);
    if (rxFault)
        return;  // the drain found a fault (maskRxFaultLocked): RX stays off until the next flush or send
    switch_mmio_write32(uart, FCR, FCR_ENABLE | FCR_DMA | FCR_RX_TRIG_1 | FCR_TX_TRIG_16);
    rxDmaOn = true;
    rxDmaArmed = false;
    rxDmaLength = rxDmaTaken = 0;
    if (!rxStalled)
        armRxDmaLocked();
    ier = (ier & IER_THRI) | (rxStalled ? 0 : IER_RX_DMA);
    switch_mmio_write32(uart, IER, ier);
}

// hwLock held: back to PIO (RX interrupt per FIFO trigger). DMA select off first, then the read the channel may have
// started lands (kRxDmaSettleUs), then what moved is taken (keep) and the channel stops: PIO sees the rest in order.
void SwitchBluetooth::leaveRxDmaLocked(bool keep)
{
    if (!rxDmaOn)
        return;
    switch_mmio_write32(uart, FCR, FCR_ENABLE | FCR_RX_TRIG_16 | FCR_TX_TRIG_16);
    (void)switch_mmio_read32(uart, LSR);
    IODelay(kRxDmaSettleUs);
    if (keep) {
        bool ended = !rxDmaArmed;
        const uint32_t moved = rxDmaArmed ? tegra_apbdma_moved(rxDma, &ended) : rxDmaLength * 4;
        (void)takeRxDmaLocked(moved, ended);
        const uint32_t left = switch_bt_rx_dma_visible(rxDmaLength, moved, ended, rxDmaTaken) - rxDmaTaken;
        if (left) {  // no room in the ring for them: gone, and the framing with them
            dropped += left;
            rxFrame.lost = true;
        }
    }
    tegra_apbdma_stop(rxDma);
    rxDmaOn = rxDmaArmed = false;
    ier = (ier & IER_THRI) | (chipOn && !rxFault && !rxStalled ? IER_RX : 0);
    switch_mmio_write32(uart, IER, ier);
}

// hwLock held: a never-booted check failed (header "RX by APB DMA"): PIO for the boot; the action logs `why` once.
void SwitchBluetooth::rxDmaFailedLocked(const char *why)
{
    if (rxDmaOff)
        return;
    snprintf(rxDmaWhy, sizeof(rxDmaWhy), "%s", why);
    rxDmaOff = true;
    rxDmaFailed = rxDmaWhy;
    leaveRxDmaLocked(true);
}

void SwitchBluetooth::action(OSObject *owner, IOInterruptEventSource *, int)
{
    auto *self = static_cast<SwitchBluetooth *>(owner);
    self->actionDue = false;  // IOKit unmasks the line when this returns
    ++self->st.actions;
    if (const char *why = __atomic_exchange_n(&self->rxDmaFailed, nullptr, __ATOMIC_ACQ_REL)) {
        char dma[320];
        tegra_apbdma_describe(self->rxDma, dma, sizeof(dma));
        IOLog("SwitchBluetooth: RX DMA off for this boot (%s): RX by interrupt per FIFO trigger; %s\n", why, dma);
    }
    self->deliver();
}

// Work loop. Received bytes go to the kernel's HCI parser during a bring-up, to the client once connected
// (as much as its socket takes; the rest when it reads, ctlRcvd), and are dropped otherwise.
void SwitchBluetooth::deliver()
{
    if (txWanted) {
        IOLockLock(txLock);
        IOLockWakeup(txLock, &tx, false);
        IOLockUnlock(txLock);
    }
again:
    // Completions made here (answerHere) go out between the controller's packets.
    if (mode == kClient && !framingLost && !rxPacketLeft)
        (void)passAnswers();
    for (uint32_t avail = 0; (avail = rx.used());) {
        const uint32_t tail = rx.tail;
        const uint32_t at = tail & (kRingBytes - 1);
        uint32_t chunk = avail < kRingBytes - at ? avail : kRingBytes - at;
        if (mode == kKernel) {
            parse(rx.data + at, chunk);
        } else if (mode == kClient) {
            // One controller packet at a time (frameRx); a completion waits here until it has all arrived.
            if (!framingLost && !rxPacketLeft && (!passAnswers() || !frameRx(avail)))
                break;
            if (!framingLost && chunk > rxPacketLeft)
                chunk = rxPacketLeft;
            if (!framingLost && rxDrop) {  // a completion the client did not ask for (frameRx): taken, not passed on
                rxPacketLeft -= chunk;
                rxDrop = rxPacketLeft != 0;
                __atomic_store_n(&rx.tail, tail + chunk, __ATOMIC_RELEASE);
                continue;
            }
            size_t room = 0;
            if (ctl_getenqueuespace(control, client, &room) != 0)
                room = chunk;  // client gone: drop below
            if (room < chunk)
                chunk = static_cast<uint32_t>(room);
            if (!chunk) {
                if (clientRoom(1))
                    continue;
                break;
            }
            if (ctl_enqueuedata(control, client, rx.data + at, chunk, 0) != 0)
                dropped += chunk;
            if (!framingLost)
                rxPacketLeft -= chunk;
        } else {
            dropped += chunk;
        }
        __atomic_store_n(&rx.tail, tail + chunk, __ATOMIC_RELEASE);
    }
    if (rxStalled && rx.space() >= kRingBytes / 2 && resumeRx())
        goto again;
    if (mode == kClient) {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        if (!rx.used())
            syncFramerLocked();  // nothing in the ring: frameRx's framing is the filter's (filter)
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
#if DEVELOPMENT || DEBUG
        logStats(false);
#endif
    }
}

// Work loop: whether the client's socket takes `need` bytes now. If not, ctlRcvd is asked to run deliver() again once it
// reads; a read between the space query and that flag would not have seen the flag, so the space is asked once more
// after raising it, or the client and the ring would wait on each other with RX masked.
bool SwitchBluetooth::clientRoom(size_t need)
{
    size_t room = 0;
    if (ctl_getenqueuespace(control, client, &room) != 0 || room >= need)
        return true;  // gone: the enqueue drops
    __atomic_store_n(&clientBlocked, true, __ATOMIC_SEQ_CST);
    if (ctl_getenqueuespace(control, client, &room) == 0 && room >= need) {
        __atomic_store_n(&clientBlocked, false, __ATOMIC_SEQ_CST);
        return true;
    }
    return false;
}

// ---- Client-mode completions ---------------------------------------------------------------------------------------
//
// bluetoothd takes this BCM4356A3 for the BCM4355C0 of the J120 table and adds a peer's IRK (MagicPairing's key update
// from AirPods) with Broadcom's RPA vendor command, _BCM_LEMetaAddIrk: opcode 0xFCE9, sub-command 2, IRK, address type,
// address (24 bytes; Android's HCI_VENDOR_BLE_RPA_VSC BTM_BLE_META_ADD_IRK_ENTRY). This firmware evidently answers it
// with the Command Complete of the standard command it maps it to, LE Add Device To Resolving List (0x2027; Command
// Disallowed while LE scanning runs): with the controller's single command credit the vendor command was the only one
// outstanding when that completion came. bluetoothd has no 0x2027 outstanding, and an unknown completion of a
// non-vendor command makes it restart ("Command Complete, orphan event, opcode: 2027", then "Bluetooth error -
// restarting { reason=635 }"; r6k 04:49:39, r7j 17:27:50), every time AirPods connect. A completion of a vendor command
// it did not expect it lets pass. So here a Command Complete or Status for an LE command the client has not
// outstanding, while a vendor command is, is passed on as that vendor command's, status Unknown HCI Command: the answer
// bluetoothd already gets, and copes with, for every other vendor command this controller lacks (its Stack.VSC "status
// 701" lines). The credits in the event stay the controller's.
//
// The firmware also answers 0xFCE9 properly (Command Complete, status 0) and then, tens of ms later, sends that LE
// completion for 0x2027 anyway, with nothing of the client's outstanding (r7m 18:52:29 and :39: bluetoothd's
// startup loads a paired device's IRK, so it restarted on every start). Any Command Complete or Status for an opcode
// the client has not outstanding (and that the rule above does not take) is not passed on at all: bluetoothd sets its
// command credits from every completion it gets and counts them down as it sends, so one whose credit belongs to no
// command of its own (even as the no-op form, opcode 0) would let it send a second command past the controller's
// single slot. One line per opcode per boot. If `sentCmds` ever pushed a command off unanswered, completions it
// cannot place pass as they come, as before, until the client reconnects.
//
// BLE. bluetoothd drives this controller's LE side with the Broadcom vendor commands of J120's BCM4355C0, which this
// firmware (CYW4356A3) implements in part; r7p5 (23:08:34-:55, AirDrop's 21 s advert) and the r7p6 live log show three
// mismatches, and each command bluetoothd sends whole (clientCommand) is looked at for them before it goes out:
// - Advertising. bluetoothd's multi-advertising instances (_BCM_*MultipleAdvInstance*: 0xFD48 create, 0xFD4A set
//   parameters {instance, interval min, interval max, type, channel map, filter policy}, 0xFD4B data, 0xFD4D enable,
//   0xFD4E address) work on this firmware: the same AirDrop advert started at once (status 0) when sharingd slowed it
//   to 181.25 ms. Asked for 30 ms (interval 48, ADV_NONCONN_IND), 0xFD4A got Invalid HCI Command Parameters ("STATUS
//   718"): Core 4.x sets a 100 ms floor (0x00A0) for scannable and non-connectable advertising, which 5.0 removed
//   and bluetoothd's chips are newer than. Each refusal made bluetoothd force-stop its 4 instances twice (0xFD4D
//   enable 0 with no callback, Command Disallowed: "status 712, hlCb" the no-op default) and start again at once,
//   ~140 times a second: 23,608 of those and 2,951 refusals in 21 s, the SwitchBluetooth thread at the top of kprof.
//   So on a controller older than 5.0 (HCI version from Read Local Version), the interval of 0xFD4A and of LE Set
//   Advertising Parameters (0x2006) for those two types is raised to the floor (fixAdvertising): the fastest this
//   controller can advertise them at all, one SwitchRefusal line and a count in the stats.
// - Scanning. bluetoothd filters adverts in the controller with two Broadcom LE meta (0xFCE9) features together: the
//   packet content filter (sub-operations 5 feature selection and 7 service UUID, status 0 here) holding the
//   service UUIDs some sessions scan for, and Apple's matching rules (0x32/0x33/0x1E/0x35: Unknown HCI Command here,
//   "Failed to add rule") passing the Apple-manufacturer adverts the others want, both switched on by 0xFCE9
//   sub-operation 1 (enable features, 0xA043 seen). With the rules missing, only adverts carrying the one UUID
//   asked for passed: 23 scan windows on r7p5 and every window since on r7p6 ended "0 Advertising Events" with
//   filter policy 0, a 30/300 ms window and passive scanning. A controller without those features answers Unknown
//   HCI Command, as this one already does for the matching rules, and bluetoothd then gets every advert and filters
//   in the host; so 0xFCE9 sub-operations 1, 5 and 7 (and the UUID filter's later forms 0x50, 0x61) are answered here
//   with that status and never reach the controller, which keeps its default of reporting every advert
//   (answerHere: a Command Complete with the controller's last credit count, passed on between its packets).
//   A controller that has Android's APCF (0xFD53 at bring-up) takes the filtering after all: bluetoothd's UUIDs,
//   Apple matching rules, address list and duplicate bypass become three APCF filters (SwitchBluetoothLeFilter.h),
//   programmed by injected commands before the scan that uses them, and each of those commands is answered here
//   with success and its sub-operation; the rest stay refused, one SwitchRefusal line per sub-operation.
// - The stats line (logStats, os_log only: at most every kStatsMs and only after traffic, and when the client goes)
//   counts the controller's packets and events by type, LE meta events by sub-event with the advertising reports
//   they carry, the client's commands, those answered here, intervals raised, scan enables, and the completions
//   with a non-zero status by opcode; the first of each LE and vendor command (and the event mask commands) is
//   logged once per boot with its parameters and its completion (traceCommand, traceCompletion).

void SwitchBluetooth::resetClientFraming()
{
    rxPacketLeft = 0;
    framingLost = false;
    rxDrop = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    txFrame = {};
    sentCount = 0;
    sentOverflow = false;
    answerHead = answerTail = 0;
    links = {};  // a new client: the controller has no links (a used one was brought up again)
    leFilter = {};
    leFilter.apcf = __atomic_load_n(&leApcf, __ATOMIC_RELAXED);
    leDirty = leScanning = false;
    switch_wake_bt_reset();  // nor any controller's buttons held
    requests = {};
    role = SWITCH_BT_ROLE_UNKNOWN;
    switch_coex_bt_reset(&coex);  // and none of its scans or priorities
    injectOp = 0;
    coexFlushesSeen = __atomic_load_n(&coexFlushes, __ATOMIC_RELAXED);
    afhMhz = 0;
    afhCheckAt = 0;
    afhLinks = 0;
    afhSet = false;
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    switch_wlan_bt_links(0);
}

// Client thread, before the bytes go out: the commands in the client's H4 stream join `sentCmds`. ACL and SCO
// payloads are skipped by their lengths. Anything that is not H4 turns the bookkeeping off for the connection. Returns
// how many of `bytes` were taken: up to the end of the first packet that ends in them, so that ctlSend can look at a
// whole command that starts next (clientCommand), else all of them.
size_t SwitchBluetooth::noteClientTx(const uint8_t *bytes, size_t length)
{
    if (framingLost)
        return length;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    auto &f = txFrame;
    size_t i = 0;
    while (i < length && !framingLost) {
        if (f.state == 0) {
            f.type = bytes[i++];
            f.need = txHeaderBytes(f.type);
            f.have = 0;
            if (f.need) {
                f.state = 1;
            } else {
                framingLost = true;
            }
        } else if (f.state == 1) {
            f.header[f.have++] = bytes[i++];
            if (f.have < f.need)
                continue;
            f.left = f.type == H4_ACL ? le16(f.header + 2) : f.header[2];
            if (f.type == H4_COMMAND)
                noteCommandLocked(le16(f.header));
            if (f.type == H4_ACL)
                switch_bt_link_tx(&links, switch_bt_acl_handle(f.header));
            f.state = f.left ? 2 : 0;
            if (!f.state)
                break;
        } else {
            const size_t skip = length - i < f.left ? length - i : f.left;
            i += skip;
            f.left -= static_cast<uint32_t>(skip);
            if (!f.left) {
                f.state = 0;
                break;
            }
        }
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    return framingLost ? length : i;
}

// cmdLock held. A command on its way to the controller joins `sentCmds`.
void SwitchBluetooth::noteCommandLocked(uint16_t opcode)
{
    if (opcode == OP_HOST_NUMBER_OF_COMPLETED_PACKETS)
        return;
    if (sentCount == kMaxPending) {  // a command that was never answered: the oldest goes
        memmove(sentCmds, sentCmds + 1, (kMaxPending - 1) * sizeof(sentCmds[0]));
        --sentCount;
        sentOverflow = true;
    }
    sentCmds[sentCount++] = opcode;
}

// cmdLock held. The oldest sent `opcode`, taken off the list; false if there is none.
bool SwitchBluetooth::takePendingLocked(uint16_t opcode)
{
    for (uint32_t i = 0; i < sentCount; ++i) {
        if (sentCmds[i] != opcode)
            continue;
        memmove(sentCmds + i, sentCmds + i + 1, (sentCount - i - 1) * sizeof(sentCmds[0]));
        --sentCount;
        return true;
    }
    return false;
}

// cmdLock held. The oldest pending vendor command, taken off the list; 0 if there is none.
uint16_t SwitchBluetooth::takeVendorLocked()
{
    for (uint32_t i = 0; i < sentCount; ++i) {
        if ((sentCmds[i] >> 10) != kOgfVendor)
            continue;
        const uint16_t opcode = sentCmds[i];
        (void)takePendingLocked(opcode);
        return opcode;
    }
    return 0;
}

// Work loop, client mode, at a packet boundary of the controller's stream: the next packet's length into
// rxPacketLeft. A Command Complete or Status waits until it is all in the ring and is matched against `sentCmds` first
// (top of section). False while what is needed has not arrived; the next interrupt's action comes back here.
bool SwitchBluetooth::frameRx(uint32_t avail)
{
    const uint32_t tail = rx.tail;
    const auto at = [this, tail](uint32_t i) -> uint8_t & {
        return rx.data[(tail + i) & (kRingBytes - 1)];
    };
    uint32_t total = 0;
    switch (at(0)) {
    case H4_EVENT: {
        if (avail < 3)
            return false;
        total = 3u + at(2);
        const uint8_t code = at(1);
        const bool status = code == EV_COMMAND_STATUS;  // Status: status, credits, opcode; Complete: credits, opcode
        const bool completion = status ? at(2) >= 4 : code == EV_COMMAND_COMPLETE && at(2) >= 3;
        // A completion is looked at whole; an LE meta event once its sub-event and report count (bytes 3, 4) are in,
        // and whole if it is a connection complete; a link event ("Links") whole.
        if (code == EV_LE_META && avail < (total < 5 ? total : 5))
            return false;
        const bool linked = !completion && switch_bt_event_wanted(code, code == EV_LE_META && at(2) ? at(3) : 0);
        if ((completion || linked) && avail < total)
            return false;
        if (linked)
            linkEvent(code, at(2));
        ++st.events;
        switch (code) {
        case EV_COMMAND_COMPLETE:
            ++st.cc;
            break;
        case EV_COMMAND_STATUS:
            ++st.cs;
            break;
        case EV_NUM_COMPLETED:
            ++st.nocp;
            break;
        case EV_VENDOR:
            ++st.vendor;
            break;
        case EV_FLUSH_OCCURRED:  // a packet the controller gave up on after the flush timeout ("Coexistence")
            __atomic_fetch_add(&coexFlushes, 1, __ATOMIC_RELAXED);
            ++st.otherEvents;
            break;
        case EV_LE_META: {
            ++st.le;
            const uint8_t sub = at(2) >= 1 ? at(3) : 0;
            if (sub == LE_ADV_REPORT) {
                ++st.leAdv;
                st.leAdvReports += at(2) >= 2 ? at(4) : 0;
            } else if (sub == LE_CONN_COMPLETE || sub == LE_ENH_CONN_COMPLETE) {
                ++st.leConn;
            } else {
                ++st.leOther;
            }
            break;
        }
        default:
            ++st.otherEvents;
            break;
        }
        if (!completion)
            break;
        const uint8_t credits = status ? at(4) : at(3);
        if (credits)
            lastCredits = credits;
        const uint32_t op = status ? 5 : 4;
        const uint16_t opcode = static_cast<uint16_t>(at(op) | static_cast<unsigned>(at(op + 1)) << 8);
        if (!opcode)
            break;  // credits only
        uint16_t vendor = 0;
        bool stray = false;
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
        if (injectOp && opcode == injectOp) {  // our own command's answer ("Coexistence"): never the client's
            uint8_t injected = 0xff;
            if (status) {
                injected = at(3);
            } else if (at(2) >= 4) {
                injected = at(6);
            }
            injectStatus = injected;
            injectLength = 0;
            for (uint32_t i = 7; !status && i < 3u + at(2) && injectLength < sizeof(injectRet); ++i)
                injectRet[injectLength++] = at(i);
            injectOp = 0;
            IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
            thread_wakeup(&injectOp);
            rxDrop = true;
            break;
        }
        if (!takePendingLocked(opcode)) {
            if ((opcode >> 10) == kOgfLe)
                vendor = takeVendorLocked();
            stray = (vendor == 0u) && !sentOverflow;
        }
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        uint8_t *statusByte = nullptr;
        if (status) {
            statusByte = &at(3);
        } else if (at(2) >= 4) {
            statusByte = &at(6);
        }
        const unsigned was = statusByte ? *statusByte : 0;
        if (!stray) {
            uint8_t ret[kTraceReturn];
            uint32_t retLength = 0;
            if (!status && at(2) > 4)
                retLength = at(2) - 4u < kTraceReturn ? at(2) - 4u : kTraceReturn;
            for (uint32_t i = 0; i < retLength; ++i)
                ret[i] = at(7 + i);
            countCompletion(opcode, static_cast<uint8_t>(was));
            if (opcode == OP_READ_BUFFER_SIZE && !was && retLength >= 5)
                aclBuffers = le16(ret + 3);
#if DEVELOPMENT || DEBUG
            traceCompletion(opcode, status, static_cast<uint8_t>(was), ret, retLength);
            linkCompletion(opcode, status, static_cast<uint8_t>(was));
#endif
        }
        if (stray) {  // not passed on (top of section)
            rxDrop = true;
            ++strayDrops;
            bool logged = false;
            for (uint32_t i = 0; i < strayOpCount && !logged; ++i)
                logged = strayOps[i] == opcode;
            if (!logged && strayOpCount < sizeof(strayOps) / sizeof(strayOps[0])) {
                strayOps[strayOpCount++] = opcode;
                IOLog("SwitchBluetooth: the controller sent a Command %s for %#06x (status 0x%02x) with no such "
                      "command of the client's outstanding; not passed on\n",
                      status ? "Status" : "Complete", opcode, was);
            }
            break;
        }
        if (!vendor)
            break;
        at(op) = static_cast<uint8_t>(vendor);
        at(op + 1) = static_cast<uint8_t>(vendor >> 8);
        if (statusByte)
            *statusByte = kUnknownCommand;
        if (!completionFixes++) {
            IOLog("SwitchBluetooth: the controller answered vendor command %#06x with a Command %s for %#06x (status "
                  "%#04x); passed on as Unknown HCI Command for %#06x\n",
                  vendor, status ? "Status" : "Complete", opcode, was, vendor);
        }
        break;
    }
    case H4_ACL: {
        if (avail < 5)
            return false;
        total = 5u + (at(3) | static_cast<uint32_t>(at(4)) << 8);
        // A Nintendo controller's Home and Capture are the console's Home button and screenshot key, and its Home
        // wakes the console (../SwitchSuspend/SwitchWake.h): the packet's start (ACL, L2CAP and HIDP headers, the
        // button bytes) is looked at once it is in the ring; the packet goes on unchanged.
        const uint32_t peek = 1u + SWITCH_WAKE_BT_PEEK < total ? 1u + SWITCH_WAKE_BT_PEEK : total;
        if (avail < peek)
            return false;
        ++st.acl;
        const uint8_t header[2] = {at(1), at(2)};
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
        switch_bt_link_rx(&links, switch_bt_acl_handle(header));
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        uint8_t acl[SWITCH_WAKE_BT_PEEK];
        for (uint32_t i = 1; i < peek; ++i)
            acl[i - 1] = at(i);
        switch_wake_bt_acl(acl, peek - 1);
        break;
    }
    case H4_SCO:
        if (avail < 4)
            return false;
        total = 4u + at(3);
        ++st.sco;
        break;
    default:
        framingLost = true;
        return true;  // not H4: pass everything on as it comes
    }
    rxPacketLeft = total;
    return true;
}

// Client thread (ctlSend): one whole command of the client's, `size` bytes from its H4 type byte, in a buffer of ours.
// Answered here (answerHere), or sent after fixAdvertising; traced and counted either way (BLE section).
errno_t SwitchBluetooth::clientCommand(uint8_t *packet, size_t size)
{
    const uint16_t opcode = le16(packet + 1);
    uint8_t *const params = packet + 4;
    const uint8_t length = packet[3];
    __atomic_fetch_add(&st.commands, 1, __ATOMIC_RELAXED);
    if ((opcode >> 10) == kOgfLe)
        __atomic_fetch_add(&st.leCommands, 1, __ATOMIC_RELAXED);
    if ((opcode >> 10) == kOgfVendor)
        __atomic_fetch_add(&st.vendorCommands, 1, __ATOMIC_RELAXED);
    if (opcode == OP_LE_SET_SCAN_ENABLE && length)
        __atomic_fetch_add(params[0] ? &st.scanOn : &st.scanOff, 1, __ATOMIC_RELAXED);
    if (opcode == OP_LE_SET_SCAN_ENABLE && length) {
        leScanning = params[0] != 0;
        if (leScanning && leDirty && leFilter.apcf)
            programLeFilter();
        switch_le_filter_scan_enable(&leFilter, params, length);
    }
    if (answerHere(opcode, params, length)) {
#if DEVELOPMENT || DEBUG
        traceCommand(opcode, params, length, true);
#endif
        loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(1));  // deliver(): the answer goes out
        return 0;
    }
    if (fixAdvertising(opcode, params, length))
        __atomic_fetch_add(&st.advFixed, 1, __ATOMIC_RELAXED);
    if (opcode == OP_ACCEPT_CONNECTION && acceptAsCentral(params, length))
        __atomic_fetch_add(&st.central, 1, __ATOMIC_RELAXED);
#if DEVELOPMENT || DEBUG
    traceCommand(opcode, params, length, false);
    linkCommand(opcode, params, length);
#endif
    coexCommand(opcode, params, length);
    coexAfh(opcode);
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    noteCommandLocked(opcode);
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    return transmit(packet, size, kWriteMs);
}

// Client thread: true when the command is answered here, not sent (BLE section): 0xFCE9 with a sub-operation in
// kRefusedLeMeta. Its Command Complete, status Unknown HCI Command, waits in `answers` for deliver() to pass it on
// between the controller's packets. Not while the controller's stream is not H4 (no packet boundary to put it at), nor
// with the queue full (bluetoothd has one command out at a time): the controller answers those.
bool SwitchBluetooth::answerHere(uint16_t opcode, const uint8_t *params, uint8_t length)
{
    if (!length || __atomic_load_n(&framingLost, __ATOMIC_RELAXED))
        return false;
    uint8_t sub = 0;
    if (switch_le_filter_take(&leFilter, opcode, params, length, &sub)) {
        leDirty = true;
        if (leScanning && !programLeFilter())
            return answer(opcode, kUnknownCommand, -1);
        return answer(opcode, 0, sub);
    }
    if (opcode != OP_BCM_LE_META)
        return false;
    bool refused = false;
    for (const uint8_t refusedSub : kRefusedLeMeta)
        refused |= params[0] == refusedSub;
    if (!refused)
        return false;
    switch_refusal_k(switch_refusal_key_with(switch_refusal_key("bluetooth", "le-meta"), params[0]), "bluetooth",
                     "le-meta", "0xFCE9 sub-operation %#x refused: %s", params[0],
                     leFilter.apcf ? "not expressible in APCF"
                                   : "no APCF in this controller, adverts filter in the host");
    return answer(opcode, kUnknownCommand, -1);
}

// Client thread: a Command Complete made here, for deliver() to pass on between the controller's packets: the status,
// and for a filtering command the sub-operation bluetoothd's vendor-command handler reads back (sub < 0: none). False
// with the queue full (bluetoothd has one command out at a time): the controller answers.
bool SwitchBluetooth::answer(uint16_t opcode, uint8_t status, int sub)
{
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    const bool room = answerHead - answerTail < sizeof(answers) / sizeof(answers[0]);
    if (room) {
        uint8_t *const e = answers[answerHead++ % (sizeof(answers) / sizeof(answers[0]))];
        e[0] = H4_EVENT;
        e[1] = EV_COMMAND_COMPLETE;
        e[2] = sub < 0 ? 4 : 5;  // credits, opcode, status[, sub-operation]
        e[3] = lastCredits ? lastCredits : 1;
        e[4] = static_cast<uint8_t>(opcode);
        e[5] = static_cast<uint8_t>(opcode >> 8);
        e[6] = status;
        e[7] = static_cast<uint8_t>(sub);
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (room)
        __atomic_fetch_add(&st.answered, 1, __ATOMIC_RELAXED);
    return room;
}

// Client thread: leFilter into the controller's APCF filters, injected between bluetoothd's commands, before the scan
// that uses them starts. A refused APCF command turns the translation off for the boot: APCF disabled, the controller
// passes every advert as before, and bluetoothd's filtering commands are refused again. An injection that could not
// go now (another command outstanding, no answer in time) is not a refusal: the state stays dirty and the next scan
// enable programs it again (the program starts by disabling APCF, so a part-done one leaves every advert passing).
bool SwitchBluetooth::programLeFilter()
{
    constexpr uint8_t kInjectBusy = 0xfe;
    constexpr uint8_t kInjectNotSent = 0xfd;
    constexpr uint8_t kInjectNoAnswer = 0xff;
    switch_le_command commands[SWITCH_LE_MAX_COMMANDS];
    const unsigned n = switch_le_filter_program(&leFilter, commands, SWITCH_LE_MAX_COMMANDS);
    for (unsigned i = 0; i < n; ++i) {
        uint8_t ret[8];
        uint8_t retLength = sizeof(ret);
        const uint8_t status = inject(commands[i].opcode, commands[i].params, commands[i].length, ret, &retLength);
        if (!status)
            continue;
        if (status == kInjectBusy || status == kInjectNotSent || status == kInjectNoAnswer) {
            const char *why = "no answer in time";
            if (status == kInjectBusy) {
                why = "a command outstanding";
            } else if (status == kInjectNotSent) {
                why = "not sent";
            }
            switch_refusal_k(switch_refusal_key_with(switch_refusal_key("bluetooth", "apcf-deferred"), status),
                             "bluetooth", "apcf-deferred",
                             "APCF programming deferred to the next scan (%s): the controller keeps its previous "
                             "filters (or none) until then",
                             why);
            leDirty = true;
            return true;
        }
        IOLog("SwitchBluetooth: APCF sub-code %#x action %#x refused (status %#x): LE adverts filter in the host this "
              "boot\n",
              commands[i].params[0], commands[i].length > 1 ? commands[i].params[1] : 0, status);
        __atomic_store_n(&leApcf, false, __ATOMIC_RELAXED);
        leFilter.apcf = false;
        const uint8_t off[2] = {SWITCH_APCF_ENABLE, 0};
        inject(SWITCH_LE_OP_APCF, off, sizeof(off), nullptr, nullptr);
        return false;
    }
    leDirty = false;
    return true;
}

// Work loop, client mode, at a packet boundary of the controller's stream: the completions answerHere made, passed on.
// False while the client's socket has no room for the next (ctlRcvd brings deliver() back).
bool SwitchBluetooth::passAnswers()
{
    constexpr uint32_t kSlots = sizeof(answers) / sizeof(answers[0]);
    for (;;) {
        uint8_t e[sizeof(answers[0])];
        IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
        const bool any = answerHead != answerTail;
        if (any)
            memcpy(e, answers[answerTail % kSlots], sizeof(e));
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        if (!any)
            return true;
        const size_t length = 3u + e[2];
        if (!clientRoom(length))
            return false;
        if (ctl_enqueuedata(control, client, e, length, 0) != 0)
            dropped += length;
        s = IOSimpleLockLockDisableInterrupt(cmdLock);
        ++answerTail;
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    }
}

// Client thread: a scannable or non-connectable advertising interval below what a controller older than Core 5.0
// takes, raised to it (BLE section); 0xFD4A {instance, min, max, type, ...} and 0x2006 {min, max, type, ...}. True if
// it changed the command.
bool SwitchBluetooth::fixAdvertising(uint16_t opcode, uint8_t *params, uint8_t length)
{
    uint8_t *interval = nullptr;
    uint8_t type = 0;
    if (opcode == OP_BCM_MULTI_ADV_PARAMS && length >= 6) {
        interval = params + 1;
        type = params[5];
    } else if (opcode == OP_LE_SET_ADV_PARAMS && length >= 5) {
        interval = params;
        type = params[4];
    } else {
        return false;
    }
    const uint8_t version = __atomic_load_n(&hciVersion, __ATOMIC_RELAXED);
    if (!version || version >= kHciVersion50 || (type != 2 && type != 3))
        return false;  // ADV_SCAN_IND, ADV_NONCONN_IND
    const uint16_t min = le16(interval);
    const uint16_t max = le16(interval + 2);
    if (min >= kAdvIntervalMinNonConnectable)
        return false;
    const uint16_t newMax = max < kAdvIntervalMinNonConnectable ? kAdvIntervalMinNonConnectable : max;
    interval[0] = static_cast<uint8_t>(kAdvIntervalMinNonConnectable);
    interval[1] = static_cast<uint8_t>(kAdvIntervalMinNonConnectable >> 8);
    interval[2] = static_cast<uint8_t>(newMax);
    interval[3] = static_cast<uint8_t>(newMax >> 8);
    switch_refusal("bluetooth", "adv-interval",
                   "%s advertising interval %u-%u (%u ms) raised to %u-%u (%#06x): this controller (HCI version %u, "
                   "older than 5.0) refuses it below 100 ms",
                   type == 3 ? "non-connectable" : "scannable", min, max, min * 5 / 8, kAdvIntervalMinNonConnectable,
                   newMax, opcode, version);
    return true;
}

#if DEVELOPMENT || DEBUG
namespace {
void hex(char *out, size_t size, const uint8_t *bytes, uint32_t count)
{
    size_t at = 0;
    out[0] = 0;
    for (uint32_t i = 0; i < count && at + 3 < size; ++i)
        at += static_cast<size_t>(snprintf(out + at, size - at, "%02x", bytes[i]));
}
}
#endif

#if DEVELOPMENT || DEBUG
// Client thread: the first LE and vendor command of each opcode (and 0xFCE9 sub-operation) and the event mask
// commands, kept for traceCompletion; one answered here is logged at once. os_log only, once per boot each.
void SwitchBluetooth::traceCommand(uint16_t opcode, const uint8_t *params, uint8_t length, bool answered)
{
    const uint16_t ogf = opcode >> 10;
    bool setup = false;  // the controller settings that shape every link (Links), and what it reports of itself
    for (const uint16_t op : kTracedSetup)
        setup |= opcode == op;
    if (ogf != kOgfLe && ogf != kOgfVendor && opcode != OP_SET_EVENT_MASK && opcode != OP_SET_EVENT_MASK_2 &&
        opcode != OP_WRITE_LE_HOST_SUPPORT && !setup)
        return;
    const uint8_t sub = opcode == OP_BCM_LE_META && length ? params[0] : 0;
    Traced entry = {};
    bool log = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    uint32_t i = 0;
    while (i < tracedCount && (traced[i].opcode != opcode || traced[i].sub != sub))
        ++i;
    if (i == tracedCount && tracedCount < kTraceEntries) {
        Traced &t = traced[tracedCount++];
        t.opcode = opcode;
        t.sub = sub;
        t.length = length;
        memcpy(t.params, params, length < kTraceParams ? length : kTraceParams);
        t.state = answered ? 2 : 1;
        entry = t;
        log = answered;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (!log)
        return;
    char p[2 * kTraceParams + 1];
    hex(p, sizeof(p), entry.params, entry.length < kTraceParams ? entry.length : kTraceParams);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: HCI %04x/%02x %u bytes [%s] -> answered here: Unknown HCI Command", opcode,
           sub, entry.length, p);
}
#endif

#if DEVELOPMENT || DEBUG
// Work loop: the completion of a command traceCommand kept, logged with its parameters (once per boot each).
void SwitchBluetooth::traceCompletion(uint16_t opcode, bool status, uint8_t result, const uint8_t *ret,
                                      uint32_t retLength)
{
    Traced entry = {};
    bool log = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    for (uint32_t i = 0; i < tracedCount && !log; ++i) {
        if (traced[i].opcode != opcode || traced[i].state != 1)
            continue;
        traced[i].state = 2;
        entry = traced[i];
        log = true;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (!log)
        return;
    char p[2 * kTraceParams + 1];
    char r[2 * kTraceReturn + 1];
    hex(p, sizeof(p), entry.params, entry.length < kTraceParams ? entry.length : kTraceParams);
    hex(r, sizeof(r), ret, retLength);
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: HCI %04x/%02x %u bytes [%s] -> Command %s, status %#04x [%s]", opcode,
           entry.sub, entry.length, p, status ? "Status" : "Complete", result, r);
}
#endif

// ---- Coexistence -------------------------------------------------------------------------------------------------
//
// The controller shares the chip's one 2.4 GHz antenna with the Wi-Fi core, which arbitrates between them in hardware.
// What bluetoothd asks of the air is followed here (SwitchCoex.h): the LE scan's interval and window (its duty cycle:
// AirDrop's discovery and the proximity scans move it between 10% and 100%), the classic page and inquiry scans, and
// which ACL it has marked high priority (A2DP). Each change of the scans' parameters or of the priority is one os_log
// line, "SwitchBluetooth: coex: LE scan on 30/300 ms passive (10%), page scan 11/1280 ms, ACL 000b high priority";
// switching the LE scan on and off is only counted (the stats line's "scan on N off M": twice every few seconds).
// SwitchWLAN prints the same state with its links on its stats line (switch_bluetooth_coex).

// Client thread: a command on its way out, looked at for the state above.
void SwitchBluetooth::coexCommand(uint16_t opcode, const uint8_t *params, uint8_t length)
{
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    const unsigned changed = switch_coex_bt_command(&coex, opcode, params, length);
    const switch_coex_bt now = coex;
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (!(changed & (static_cast<unsigned>(SWITCH_COEX_LE_PARAMS) | SWITCH_COEX_CLASSIC_SCAN | SWITCH_COEX_PRIORITY)))
        return;
    char text[160];
    switch_coex_describe(&now, text, sizeof(text));
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: coex: %s", text);
}

// Any thread: the state above and the ACL links ("3 ACL, 1 active: 000b"), false without a client.
bool SwitchBluetooth::coexSnapshot(switch_coex_bt *out, char *linksText, size_t size)
{
    if (mode != kClient || !cmdLock)
        return false;
    uint16_t active[SWITCH_BT_LINKS];
    unsigned acl = 0;
    unsigned le = 0;
    unsigned activeCount = 0;
    uint32_t held = 0;  // the most ACL packets one link had in the controller since the last snapshot
    uint16_t heldBy = 0;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    *out = coex;
    for (switch_bt_link &l : links.link) {
        if (l.kind == SWITCH_BT_LINK_LE)
            ++le;
        if (l.kind == SWITCH_BT_LINK_FREE)
            continue;
        if (l.window > held) {
            held = l.window;
            heldBy = l.handle;
        }
        l.window = l.tx - l.done;  // the next window starts with what it holds now
        if (l.kind != SWITCH_BT_LINK_ACL)
            continue;
        ++acl;
        if (l.mode == 0)
            active[activeCount++] = l.handle;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    const uint32_t flushes = __atomic_load_n(&coexFlushes, __ATOMIC_RELAXED);
    const uint32_t flushed = flushes - coexFlushesSeen;
    coexFlushesSeen = flushes;
    if (!size)
        return true;
    int n = snprintf(linksText, size, "%u ACL", acl);
    size_t at = n > 0 ? static_cast<size_t>(n) : 0;
    if (at < size && le) {
        n = snprintf(linksText + at, size - at, ", %u LE", le);
        at += n > 0 ? static_cast<size_t>(n) : 0;
    }
    for (unsigned i = 0; i < activeCount && at < size; ++i) {
        n = snprintf(linksText + at, size - at, "%s%04x", i ? " " : ", active ", active[i]);
        at += n > 0 ? static_cast<size_t>(n) : 0;
    }
    // Air time: the most packets a link waited with in the controller (of its aclBuffers), and the packets it flushed.
    if (at < size && held) {
        n = snprintf(linksText + at, size - at, ", held at most %u of %u (%04x)", held, aclBuffers, heldBy);
        at += n > 0 ? static_cast<size_t>(n) : 0;
    }
    if (at < size && flushed)
        snprintf(linksText + at, size - at, ", %u flushed", flushed);
    return true;
}

// Client thread (clientCommand), at an H4 packet boundary: one command of ours to the controller while none of the
// client's is outstanding, its Command Complete taken out of the client's stream (frameRx) and waited for, kInjectMs
// at most, before the client's own command goes; bluetoothd keeps one command out at a time, so the controller never
// holds two. Returns the status: 0xfe when the client has a command out, 0xfd when it could not be sent, 0xff when no
// answer came in time (a late one is still taken out).
uint8_t SwitchBluetooth::inject(uint16_t opcode, const uint8_t *params, uint8_t length, uint8_t *ret,
                                uint8_t *retLength)
{
    constexpr uint32_t kInjectMs = 100;
    uint8_t packet[4 + kInjectMax];
    if (length > kInjectMax)
        return 0xfd;
    packet[0] = H4_COMMAND;
    packet[1] = static_cast<uint8_t>(opcode);
    packet[2] = static_cast<uint8_t>(opcode >> 8);
    packet[3] = length;
    memcpy(packet + 4, params, length);
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    const bool busy = (sentCount != 0u) || (injectOp != 0u);
    if (!busy) {
        injectOp = opcode;
        injectStatus = 0xff;
        injectLength = 0;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (busy)
        return 0xfe;
    if (transmit(packet, 4u + length, kWriteMs)) {
        s = IOSimpleLockLockDisableInterrupt(cmdLock);
        injectOp = 0;
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        return 0xfd;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(kInjectMs, kMillisecondScale, &deadline);
    s = IOSimpleLockLockDisableInterrupt(cmdLock);
    while (injectOp == opcode) {
        assert_wait_deadline(&injectOp, THREAD_UNINT, deadline);
        IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
        const wait_result_t w = thread_block(THREAD_CONTINUE_NULL);
        s = IOSimpleLockLockDisableInterrupt(cmdLock);
        if (w == THREAD_TIMED_OUT)
            break;
    }
    const bool answered = injectOp != opcode;
    const uint8_t result = answered ? injectStatus : 0xff;
    uint8_t n = 0;
    if (answered && ret && retLength)
        n = injectLength < *retLength ? injectLength : *retLength;
    if (n)
        memcpy(ret, injectRet, n);
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (retLength)
        *retLength = n;
    return result;
}

// Client thread, before each of the client's commands goes out (SwitchCoex.h, "AFH"). The Wi-Fi firmware hands its
// channel to the Bluetooth core over the chip's internal coexistence bus (Broadcom's si_gci_init: GCI control bits
// 40-47 "contain WL channel info and are sent to BT", with BFL2_BTCLEGACY clear as on this board), so the first step is
// to look: 5 s after the Wi-Fi channel or the set of ACL links changes, each link's AFH map is read (one line). Only
// a link still hopping on the Wi-Fi channel's range gets the host classification (one line), and its maps are read
// again 5 s later. Back off 2.4 GHz, a classification we set is withdrawn (every channel usable). A client's HCI Reset
// clears the controller's classification and starts over; a controller that refuses one is not asked again.
void SwitchBluetooth::coexAfh(uint16_t opcode)
{
    if (opcode == OP_RESET) {
        afhMhz = 0;
        afhCheckAt = 0;
        afhLinks = 0;
        afhSet = false;
        return;
    }
    const uint16_t infra = swlan_awdl_infra_freq();
    const uint16_t want = infra >= 2412 && infra <= 2484 ? infra : 0;
    uint16_t handles[4];
    unsigned count = 0;
    unsigned acl = 0;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    for (const switch_bt_link &l : links.link) {
        if (l.kind != SWITCH_BT_LINK_ACL)
            continue;
        ++acl;
        if (count < 4)
            handles[count++] = l.handle;
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    uint64_t settle = 0;
    nanoseconds_to_absolutetime(5000000000ULL, &settle);  // the controller moves its links over within seconds
    if (want != afhMhz || acl != afhLinks) {
        if (want != afhMhz && afhSet && !afhRefused) {  // our classification is for a channel Wi-Fi left
            uint8_t map[10];
            const unsigned usable = switch_coex_afh_map(want, map, nullptr, nullptr);
            const uint8_t status = inject(SWITCH_COEX_OP_SET_AFH_CLASSIFICATION, map, sizeof(map), nullptr, nullptr);
            if (status == 0xfe)
                return;  // the client has a command out: at its next one
            afhSet = false;
            os_log(OS_LOG_DEFAULT,
                   "SwitchBluetooth: coex: AFH host classification withdrawn (Wi-Fi %u MHz): %u "
                   "channels usable, status %#04x",
                   want, usable, status);
        }
        afhMhz = want;
        afhLinks = acl;
        afhCheckAt = want && acl ? mach_absolute_time() + settle : 0;
        return;
    }
    if (!afhCheckAt || mach_absolute_time() < afhCheckAt)
        return;
    // The links' maps as the controller hops them now (Read AFH Channel Map) against the Wi-Fi channel's range.
    uint8_t map[10];
    unsigned first = 0;
    unsigned last = 0;
    const unsigned usable = switch_coex_afh_map(afhMhz, map, &first, &last);
    char text[200];
    size_t at = 0;
    text[0] = 0;
    bool onWifi = false;
    for (unsigned i = 0; i < count && at < sizeof(text); ++i) {
        const uint8_t params[2] = {static_cast<uint8_t>(handles[i]), static_cast<uint8_t>(handles[i] >> 8)};
        uint8_t ret[13];
        uint8_t n = sizeof(ret);
        const uint8_t status = inject(SWITCH_COEX_OP_READ_AFH_MAP, params, sizeof(params), ret, &n);
        if (status == 0xfe)
            return;  // again at the client's next command
        int k = 0;
        if (status || n < 13) {
            k = snprintf(text + at, sizeof(text) - at, "%s%04x status %#04x", i ? ", " : "", handles[i], status);
        } else {
            unsigned inside = 0;
            const unsigned used = switch_coex_afh_used(ret + 3, first, last, &inside);
            onWifi |= inside > 0;
            k = snprintf(text + at, sizeof(text) - at, "%s%04x AFH %s, %u channels (%u in Wi-Fi's)", i ? ", " : "",
                         handles[i], ret[2] ? "on" : "off", used, inside);
        }
        at += k > 0 ? static_cast<size_t>(k) : 0;
    }
    afhCheckAt = 0;
    os_log(OS_LOG_DEFAULT, "SwitchBluetooth: coex: AFH maps with Wi-Fi at %u MHz (Bluetooth channels %u-%u)%s: %s",
           afhMhz, first, last, afhSet ? ", our classification set" : "", text);
    if (!onWifi || afhSet || afhRefused)
        return;
    const uint8_t status = inject(SWITCH_COEX_OP_SET_AFH_CLASSIFICATION, map, sizeof(map), nullptr, nullptr);
    if (status == 0xfe) {
        afhCheckAt = mach_absolute_time();  // at the client's next command
        return;
    }
    if (status) {
        afhRefused = true;
        IOLog("SwitchBluetooth: coex: AFH host channel classification refused (status %#04x)\n", status);
        return;
    }
    afhSet = true;
    afhCheckAt = mach_absolute_time() + settle;  // and look again
    os_log(OS_LOG_DEFAULT,
           "SwitchBluetooth: coex: AFH host channel classification set: channels %u-%u bad (Wi-Fi %u "
           "MHz), %u usable",
           first, last, afhMhz, usable);
}

extern "C" bool switch_bluetooth_coex(struct switch_coex_bt *state, char *links, size_t size)
{
    SwitchBluetooth *self = gDriver;
    if (size)
        links[0] = 0;
    return (self != nullptr) && self->coexSnapshot(state, links, size);
}

// ---- Links -------------------------------------------------------------------------------------------------------
//
// What every connection does, for a controller that holds several at once (two Joy-Cons and AirPods: two classic HID
// links in sniff and an A2DP one). The table (SwitchBluetoothLinks.h, cmdLock) follows each connection handle from its
// Connection Complete to its Disconnection Complete: the ACL packets each way, the controller's completions for them
// (the credit accounting bluetoothd does across all links from Read Buffer Size's total), the most ever outstanding, the
// role and sniff mode; the stats line carries it. The link trace (os_log, kLinkTraceLines a boot) logs every command
// that makes, accepts, secures, shapes or ends a link, with its Command Status or Complete, and every link event:
// connection request and complete, role and mode changes, link key and IO capability requests, authentication,
// encryption, disconnection (with the link's counts), max slots, supervision timeout, Hardware Error and Data Buffer
// Overflow. A Link Key Request Reply is logged without its key.

#if DEVELOPMENT || DEBUG
// Any thread: one link trace line ("->" to the controller, "<-" from it), with a closed link's counts; the last line of
// the budget says the rest are not logged.
void SwitchBluetooth::linkLog(const char *direction, const char *line, const switch_bt_link *last)
{
    const uint32_t n = __atomic_fetch_add(&linkLines, 1, __ATOMIC_RELAXED);
    if (n > kLinkTraceLines)
        return;
    if (n == kLinkTraceLines) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: link trace: %u lines this boot, no more", kLinkTraceLines);
        return;
    }
    if (last) {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: link %s %s: rx %u tx %u done %u outstanding %u peak %u", direction,
               line, last->rx, last->tx, last->done, last->tx - last->done, last->peak);
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: link %s %s", direction, line);
    }
}
#endif

#if DEVELOPMENT || DEBUG
// Client thread (clientCommand): a link command on its way to the controller.
void SwitchBluetooth::linkCommand(uint16_t opcode, const uint8_t *params, uint8_t length)
{
    if (!switch_bt_link_command_name(opcode))
        return;
    char line[160];
    switch_bt_describe_command(line, sizeof(line), opcode, params, length);
    linkLog("->", line);
}
#endif

#if DEVELOPMENT || DEBUG
// Work loop (frameRx): the Command Status or Complete of a link command.
void SwitchBluetooth::linkCompletion(uint16_t opcode, bool status, uint8_t result)
{
    const char *name = switch_bt_link_command_name(opcode);
    if (!name)
        return;
    char line[96];
    snprintf(line, sizeof(line), "Command %s for %s: 0x%02x", status ? "Status" : "Complete", name, result);
    linkLog("<-", line);
}
#endif

// Work loop (frameRx): a whole link event at the ring's tail (`length` parameter bytes after its 3-byte H4 header),
// into the table and the trace. Number Of Completed Packets is only counted.
void SwitchBluetooth::linkEvent(uint8_t code, uint32_t length)
{
    uint8_t *const p = linkParams;
    const uint32_t tail = rx.tail;
    for (uint32_t i = 0; i < length; ++i)
        p[i] = rx.data[(tail + 3 + i) & (kRingBytes - 1)];
    switch_bt_link closed = {};
    bool wasOpen = false;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    switch (code) {
    case SWITCH_BT_EV_NUM_COMPLETED:
        (void)switch_bt_links_completed(&links, p, length);
        break;
    case SWITCH_BT_EV_CONNECTION_REQUEST:
        if (length >= 10) {
            switch_bt_request_note(&requests, p,
                                   p[6] | static_cast<uint32_t>(p[7]) << 8 | static_cast<uint32_t>(p[8]) << 16);
        }
        break;
    case SWITCH_BT_EV_CONNECTION_COMPLETE:
        if (length >= 11 && !p[0]) {
            (void)switch_bt_link_open(&links, switch_bt_le16(p + 1), SWITCH_BT_LINK_ACL, p + 3,
                                      memcmp(p + 3, roleAddr, 6) ? static_cast<uint8_t>(SWITCH_BT_ROLE_UNKNOWN) : role);
        }
        break;
    case SWITCH_BT_EV_LE_META:
        if (length >= 19 && !p[1]) {
            (void)switch_bt_link_open(&links, static_cast<uint16_t>(switch_bt_le16(p + 2) & 0x0fffu), SWITCH_BT_LINK_LE,
                                      p + 6, p[4]);
        }
        break;
    case SWITCH_BT_EV_DISCONNECTION_COMPLETE:
        if (length >= 4 && !p[0])
            wasOpen = switch_bt_link_close(&links, switch_bt_le16(p + 1), &closed);
        if (length >= 4 && !p[0])
            switch_wake_bt_gone(switch_bt_le16(p + 1));  // its controller's buttons go
        __atomic_fetch_add(&disconnects, 1, __ATOMIC_RELEASE);
        thread_wakeup(&disconnects);
        break;
    case SWITCH_BT_EV_ROLE_CHANGE:
        if (length >= 8 && !p[0]) {
            memcpy(roleAddr, p + 1, 6);
            role = p[7];
            for (switch_bt_link &l : links.link) {
                if (l.kind != SWITCH_BT_LINK_FREE && !memcmp(l.addr, p + 1, 6))
                    l.role = p[7];
            }
        }
        break;
    case SWITCH_BT_EV_MODE_CHANGE: {
        switch_bt_link *l = length >= 6 && !p[0] ? switch_bt_link_find(&links, switch_bt_le16(p + 1)) : nullptr;
        if (l) {
            l->mode = p[3];
            l->interval = switch_bt_le16(p + 4);
        }
        break;
    }
    case SWITCH_BT_EV_ENCRYPTION_CHANGE: {
        switch_bt_link *l = length >= 4 && !p[0] ? switch_bt_link_find(&links, switch_bt_le16(p + 1)) : nullptr;
        if (l)
            l->encrypted = p[3];
        break;
    }
    default:
        break;
    }
    unsigned linkCount = 0;
    const bool linksMayChange = code == SWITCH_BT_EV_CONNECTION_COMPLETE || code == SWITCH_BT_EV_LE_META ||
                                code == SWITCH_BT_EV_DISCONNECTION_COMPLETE;
    if (linksMayChange) {
        for (const switch_bt_link &l : links.link)
            linkCount += static_cast<unsigned int>(l.kind != SWITCH_BT_LINK_FREE);
    }
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (linksMayChange)
        switch_wlan_bt_links(linkCount);  // the Wi-Fi coexistence mode follows ("Coexistence")
#if DEVELOPMENT || DEBUG
    if (code == SWITCH_BT_EV_NUM_COMPLETED)
        return;
    switch_bt_describe_event(linkText, sizeof(linkText), code, p, length);
    linkLog("<-", linkText, wasOpen ? &closed : nullptr);
#else
    (void)wasOpen;
#endif
}

// Client thread (clientCommand): bluetoothd's Accept Connection Request for a gamepad that paged us, Role 1 (stay
// peripheral) made 0 (become central; SwitchBluetoothLinks.h). One line the first time.
bool SwitchBluetooth::acceptAsCentral(uint8_t *params, uint8_t length)
{
    uint32_t cod = 0;
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(cmdLock);
    const bool changed = switch_bt_accept_as_central(&requests, params, length, &cod);
    IOSimpleLockUnlockEnableInterrupt(cmdLock, s);
    if (changed && !__atomic_exchange_n(&centralLogged, true, __ATOMIC_RELAXED)) {
        char a[18];
        switch_bt_format_addr(a, params);
        IOLog(
            "SwitchBluetooth: gamepad %s (class %#08x) accepted as central, so the controller switches roles while it "
            "connects instead of the gamepad after it\n",
            a, cod);
    }
    return changed;
}

// Work loop: a completion with a non-zero status, counted by opcode and status for the stats line.
void SwitchBluetooth::countCompletion(uint16_t opcode, uint8_t result)
{
    if (!result)
        return;
    Failed *slot = nullptr;
    for (Failed &f : failed) {
        if (f.count && f.opcode == opcode && f.status == result) {
            slot = &f;
            break;
        }
        if (!f.count && !slot)
            slot = &f;
    }
    if (!slot)
        return;  // eight kinds already; the stats line shows those
    if (!slot->count) {
        slot->opcode = opcode;
        slot->status = result;
    }
    ++slot->count;
}

#if DEVELOPMENT || DEBUG
// Work loop, client mode: one line for what changed since the last, at most every kStatsMs and only when something
// did (force: whatever the time, as the client goes). os_log only, never the console.
void SwitchBluetooth::logStats(bool force)
{
    const uint64_t now = mach_absolute_time();
    if (!force && now - statsAt < statsWindow)
        return;
    Stats n = st;
    n.commands = __atomic_load_n(&st.commands, __ATOMIC_RELAXED);
    n.leCommands = __atomic_load_n(&st.leCommands, __ATOMIC_RELAXED);
    n.vendorCommands = __atomic_load_n(&st.vendorCommands, __ATOMIC_RELAXED);
    n.answered = __atomic_load_n(&st.answered, __ATOMIC_RELAXED);
    n.advFixed = __atomic_load_n(&st.advFixed, __ATOMIC_RELAXED);
    n.scanOn = __atomic_load_n(&st.scanOn, __ATOMIC_RELAXED);
    n.scanOff = __atomic_load_n(&st.scanOff, __ATOMIC_RELAXED);
    n.central = __atomic_load_n(&st.central, __ATOMIC_RELAXED);
    const uint32_t irqsNow = irqs;
    const uint32_t rxNow = rxBytes;
    const uint32_t txNow = txBytes;
    const uint32_t rxDmaBytesNow = rxDmaBytes;
    const uint32_t rxDmaTransfersNow = rxDmaTransfers;
    const uint32_t rxDmaEndsNow = rxDmaEnds;
    const uint32_t rxDmaIdleTakesNow = rxDmaIdleTakes;
    const uint32_t txDmaRunsNow = txDmaRuns;
    const uint64_t isrTicksNow = __atomic_load_n(&isrTicks, __ATOMIC_RELAXED);
    const Stats &o = stLogged;
    if (n.events == o.events && n.acl == o.acl && n.sco == o.sco && n.commands == o.commands &&
        n.answered == o.answered) {
        statsAt = now;
        return;
    }
    // r16-idle: a window of routine LE scanning only (advertising reports, the LE commands that turn the scan on and
    // off, and their completions: bluetoothd's duty-cycled scan on an idle Home Screen, ~20 reports a second) is
    // logged once a minute with its counts carried over; anything else (ACL, SCO, a vendor command, a connection,
    // another event, a command answered here or failed) at once, as before.
    bool failedMoved = false;
    for (uint32_t i = 0; i < sizeof(failed) / sizeof(failed[0]); ++i)
        failedMoved |= failed[i].count != failedLogged[i].count;
    const bool routine = n.acl == o.acl && n.sco == o.sco && n.vendorCommands == o.vendorCommands &&
                         n.answered == o.answered && n.leConn == o.leConn && n.leOther == o.leOther &&
                         n.vendor == o.vendor && n.otherEvents == o.otherEvents && n.nocp == o.nocp &&
                         n.central == o.central && !failedMoved;
    if (!force && routine && now - statsAt < kStatsRoutineWindows * statsWindow)
        return;
    char fails[200] = "";
    size_t at = 0;
    for (uint32_t i = 0; i < sizeof(failed) / sizeof(failed[0]) && at + 24 < sizeof(fails); ++i) {
        if (failed[i].count == failedLogged[i].count)
            continue;
        at += static_cast<size_t>(snprintf(fails + at, sizeof(fails) - at, " %04x=%#04x x%u", failed[i].opcode,
                                           failed[i].status, failed[i].count - failedLogged[i].count));
    }
    // The links as they are now (totals since each came up; "Links" section): tx - done is what each holds of the
    // controller's ACL buffers.
    switch_bt_links &snapshot = statsLinks;
    const IOInterruptState locked = IOSimpleLockLockDisableInterrupt(cmdLock);
    snapshot = links;
    IOSimpleLockUnlockEnableInterrupt(cmdLock, locked);
    char *const held = statsHeld;
    constexpr size_t kHeld = sizeof(statsHeld);
    held[0] = '\0';
    size_t used = 0;  // what is in `held`; an entry that would not fit ends it (snprintf returns what it wanted)
    for (const switch_bt_link &l : snapshot.link) {
        if (l.kind == SWITCH_BT_LINK_FREE)
            continue;
        char a[18];
        switch_bt_format_addr(a, l.addr);
        const char *role = l.role == 1 ? "peripheral" : "?";
        if (l.role == 0)
            role = "central";
        const int w = snprintf(held + used, kHeld - used,
                               " %04x %s %s role %s %s %u, rx %u tx %u done %u outstanding %u peak %u;", l.handle, a,
                               l.kind == SWITCH_BT_LINK_LE ? "LE" : "ACL", role, l.mode == 2 ? "sniff" : "active",
                               l.interval, l.rx, l.tx, l.done, l.tx - l.done, l.peak);
        if (w < 0 || used + static_cast<size_t>(w) >= kHeld) {
            held[used] = '\0';
            break;
        }
        used += static_cast<size_t>(w);
    }
    if (snapshot.strayTx || snapshot.strayDone) {
        const int w = snprintf(held + used, kHeld - used, " unknown handles: tx %u done %u", snapshot.strayTx,
                               snapshot.strayDone);
        if (w < 0 || used + static_cast<size_t>(w) >= kHeld)
            held[used] = '\0';
    }
    uint64_t ns = 0;
    uint64_t isrNs = 0;
    absolutetime_to_nanoseconds(now - statsAt, &ns);
    absolutetime_to_nanoseconds(isrTicksNow - isrTicksLogged, &isrNs);
    const uint32_t isrBasis =
        ns ? static_cast<uint32_t>(isrNs * 10000 / ns) : 0;  // hundredths of a percent of one core
    char *const line = statsLine;
    snprintf(
        line, sizeof(statsLine),
        "SwitchBluetooth: stats %llu ms: rx %u B, tx %u B, %u interrupts, %u wakeups; from the controller %u events "
        "(complete %u, status %u, completed packets %u, vendor %u, other %u), LE meta %u (advertising reports "
        "%u in %u events, connections %u, other %u), ACL %u, SCO %u; to it %u commands (LE %u, vendor %u), "
        "answered here %u, advertising intervals raised %u, scan on %u off %u, gamepads accepted as central %u; "
        "failed:%s; "
        "links:%s",
        static_cast<unsigned long long>(ns / 1000000), rxNow - rxBytesLogged, txNow - txBytesLogged,
        irqsNow - irqsLogged, n.actions - o.actions, n.events - o.events, n.cc - o.cc, n.cs - o.cs, n.nocp - o.nocp,
        n.vendor - o.vendor, n.otherEvents - o.otherEvents, n.le - o.le, n.leAdvReports - o.leAdvReports,
        n.leAdv - o.leAdv, n.leConn - o.leConn, n.leOther - o.leOther, n.acl - o.acl, n.sco - o.sco,
        n.commands - o.commands, n.leCommands - o.leCommands, n.vendorCommands - o.vendorCommands,
        n.answered - o.answered, n.advFixed - o.advFixed, n.scanOn - o.scanOn, n.scanOff - o.scanOff,
        n.central - o.central, at ? fails : " none", held[0] ? held : " none");
    os_log(OS_LOG_DEFAULT, "%s", line);
    // The transport's own line (headers "TX by APB DMA", "RX by APB DMA"): what each path moved, the interrupts it
    // took, and the time spent in them (the UART filter and both channels' handlers, all on CPU0).
    os_log(OS_LOG_DEFAULT,
           "SwitchBluetooth: transport %llu ms: rx %u B (%u by DMA in %u transfers: %u ends of count, %u line-idle "
           "takes; DMA %s), tx %u B (%u DMA transfers), %u UART interrupts, interrupt time %llu us = %u.%02u%% of CPU0",
           (unsigned long long)(ns / 1000000), rxNow - rxBytesLogged, rxDmaBytesNow - rxDmaBytesLogged,
           rxDmaTransfersNow - rxDmaTransfersLogged, rxDmaEndsNow - rxDmaEndsLogged,
           rxDmaIdleTakesNow - rxDmaIdleTakesLogged,
           !rxDma || rxDmaOff ? "off"
           : rxDmaOn          ? "on"
                              : "pio now",
           txNow - txBytesLogged, txDmaRunsNow - txDmaRunsLogged, irqsNow - irqsLogged,
           (unsigned long long)(isrNs / 1000), isrBasis / 100, isrBasis % 100);
    stLogged = n;
    memcpy(failedLogged, failed, sizeof(failed));
    irqsLogged = irqsNow;
    rxBytesLogged = rxNow;
    txBytesLogged = txNow;
    rxDmaBytesLogged = rxDmaBytesNow;
    rxDmaTransfersLogged = rxDmaTransfersNow;
    rxDmaEndsLogged = rxDmaEndsNow;
    rxDmaIdleTakesLogged = rxDmaIdleTakesNow;
    txDmaRunsLogged = txDmaRunsNow;
    isrTicksLogged = isrTicksNow;
    statsAt = now;
}
#endif

// Work loop, RX masked (rxStalled) and room in the ring again: RX interrupts back on, then what the FIFO already
// holds is taken here, bounded like one interrupt. A residue below the RX trigger (8) whose character timeout ran
// out while RX was masked raises neither RDI nor the RX timeout once unmasked: the 7 bytes left of a 71-byte
// Command Complete (Read Local Supported Commands, 64 taken by the budget) sat in the FIFO and bluetoothd's stack
// waited on them for good. At most one FIFO's worth is taken, without masking RX again: that is every byte that was in
// the FIFO when RX came back on; later ones raise RDI or the RX timeout themselves. So deliver() passes these on and
// returns. (It used to loop here while each drain spent a 64-byte budget, holding the work-loop gate for as long as
// the controller kept sending; a bring-up's runAction then waited on it for good.) True when bytes were taken.
//
// DMA mode (header "RX by APB DMA"): what the stall left in rxBuf is taken, the next transfer starts if the last has
// ended, and the RX sources come back; what the FIFO gathered meanwhile the channel then reads, in order.
bool SwitchBluetooth::resumeRx()
{
    const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    rxStalled = false;
    bool took = false;
    if (rxDmaOn) {
        bool ended = false;
        const uint32_t moved = rxDmaArmed ? tegra_apbdma_moved(rxDma, &ended) : 0;
        if (rxDmaArmed && ended) {
            took = endRxDmaLocked();
        } else {
            took = takeRxDmaLocked(rxDmaArmed ? moved : rxDmaLength * 4, !rxDmaArmed);  // the stall's rest in rxBuf
        }
        if (rxDmaOff) {
            leaveRxDmaLocked(true);
        } else if (!rxStalled) {
            armRxDmaLocked();
            ier = (ier & IER_THRI) | IER_RX_DMA;
            switch_mmio_write32(uart, IER, ier);
        }
    } else if (chipOn && !rxFault) {  // power() and a fault mask RX until a flush (the one after power-on) enables it
        ier |= IER_RX;
        switch_mmio_write32(uart, IER, ier);
        const uint32_t before = rx.head;
        uint32_t budget = kFifoBytes;
        (void)drainRx(budget, false);
        took = rx.head != before;
    }
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    return took;
}

// After a wait timed out: if the UART shows a condition the filter would have serviced within microseconds
// (received data with RX interrupts on, or an empty TX FIFO with bytes queued and THRI on) and still does 50 ms
// later with no interrupt taken and no action outstanding, the interrupt is lost. A chip holding CTS keeps THRE
// clear, so a silent or stuck chip is a timeout, not a panic.
void SwitchBluetooth::checkDelivery(const char *what)
{
    // TX DMA in flight (header "TX by APB DMA"): ended but never taken is a lost interrupt (after the grace); not moved
    // with the TX FIFO empty and CTS asserted is a channel that does not serve the UART (DMA off, PIO goes on).
    for (unsigned pass = 0; txDma && pass < 2; ++pass) {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        bool ended = false;
        const uint32_t inFlight = txDmaBytes;
        const uint32_t moved = inFlight ? tegra_apbdma_moved(txDma, &ended) : 0;
        const uint32_t lsr = switch_mmio_read32(uart, LSR);
        const uint32_t msr = switch_mmio_read32(uart, MSR);
        if (inFlight && !ended && !moved && (lsr & LSR_THRE) && (msr & MSR_CTS) && pass) {
            txDmaFailedLocked(0);
            IOSimpleLockUnlockEnableInterrupt(hwLock, s);
            char dma[320];
            tegra_apbdma_describe(txDma, dma, sizeof(dma));
            IOLog(
                "SwitchBluetooth: TX DMA off for this boot (the channel moved none of %u bytes with the TX FIFO empty "
                "and CTS asserted during %s): TX by THR-empty interrupt; %s\n",
                inFlight, what, dma);
            return;
        }
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        if (!inFlight)
            break;
        if (pass && ended) {
            char dma[320];
            tegra_apbdma_describe(txDma, dma, sizeof(dma));
            panic("SwitchBluetooth: UART-D TX DMA end-of-count interrupt lost during %s: %u bytes, %s", what, inFlight,
                  dma);
        }
        if (!ended && !((lsr & LSR_THRE) && (msr & MSR_CTS)))
            break;  // the chip holds CTS: a timeout, not a fault
        IOSleep(kGraceMs);
    }
    // RX DMA (header "RX by APB DMA"): a transfer ended, or bytes moved with no line-idle interrupt to take them, and
    // still so 50 ms later with no interrupt taken: lost. Until a line-idle interrupt has once delivered what moved
    // (rxDmaProven), the latter is DMA mode not working as this driver needs (never booted): DMA off, PIO.
    uint32_t rxSeen = 0;
    uint32_t rxMoved = 0;
    for (unsigned pass = 0; rxDma && pass < 2; ++pass) {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        bool ended = false;
        bool pending = false;
        uint32_t moved = 0;
        if (rxDmaOn && rxDmaArmed && !rxStalled) {
            moved = tegra_apbdma_moved(rxDma, &ended);
            pending = ended || switch_bt_rx_dma_visible(rxDmaLength, moved, false, rxDmaTaken) > rxDmaTaken;
        }
        const uint32_t seen = irqs + tegra_apbdma_interrupts(rxDma);
        if (pending && pass && seen == rxSeen && moved == rxMoved) {
            if (!ended && !rxDmaProven) {
                rxDmaFailedLocked("bytes the channel moved raised no line-idle interrupt (EORD)");
                IOSimpleLockUnlockEnableInterrupt(hwLock, s);
                break;
            }
            const uint32_t taken = rxDmaTaken;
            const uint32_t length = rxDmaLength;
            IOSimpleLockUnlockEnableInterrupt(hwLock, s);
            char dma[320];
            tegra_apbdma_describe(rxDma, dma, sizeof(dma));
            panic("SwitchBluetooth: UART-D RX DMA %s interrupt lost during %s: %u of %u words taken, %u moved, %s",
                  ended ? "end-of-count" : "line-idle (EORD)", what, taken, length, moved / 4, dma);
        }
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        if (!pending || pass)
            break;
        rxSeen = seen;
        rxMoved = moved;
        IOSleep(kGraceMs);
    }
    uint32_t before = 0;
    for (unsigned pass = 0; pass < 2; ++pass) {
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        const uint32_t lsr = switch_mmio_read32(uart, LSR);
        const uint32_t enabled = ier;
        const uint32_t seen = irqs;
        const bool pending =
            !actionDue && ((((lsr & LSR_DR) != 0u) && ((enabled & IER_RDI) != 0u) && !rxStalled) ||
                           ((tx.used() != 0u) && ((enabled & IER_THRI) != 0u) && ((lsr & LSR_THRE) != 0u)));
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        if (!pending || (pass && seen != before))
            return;
        if (pass) {
            char line[400];
            describe(line, sizeof(line));
            panic("SwitchBluetooth: UART-D interrupt lost during %s: %s", what, line);
        }
        before = seen;
        IOSleep(kGraceMs);
    }
}

// One line for a panic or a failed bring-up: UART, pins, clock, counters and the GIC's view of INTID 122.
void SwitchBluetooth::describe(char *buffer, size_t size)
{
    char gic[160];
    switch_interrupt_describe(kIntid, gic, sizeof(gic));
    const uint32_t port = portReg(PH4, 0);
    const auto mux = [this](uint32_t o) {
        return switch_mmio_read32(pinmux, o) & PM_LOW;
    };
    const char *rxDmaState = "pio";
    if (rxDmaOn)
        rxDmaState = rxDmaArmed ? "armed" : "idle";
    if (!rxDma || rxDmaOff)
        rxDmaState = "off";
    snprintf(buffer, size,
             "lsr %#x msr %#x (cts %u) ier %#x mcr %#x lcr %#x baud %u; rx %u tx %u queued %u (dma %u, %u done%s) "
             "rx-dma %s %u/%u words, %u transfers %u ends %u idle takes; irqs %u (idle %u) "
             "errors %u rx-resets %u rx-faults %u (last %s)%s dropped %u resyncs %u hw-errors %u fixes %u strays %u; "
             "gpio H cnf/oe/out/in "
             "%02x/%02x/%02x/%02x; pinmux %x/%x/%x/%x %x/%x/%x; car clk %u rst %u src %#x; %s",
             switch_mmio_read32(uart, LSR), switch_mmio_read32(uart, MSR), flag(switch_mmio_read32(uart, MSR), MSR_CTS),
             ier, switch_mmio_read32(uart, MCR), switch_mmio_read32(uart, LCR), baud, rxBytes, txBytes, tx.used(),
             txDmaBytes, txDmaRuns, txDma && !txDmaOff ? "" : " off", rxDmaState, rxDmaTaken, rxDmaLength,
             rxDmaTransfers, rxDmaEnds, rxDmaIdleTakes, irqs, idleIrqs, lineErrors, rxResets, rxFaults, lastFault,
             rxFault ? " RX off" : "", dropped, resyncs, hardwareErrors, completionFixes, strayDrops,
             switch_mmio_read32(gpio, port + GPIO_CNF) & 0xff, switch_mmio_read32(gpio, port + GPIO_OE) & 0xff,
             switch_mmio_read32(gpio, port + GPIO_OUT) & 0xff, switch_mmio_read32(gpio, port + GPIO_IN) & 0xff,
             mux(0x114), mux(0x118), mux(0x11c), mux(0x120), mux(0x1c0), mux(0x1c4), mux(0x1c8),
             flag(switch_mmio_read32(car, CLK_OUT_ENB_U), kUartDBit),
             flag(switch_mmio_read32(car, RST_DEVICES_U), kUartDBit), switch_mmio_read32(car, CLK_SOURCE_UARTD), gic);
}

// Every wait in a bring-up or a connect is bounded, so one that is still running after kStuckMs has stopped for a
// reason the driver does not know: one line, once per boot, so that Bluetooth never fails silently.
void SwitchBluetooth::watch(const char *what)
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kStuckMs, kMillisecondScale, &deadline);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): a thread call's parameter is void *; the call reads it
    thread_call_enter1_delayed(stuckCall, const_cast<char *>(what), deadline);
}

void SwitchBluetooth::unwatch()
{
    thread_call_cancel(stuckCall);
}

void SwitchBluetooth::stuck(thread_call_param_t owner, thread_call_param_t what)
{
    static bool said;
    if (__atomic_exchange_n(&said, true, __ATOMIC_RELAXED))
        return;
    char line[400];
    static_cast<SwitchBluetooth *>(owner)->describe(line, sizeof(line));
    IOLog("SwitchBluetooth: %s still running after %u s: [%s]\n", static_cast<const char *>(what), kStuckMs / 1000,
          line);
}

// ---- Transmit and the kernel's HCI -------------------------------------------------------------------------------

// Queues all of `bytes`, waiting (by interrupt) for ring space. One writer at a time.
int SwitchBluetooth::transmit(const uint8_t *bytes, size_t length, uint32_t timeoutMs)
{
    IOLockLock(txLock);
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    while (length) {
        // txWanted goes up before the check, so the interrupt that frees the space also wakes us.
        __atomic_store_n(&txWanted, true, __ATOMIC_SEQ_CST);
        uint32_t room = tx.space();
        if (!room) {
            const int r = IOLockSleepDeadline(txLock, &tx, deadline, THREAD_UNINT);
            if (r == THREAD_TIMED_OUT && !tx.space()) {
                txWanted = false;
                IOLockUnlock(txLock);
                checkDelivery("transmit");
                return ETIMEDOUT;
            }
            continue;
        }
        txWanted = false;
        uint32_t head = tx.head;
        for (; room && length; --room, --length)
            tx.data[head++ & (kRingBytes - 1)] = *bytes++;
        __atomic_store_n(&tx.head, head, __ATOMIC_RELEASE);
        kickTx();
    }
    IOLockUnlock(txLock);
    return 0;
}

// Waits until everything queued has left the ring, then for the FIFO and shift register at the current rate
// (TEMT has no interrupt; 33 characters is the worst case).
int SwitchBluetooth::drainTx(uint32_t timeoutMs)
{
    IOLockLock(txLock);
    uint64_t deadline = 0;
    clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
    __atomic_store_n(&txWanted, true, __ATOMIC_SEQ_CST);
    while (tx.used()) {
        const int r = IOLockSleepDeadline(txLock, &tx, deadline, THREAD_UNINT);
        if (r == THREAD_TIMED_OUT && tx.used()) {
            txWanted = false;
            IOLockUnlock(txLock);
            checkDelivery("transmit drain");
            return ETIMEDOUT;
        }
    }
    txWanted = false;
    IOLockUnlock(txLock);
    IODelay((kFifoBytes + 1) * 10 * 1000000 / baud + 1);
    return 0;
}

// Work loop, kernel mode only: H4 framing; events go to event(), ACL/SCO and garbage are skipped.
void SwitchBluetooth::parse(const uint8_t *bytes, uint32_t length)
{
    H4Parser &p = h4;
    for (uint32_t i = 0; i < length; ++i) {
        const uint8_t b = bytes[i];
        switch (p.state) {
        case 0:
            p.type = b;
            p.have = 0;
            p.need = rxHeaderBytes(b);
            if (p.need) {
                p.state = 1;
            } else {
                ++resyncs;
            }
            break;
        case 1:
            p.header[p.have++] = b;
            if (p.have < p.need)
                break;
            p.length = p.header[2];
            if (p.type == H4_EVENT)
                p.length = p.header[1];
            if (p.type == H4_ACL)
                p.length = le16(p.header + 2);
            p.got = 0;
            p.state = p.length ? 2 : 0;
            if (!p.length && p.type == H4_EVENT)
                event(p.header[0], p.payload, 0);
            break;
        default:
            if (p.type == H4_EVENT)
                p.payload[p.got] = b;
            if (++p.got < p.length)
                break;
            if (p.type == H4_EVENT)
                event(p.header[0], p.payload, static_cast<uint8_t>(p.length));
            p.state = 0;
            break;
        }
    }
}

// Completes the command in flight: Command Complete for its opcode, a failed Command Status, or a Hardware Error.
void SwitchBluetooth::event(uint8_t code, const uint8_t *params, uint8_t length)
{
    IOLockLock(hciLock);
    bool done = false;
    if (code == EV_COMMAND_COMPLETE && length >= 4 && cmd.waiting && le16(params + 1) == cmd.opcode) {
        cmd.length = static_cast<uint8_t>(length - 3);
        memcpy(cmd.data, params + 3, cmd.length);
        done = true;
    } else if (code == EV_COMMAND_STATUS && length >= 4 && cmd.waiting && le16(params + 2) == cmd.opcode && params[0]) {
        cmd.length = 1;
        cmd.data[0] = params[0];
        done = true;
    } else if (code == EV_HARDWARE_ERROR) {
        ++hardwareErrors;
        cmd.hardwareError = true;
        done = cmd.waiting;
    }
    if (done) {
        cmd.done = true;
        IOLockWakeup(hciLock, &cmd, false);
    }
    IOLockUnlock(hciLock);
}

// One HCI command and its completion. Returns 0, the controller's status as EIO, EPROTO for a Hardware Error, or
// ETIMEDOUT (after the lost-interrupt check).
int SwitchBluetooth::command(uint16_t opcode, const uint8_t *params, uint8_t length, uint32_t timeoutMs, uint8_t *out,
                             uint8_t *outLength)
{
    uint8_t packet[4 + 255] = {H4_COMMAND, static_cast<uint8_t>(opcode), static_cast<uint8_t>(opcode >> 8), length};
    if (length)
        memcpy(packet + 4, params, length);
    IOLockLock(hciLock);
    cmd.opcode = opcode;
    cmd.waiting = true;
    cmd.done = cmd.hardwareError = false;
    IOLockUnlock(hciLock);
    const int error = transmit(packet, 4u + length, kWriteMs);
    IOLockLock(hciLock);
    if (!error) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(timeoutMs, kMillisecondScale, &deadline);
        while (!cmd.done && IOLockSleepDeadline(hciLock, &cmd, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
        }
    }
    const bool done = cmd.done;
    const bool hardwareError = cmd.hardwareError;
    cmd.waiting = false;
    if (done && !hardwareError && out) {
        memcpy(out, cmd.data, cmd.length);
        if (outLength)
            *outLength = cmd.length;
    }
    const uint8_t status = cmd.length ? cmd.data[0] : 0;
    IOLockUnlock(hciLock);
    if (error)
        return error;
    if (hardwareError)
        return EPROTO;
    if (!done) {
        checkDelivery("an HCI command");
        return ETIMEDOUT;
    }
    return status ? EIO : 0;
}

// Host UART at `rate`, stale bytes dropped, then HCI Reset up to `tries` times. EPROTO (Hardware Error) ends it:
// the bytes framed correctly, so the rate is right and only a power cycle helps (nxk bcm.rs reset()).
int SwitchBluetooth::resetAt(uint32_t rate, unsigned tries)
{
    if (rate != baud)
        setHostBaud(rate);
    int error = ETIMEDOUT;
    for (unsigned i = 0; i < tries && error != EPROTO; ++i) {
        flushRx();
        error = command(OP_RESET, nullptr, 0, kCommandMs);
        if (!error)
            break;
    }
    return error;
}

// Controller and host to 3 Mbaud (Broadcom 0xFC18: two zero bytes, the rate little-endian); false leaves both at
// the old rate (nxk go_fast).
bool SwitchBluetooth::fast()
{
    const uint32_t old = baud;
    const uint8_t p[6] = {0,
                          0,
                          static_cast<uint8_t>(kFastBaud),
                          static_cast<uint8_t>(kFastBaud >> 8),
                          static_cast<uint8_t>(kFastBaud >> 16),
                          static_cast<uint8_t>(kFastBaud >> 24)};
    if (command(OP_BCM_UART_BAUD, p, sizeof(p), kCommandMs))
        return false;
    setHostBaud(kFastBaud);
    IOSleep(10);
    flushRx();
    if (!command(OP_READ_VERSION, nullptr, 0, kCommandMs))
        return true;
    setHostBaud(old);
    flushRx();
    return false;
}

// One power cycle and bring-up (header comment). On failure the core is left off and `why` says where it stopped.
bool SwitchBluetooth::bringUp(uint32_t offMs, char *why, size_t whySize)
{
    watch("bring-up");
    const uint32_t faultsBefore = rxFaults;
    setMode(kKernel);
    power(false);
    chipAddressSet = false;  // a power cycle takes the controller back to its own address
    setPin(PH3, GPIO_OUT, false);
    IOSleep(offMs);
    setHostBaud(kInitBaud);
    power(true);
    IOSleep(kPowerOnMs);

    uint8_t v[255];
    uint8_t n = 0;
    int error = resetAt(kInitBaud, 6);
    if (error == ETIMEDOUT)
        error = resetAt(kFastBaud, 2);  // a chip that kept another rate across the cycle
    if (error) {
        snprintf(why, whySize, "no HCI Reset answer (error %d)", error);
        goto fail;
    }
    error = command(OP_READ_VERSION, nullptr, 0, kCommandMs, v, &n);
    if (error || n < 9) {
        snprintf(why, whySize, "Read Local Version failed (error %d)", error);
        goto fail;
    }
    {
        const uint16_t romSubversion = le16(v + 7);
        // A failed switch leaves the host at 115200 for the download; the chip may have switched all the same.
        const bool fastOk = baud == kFastBaud || fast();

        // Patch RAM: the minidriver, then the file's own records, the last launching it (btbcm_patchram).
        uint32_t records = 0;
        if (patch) {
            const uint8_t *p = static_cast<const uint8_t *>(patch->getBytesNoCopy());
            const uint32_t size = patch->getLength();
            error = command(OP_BCM_MINIDRIVER, nullptr, 0, kRecordMs);
            if (error) {
                snprintf(why, whySize, "download minidriver failed (error %d)%s", error,
                         fastOk ? "" : " after the 3 Mbaud switch failed");
                goto fail;
            }
            IOSleep(kMinidriverMs);
            for (uint32_t at = 0; at + 3 <= size; ++records) {
                const uint16_t op = le16(p + at);
                const uint8_t len = p[at + 2];
                error = command(op, p + at + 3, len, kRecordMs);
                if (error) {
                    snprintf(why, whySize, "patch record %u (opcode %#06x) failed (error %d)", records, op, error);
                    goto fail;
                }
                at += 3u + len;
            }
            IOSleep(kLaunchMs);
            // The patched firmware restarts the UART at its default rate; nxk also saw it want BT_WAKE high once.
            error = resetAt(kInitBaud, 2);
            if (error == ETIMEDOUT)
                error = resetAt(kFastBaud, 2);
            if (error == ETIMEDOUT) {
                setPin(PH3, GPIO_OUT, true);
                IOSleep(20);
                error = resetAt(kInitBaud, 2);
                if (error == ETIMEDOUT)
                    error = resetAt(kFastBaud, 2);
                setPin(PH3, GPIO_OUT, false);
            }
            if (error) {
                snprintf(why, whySize, "no answer after the patch (%u records, error %d)", records, error);
                goto fail;
            }
            if (baud != kFastBaud && !fast()) {
                snprintf(why, whySize, "patched firmware refused 3 Mbaud");
                goto fail;
            }
        } else if (baud != kFastBaud) {
            snprintf(why, whySize, "ROM firmware refused 3 Mbaud");
            goto fail;
        }
        error = command(OP_READ_VERSION, nullptr, 0, kCommandMs, v, &n);
        if (error || n < 9) {
            snprintf(why, whySize, "Read Local Version after the patch failed (error %d)", error);
            goto fail;
        }
        const uint16_t manufacturer = le16(v + 5);
        const uint16_t subversion = le16(v + 7);
        __atomic_store_n(&hciVersion, v[1], __ATOMIC_RELAXED);  // status, HCI version, revision, LMP version, ...
        uint8_t name[255] = {};
        if (command(OP_READ_LOCAL_NAME, nullptr, 0, kCommandMs, name, &n) || n < 2)
            strlcpy(reinterpret_cast<char *>(name) + 1, "unnamed", 16);
        name[sizeof(name) - 1] = 0;
        uint8_t caps[255] = {};
        uint8_t capsLength = 0;
        const int capsError = command(SWITCH_LE_OP_VENDOR_CAPS, nullptr, 0, kCommandMs, caps, &capsLength);
        const bool apcf = (capsError == 0) && switch_le_filter_capabilities(caps, capsLength);
        __atomic_store_n(&leApcf, apcf, __ATOMIC_RELAXED);
        if (!leCapsLogged) {
            leCapsLogged = true;
            IOLog("SwitchBluetooth: LE vendor capabilities (0xFD53): %s, filter support %u, %u filters: LE adverts "
                  "filter in the %s\n",
                  capsError ? "refused" : "answered", capsLength > 6 ? caps[6] : 0, capsLength > 7 ? caps[7] : 0,
                  apcf ? "controller" : "host");
        }

        // Sleep mode off (12 zero bytes, nxk): BT_WAKE stays asserted and host wake is unused. bluetoothd may
        // configure its own later.
        const uint8_t sleep[12] = {};
        (void)command(OP_BCM_SLEEP_MODE, sleep, sizeof(sleep), kCommandMs);
        if (haveAddress) {
            const uint8_t le[6] = {address[5], address[4], address[3], address[2], address[1], address[0]};
            error = command(OP_BCM_WRITE_BDADDR, le, sizeof(le), kCommandMs);
            if (error) {
                snprintf(why, whySize, "Write BD_ADDR failed (error %d)", error);
                goto fail;
            }
            memcpy(chipAddress, address, sizeof(chipAddress));
            chipAddressSet = true;
        }
        setMode(kIdle);
        IOLog("SwitchBluetooth: %s up at %u baud, manufacturer %#06x, LMP subversion %#06x (ROM %#06x), %s %u records, "
              "BD %02x:%02x:%02x:%02x:%02x:%02x (%s), HCI version %u\n",
              reinterpret_cast<const char *>(name) + 1, baud, manufacturer, subversion, romSubversion,
              patch ? "patch RAM" : "ROM firmware, patch", records, address[0], address[1], address[2], address[3],
              address[4], address[5], haveAddress ? addressFrom : "none: chip default", v[1]);
        if (manufacturer != kBroadcom)
            IOLog("SwitchBluetooth: manufacturer %#06x is not Broadcom\n", manufacturer);
        setProperty("local-name", reinterpret_cast<const char *>(name) + 1);
        setProperty("lmp-subversion", subversion, 16);
        setProperty("hci-version", v[1], 8);
        setProperty("baud", baud, 32);
        unwatch();
        return true;
    }
fail:
    // RX masked for a fault during the attempt: which one (r7h said "the line held low" for what was a storm).
    if (rxFaults != faultsBefore) {
        char masked[80];
        snprintf(masked, sizeof(masked), ", RX masked for %s", lastFault);
        strlcat(why, masked, whySize);
    }
    setMode(kIdle);
    power(false);
    unwatch();
    return false;
}

// ---- Work loop gated actions -------------------------------------------------------------------------------------

IOReturn SwitchBluetooth::gated(OSObject *owner, void *what, void *, void *, void *)
{
    auto *self = static_cast<SwitchBluetooth *>(owner);
    const uintptr_t op = reinterpret_cast<uintptr_t>(what);
    if (op == 0) {
        // Flush: the RX FIFO, then the ring and the H4 parser. Under hwLock the filter cannot produce meanwhile.
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(self->hwLock);
        self->resetRxFifoLocked();
        __atomic_store_n(&self->rx.tail, __atomic_load_n(&self->rx.head, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        // The ring and FIFO a stall waited on are gone, so nothing else would unmask RX; and this is where RX comes
        // back on after power-on (power() keeps it off for an unpowered core) and after a fault (maskRxFaultLocked).
        self->rxStalled = false;
        self->rxFault = false;
        self->rxFaultRun = 0;
        self->stormCleared = false;
        self->rxFrame = {};  // the controller's next byte starts a packet
        self->rxPacketDone = false;
        uint32_t ier = self->chipOn ? self->ier | IER_RX : self->ier & ~IER_RX;
        if (self->tx.used() && !self->txDmaBytes)
            ier |= IER_THRI;  // a fault that took every source left bytes queued
        if (ier != self->ier) {
            self->ier = ier;
            switch_mmio_write32(self->uart, IER, ier);
        }
        self->enterRxDmaLocked();  // header "RX by APB DMA": from here on the channel reads the FIFO
        IOSimpleLockUnlockEnableInterrupt(self->hwLock, s);
        self->h4.state = 0;
    } else if (op == 1) {
        self->deliver();
    } else {
        const Mode was = self->mode;
        self->mode = static_cast<Mode>(op - 2);
#if DEVELOPMENT || DEBUG
        if (was == kClient && self->mode != kClient)
            self->logStats(true);  // the client's last stats line
#endif
        if (self->mode == kClient && was != kClient)
            self->statsAt = mach_absolute_time();
        if (self->mode == kClient)
            self->resetClientFraming();
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(self->hwLock);
        self->framePackets = self->mode == kClient;
        if (self->framePackets && !self->rx.used())
            self->syncFramerLocked();
        IOSimpleLockUnlockEnableInterrupt(self->hwLock, s);
        __atomic_store_n(&self->clientBlocked, false, __ATOMIC_SEQ_CST);
        self->deliver();  // what the ring holds goes by the new mode, and a stall held for the old client resumes
    }
    return kIOReturnSuccess;
}

// hwLock held. The RX FIFO as Linux tegra_uart_fifo_reset does on T210, which cannot clear it in FIFO mode: FIFO mode
// off, clear, on (the TX FIFO empties with it), then 32 UART clocks for the clear to propagate.
void SwitchBluetooth::resetRxFifoLocked()
{
    leaveRxDmaLocked(false);  // DMA mode's FCR goes with it; a flush starts it again (gated)
    rxFrame.lost = true;      // whatever the FIFO held is gone (a flush starts the framing afresh after this)
    const uint32_t fcr = FCR_RX_TRIG_16 | FCR_TX_TRIG_16;
    switch_mmio_write32(uart, FCR, fcr);
    IODelay(60);
    switch_mmio_write32(uart, FCR, fcr | FCR_CLEAR_RX);
    switch_mmio_write32(uart, FCR, fcr | FCR_ENABLE);
    (void)switch_mmio_read32(uart, SPR);
    IODelay(20);  // 32 UART clocks (serial-tegra), generous at any rate here
}

void SwitchBluetooth::flushRx()
{
    loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(0));
}

void SwitchBluetooth::setMode(Mode m)
{
    loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(static_cast<uintptr_t>(m) + 2));
}

// ---- BD address --------------------------------------------------------------------------------------------------

// The address where bluetoothd (/arm-io/bluetooth local-mac-address) and MobileGestalt (/chosen
// mac-address-bluetooth0) read it, as publishAddress() puts the fuses' one.
void SwitchBluetooth::publishAddressProperties()
{
    publishAddressBytes(address);
}

// bluetoothd's first connect (see the header): the staged address as `address`, if there is one and it is not that
// already. Normally switch_bluetooth_data_mounted found it before launchd and published it, so bluetoothd has read it
// as local-mac-address from its start and this only takes it over for the controller. A kernel that never called that
// hook (no Data mount of its own) reads the file here, as before: this runs on bluetoothd's thread, so with the
// kernel's context (bluetoothd's sandbox would refuse the path). Returns whether `address` changed.
bool SwitchBluetooth::adoptStagedAddress()
{
    uint8_t staged[6];
    const int found = __atomic_load_n(&gStagedState, __ATOMIC_ACQUIRE);
    if (found == kStagedFound) {
        memcpy(staged, gStaged, sizeof(staged));
    } else if (found == kStagedNone || !readStagedAddress(staged)) {
        return false;
    }
    if (haveAddress && !memcmp(staged, address, 6))
        return false;
    IOLog("SwitchBluetooth: BD %02x:%02x:%02x:%02x:%02x:%02x from the staged Joy-Con pairing (Horizon's), was "
          "%02x:%02x:%02x:%02x:%02x:%02x (%s)%s\n",
          staged[0], staged[1], staged[2], staged[3], staged[4], staged[5], address[0], address[1], address[2],
          address[3], address[4], address[5], haveAddress ? addressFrom : "chip default",
          found == kStagedFound ? "" : "; bluetoothd read the old one at its start");
    memcpy(address, staged, 6);
    haveAddress = true;
    addressFrom = "staged";
    publishAddressProperties();
    return true;
}

// ---- Kernel control ----------------------------------------------------------------------------------------------

// Waits for the first bring-up, brings the chip up again if the last client used it, the last attempt failed or the
// line faulted since (maskRxFaultLocked: the core stopped driving it or the UART wedged), then hands the stream over
// with nothing stale in it. One client at a time.
errno_t SwitchBluetooth::ctlConnect(kern_ctl_ref, struct sockaddr_ctl *sac, void **unitinfo)
{
    SwitchBluetooth *self = gDriver;
    IOLockLock(self->stateLock);
    uint64_t deadline = 0;
    clock_interval_to_deadline(kConnectWaitMs, kMillisecondScale, &deadline);
    while (self->state <= kBusy &&
           IOLockSleepDeadline(self->stateLock, &self->state, deadline, THREAD_UNINT) != THREAD_TIMED_OUT) {
    }
    errno_t error = 0;
    // The first connect (see the header) takes the staged address over; the controller, brought up at boot before
    // Data was mounted, still has the fuses' one. Bring-up writes `address`; a controller that needs none gets it
    // with one command.
    if (self->state > kBusy && !self->client && !self->stagedChecked) {
        self->stagedChecked = true;
        IOLockUnlock(self->stateLock);
        (void)self->adoptStagedAddress();
        IOLockLock(self->stateLock);
    }
    const bool readdress =
        self->haveAddress &&
        (!self->chipAddressSet || (memcmp(self->chipAddress, self->address, sizeof(self->address)) != 0));
    if (readdress && self->state == kReady && !self->client && !self->chipDirty &&
        !__atomic_load_n(&self->rxFault, __ATOMIC_RELAXED)) {
        self->state = kBusy;
        IOLockUnlock(self->stateLock);
        self->setMode(kKernel);
        const uint8_t le[6] = {self->address[5], self->address[4], self->address[3],
                               self->address[2], self->address[1], self->address[0]};
        const int written = self->command(OP_BCM_WRITE_BDADDR, le, sizeof(le), kCommandMs);
        self->setMode(kIdle);
        if (written)
            IOLog("SwitchBluetooth: Write BD_ADDR for the staged address failed (error %d)\n", written);
        if (!written) {
            memcpy(self->chipAddress, self->address, sizeof(self->chipAddress));
            self->chipAddressSet = true;
        }
        IOLockLock(self->stateLock);
        self->state = kReady;
        self->chipDirty |= written != 0;  // the bring-up below writes it
        IOLockWakeup(self->stateLock, &self->state, false);
    }
    if (self->state <= kBusy) {
        error = ETIMEDOUT;
    } else if (self->client) {
        error = EBUSY;
    } else if (self->state == kFailed || self->chipDirty || __atomic_load_n(&self->rxFault, __ATOMIC_RELAXED)) {
        self->state = kBusy;
        IOLockUnlock(self->stateLock);
        char why[128];
        const bool ok = self->bringUp(kPowerOffMs, why, sizeof(why));
        static bool said;  // bluetoothd retries every few seconds: one line is enough
        if (!ok && !said) {
            said = true;
            char line[400];
            self->describe(line, sizeof(line));
            IOLog("SwitchBluetooth: bring-up for a client failed: %s [%s]\n", why, line);
        }
        IOLockLock(self->stateLock);
        self->state = ok ? kReady : kFailed;
        self->chipDirty = false;
        IOLockWakeup(self->stateLock, &self->state, false);
        if (!ok)
            error = ECONNREFUSED;
    }
    if (!error) {
        self->client = sac->sc_unit;
        self->clientUsed = false;
    }
    IOLockUnlock(self->stateLock);
    if (error)
        return error;
    self->watch("connect");
    self->flushRx();
    self->setMode(kClient);
    self->unwatch();
    *unitinfo = self;
    char line[400];
    self->describe(line, sizeof(line));
    IOLog("SwitchBluetooth: client %u connected: %s\n", sac->sc_unit, line);
    if (self->haveAddress)
        switch_joycon_bluetooth_address(self->address);
    return 0;
}

errno_t SwitchBluetooth::ctlDisconnect(kern_ctl_ref, u_int32_t unit, void *)
{
    SwitchBluetooth *self = gDriver;
    IOLockLock(self->stateLock);
    const bool mine = self->client == unit;
    if (mine) {
        self->client = 0;
        self->chipDirty |= self->clientUsed;
        // A core the client used goes off now (under stateLock, so a connect's bring-up cannot be cut short): the
        // next connect brings it up again anyway, and until then no one would take what it keeps sending (r6k:
        // bluetoothd exited mid-connection to AirPods with an LE scan and inquiry running).
        if (self->chipDirty)
            self->power(false);
    }
    IOLockUnlock(self->stateLock);
    if (mine) {
        self->setMode(kIdle);
        switch_wake_bt_reset();  // every connection went with the client: no controller holds a button
        char line[400];
        self->describe(line, sizeof(line));
        IOLog("SwitchBluetooth: client %u gone%s: %s\n", unit, self->rxStalled ? " (RX was stalled)" : "", line);
    }
    return 0;
}

// The client's bytes, in order. A command that starts at a packet boundary and is whole in this send is taken out and
// looked at (clientCommand: answered here, or sent, possibly changed); the rest go out as they are, noted
// (noteClientTx) before the controller can see, let alone answer, a command.
errno_t SwitchBluetooth::ctlSend(kern_ctl_ref, u_int32_t unit, void *, mbuf_t m, int)
{
    SwitchBluetooth *self = gDriver;
    IOLockLock(self->sendGate);
    const errno_t sent = self->clientSend(unit, m);
    IOLockUnlock(self->sendGate);
    return sent;
}

errno_t SwitchBluetooth::clientSend(u_int32_t unit, mbuf_t m)
{
    SwitchBluetooth *self = this;
    errno_t error = self->client == unit ? 0 : ENOTCONN;
    if (!error)
        self->clientUsed = true;
    size_t total = 0;
    for (mbuf_t n = m; n; n = mbuf_next(n))
        total += mbuf_len(n);
    mbuf_t n = m;
    size_t base = 0;
    size_t off = 0;  // n starts at byte `base` of the send; the first `off` bytes are done
    while (!error && off < total) {
        while (base + mbuf_len(n) <= off) {
            base += mbuf_len(n);
            n = mbuf_next(n);
        }
        const uint8_t *bytes = static_cast<const uint8_t *>(mbuf_data(n)) + (off - base);
        if (!self->txFrame.state && !self->framingLost && bytes[0] == H4_COMMAND && total - off >= 4) {
            uint8_t packet[4 + 255];
            if (!mbuf_copydata(m, off, 4, packet) && total - off >= 4u + packet[3] &&
                !mbuf_copydata(m, off, 4u + packet[3], packet)) {
                const size_t size = 4u + packet[3];
                error = self->clientCommand(packet, size);
                off += size;
                continue;
            }
        }
        const size_t taken = self->noteClientTx(bytes, base + mbuf_len(n) - off);
        error = self->transmit(bytes, taken, kWriteMs);
        off += taken;
    }
    mbuf_freem(m);
    return error;
}

// struct termios in, as a tty: the output speed becomes the UART rate once queued bytes are out. Hardware flow
// control stays on whatever c_cflag says (H4 has no other). TIOCSETAW/F behave like TIOCSETA.
errno_t SwitchBluetooth::ctlSetopt(kern_ctl_ref, u_int32_t unit, void *, int opt, void *data, size_t len)
{
    SwitchBluetooth *self = gDriver;
    const uint32_t name = static_cast<uint32_t>(opt);
    if (name != TIOCSETA && name != TIOCSETAW && name != TIOCSETAF)
        return ENOPROTOOPT;
    if (self->client != unit)
        return ENOTCONN;
    if (!data || len < sizeof(struct termios))
        return EINVAL;
    struct termios t;
    memcpy(&t, data, sizeof(t));
    const uint32_t rate = static_cast<uint32_t>(t.c_ospeed ? t.c_ospeed : t.c_ispeed);
    if (rate && rate != self->baud) {
        if (rate != 3000000 && rate != 1000000 && (rate < 9600 || rate > 921600))
            return EINVAL;
        IOLockLock(self->sendGate);
        const bool drained = self->drainTx(kWriteMs) == 0;
        if (drained)
            self->setHostBaud(rate);
        IOLockUnlock(self->sendGate);
        if (!drained)
            return EIO;
        self->clientUsed = true;
    }
    self->tio = t;
    self->tio.c_ispeed = self->tio.c_ospeed = self->baud;
    return 0;
}

errno_t SwitchBluetooth::ctlGetopt(kern_ctl_ref, u_int32_t, void *, int opt, void *data, size_t *len)
{
    SwitchBluetooth *self = gDriver;
    if (static_cast<uint32_t>(opt) != TIOCGETA)
        return ENOPROTOOPT;
    if (!data) {
        *len = sizeof(struct termios);
        return 0;
    }
    if (*len < sizeof(struct termios))
        return EINVAL;
    self->tio.c_ispeed = self->tio.c_ospeed = self->baud;
    memcpy(data, &self->tio, sizeof(struct termios));
    *len = sizeof(struct termios);
    return 0;
}

// The client read: room again in its socket for what deliver() had to leave in the ring.
void SwitchBluetooth::ctlRcvd(kern_ctl_ref, u_int32_t, void *, int)
{
    SwitchBluetooth *self = gDriver;
    if (__atomic_exchange_n(&self->clientBlocked, false, __ATOMIC_SEQ_CST))
        self->loop->runAction(&SwitchBluetooth::gated, self, reinterpret_cast<void *>(1));
}

// ---- Start -------------------------------------------------------------------------------------------------------

// Kernel thread: the control first (so a client's CTLIOCGINFO finds it and its connect waits), then the first
// bring-up, power cycled and tried again after 1, 2, 4 and 8 s when it fails. nxk retries a Hardware Error so
// (Erista's first bring-up after a cold boot gets one for seconds); a core that does not drive its TX at all after a
// cold power-on (RX held low, maskRxFaultLocked) or stops answering is retried the same way. Only a failure takes
// longer.
void SwitchBluetooth::run()
{
    struct kern_ctl_reg reg = {};
    strlcpy(reg.ctl_name, kControlName, sizeof(reg.ctl_name));
    reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM | CTL_FLAG_REG_EXTENDED;  // bluetoothd runs as mobile: not privileged
    reg.ctl_sendsize = 64 * 1024;
    reg.ctl_recvsize = 64 * 1024;
    reg.ctl_connect = &SwitchBluetooth::ctlConnect;
    reg.ctl_disconnect = &SwitchBluetooth::ctlDisconnect;
    reg.ctl_send = &SwitchBluetooth::ctlSend;
    reg.ctl_setopt = &SwitchBluetooth::ctlSetopt;
    reg.ctl_getopt = &SwitchBluetooth::ctlGetopt;
    reg.ctl_rcvd = &SwitchBluetooth::ctlRcvd;
    const errno_t registered = ctl_register(&reg, &control);
    if (registered)
        IOLog("SwitchBluetooth: kernel control %s not registered (%d)\n", kControlName, registered);

    if (!patch)
        IOLog("SwitchBluetooth: no /arm-io/bluetooth switch,patchram; trying the ROM firmware\n");
    char why[128] = "";
    bool ok = false;
    uint32_t offMs = kPowerOffMs;
    unsigned attempt = 0;
    IOLockLock(stateLock);
    state = kBusy;
    IOLockUnlock(stateLock);
    for (;; ++attempt) {
        ok = bringUp(offMs, why, sizeof(why));
        if (ok || attempt == sizeof(kRetrySeconds) / sizeof(kRetrySeconds[0]))
            break;
        IOSleep(kRetrySeconds[attempt] * 1000);
        offMs = kPowerOffAfterErrorMs;
    }
    if (!ok) {
        char line[400];
        describe(line, sizeof(line));
        IOLog("SwitchBluetooth: bring-up failed (%u attempts): %s [%s]\n", attempt + 1, why, line);
        setProperty("error", why);
    } else if (attempt) {
        IOLog("SwitchBluetooth: up at attempt %u; the one before failed: %s\n", attempt + 1, why);
    }
    IOLockLock(stateLock);
    state = ok ? kReady : kFailed;
    IOLockWakeup(stateLock, &state, false);
    IOLockUnlock(stateLock);
}

namespace {
void startThread(void *, wait_result_t)
{
    SwitchBluetooth *driver = OSTypeAlloc(SwitchBluetooth);
    IOService *resources = IOService::getResourceService();
    if (!driver || !driver->init() || !resources || !driver->attach(resources)) {
        IOLog("SwitchBluetooth: attach failed\n");
        OSSafeReleaseNULL(driver);
        return;
    }
    if (!driver->start(resources)) {
        driver->detach(resources);
        driver->release();
        return;
    }
    gDriver = driver;
    driver->release();  // the attachment keeps it
    gDriver->run();
}
}

// ---- System sleep --------------------------------------------------------------------------------------------------
//
// SC7 powers the SoC off and keeps the combo chip powered (BT_REG_ON PH4 and the pads keep their levels), so the
// controller, its patch RAM and its links outlive the sleep and the controller can wake the console:
//   suspend: the client's sends wait (sendGate, until resume); the Nintendo controllers' links are dropped, as Horizon
//   does at sleep (a connected Joy-Con streams reports; its next button press pages the console back); Broadcom Set
//   Sleep Mode on with host wake (switch_bt_sleep_mode_params); TX drained; BT_WAKE released (PH3 high); RTS deasserted
//   so the controller holds what it has; what is in the ring goes to the client; UART interrupts off.
//   resume: UART-D from reset at the rate it had with RTS still off, the FIFO, ring and framing flushed and RX (DMA)
//   on, then RTS: the controller sends what it held (the page that woke the console) in order; BT_WAKE asserted; sleep
//   mode off as at bring-up.
// Commands go as the client's stream allows: injected between its packets (inject) while a client is connected, as the
// kernel's own HCI otherwise.
uint8_t SwitchBluetooth::sleepCommand(uint16_t opcode, const uint8_t *params, uint8_t length)
{
    if (mode != kClient) {
        setMode(kKernel);
        const int error = command(opcode, params, length, kCommandMs);
        setMode(kIdle);
        return error ? 0xff : 0;
    }
    uint8_t status = 0xfe;
    for (unsigned tries = 0; tries < 50 && status == 0xfe; ++tries) {
        status = inject(opcode, params, length, nullptr, nullptr);
        if (status == 0xfe)
            IOSleep(10);  // the client's last command is still out (it waits at sendGate now)
    }
    return status;
}

// HCI Disconnect (reason 0x15, power off) for each controller, then up to a second for their Disconnection Complete.
unsigned SwitchBluetooth::dropControllers()
{
    uint16_t handles[SWITCH_BT_BUTTON_LINKS];
    unsigned n = 0;
    loop->runAction(
        [](OSObject *, void *list, void *count, void *, void *) -> IOReturn {
            *static_cast<unsigned *>(count) =
                switch_wake_bt_controllers(static_cast<uint16_t *>(list), SWITCH_BT_BUTTON_LINKS);
            return kIOReturnSuccess;
        },
        this, handles, &n);
    const uint32_t before = __atomic_load_n(&disconnects, __ATOMIC_ACQUIRE);
    unsigned asked = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint8_t params[3] = {static_cast<uint8_t>(handles[i]), static_cast<uint8_t>(handles[i] >> 8), 0x15};
        if (sleepCommand(OP_DISCONNECT, params, sizeof(params)) == 0)
            ++asked;
    }
    uint64_t deadline = 0;
    clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
    while (__atomic_load_n(&disconnects, __ATOMIC_ACQUIRE) - before < asked) {
        assert_wait_deadline(&disconnects, THREAD_UNINT, deadline);
        if (__atomic_load_n(&disconnects, __ATOMIC_ACQUIRE) - before >= asked) {
            clear_wait(current_thread(), THREAD_AWAKENED);
            break;
        }
        if (thread_block(THREAD_CONTINUE_NULL) == THREAD_TIMED_OUT)
            break;
    }
    const unsigned done = __atomic_load_n(&disconnects, __ATOMIC_ACQUIRE) - before;
    if (asked != n || done < asked)
        IOLog("SwitchBluetooth: sleep: %u of %u controllers disconnected (%u asked)\n", done, n, asked);
    return done < asked ? done : asked;
}

// Returns whether the controller will wake the host (PMC wake event 52).
bool SwitchBluetooth::sleep()
{
    IOLockLock(sendGate);
    asleep = true;
    IOLockLock(stateLock);
    const bool up = state == kReady && chipOn;
    IOLockUnlock(stateLock);
    slept = up;
    if (!up)
        return false;
    bool hostWake = false;
    unsigned linksDropped = 0;
    // A client packet half sent would have our commands land inside it: no commands then, the stream stops as it is.
    const bool boundary = mode != kClient || ((txFrame.state == 0u) && !framingLost);
    if (boundary) {
        linksDropped = dropControllers();
        uint8_t params[SWITCH_BT_SLEEP_PARAMS];
        switch_bt_sleep_mode_params(params, true);
        hostWake = sleepCommand(OP_BCM_SLEEP_MODE, params, sizeof(params)) == 0;
    }
    const int drained = drainTx(kWriteMs);
    if (hostWake)
        setPin(PH3, GPIO_OUT, true);
    IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
    switch_mmio_write32(uart, MCR, MCR_CTS_EN);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    IODelay(2 * 10 * 1000000 / baud + 20);  // a character the controller had started
    loop->runAction(&SwitchBluetooth::gated, this, reinterpret_cast<void *>(1));  // the ring to the client
    s = IOSimpleLockLockDisableInterrupt(hwLock);
    leaveRxDmaLocked(true);
    switch_mmio_write32(uart, IER, 0);
    IOSimpleLockUnlockEnableInterrupt(hwLock, s);
    switch_power_journal("SwitchBluetooth: sleep: %u controllers dropped, host wake %s%s%s", linksDropped,
                         hostWake ? "on (PH5, PMC 52)" : "off", boundary ? "" : " (client mid-packet)",
                         drained ? "; TX not drained" : "");
    return hostWake;
}

void SwitchBluetooth::wake()
{
    if (!asleep)
        return;
    if (slept) {
        const uint32_t rate = baud;
        setupUart(false);  // RTS stays off: what the controller holds must not come at the reset rate
        if (rate != baud)
            setHostBaud(rate);
        flushRx();
        const IOInterruptState s = IOSimpleLockLockDisableInterrupt(hwLock);
        switch_mmio_write32(uart, IER, ier);
        switch_mmio_write32(uart, MCR, MCR_CTS_EN | MCR_RTS_EN | MCR_RTS);
        IOSimpleLockUnlockEnableInterrupt(hwLock, s);
        setPin(PH3, GPIO_OUT, false);
        uint8_t params[SWITCH_BT_SLEEP_PARAMS];
        switch_bt_sleep_mode_params(params, false);
        const uint8_t status = sleepCommand(OP_BCM_SLEEP_MODE, params, sizeof(params));
        switch_power_journal("SwitchBluetooth: wake: UART-D at %u baud again, sleep mode off (%#x)", baud, status);
    }
    asleep = slept = false;
    IOLockUnlock(sendGate);
}

extern "C" bool switch_bluetooth_suspend(void)
{
    SwitchBluetooth *self = __atomic_load_n(&gDriver, __ATOMIC_ACQUIRE);
    return (self != nullptr) && self->sleep();
}

extern "C" void switch_bluetooth_resume(void)
{
    if (SwitchBluetooth *self = __atomic_load_n(&gDriver, __ATOMIC_ACQUIRE))
        self->wake();
}

// ---- /dev/btwake ---------------------------------------------------------------------------------------------------
//
// bluetoothd's HCI transport is H4BC (BT_HCI_TRANSPORT, set for it by kern_exec.c; J120's ADT transport-encoding 3):
// H4 framing over the same POSIX driver and kernel control, plus Apple's BT_WAKE line as a device it opens before it
// sends after an idle spell and closes 200 ms after the last packet (21H461 bluetoothd sub_1000dd9f0 / sub_1000dda74),
// and a prepare-to-terminate step the plain "H4" transport lacks: its ops table has a NULL there (+0x60), so every
// stack terminate jumped to address 0 (r8a 11-05 15:19:42: SIGSEGV when Bluetooth was turned off in Settings, then a
// crash loop, each relaunch terminating the stack again for the saved "off"). Here BT_WAKE stays asserted while the
// chip is up (sleep mode off, bring-up), so the device's open and close succeed and change nothing; without the node
// H4BC would log "Unable to turn on BT)_WAKE signal" at every wake. One line at the first open.
namespace {
uint32_t gBtwakeOpens;

int btwakeOpen(dev_t, int, int, struct proc *)
{
    if (!__atomic_fetch_add(&gBtwakeOpens, 1, __ATOMIC_RELAXED))
        os_log(OS_LOG_DEFAULT, "SwitchBluetooth: /dev/btwake opened (bluetoothd's H4BC wake; BT_WAKE stays asserted)");
    return 0;
}

int btwakeClose(dev_t, int, int, struct proc *)
{
    return 0;
}

const struct cdevsw kBtwake = {btwakeOpen, btwakeClose, eno_rdwrt, eno_rdwrt, eno_ioctl, eno_stop, eno_reset,
                               nullptr,    eno_select,  eno_mmap,  eno_strat, eno_getc,  eno_putc, 0};

void publishBtwake()
{
    const int major = cdevsw_add(-1, &kBtwake);
    if (major < 0 ||
        !devfs_make_node(makedev(static_cast<uint32_t>(major), 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "btwake")) {
        IOLog("SwitchBluetooth: /dev/btwake not created (major %d); bluetoothd logs a BT_WAKE error at each wake\n",
              major);
    }
}

bool bluetoothDisabled()
{
    uint32_t off = 0;
    return (PE_parse_boot_argn("switch-no-bt", &off, sizeof(off)) != 0) && (off != 0u);
}
}  // namespace

extern "C" int switch_bluetooth_start(void)
{
    if (bluetoothDisabled())
        return 0;
    static volatile bool started;
    if (__atomic_exchange_n(&started, true, __ATOMIC_ACQ_REL))
        return 0;
    publishAddress();
    publishBtwake();
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(&startThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("SwitchBluetooth: thread start failed\n");
        return -1;
    }
    thread_deallocate(thread);
    return 0;
}

// kern_exec.c load_init_program, through SwitchNVRAM's switch_nvram_data_mounted: Data is mounted and launchd not yet
// started. A staged address is published now, so that bluetoothd reads it as local-mac-address when it starts and
// never sees another one (see the header, "BD address"); the controller gets it at bluetoothd's first connect.
extern "C" void switch_bluetooth_data_mounted(void)
{
    if (bluetoothDisabled())
        return;
    uint8_t staged[6];
    if (readStagedAddress(staged)) {
        memcpy(gStaged, staged, sizeof(gStaged));
        __atomic_store_n(&gStagedState, kStagedFound, __ATOMIC_RELEASE);
        publishAddressBytes(staged);
        IOLog("SwitchBluetooth: BD %02x:%02x:%02x:%02x:%02x:%02x from the staged Joy-Con pairing (Horizon's), in "
              "place before launchd\n",
              staged[0], staged[1], staged[2], staged[3], staged[4], staged[5]);
    } else {
        __atomic_store_n(&gStagedState, kStagedNone, __ATOMIC_RELEASE);
    }
    announceAddressToRails();
}
