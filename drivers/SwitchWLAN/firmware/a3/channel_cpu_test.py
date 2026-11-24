import json
import socket
import subprocess
import tempfile
import time
import unittest
from pathlib import Path

from drivers.SwitchWLAN.firmware.tools import a3_cpu_harness as switch_cpu
from drivers.SwitchWLAN.firmware.tools.arm_tools import switch_tool


def switch_channel_results(qmp: socket.socket, qtest: socket.socket, symbols: dict[str, int]) -> tuple[int, ...]:
    with qmp.makefile('rwb', buffering=0) as channel, qtest.makefile('rwb', buffering=0) as memory:
        json.loads(channel.readline())
        switch_cpu.switch_qmp_command(channel, 'qmp_capabilities')
        switch_cpu.switch_qmp_command(channel, 'cont')
        time.sleep(0.1)
        switch_cpu.switch_qmp_command(channel, 'stop')
        results = switch_cpu.switch_read_words(memory, symbols['switch_channel_test_results'], 26)
        if switch_cpu.switch_read_words(memory, symbols['switch_channel_test_done'], 1) != (0x53574133,):
            registers = switch_cpu.switch_qmp_command(
                channel, 'human-monitor-command', {'command-line': 'info registers'}
            )
            raise AssertionError(f'ARM channel tests did not finish; results={results}; {registers}')
        return results


class ChannelCPU(unittest.TestCase):
    def test_native_gate_preserves_thumb_tailcalls(self):
        objdump = switch_tool('arm-none-eabi-objdump')
        disassembly = subprocess.check_output(
            [objdump, '-d', str(Path(__file__).with_name('channel_link.elf'))], text=True
        )
        self.assertNotIn('blx', disassembly)
        self.assertRegex(disassembly, r'\b193774 <switch_a3_channel_original>')
        self.assertRegex(disassembly, r'\b2586c <switch_a3_channel_mac_enable>')
        self.assertRegex(disassembly, r'\b1cfb2e <switch_a3_channel_cleanup>')

    def test_owned_retune_and_pm_gate(self):
        qemu, nm = switch_tool('qemu-system-arm'), switch_tool('arm-none-eabi-nm')
        for lto in (False, True):
            with (
                self.subTest(lto=lto),
                tempfile.TemporaryDirectory(prefix='switch-a3-channel-', dir='/private/tmp') as directory,
            ):
                path = Path(directory)
                elf = Path(__file__).with_name('channel_cpu_lto.elf' if lto else 'channel_cpu.elf')
                symbols = {}
                for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
                    fields = line.split()
                    if len(fields) == 3:
                        symbols[fields[2]] = int(fields[0], 16)
                results = switch_cpu.switch_run_arm(qemu, elf, path, symbols, switch_channel_results)
                self.assertEqual(results, (0,) * 26, f'failing channel_test.c line numbers: {results}')


if __name__ == '__main__':
    unittest.main()
