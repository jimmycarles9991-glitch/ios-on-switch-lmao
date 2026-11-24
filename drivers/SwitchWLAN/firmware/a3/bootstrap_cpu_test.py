import json
import re
import struct
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.a3 import payload_header
from drivers.SwitchWLAN.firmware.tools.a3_cpu_harness import switch_qemu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool

SWITCH_CONFIG = 0x110000
SWITCH_RESULT = 0x110100
SWITCH_OUTPUT = 0x130000
SWITCH_STACK = 0x23F000
SWITCH_HEAP_END = 0x23DF6C
SWITCH_HEAP_MIN = 186256
SWITCH_WLC = 0x120000
SWITCH_PUB = 0x120100
SWITCH_REGISTER_PREFIX = bytes.fromhex('2de9f0478a461146984691460468fff7dfff')


def switch_branch(source: int, target: int, link: bool = False) -> bytes:
    displacement = (target & ~1) - source - 4
    if displacement & 1 or not -(1 << 24) <= displacement < 1 << 24:
        raise ValueError('Thumb branch outside range')
    value = displacement & 0x1FFFFFF
    sign, first, second = value >> 24, (value >> 23) & 1, (value >> 22) & 1
    return struct.pack(
        '<HH',
        0xF000 | (sign << 10) | ((value >> 12) & 0x3FF),
        (0xD000 if link else 0x9000)
        | ((1 ^ first ^ sign) << 13)
        | ((1 ^ second ^ sign) << 11)
        | ((value >> 1) & 0x7FF),
    )


class BootstrapCPU(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.qemu = switch_tool('qemu-system-arm')
        cls.directory = tempfile.TemporaryDirectory(prefix='switch-a3-cpu-', dir='/private/tmp')
        cls.path = Path(cls.directory.name)
        cls.elf = Path(__file__).with_name('bootstrap.elf')
        cls.binary = cls.elf.with_suffix('.bin').read_bytes()
        cls.symbols = payload_header.switch_a3_symbols(cls.elf.read_bytes())
        cls.header = payload_header.switch_a3_header(cls.elf.read_bytes(), cls.binary)
        cls.begin = cls.symbols['switch_a3_island_start']
        cls.end = cls.symbols['switch_a3_island_end']
        cls.state = cls.symbols['switch_a3_state']
        cls.harness = Path(__file__).with_name('bootstrap_harness.elf')
        cls.test_symbols = payload_header.switch_a3_symbols(cls.harness.read_bytes())

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def rom_blocks(self) -> dict[str, tuple[int, bytes]]:
        blocks = {}
        guard_offset = self.symbols['switch_a3_rom_guards'] - self.begin
        while True:
            address, length, data = struct.unpack_from('<II16s', self.binary, guard_offset)
            if not length:
                break
            if address != 0x27674:
                blocks[f'rom-{address:x}'] = (address, data[:length])
            guard_offset += 24
        return blocks

    def run_arm(  # noqa: PLR0913
        self,
        *,
        heap_end: int = SWITCH_HEAP_END,
        wlc: int = SWITCH_WLC,
        register_result: int = 0,
        action: int = 0xFFFFFFFF,
        length: int = 64,
        output: int = SWITCH_OUTPUT,
        context: int = 0,
        rom_mismatch: bool = False,
    ) -> tuple[tuple[int, ...], tuple[int, ...], bytes, bytes]:
        with tempfile.TemporaryDirectory(prefix='run-', dir=self.path) as directory:
            path = Path(directory)
            blocks = {
                'config': (
                    SWITCH_CONFIG,
                    struct.pack('<7I', heap_end, wlc, register_result & 0xFFFFFFFF, action, length, output, context),
                ),
                'wlc': (SWITCH_WLC, struct.pack('<I', SWITCH_PUB)),
                'pub': (SWITCH_PUB, struct.pack('<I', SWITCH_WLC)),
                'output': (SWITCH_OUTPUT - 4, bytes([0xA5]) * 88),
                'early': (0x1E006C, switch_branch(0x1E006C, self.symbols['switch_a3_heap_init'], True)),
                'early_return': (0x1E0070, switch_branch(0x1E0070, self.test_symbols['switch_test_heap_return'])),
                'heap': (0x1DFB9C, switch_branch(0x1DFB9C, self.test_symbols['switch_test_heap'])),
                'late': (0x1E9B8E, switch_branch(0x1E9B8E, self.symbols['switch_a3_attach'])),
                'epilogue': (0x1E9B92, bytes.fromhex('bde8f08f')),
                'finder': (0x27644, bytes.fromhex('7047')),
                'register': (
                    0x27674,
                    (b'\0' + SWITCH_REGISTER_PREFIX[1:] if rom_mismatch else SWITCH_REGISTER_PREFIX)
                    + switch_branch(0x27686, self.test_symbols['switch_test_register']),
                ),
            }
            blocks.update(self.rom_blocks())
            args = [
                self.qemu,
                '-M',
                'versatilepb',
                '-cpu',
                'cortex-a9',
                '-m',
                '16M',
                '-display',
                'none',
                '-serial',
                'none',
                '-monitor',
                'none',
                '-audio',
                'none',
                '-device',
                f'loader,file={self.harness},cpu-num=0',
                '-device',
                f'loader,file={self.elf}',
            ]
            for name, (address, data) in blocks.items():
                file = path / name
                file.write_bytes(data)
                args += ['-device', f'loader,file={file},addr={address:#x},force-raw=on']
            with (
                switch_qemu(args, path) as (qmp, qtest),
                qmp.makefile('rwb', buffering=0) as control,
                qtest.makefile('rwb', buffering=0) as memory,
            ):
                json.loads(control.readline())

                def command(name: str) -> None:
                    control.write(json.dumps({'execute': name}).encode() + b'\n')
                    while True:
                        reply = json.loads(control.readline())
                        if 'error' in reply:
                            raise RuntimeError(reply)
                        if 'return' in reply:
                            return

                def read(address: int, size: int) -> bytes:
                    memory.write(f'read {address:#x} {size}\n'.encode())
                    reply = memory.readline().decode().split()
                    if len(reply) != 2 or reply[0] != 'OK':
                        raise RuntimeError(reply)
                    return bytes.fromhex(reply[1][2:])

                command('qmp_capabilities')
                command('cont')
                time.sleep(0.05)
                command('stop')
                return (
                    struct.unpack('<32I', read(SWITCH_RESULT, 128)),
                    struct.unpack('<16I', read(self.state, 64)),
                    read(SWITCH_OUTPUT - 4, 88),
                    read(self.begin, self.end - self.begin),
                )

    def assert_attach_return(self, result: tuple[int, ...], wlc: int) -> None:
        self.assertEqual(result[4:6], (0x44444444, SWITCH_STACK))
        self.assertEqual(result[7:9], (wlc, SWITCH_STACK))
        self.assertEqual(result[9:17], tuple(range(0x4404, 0x440C)))
        self.assertEqual(result[29], 1)

    def test_release_payload_excludes_diagnostics(self):
        elf = Path(__file__).with_name('bootstrap_release.elf')
        payload = elf.with_suffix('.bin').read_bytes()
        self.assertIn(b'switch_awdl_deadline', self.binary)
        self.assertNotIn(b'switch_awdl_deadline', payload)
        symbols = payload_header.switch_a3_symbols(elf.read_bytes())
        for name in ('switch_a3_control_receive', 'switch_a3_control_deadline_fired', 'switch_a3_control_deadline'):
            self.assertNotIn(name, symbols)
        payload_header.switch_a3_header(elf.read_bytes(), payload)

    def test_generated_header_and_arm_hooks_register_only_persistent_caps(self):
        array = self.header.split('switch_wlan_a3_payload[] = {', 1)[1].split('};', 1)[0]
        self.assertEqual(
            bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]+)', array)),
            self.binary.ljust(self.end - self.begin, b'\0'),
        )
        for name in ('heap_init', 'attach', 'rx_entry', 'state'):
            value = self.symbols['switch_a3_' + name] & ~1
            self.assertIn(f'switch_wlan_a3_{name} = 0x{value:x};', self.header)
        result, caps, _, island = self.run_arm(action=0)
        self.assert_attach_return(result, SWITCH_WLC)
        self.assertEqual(result[:3], (self.end, SWITCH_HEAP_END, SWITCH_HEAP_END))
        self.assertTrue(self.begin <= result[6] < self.end)
        self.assertEqual(result[17], 1)
        self.assertEqual(result[18], SWITCH_PUB)
        table, module, context, callback = result[19:23]
        for address in (table, module, context, callback & ~1):
            self.assertTrue(self.begin <= address < self.end)
        self.assertEqual(context, self.state)
        self.assertEqual((result[23], result[26], result[27]), (0, 0, 0))
        for value in result[24:26]:
            self.assertTrue(self.begin <= (value & ~1) < self.end)
        self.assertEqual(caps[:4], (0x33415753, 1, 64, 2))
        self.assertEqual(caps[4:9], (0, 0, 0, SWITCH_WLC, self.state))
        self.assertEqual(caps[9:13], (self.begin, self.end, self.end, SWITCH_HEAP_END))
        self.assertEqual(caps[13:], (0, 0, 0))
        self.assertEqual(result[28], 0)
        name = struct.unpack_from('<I', island, table - self.begin)[0]
        self.assertTrue(self.begin <= name < self.end)
        self.assertEqual(island[name - self.begin :].split(b'\0', 1)[0], b'switch_awdl_caps')
        self.assertEqual(island[module - self.begin :].split(b'\0', 1)[0], b'switch_awdl_a3')

    def test_null_attach_and_registration_failure_preserve_sta_return(self):
        for wlc, error, status, calls in ((0, 0, 1, 0), (SWITCH_WLC, -22, 3, 1)):
            with self.subTest(wlc=wlc, error=error):
                result, caps, _, _ = self.run_arm(wlc=wlc, register_result=error)
                self.assert_attach_return(result, wlc)
                self.assertEqual((result[17], caps[3]), (calls, status))
                self.assertEqual(caps[4], error & 0xFFFFFFFF)
                self.assertEqual(caps[5:7], (0, 0))

    def test_rom_entry_mismatch_skips_registration_and_preserves_sta_return(self):
        result, caps, _, _ = self.run_arm(rom_mismatch=True)
        self.assert_attach_return(result, SWITCH_WLC)
        self.assertEqual((result[17], caps[3]), (0, 5))

    def test_heap_wrapper_rejects_actual_end_before_initialization(self):
        for end in (0, self.end - 1, self.end, self.end + SWITCH_HEAP_MIN - 1):
            with self.subTest(end=end):
                result, caps, _, _ = self.run_arm(heap_end=end)
                self.assertEqual(result, (0,) * 32)
                self.assertEqual((caps[3], caps[11], caps[12]), (4, self.end, end))
        result, caps, _, _ = self.run_arm(heap_end=self.end + SWITCH_HEAP_MIN)
        self.assert_attach_return(result, SWITCH_WLC)
        self.assertEqual((result[0], caps[3]), (self.end, 2))

    def test_caps_callback_bounds_and_actions(self):
        cases = [
            (0, 0, SWITCH_OUTPUT, 0, -14),
            (0, 63, SWITCH_OUTPUT, 0, -14),
            (0, 64, 0, 0, -2),
            (0, 64, SWITCH_OUTPUT, 1, -2),
            (1, 64, SWITCH_OUTPUT, 0, -23),
            (2, 64, SWITCH_OUTPUT, 0, -2),
            (0, 64, SWITCH_OUTPUT, 0, 0),
            (0, 80, SWITCH_OUTPUT, 0, 0),
        ]
        for action, length, output, context, error in cases:
            with self.subTest(action=action, length=length, output=output, context=context):
                result, caps, data, _ = self.run_arm(action=action, length=length, output=output, context=context)
                self.assert_attach_return(result, SWITCH_WLC)
                self.assertEqual(result[28], error & 0xFFFFFFFF)
                expected = bytes([0xA5]) * 88
                if not error:
                    expected = expected[:4] + struct.pack('<16I', *caps) + expected[68:]
                self.assertEqual(data, expected)


if __name__ == '__main__':
    unittest.main()
