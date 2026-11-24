import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_beacon_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as control, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(control.readline())
        switch_cpu.switch_qmp_command(control, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(control, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(control, 'stop')
        if switch_cpu.switch_read_words(memory, symbols['switch_beacon_test_done'], 1) != (0x53574133,):
            raise AssertionError('ARM beacon tests did not finish')
        return switch_cpu.switch_read_words(memory, symbols['switch_beacon_test_results'], 2)


class BeaconCPU(unittest.TestCase):
    def test_station_beacon_bounds(self):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        for lto in (False, True):
            with (
                self.subTest(lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-beacon-', dir='/private/tmp') as tmp,
            ):
                path = Path(tmp)
                elf = Path(__file__).with_name('beacon_cpu_lto.elf' if lto else 'beacon_cpu.elf')
                symbols = {}
                for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                    fields = line.split()
                    if len(fields) == 3:
                        symbols[fields[2]] = int(fields[0], 16)
                error, checks = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_beacon_results)
                self.assertEqual(error, 0, f'failed ARM assertion at beacon_test.c:{error}')
                self.assertGreaterEqual(checks, 80)


if __name__ == '__main__':
    unittest.main()
