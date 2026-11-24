import json
import struct
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import rom_capture
from drivers.SwitchWLAN.firmware.tools.a3_cpu_harness import switch_qemu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


class CopyCPU(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.qemu = switch_tool('qemu-system-arm')
        objcopy = switch_tool('arm-none-eabi-objcopy')
        cls.directory = tempfile.TemporaryDirectory(prefix='switch-rom-cpu-', dir='/private/tmp')
        cls.path = Path(cls.directory.name)
        cls.elf = Path(__file__).with_name('rom_capture_cpu.elf')
        binary = cls.path / 'capture.bin'
        subprocess.run([objcopy, '-O', 'binary', '-j', '.text', str(cls.elf), str(binary)], check=True)
        if binary.read_bytes() != rom_capture.header_bytes('switch_wlan_rom_start_code'):
            raise AssertionError('embedded copy code differs from rom_capture.S')

    @classmethod
    def tearDownClass(cls) -> None:
        cls.directory.cleanup()

    def run_copy(self, length: int, ram: int = 0xC0000, magic: int = rom_capture.MAGIC):
        with tempfile.TemporaryDirectory(prefix='run-', dir=self.path) as directory:
            path = Path(directory)
            source = b''.join(struct.pack('<I', (index * index + 17 * index + 9) & 0xFFFFFFFF) for index in range(1024))
            (path / 'source').write_bytes(source)
            (path / 'header').write_bytes(struct.pack('<6I', magic, 0, length, ram, 0, 0))
            arguments = [
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
                f'loader,file={self.elf},cpu-num=0',
                '-device',
                f'loader,file={path}/source,addr=0,force-raw=on',
                '-device',
                f'loader,file={path}/header,addr=0x180100,force-raw=on',
            ]
            with (
                switch_qemu(arguments, path) as (qmp, qtest),
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
                header = struct.unpack('<6I', read(0x180100, 24))
                data = read(0x181000, len(source) + 4)
                return header, data, source

    def test_actual_thumb_code_copies_and_publishes_after_the_last_word(self):
        header, data, source = self.run_copy(4096)
        checksum = 0
        for (word,) in struct.iter_unpack('<I', source):
            checksum ^= word
        self.assertEqual(header, (rom_capture.MAGIC, 2, len(source), 0xC0000, len(source), checksum))
        self.assertEqual(data, source + bytes(4))

    def test_actual_thumb_code_rejects_invalid_bounds_before_copying(self):
        for length, ram, magic in (
            (0, 0xC0000, rom_capture.MAGIC),
            (3, 0xC0000, rom_capture.MAGIC),
            (0xC0000, 0xC0000, rom_capture.MAGIC),
            (4, 0x1000, rom_capture.MAGIC),
            (4, 0xC0000, 0),
        ):
            with self.subTest(length=length, ram=ram, magic=magic):
                header, data, _ = self.run_copy(length, ram, magic)
                self.assertEqual(header, (magic, 3, length, ram, 0, 0))
                self.assertEqual(data, bytes(len(data)))


if __name__ == '__main__':
    unittest.main()
