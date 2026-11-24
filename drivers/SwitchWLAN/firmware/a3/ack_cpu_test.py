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


def switch_ack_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_ack_test_results'], 45)
        if switch_cpu.switch_read_words(memory, symbols['switch_ack_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control, 'human-monitor-command', {'command-line': 'info registers'}
            )
            raise AssertionError(f'ARM ACK tests did not finish; results={results}; {registers}')
        return results


class AddressMatchCPU(unittest.TestCase):
    def test_factory_address_dispatch_patches(self):
        image = (Path(__file__).resolve().parent.parent / 'brcm/brcmfmac4356A3-pcie.bin').read_bytes()
        for index, target, payload, replacement in (
            (187, 0x2E0DC, 0x181DD8, '6ff16bbd'),
            (188, 0x2A7BC, 0x181DE0, '6af12bba'),
        ):
            self.assertEqual(struct.unpack_from('<III', image, 0x2000 + 12 * index), (target, 4, payload))
            self.assertEqual(image[payload - 0x180000 : payload - 0x180000 + 4], bytes.fromhex(replacement))

    def test_native_symbols_preserve_thumb_calls(self):
        objdump = switch_tool('arm-none-eabi-objdump')
        disassembly = subprocess.check_output([objdump, '-d', str(Path(__file__).with_name('ack_link.elf'))], text=True)
        self.assertNotIn('blx', disassembly)
        for name, address in (
            ('key_allocate', '550e4'),
            ('amt_clear', '2e0d0'),
            ('receiver_set', '5bc70'),
            ('network_set', '5bc2c'),
            ('amt_info_set', '2ce48'),
        ):
            self.assertRegex(disassembly, rf'\b{address} <switch_a3_{name}>')

    def test_actual_thumb_reservation_lifecycle(self):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        elf = Path(__file__).with_name('ack_cpu.elf')
        with tempfile.TemporaryDirectory(prefix='switch-a3-ack-', dir='/private/tmp') as directory:
            path = Path(directory)
            symbols = {}
            for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    symbols[fields[2]] = int(fields[0], 16)
            results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_ack_results)
            self.assertEqual(results, (0,) * 45, f'failing ACK/BSSID test line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
