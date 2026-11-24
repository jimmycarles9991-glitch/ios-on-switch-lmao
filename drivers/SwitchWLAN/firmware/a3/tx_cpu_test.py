import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_tx_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_tx_test_results'], 86)
        results += switch_cpu.switch_read_words(memory, symbols['switch_timing_test_results'], 27)
        if switch_cpu.switch_read_words(memory, symbols['switch_tx_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control,
                'human-monitor-command',
                {'command-line': 'info registers'},
            )
            raise AssertionError(f'ARM TX tests did not finish; results={results}; {registers}')
        return results


class TransmitCPU(unittest.TestCase):
    def test_native_pm_tailcall_preserves_thumb_state(self):
        objdump = switch_tool('arm-none-eabi-objdump')
        disassembly = subprocess.check_output(
            [objdump, '-d', str(Path(__file__).with_name('pm_status_link.elf'))], text=True
        )
        self.assertNotIn('blx', disassembly)
        self.assertRegex(disassembly, r'\b195f30 <switch_a3_pm_status_original>')

    def test_native_symbols_preserve_thumb_calls(self):
        objdump = switch_tool('arm-none-eabi-objdump')
        disassembly = subprocess.check_output([objdump, '-d', str(Path(__file__).with_name('tx_link.elf'))], text=True)
        self.assertNotIn('blx', disassembly)
        for name, address in (
            ('sendctl', '197754'),
            ('register', '5d6f0'),
            ('unregister', '1b20c8'),
            ('original', '18c7a4'),
            ('status_original', '5d6cc'),
        ):
            self.assertRegex(disassembly, rf'\b{address} <switch_a3_tx_{name}>')

    def test_actual_thumb_ingress_dma_and_completion(self):
        for lto in (False, True):
            with self.subTest(lto=lto):
                self.switch_check_cpu(lto)

    def switch_check_cpu(self, lto: bool):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        elf = Path(__file__).with_name('tx_cpu_lto.elf' if lto else 'tx_cpu.elf')
        with tempfile.TemporaryDirectory(prefix='switch-a3-tx-', dir='/private/tmp') as directory:
            path = Path(directory)
            symbols = {}
            for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    symbols[fields[2]] = int(fields[0], 16)
            results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_tx_results)
            self.assertEqual(results, (0,) * 113, f'failing tx_test.c/tx_timing_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
