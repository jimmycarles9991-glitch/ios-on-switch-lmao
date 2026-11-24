import json
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.a3.pm_native import SWITCH_ROM
from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool

SWITCH_RAM_RANGES = (
    (0x18EBAE, 0x18EC0A), (0x193662, 0x19368A), (0x19528C, 0x195360),
    (0x1953E0, 0x195446), (0x195F30, 0x19602A), (0x1969EE, 0x196A00),
    (0x196E6C, 0x196FC4), (0x1CF694, 0x1CF740), (0x1CF7C6, 0x1CF8F6),
)

SWITCH_STUBS = {
    0x2E5C: 'copy', 0x89A3C: 'allocate', 0x6BBF8: 'scb', 0x18F110: 'enqueue',
    0x19508E: 'drain', 0x18A838: 'free', 0x12A18: 'timer', 0x12A60: 'noop',
    0x18BFB8: 'zero', 0x18FB12: 'noop', 0x1935F0: 'noop', 0x27B54: 'one',
    0x3CD4: 'zero', 0x289E8: 'noop', 0x1E4F4: 'noop', 0x2A764: 'noop',
    0x5F9CC: 'noop', 0x2B4A0: 'noop', 0x1AC58: 'noop', 0x69784: 'zero',
    0x6972C: 'zero', 0x2FB0: 'noop', 0x2C000: 'suspend', 0x194956: 'noop',
    0x1A790E: 'zero', 0x1A7912: 'zero', 0x193774: 'home', 0x26900: 'noop',
    0x1AEEAE: 'noop', 0x274C4: 'noop', 0x54BA8: 'noop', 0x272F4: 'noop',
    0x2586C: 'enable', 0x2C530: 'noop', 0x1C8FC6: 'noop',
    0x41480: 'pm2_eligible', 0x5FAAC: 'pm2', 0x29AF4: 'noop',
}


def switch_branch(site: int, target: int, link: bool = False) -> bytes:
    offset = (target & ~1) - site - 4
    sign = (offset >> 24) & 1
    j1, j2 = ~((offset >> 23) ^ sign) & 1, ~((offset >> 22) ^ sign) & 1
    return struct.pack('<HH', 0xF000 | sign << 10 | (offset >> 12) & 0x3FF,
                       (0xD000 if link else 0x9000) | j1 << 13 | j2 << 11 | (offset >> 1) & 0x7FF)


def switch_write(memory, address: int, data: bytes):
    memory.write(f'write {address:#x} {len(data)} 0x{data.hex()}\n'.encode())
    if memory.readline().strip() != b'OK':
        raise AssertionError(f'could not load native bytes at {address:#x}')


def switch_pm_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int], original=False) -> tuple[int, ...]:
    source = Path(__file__).resolve().parent
    ram = (source.parent / 'brcm/brcmfmac4356A3-pcie.bin').read_bytes()
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        for address, native in SWITCH_ROM.items():
            switch_write(memory, address, bytes.fromhex(native))
        for begin, end in SWITCH_RAM_RANGES:
            switch_write(memory, begin, ram[begin - 0x180000:end - 0x180000])
        for address, stub in SWITCH_STUBS.items():
            switch_write(memory, address, switch_branch(address, symbols['switch_pm_test_' + stub]))
        for address, entry, link, expected in (
            (0x1CF6E6, 'pm_prepare_entry', False, '1b7a53b9'),
            (0x19542C, 'pm_submit_entry', True, 'fff72eff'),
            (0x193662, 'pm_set_entry', False, 'd0f82031'),
            (0x1969FC, 'pm_status_entry', True, 'fff798fa'),
        ):
            if ram[address - 0x180000:address - 0x180000 + 4].hex() != expected:
                raise AssertionError(f'native PM hook signature mismatch at {address:#x}')
            if not original or address != 0x1CF6E6:
                switch_write(memory, address, switch_branch(address, symbols['switch_a3_' + entry], link))
        switch_write(memory, 0x196A00, switch_branch(0x196A00, symbols['switch_pm_test_status_return']))
        switch_write(memory, symbols['switch_pm_test_original'], struct.pack('<I', int(original)))
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.15)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_pm_test_results'], 35)
        if switch_cpu.switch_read_words(memory, symbols['switch_pm_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(control, 'human-monitor-command', {'command-line': 'info registers'})
            raise AssertionError(f'native PM fixture did not finish: {results}; {registers}')
        return results


class SwitchNativePM(unittest.TestCase):
    def test_captured_native_preparation_packets_and_completion(self):
        qemu, gcc, nm = (switch_tool(name) for name in ('qemu-system-arm', 'arm-none-eabi-gcc', 'arm-none-eabi-nm'))
        source = Path(__file__).resolve().parent
        for lto in (False, True):
            with self.subTest(lto=lto), tempfile.TemporaryDirectory(prefix='switch-a3-pm-', dir='/private/tmp') as directory:
                path = Path(directory)
                elf = path / 'pm.elf'
                subprocess.run([
                    gcc, *(['-flto'] if lto else []), '-mcpu=cortex-r4', '-mthumb', '-mno-unaligned-access',
                    '-Os', '-std=c11', '-Wall', '-Wextra', '-Werror', '-ffreestanding', '-fno-builtin', '-nostdlib',
                    '-DDEVELOPMENT=1', '-Wl,-Ttext=0x100000,--entry=switch_pm_test_start,--build-id=none',
                    *('-Wl,--undefined=switch_pm_test_' + name for name in set(SWITCH_STUBS.values())),
                    *(str(source / name) for name in ('diagnostic.c', 'channel.c', 'pm.S', 'pm_status.S',
                                                     'pm_symbols.S', 'pm_test.c', 'pm_test_start.S')),
                    '-o', str(elf),
                ], check=True)
                symbols = {}
                for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                    fields = line.split()
                    if len(fields) == 3:
                        symbols[fields[2]] = int(fields[0], 16)
                for original in (True, False):
                    with self.subTest(original=original):
                        results = switch_cpu.switch_run_arm(qemu, elf, path, symbols,
                            lambda qmp, qtest, symbols: switch_pm_results(qmp, qtest, symbols, original))
                        self.assertEqual(results, (0,) * 35, f'failing pm_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
