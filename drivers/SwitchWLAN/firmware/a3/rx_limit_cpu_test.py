import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_limit_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        if switch_cpu.switch_read_words(memory, symbols['switch_rx_limit_test_done'], 1) != (0x53574133,):
            raise AssertionError('ARM receive admission tests did not finish')
        return switch_cpu.switch_read_words(memory, symbols['switch_rx_limit_test_results'], 3)


class ReceiveLimit(unittest.TestCase):
    def test_candidate_reserves_native_fetch_opportunity(self):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        for candidate, lto in ((0, False), (1, False), (0, True), (1, True)):
            with (
                self.subTest(candidate=candidate, lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-rx-limit-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = Path(__file__).with_name(f'rx_limit_cpu_candidate{candidate}{"_lto" if lto else ""}.elf')
                symbols = {}
                for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                    fields = line.split()
                    if len(fields) == 3:
                        symbols[fields[2]] = int(fields[0], 16)
                results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_limit_results)
                self.assertEqual(results, (0, 3 if candidate else 4, 4))


if __name__ == '__main__':
    unittest.main()
