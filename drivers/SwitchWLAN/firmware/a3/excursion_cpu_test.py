import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_excursion_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_excursion_test_results'], 49)
        if switch_cpu.switch_read_words(memory, symbols['switch_excursion_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control,
                'human-monitor-command',
                {'command-line': 'info registers'},
            )
            raise AssertionError(f'ARM excursion tests did not finish; results={results}; {registers}')
        return results


class ExcursionCPU(unittest.TestCase):
    def test_native_request_is_thumb_symbol(self):
        objdump = switch_tool('arm-none-eabi-objdump')
        disassembly = subprocess.check_output(
            [objdump, '-d', str(Path(__file__).with_name('excursion_link.elf'))], text=True
        )
        self.assertNotIn('blx', disassembly)
        self.assertRegex(disassembly, r'\b2972c <switch_a3_excursion_scan_request>')

    def test_actual_thumb_native_request_and_lifetime(self):
        for lto in (False, True):
            with self.subTest(lto=lto):
                self.switch_check_cpu(lto)

    def switch_check_cpu(self, lto: bool):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        elf = Path(__file__).with_name('excursion_cpu_lto.elf' if lto else 'excursion_cpu.elf')
        with tempfile.TemporaryDirectory(prefix='switch-a3-excursion-', dir='/private/tmp') as directory:
            path = Path(directory)
            symbols = {}
            for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    symbols[fields[2]] = int(fields[0], 16)
            results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_excursion_results)
            self.assertEqual(results, (0,) * 49, f'failing excursion_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
