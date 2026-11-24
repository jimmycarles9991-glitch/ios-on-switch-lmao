import hashlib
import json
import socket
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_rom_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        if switch_cpu.switch_read_words(memory, symbols['switch_rom_test_done'], 1) != (0x53574133,):
            raise AssertionError('ARM ROM guard tests did not finish')
        return switch_cpu.switch_read_words(memory, symbols['switch_rom_test_results'], 6)


class CandidateROM(unittest.TestCase):
    def test_exact_stock_flashpatch_sources_and_targets(self):
        image = (Path(__file__).resolve().parent.parent / 'brcm/brcmfmac4356A3-pcie.bin').read_bytes()
        self.assertEqual(
            hashlib.sha256(image).hexdigest(),
            '32a16e7b4391ccd248d3677c17cde940f398aaaaa6163571fcbff2bbee026ade',
        )
        for table, rom, source, target, branch, prefix in (
            (0x182834, 0x6A6B8, 0x181D78, 0x1CFC88, '65f1e6ba', 'f8b5837905460e460468002b78d08788'),
            (0x18245C, 0x44DAC, 0x181AE8, 0x1A7B94, '62f1f2be', '38b5d0f844310446d0f88850dbb9d0f8'),
        ):
            with self.subTest(rom=hex(rom)):
                self.assertEqual(struct.unpack_from('<3I', image, table - 0x180000), (rom, 4, source))
                slot = image[source - 0x180000 : source - 0x180000 + 8]
                self.assertEqual(slot, bytes.fromhex(branch + '00000000'))
                self.assertEqual(image[target - 0x180000 : target - 0x180000 + 16], bytes.fromhex(prefix))
                first, second = struct.unpack_from('<HH', slot)
                self.assertEqual(first & 0xF800, 0xF000)
                self.assertEqual(second & 0xD000, 0x9000)
                sign = (first >> 10) & 1
                i1 = (~((second >> 13) ^ sign)) & 1
                i2 = (~((second >> 11) ^ sign)) & 1
                offset = (sign << 24) | (i1 << 23) | (i2 << 22) | ((first & 0x3FF) << 12) | ((second & 0x7FF) << 1)
                self.assertEqual(rom + 4 + offset - (sign << 25), target)

    def test_every_guarded_byte_rejects_corruption(self):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        for development, lto in ((0, False), (1, False), (0, True), (1, True)):
            with (
                self.subTest(development=development, lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-rom-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = Path(__file__).with_name(f'rom_candidate_cpu_dev{development}{"_lto" if lto else ""}.elf')
                symbols = {}
                for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                    fields = line.split()
                    if len(fields) == 3:
                        symbols[fields[2]] = int(fields[0], 16)
                results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_rom_results)
                self.assertEqual(results, (0, 0, 33, 472, 0, 0))


if __name__ == '__main__':
    unittest.main()
