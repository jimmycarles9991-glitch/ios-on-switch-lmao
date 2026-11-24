import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools.a3_cpu_harness import switch_qmp_command, switch_read_words, switch_run_arm
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool

SWITCH_IMAGES = Path(__file__).parent.parent / 'a3'


def switch_rx_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_qmp_command(control, 'qmp_capabilities')
        switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_qmp_command(control, 'stop')
        results = switch_read_words(memory, symbols['switch_rx_test_results'], 31)
        if switch_read_words(memory, symbols['switch_rx_test_done'], 1) != (0x53574133,):
            registers = switch_qmp_command(control, 'human-monitor-command', {'command-line': 'info registers'})
            raise AssertionError(f'ARM test did not finish; results={results}; {registers}')
        return results


class ReceiveCPU(unittest.TestCase):
    def test_native_symbols_preserve_thumb_calls(self):
        objdump = switch_tool('arm-none-eabi-objdump')
        disassembly = subprocess.check_output([objdump, '-d', str(SWITCH_IMAGES / 'rx_link.elf')], text=True)
        self.assertNotIn('blx', disassembly)
        self.assertRegex(disassembly, r'beq\.w\s+19cff8 <switch_a3_rx_original>')
        for name in (
            'packet_get',
            'packet_free',
            'malloc_zero',
            'free',
            'packet_fetch',
            'raw_sendup',
            'tsf_read',
            'tsf_receive',
        ):
            self.assertIn(f'<switch_a3_{name}>', disassembly)

    def test_actual_thumb_receiver_and_dma_ownership(self):
        qemu = switch_tool('qemu-system-arm')
        nm = switch_tool('arm-none-eabi-nm')
        elf = SWITCH_IMAGES / 'rx_cpu.elf'
        with tempfile.TemporaryDirectory(prefix='switch-a3-rx-', dir='/private/tmp') as directory:
            path = Path(directory)
            symbols = {}
            for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                fields = line.split()
                if len(fields) == 3:
                    symbols[fields[2]] = int(fields[0], 16)
            results = switch_run_arm(qemu, elf, path, symbols, switch_rx_results)
            self.assertEqual(results, (0,) * 31, f'failing rx_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
