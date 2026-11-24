import functools
import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_control_results(
    qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int], count: int = 14
) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_control_test_results'], count)
        if switch_cpu.switch_read_words(memory, symbols['switch_control_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                control, 'human-monitor-command', {'command-line': 'info registers'}
            )
            raise AssertionError(f'ARM control tests did not finish; results={results}; {registers}')
        return results


class ControlCPU(unittest.TestCase):
    switch_image = 'control_cpu'
    switch_cases = 14

    def test_control_protocol_in_development_and_release(self):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        for development in (0, 1):
            with (
                self.subTest(development=development),
                tempfile.TemporaryDirectory(prefix='switch-a3-control-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = Path(__file__).with_name(f'{self.switch_image}_dev{development}.elf')
                symbols = {}
                for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                    fields = line.split()
                    if len(fields) == 3:
                        symbols[fields[2]] = int(fields[0], 16)
                results = switch_cpu.switch_run_arm(
                    qemu, elf, path, symbols, functools.partial(switch_control_results, count=self.switch_cases)
                )
                self.assertEqual(results, (0,) * self.switch_cases, f'failing control test line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
