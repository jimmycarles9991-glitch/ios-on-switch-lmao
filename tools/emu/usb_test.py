"""The USB-C port's models under qtest (patches 0041-0048).

The BM92T36 and BQ24193 replay the device's trace; the XUSB host is brought up as xhci-tegra.c and SwitchXHCI do it.

The PD trace is boot 1109-2145 (2026-11-09 18:04): a sink-only partner attaches as STATUS1 0x1280 / STATUS2 0x2000 once
the source switches are on, leaves when they go off, and is sourced (0x9680) once v_vdd5v3 (PX3) is up.
"""

import struct
import tempfile
import unittest
from pathlib import Path

from tools.emu import qemu
from tools.emu.qtest import Qtest

I2C1 = 0x7000C000
I2C_CNFG_SEND, I2C_CNFG_READ = 1 << 9, 1 << 6
PD, CHARGER = 0x18, 0x6B
ALERT_STATUS, STATUS1, STATUS2, CONFIG1, VENDOR_CONFIG, FW_TYPE, FW_REVISION = 0x02, 0x03, 0x04, 0x06, 0x1A, 0x4B, 0x4C
SWITCHES_ON, SWITCHES_OFF = 0x00E0, 0xC0E0
GPIO = 0x6000D000
PK4, PX3 = (10, 4), (23, 3)
GPIO_CNF, GPIO_OE, GPIO_OUT, GPIO_IN = 0x00, 0x10, 0x20, 0x30
MS = 1_000_000

PMC, CAR = 0x7000E400, 0x60006000
PWRGATE_TOGGLE, PWRGATE_STATUS, XUSBC = 0x30, 0x38, 22
CLK_ENB_U_SET, RST_DEV_U_CLR, XUSB_HOST = 0x330, 0x314, 1 << 25
XHCI, FPCI, IPFS = 0x70090000, 0x70098000, 0x70099000
IPFS_CONFIGURATION, CFG_1, CFG_4 = 0x180, 0x004, 0x010
MBOX_CMD, MBOX_DATA_IN, MBOX_DATA_OUT, MBOX_OWNER, CSBRANGE, CSB_WINDOW = 0x0E4, 0x0E8, 0x0EC, 0x0F0, 0x41C, 0x800
SMI_INTR, SMI_INTR_EN, MBOX_DEST_SMI, MBOX_ACK = 0x428, 1 << 3, 1 << 29, 128
FALC_CPUCTL, FALC_BOOTVEC, FALC_IMFILLRNG1, FALC_IMFILLCTL = 0x100, 0x104, 0x154, 0x158
ILOAD_ATTR, ILOAD_BASE_LO, ILOAD_BASE_HI = 0x101A00, 0x101A04, 0x101A08
L2IMEMOP_SIZE, L2IMEMOP_TRIG, L2IMEMOP_RESULT = 0x101A10, 0x101A14, 0x101A18
USBSTS_CNR = 1 << 11
FIRMWARE = 0x90000000
CODE_TAG, CODE_SIZE, IMAGE_BYTES = 0x1D800, 0x500, 122368  # tegra210b01_xusb_firmware's header


class Board:
    def __init__(self, q: Qtest):
        self.q = q

    def i2c(self, address: int, data: bytes, read: int = 0) -> bytes:
        self.q.writel(I2C1 + 0x04, address << 1)
        self.q.writel(I2C1 + 0x0C, int.from_bytes(data[:4].ljust(4, b'\0'), 'little'))
        self.q.writel(I2C1 + 0x00, I2C_CNFG_SEND | (len(data) - 1) << 1)
        if not read:
            return b''
        self.q.writel(I2C1 + 0x00, I2C_CNFG_SEND | I2C_CNFG_READ | (read - 1) << 1)
        return self.q.readl(I2C1 + 0x0C).to_bytes(4, 'little')[:read]

    def pd(self, register: int) -> int:
        return int.from_bytes(self.i2c(PD, bytes([register]), read=2), 'little')

    def set_pd(self, register: int, value: int) -> None:
        self.i2c(PD, bytes([register]) + value.to_bytes(2, 'little'))

    def charger(self, register: int) -> int:
        return self.i2c(CHARGER, bytes([register]), read=1)[0]

    def gpio(self, pin: tuple[int, int], register: int) -> int:
        port, _ = pin
        return GPIO + (port // 4) * 0x100 + register + (port % 4) * 4

    def pad(self, pin: tuple[int, int]) -> int:
        return (self.q.readl(self.gpio(pin, GPIO_IN)) >> pin[1]) & 1

    def drive(self, pin: tuple[int, int], level: int) -> None:
        bit = 1 << pin[1]
        for register, value in ((GPIO_CNF, bit), (GPIO_OE, bit), (GPIO_OUT, bit if level else 0)):
            self.q.writel(self.gpio(pin, register), value)


class Host:
    def __init__(self, q: Qtest):
        self.q = q

    def power_up(self) -> None:
        if not self.q.readl(PMC + PWRGATE_STATUS) & 1 << XUSBC:
            self.q.writel(PMC + PWRGATE_TOGGLE, 1 << 8 | XUSBC)
        self.q.writel(CAR + CLK_ENB_U_SET, XUSB_HOST)
        self.q.writel(CAR + RST_DEV_U_CLR, XUSB_HOST)

    def bridge(self) -> None:
        self.q.writel(IPFS + IPFS_CONFIGURATION, 1)
        self.q.writel(FPCI + CFG_4, XHCI)
        self.q.writel(FPCI + CFG_1, 7)

    def csb(self, address: int, value: int | None = None) -> int:
        self.q.writel(FPCI + CSBRANGE, address >> 9)
        window = FPCI + CSB_WINDOW + (address & 0x1FF)
        if value is None:
            return self.q.readl(window)
        self.q.writel(window, value)
        return value

    def load(self, bootvec: int = CODE_TAG) -> None:
        header = bytearray(256)
        struct.pack_into('<III', header, 8, CODE_TAG, CODE_SIZE, 0)
        struct.pack_into('<I', header, 100, IMAGE_BYTES)
        self.q.write(FIRMWARE, bytes(header))
        first, count = CODE_TAG // 256, CODE_SIZE // 256
        self.csb(ILOAD_ATTR, IMAGE_BYTES)
        self.csb(ILOAD_BASE_HI, 0)
        self.csb(ILOAD_BASE_LO, FIRMWARE + 256)
        self.csb(L2IMEMOP_TRIG, 0x40 << 24)
        self.csb(L2IMEMOP_SIZE, first << 8 | count << 24)
        self.csb(L2IMEMOP_TRIG, 0x11 << 24)
        self.csb(FALC_IMFILLCTL, count)
        self.csb(FALC_IMFILLRNG1, first | (first + count) << 16)
        self.csb(FALC_BOOTVEC, bootvec)
        self.csb(FALC_CPUCTL, 1 << 1)

    def usbsts(self) -> int:
        return self.q.readl(XHCI + (self.q.readl(XHCI) & 0xFF) + 4)


class Emulator(unittest.TestCase):
    extra: tuple[str, ...] = ()

    def setUp(self) -> None:
        binary = qemu.built() if qemu.ever_built() else None
        if not binary:
            self.skipTest('no tegra_qemu build of these patches: bazel run //tools/emu:build')
        self.q = Qtest(binary, Path(tempfile.mkdtemp()) / 'qemu.log', self.extra)

    def tearDown(self) -> None:
        self.q.close()


class PdTraceTest(Emulator):
    extra = ('-global', 'bm92t36.partner=sink')

    def test_a_sink_only_partner_as_the_device_saw_it(self):
        board = Board(self.q)
        self.assertEqual((board.pd(FW_TYPE), board.pd(FW_REVISION)), (0x0603, 0x0D7C))
        self.assertEqual((board.pd(CONFIG1), board.pd(VENDOR_CONFIG)), (SWITCHES_OFF, 0x8072))
        self.assertEqual(board.pad(PK4), 1)

        board.set_pd(CONFIG1, SWITCHES_ON)
        self.q.step(30 * MS)
        self.assertEqual(board.pad(PK4), 0, 'ALERT low with the attach pending')
        self.assertEqual(board.pd(ALERT_STATUS), 0x0008)
        self.assertEqual(board.pad(PK4), 1, 'reading ALERT_STATUS releases ALERT')
        self.assertEqual((board.pd(STATUS1), board.pd(STATUS2)), (0x1280, 0x2000))

        board.set_pd(CONFIG1, SWITCHES_OFF)
        self.q.step(20 * MS)
        self.assertEqual(board.pd(ALERT_STATUS), 0x0008)
        self.assertEqual((board.pd(STATUS1), board.pd(STATUS2)), (0, 0), 'it left with the switches off')

        board.drive(PX3, 1)
        board.set_pd(CONFIG1, SWITCHES_ON)
        self.q.step(30 * MS)
        self.assertEqual(board.pd(ALERT_STATUS), 0x0008)
        self.assertEqual(board.pd(STATUS1), 0x1280, 'back as our sink, the switch still closing')
        self.q.step(10 * MS)
        self.assertEqual(board.pd(STATUS1), 0x9680, 'sourced: SPDSRC1 and VSAFE')

        board.drive(PX3, 0)
        self.assertEqual(board.pd(STATUS1), 0x1280, 'the switch opens with the rail')

    def test_the_charger_reports_only_its_own_boost(self):
        board = Board(self.q)
        self.assertEqual(board.charger(0x08), 0x00, 'no VBUS, not charging, not power good')
        board.i2c(CHARGER, bytes([0x01, 0x21]))
        self.assertEqual(board.charger(0x08), 0xC0, 'VBUS OTG while CHG_CONFIG is OTG')
        self.assertEqual(board.charger(0x0A), 0x2F)


class XusbHostTest(Emulator):
    def test_nothing_answers_before_the_clock(self):
        host = Host(self.q)
        host.bridge()
        self.assertEqual(self.q.readl(IPFS + IPFS_CONFIGURATION), 0)
        self.assertEqual(self.q.readl(XHCI + 4), 0)

    def test_the_xhci_decodes_through_the_bridge_and_runs_once_booted(self):
        host = Host(self.q)
        host.power_up()
        self.assertEqual(self.q.readl(XHCI + 4), 0, 'no BAR0 yet')
        host.bridge()
        self.assertEqual(self.q.readl(XHCI + 4), 0x09000124, "the Switch's HCSPARAMS1")
        self.assertTrue(host.usbsts() & USBSTS_CNR, 'not ready before the falcon runs')
        host.load()
        self.assertEqual(host.csb(L2IMEMOP_RESULT), 1 << 31)
        self.q.step(2 * MS)
        self.assertFalse(host.usbsts() & USBSTS_CNR)
        self.assertEqual(host.csb(FALC_CPUCTL), 0)

        self.command(1)
        self.assertEqual(self.q.readl(FPCI + MBOX_OWNER), 0, 'MSG_ENABLED taken by releasing the mailbox')
        self.assertFalse(self.q.readl(FPCI + SMI_INTR) & SMI_INTR_EN, 'and no message')

        self.command(16)
        self.assertEqual(self.q.readl(FPCI + MBOX_DATA_OUT), MBOX_ACK << 24, 'RESET_SSPI answered by an ACK message')
        self.assertTrue(self.q.readl(FPCI + SMI_INTR) & SMI_INTR_EN, 'the answer raises the mailbox interrupt')
        self.assertTrue(self.q.readl(FPCI + MBOX_CMD) & MBOX_DEST_SMI)
        self.assertEqual(self.q.readl(FPCI + MBOX_OWNER), 2, "the mailbox stays the driver's until it takes the ACK")
        self.q.writel(FPCI + SMI_INTR, SMI_INTR_EN)
        self.q.writel(FPCI + MBOX_OWNER, 0)
        self.assertEqual(self.q.readl(FPCI + MBOX_OWNER), 0)

    def command(self, cmd: int) -> None:
        self.q.writel(FPCI + MBOX_OWNER, 2)
        self.assertEqual(self.q.readl(FPCI + MBOX_OWNER), 2)
        self.q.writel(FPCI + MBOX_DATA_IN, cmd << 24)
        self.q.writel(FPCI + MBOX_CMD, 1 << 31 | 1 << 27)
        self.q.step(1 * MS)

    def test_a_boot_vector_outside_the_load_never_boots(self):
        host = Host(self.q)
        host.power_up()
        host.bridge()
        host.load(bootvec=0)
        self.q.step(2 * MS)
        self.assertTrue(host.usbsts() & USBSTS_CNR)
        self.assertEqual(host.csb(FALC_CPUCTL), 1 << 4, 'halted')

    def test_the_capability_registers_are_the_t210s(self):
        """Tegra X1 TRM 22.16.11 (patch 0053); the port and slot counts stay the console's."""
        host = Host(self.q)
        host.power_up()
        host.bridge()
        caps = {offset: self.q.readl(XHCI + offset) for offset in (0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18)}
        self.assertEqual(caps[0x00], 0x01000020, 'HCIVERSION 100h, CAPLENGTH 20h')
        self.assertEqual(caps[0x04], 0x09000124)
        self.assertEqual(caps[0x08], 0xF8, 'ERST Max Fh, IST 8')
        self.assertEqual(caps[0x0C], 0x00400002)
        self.assertEqual(caps[0x10] >> 16, 0x180, 'xECP 180h dwords')
        self.assertEqual((caps[0x10] >> 12) & 0xF, 0xF, 'MaxPSASize Fh')
        self.assertEqual((caps[0x14], caps[0x18]), (0xC00, 0x800), 'DBOFF 300h dwords, RTSOFF 40h x 32 bytes')
        legacy, usb3, usb2 = (self.q.readl(XHCI + offset) for offset in (0x600, 0x610, 0x620))
        self.assertEqual((legacy & 0xFF, (legacy >> 8) & 0xFF), (1, 4))
        self.assertEqual((usb3 >> 16, (usb3 >> 8) & 0xFF), (0x0300, 4))
        self.assertEqual((usb2 >> 16, (usb2 >> 8) & 0xFF), (0x0200, 0))
        self.assertEqual(self.q.readl(XHCI + 0x618), 4 << 8 | 1, 'USB 3 ports 1-4')
        self.assertEqual(self.q.readl(XHCI + 0x628), 1 << 19 | 5 << 8 | 5, 'USB 2.0 ports 5-9, HLC')


if __name__ == '__main__':
    unittest.main()
